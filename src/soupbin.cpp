#include "ouch/soupbin.hpp"

#include "ouch/wire.hpp"

#include <charconv>
#include <cstring>
#include <string>

namespace ouch::soupbin {

void append_packet(std::vector<std::uint8_t>& out, char type, std::span<const std::uint8_t> payload) {
    const std::size_t len = 1 + payload.size();
    out.reserve(out.size() + 2 + len);
    out.push_back(static_cast<std::uint8_t>(len >> 8));
    out.push_back(static_cast<std::uint8_t>(len & 0xFF));
    out.push_back(static_cast<std::uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
}

namespace {

std::string seq_text(std::uint64_t seq) {
    char b[24];
    auto [p, ec] = std::to_chars(b, b + sizeof b, seq);
    (void)ec;
    return std::string(b, p);
}

bool parse_seq(std::string_view s, std::uint64_t& out) noexcept {
    if (s.empty()) { out = 0; return true; }
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}

} // namespace

void append_login_request(std::vector<std::uint8_t>& out, const LoginRequest& req) {
    std::uint8_t body[kUsernameLen + kPasswordLen + kSessionLen + kSeqNumLen];
    wire::Writer w(body, sizeof body);
    w.alpha(req.username, kUsernameLen);
    w.alpha(req.password, kPasswordLen);
    w.alpha_right(req.session, kSessionLen);
    w.alpha_right(seq_text(req.sequence), kSeqNumLen);
    append_packet(out, packet::LoginRequest, {body, w.size()});
}

void append_login_accepted(std::vector<std::uint8_t>& out, std::string_view session, std::uint64_t seq) {
    std::uint8_t body[kSessionLen + kSeqNumLen];
    wire::Writer w(body, sizeof body);
    w.alpha_right(session, kSessionLen);       // spec: left-padded with spaces
    w.alpha_right(seq_text(seq), kSeqNumLen);  // spec: left-padded with spaces
    append_packet(out, packet::LoginAccepted, {body, w.size()});
}

void append_login_rejected(std::vector<std::uint8_t>& out, char reason) {
    const auto b = static_cast<std::uint8_t>(reason);
    append_packet(out, packet::LoginRejected, {&b, 1});
}

bool parse_login_request(std::span<const std::uint8_t> payload, LoginRequest& out) noexcept {
    if (payload.size() != kUsernameLen + kPasswordLen + kSessionLen + kSeqNumLen) return false;
    wire::Reader r(payload.data(), payload.size());
    out.username = r.alpha(kUsernameLen);
    out.password = r.alpha(kPasswordLen);
    out.session  = r.alpha(kSessionLen);
    return parse_seq(r.alpha(kSeqNumLen), out.sequence) && r.ok();
}

bool parse_login_accepted(std::span<const std::uint8_t> payload, LoginAccepted& out) noexcept {
    if (payload.size() != kSessionLen + kSeqNumLen) return false;
    wire::Reader r(payload.data(), payload.size());
    out.session = r.alpha(kSessionLen);
    return parse_seq(r.alpha(kSeqNumLen), out.sequence) && r.ok();
}

void Framer::feed(std::span<const std::uint8_t> bytes) {
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
}

bool Framer::next(Packet& out) noexcept {
    const std::size_t avail = buf_.size() - pos_;
    if (avail < 3) return false;
    const std::size_t len = static_cast<std::size_t>(buf_[pos_]) << 8 | buf_[pos_ + 1];
    if (len == 0) {  // a packet always has at least the type byte
        malformed_ = true;
        return false;
    }
    if (avail < 2 + len) return false;
    out.type    = static_cast<char>(buf_[pos_ + 2]);
    out.payload = {buf_.data() + pos_ + 3, len - 1};
    pos_ += 2 + len;
    return true;
}

void Framer::compact() {
    if (pos_ == 0) return;
    if (pos_ == buf_.size()) {
        buf_.clear();
    } else {
        std::memmove(buf_.data(), buf_.data() + pos_, buf_.size() - pos_);
        buf_.resize(buf_.size() - pos_);
    }
    pos_ = 0;
}

} // namespace ouch::soupbin
