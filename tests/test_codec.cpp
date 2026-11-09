#include "ouch/messages.hpp"
#include "ouch/wire.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace ouch;

namespace {

std::vector<std::uint8_t> bytes_of(const auto& msg) {
    std::uint8_t buf[kMaxMessageSize];
    const std::size_t n = encode(msg, buf, sizeof buf);
    return {buf, buf + n};
}

std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) << 24 | static_cast<std::uint32_t>(b[at + 1]) << 16 |
           static_cast<std::uint32_t>(b[at + 2]) << 8 | b[at + 3];
}

std::uint64_t be64(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint64_t>(be32(b, at)) << 32 | be32(b, at + 4);
}

std::string str(const std::vector<std::uint8_t>& b, std::size_t at, std::size_t n) {
    return std::string(reinterpret_cast<const char*>(b.data() + at), n);
}

EnterOrder sample_enter() {
    EnterOrder m;
    m.user_ref_num = 0x01020304;
    m.side = side::Sell;
    m.quantity = 2'500;
    m.symbol = "AAPL";
    m.price = to_price(187.25);
    m.time_in_force = tif::IOC;
    m.display = display::Hidden;
    m.capacity = capacity::Principal;
    m.intermarket_sweep = 'N';
    m.cross_type = cross_type::Continuous;
    m.cl_ord_id = "ABC123";
    return m;
}

} // namespace

// Byte-for-byte check against the offsets in the OUCH 5.0 Enter Order table.
TEST(Codec, EnterOrderMatchesSpecLayout) {
    const auto b = bytes_of(sample_enter());
    ASSERT_EQ(b.size(), 47u) << "fixed part is 47 bytes incl. 2-byte appendage length";
    EXPECT_EQ(b[0], 'O');
    EXPECT_EQ(be32(b, 1), 0x01020304u);           // UserRefNum, big-endian
    EXPECT_EQ(b[5], 'S');                          // Side
    EXPECT_EQ(be32(b, 6), 2'500u);                 // Quantity
    EXPECT_EQ(str(b, 10, 8), "AAPL    ");          // Symbol: left-justified, space padded
    EXPECT_EQ(be64(b, 18), 1'872'500u);            // Price, 4 implied decimals
    EXPECT_EQ(b[26], '3');                         // TIF
    EXPECT_EQ(b[27], 'N');                         // Display
    EXPECT_EQ(b[28], 'P');                         // Capacity
    EXPECT_EQ(b[29], 'N');                         // ISO
    EXPECT_EQ(b[30], 'N');                         // CrossType
    EXPECT_EQ(str(b, 31, 14), "ABC123        ");   // ClOrdID
    EXPECT_EQ(b[45], 0);                           // Appendage length (2 bytes)
    EXPECT_EQ(b[46], 0);
}

// Fixed sizes from the spec tables (offset of Optional Appendage).
TEST(Codec, FixedMessageSizesMatchSpec) {
    EXPECT_EQ(bytes_of(ReplaceOrder{}).size(), 40u);
    EXPECT_EQ(bytes_of(CancelOrder{}).size(), 11u);
    EXPECT_EQ(bytes_of(ModifyOrder{}).size(), 12u);
    EXPECT_EQ(bytes_of(MassCancel{}).size(), 19u);
    EXPECT_EQ(bytes_of(DisableOrderEntry{}).size(), 11u);
    EXPECT_EQ(bytes_of(EnableOrderEntry{}).size(), 11u);
    EXPECT_EQ(bytes_of(AccountQuery{}).size(), 3u);

    EXPECT_EQ(bytes_of(SystemEvent{}).size(), 10u);
    EXPECT_EQ(bytes_of(OrderAccepted{}).size(), 64u);
    EXPECT_EQ(bytes_of(OrderReplaced{}).size(), 68u);
    EXPECT_EQ(bytes_of(OrderCanceled{}).size(), 20u);
    EXPECT_EQ(bytes_of(AiqCanceled{}).size(), 34u);
    EXPECT_EQ(bytes_of(OrderExecuted{}).size(), 36u);
    EXPECT_EQ(bytes_of(BrokenTrade{}).size(), 38u);
    EXPECT_EQ(bytes_of(Rejected{}).size(), 31u);
    EXPECT_EQ(bytes_of(CancelPending{}).size(), 15u);
    EXPECT_EQ(bytes_of(CancelReject{}).size(), 15u);
    EXPECT_EQ(bytes_of(OrderPriorityUpdate{}).size(), 32u);
    EXPECT_EQ(bytes_of(OrderModified{}).size(), 20u);
    EXPECT_EQ(bytes_of(OrderRestated{}).size(), 16u);
    EXPECT_EQ(bytes_of(MassCancelResponse{}).size(), 27u);
    EXPECT_EQ(bytes_of(DisableOrderEntryResponse{}).size(), 19u);
    EXPECT_EQ(bytes_of(EnableOrderEntryResponse{}).size(), 19u);
    EXPECT_EQ(bytes_of(AccountQueryResponse{}).size(), 15u);
}

TEST(Codec, OrderExecutedMatchesSpecLayout) {
    OrderExecuted m{0x1122334455667788ULL, 42, 300, to_price(10.5), liquidity::Removed, 99, {}};
    const auto b = bytes_of(m);
    EXPECT_EQ(b[0], 'E');
    EXPECT_EQ(be64(b, 1), 0x1122334455667788ULL);
    EXPECT_EQ(be32(b, 9), 42u);
    EXPECT_EQ(be32(b, 13), 300u);
    EXPECT_EQ(be64(b, 17), 105'000u);
    EXPECT_EQ(b[25], 'R');
    EXPECT_EQ(be64(b, 26), 99u);
}

TEST(Codec, RejectedReasonIsTwoByteNumeric) {
    Rejected m{1, 7, reject_reason::InvalidSymbol, "X1", {}};
    const auto b = bytes_of(m);
    EXPECT_EQ(b[13], 0x00);
    EXPECT_EQ(b[14], 0x17);
}

// ── Round trips ───────────────────────────────────────────────────────────────

template <class T>
void expect_inbound_roundtrip(const T& m) {
    const auto b = bytes_of(m);
    ASSERT_FALSE(b.empty());
    const auto d = decode_inbound(b);
    ASSERT_TRUE(d.has_value());
    ASSERT_TRUE(std::holds_alternative<T>(*d));
    EXPECT_EQ(std::get<T>(*d), m);
}

template <class T>
void expect_outbound_roundtrip(const T& m) {
    const auto b = bytes_of(m);
    ASSERT_FALSE(b.empty());
    const auto d = decode_outbound(b);
    ASSERT_TRUE(d.has_value());
    ASSERT_TRUE(std::holds_alternative<T>(*d));
    EXPECT_EQ(std::get<T>(*d), m);
}

TEST(Codec, InboundRoundTrips) {
    auto e = sample_enter();
    e.appendage.set_u32(tag::MinQty, 100);
    e.appendage.set_alpha(tag::Firm, "ABCD", 4);
    e.appendage.set_u8(tag::PostOnly, 'P');
    expect_inbound_roundtrip(e);
    expect_inbound_roundtrip(ReplaceOrder{5, 6, 700, to_price(1.23), tif::Day, display::Visible, 'N', "R1", {}});
    expect_inbound_roundtrip(CancelOrder{9, 0, {}});
    expect_inbound_roundtrip(ModifyOrder{9, side::SellShort, 50, {}});
    expect_inbound_roundtrip(MassCancel{10, "FIRM", "MSFT", {}});
    expect_inbound_roundtrip(DisableOrderEntry{11, "FIRM", {}});
    expect_inbound_roundtrip(EnableOrderEntry{12, "FIRM", {}});
    expect_inbound_roundtrip(AccountQuery{});
}

TEST(Codec, OutboundRoundTrips) {
    expect_outbound_roundtrip(SystemEvent{123, event_code::EndOfDay});
    OrderAccepted a;
    a.timestamp = 34'200'000'000'000ULL;
    a.user_ref_num = 1;
    a.quantity = 100;
    a.symbol = "AAPL";
    a.price = to_price(150);
    a.order_ref = 777;
    a.cl_ord_id = "C1";
    a.appendage.set_u64(tag::DiscretionPrice, to_price(150.05));
    expect_outbound_roundtrip(a);
    expect_outbound_roundtrip(OrderReplaced{1, 2, 3, side::Buy, 4, "MSFT", 5, tif::Day, display::Visible, 6,
                                            capacity::Agency, 'N', cross_type::Continuous, order_state::Live, "C2", {}});
    expect_outbound_roundtrip(OrderCanceled{1, 2, 3, cancel_reason::ImmediateOrCancel, {}});
    expect_outbound_roundtrip(AiqCanceled{1, 2, 3, 'Q', 4, 5, 'A', 'N', {}});
    expect_outbound_roundtrip(OrderExecuted{1, 2, 3, 4, 'A', 5, {}});
    expect_outbound_roundtrip(BrokenTrade{1, 2, 3, 'E', "C3", {}});
    expect_outbound_roundtrip(Rejected{1, 2, reject_reason::InvalidPrice, "C4", {}});
    expect_outbound_roundtrip(CancelPending{1, 2, {}});
    expect_outbound_roundtrip(CancelReject{1, 2, {}});
    expect_outbound_roundtrip(OrderPriorityUpdate{1, 2, 3, 'Y', 4, {}});
    expect_outbound_roundtrip(OrderModified{1, 2, side::SellShortExempt, 3, {}});
    expect_outbound_roundtrip(OrderRestated{1, 2, 'P', {}});
    expect_outbound_roundtrip(MassCancelResponse{1, 2, "FIRM", "", {}});
    expect_outbound_roundtrip(DisableOrderEntryResponse{1, 2, "FIRM", {}});
    expect_outbound_roundtrip(EnableOrderEntryResponse{1, 2, "FIRM", {}});
    expect_outbound_roundtrip(AccountQueryResponse{1, 2, {}});
}

// ── Robustness ────────────────────────────────────────────────────────────────

TEST(Codec, RejectsEveryTruncationAndTrailingBytes) {
    auto e = sample_enter();
    e.appendage.set_u32(tag::MinQty, 100);
    const auto b = bytes_of(e);
    for (std::size_t n = 0; n < b.size(); ++n) {
        EXPECT_FALSE(decode_inbound(std::span(b.data(), n)).has_value()) << "truncated at " << n;
    }
    auto extra = b;
    extra.push_back(0);
    EXPECT_FALSE(decode_inbound(extra).has_value()) << "trailing byte must be a framing error";
}

TEST(Codec, UnknownTypesAndDirectionsAreRejected) {
    const std::uint8_t junk[] = {'Z', 0, 0};
    EXPECT_FALSE(decode_inbound(junk).has_value());
    EXPECT_FALSE(decode_outbound(junk).has_value());
    // An inbound Enter Order is not a valid outbound message ('O' is unused outbound).
    EXPECT_FALSE(decode_outbound(bytes_of(sample_enter())).has_value());
}

TEST(Codec, AppendageOptionWireFormat) {
    CancelOrder c{1, 0, {}};
    c.appendage.set_u32(tag::MinQty, 0x0A0B0C0D);
    const auto b = bytes_of(c);
    // [type][ref:4][qty:4][applen:2] then [len=5][tag=3][value:4]
    ASSERT_EQ(b.size(), 11u + 6u);
    EXPECT_EQ(b[9], 0);
    EXPECT_EQ(b[10], 6);    // appendage length
    EXPECT_EQ(b[11], 5);    // element length covers tag + value
    EXPECT_EQ(b[12], 3);    // MinQty
    EXPECT_EQ(be32(b, 13), 0x0A0B0C0Du);
    const auto d = decode_inbound(b);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(std::get<CancelOrder>(*d).appendage.get_u32(tag::MinQty), 0x0A0B0C0Du);
}

TEST(Codec, AppendageWithOversizedUnknownOptionIsSkipped) {
    // Hand-built Cancel Order with one 12-byte option (tag 99): decodes, option dropped.
    std::vector<std::uint8_t> b = {'X', 0, 0, 0, 1, 0, 0, 0, 0, 0, 14, 13, 99};
    b.insert(b.end(), 12, 0xEE);
    const auto d = decode_inbound(b);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(std::get<CancelOrder>(*d).appendage.count, 0);
}

TEST(Codec, MalformedAppendageLengthIsRejected) {
    std::vector<std::uint8_t> b = {'X', 0, 0, 0, 1, 0, 0, 0, 0, 0, 3, 5, 3, 0};  // element claims 5, only 2 left
    EXPECT_FALSE(decode_inbound(b).has_value());
}

TEST(Codec, AlphaFieldsTrimPadding) {
    MassCancel m{1, "AB", "", {}};
    const auto b = bytes_of(m);
    EXPECT_EQ(str(b, 5, 4), "AB  ");
    EXPECT_EQ(str(b, 9, 8), "        ");
    const auto d = decode_inbound(b);
    EXPECT_EQ(std::get<MassCancel>(*d).firm.view(), "AB");
    EXPECT_EQ(std::get<MassCancel>(*d).symbol.len, 0);
}

TEST(Codec, EncodeFailsCleanlyWhenBufferTooSmall) {
    std::uint8_t small[10];
    EXPECT_EQ(encode(sample_enter(), small, sizeof small), 0u);
}
