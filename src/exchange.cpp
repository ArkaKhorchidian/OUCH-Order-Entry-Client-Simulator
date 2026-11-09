#include "ouch/exchange.hpp"

#include <algorithm>
#include <functional>

namespace ouch {

// ── Internal state ────────────────────────────────────────────────────────────

struct ExchangeCore::Order {
    OrderRef   ref{0};
    Account*   owner{nullptr};
    Book*      book{nullptr};
    UserRefNum user_ref{0};
    char       side{side::Buy};
    Qty        leaves{0};
    Qty        executed{0};
    Price      price{0};
    char       tif{tif::Day};
    char       display{display::Visible};
    char       capacity{capacity::Agency};
    char       iso{'N'};
    char       cross{cross_type::Continuous};
    Symbol     symbol;
    ClOrdId    cl_ord_id;
    bool       resting{false};
    std::list<Order*>::iterator level_pos{};
};

struct ExchangeCore::Book {
    Symbol symbol;
    // Price-time priority: map keeps levels sorted, list keeps arrival order
    // within a level and gives O(1) removal through the stored iterator.
    std::map<Price, std::list<Order*>, std::greater<>> bids;
    std::map<Price, std::list<Order*>, std::less<>>    asks;
};

struct ExchangeCore::Account {
    std::string name;
    Journal     journal;
    UserRefNum  last_user_ref{0};
    bool        disabled{false};
    std::unordered_map<UserRefNum, Order*> live;  // live orders by current UserRefNum
};

ExchangeCore::ExchangeCore(ExchangeConfig cfg) : cfg_(std::move(cfg)) {}
ExchangeCore::~ExchangeCore() = default;

template <class Msg>
void ExchangeCore::emit(Account& a, const Msg& m) {
    std::uint8_t buf[kMaxMessageSize];
    const std::size_t n = encode(m, buf, sizeof buf);
    a.journal.emplace_back(buf, buf + n);
}

ExchangeCore::Account& ExchangeCore::account(std::string_view name) {
    if (auto it = accounts_.find(name); it != accounts_.end()) return *it->second;
    auto acc = std::make_unique<Account>();
    acc->name = std::string(name);
    Account& ref = *acc;
    accounts_.emplace(std::string(name), std::move(acc));
    emit(ref, SystemEvent{0, event_code::StartOfDay});
    return ref;
}

const ExchangeCore::Journal& ExchangeCore::journal(std::string_view name) {
    return account(name).journal;
}

ExchangeCore::Book* ExchangeCore::book(std::string_view symbol, bool create) {
    if (auto it = books_.find(symbol); it != books_.end()) return it->second.get();
    if (!create) return nullptr;
    auto b = std::make_unique<Book>();
    b->symbol = symbol;
    Book* raw = b.get();
    books_.emplace(std::string(symbol), std::move(b));
    return raw;
}

// ── Dispatch ──────────────────────────────────────────────────────────────────

void ExchangeCore::handle(std::string_view acc, std::span<const std::uint8_t> bytes, Timestamp now) {
    const auto msg = decode_inbound(bytes);
    if (!msg) {
        ++stats_.malformed;
        return;
    }
    handle(acc, *msg, now);
}

void ExchangeCore::handle(std::string_view acc, const InboundMsg& msg, Timestamp now) {
    Account& a = account(acc);
    std::visit([&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, EnterOrder>) {
            on_enter(a, m, now);
        } else if constexpr (std::is_same_v<T, ReplaceOrder>) {
            on_replace(a, m, now);
        } else if constexpr (std::is_same_v<T, CancelOrder>) {
            on_cancel(a, m, now);
        } else if constexpr (std::is_same_v<T, ModifyOrder>) {
            on_modify(a, m, now);
        } else if constexpr (std::is_same_v<T, MassCancel>) {
            on_mass_cancel(a, m, now);
        } else if constexpr (std::is_same_v<T, DisableOrderEntry>) {
            a.disabled = true;
            emit(a, DisableOrderEntryResponse{now, m.user_ref_num, m.firm, {}});
        } else if constexpr (std::is_same_v<T, EnableOrderEntry>) {
            a.disabled = false;
            emit(a, EnableOrderEntryResponse{now, m.user_ref_num, m.firm, {}});
        } else if constexpr (std::is_same_v<T, AccountQuery>) {
            emit(a, AccountQueryResponse{now, a.last_user_ref + 1, {}});
        }
    }, msg);
}

// ── Enter ─────────────────────────────────────────────────────────────────────

void ExchangeCore::on_enter(Account& a, const EnterOrder& m, Timestamp now) {
    if (m.user_ref_num <= a.last_user_ref) {  // retransmission: ignore
        ++stats_.ignored_retransmissions;
        return;
    }
    a.last_user_ref = m.user_ref_num;

    auto reject = [&](std::uint16_t reason) {
        ++stats_.orders_rejected;
        emit(a, Rejected{now, m.user_ref_num, reason, m.cl_ord_id, {}});
    };

    if (a.disabled) return reject(reject_reason::FirmNotAuthorized);
    if (!side::valid(m.side)) return reject(reject_reason::InvalidSide);
    if (m.quantity == 0 || m.quantity > cfg_.max_order_qty) return reject(reject_reason::InvalidQuantity);
    if (m.symbol.len == 0 ||
        (!cfg_.symbols.empty() &&
         std::find(cfg_.symbols.begin(), cfg_.symbols.end(), m.symbol.view()) == cfg_.symbols.end()))
        return reject(reject_reason::InvalidSymbol);
    // Limit orders only: price 0 or the market-order sentinels are refused.
    if (m.price == 0 || m.price > cfg_.max_price) return reject(reject_reason::InvalidPrice);
    if (m.time_in_force != tif::Day && m.time_in_force != tif::IOC) return reject(reject_reason::Other);
    if (m.display != display::Visible && m.display != display::Hidden && m.display != display::Attributable)
        return reject(reject_reason::InvalidDisplay);

    auto o = std::make_unique<Order>();
    o->ref       = next_order_ref_++;
    o->owner     = &a;
    o->book      = book(m.symbol.view(), true);
    o->user_ref  = m.user_ref_num;
    o->side      = m.side;
    o->leaves    = m.quantity;
    o->price     = m.price;
    o->tif       = m.time_in_force;
    o->display   = m.display;
    o->capacity  = m.capacity;
    o->iso       = m.intermarket_sweep;
    o->cross     = m.cross_type;
    o->symbol    = m.symbol;
    o->cl_ord_id = m.cl_ord_id;
    Order& ord = *o;
    orders_.emplace(ord.ref, std::move(o));
    a.live.emplace(ord.user_ref, &ord);

    ++stats_.orders_accepted;
    emit(a, OrderAccepted{now, ord.user_ref, ord.side, m.quantity, ord.symbol, ord.price, ord.tif,
                          ord.display, ord.ref, ord.capacity, ord.iso, ord.cross,
                          order_state::Live, ord.cl_ord_id, {}});

    match(ord, now);
    if (ord.leaves == 0) { kill(ord); return; }
    if (ord.tif == tif::IOC) {
        cancel_shares(ord, ord.leaves, cancel_reason::ImmediateOrCancel, now);
        return;
    }
    rest(ord);
}

// ── Matching ──────────────────────────────────────────────────────────────────

void ExchangeCore::match(Order& o, Timestamp now) {
    Book& b = *o.book;
    auto run = [&](auto& levels, auto crosses) {
        while (o.leaves > 0 && !levels.empty()) {
            auto lvl = levels.begin();
            if (!crosses(lvl->first)) break;
            auto& queue = lvl->second;
            while (o.leaves > 0 && !queue.empty()) {
                Order& maker = *queue.front();
                const Qty q = std::min(o.leaves, maker.leaves);
                const MatchNum mn = next_match_++;
                // Trade at the resting order's price.
                maker.leaves -= q; maker.executed += q;
                o.leaves     -= q; o.executed     += q;
                ++stats_.executions;
                emit(*maker.owner, OrderExecuted{now, maker.user_ref, q, maker.price, liquidity::Added, mn, {}});
                emit(*o.owner,     OrderExecuted{now, o.user_ref,     q, maker.price, liquidity::Removed, mn, {}});
                if (maker.leaves == 0) {
                    queue.pop_front();
                    maker.resting = false;
                    kill(maker);
                }
            }
            if (queue.empty()) levels.erase(lvl);
        }
    };
    if (side::is_buy(o.side)) run(b.asks, [&](Price ask) { return ask <= o.price; });
    else                      run(b.bids, [&](Price bid) { return bid >= o.price; });
}

void ExchangeCore::rest(Order& o) {
    Book& b = *o.book;
    auto& queue = side::is_buy(o.side) ? b.bids[o.price] : b.asks[o.price];
    queue.push_back(&o);
    o.level_pos = std::prev(queue.end());
    o.resting = true;
}

void ExchangeCore::unrest(Order& o) {
    if (!o.resting) return;
    Book& b = *o.book;
    auto drop = [&](auto& levels) {
        auto it = levels.find(o.price);
        it->second.erase(o.level_pos);
        if (it->second.empty()) levels.erase(it);
    };
    if (side::is_buy(o.side)) drop(b.bids); else drop(b.asks);
    o.resting = false;
}

void ExchangeCore::kill(Order& o) {
    unrest(o);
    o.owner->live.erase(o.user_ref);
    orders_.erase(o.ref);  // destroys o
}

void ExchangeCore::cancel_shares(Order& o, Qty decrement, char reason, Timestamp now) {
    if (decrement == 0) return;
    o.leaves -= decrement;
    emit(*o.owner, OrderCanceled{now, o.user_ref, decrement, reason, {}});
    if (o.leaves == 0) kill(o);
}

// ── Cancel / Replace / Modify ─────────────────────────────────────────────────

void ExchangeCore::on_cancel(Account& a, const CancelOrder& m, Timestamp now) {
    auto it = a.live.find(m.user_ref_num);
    if (it == a.live.end()) { ++stats_.ignored_unknown_order; return; }
    Order& o = *it->second;
    // "New intended order size": caps total executable shares after the
    // cancel, counting what has already executed.
    const Qty allowed = m.quantity > o.executed ? m.quantity - o.executed : 0;
    const Qty new_leaves = std::min(o.leaves, allowed);
    cancel_shares(o, o.leaves - new_leaves, cancel_reason::UserRequested, now);
}

void ExchangeCore::on_replace(Account& a, const ReplaceOrder& m, Timestamp now) {
    if (m.user_ref_num <= a.last_user_ref) { ++stats_.ignored_retransmissions; return; }
    a.last_user_ref = m.user_ref_num;

    auto reject = [&](std::uint16_t reason) {
        ++stats_.orders_rejected;
        emit(a, Rejected{now, m.user_ref_num, reason, m.cl_ord_id, {}});
    };

    auto it = a.live.find(m.orig_user_ref_num);
    if (it == a.live.end()) return reject(reject_reason::ReplaceNotAllowed);
    if (a.disabled) return reject(reject_reason::FirmNotAuthorized);
    if (m.quantity == 0 || m.quantity > cfg_.max_order_qty) return reject(reject_reason::InvalidQuantity);
    if (m.price == 0 || m.price > cfg_.max_price) return reject(reject_reason::InvalidPrice);
    if (m.time_in_force != tif::Day && m.time_in_force != tif::IOC) return reject(reject_reason::Other);

    Order& o = *it->second;
    // Quantity is the new number of open shares (executed shares excluded).
    const bool keeps_priority = m.price == o.price && m.quantity <= o.leaves;
    if (!keeps_priority) unrest(o);

    a.live.erase(it);
    o.user_ref  = m.user_ref_num;
    o.leaves    = m.quantity;
    o.price     = m.price;
    o.tif       = m.time_in_force;
    o.display   = m.display;
    o.iso       = m.intermarket_sweep;
    o.cl_ord_id = m.cl_ord_id;
    a.live.emplace(o.user_ref, &o);

    emit(a, OrderReplaced{now, m.orig_user_ref_num, o.user_ref, o.side, o.leaves, o.symbol, o.price,
                          o.tif, o.display, o.ref, o.capacity, o.iso, o.cross, order_state::Live,
                          o.cl_ord_id, {}});
    if (keeps_priority) return;  // still resting at the same place, cannot cross

    match(o, now);
    if (o.leaves == 0) { kill(o); return; }
    if (o.tif == tif::IOC) { cancel_shares(o, o.leaves, cancel_reason::ImmediateOrCancel, now); return; }
    rest(o);
}

void ExchangeCore::on_modify(Account& a, const ModifyOrder& m, Timestamp now) {
    auto it = a.live.find(m.user_ref_num);
    if (it == a.live.end()) { ++stats_.ignored_unknown_order; return; }
    Order& o = *it->second;
    // Only reductions, and only between sell-side flavours (S <-> T <-> E).
    const bool side_ok = m.side == o.side || (!side::is_buy(o.side) && side::valid(m.side) && !side::is_buy(m.side));
    if (!side_ok || m.quantity > o.leaves) { ++stats_.ignored_unknown_order; return; }
    o.side = m.side;
    const Qty decrement = o.leaves - m.quantity;
    o.leaves = m.quantity;
    emit(a, OrderModified{now, o.user_ref, o.side, o.leaves, {}});
    if (o.leaves == 0) kill(o);
    (void)decrement;
}

void ExchangeCore::on_mass_cancel(Account& a, const MassCancel& m, Timestamp now) {
    std::vector<Order*> victims;
    for (auto& [ref, o] : a.live)
        if (m.symbol.len == 0 || o->symbol == m.symbol) victims.push_back(o);
    // Deterministic order for the journal: oldest UserRefNum first.
    std::sort(victims.begin(), victims.end(), [](auto* x, auto* y) { return x->user_ref < y->user_ref; });
    for (Order* o : victims) cancel_shares(*o, o->leaves, cancel_reason::UserRequested, now);
    emit(a, MassCancelResponse{now, m.user_ref_num, m.firm, m.symbol, {}});
}

void ExchangeCore::end_of_day(Timestamp now) {
    for (auto& [name, acc] : accounts_) {
        std::vector<Order*> live;
        for (auto& [ref, o] : acc->live) live.push_back(o);
        std::sort(live.begin(), live.end(), [](auto* x, auto* y) { return x->user_ref < y->user_ref; });
        for (Order* o : live) cancel_shares(*o, o->leaves, cancel_reason::Closed, now);
        emit(*acc, SystemEvent{now, event_code::EndOfDay});
    }
}

// ── Inspection ────────────────────────────────────────────────────────────────

namespace {
template <class Levels>
std::vector<ExchangeCore::Level> levels_of(const Levels& lv) {
    std::vector<ExchangeCore::Level> out;
    for (const auto& [px, q] : lv) {
        Qty total = 0;
        for (auto* o : q) total += o->leaves;
        out.push_back({px, total, q.size()});
    }
    return out;
}
} // namespace

std::vector<ExchangeCore::Level> ExchangeCore::bids(std::string_view symbol) const {
    auto it = books_.find(symbol);
    return it == books_.end() ? std::vector<Level>{} : levels_of(it->second->bids);
}

std::vector<ExchangeCore::Level> ExchangeCore::asks(std::string_view symbol) const {
    auto it = books_.find(symbol);
    return it == books_.end() ? std::vector<Level>{} : levels_of(it->second->asks);
}

} // namespace ouch
