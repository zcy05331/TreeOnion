#pragma once

#include "tree_superrecord_codec.h"

#include <algorithm>

namespace tree_superrecord_benchmark_data {

// Structured fixtures for a validated, frozen codec. Random pattern 1 stays
// with the benchmark's original deterministic database source.
inline tree_superrecord::EntryBits patterned_entry(
    const tree_superrecord::CodecSpec &spec, size_t node, size_t pattern) {
  using namespace tree_superrecord;
  EntryBits entry = zero_entry(spec.entry_bits);
  if (pattern == 0) return entry;
  if (pattern == 2 || pattern == 3) {
    const uint8_t value = pattern == 2 ? uint8_t{0xff}
        : ((node & 1U) == 0 ? uint8_t{0x55} : uint8_t{0xaa});
    std::fill(entry.bytes.begin(), entry.bytes.end(), value);
    const size_t used = spec.entry_bits % 8;
    if (used) entry.bytes.back() &= static_cast<uint8_t>((1U << used) - 1);
    return entry;
  }
  if (pattern != 4) {
    throw std::invalid_argument("unknown structured noise fixture pattern");
  }
  const size_t plane_bits = spec.payload_bits_per_coeff * spec.record_coeffs;
  const size_t offset = ((node % spec.bundle_arity) * spec.entry_bits) % plane_bits;
  const auto mark_every_plane = [&](size_t first) {
    for (size_t bit = first; bit < spec.entry_bits;) {
      set_bit(entry, bit, 1);
      if (plane_bits >= spec.entry_bits - bit) break;
      bit += plane_bits;
    }
  };
  mark_every_plane(offset == 0 ? 0 : plane_bits - offset);
  mark_every_plane(plane_bits - 1 - offset);
  return entry;
}

}  // namespace tree_superrecord_benchmark_data
