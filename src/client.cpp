#include "ouch/client.hpp"

#include <algorithm>
#include <poll.h>

namespace ouch {

// ── OrderTracker ──────────────────────────────────────────────────────────────

void OrderTracker::on_sent(const EnterOrder& m) {
    TrackedOrder t;
    t.user_ref = m.user_ref_num;
    t.symbol   = m.symbol;
    t.side     = m.side;
    t.price    = m.price;
    t.open     = m.quantity;
    orders_[m.user_ref_num] = t;
}

void OrderTracker::on_message(const OutboundMsg& msg) {
    std::visit([&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, OrderAccepted>) {
            auto& t = orders_[m.user_ref_num];
            t.user_ref  = m.user_ref_num;
            t.symbol    = m.symbol;
            t.side      = m.side;
            t.price     = m.price;
            t.open      = m.quantity;
            t.order_ref = m.order_ref;
            t.status    = m.order_state == order_state::Live ? OrderStatus::Live : OrderStatus::Dead;
        } else if constexpr (std::is_same_v<T, OrderExecuted>) {
            auto it = orders_.find(m.user_ref_num);
            if (it == orders_.end()) return;
            auto& t = it->second;
            t.executed += m.quantity;
            t.open = t.open > m.quantity ? t.open - m.quantity : 0;
            if (t.open == 0) t.status = OrderStatus::Dead;
        } else if constexpr (std::is_same_v<T, OrderCanceled>) {
            auto it = orders_.find(m.user_ref_num);
            if (it == orders_.end()) return;
            auto& t = it->second;
            t.open = t.open > m.quantity ? t.open - m.quantity : 0;
            if (t.open == 0) t.status = OrderStatus::Dead;
        } else if constexpr (std::is_same_v<T, OrderReplaced>) {
            TrackedOrder t;
            if (auto it = orders_.find(m.orig_user_ref_num); it != orders_.end()) {
                t = it->second;
                it->second.status = OrderStatus::Dead;  // the old UserRefNum no longer addresses it
                it->second.open = 0;
            }
            t.user_ref  = m.user_ref_num;
            t.price     = m.price;
            t.open      = m.quantity;
            t.order_ref = m.order_ref;
            t.symbol    = m.symbol;
            t.side      = m.side;
            t.status    = OrderStatus::Live;
            orders_[m.user_ref_num] = t;
        } else if constexpr (std::is_same_v<T, OrderModified>) {
            auto it = orders_.find(m.user_ref_num);
            if (it == orders_.end()) return;
            it->second.side = m.side;
            it->second.open = m.quantity;
            if (m.quantity == 0) it->second.status = OrderStatus::Dead;
        } else if constexpr (std::is_same_v<T, Rejected>) {
            auto& t = orders_[m.user_ref_num];
            t.user_ref = m.user_ref_num;
            t.status = OrderStatus::Rejected;
            t.reject_reason = m.reason;
            t.open = 0;
        }
    }, msg);
}

const TrackedOrder* OrderTracker::find(UserRefNum ref) const {
    auto it = orders_.find(ref);
    return it == orders_.end() ? nullptr : &it->second;
}

std::size_t OrderTracker::live_count() const {
    std::size_t n = 0;
    for (const auto& [ref, t] : orders_) n += t.status == OrderStatus::Live;
    return n;
}

// ── OuchClient ────────────────────────────────────────────────────────────────

OuchClient::OuchClient(ClientConfig cfg)
    : cfg_(std::move(cfg)), session_(cfg_.session), next_seq_(cfg_.next_seq),
      next_user_ref_(cfg_.next_user_ref) {}

OuchClient::~OuchClient() { disconnect(true); }

bool OuchClient::open_and_login(int timeout_ms) {
    fd_ = net::tcp_connect(cfg_.host, cfg_.port, cfg_.connect_timeout_ms);
    if (fd_ < 0) { state_ = State::Disconnected; return false; }
    framer_.reset();
    out_.clear();
    reject_reason_ = 0;

    soupbin::append_login_request(out_, {cfg_.username, cfg_.password, session_, next_seq_});
    if (!net::flush(fd_, out_)) { net::close_fd(fd_); return false; }
    last_send_ = last_recv_ = net::mono_ns();
    state_ = State::LoginSent;
    poll_until([&] { return state_ != State::LoginSent; }, timeout_ms);
    return state_ == State::Active;
}

bool OuchClient::connect(int timeout_ms) {
    if (state_ == State::Active) return true;
    return open_and_login(timeout_ms);
}

bool OuchClient::reconnect(int timeout_ms) {
    drop_connection();
    return open_and_login(timeout_ms);
}

void OuchClient::drop_connection() {
    net::close_fd(fd_);
    // Terminal states (Rejected, EndOfSession) carry information the caller
    // needs; only a live or in-progress session becomes Disconnected.
    if (state_ == State::Active || state_ == State::LoginSent) state_ = State::Disconnected;
}

void OuchClient::disconnect(bool send_logout) {
    if (fd_ >= 0 && send_logout && state_ == State::Active) {
        soupbin::append_packet(out_, soupbin::packet::LogoutRequest);
        (void)net::flush(fd_, out_);
    }
    drop_connection();
}

bool OuchClient::send_packet(char type, std::span<const std::uint8_t> payload) {
    if (fd_ < 0) return false;
    soupbin::append_packet(out_, type, payload);
    if (!net::flush(fd_, out_)) { drop_connection(); return false; }
    last_send_ = net::mono_ns();
    return true;
}

bool OuchClient::send(const InboundMsg& m) {
    if (state_ != State::Active) return false;
    std::uint8_t buf[kMaxMessageSize];
    const std::size_t n = encode(m, buf, sizeof buf);
    if (n == 0) return false;
    return send_packet(soupbin::packet::UnsequencedData, {buf, n});
}

UserRefNum OuchClient::enter_order(EnterOrder m) {
    if (state_ != State::Active) return 0;
    if (m.user_ref_num == 0) m.user_ref_num = next_user_ref_;
    if (m.user_ref_num >= next_user_ref_) next_user_ref_ = m.user_ref_num + 1;
    tracker_.on_sent(m);
    return send(m) ? m.user_ref_num : 0;
}

UserRefNum OuchClient::replace_order(ReplaceOrder m) {
    if (state_ != State::Active) return 0;
    if (m.user_ref_num == 0) m.user_ref_num = next_user_ref_;
    if (m.user_ref_num >= next_user_ref_) next_user_ref_ = m.user_ref_num + 1;
    return send(m) ? m.user_ref_num : 0;
}

bool OuchClient::cancel_order(UserRefNum ref, Qty new_size) {
    return send(CancelOrder{ref, new_size, {}});
}

bool OuchClient::account_query() { return send(AccountQuery{}); }

void OuchClient::on_packet(const soupbin::Packet& p) {
    switch (p.type) {
        case soupbin::packet::LoginAccepted: {
            soupbin::LoginAccepted acc;
            if (!soupbin::parse_login_accepted(p.payload, acc)) { drop_connection(); return; }
            session_  = std::string(acc.session);
            next_seq_ = acc.sequence;
            state_    = State::Active;
            break;
        }
        case soupbin::packet::LoginRejected:
            reject_reason_ = p.payload.empty() ? '?' : static_cast<char>(p.payload[0]);
            net::close_fd(fd_);
            state_ = State::Rejected;
            break;
        case soupbin::packet::SequencedData: {
            const std::uint64_t seq = next_seq_++;
            auto msg = decode_outbound(p.payload);
            if (!msg) break;  // count it (sequence must advance) but skip
            if (const auto* q = std::get_if<AccountQueryResponse>(&*msg))
                if (q->next_user_ref_num > next_user_ref_) next_user_ref_ = q->next_user_ref_num;
            tracker_.on_message(*msg);
            if (handler_) handler_(*msg, seq);
            break;
        }
        case soupbin::packet::EndOfSession:
            net::close_fd(fd_);
            state_ = State::EndOfSession;
            break;
        case soupbin::packet::ServerHeartbeat:
        case soupbin::packet::Debug:
        default:
            break;
    }
}

bool OuchClient::poll(int timeout_ms) {
    if (fd_ < 0) return false;
    pollfd p{fd_, POLLIN, 0};
    if (!out_.empty()) p.events |= POLLOUT;
    ::poll(&p, 1, timeout_ms);

    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        // Drain whatever arrived before the hangup (e.g. End of Session).
        p.revents |= POLLIN;
    }
    if (p.revents & POLLIN) {
        for (;;) {
            const long n = net::read_some(fd_, scratch_);
            if (n < 0) {
                // Process anything already buffered, then report the drop.
                soupbin::Packet pk;
                while (framer_.next(pk)) on_packet(pk);
                framer_.compact();
                drop_connection();
                return false;
            }
            if (n == 0) break;
            last_recv_ = net::mono_ns();
            framer_.feed(scratch_);
        }
        soupbin::Packet pk;
        while (fd_ >= 0 && framer_.next(pk)) on_packet(pk);
        if (framer_.malformed()) { drop_connection(); return false; }
        framer_.compact();
    }
    if (fd_ < 0) return false;

    const net::Nanos now = net::mono_ns();
    if (!out_.empty() && !net::flush(fd_, out_)) { drop_connection(); return false; }
    if (state_ == State::Active && now - last_send_ >= cfg_.heartbeat_interval_ns)
        send_packet(soupbin::packet::ClientHeartbeat, {});
    if (now - last_recv_ > cfg_.server_timeout_ns) { drop_connection(); return false; }
    return fd_ >= 0;
}

bool OuchClient::poll_until(const std::function<bool()>& pred, int timeout_ms) {
    const net::Nanos deadline = net::mono_ns() + static_cast<net::Nanos>(timeout_ms) * 1'000'000ULL;
    while (!pred()) {
        const net::Nanos now = net::mono_ns();
        if (now >= deadline) return false;
        const int left = static_cast<int>((deadline - now) / 1'000'000ULL);
        if (!poll(std::max(1, std::min(left, 50))) && !pred()) return pred();
    }
    return true;
}

} // namespace ouch
