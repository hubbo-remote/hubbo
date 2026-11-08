#pragma once

#include <cstdint>

// Millisecond timestamps are uint32 and wrap every ~49.7 days. Compare them
// only through these helpers: they are correct across the wrap as long as
// the two times are less than ~24.8 days apart.
namespace hubbo {

// `now` is at or past `deadline`.
constexpr bool timeReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

// `a` is strictly later than `b`.
constexpr bool timeAfter(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }

}  // namespace hubbo
