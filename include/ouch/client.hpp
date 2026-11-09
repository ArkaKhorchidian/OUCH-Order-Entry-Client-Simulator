#pragma once

// OuchClient: OUCH 5.0 order-entry client over SoupBinTCP.
//
//   connect()  TCP connect + Login Request; waits for Login Accepted/Rejected
//   poll()     read, reassemble packets, dispatch sequenced messages, send
//              client heartbeats, detect a silent server
//   reconnect() log back in with the same session and the next expected
//              sequence number: the server replays anything missed
//
// Sequence numbers are counted locally (SoupBinTCP does not put them on the
// wire), starting from the value in Login Accepted. UserRefNums are assigned
// strictly increasing across reconnects, as the spec requires; after a
// process restart, account_query() recovers the next valid value.
//
// The OrderTracker keeps a local view of every order (open, executed, dead)
// built purely from the exchange's messages, so the client state always
// reflects what the exchange has confirmed, never what we hoped it did.

#include "messages.hpp"
#include "net.hpp"
#include "soupbin.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ouch {

struct ClientConfig {
    std::string   host{"127.0.0.1"};
    std::uint16_t port{0};
    std::string   username;
    std::string   password;
    std::string   session;                 // blank = current session
    std::uint64_t next_seq{1};             // first sequenced message wanted
    UserRefNum    next_user_ref{1};
    std::uint64_t heartbeat_interval_ns{1'000'000'000ULL};
    std::uint64_t server_timeout_ns{15'000'000'000ULL};
    int           connect_timeout_ms{2'000};
};

enum class OrderStatus : std::uint8_t { PendingNew, Live, Dead, Rejected };

struct TrackedOrder {
    UserRefNum  user_ref{0};
    Symbol      symbol;
    char        side{side::Buy};
    Price       price{0};
    Qty         open{0};       // shares still working
    Qty         executed{0};
    OrderRef    order_ref{0};
    OrderStatus status{OrderStatus::PendingNew};
    std::uint16_t reject_reason{0};
};

class OrderTracker {
public:
    void on_sent(const EnterOrder& m);
    void on_message(const OutboundMsg& m);

    [[nodiscard]] const TrackedOrder* find(UserRefNum ref) const;
    [[nodiscard]] std::size_t live_count() const;
    [[nodiscard]] const std::unordered_map<UserRefNum, TrackedOrder>& all() const { return orders_; }

private:
    std::unordered_map<UserRefNum, TrackedOrder> orders_;
};

class OuchClient {
public:
    enum class State : std::uint8_t { Disconnected, LoginSent, Active, Rejected, EndOfSession };

    // Called for every sequenced message, with its SoupBinTCP sequence number.
    using MessageHandler = std::function<void(const OutboundMsg&, std::uint64_t seq)>;

    explicit OuchClient(ClientConfig cfg);
    ~OuchClient();

    OuchClient(const OuchClient&) = delete;
    OuchClient& operator=(const OuchClient&) = delete;

    void on_message(MessageHandler h) { handler_ = std::move(h); }

    // Blocks up to `timeout_ms` for Login Accepted. True when Active.
    bool connect(int timeout_ms = 2'000);
    bool reconnect(int timeout_ms = 2'000);
    void disconnect(bool send_logout = true);

    // Drop the TCP connection without a Logout (simulates a network failure).
    void drop_connection();

    // One I/O iteration. Returns false once disconnected.
    bool poll(int timeout_ms = 0);

    // Poll until `pred()` is true or the timeout expires.
    bool poll_until(const std::function<bool()>& pred, int timeout_ms);

    // ── Order entry ──────────────────────────────────────────────────────────
    // user_ref_num == 0 means "assign the next one". Returns the UserRefNum
    // used, or 0 if not connected.
    UserRefNum enter_order(EnterOrder m);
    UserRefNum replace_order(ReplaceOrder m);
    bool cancel_order(UserRefNum ref, Qty new_size = 0);
    bool send(const InboundMsg& m);
    bool account_query();

    // ── State ────────────────────────────────────────────────────────────────
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] const std::string& session() const noexcept { return session_; }
    [[nodiscard]] std::uint64_t next_expected_seq() const noexcept { return next_seq_; }
    [[nodiscard]] UserRefNum next_user_ref() const noexcept { return next_user_ref_; }
    [[nodiscard]] char login_reject_reason() const noexcept { return reject_reason_; }
    [[nodiscard]] const OrderTracker& orders() const noexcept { return tracker_; }

private:
    bool send_packet(char type, std::span<const std::uint8_t> payload);
    void on_packet(const soupbin::Packet& p);
    bool open_and_login(int timeout_ms);

    ClientConfig cfg_;
    int          fd_{-1};
    State        state_{State::Disconnected};
    std::string  session_;
    std::uint64_t next_seq_;
    UserRefNum   next_user_ref_;
    char         reject_reason_{0};

    soupbin::Framer           framer_;
    std::vector<std::uint8_t> out_;
    std::vector<std::uint8_t> scratch_;
    net::Nanos   last_send_{0};
    net::Nanos   last_recv_{0};

    OrderTracker   tracker_;
    MessageHandler handler_;
};

} // namespace ouch
