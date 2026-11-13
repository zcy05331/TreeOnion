#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tree_entry_codec {

struct EntryBits {
  std::vector<uint8_t> bytes;
  size_t bit_length = 0;

  bool operator==(const EntryBits &) const = default;
};

struct CodecSpec {
  size_t entry_bits = 0;
  size_t payload_bits_per_coeff = 0;
  size_t record_coeffs = 0;
  size_t plane_count = 0;
  uint64_t plaintext_modulus = 0;
};

struct LayoutChoice {
  size_t g = 0;
  size_t rho = 0;
  size_t plane_count = 0;
  double payload_utilization = 0.0;
};

using PlaneDigits = std::vector<uint64_t>;
using EncodedEntry = std::vector<PlaneDigits>;

void validate_entry(const EntryBits &entry);
void validate_codec_spec(const CodecSpec &spec);

EntryBits zero_entry(size_t bit_length);
EntryBits entry_from_bytes(std::span<const uint8_t> bytes, size_t bit_length);
uint8_t get_bit(const EntryBits &entry, size_t bit);
void set_bit(EntryBits &entry, size_t bit, uint8_t value);

size_t minimal_plane_count(size_t entry_bits, size_t payload_bits_per_coeff,
                           size_t record_coeffs);
LayoutChoice choose_layout(size_t entry_bits, size_t payload_bits_per_coeff,
                           size_t ring_degree);

EncodedEntry encode_entry(const EntryBits &entry, const CodecSpec &spec);
// encode_entry without allocation: digits[plane * record_coeffs + digit]. The
// caller has validated the spec.
void encode_entry_flat(const EntryBits &entry, const CodecSpec &spec,
                       std::span<uint64_t> digits);
EntryBits decode_entry(const EncodedEntry &encoded, const CodecSpec &spec);

}
