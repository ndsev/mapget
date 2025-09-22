#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <variant>
#include <utility>

#include "info.h"

namespace mapget
{

/// Small, chainable FNV-1a hasher with overloads for common types.
/// The byte order for integers is fixed to little-endian for cross-compiler stability.
struct Hash
{
    // FNV-1a 64-bit constants (public for reuse).
    constexpr static uint64_t FNV_prime     = 1099511628211ULL;
    constexpr static uint64_t offset_basis  = 14695981039346656037ULL;

    /// Current hash value (starts at offset basis).
    uint64_t hash_ = offset_basis;

    /// Final value.
    [[nodiscard]] uint64_t value() const noexcept { return hash_; }

    /// Reset to the offset basis.
    Hash& reset() noexcept { hash_ = offset_basis; return *this; }

    /// Combine a single byte.
    constexpr Hash& byte(uint8_t b) noexcept
    {
        hash_ ^= static_cast<uint64_t>(b);
        hash_ *= FNV_prime;
        return *this;
    }

    /// Mix raw bytes (stable order).
    Hash& mix(const void* data, size_t len) noexcept
    {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) byte(p[i]);
        return *this;
    }

    /// Strings / string_view: hash bytes as-is.
    Hash& mix(std::string_view const& sv) noexcept
    {
        return mix(sv.data(), sv.size());
    }

    /// Bool as one byte
    Hash& mix(bool v) noexcept
    {
        return byte(static_cast<uint8_t>(v ? 1 : 0));
    }

    /// Unsigned integrals: force little-endian byte order for stability.
    template <class T,
              std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, int> = 0>
    Hash& mix(T v) noexcept
    {
        for (size_t i = 0; i < sizeof(T); ++i)
        {
            byte(static_cast<uint8_t>(v & 0xffu));
            v >>= 8;
        }
        return *this;
    }

    /// Signed integrals: two's-complement bit pattern via make_unsigned.
    template <class T,
              std::enable_if_t<std::is_integral_v<T> && std::is_signed_v<T>, int> = 0>
    Hash& mix(T v) noexcept
    {
        using U = std::make_unsigned_t<T>;
        return mix(static_cast<U>(v));
    }

    /// Float: hash exact bit pattern (IEEE 754) via bit-cast.
    Hash& mix(float f) noexcept
    {
        static_assert(sizeof(float) == 4, "Unexpected float size");
        return mix(std::bit_cast<uint32_t>(f));
    }

    /// Double: hash exact bit pattern (IEEE 754) via bit-cast.
    Hash& mix(double const& d) noexcept
    {
        static_assert(sizeof(double) == 8, "Unexpected double size");
        return mix(std::bit_cast<uint64_t>(d));
    }

    /// Explicit 64-bit overload (often handy).
    Hash& mix(uint64_t const& v) noexcept
    {
        return mix<uint64_t>(v);
    }

    /// Convenience for C-strings.
    Hash& mix(const char* v) noexcept
    {
        return mix(std::string_view{v ? v : ""});
    }

    /// Convenience for KeyValueViewPairs
    Hash& mix(KeyValueViewPairs const& kvPairs)
    {
        for (const auto& kv : kvPairs) {
            mix(kv.first);
            std::visit([this](const auto& v){mix(v);}, kv.second);
        }
        return *this;
    }
};

} // namespace mapget
