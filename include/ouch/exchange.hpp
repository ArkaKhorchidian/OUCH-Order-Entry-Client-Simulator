#pragma once

// ExchangeCore: OUCH 5.0 order-entry semantics on top of a price-time
// priority matching engine. No sockets, no clocks: the server feeds it
// decoded bytes and a timestamp, and reads back each account's journal of
// sequenced outbound messages. That keeps every rule here unit-testable and
// makes replay after reconnect trivial (the journal *is* the replay source).
//
// Implemented:
//   Enter Order (O)     validation -> Rejected (J) with Appendix C codes,
//                       Order Accepted (A), then matching
//   Replace Order (U)   new UserRefNum, keeps priority only if price is
//                       unchanged and size does not increase
//   Cancel Order (X)    "new intended order size" semantics
//   Modify Order (M)    reduce size / change sell-side type
//   Mass Cancel (C)     by account, optionally by symbol
//   Disable/Enable (D/E), Account Query (Q)
//   Day and IOC time in force
//
// UserRefNum rule (spec): strictly increasing per account for the day.
// An Enter/Replace with a UserRefNum at or below the last one processed is
// treated as a retransmission and ignored.

#include "messages.hpp"

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ouch {

struct ExchangeConfig {
    std::vector<std::string> symbols;          // empty = accept any symbol
    Qty   max_order_qty{1'000'000};
    Price max_price{kMaxPrice};
};

struct ExchangeStats {
    std::uint64_t orders_accepted{0};
    std::uint64_t orders_rejected{0};
    std::uint64_t executions{0};
    std::uint64_t ignored_retransmissions{0};
    std::uint64_t ignored_unknown_order{0};
    std::uint64_t malformed{0};
};

class ExchangeCore {
public:
    using Journal = std::vector<std::vector<std::uint8_t>>;  // index 0 = sequence 1

    explicit ExchangeCore(ExchangeConfig cfg = {});
    ~ExchangeCore();

    // Process one inbound OUCH message (the payload of an Unsequenced Data
    // packet) from `account`. Outbound messages are appended to journals.
    void handle(std::string_view account, std::span<const std::uint8_t> msg, Timestamp now);
    void handle(std::string_view account, const InboundMsg& msg, Timestamp now);

    // Creates the account (and its Start of Day event) if needed.
    const Journal& journal(std::string_view account);

    // Emit End of Day to every account and cancel all resting Day orders.
    void end_of_day(Timestamp now);

    [[nodiscard]] const ExchangeStats& stats() const noexcept { return stats_; }

    // Book inspection for tests and tools.
    struct Level { Price price; Qty qty; std::size_t orders; };
    [[nodiscard]] std::vector<Level> bids(std::string_view symbol) const;
    [[nodiscard]] std::vector<Level> asks(std::string_view symbol) const;

private:
    struct Order;
    struct Book;
    struct Account;

    Account& account(std::string_view name);
    Book*    book(std::string_view symbol, bool create);

    void on_enter(Account& a, const EnterOrder& m, Timestamp now);
    void on_replace(Account& a, const ReplaceOrder& m, Timestamp now);
    void on_cancel(Account& a, const CancelOrder& m, Timestamp now);
    void on_modify(Account& a, const ModifyOrder& m, Timestamp now);
    void on_mass_cancel(Account& a, const MassCancel& m, Timestamp now);

    void match(Order& o, Timestamp now);
    void rest(Order& o);
    void unrest(Order& o);
    void kill(Order& o);
    void cancel_shares(Order& o, Qty decrement, char reason, Timestamp now);

    template <class Msg> void emit(Account& a, const Msg& m);

    ExchangeConfig cfg_;
    std::map<std::string, std::unique_ptr<Account>, std::less<>> accounts_;
    std::map<std::string, std::unique_ptr<Book>, std::less<>>    books_;
    std::unordered_map<OrderRef, std::unique_ptr<Order>>         orders_;
    OrderRef      next_order_ref_{1};
    MatchNum      next_match_{1};
    ExchangeStats stats_{};
};

} // namespace ouch
