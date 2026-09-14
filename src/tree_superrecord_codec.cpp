#include "tree_superrecord_codec.h"

#include "tree_superrecord_common.h"

#include <algorithm>

namespace tree_superrecord {

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
  require(spec.bundle_arity > 0, "codec bundle arity must be positive");
  require(spec.record_coeffs > 0, "codec record width must be positive");
  require(spec.plaintext_modulus > 1, "codec plaintext modulus must exceed 1");
  require((uint64_t{1} << spec.payload_bits_per_coeff) <= spec.plaintext_modulus,
          "codec payload width exceeds the plaintext modulus");
  require(spec.plane_count ==
              minimal_plane_count(spec.entry_bits, spec.payload_bits_per_coeff,
                                  spec.bundle_arity, spec.record_coeffs),
          "codec plane count is not the minimal ceil(hE/(cG))");
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
    throw std::out_of_range("tree_superrecord: entry bit index out of range");
  }
  return static_cast<uint8_t>((entry.bytes[bit / 8] >> (bit % 8)) & 1U);
}

void set_bit(EntryBits &entry, size_t bit, uint8_t value) {
  if (bit >= entry.bit_length) {
    throw std::out_of_range("tree_superrecord: entry bit index out of range");
  }
  const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8));
  if (value == 0) {
    entry.bytes[bit / 8] &= static_cast<uint8_t>(~mask);
  } else {
    entry.bytes[bit / 8] |= mask;
  }
}

size_t minimal_plane_count(size_t entry_bits, size_t payload_bits_per_coeff,
                           size_t bundle_arity, size_t record_coeffs) {
  const size_t bundle_bits =
      checked_mul(bundle_arity, entry_bits, "bundle payload bits");
  const size_t plane_bits =
      checked_mul(payload_bits_per_coeff, record_coeffs, "plane payload bits");
  return ceil_div(bundle_bits, plane_bits);
}

EncodedSuperRecord encode_super_record(std::span<const EntryBits> entries,
                                       const CodecSpec &spec) {
  validate_codec_spec(spec);
  require(entries.size() <= spec.bundle_arity,
          "more entries than the bundle arity");
  for (const EntryBits &entry : entries) {
    validate_entry(entry);
    require(entry.bit_length == spec.entry_bits,
            "entry bit length does not match the codec");
  }
  const size_t c = spec.payload_bits_per_coeff;
  EncodedSuperRecord planes(spec.plane_count,
                            PlaneDigits(spec.record_coeffs, 0));
  // Consecutive byte fragments from the concatenated bundle are streamed into
  // c-bit digits. Missing tail lanes and plane padding remain zero.
  size_t plane = 0;
  size_t digit = 0;
  size_t digit_offset = 0;
  for (size_t lane = 0; lane < entries.size(); ++lane) {
    const EntryBits &entry = entries[lane];
    size_t remaining_entry_bits = spec.entry_bits;
    for (uint8_t byte : entry.bytes) {
      size_t fragment_bits = std::min<size_t>(8, remaining_entry_bits);
      size_t consumed = 0;
      while (fragment_bits > 0) {
        const size_t take =
            std::min(fragment_bits, c - digit_offset);
        const uint64_t mask = (uint64_t{1} << take) - 1;
        planes[plane][digit] |=
            static_cast<uint64_t>((byte >> consumed) & mask) << digit_offset;
        fragment_bits -= take;
        consumed += take;
        digit_offset += take;
        if (digit_offset == c) {
          digit_offset = 0;
          ++digit;
          if (digit == spec.record_coeffs) {
            digit = 0;
            ++plane;
          }
        }
      }
      remaining_entry_bits -= std::min<size_t>(8, remaining_entry_bits);
    }
  }
  return planes;
}

std::vector<EntryBits> decode_super_record(const EncodedSuperRecord &encoded,
                                           const CodecSpec &spec) {
  validate_codec_spec(spec);
  require(encoded.size() == spec.plane_count,
          "encoded super-record has the wrong plane count");
  const size_t c = spec.payload_bits_per_coeff;
  const size_t plane_bits = c * spec.record_coeffs;
  for (const PlaneDigits &plane : encoded) {
    require(plane.size() == spec.record_coeffs,
            "encoded plane has the wrong digit count");
    for (uint64_t digit : plane) {
      require(digit < (uint64_t{1} << c), "encoded digit exceeds 2^c");
    }
  }
  std::vector<EntryBits> entries;
  entries.reserve(spec.bundle_arity);
  for (size_t lane = 0; lane < spec.bundle_arity; ++lane) {
    EntryBits entry = zero_entry(spec.entry_bits);
    const size_t base = lane * spec.entry_bits;
    for (size_t bit = 0; bit < spec.entry_bits; ++bit) {
      const size_t pos = base + bit;
      const size_t plane = pos / plane_bits;
      const size_t within = pos % plane_bits;
      const uint64_t digit = encoded[plane][within / c];
      if ((digit >> (within % c)) & 1U) set_bit(entry, bit, 1);
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

}  // namespace tree_superrecord
