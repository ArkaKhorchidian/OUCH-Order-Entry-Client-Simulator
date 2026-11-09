#include "ouch/soupbin.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace ouch::soupbin;

namespace {
std::string str(const std::vector<std::uint8_t>& b, std::size_t at, std::size_t n) {
    return std::string(reinterpret_cast<const char*>(b.data() + at), n);
}
} // namespace

TEST(SoupBin, LoginRequestLayout) {
    std::vector<std::uint8_t> b;
    append_login_request(b, {"USER01", "SECRET", "", 42});
    ASSERT_EQ(b.size(), 49u);
    EXPECT_EQ(b[0], 0);
    EXPECT_EQ(b[1], 47);                         // packet length per spec
    EXPECT_EQ(b[2], 'L');
    EXPECT_EQ(str(b, 3, 6), "USER01");
    EXPECT_EQ(str(b, 9, 10), "SECRET    ");      // right-padded with spaces
    EXPECT_EQ(str(b, 19, 10), "          ");     // blank session = current
    EXPECT_EQ(str(b, 29, 20), std::string(18, ' ') + "42");
}

TEST(SoupBin, LoginAcceptedLayoutAndParse) {
    std::vector<std::uint8_t> b;
    append_login_accepted(b, "SESS1", 1001);
    ASSERT_EQ(b.size(), 33u);
    EXPECT_EQ(b[1], 31);                         // packet length per spec
    EXPECT_EQ(b[2], 'A');
    EXPECT_EQ(str(b, 3, 10), "     SESS1");      // left-padded with spaces
    LoginAccepted acc;
    ASSERT_TRUE(parse_login_accepted(std::span(b).subspan(3), acc));
    EXPECT_EQ(acc.session, "SESS1");
    EXPECT_EQ(acc.sequence, 1001u);
}

TEST(SoupBin, LoginRequestParseRoundTrip) {
    std::vector<std::uint8_t> b;
    append_login_request(b, {"abc", "pw", "SESS1", 0});
    LoginRequest req;
    ASSERT_TRUE(parse_login_request(std::span(b).subspan(3), req));
    EXPECT_EQ(req.username, "abc");
    EXPECT_EQ(req.password, "pw");
    EXPECT_EQ(req.session, "SESS1");
    EXPECT_EQ(req.sequence, 0u);
}

TEST(SoupBin, LoginRejectedAndHeartbeats) {
    std::vector<std::uint8_t> b;
    append_login_rejected(b, login_reject::NotAuthorized);
    append_packet(b, packet::ServerHeartbeat);
    EXPECT_EQ(b, (std::vector<std::uint8_t>{0, 2, 'J', 'A', 0, 1, 'H'}));
}

// TCP can split and merge packets anywhere. Feed a stream of mixed packets
// one byte at a time, and all at once, and expect identical results.
TEST(SoupBin, FramerReassemblesAcrossArbitrarySplits) {
    std::vector<std::uint8_t> stream;
    append_packet(stream, packet::ServerHeartbeat);
    for (int i = 0; i < 50; ++i) {
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(i * 7 % 300), static_cast<std::uint8_t>(i));
        append_packet(stream, packet::SequencedData, payload);
    }
    append_packet(stream, packet::EndOfSession);

    auto drain = [](Framer& f, std::vector<std::pair<char, std::size_t>>& got) {
        Packet p;
        while (f.next(p)) got.emplace_back(p.type, p.payload.size());
        f.compact();
    };

    std::vector<std::pair<char, std::size_t>> whole, bytewise, chunked;
    {
        Framer f;
        f.feed(stream);
        drain(f, whole);
        EXPECT_EQ(f.buffered(), 0u);
    }
    {
        Framer f;
        for (std::uint8_t byte : stream) {
            f.feed(std::span(&byte, 1));
            drain(f, bytewise);
        }
    }
    {
        Framer f;
        for (std::size_t i = 0; i < stream.size(); i += 37) {
            f.feed(std::span(stream).subspan(i, std::min<std::size_t>(37, stream.size() - i)));
            drain(f, chunked);
        }
    }
    ASSERT_EQ(whole.size(), 52u);
    EXPECT_EQ(whole.front().first, 'H');
    EXPECT_EQ(whole.back().first, 'Z');
    EXPECT_EQ(whole, bytewise);
    EXPECT_EQ(whole, chunked);
}

TEST(SoupBin, FramerFlagsZeroLengthPacket) {
    Framer f;
    const std::uint8_t bad[] = {0, 0, 'H'};
    f.feed(bad);
    Packet p;
    EXPECT_FALSE(f.next(p));
    EXPECT_TRUE(f.malformed());
}
