#include "ouch/messages.hpp"

#include "ouch/wire.hpp"

#include <cstdio>
#include <string>

namespace ouch {

using wire::Reader;
using wire::Writer;

// ── Appendage ─────────────────────────────────────────────────────────────────

namespace {

bool put_option(Appendage& a, std::uint8_t tag, const std::uint8_t* bytes, std::uint8_t size) noexcept {
    // Replace an existing option with the same tag, else append.
    Appendage::Option* slot = nullptr;
    for (std::uint8_t i = 0; i < a.count; ++i)
        if (a.options[i].tag == tag) slot = &a.options[i];
    if (!slot) {
        if (a.count >= Appendage::kMaxOptions || size > 8) return false;
        slot = &a.options[a.count++];
    }
    slot->tag  = tag;
    slot->size = size;
    slot->value = {};
    for (std::uint8_t i = 0; i < size; ++i) slot->value[i] = bytes[i];
    return true;
}

template <class T>
bool put_be(Appendage& a, std::uint8_t tag, T v) noexcept {
    v = wire::to_be(v);
    std::uint8_t b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    return put_option(a, tag, b, sizeof(T));
}

} // namespace

bool Appendage::set_u8(std::uint8_t t, std::uint8_t v) noexcept   { return put_option(*this, t, &v, 1); }
bool Appendage::set_u16(std::uint8_t t, std::uint16_t v) noexcept { return put_be(*this, t, v); }
bool Appendage::set_u32(std::uint8_t t, std::uint32_t v) noexcept { return put_be(*this, t, v); }
bool Appendage::set_u64(std::uint8_t t, std::uint64_t v) noexcept { return put_be(*this, t, v); }

bool Appendage::set_alpha(std::uint8_t t, std::string_view s, std::size_t width) noexcept {
    if (width > 8) return false;
    std::uint8_t b[8];
    for (std::size_t i = 0; i < width; ++i) b[i] = i < s.size() ? static_cast<std::uint8_t>(s[i]) : ' ';
    return put_option(*this, t, b, static_cast<std::uint8_t>(width));
}

const Appendage::Option* Appendage::find(std::uint8_t t) const noexcept {
    for (std::uint8_t i = 0; i < count; ++i)
        if (options[i].tag == t) return &options[i];
    return nullptr;
}

std::optional<std::uint32_t> Appendage::get_u32(std::uint8_t t) const noexcept {
    const Option* o = find(t);
    if (!o || o->size != 4) return std::nullopt;
    Reader r(o->value.data(), 4);
    return r.u32();
}

std::optional<std::uint64_t> Appendage::get_u64(std::uint8_t t) const noexcept {
    const Option* o = find(t);
    if (!o || o->size != 8) return std::nullopt;
    Reader r(o->value.data(), 8);
    return r.u64();
}

std::optional<char> Appendage::get_char(std::uint8_t t) const noexcept {
    const Option* o = find(t);
    if (!o || o->size != 1) return std::nullopt;
    return static_cast<char>(o->value[0]);
}

std::size_t Appendage::wire_size() const noexcept {
    std::size_t n = 0;
    for (std::uint8_t i = 0; i < count; ++i) n += 2u + options[i].size;  // length + tag + value
    return n;
}

bool Appendage::operator==(const Appendage& o) const noexcept {
    if (count != o.count) return false;
    for (std::uint8_t i = 0; i < count; ++i)
        if (!(options[i] == o.options[i])) return false;
    return true;
}

namespace {

void write_appendage(Writer& w, const Appendage& a) noexcept {
    w.u16(static_cast<std::uint16_t>(a.wire_size()));
    for (std::uint8_t i = 0; i < a.count; ++i) {
        const auto& o = a.options[i];
        w.u8(static_cast<std::uint8_t>(o.size + 1));  // length covers tag + value
        w.u8(o.tag);
        w.bytes(o.value.data(), o.size);
    }
}

bool read_appendage(Reader& r, Appendage& a) noexcept {
    const std::uint16_t total = r.u16();
    if (!r.ok() || total > r.remaining()) return false;
    std::size_t consumed = 0;
    while (consumed < total) {
        const std::uint8_t len = r.u8();
        if (len == 0 || consumed + 1 + len > total) return false;
        const std::uint8_t tag = r.u8();
        const std::uint8_t vsize = static_cast<std::uint8_t>(len - 1);
        const std::uint8_t* v = r.bytes(vsize);
        if (!r.ok()) return false;
        // Options larger than we model are skipped, not fatal: forward compatible.
        if (vsize <= 8) (void)put_option(a, tag, v, vsize);
        consumed += 1u + len;
    }
    return true;
}

template <class F>
std::size_t encode_with(std::uint8_t* buf, std::size_t cap, F&& body) noexcept {
    Writer w(buf, cap);
    body(w);
    return w.ok() ? w.size() : 0;
}

} // namespace

// ── Inbound encoders ──────────────────────────────────────────────────────────

std::size_t encode(const EnterOrder& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('O');
        w.u32(m.user_ref_num);
        w.ch(m.side);
        w.u32(m.quantity);
        w.alpha(m.symbol.view(), kSymbolLen);
        w.u64(m.price);
        w.ch(m.time_in_force);
        w.ch(m.display);
        w.ch(m.capacity);
        w.ch(m.intermarket_sweep);
        w.ch(m.cross_type);
        w.alpha(m.cl_ord_id.view(), kClOrdIdLen);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const ReplaceOrder& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('U');
        w.u32(m.orig_user_ref_num);
        w.u32(m.user_ref_num);
        w.u32(m.quantity);
        w.u64(m.price);
        w.ch(m.time_in_force);
        w.ch(m.display);
        w.ch(m.intermarket_sweep);
        w.alpha(m.cl_ord_id.view(), kClOrdIdLen);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const CancelOrder& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('X');
        w.u32(m.user_ref_num);
        w.u32(m.quantity);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const ModifyOrder& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('M');
        w.u32(m.user_ref_num);
        w.ch(m.side);
        w.u32(m.quantity);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const MassCancel& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('C');
        w.u32(m.user_ref_num);
        w.alpha(m.firm.view(), kFirmLen);
        w.alpha(m.symbol.view(), kSymbolLen);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const DisableOrderEntry& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('D');
        w.u32(m.user_ref_num);
        w.alpha(m.firm.view(), kFirmLen);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const EnableOrderEntry& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('E');
        w.u32(m.user_ref_num);
        w.alpha(m.firm.view(), kFirmLen);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const AccountQuery& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('Q');
        write_appendage(w, m.appendage);
    });
}

// ── Outbound encoders ─────────────────────────────────────────────────────────

std::size_t encode(const SystemEvent& m, std::uint8_t* buf, std::size_t cap) noexcept {
    // System Event carries no appendage in OUCH 5.0.
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('S');
        w.u64(m.timestamp);
        w.ch(m.event_code);
    });
}

std::size_t encode(const OrderAccepted& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('A');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        w.ch(m.side);
        w.u32(m.quantity);
        w.alpha(m.symbol.view(), kSymbolLen);
        w.u64(m.price);
        w.ch(m.time_in_force);
        w.ch(m.display);
        w.u64(m.order_ref);
        w.ch(m.capacity);
        w.ch(m.intermarket_sweep);
        w.ch(m.cross_type);
        w.ch(m.order_state);
        w.alpha(m.cl_ord_id.view(), kClOrdIdLen);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const OrderReplaced& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('U');
        w.u64(m.timestamp);
        w.u32(m.orig_user_ref_num);
        w.u32(m.user_ref_num);
        w.ch(m.side);
        w.u32(m.quantity);
        w.alpha(m.symbol.view(), kSymbolLen);
        w.u64(m.price);
        w.ch(m.time_in_force);
        w.ch(m.display);
        w.u64(m.order_ref);
        w.ch(m.capacity);
        w.ch(m.intermarket_sweep);
        w.ch(m.cross_type);
        w.ch(m.order_state);
        w.alpha(m.cl_ord_id.view(), kClOrdIdLen);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const OrderCanceled& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('C');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        w.u32(m.quantity);
        w.ch(m.reason);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const AiqCanceled& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('D');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        w.u32(m.decrement_shares);
        w.ch(m.reason);
        w.u32(m.quantity_prevented);
        w.u64(m.execution_price);
        w.ch(m.liquidity_flag);
        w.ch(m.aiq_strategy);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const OrderExecuted& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('E');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        w.u32(m.quantity);
        w.u64(m.price);
        w.ch(m.liquidity_flag);
        w.u64(m.match_number);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const BrokenTrade& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('B');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        w.u64(m.match_number);
        w.ch(m.reason);
        w.alpha(m.cl_ord_id.view(), kClOrdIdLen);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const Rejected& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('J');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        w.u16(m.reason);
        w.alpha(m.cl_ord_id.view(), kClOrdIdLen);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const CancelPending& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('P');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const CancelReject& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('I');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const OrderPriorityUpdate& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('T');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        w.u64(m.price);
        w.ch(m.display);
        w.u64(m.order_ref);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const OrderModified& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('M');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        w.ch(m.side);
        w.u32(m.quantity);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const OrderRestated& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('R');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        w.ch(m.reason);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const MassCancelResponse& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('X');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        w.alpha(m.firm.view(), kFirmLen);
        w.alpha(m.symbol.view(), kSymbolLen);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const DisableOrderEntryResponse& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('G');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        w.alpha(m.firm.view(), kFirmLen);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const EnableOrderEntryResponse& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('K');
        w.u64(m.timestamp);
        w.u32(m.user_ref_num);
        w.alpha(m.firm.view(), kFirmLen);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const AccountQueryResponse& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return encode_with(buf, cap, [&](Writer& w) {
        w.ch('Q');
        w.u64(m.timestamp);
        w.u32(m.next_user_ref_num);
        write_appendage(w, m.appendage);
    });
}

std::size_t encode(const InboundMsg& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return std::visit([&](const auto& x) { return encode(x, buf, cap); }, m);
}

std::size_t encode(const OutboundMsg& m, std::uint8_t* buf, std::size_t cap) noexcept {
    return std::visit([&](const auto& x) { return encode(x, buf, cap); }, m);
}

// ── Decoders ──────────────────────────────────────────────────────────────────

namespace {

// Every decoder reads all fields, then checks the reader once. The message
// must be consumed exactly: trailing garbage is a framing error.
template <class T>
std::optional<T> finish(Reader& r, T&& m) noexcept {
    if (!r.ok() || r.remaining() != 0) return std::nullopt;
    return std::move(m);
}

} // namespace

std::optional<InboundMsg> decode_inbound(std::span<const std::uint8_t> msg) noexcept {
    Reader r(msg.data(), msg.size());
    const char type = r.ch();
    if (!r.ok()) return std::nullopt;

    switch (type) {
        case 'O': {
            EnterOrder m;
            m.user_ref_num = r.u32();
            m.side = r.ch();
            m.quantity = r.u32();
            m.symbol = r.alpha(kSymbolLen);
            m.price = r.u64();
            m.time_in_force = r.ch();
            m.display = r.ch();
            m.capacity = r.ch();
            m.intermarket_sweep = r.ch();
            m.cross_type = r.ch();
            m.cl_ord_id = r.alpha(kClOrdIdLen);
            if (!read_appendage(r, m.appendage)) return std::nullopt;
            if (auto v = finish(r, std::move(m))) return InboundMsg{std::move(*v)};
            return std::nullopt;
        }
        case 'U': {
            ReplaceOrder m;
            m.orig_user_ref_num = r.u32();
            m.user_ref_num = r.u32();
            m.quantity = r.u32();
            m.price = r.u64();
            m.time_in_force = r.ch();
            m.display = r.ch();
            m.intermarket_sweep = r.ch();
            m.cl_ord_id = r.alpha(kClOrdIdLen);
            if (!read_appendage(r, m.appendage)) return std::nullopt;
            if (auto v = finish(r, std::move(m))) return InboundMsg{std::move(*v)};
            return std::nullopt;
        }
        case 'X': {
            CancelOrder m;
            m.user_ref_num = r.u32();
            m.quantity = r.u32();
            if (!read_appendage(r, m.appendage)) return std::nullopt;
            if (auto v = finish(r, std::move(m))) return InboundMsg{std::move(*v)};
            return std::nullopt;
        }
        case 'M': {
            ModifyOrder m;
            m.user_ref_num = r.u32();
            m.side = r.ch();
            m.quantity = r.u32();
            if (!read_appendage(r, m.appendage)) return std::nullopt;
            if (auto v = finish(r, std::move(m))) return InboundMsg{std::move(*v)};
            return std::nullopt;
        }
        case 'C': {
            MassCancel m;
            m.user_ref_num = r.u32();
            m.firm = r.alpha(kFirmLen);
            m.symbol = r.alpha(kSymbolLen);
            if (!read_appendage(r, m.appendage)) return std::nullopt;
            if (auto v = finish(r, std::move(m))) return InboundMsg{std::move(*v)};
            return std::nullopt;
        }
        case 'D': {
            DisableOrderEntry m;
            m.user_ref_num = r.u32();
            m.firm = r.alpha(kFirmLen);
            if (!read_appendage(r, m.appendage)) return std::nullopt;
            if (auto v = finish(r, std::move(m))) return InboundMsg{std::move(*v)};
            return std::nullopt;
        }
        case 'E': {
            EnableOrderEntry m;
            m.user_ref_num = r.u32();
            m.firm = r.alpha(kFirmLen);
            if (!read_appendage(r, m.appendage)) return std::nullopt;
            if (auto v = finish(r, std::move(m))) return InboundMsg{std::move(*v)};
            return std::nullopt;
        }
        case 'Q': {
            AccountQuery m;
            if (!read_appendage(r, m.appendage)) return std::nullopt;
            if (auto v = finish(r, std::move(m))) return InboundMsg{std::move(*v)};
            return std::nullopt;
        }
        default:
            return std::nullopt;
    }
}

std::optional<OutboundMsg> decode_outbound(std::span<const std::uint8_t> msg) noexcept {
    Reader r(msg.data(), msg.size());
    const char type = r.ch();
    if (!r.ok()) return std::nullopt;

#define OUCH_FINISH(m)                                                     \
    do {                                                                   \
        if (auto v = finish(r, std::move(m))) return OutboundMsg{std::move(*v)}; \
        return std::nullopt;                                               \
    } while (0)
#define OUCH_APPENDAGE(m) \
    if (!read_appendage(r, (m).appendage)) return std::nullopt

    switch (type) {
        case 'S': {
            SystemEvent m;
            m.timestamp = r.u64();
            m.event_code = r.ch();
            OUCH_FINISH(m);
        }
        case 'A': {
            OrderAccepted m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            m.side = r.ch();
            m.quantity = r.u32();
            m.symbol = r.alpha(kSymbolLen);
            m.price = r.u64();
            m.time_in_force = r.ch();
            m.display = r.ch();
            m.order_ref = r.u64();
            m.capacity = r.ch();
            m.intermarket_sweep = r.ch();
            m.cross_type = r.ch();
            m.order_state = r.ch();
            m.cl_ord_id = r.alpha(kClOrdIdLen);
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'U': {
            OrderReplaced m;
            m.timestamp = r.u64();
            m.orig_user_ref_num = r.u32();
            m.user_ref_num = r.u32();
            m.side = r.ch();
            m.quantity = r.u32();
            m.symbol = r.alpha(kSymbolLen);
            m.price = r.u64();
            m.time_in_force = r.ch();
            m.display = r.ch();
            m.order_ref = r.u64();
            m.capacity = r.ch();
            m.intermarket_sweep = r.ch();
            m.cross_type = r.ch();
            m.order_state = r.ch();
            m.cl_ord_id = r.alpha(kClOrdIdLen);
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'C': {
            OrderCanceled m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            m.quantity = r.u32();
            m.reason = r.ch();
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'D': {
            AiqCanceled m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            m.decrement_shares = r.u32();
            m.reason = r.ch();
            m.quantity_prevented = r.u32();
            m.execution_price = r.u64();
            m.liquidity_flag = r.ch();
            m.aiq_strategy = r.ch();
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'E': {
            OrderExecuted m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            m.quantity = r.u32();
            m.price = r.u64();
            m.liquidity_flag = r.ch();
            m.match_number = r.u64();
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'B': {
            BrokenTrade m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            m.match_number = r.u64();
            m.reason = r.ch();
            m.cl_ord_id = r.alpha(kClOrdIdLen);
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'J': {
            Rejected m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            m.reason = r.u16();
            m.cl_ord_id = r.alpha(kClOrdIdLen);
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'P': {
            CancelPending m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'I': {
            CancelReject m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'T': {
            OrderPriorityUpdate m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            m.price = r.u64();
            m.display = r.ch();
            m.order_ref = r.u64();
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'M': {
            OrderModified m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            m.side = r.ch();
            m.quantity = r.u32();
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'R': {
            OrderRestated m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            m.reason = r.ch();
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'X': {
            MassCancelResponse m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            m.firm = r.alpha(kFirmLen);
            m.symbol = r.alpha(kSymbolLen);
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'G': {
            DisableOrderEntryResponse m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            m.firm = r.alpha(kFirmLen);
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'K': {
            EnableOrderEntryResponse m;
            m.timestamp = r.u64();
            m.user_ref_num = r.u32();
            m.firm = r.alpha(kFirmLen);
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        case 'Q': {
            AccountQueryResponse m;
            m.timestamp = r.u64();
            m.next_user_ref_num = r.u32();
            OUCH_APPENDAGE(m);
            OUCH_FINISH(m);
        }
        default:
            return std::nullopt;
    }
#undef OUCH_FINISH
#undef OUCH_APPENDAGE
}

// ── Formatting ────────────────────────────────────────────────────────────────

namespace {

std::string px(Price p) {
    char b[32];
    std::snprintf(b, sizeof b, "%llu.%04llu", static_cast<unsigned long long>(p / kPriceScale),
                  static_cast<unsigned long long>(p % kPriceScale));
    return b;
}

template <class... Args>
std::string fmt(const char* f, Args... args) {
    char b[256];
    std::snprintf(b, sizeof b, f, args...);
    return b;
}

std::string sv(std::string_view s) { return std::string(s); }

struct InboundFmt {
    std::string operator()(const EnterOrder& m) const {
        return fmt("EnterOrder ref=%u %c %u %s @ %s tif=%c clordid=%s", m.user_ref_num, m.side, m.quantity,
                   sv(m.symbol.view()).c_str(), px(m.price).c_str(), m.time_in_force, sv(m.cl_ord_id.view()).c_str());
    }
    std::string operator()(const ReplaceOrder& m) const {
        return fmt("ReplaceOrder orig=%u ref=%u %u @ %s", m.orig_user_ref_num, m.user_ref_num, m.quantity,
                   px(m.price).c_str());
    }
    std::string operator()(const CancelOrder& m) const {
        return fmt("CancelOrder ref=%u qty=%u", m.user_ref_num, m.quantity);
    }
    std::string operator()(const ModifyOrder& m) const {
        return fmt("ModifyOrder ref=%u side=%c qty=%u", m.user_ref_num, m.side, m.quantity);
    }
    std::string operator()(const MassCancel& m) const {
        return fmt("MassCancel ref=%u firm=%s symbol=%s", m.user_ref_num, sv(m.firm.view()).c_str(),
                   sv(m.symbol.view()).c_str());
    }
    std::string operator()(const DisableOrderEntry& m) const { return fmt("DisableOrderEntry ref=%u", m.user_ref_num); }
    std::string operator()(const EnableOrderEntry& m) const { return fmt("EnableOrderEntry ref=%u", m.user_ref_num); }
    std::string operator()(const AccountQuery&) const { return "AccountQuery"; }
};

struct OutboundFmt {
    std::string operator()(const SystemEvent& m) const { return fmt("SystemEvent %c", m.event_code); }
    std::string operator()(const OrderAccepted& m) const {
        return fmt("OrderAccepted ref=%u %c %u %s @ %s orderref=%llu state=%c", m.user_ref_num, m.side,
                   m.quantity, sv(m.symbol.view()).c_str(), px(m.price).c_str(),
                   static_cast<unsigned long long>(m.order_ref), m.order_state);
    }
    std::string operator()(const OrderReplaced& m) const {
        return fmt("OrderReplaced orig=%u ref=%u %u @ %s", m.orig_user_ref_num, m.user_ref_num, m.quantity,
                   px(m.price).c_str());
    }
    std::string operator()(const OrderCanceled& m) const {
        return fmt("OrderCanceled ref=%u decrement=%u reason=%c", m.user_ref_num, m.quantity, m.reason);
    }
    std::string operator()(const AiqCanceled& m) const { return fmt("AiqCanceled ref=%u", m.user_ref_num); }
    std::string operator()(const OrderExecuted& m) const {
        return fmt("OrderExecuted ref=%u %u @ %s liq=%c match=%llu", m.user_ref_num, m.quantity,
                   px(m.price).c_str(), m.liquidity_flag, static_cast<unsigned long long>(m.match_number));
    }
    std::string operator()(const BrokenTrade& m) const { return fmt("BrokenTrade ref=%u", m.user_ref_num); }
    std::string operator()(const Rejected& m) const {
        return fmt("Rejected ref=%u reason=0x%04X (%s)", m.user_ref_num, m.reason,
                   sv(reject_reason::name(m.reason)).c_str());
    }
    std::string operator()(const CancelPending& m) const { return fmt("CancelPending ref=%u", m.user_ref_num); }
    std::string operator()(const CancelReject& m) const { return fmt("CancelReject ref=%u", m.user_ref_num); }
    std::string operator()(const OrderPriorityUpdate& m) const { return fmt("OrderPriorityUpdate ref=%u", m.user_ref_num); }
    std::string operator()(const OrderModified& m) const {
        return fmt("OrderModified ref=%u side=%c qty=%u", m.user_ref_num, m.side, m.quantity);
    }
    std::string operator()(const OrderRestated& m) const { return fmt("OrderRestated ref=%u", m.user_ref_num); }
    std::string operator()(const MassCancelResponse& m) const { return fmt("MassCancelResponse ref=%u", m.user_ref_num); }
    std::string operator()(const DisableOrderEntryResponse& m) const { return fmt("DisableOrderEntryResponse ref=%u", m.user_ref_num); }
    std::string operator()(const EnableOrderEntryResponse& m) const { return fmt("EnableOrderEntryResponse ref=%u", m.user_ref_num); }
    std::string operator()(const AccountQueryResponse& m) const {
        return fmt("AccountQueryResponse next_ref=%u", m.next_user_ref_num);
    }
};

} // namespace

std::string to_string(const InboundMsg& m) { return std::visit(InboundFmt{}, m); }
std::string to_string(const OutboundMsg& m) { return std::visit(OutboundFmt{}, m); }

} // namespace ouch
