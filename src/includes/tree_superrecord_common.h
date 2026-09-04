#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

// Generic super-record tree PIR: dependency-light checked arithmetic shared by
// the codec, planner, index, and database layers. Every shift, product, sum,
// and allocation size in the generic subsystem goes through these helpers so
// an infeasible shape fails with a named field instead of wrapping silently.
// This header owns no scheme or planner types (see the ownership map in the
// design specification, section 18).

namespace tree_superrecord {

using uint128 = unsigned __int128;

// Throws std::invalid_argument with the subsystem prefix.
[[noreturn]] void fail(const std::string &message);

inline void require(bool condition, const char *message) {
  if (!condition) fail(message);
}

// Checked size_t arithmetic; `field` names the quantity in the error text.
size_t checked_add(size_t a, size_t b, const char *field);
size_t checked_mul(size_t a, size_t b, const char *field);
uint128 checked_mul_u128(uint128 a, uint128 b, const char *field);
size_t checked_pow2(size_t exponent, const char *field);
size_t checked_bit_ceil(size_t value, const char *field);

// ceil(numerator / denominator); rejects a zero denominator.
size_t ceil_div(size_t numerator, size_t denominator);
// Smallest multiple of `alignment` that is >= value; rejects zero alignment
// and overflow.
size_t round_up(size_t value, size_t alignment);
// log2 of a power of two; rejects everything else.
size_t exact_log2(size_t value, const char *field);
// bit_width(value) as a size_t (0 for value 0).
size_t bit_width(size_t value);

}  // namespace tree_superrecord
