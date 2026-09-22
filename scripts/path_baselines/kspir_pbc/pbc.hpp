// Standalone copy of the OnionPIR cuckoo baseline's PBC instance
// (src/cuckoo_batch.cpp, the Merkle helpers in the merkle sources and
// make_benchmark_trial_plan), so the KsPIR backend sees the same records,
// hashes, buckets, bucket positions and seed-7 leaves as the OnionPIR cuckoo
// rows. Kept bit-identical to scripts/path_baselines/spiral_pbc/src/pbc.rs.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pbc {

inline uint64_t splitmix64_once(uint64_t v) {
  v += 0x9e3779b97f4a7c15ULL;
  v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
  v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
  return v ^ (v >> 31);
}

inline uint64_t splitmix64_next(uint64_t &state) {
  state += 0x9e3779b97f4a7c15ULL;
  uint64_t v = state;
  v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
  v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
  return v ^ (v >> 31);
}

constexpr uint64_t kBenchmarkHashSeed = 0x63756b32ULL;

struct Params {
  size_t item_count = 0;
  size_t batch_size = 0;
  size_t num_buckets = 0;
  size_t num_hashes = 3;
  uint64_t seed = kBenchmarkHashSeed;
};

// make_cuckoo_batch_params followed by the benchmark's ++num_buckets.
inline Params benchmark_params(size_t item_count, size_t batch_size) {
  if (item_count == 0 || batch_size == 0) throw std::invalid_argument("empty batch or database");
  Params p;
  p.item_count = item_count;
  p.batch_size = batch_size;
  p.num_buckets = std::max((3 * batch_size + 1) / 2, batch_size + 1) + 1;
  return p;
}

inline size_t bucket_of(uint64_t ordinal, size_t hash_index, const Params &p) {
  return static_cast<size_t>(
      splitmix64_once(p.seed ^ (0x9e3779b97f4a7c15ULL * (hash_index + 1)) ^
                      (ordinal * 0xd6e8feb86659fd93ULL)) %
      p.num_buckets);
}

inline std::vector<std::vector<uint32_t>> build_buckets(const Params &p) {
  std::vector<std::vector<uint32_t>> buckets(p.num_buckets);
  const size_t expected = p.item_count * p.num_hashes / p.num_buckets + 1;
  for (auto &b : buckets) b.reserve(expected + expected / 16);
  for (uint64_t ordinal = 0; ordinal < p.item_count; ++ordinal) {
    for (size_t j = 0; j < p.num_hashes; ++j) {
      buckets[bucket_of(ordinal, j, p)].push_back(static_cast<uint32_t>(ordinal));
    }
  }
  for (auto &b : buckets) {
    std::sort(b.begin(), b.end());
    b.erase(std::unique(b.begin(), b.end()), b.end());
    if (b.empty()) throw std::runtime_error("every bucket must hold at least one item");
  }
  return buckets;
}

inline size_t position_in_bucket(const std::vector<uint32_t> &bucket, uint32_t ordinal) {
  const auto it = std::lower_bound(bucket.begin(), bucket.end(), ordinal);
  if (it == bucket.end() || *it != ordinal) throw std::invalid_argument("ordinal does not hash into this bucket");
  return static_cast<size_t>(it - bucket.begin());
}

inline std::vector<std::optional<uint32_t>> cuckoo_place(const std::vector<uint32_t> &ordinals, const Params &p) {
  if (ordinals.size() != p.batch_size) throw std::invalid_argument("placement expects exactly the batch size");
  std::vector<std::optional<uint32_t>> assignment(p.num_buckets);
  uint64_t walk_state = p.seed ^ 0x66616365ULL;
  for (uint32_t start : ordinals) {
    uint32_t current = start;
    bool placed = false;
    for (size_t step = 0; step < 10000 && !placed; ++step) {
      size_t free_bucket = p.num_buckets;
      for (size_t j = 0; j < p.num_hashes; ++j) {
        const size_t b = bucket_of(current, j, p);
        if (assignment[b].has_value() && *assignment[b] == current) throw std::invalid_argument("batch ordinals must be distinct");
        if (!assignment[b].has_value()) { free_bucket = b; break; }
      }
      if (free_bucket != p.num_buckets) { assignment[free_bucket] = current; placed = true; break; }
      walk_state = splitmix64_once(walk_state);
      const size_t j = static_cast<size_t>(walk_state % p.num_hashes);
      const size_t b = bucket_of(current, j, p);
      const uint32_t evicted = *assignment[b];
      assignment[b] = current;
      current = evicted;
    }
    if (!placed) throw std::runtime_error("eviction walk failed");
  }
  return assignment;
}

inline size_t item_count(size_t tree_height) { return 2 * ((size_t{1} << tree_height) - 1); }
inline size_t flat_ordinal(size_t level, size_t local) { return (size_t{1} << level) - 2 + local; }
inline std::pair<size_t, size_t> node_of_ordinal(size_t ordinal) {
  const size_t heap = ordinal + 2;
  const size_t level = 63 - static_cast<size_t>(__builtin_clzll(heap));
  return {level, heap - (size_t{1} << level)};
}
inline size_t sibling_local(size_t leaf, size_t h, size_t level) { return (leaf >> (h - level)) ^ size_t{1}; }

inline std::vector<uint32_t> sibling_ordinals(size_t leaf, size_t h) {
  std::vector<uint32_t> out;
  for (size_t level = h; level >= 1; --level) {
    out.push_back(static_cast<uint32_t>(flat_ordinal(level, sibling_local(leaf, h, level))));
    if (level == 1) break;
  }
  return out;
}

// synthetic_merkle_node: deterministic 32-byte oracle record.
inline std::array<uint8_t, 32> merkle_node(size_t level, size_t local) {
  std::array<uint8_t, 32> node{};
  uint64_t state = 0x6d65726b6c655f32ULL ^ (static_cast<uint64_t>(level) * 0xd6e8feb86659fd93ULL) ^
                   (static_cast<uint64_t>(local) * 0xa5a3564e27f8862fULL);
  for (size_t word = 0; word < 4; ++word) {
    state = splitmix64_once(state + word);
    for (size_t byte = 0; byte < 8; ++byte) node[word * 8 + byte] = static_cast<uint8_t>(state >> (byte * 8));
  }
  return node;
}

// make_benchmark_trial_plan: Floyd sampling without replacement.
inline std::pair<std::vector<size_t>, std::vector<size_t>> trial_plan(size_t leaf_count, size_t warmups,
                                                                       size_t measured, uint64_t seed) {
  const size_t query_count = warmups + measured;
  if (measured == 0 || query_count > leaf_count) throw std::invalid_argument("invalid trial plan");
  std::vector<size_t> selected;
  uint64_t state = seed;
  for (size_t offset = 0; offset < query_count; ++offset) {
    const size_t upper = leaf_count - query_count + offset;
    const size_t candidate = static_cast<size_t>(splitmix64_next(state) % (upper + 1));
    selected.push_back(std::find(selected.begin(), selected.end(), candidate) == selected.end() ? candidate : upper);
  }
  return {std::vector<size_t>(selected.begin(), selected.begin() + warmups),
          std::vector<size_t>(selected.begin() + warmups, selected.end())};
}

}  // namespace pbc
