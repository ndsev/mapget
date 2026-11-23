#pragma once

#include "simfil/model/schema-type.h"

#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace mapget::point_schema
{
// A browser's long double can be just double. Compare 64-bit raw codes without
// rounding away adjacent integer bounds above 2^53, including mixed signedness.
using Bound = simfil::SchemaType::NumericValue;
template <class I>
int compareReal(I integer, double real)
{
    constexpr auto digits = std::numeric_limits<I>::digits;
    auto limit = std::ldexp(1., digits);
    if (real >= limit)
        return -1;
    if (real < (std::is_signed_v<I> ? -limit : 0.))
        return 1;
    auto truncated = static_cast<I>(real);
    if (integer != truncated)
        return integer < truncated ? -1 : 1;
    return (std::trunc(real) > real) - (std::trunc(real) < real);
}

inline bool less(Bound const& a, Bound const& b)
{
    return std::visit(
        [](auto x, auto y)
        {
            if constexpr (std::is_integral_v<decltype(x)> && std::is_integral_v<decltype(y)>)
                return std::cmp_less(x, y);
            else if constexpr (std::is_integral_v<decltype(x)>)
                return compareReal(x, y) < 0;
            else if constexpr (std::is_integral_v<decltype(y)>)
                return compareReal(y, x) > 0;
            else
                return x < y;
        },
        a,
        b);
}

}  // namespace mapget::point_schema
