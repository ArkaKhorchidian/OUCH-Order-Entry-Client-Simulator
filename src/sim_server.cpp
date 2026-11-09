#include "ouch/sim_server.hpp"

#include "ouch/net.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <poll.h>
#include <sys/socket.h>

namespace ouch {

struct SimServer::Conn {
    int                       fd{-1};
    soupbin::Framer           framer;
    std::vector<std::uint8_t> out;
    std::string               account;      // set after login
    bool                      logged_in{false};
    std::uint64_t             next_seq{1};  // next journal sequence to send
    net::Nanos                connected_at{0};
    net::Nanos                last_recv{0};
    net::Nanos                last_send{0};
    bool                      dead{false};
};

SimServer::SimServer(SimServerConfig cfg) : cfg_(std::move(cfg)), core_(cfg_.exchange) {}

SimServer::~SimServer() {
    stop();
    join();
    for (auto& c : conns_) net::close_fd(c->fd);
    net::close_fd(listen_fd_);
}

bool SimServer::start() {
    listen_fd_ = net::tcp_listen(cfg_.host, cfg_.port);
    if (listen_fd_ < 0) return false;
    port_ = net::local_port(listen_fd_);
    running_.store(true);
    return true;
}

void SimServer::run() {
    const int timeout_ms = cfg_.busy_poll ? 0 : 50;
    while (running_.load()) poll_once(timeout_ms);
}

void SimServer::start_background() {
    thread_ = std::thread([this] { run(); });
}

void SimServer::join() {
    if (thread_.joinable()) thread_.join();
}

void SimServer::accept_new() {
    for (;;) {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) return;
        net::set_nonblocking(fd);
        net::set_nodelay(fd);
        auto c = std::make_unique<Conn>();
        c->fd = fd;
        c->connected_at = c->last_recv = c->last_send = net::mono_ns();
        conns_.push_back(std::move(c));
    }
}

void SimServer::close_conn(Conn& c) {
    if (cfg_.verbose && c.logged_in) std::printf("[sim] %s disconnected\n", c.account.c_str());
    net::close_fd(c.fd);
    c.dead = true;
}

void SimServer::on_login(Conn& c, std::span<const std::uint8_t> payload) {
    soupbin::LoginRequest req;
    auto reject = [&](char why) {
        soupbin::append_login_rejected(c.out, why);
        (void)net::flush(c.fd, c.out);
        close_conn(c);
    };
    if (!soupbin::parse_login_request(payload, req)) return reject(soupbin::login_reject::NotAuthorized);

    if (!cfg_.users.empty()) {
        auto it = cfg_.users.find(std::string(req.username));
        if (it == cfg_.users.end() || it->second != req.password)
            return reject(soupbin::login_reject::NotAuthorized);
    }
    if (!req.session.empty() && req.session != cfg_.session_id)
        return reject(soupbin::login_reject::SessionNotAvailable);

    const std::string account(req.username);
    // Take over any existing connection for this account.
    for (auto& other : conns_)
        if (other.get() != &c && other->logged_in && !other->dead && other->account == account)
            close_conn(*other);

    const auto& journal = core_.journal(account);
    const std::uint64_t latest = journal.size() + 1;  // next new message
    std::uint64_t seq = req.sequence == 0 ? latest : req.sequence;
    if (seq > latest) seq = latest;

    c.account   = account;
    c.logged_in = true;
    c.next_seq  = seq;
    soupbin::append_login_accepted(c.out, cfg_.session_id, seq);
    if (cfg_.verbose)
        std::printf("[sim] %s logged in, session %s, next seq %llu\n", account.c_str(),
                    cfg_.session_id.c_str(), static_cast<unsigned long long>(seq));
}

void SimServer::on_packet(Conn& c, const soupbin::Packet& p) {
    if (!c.logged_in) {
        if (p.type == soupbin::packet::LoginRequest) on_login(c, p.payload);
        else if (p.type != soupbin::packet::Debug) close_conn(c);  // protocol violation
        return;
    }
    switch (p.type) {
        case soupbin::packet::UnsequencedData:
            core_.handle(c.account, p.payload, net::ns_since_midnight());
            break;
        case soupbin::packet::LogoutRequest:
            close_conn(c);
            break;
        case soupbin::packet::ClientHeartbeat:
        case soupbin::packet::Debug:
        default:
            break;
    }
}

void SimServer::on_readable(Conn& c) {
    for (;;) {
        const long n = net::read_some(c.fd, scratch_);
        if (n < 0) { close_conn(c); return; }
        if (n == 0) break;
        c.last_recv = net::mono_ns();
        c.framer.feed(scratch_);
    }
    soupbin::Packet p;
    while (!c.dead && c.framer.next(p)) on_packet(c, p);
    if (c.framer.malformed()) close_conn(c);
    c.framer.compact();
}

// Stream any journal entries the connection hasn't seen yet. Every account's
// journal may have grown (matches emit to both sides), so pump every
// connection each iteration.
void SimServer::pump(Conn& c) {
    if (c.dead || !c.logged_in) return;
    const auto& journal = core_.journal(c.account);
    while (c.next_seq <= journal.size()) {
        const auto& msg = journal[c.next_seq - 1];
        soupbin::append_packet(c.out, soupbin::packet::SequencedData, msg);
        ++c.next_seq;
    }
    const net::Nanos now = net::mono_ns();
    if (c.out.empty() && now - c.last_send >= cfg_.heartbeat_interval_ns)
        soupbin::append_packet(c.out, soupbin::packet::ServerHeartbeat);
    if (!c.out.empty()) {
        if (!net::flush(c.fd, c.out)) { close_conn(c); return; }
        c.last_send = now;
    }
}

void SimServer::end_session() {
    core_.end_of_day(net::ns_since_midnight());
    for (auto& c : conns_) {
        if (c->dead || !c->logged_in) continue;
        pump(*c);
        soupbin::append_packet(c->out, soupbin::packet::EndOfSession);
        (void)net::flush(c->fd, c->out);
        close_conn(*c);
    }
}

void SimServer::poll_once(int timeout_ms) {
    std::vector<pollfd> fds;
    fds.reserve(conns_.size() + 1);
    fds.push_back({listen_fd_, POLLIN, 0});
    for (auto& c : conns_) {
        short ev = POLLIN;
        if (!c->out.empty()) ev |= POLLOUT;
        fds.push_back({c->fd, ev, 0});
    }
    ::poll(fds.data(), fds.size(), timeout_ms);

    if (fds[0].revents & POLLIN) accept_new();
    for (std::size_t i = 1; i < fds.size(); ++i) {
        Conn& c = *conns_[i - 1];
        if (c.dead) continue;
        if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { close_conn(c); continue; }
        if (fds[i].revents & POLLIN) on_readable(c);
    }

    const net::Nanos now = net::mono_ns();
    for (auto& c : conns_) {
        if (c->dead) continue;
        if (!c->logged_in && now - c->connected_at > cfg_.login_timeout_ns) { close_conn(*c); continue; }
        if (c->logged_in && now - c->last_recv > cfg_.receive_timeout_ns) { close_conn(*c); continue; }
        pump(*c);
    }
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(), [](auto& c) { return c->dead; }),
                 conns_.end());
}

} // namespace ouch
