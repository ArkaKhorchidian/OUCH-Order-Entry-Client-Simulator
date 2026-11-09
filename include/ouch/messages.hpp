#pragma once

// OUCH 5.0 message definitions and codec.
//
// Every inbound (client -> exchange) and outbound (exchange -> client) message
// in the OUCH 5.0 specification, with field order and widths taken from the
// spec's layout tables. Each message is a plain struct; encode() writes the
// exact wire bytes, decode_inbound() / decode_outbound() parse them.
//
// Message type characters overlap between directions ('U' is Replace Order
// inbound and Order Replaced outbound, 'C' is Mass Cancel inbound and Order
// Canceled outbound, ...), so decoding is always direction specific.

#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace ouch {

inline constexpr std::size_t kMaxMessageSize = 512;

// Fixed-width Alpha value, stored without padding.
template <std::size_t N>
struct Alpha {
    std::array<char, N> data{};
    std::uint8_t        len{0};

    Alpha() = default;
    Alpha(std::string_view s) noexcept { assign(s); }  // NOLINT: implicit by design
    Alpha(const char* s) noexcept { assign(s); }       // NOLINT
    Alpha(const std::string& s) noexcept { assign(s); } // NOLINT

    void assign(std::string_view s) noexcept {
        const std::size_t n = s.size() < N ? s.size() : N;
        data.fill(0);
        std::memcpy(data.data(), s.data(), n);
        len = static_cast<std::uint8_t>(n);
    }
    [[nodiscard]] std::string_view view() const noexcept { return {data.data(), len}; }
    bool operator==(const Alpha&) const = default;
};

using Symbol  = Alpha<kSymbolLen>;
using ClOrdId = Alpha<kClOrdIdLen>;
using Firm    = Alpha<kFirmLen>;

// Optional appendage: a list of TagValue elements
//   [length:1][tag:1][value:length-1]
// Values here are at most 8 bytes (the largest Appendix A option is a Price).
struct Appendage {
    struct Option {
        std::uint8_t                tag{0};
        std::uint8_t                size{0};
        std::array<std::uint8_t, 8> value{};
        bool operator==(const Option&) const = default;
    };
    static constexpr std::size_t kMaxOptions = 12;

    std::array<Option, kMaxOptions> options{};
    std::uint8_t                    count{0};

    bool set_u8(std::uint8_t tag, std::uint8_t v) noexcept;
    bool set_u16(std::uint8_t tag, std::uint16_t v) noexcept;
    bool set_u32(std::uint8_t tag, std::uint32_t v) noexcept;
    bool set_u64(std::uint8_t tag, std::uint64_t v) noexcept;
    bool set_alpha(std::uint8_t tag, std::string_view s, std::size_t width) noexcept;

    [[nodiscard]] const Option* find(std::uint8_t tag) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> get_u32(std::uint8_t tag) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> get_u64(std::uint8_t tag) const noexcept;
    [[nodiscard]] std::optional<char> get_char(std::uint8_t tag) const noexcept;

    [[nodiscard]] std::size_t wire_size() const noexcept;
    bool operator==(const Appendage& o) const noexcept;
};

// ── Inbound: client -> exchange ───────────────────────────────────────────────

struct EnterOrder {                  // 'O'
    UserRefNum user_ref_num{0};
    char       side{side::Buy};
    Qty        quantity{0};
    Symbol     symbol;
    Price      price{0};
    char       time_in_force{tif::Day};
    char       display{display::Visible};
    char       capacity{capacity::Agency};
    char       intermarket_sweep{'N'};
    char       cross_type{cross_type::Continuous};
    ClOrdId    cl_ord_id;
    Appendage  appendage;
    bool operator==(const EnterOrder&) const = default;
};

struct ReplaceOrder {                // 'U'
    UserRefNum orig_user_ref_num{0};
    UserRefNum user_ref_num{0};
    Qty        quantity{0};
    Price      price{0};
    char       time_in_force{tif::Day};
    char       display{display::Visible};
    char       intermarket_sweep{'N'};
    ClOrdId    cl_ord_id;
    Appendage  appendage;
    bool operator==(const ReplaceOrder&) const = default;
};

struct CancelOrder {                 // 'X'
    UserRefNum user_ref_num{0};
    Qty        quantity{0};          // new intended order size; 0 cancels all
    Appendage  appendage;
    bool operator==(const CancelOrder&) const = default;
};

struct ModifyOrder {                 // 'M'
    UserRefNum user_ref_num{0};
    char       side{side::Sell};
    Qty        quantity{0};
    Appendage  appendage;
    bool operator==(const ModifyOrder&) const = default;
};

struct MassCancel {                  // 'C'
    UserRefNum user_ref_num{0};
    Firm       firm;
    Symbol     symbol;               // blank = all symbols
    Appendage  appendage;
    bool operator==(const MassCancel&) const = default;
};

struct DisableOrderEntry {           // 'D'
    UserRefNum user_ref_num{0};
    Firm       firm;
    Appendage  appendage;
    bool operator==(const DisableOrderEntry&) const = default;
};

struct EnableOrderEntry {            // 'E'
    UserRefNum user_ref_num{0};
    Firm       firm;
    Appendage  appendage;
    bool operator==(const EnableOrderEntry&) const = default;
};

struct AccountQuery {                // 'Q'
    Appendage appendage;
    bool operator==(const AccountQuery&) const = default;
};

using InboundMsg = std::variant<EnterOrder, ReplaceOrder, CancelOrder, ModifyOrder,
                                MassCancel, DisableOrderEntry, EnableOrderEntry, AccountQuery>;

// ── Outbound: exchange -> client ──────────────────────────────────────────────

struct SystemEvent {                 // 'S'
    Timestamp timestamp{0};
    char      event_code{event_code::StartOfDay};
    bool operator==(const SystemEvent&) const = default;
};

struct OrderAccepted {               // 'A'
    Timestamp  timestamp{0};
    UserRefNum user_ref_num{0};
    char       side{side::Buy};
    Qty        quantity{0};
    Symbol     symbol;
    Price      price{0};
    char       time_in_force{tif::Day};
    char       display{display::Visible};
    OrderRef   order_ref{0};
    char       capacity{capacity::Agency};
    char       intermarket_sweep{'N'};
    char       cross_type{cross_type::Continuous};
    char       order_state{order_state::Live};
    ClOrdId    cl_ord_id;
    Appendage  appendage;
    bool operator==(const OrderAccepted&) const = default;
};

struct OrderReplaced {               // 'U'
    Timestamp  timestamp{0};
    UserRefNum orig_user_ref_num{0};
    UserRefNum user_ref_num{0};
    char       side{side::Buy};
    Qty        quantity{0};
    Symbol     symbol;
    Price      price{0};
    char       time_in_force{tif::Day};
    char       display{display::Visible};
    OrderRef   order_ref{0};
    char       capacity{capacity::Agency};
    char       intermarket_sweep{'N'};
    char       cross_type{cross_type::Continuous};
    char       order_state{order_state::Live};
    ClOrdId    cl_ord_id;
    Appendage  appendage;
    bool operator==(const OrderReplaced&) const = default;
};

struct OrderCanceled {               // 'C'
    Timestamp  timestamp{0};
    UserRefNum user_ref_num{0};
    Qty        quantity{0};          // shares decremented by this cancel
    char       reason{cancel_reason::UserRequested};
    Appendage  appendage;
    bool operator==(const OrderCanceled&) const = default;
};

struct AiqCanceled {                 // 'D'
    Timestamp  timestamp{0};
    UserRefNum user_ref_num{0};
    Qty        decrement_shares{0};
    char       reason{cancel_reason::SelfMatchPrevention};
    Qty        quantity_prevented{0};
    Price      execution_price{0};
    char       liquidity_flag{liquidity::Added};
    char       aiq_strategy{' '};
    Appendage  appendage;
    bool operator==(const AiqCanceled&) const = default;
};

struct OrderExecuted {               // 'E'
    Timestamp  timestamp{0};
    UserRefNum user_ref_num{0};
    Qty        quantity{0};          // shares executed in this fill
    Price      price{0};
    char       liquidity_flag{liquidity::Added};
    MatchNum   match_number{0};
    Appendage  appendage;
    bool operator==(const OrderExecuted&) const = default;
};

struct BrokenTrade {                 // 'B'
    Timestamp  timestamp{0};
    UserRefNum user_ref_num{0};
    MatchNum   match_number{0};
    char       reason{'E'};
    ClOrdId    cl_ord_id;
    Appendage  appendage;
    bool operator==(const BrokenTrade&) const = default;
};

struct Rejected {                    // 'J'
    Timestamp     timestamp{0};
    UserRefNum    user_ref_num{0};
    std::uint16_t reason{0};
    ClOrdId       cl_ord_id;
    Appendage     appendage;
    bool operator==(const Rejected&) const = default;
};

struct CancelPending {               // 'P'
    Timestamp  timestamp{0};
    UserRefNum user_ref_num{0};
    Appendage  appendage;
    bool operator==(const CancelPending&) const = default;
};

struct CancelReject {                // 'I'
    Timestamp  timestamp{0};
    UserRefNum user_ref_num{0};
    Appendage  appendage;
    bool operator==(const CancelReject&) const = default;
};

struct OrderPriorityUpdate {         // 'T'
    Timestamp  timestamp{0};
    UserRefNum user_ref_num{0};
    Price      price{0};
    char       display{display::Visible};
    OrderRef   order_ref{0};
    Appendage  appendage;
    bool operator==(const OrderPriorityUpdate&) const = default;
};

struct OrderModified {               // 'M'
    Timestamp  timestamp{0};
    UserRefNum user_ref_num{0};
    char       side{side::Sell};
    Qty        quantity{0};          // shares now outstanding
    Appendage  appendage;
    bool operator==(const OrderModified&) const = default;
};

struct OrderRestated {               // 'R'
    Timestamp  timestamp{0};
    UserRefNum user_ref_num{0};
    char       reason{'R'};
    Appendage  appendage;
    bool operator==(const OrderRestated&) const = default;
};

struct MassCancelResponse {          // 'X'
    Timestamp  timestamp{0};
    UserRefNum user_ref_num{0};
    Firm       firm;
    Symbol     symbol;
    Appendage  appendage;
    bool operator==(const MassCancelResponse&) const = default;
};

struct DisableOrderEntryResponse {   // 'G'
    Timestamp  timestamp{0};
    UserRefNum user_ref_num{0};
    Firm       firm;
    Appendage  appendage;
    bool operator==(const DisableOrderEntryResponse&) const = default;
};

struct EnableOrderEntryResponse {    // 'K'
    Timestamp  timestamp{0};
    UserRefNum user_ref_num{0};
    Firm       firm;
    Appendage  appendage;
    bool operator==(const EnableOrderEntryResponse&) const = default;
};

struct AccountQueryResponse {        // 'Q'
    Timestamp  timestamp{0};
    UserRefNum next_user_ref_num{0};
    Appendage  appendage;
    bool operator==(const AccountQueryResponse&) const = default;
};

using OutboundMsg = std::variant<SystemEvent, OrderAccepted, OrderReplaced, OrderCanceled,
                                 AiqCanceled, OrderExecuted, BrokenTrade, Rejected,
                                 CancelPending, CancelReject, OrderPriorityUpdate,
                                 OrderModified, OrderRestated, MassCancelResponse,
                                 DisableOrderEntryResponse, EnableOrderEntryResponse,
                                 AccountQueryResponse>;

// ── Codec ─────────────────────────────────────────────────────────────────────
// encode() returns the number of bytes written, or 0 if `cap` is too small.

std::size_t encode(const EnterOrder&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const ReplaceOrder&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const CancelOrder&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const ModifyOrder&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const MassCancel&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const DisableOrderEntry&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const EnableOrderEntry&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const AccountQuery&, std::uint8_t* buf, std::size_t cap) noexcept;

std::size_t encode(const SystemEvent&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const OrderAccepted&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const OrderReplaced&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const OrderCanceled&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const AiqCanceled&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const OrderExecuted&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const BrokenTrade&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const Rejected&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const CancelPending&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const CancelReject&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const OrderPriorityUpdate&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const OrderModified&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const OrderRestated&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const MassCancelResponse&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const DisableOrderEntryResponse&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const EnableOrderEntryResponse&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const AccountQueryResponse&, std::uint8_t* buf, std::size_t cap) noexcept;

std::size_t encode(const InboundMsg&, std::uint8_t* buf, std::size_t cap) noexcept;
std::size_t encode(const OutboundMsg&, std::uint8_t* buf, std::size_t cap) noexcept;

[[nodiscard]] std::optional<InboundMsg>  decode_inbound(std::span<const std::uint8_t> msg) noexcept;
[[nodiscard]] std::optional<OutboundMsg> decode_outbound(std::span<const std::uint8_t> msg) noexcept;

// Human-readable one-line rendering, for tools and logs (allocates).
[[nodiscard]] std::string to_string(const InboundMsg&);
[[nodiscard]] std::string to_string(const OutboundMsg&);

} // namespace ouch
