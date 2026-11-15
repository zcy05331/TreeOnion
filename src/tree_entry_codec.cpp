#include "tree_entry_codec.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>

namespace tree_entry_codec {
namespace {

[[noreturn]] void fail(const std::string &message) {
  throw std::invalid_argument("tree_entry_codec: " + message);
}

void require(bool condition, const char *message) {
  if (!condition) fail(message);
}

size_t ceil_div(size_t numerator, size_t denominator) {
  if (denominator == 0) fail("division by zero");
  return numerator / denominator + (numerator % denominator != 0 ? 1 : 0);
}

size_t checked_mul(size_t a, size_t b, const char *field) {
  if (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
    fail(std::string(field) + " overflows size_t");
  }
  return a * b;
}

}

void validate_entry(const EntryBits &entry) {
  require(entry.bit_length > 0, "entry bit length must be positive");
  require(entry.bytes.size() == ceil_div(entry.bit_length, 8),
          "entry byte count does not match its bit length");
  const size_t used_in_last = entry.bit_length % 8;
  if (used_in_last != 0) {
    const uint8_t mask = static_cast<uint8_t>(0xFFu << used_in_last);
    require((entry.bytes.back() & mask) == 0,
            "entry has nonzero bits above its bit length");
  }
}

void validate_codec_spec(const CodecSpec &spec) {
  require(spec.entry_bits > 0, "codec entry_bits must be positive");
  require(spec.payload_bits_per_coeff > 0 && spec.payload_bits_per_coeff < 64,
          "codec payload bits per coefficient must be in [1, 63]");
  require(spec.record_coeffs > 0, "codec record width must be positive");
  require(spec.plaintext_modulus > 1, "codec plaintext modulus must exceed 1");
  require((uint64_t{1} << spec.payload_bits_per_coeff) <=
              spec.plaintext_modulus,
          "codec payload width exceeds the plaintext modulus");
  require(spec.plane_count == minimal_plane_count(
                                  spec.entry_bits,
                                  spec.payload_bits_per_coeff,
                                  spec.record_coeffs),
          "codec plane count is not the minimal ceil(E/(cG))");
}

EntryBits zero_entry(size_t bit_length) {
  require(bit_length > 0, "entry bit length must be positive");
  EntryBits entry;
  entry.bit_length = bit_length;
  entry.bytes.assign(ceil_div(bit_length, 8), 0);
  return entry;
}

EntryBits entry_from_bytes(std::span<const uint8_t> bytes, size_t bit_length) {
  EntryBits entry = zero_entry(bit_length);
  require(bytes.size() >= entry.bytes.size(),
          "byte source is shorter than the entry");
  std::copy(bytes.begin(), bytes.begin() + entry.bytes.size(),
            entry.bytes.begin());
  const size_t used_in_last = bit_length % 8;
  if (used_in_last != 0) {
    const uint8_t mask =
        static_cast<uint8_t>((uint16_t{1} << used_in_last) - 1);
    entry.bytes.back() &= mask;
  }
  return entry;
}

uint8_t get_bit(const EntryBits &entry, size_t bit) {
  if (bit >= entry.bit_length) {
    throw std::out_of_range("tree_entry_codec: entry bit index out of range");
  }
  return static_cast<uint8_t>((entry.bytes[bit / 8] >> (bit % 8)) & 1U);
}

void set_bit(EntryBits &entry, size_t bit, uint8_t value) {
  if (bit >= entry.bit_length) {
    throw std::out_of_range("tree_entry_codec: entry bit index out of range");
  }
  const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8));
  if (value == 0) {
    entry.bytes[bit / 8] &= static_cast<uint8_t>(~mask);
  } else {
    entry.bytes[bit / 8] |= mask;
  }
}

size_t minimal_plane_count(size_t entry_bits, size_t payload_bits_per_coeff,
                           size_t record_coeffs) {
  const size_t plane_bits =
      checked_mul(payload_bits_per_coeff, record_coeffs, "plane payload bits");
  return ceil_div(entry_bits, plane_bits);
}

LayoutChoice choose_layout(size_t entry_bits, size_t payload_bits_per_coeff,
                           size_t ring_degree) {
  require(entry_bits > 0, "entry bit length must be positive");
  require(payload_bits_per_coeff > 0, "payload width must be positive");
  require(std::has_single_bit(ring_degree),
          "ring degree must be a power of two");
  constexpr size_t kMaxPlanes = 6;

  LayoutChoice best;
  size_t best_product = std::numeric_limits<size_t>::max();
  for (size_t g = 1; g <= ring_degree / 2; g <<= 1) {
    const size_t rho = ring_degree / g;
    if (rho < 2) continue;
    const size_t planes =
        minimal_plane_count(entry_bits, payload_bits_per_coeff, g);
    if (planes == 0 || planes > kMaxPlanes) continue;
    const size_t product = checked_mul(g, planes, "layout capacity product");
    if (product < best_product ||
        (product == best_product &&
         (best.plane_count == 0 || planes < best.plane_count))) {
      best_product = product;
      best.g = g;
      best.rho = rho;
      best.plane_count = planes;
      best.payload_utilization =
          static_cast<double>(entry_bits) /
          static_cast<double>(payload_bits_per_coeff * product);
    }
  }
  require(best.g != 0, "no h=1 layout satisfies the plane bound");
  return best;
}

void encode_entry_flat(const EntryBits &entry, const CodecSpec &spec,
                       std::span<uint64_t> digits) {
  validate_entry(entry);
  require(entry.bit_length == spec.entry_bits,
          "entry bit length does not match the codec");
  require(digits.size() == spec.plane_count * spec.record_coeffs,
          "digit buffer does not match the codec");

  const size_t c = spec.payload_bits_per_coeff;
  require(c > 0 && c < 64 && digits.size() * c >= spec.entry_bits,
          "digit buffer is too small for the entry");

  // Read the entry eight bytes at a time into a bit window and cut c-bit
  // digits off its low end; a last partial digit keeps its low bits.
  const uint64_t mask = (uint64_t{1} << c) - 1;
  std::fill(digits.begin(), digits.end(), 0);
  const uint8_t *bytes = entry.bytes.data();
  const size_t byte_count = entry.bytes.size();
  unsigned __int128 window = 0;
  size_t have = 0;
  size_t digit = 0;
  for (size_t at = 0; at < byte_count; at += 8) {
    const size_t n = std::min<size_t>(8, byte_count - at);
    uint64_t word = 0;
    for (size_t j = 0; j < n; ++j) {
      word |= static_cast<uint64_t>(bytes[at + j]) << (8 * j);
    }
    window |= static_cast<unsigned __int128>(word) << have;
    have += std::min(8 * n, spec.entry_bits - 8 * at);
    while (have >= c) {
      digits[digit++] = static_cast<uint64_t>(window) & mask;
      window >>= c;
      have -= c;
    }
  }
  if (have > 0) digits[digit] = static_cast<uint64_t>(window);
}

EncodedEntry encode_entry(const EntryBits &entry, const CodecSpec &spec) {
  validate_codec_spec(spec);
  std::vector<uint64_t> digits(spec.plane_count * spec.record_coeffs);
  encode_entry_flat(entry, spec, digits);
  EncodedEntry planes(spec.plane_count);
  for (size_t plane = 0; plane < spec.plane_count; ++plane) {
    const auto first = digits.begin() + plane * spec.record_coeffs;
    planes[plane].assign(first, first + spec.record_coeffs);
  }
  return planes;
}

EntryBits decode_entry(const EncodedEntry &encoded, const CodecSpec &spec) {
  validate_codec_spec(spec);
  require(encoded.size() == spec.plane_count,
          "encoded entry has the wrong plane count");
  const size_t c = spec.payload_bits_per_coeff;
  const size_t plane_bits = c * spec.record_coeffs;
  for (const PlaneDigits &plane : encoded) {
    require(plane.size() == spec.record_coeffs,
            "encoded plane has the wrong digit count");
    for (uint64_t digit : plane) {
      require(digit < (uint64_t{1} << c), "encoded digit exceeds 2^c");
    }
  }
  EntryBits entry = zero_entry(spec.entry_bits);
  for (size_t bit = 0; bit < spec.entry_bits; ++bit) {
    const size_t plane = bit / plane_bits;
    const size_t within = bit % plane_bits;
    const uint64_t digit = encoded[plane][within / c];
    if ((digit >> (within % c)) & 1U) set_bit(entry, bit, 1);
  }
  return entry;
}

}
