#include "tree_superrecord_common.h"

#include <bit>
#include <limits>

namespace tree_superrecord {

void fail(const std::string &message) {
  throw std::invalid_argument("tree_superrecord: " + message);
}

size_t checked_add(size_t a, size_t b, const char *field) {
  if (b > std::numeric_limits<size_t>::max() - a) {
    fail(std::string(field) + " overflows size_t");
  }
  return a + b;
}

size_t checked_mul(size_t a, size_t b, const char *field) {
  if (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
    fail(std::string(field) + " overflows size_t");
  }
  return a * b;
}

uint128 checked_mul_u128(uint128 a, uint128 b, const char *field) {
  if (a != 0 && b > (~static_cast<uint128>(0)) / a) {
    fail(std::string(field) + " overflows 128 bits");
  }
  return a * b;
}

size_t checked_pow2(size_t exponent, const char *field) {
  if (exponent >= static_cast<size_t>(std::numeric_limits<size_t>::digits)) {
    fail(std::string(field) + ": 2^exponent overflows size_t");
  }
  return size_t{1} << exponent;
}

size_t checked_bit_ceil(size_t value, const char *field) {
  constexpr size_t top_bit =
      size_t{1} << (std::numeric_limits<size_t>::digits - 1);
  if (value > top_bit) {
    fail(std::string(field) + ": bit_ceil overflows size_t");
  }
  return std::bit_ceil(value);
}

size_t ceil_div(size_t numerator, size_t denominator) {
  if (denominator == 0) fail("division by zero");
  return numerator / denominator + (numerator % denominator != 0 ? 1 : 0);
}

size_t round_up(size_t value, size_t alignment) {
  if (alignment == 0) fail("zero alignment");
  const size_t remainder = value % alignment;
  if (remainder == 0) return value;
  return checked_add(value, alignment - remainder, "round-up");
}

size_t exact_log2(size_t value, const char *field) {
  if (!std::has_single_bit(value)) {
    fail(std::string(field) + " must be a power of two");
  }
  return static_cast<size_t>(std::bit_width(value)) - 1;
}

size_t bit_width(size_t value) {
  return static_cast<size_t>(std::bit_width(value));
}

}  // namespace tree_superrecord
