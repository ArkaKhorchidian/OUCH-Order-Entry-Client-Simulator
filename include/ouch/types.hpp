#pragma once

// OUCH 5.0 scalar types and enumerations.
// Source: Nasdaq OUCH 5.0 specification (field types, Appendix A-C).

#include <cstdint>
#include <string_view>

namespace ouch {

using Price      = std::uint64_t;  // 8 bytes, unsigned, 4 implied decimals
using Qty        = std::uint32_t;  // 4 bytes
using UserRefNum = std::uint32_t;  // unique, strictly increasing per OUCH port, starts at 1
using Timestamp  = std::uint64_t;  // nanoseconds since midnight
using OrderRef   = std::uint64_t;  // exchange-assigned Order Reference Number
using MatchNum   = std::uint64_t;

inline constexpr Price kPriceScale = 10'000;
inline constexpr Price kMaxPrice   = 1'999'999'900;  // $199,999.9900

inline constexpr std::size_t kSymbolLen  = 8;
inline constexpr std::size_t kClOrdIdLen = 14;
inline constexpr std::size_t kFirmLen    = 4;

[[nodiscard]] constexpr Price to_price(double px) noexcept {
    return static_cast<Price>(px * static_cast<double>(kPriceScale) + 0.5);
}
[[nodiscard]] constexpr double to_double(Price px) noexcept {
    return static_cast<double>(px) / static_cast<double>(kPriceScale);
}

namespace side {
inline constexpr char Buy = 'B', Sell = 'S', SellShort = 'T', SellShortExempt = 'E';
[[nodiscard]] constexpr bool valid(char c) noexcept {
    return c == Buy || c == Sell || c == SellShort || c == SellShortExempt;
}
[[nodiscard]] constexpr bool is_buy(char c) noexcept { return c == Buy; }
}

namespace tif {
inline constexpr char Day = '0', IOC = '3', GTX = '5', GTT = '6', AfterHours = 'E';
}

namespace display {
inline constexpr char Visible = 'Y', Hidden = 'N', Attributable = 'A';
}

namespace capacity {
inline constexpr char Agency = 'A', Principal = 'P', Riskless = 'R', Other = 'O';
}

namespace cross_type {
inline constexpr char Continuous = 'N', Opening = 'O', Closing = 'C', Halt = 'H',
                      Supplemental = 'S', Retail = 'R', ExtendedLife = 'E', AfterHoursClose = 'A';
}

namespace order_state {
inline constexpr char Live = 'L', Dead = 'D';
}

namespace event_code {
inline constexpr char StartOfDay = 'S', EndOfDay = 'E';
}

namespace liquidity {
inline constexpr char Added = 'A', Removed = 'R';
}

// Appendix B: Order Canceled reasons.
namespace cancel_reason {
inline constexpr char RegulatoryRestriction = 'D', Closed = 'E', PostOnlyNms = 'F',
                      PostOnlyDisplayed = 'G', Halted = 'H', ImmediateOrCancel = 'I',
                      MarketCollars = 'K', SelfMatchPrevention = 'Q', Supervisory = 'S',
                      Timeout = 'T', UserRequested = 'U', OpenProtection = 'X', System = 'Z';
}

// Appendix C: Rejected reasons (2-byte numeric). Subset used by the simulator.
namespace reject_reason {
inline constexpr std::uint16_t QuoteUnavailable = 0x0001, DestinationClosed = 0x0002,
    InvalidDisplay = 0x0003, FatFinger = 0x0006, Halted = 0x0007, InvalidSide = 0x0009,
    ProcessingError = 0x000A, CancelPending = 0x000B, FirmNotAuthorized = 0x000C,
    Other = 0x000F, InvalidQuantity = 0x0013, ReplaceNotAllowed = 0x0015,
    InvalidSymbol = 0x0017, InvalidPrice = 0x001D, SharesExceedThreshold = 0x001E,
    ExceedsMaxNotional = 0x001F, RiskMarketOrderNotAllowed = 0x002C,
    RiskMaxQuantityExceeded = 0x0031;

[[nodiscard]] constexpr std::string_view name(std::uint16_t r) noexcept {
    switch (r) {
        case QuoteUnavailable:          return "QuoteUnavailable";
        case DestinationClosed:         return "DestinationClosed";
        case InvalidDisplay:            return "InvalidDisplay";
        case FatFinger:                 return "FatFinger";
        case Halted:                    return "Halted";
        case InvalidSide:               return "InvalidSide";
        case ProcessingError:           return "ProcessingError";
        case CancelPending:             return "CancelPending";
        case FirmNotAuthorized:         return "FirmNotAuthorized";
        case Other:                     return "Other";
        case InvalidQuantity:           return "InvalidQuantity";
        case ReplaceNotAllowed:         return "ReplaceNotAllowed";
        case InvalidSymbol:             return "InvalidSymbol";
        case InvalidPrice:              return "InvalidPrice";
        case SharesExceedThreshold:     return "SharesExceedThreshold";
        case ExceedsMaxNotional:        return "ExceedsMaxNotional";
        case RiskMarketOrderNotAllowed: return "RiskMarketOrderNotAllowed";
        case RiskMaxQuantityExceeded:   return "RiskMaxQuantityExceeded";
        default:                        return "Unknown";
    }
}
}

// Appendix A: optional appendage tags (subset).
namespace tag {
inline constexpr std::uint8_t Firm = 2, MinQty = 3, CustomerType = 4, MaxFloor = 5,
    PriceType = 6, PegOffset = 7, DiscretionPrice = 9, PostOnly = 12, ExpireTime = 15,
    TradeNow = 16, HandleInst = 17, DisplayQuantity = 22, DisplayPrice = 23, GroupId = 24,
    SharesLocated = 25, LocateBroker = 26, Side = 27, UserRefIdx = 28;
}

} // namespace ouch
