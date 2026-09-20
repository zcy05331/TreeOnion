#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tree_entry_codec {

struct EntryBits {
  std::vector<uint8_t> bytes;  // ceil(bit_length / 8) bytes, LSB-first
  size_t bit_length = 0;

  bool operator==(const EntryBits &) const = default;
};

struct CodecSpec {
  size_t entry_bits = 0;              // E
  size_t payload_bits_per_coeff = 0;  // c
  size_t record_coeffs = 0;           // G
  size_t plane_count = 0;             // P = ceil(E / (cG))
  uint64_t plaintext_modulus = 0;     // t; every digit is < 2^c <= t
};

struct LayoutChoice {
  size_t g = 0;
  size_t rho = 0;
  size_t plane_count = 0;
  double payload_utilization = 0.0;
};

using PlaneDigits = std::vector<uint64_t>;
using EncodedEntry = std::vector<PlaneDigits>;  // [plane][digit]

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

// Encodes one h=1 entry into exactly P planes of G c-bit digits. Missing plane
// capacity is public zero padding.
EncodedEntry encode_entry(const EntryBits &entry, const CodecSpec &spec);
EntryBits decode_entry(const EncodedEntry &encoded, const CodecSpec &spec);

}  // namespace tree_entry_codec
