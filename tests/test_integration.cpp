// End-to-end: real TCP sockets, SoupBinTCP sessions, simulator on a
// background thread, clients in the test thread.

#include "ouch/client.hpp"
#include "ouch/net.hpp"
#include "ouch/sim_server.hpp"

#include <gtest/gtest.h>

#include <poll.h>
#include <thread>
#include <vector>

using namespace ouch;

namespace {

SimServerConfig sim_config() {
    SimServerConfig c;
    c.users = {{"ALICE", "pw1"}, {"BOB", "pw2"}};
    c.heartbeat_interval_ns = 50'000'000;   // 50 ms so heartbeat tests are fast
    c.receive_timeout_ns    = 400'000'000;  // 400 ms
    return c;
}

ClientConfig client_config(std::uint16_t port, const char* user, const char* pw) {
    ClientConfig c;
    c.port = port;
    c.username = user;
    c.password = pw;
    c.heartbeat_interval_ns = 50'000'000;
    c.server_timeout_ns = 2'000'000'000;
    return c;
}

EnterOrder limit(char s, Qty q, double px, char t = tif::Day) {
    EnterOrder m;
    m.side = s;
    m.quantity = q;
    m.symbol = "AAPL";
    m.price = to_price(px);
    m.time_in_force = t;
    m.cl_ord_id = "IT";
    return m;
}

struct Recorder {
    std::vector<std::pair<std::uint64_t, OutboundMsg>> msgs;
    void attach(OuchClient& c) {
        c.on_message([this](const OutboundMsg& m, std::uint64_t seq) { msgs.emplace_back(seq, m); });
    }
    template <class T>
    std::size_t count() const {
        std::size_t n = 0;
        for (const auto& [s, m] : msgs) n += std::holds_alternative<T>(m);
        return n;
    }
};

class SimFixture : public ::testing::Test {
protected:
    void SetUp() override {
        server = std::make_unique<SimServer>(sim_config());
        ASSERT_TRUE(server->start());
        server->start_background();
    }
    void TearDown() override {
        server->stop();
        server->join();
    }
    std::uint16_t port() const { return server->port(); }
    std::unique_ptr<SimServer> server;
};

} // namespace

TEST_F(SimFixture, LoginAndOrderLifecycle) {
    OuchClient c(client_config(port(), "ALICE", "pw1"));
    Recorder rec;
    rec.attach(c);
    ASSERT_TRUE(c.connect());
    EXPECT_EQ(c.session(), "SIM0000001");

    const UserRefNum r1 = c.enter_order(limit(side::Buy, 100, 150.00));
    ASSERT_EQ(r1, 1u);
    ASSERT_TRUE(c.poll_until([&] { return rec.count<OrderAccepted>() == 1; }, 2'000));
    EXPECT_EQ(c.orders().find(r1)->status, OrderStatus::Live);

    ASSERT_TRUE(c.cancel_order(r1));
    ASSERT_TRUE(c.poll_until([&] { return rec.count<OrderCanceled>() == 1; }, 2'000));
    EXPECT_EQ(c.orders().find(r1)->status, OrderStatus::Dead);

    // Sequence 1 is Start of Day; sequence numbers are contiguous.
    ASSERT_GE(rec.msgs.size(), 3u);
    EXPECT_TRUE(std::holds_alternative<SystemEvent>(rec.msgs[0].second));
    for (std::size_t i = 0; i < rec.msgs.size(); ++i) EXPECT_EQ(rec.msgs[i].first, i + 1);
    c.disconnect();
}

TEST_F(SimFixture, BadPasswordIsRejected) {
    OuchClient c(client_config(port(), "ALICE", "wrong"));
    EXPECT_FALSE(c.connect());
    EXPECT_EQ(c.state(), OuchClient::State::Rejected);
    EXPECT_EQ(c.login_reject_reason(), soupbin::login_reject::NotAuthorized);
}

TEST_F(SimFixture, UnknownSessionIsRejected) {
    auto cfg = client_config(port(), "ALICE", "pw1");
    cfg.session = "OLDSESSION";
    OuchClient c(cfg);
    EXPECT_FALSE(c.connect());
    EXPECT_EQ(c.login_reject_reason(), soupbin::login_reject::SessionNotAvailable);
}

TEST_F(SimFixture, TwoClientsTrade) {
    OuchClient alice(client_config(port(), "ALICE", "pw1"));
    OuchClient bob(client_config(port(), "BOB", "pw2"));
    Recorder ra, rb;
    ra.attach(alice);
    rb.attach(bob);
    ASSERT_TRUE(alice.connect());
    ASSERT_TRUE(bob.connect());

    const auto ask = alice.enter_order(limit(side::Sell, 300, 25.10));
    ASSERT_TRUE(alice.poll_until([&] { return ra.count<OrderAccepted>() == 1; }, 2'000));
    const auto bid = bob.enter_order(limit(side::Buy, 200, 25.15));
    ASSERT_TRUE(bob.poll_until([&] { return rb.count<OrderExecuted>() == 1; }, 2'000));
    ASSERT_TRUE(alice.poll_until([&] { return ra.count<OrderExecuted>() == 1; }, 2'000));

    EXPECT_EQ(bob.orders().find(bid)->executed, 200u);
    EXPECT_EQ(bob.orders().find(bid)->status, OrderStatus::Dead);
    EXPECT_EQ(alice.orders().find(ask)->executed, 200u);
    EXPECT_EQ(alice.orders().find(ask)->open, 100u);
    EXPECT_EQ(alice.orders().find(ask)->status, OrderStatus::Live);
}

// The core reliability property of SoupBinTCP: a client that loses its
// connection logs back in with its next expected sequence number and
// receives everything it missed, exactly once, in order.
TEST_F(SimFixture, ReconnectReplaysMissedMessagesExactlyOnce) {
    OuchClient alice(client_config(port(), "ALICE", "pw1"));
    OuchClient bob(client_config(port(), "BOB", "pw2"));
    Recorder ra, rb;
    ra.attach(alice);
    rb.attach(bob);
    ASSERT_TRUE(alice.connect());
    ASSERT_TRUE(bob.connect());

    const auto ask = alice.enter_order(limit(side::Sell, 500, 30.00));
    ASSERT_TRUE(alice.poll_until([&] { return ra.count<OrderAccepted>() == 1; }, 2'000));
    const std::uint64_t seq_before = alice.next_expected_seq();

    // Alice's link dies. While she is offline, Bob trades against her order
    // three times.
    alice.drop_connection();
    for (int i = 0; i < 3; ++i) {
        bob.enter_order(limit(side::Buy, 100, 30.00));
    }
    ASSERT_TRUE(bob.poll_until([&] { return rb.count<OrderExecuted>() == 3; }, 2'000));

    // Reconnect with the same session and next sequence number.
    const std::size_t seen_before = ra.msgs.size();
    ASSERT_TRUE(alice.reconnect());
    ASSERT_TRUE(alice.poll_until([&] { return ra.count<OrderExecuted>() == 3; }, 2'000));
    ASSERT_GT(ra.msgs.size(), seen_before);
    EXPECT_EQ(ra.msgs[seen_before].first, seq_before) << "server resumes exactly where we left off";

    // No gaps, no duplicates.
    for (std::size_t i = 0; i < ra.msgs.size(); ++i) EXPECT_EQ(ra.msgs[i].first, i + 1);
    EXPECT_EQ(alice.orders().find(ask)->executed, 300u);
    EXPECT_EQ(alice.orders().find(ask)->open, 200u);

    // UserRefNum keeps increasing across the reconnect.
    const auto next = alice.enter_order(limit(side::Sell, 10, 31.00));
    EXPECT_EQ(next, ask + 1);
    ASSERT_TRUE(alice.poll_until([&] { return ra.count<OrderAccepted>() == 2; }, 2'000));
}

TEST_F(SimFixture, FreshProcessRecoversUserRefNumViaAccountQuery) {
    {
        OuchClient first(client_config(port(), "ALICE", "pw1"));
        ASSERT_TRUE(first.connect());
        for (int i = 0; i < 5; ++i) first.enter_order(limit(side::Buy, 1, 1.0 + i));
        first.poll(50);
        first.disconnect();
    }
    // A restarted process knows nothing; ask for the next valid UserRefNum.
    auto cfg = client_config(port(), "ALICE", "pw1");
    cfg.next_seq = 0;  // only new messages
    OuchClient restarted(cfg);
    ASSERT_TRUE(restarted.connect());
    ASSERT_TRUE(restarted.account_query());
    ASSERT_TRUE(restarted.poll_until([&] { return restarted.next_user_ref() == 6; }, 2'000));
}

TEST_F(SimFixture, HeartbeatsKeepIdleSessionAlive) {
    OuchClient c(client_config(port(), "ALICE", "pw1"));
    ASSERT_TRUE(c.connect());
    // Idle for longer than the server's 400 ms receive timeout: client
    // heartbeats must keep the session up.
    const auto until = net::mono_ns() + 900'000'000ULL;
    while (net::mono_ns() < until) ASSERT_TRUE(c.poll(20));
    EXPECT_EQ(c.state(), OuchClient::State::Active);
    EXPECT_NE(c.enter_order(limit(side::Buy, 1, 1.0)), 0u);
}

TEST_F(SimFixture, ServerDropsSilentClient) {
    // Raw socket: log in, then never send a heartbeat.
    int fd = net::tcp_connect("127.0.0.1", port(), 1'000);
    ASSERT_GE(fd, 0);
    std::vector<std::uint8_t> out;
    soupbin::append_login_request(out, {"ALICE", "pw1", "", 1});
    ASSERT_TRUE(net::flush(fd, out));
    std::vector<std::uint8_t> scratch;
    bool closed = false;
    const auto deadline = net::mono_ns() + 3'000'000'000ULL;
    while (!closed && net::mono_ns() < deadline) {
        pollfd p{fd, POLLIN, 0};
        ::poll(&p, 1, 50);
        if (p.revents) closed = net::read_some(fd, scratch) < 0;
    }
    EXPECT_TRUE(closed) << "server must close a session that goes quiet";
    net::close_fd(fd);
}

TEST_F(SimFixture, SecondLoginTakesOverTheAccount) {
    OuchClient a1(client_config(port(), "ALICE", "pw1"));
    ASSERT_TRUE(a1.connect());
    OuchClient a2(client_config(port(), "ALICE", "pw1"));
    ASSERT_TRUE(a2.connect());
    // The first connection gets closed by the server.
    bool dropped = false;
    const auto deadline = net::mono_ns() + 2'000'000'000ULL;
    while (!dropped && net::mono_ns() < deadline) dropped = !a1.poll(20);
    EXPECT_TRUE(dropped);
    EXPECT_EQ(a2.state(), OuchClient::State::Active);
}

TEST(SimServerStandalone, EndOfSessionClosesClients) {
    SimServer server(sim_config());
    ASSERT_TRUE(server.start());
    OuchClient c(client_config(server.port(), "ALICE", "pw1"));
    Recorder rec;
    rec.attach(c);
    // Drive both sides from one thread: no background thread needed.
    std::thread t([&] { for (int i = 0; i < 40; ++i) server.poll_once(10); });
    ASSERT_TRUE(c.connect());
    t.join();
    server.end_session();
    c.poll_until([&] { return c.state() == OuchClient::State::EndOfSession; }, 1'000);
    EXPECT_EQ(c.state(), OuchClient::State::EndOfSession);
    ASSERT_FALSE(rec.msgs.empty());
    EXPECT_EQ(std::get<SystemEvent>(rec.msgs.back().second).event_code, event_code::EndOfDay);
}
