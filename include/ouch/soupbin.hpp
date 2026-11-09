#pragma once

// SoupBinTCP: the session layer OUCH runs on.
// Source: Nasdaq SoupBinTCP specification, version 3.00.
//
//   [length: 2 bytes big-endian][type: 1 byte][payload]
//
// `length` counts the type byte plus the payload. TCP may split or merge
// packets arbitrarily, so the receiver reassembles them with a Framer.
//
// Server -> client sequenced messages carry no sequence number on the wire:
// both sides count them, starting at 1 for the first message of a session.
// That is what makes reconnection work: the client logs back in with
// "session X, next sequence N" and the server replays from N.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ouch::soupbin {

namespace packet {
inline constexpr char Debug = '+';
// Server -> client
inline constexpr char LoginAccepted = 'A';
inline constexpr char LoginRejected = 'J';
inline constexpr char SequencedData = 'S';
inline constexpr char ServerHeartbeat = 'H';
inline constexpr char EndOfSession = 'Z';
// Client -> server
inline constexpr char LoginRequest = 'L';
inline constexpr char UnsequencedData = 'U';
inline constexpr char ClientHeartbeat = 'R';
inline constexpr char LogoutRequest = 'O';
}

namespace login_reject {
inline constexpr char NotAuthorized = 'A';
inline constexpr char SessionNotAvailable = 'S';
}

inline constexpr std::size_t kUsernameLen = 6;
inline constexpr std::size_t kPasswordLen = 10;
inline constexpr std::size_t kSessionLen  = 10;
inline constexpr std::size_t kSeqNumLen   = 20;
inline constexpr std::size_t kMaxPacket   = 2 + 65'535;

struct LoginRequest {
    std::string_view username;
    std::string_view password;
    std::string_view session;        // blank = currently active session
    std::uint64_t    sequence{1};    // next sequenced message wanted; 0 = most recent
};

struct LoginAccepted {
    std::string_view session;
    std::uint64_t    sequence{0};    // next sequence number the server will send
};

// ── Packet builders: append one complete packet to `out` ─────────────────────

void append_packet(std::vector<std::uint8_t>& out, char type,
                   std::span<const std::uint8_t> payload = {});
void append_login_request(std::vector<std::uint8_t>& out, const LoginRequest& req);
void append_login_accepted(std::vector<std::uint8_t>& out, std::string_view session, std::uint64_t seq);
void append_login_rejected(std::vector<std::uint8_t>& out, char reason);

// ── Packet parsers (payload = bytes after the type byte) ─────────────────────

[[nodiscard]] bool parse_login_request(std::span<const std::uint8_t> payload, LoginRequest& out) noexcept;
[[nodiscard]] bool parse_login_accepted(std::span<const std::uint8_t> payload, LoginAccepted& out) noexcept;

// ── Stream reassembly ────────────────────────────────────────────────────────

struct Packet {
    char                          type;
    std::span<const std::uint8_t> payload;
};

// Accumulates a TCP byte stream and yields complete packets. Packets returned
// by next() stay valid until the next call to feed() or compact().
class Framer {
public:
    explicit Framer(std::size_t reserve = 64 * 1024) { buf_.reserve(reserve); }

    void feed(std::span<const std::uint8_t> bytes);

    // Returns false when no complete packet is buffered.
    bool next(Packet& out) noexcept;

    // Drop consumed bytes. Call after draining next().
    void compact();

    [[nodiscard]] std::size_t buffered() const noexcept { return buf_.size() - pos_; }
    [[nodiscard]] bool malformed() const noexcept { return malformed_; }
    void reset() { buf_.clear(); pos_ = 0; malformed_ = false; }

private:
    std::vector<std::uint8_t> buf_;
    std::size_t               pos_{0};
    bool                      malformed_{false};
};

} // namespace ouch::soupbin
