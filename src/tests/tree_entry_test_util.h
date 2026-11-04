#pragma once

#include "pir.h"
#include "tree_entry_codec.h"

#include <bit>
#include <cstdint>

namespace tree_entry_test {

inline size_t payload_bits(const PirParams &scheme) {
  return static_cast<size_t>(std::bit_width(scheme.get_plain_mod())) - 1;
}

inline uint64_t splitmix64(uint64_t &state) {
  uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

inline tree_entry_codec::EntryBits synthetic_entry(size_t entry_bits,
                                                   size_t level, size_t node,
                                                   uint64_t salt = 0) {
  tree_entry_codec::EntryBits entry = tree_entry_codec::zero_entry(entry_bits);
  uint64_t state = 0x53757065725265ULL ^ salt ^
                   (static_cast<uint64_t>(level) * 0xD6E8FEB86659FD93ULL) ^
                   (static_cast<uint64_t>(node) * 0xA5A3564E27F8862FULL);
  uint64_t word = 0;
  for (size_t bit = 0; bit < entry_bits; ++bit) {
    if (bit % 64 == 0) word = splitmix64(state);
    if ((word >> (bit % 64)) & 1U) {
      tree_entry_codec::set_bit(entry, bit, 1);
    }
  }
  return entry;
}

}
