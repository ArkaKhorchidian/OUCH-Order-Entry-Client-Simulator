#include "ouch/exchange.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace ouch;

namespace {

constexpr Timestamp T = 34'200'000'000'000ULL;  // 09:30

// Decode an account's journal from sequence `from` (1-based) onwards.
std::vector<OutboundMsg> journal_from(ExchangeCore& x, std::string_view acc, std::size_t from = 2) {
    std::vector<OutboundMsg> out;
    const auto& j = x.journal(acc);
    for (std::size_t i = from - 1; i < j.size(); ++i) {
        auto m = decode_outbound(j[i]);
        EXPECT_TRUE(m.has_value());
        out.push_back(*m);
    }
    return out;
}

EnterOrder order(UserRefNum ref, char s, Qty q, double px, std::string_view sym = "AAPL", char t = tif::Day) {
    EnterOrder m;
    m.user_ref_num = ref;
    m.side = s;
    m.quantity = q;
    m.symbol = sym;
    m.price = to_price(px);
    m.time_in_force = t;
    m.cl_ord_id = "C" + std::to_string(ref);
    return m;
}

template <class T>
const T& as(const OutboundMsg& m) {
    EXPECT_TRUE(std::holds_alternative<T>(m)) << to_string(m);
    return std::get<T>(m);
}

} // namespace

TEST(Exchange, JournalStartsWithStartOfDay) {
    ExchangeCore x;
    const auto j = journal_from(x, "A", 1);
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(as<SystemEvent>(j[0]).event_code, event_code::StartOfDay);
}

TEST(Exchange, AcceptsAndRests) {
    ExchangeCore x;
    x.handle("A", order(1, side::Buy, 100, 150.00), T);
    const auto j = journal_from(x, "A");
    ASSERT_EQ(j.size(), 1u);
    const auto& acc = as<OrderAccepted>(j[0]);
    EXPECT_EQ(acc.user_ref_num, 1u);
    EXPECT_EQ(acc.quantity, 100u);
    EXPECT_EQ(acc.order_state, order_state::Live);
    EXPECT_GT(acc.order_ref, 0u);
    ASSERT_EQ(x.bids("AAPL").size(), 1u);
    EXPECT_EQ(x.bids("AAPL")[0].qty, 100u);
}

TEST(Exchange, ValidationRejectsWithSpecCodes) {
    ExchangeConfig cfg;
    cfg.symbols = {"AAPL"};
    cfg.max_order_qty = 1'000;
    ExchangeCore x(cfg);
    x.handle("A", order(1, 'Q', 100, 150.0), T);
    x.handle("A", order(2, side::Buy, 0, 150.0), T);
    x.handle("A", order(3, side::Buy, 1'001, 150.0), T);
    x.handle("A", order(4, side::Buy, 100, 150.0, "ZZZZ"), T);
    x.handle("A", order(5, side::Buy, 100, 0.0), T);
    x.handle("A", order(6, side::Buy, 100, 150.0, "AAPL", tif::GTT), T);
    const auto j = journal_from(x, "A");
    ASSERT_EQ(j.size(), 6u);
    EXPECT_EQ(as<Rejected>(j[0]).reason, reject_reason::InvalidSide);
    EXPECT_EQ(as<Rejected>(j[1]).reason, reject_reason::InvalidQuantity);
    EXPECT_EQ(as<Rejected>(j[2]).reason, reject_reason::InvalidQuantity);
    EXPECT_EQ(as<Rejected>(j[3]).reason, reject_reason::InvalidSymbol);
    EXPECT_EQ(as<Rejected>(j[4]).reason, reject_reason::InvalidPrice);
    EXPECT_EQ(as<Rejected>(j[5]).reason, reject_reason::Other);
    EXPECT_EQ(as<Rejected>(j[3]).cl_ord_id.view(), "C4");
}

TEST(Exchange, UserRefNumMustIncrease) {
    ExchangeCore x;
    x.handle("A", order(5, side::Buy, 100, 150.0), T);
    x.handle("A", order(5, side::Buy, 100, 150.0), T);  // retransmission
    x.handle("A", order(3, side::Buy, 100, 150.0), T);  // lower: ignored
    x.handle("B", order(1, side::Sell, 10, 151.0), T);  // per-account counters
    EXPECT_EQ(journal_from(x, "A").size(), 1u);
    EXPECT_EQ(journal_from(x, "B").size(), 1u);
    EXPECT_EQ(x.stats().ignored_retransmissions, 2u);
}

TEST(Exchange, PriceTimePriorityAndPartialFills) {
    ExchangeCore x;
    x.handle("M1", order(1, side::Sell, 100, 10.02), T);
    x.handle("M2", order(1, side::Sell, 100, 10.01), T);  // better price
    x.handle("M3", order(1, side::Sell, 100, 10.01), T);  // same price, later
    x.handle("T", order(1, side::Buy, 250, 10.02), T);

    // Taker fills against M2 (best price, first), M3, then 50 of M1.
    const auto t = journal_from(x, "T");
    ASSERT_EQ(t.size(), 4u);
    as<OrderAccepted>(t[0]);
    EXPECT_EQ(as<OrderExecuted>(t[1]).quantity, 100u);
    EXPECT_EQ(as<OrderExecuted>(t[1]).price, to_price(10.01));
    EXPECT_EQ(as<OrderExecuted>(t[1]).liquidity_flag, liquidity::Removed);
    EXPECT_EQ(as<OrderExecuted>(t[2]).quantity, 100u);
    EXPECT_EQ(as<OrderExecuted>(t[3]).quantity, 50u);
    EXPECT_EQ(as<OrderExecuted>(t[3]).price, to_price(10.02)) << "trades at the resting price";

    const auto m2 = journal_from(x, "M2");
    ASSERT_EQ(m2.size(), 2u);
    EXPECT_EQ(as<OrderExecuted>(m2[1]).liquidity_flag, liquidity::Added);
    EXPECT_EQ(as<OrderExecuted>(m2[1]).match_number, as<OrderExecuted>(t[1]).match_number)
        << "both sides of a trade share the match number";

    ASSERT_EQ(x.asks("AAPL").size(), 1u);
    EXPECT_EQ(x.asks("AAPL")[0].qty, 50u);
    EXPECT_TRUE(x.bids("AAPL").empty()) << "taker fully filled";
}

TEST(Exchange, IocRemainderIsCanceled) {
    ExchangeCore x;
    x.handle("M", order(1, side::Sell, 60, 20.0), T);
    x.handle("T", order(1, side::Buy, 100, 20.0, "AAPL", tif::IOC), T);
    const auto t = journal_from(x, "T");
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(as<OrderExecuted>(t[1]).quantity, 60u);
    EXPECT_EQ(as<OrderCanceled>(t[2]).quantity, 40u);
    EXPECT_EQ(as<OrderCanceled>(t[2]).reason, cancel_reason::ImmediateOrCancel);
    EXPECT_TRUE(x.bids("AAPL").empty());
}

TEST(Exchange, CancelUsesNewIntendedSizeSemantics) {
    ExchangeCore x;
    x.handle("A", order(1, side::Buy, 100, 10.0), T);
    x.handle("B", order(1, side::Sell, 30, 10.0), T);  // A: 30 executed, 70 open

    // "Reduce to 50 total": 30 already executed, so 20 may remain open.
    x.handle("A", CancelOrder{1, 50, {}}, T);
    auto j = journal_from(x, "A");
    EXPECT_EQ(as<OrderCanceled>(j.back()).quantity, 50u);
    EXPECT_EQ(x.bids("AAPL")[0].qty, 20u);

    // Size below executed: everything left is canceled.
    x.handle("A", CancelOrder{1, 10, {}}, T);
    j = journal_from(x, "A");
    EXPECT_EQ(as<OrderCanceled>(j.back()).quantity, 20u);
    EXPECT_TRUE(x.bids("AAPL").empty());

    // Unknown / dead order: ignored, no message.
    const auto before = x.journal("A").size();
    x.handle("A", CancelOrder{1, 0, {}}, T);
    EXPECT_EQ(x.journal("A").size(), before);
}

TEST(Exchange, ReplaceKeepsPriorityOnlyWhenNotImproved) {
    ExchangeCore x;
    x.handle("A", order(1, side::Buy, 100, 10.0), T);
    x.handle("B", order(1, side::Buy, 100, 10.0), T);

    // A reduces size at the same price: keeps its place ahead of B.
    x.handle("A", ReplaceOrder{1, 2, 80, to_price(10.0), tif::Day, display::Visible, 'N', "A2", {}}, T);
    x.handle("S", order(1, side::Sell, 80, 10.0), T);
    EXPECT_EQ(as<OrderExecuted>(journal_from(x, "A").back()).user_ref_num, 2u) << "A filled first";

    // B increases size: loses priority behind a newer order C.
    x.handle("C", order(1, side::Buy, 100, 10.0), T);
    x.handle("B", ReplaceOrder{1, 2, 150, to_price(10.0), tif::Day, display::Visible, 'N', "B2", {}}, T);
    x.handle("S", order(2, side::Sell, 100, 10.0), T);
    EXPECT_TRUE(std::holds_alternative<OrderExecuted>(journal_from(x, "C").back()));
    EXPECT_TRUE(std::holds_alternative<OrderReplaced>(journal_from(x, "B").back()));
}

TEST(Exchange, ReplaceThatCrossesTrades) {
    ExchangeCore x;
    x.handle("M", order(1, side::Sell, 100, 10.05), T);
    x.handle("A", order(1, side::Buy, 100, 10.00), T);
    x.handle("A", ReplaceOrder{1, 2, 100, to_price(10.05), tif::Day, display::Visible, 'N', "A2", {}}, T);
    const auto j = journal_from(x, "A");
    const auto& rep = as<OrderReplaced>(j[1]);
    EXPECT_EQ(rep.orig_user_ref_num, 1u);
    EXPECT_EQ(rep.user_ref_num, 2u);
    EXPECT_EQ(as<OrderExecuted>(j[2]).user_ref_num, 2u);
    EXPECT_TRUE(x.asks("AAPL").empty());
}

TEST(Exchange, ReplaceUnknownOrderIsRejected) {
    ExchangeCore x;
    x.handle("A", ReplaceOrder{7, 8, 100, to_price(10.0), tif::Day, display::Visible, 'N', "X", {}}, T);
    EXPECT_EQ(as<Rejected>(journal_from(x, "A").back()).reason, reject_reason::ReplaceNotAllowed);
}

TEST(Exchange, ModifyReducesAndSwitchesSellType) {
    ExchangeCore x;
    x.handle("A", order(1, side::Sell, 100, 10.0), T);
    x.handle("A", ModifyOrder{1, side::SellShort, 60, {}}, T);
    const auto j = journal_from(x, "A");
    const auto& mod = as<OrderModified>(j.back());
    EXPECT_EQ(mod.side, side::SellShort);
    EXPECT_EQ(mod.quantity, 60u);
    EXPECT_EQ(x.asks("AAPL")[0].qty, 60u);

    const auto before = x.journal("A").size();
    x.handle("A", ModifyOrder{1, side::Buy, 10, {}}, T);    // sell -> buy not allowed
    x.handle("A", ModifyOrder{1, side::SellShort, 90, {}}, T);  // increase not allowed
    EXPECT_EQ(x.journal("A").size(), before);
}

TEST(Exchange, MassCancelBySymbol) {
    ExchangeCore x;
    x.handle("A", order(1, side::Buy, 100, 10.0, "AAPL"), T);
    x.handle("A", order(2, side::Buy, 100, 20.0, "MSFT"), T);
    x.handle("A", order(3, side::Sell, 100, 11.0, "AAPL"), T);
    x.handle("A", MassCancel{4, "FIRM", "AAPL", {}}, T);
    const auto j = journal_from(x, "A");
    ASSERT_EQ(j.size(), 6u);
    EXPECT_EQ(as<OrderCanceled>(j[3]).user_ref_num, 1u);
    EXPECT_EQ(as<OrderCanceled>(j[4]).user_ref_num, 3u);
    EXPECT_EQ(as<MassCancelResponse>(j[5]).symbol.view(), "AAPL");
    EXPECT_TRUE(x.bids("AAPL").empty());
    EXPECT_EQ(x.bids("MSFT").size(), 1u);
}

TEST(Exchange, DisableEnableAndAccountQuery) {
    ExchangeCore x;
    x.handle("A", DisableOrderEntry{1, "FIRM", {}}, T);
    x.handle("A", order(2, side::Buy, 100, 10.0), T);
    x.handle("A", EnableOrderEntry{3, "FIRM", {}}, T);
    x.handle("A", order(4, side::Buy, 100, 10.0), T);
    x.handle("A", AccountQuery{}, T);
    const auto j = journal_from(x, "A");
    ASSERT_EQ(j.size(), 5u);
    as<DisableOrderEntryResponse>(j[0]);
    EXPECT_EQ(as<Rejected>(j[1]).reason, reject_reason::FirmNotAuthorized);
    as<EnableOrderEntryResponse>(j[2]);
    as<OrderAccepted>(j[3]);
    EXPECT_EQ(as<AccountQueryResponse>(j[4]).next_user_ref_num, 5u);
}

TEST(Exchange, EndOfDayCancelsRestingOrders) {
    ExchangeCore x;
    x.handle("A", order(1, side::Buy, 100, 10.0), T);
    x.end_of_day(T + 1);
    const auto j = journal_from(x, "A");
    EXPECT_EQ(as<OrderCanceled>(j[1]).reason, cancel_reason::Closed);
    EXPECT_EQ(as<SystemEvent>(j[2]).event_code, event_code::EndOfDay);
    EXPECT_TRUE(x.bids("AAPL").empty());
}

TEST(Exchange, MalformedBytesAreCountedNotFatal) {
    ExchangeCore x;
    const std::uint8_t junk[] = {'O', 1, 2};
    x.handle("A", std::span<const std::uint8_t>(junk), T);
    EXPECT_EQ(x.stats().malformed, 1u);
}

// Book conservation under random flow: shares in the book always equal
// accepted shares minus executed and canceled, per order.
TEST(Exchange, RandomFlowConservesShares) {
    ExchangeCore x;
    std::uint64_t rng = 7;
    auto next = [&] { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    UserRefNum ref[3] = {0, 0, 0};
    const char* accts[3] = {"A", "B", "C"};
    for (int i = 0; i < 5'000; ++i) {
        const int a = static_cast<int>(next() % 3);
        if (next() % 4 == 0 && ref[a] > 0) {
            x.handle(accts[a], CancelOrder{1 + static_cast<UserRefNum>(next() % ref[a]), 0, {}}, T);
        } else {
            const char s = next() % 2 ? side::Buy : side::Sell;
            x.handle(accts[a], order(++ref[a], s, 1 + static_cast<Qty>(next() % 300),
                                     10.0 + static_cast<double>(next() % 20) / 100.0), T);
        }
    }
    std::uint64_t accepted = 0, executed = 0, canceled = 0;
    for (const char* acc : accts) {
        for (const auto& m : journal_from(x, acc)) {
            if (auto* p = std::get_if<OrderAccepted>(&m)) accepted += p->quantity;
            if (auto* p = std::get_if<OrderExecuted>(&m)) executed += p->quantity;
            if (auto* p = std::get_if<OrderCanceled>(&m)) canceled += p->quantity;
        }
    }
    std::uint64_t resting = 0;
    for (const auto& l : x.bids("AAPL")) resting += l.qty;
    for (const auto& l : x.asks("AAPL")) resting += l.qty;
    EXPECT_EQ(accepted, executed + canceled + resting);
    // Book never crossed.
    if (!x.bids("AAPL").empty() && !x.asks("AAPL").empty()) {
        EXPECT_LT(x.bids("AAPL").front().price, x.asks("AAPL").front().price);
    }
}
