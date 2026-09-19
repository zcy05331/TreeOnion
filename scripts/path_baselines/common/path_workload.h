#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// Mirrors the existing tree_database.cpp data source. No dependency on a PIR
// library: all external adapters can encrypt exactly the same byte database.
namespace path_workload {
inline uint64_t splitmix64_once(uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

inline std::array<uint8_t, 32> node_bytes(size_t level, size_t index) {
  std::array<uint8_t, 32> result{};
  uint64_t state = 0x747265654e6f6465ULL ^
                   (uint64_t(level) * 0xd6e8feb86659fd93ULL) ^
                   (uint64_t(index) * 0xa5a3564e27f8862fULL);
  for (size_t word = 0; word < 4; ++word) {
    state = splitmix64_once(state + word);
    for (size_t byte = 0; byte < 8; ++byte)
      result[word * 8 + byte] = static_cast<uint8_t>(state >> (byte * 8));
  }
  return result;
}

inline std::array<uint8_t, 32> record_bytes(size_t flat_index) {
  size_t level = 0;
  for (size_t x = flat_index + 1; x > 1; x >>= 1) ++level;
  return node_bytes(level, flat_index - ((size_t{1} << level) - 1));
}

inline std::vector<size_t> path_indices(size_t height, size_t leaf) {
  if (height < 1 || height > 30 || leaf >= (size_t{1} << height))
    throw std::invalid_argument("height/leaf outside supported tree");
  std::vector<size_t> result;
  result.reserve(height + 1);
  for (size_t level = 0; level <= height; ++level)
    result.push_back((size_t{1} << level) - 1 + (leaf >> (height - level)));
  return result;
}

inline std::vector<size_t> read_leaves(const std::string &path) {
  std::ifstream input(path);
  if (!input) throw std::invalid_argument("cannot open leaf file: " + path);
  std::vector<size_t> leaves;
  size_t leaf;
  while (input >> leaf) leaves.push_back(leaf);
  if (!input.eof()) throw std::invalid_argument("invalid leaf file: " + path);
  return leaves;
}
}  // namespace path_workload
