#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Generic arbitrary-bit multi-plane codec (design specification, section 7).
//
// One super-record concatenates h entries of E bits each (lane order 0..h-1,
// little-endian bit order inside an entry), zero-pads the h*E-bit string to
// P*c*G bits, and splits it into P consecutive planes of c*G bits. Each plane
// becomes G little-endian c-bit digits, one per plaintext coefficient of the
// record's stride lattice. P = ceil(h*E / (c*G)) is minimal by definition and
// is recomputed by every validator; native integer reinterpretation is never
// used, so the byte encoding is identical on every platform.

namespace tree_superrecord {

struct EntryBits {
  std::vector<uint8_t> bytes;  // ceil(bit_length / 8) bytes, LSB-first
  size_t bit_length = 0;

  bool operator==(const EntryBits &) const = default;
};

struct CodecSpec {
  size_t entry_bits = 0;              // E
  size_t payload_bits_per_coeff = 0;  // c
  size_t bundle_arity = 0;            // h
  size_t record_coeffs = 0;           // G
  size_t plane_count = 0;             // P = ceil(h*E / (c*G))
  uint64_t plaintext_modulus = 0;     // t; every digit is < 2^c <= t
};

using PlaneDigits = std::vector<uint64_t>;
using EncodedSuperRecord = std::vector<PlaneDigits>;  // [plane][digit]

// Rejects a zero bit length, a byte vector of the wrong size, and nonzero
// padding bits above bit_length in the last byte.
void validate_entry(const EntryBits &entry);
// Rejects zero fields, a nonminimal plane count, and 2^c > t.
void validate_codec_spec(const CodecSpec &spec);

EntryBits zero_entry(size_t bit_length);
EntryBits entry_from_bytes(std::span<const uint8_t> bytes, size_t bit_length);
uint8_t get_bit(const EntryBits &entry, size_t bit);
void set_bit(EntryBits &entry, size_t bit, uint8_t value);

// P = ceil(h*E / (c*G)); the only place that formula is written.
size_t minimal_plane_count(size_t entry_bits, size_t payload_bits_per_coeff,
                           size_t bundle_arity, size_t record_coeffs);

// `entries` holds at most h validated entries of E bits; missing tail lanes
// are zero. Returns exactly P planes of G digits.
EncodedSuperRecord encode_super_record(std::span<const EntryBits> entries,
                                       const CodecSpec &spec);
// Inverse of encode: exactly h entries of E bits.
std::vector<EntryBits> decode_super_record(const EncodedSuperRecord &encoded,
                                           const CodecSpec &spec);

}  // namespace tree_superrecord
