#pragma once

// Big-endian field readers/writers for fixed-layout binary protocols.
//
// OUCH and SoupBinTCP put every integer on the wire big-endian, and every
// Alpha field as fixed-width ASCII, left-justified and right-padded with
// spaces. Fields are written with explicit offsets through these helpers
// rather than by casting packed structs onto buffers: no alignment UB, no
// endianness surprises, and the compiler still turns it into a few movs plus
// a bswap.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace ouch::wire {

template <class T>
[[nodiscard]] constexpr T to_be(T v) noexcept {
    if constexpr (std::endian::native == std::endian::big || sizeof(T) == 1) {
        return v;
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(__builtin_bswap16(static_cast<std::uint16_t>(v)));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(__builtin_bswap32(static_cast<std::uint32_t>(v)));
    } else {
        static_assert(sizeof(T) == 8);
        return static_cast<T>(__builtin_bswap64(static_cast<std::uint64_t>(v)));
    }
}

// Sequential writer over a caller-owned buffer. Overflow is a programming
// error (buffers are sized from the protocol maximum); ok() reports it so
// encoders can fail loudly in tests instead of writing past the end.
class Writer {
public:
    Writer(std::uint8_t* buf, std::size_t cap) noexcept : p_(buf), cap_(cap) {}

    void u8(std::uint8_t v) noexcept  { put(&v, 1); }
    void ch(char v) noexcept          { put(&v, 1); }
    void u16(std::uint16_t v) noexcept { v = to_be(v); put(&v, 2); }
    void u32(std::uint32_t v) noexcept { v = to_be(v); put(&v, 4); }
    void u64(std::uint64_t v) noexcept { v = to_be(v); put(&v, 8); }
    void i32(std::int32_t v) noexcept  { u32(static_cast<std::uint32_t>(v)); }

    // Fixed-width Alpha: left-justified, space padded, truncated if too long.
    void alpha(std::string_view s, std::size_t width) noexcept {
        if (!room(width)) return;
        const std::size_t n = s.size() < width ? s.size() : width;
        std::memcpy(p_ + len_, s.data(), n);
        std::memset(p_ + len_ + n, ' ', width - n);
        len_ += width;
    }

    // Right-justified, space padded (SoupBinTCP session / sequence fields).
    void alpha_right(std::string_view s, std::size_t width) noexcept {
        if (!room(width)) return;
        const std::size_t n = s.size() < width ? s.size() : width;
        std::memset(p_ + len_, ' ', width - n);
        std::memcpy(p_ + len_ + width - n, s.data() + (s.size() - n), n);
        len_ += width;
    }

    void bytes(const void* src, std::size_t n) noexcept { put(src, n); }

    // Reserve a 2-byte big-endian slot to patch later (length prefixes).
    [[nodiscard]] std::size_t reserve_u16() noexcept {
        const std::size_t at = len_;
        u16(0);
        return at;
    }
    void patch_u16(std::size_t at, std::uint16_t v) noexcept {
        if (at + 2 > len_) { overflow_ = true; return; }
        v = to_be(v);
        std::memcpy(p_ + at, &v, 2);
    }

    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    [[nodiscard]] bool ok() const noexcept { return !overflow_; }

private:
    bool room(std::size_t n) noexcept {
        if (len_ + n > cap_) { overflow_ = true; return false; }
        return true;
    }
    void put(const void* src, std::size_t n) noexcept {
        if (!room(n)) return;
        std::memcpy(p_ + len_, src, n);
        len_ += n;
    }

    std::uint8_t* p_;
    std::size_t   cap_;
    std::size_t   len_{0};
    bool          overflow_{false};
};

// Sequential reader. Reads past the end set ok() = false and return zeros,
// so a decoder can read every field unconditionally and check once.
class Reader {
public:
    Reader(const std::uint8_t* buf, std::size_t len) noexcept : p_(buf), len_(len) {}

    std::uint8_t  u8() noexcept  { std::uint8_t v = 0; get(&v, 1); return v; }
    char          ch() noexcept  { char v = 0; get(&v, 1); return v; }
    std::uint16_t u16() noexcept { std::uint16_t v = 0; get(&v, 2); return to_be(v); }
    std::uint32_t u32() noexcept { std::uint32_t v = 0; get(&v, 4); return to_be(v); }
    std::uint64_t u64() noexcept { std::uint64_t v = 0; get(&v, 8); return to_be(v); }
    std::int32_t  i32() noexcept { return static_cast<std::int32_t>(u32()); }

    // Alpha field with trailing (and, for right-justified fields, leading)
    // spaces removed. The view points into the input buffer.
    std::string_view alpha(std::size_t width) noexcept {
        if (pos_ + width > len_) { bad_ = true; pos_ = len_; return {}; }
        std::string_view s(reinterpret_cast<const char*>(p_ + pos_), width);
        pos_ += width;
        while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
        while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
        return s;
    }

    const std::uint8_t* bytes(std::size_t n) noexcept {
        if (pos_ + n > len_) { bad_ = true; pos_ = len_; return nullptr; }
        const std::uint8_t* at = p_ + pos_;
        pos_ += n;
        return at;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return len_ - pos_; }
    [[nodiscard]] bool ok() const noexcept { return !bad_; }

private:
    void get(void* dst, std::size_t n) noexcept {
        if (pos_ + n > len_) { bad_ = true; pos_ = len_; std::memset(dst, 0, n); return; }
        std::memcpy(dst, p_ + pos_, n);
        pos_ += n;
    }

    const std::uint8_t* p_;
    std::size_t         len_;
    std::size_t         pos_{0};
    bool                bad_{false};
};

} // namespace ouch::wire
