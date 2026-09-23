#pragma once

#include "pir.h"
#include "rlwe.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using MerkleNode = std::array<uint8_t, 32>;

// Levels use root=0 and leaves=tree_height. An authentication path contains
// one sibling from every level H..1; the root is absent from both baselines.
struct MerkleWorkload {
  size_t leaf_count;
  size_t tree_height;
  size_t node_bytes;
  // 0 keeps make_benchmark_reference's default rule (expansion height 10 from
  // 2^24 leaves, the smallest feasible height below). A nonzero value pins
  // the reference expansion height, so small databases can be measured with
  // the same first-dimension geometry as the 1 GiB row.
  size_t reference_expansion_height = 0;
};

// Minimal per-level database shape. params changes layout only and remains
// scheme-compatible with the reference so all levels can share helper keys.
struct LayerLayout {
  size_t level;
  size_t node_count;
  size_t target_num_pt;
  PirParams params;
  // A level whose node_count fits one plaintext is handed to the client in
  // the clear instead of being served by PIR: sending the whole level reveals
  // nothing about which node is wanted, and it costs node_count * 32 bytes
  // instead of a query plus a ciphertext response.
  bool direct_return = false;
};

struct TargetPlaintextLayout {
  size_t target_num_pt = 0;
  PirParams params;
};

// Return a level-local sibling index, not a global heap index.
size_t merkle_sibling_local(size_t leaf, size_t tree_height, size_t level);
// Breadth-first ordinal in the root-excluded flat database; ordinal 0 is the
// left node at level 1.
size_t merkle_flat_ordinal(size_t level, size_t local_index);

// Deterministic benchmark oracle, not a cryptographic Merkle hash. Stable
// coordinate-derived bytes avoid retaining a second in-memory tree for tests.
MerkleNode synthetic_merkle_node(size_t level, size_t local_index);
// Pack exactly 96 32-byte nodes into the v2 2048 x 12-bit coefficient payload;
// unused node slots are zero padded.
RlwePt encode_merkle_nodes(std::span<const MerkleNode> nodes,
                           const PirParams &params);
MerkleNode decode_merkle_node(const RlwePt &pt, size_t node_offset,
                              const PirParams &params);

RlwePt make_flat_merkle_plaintext(size_t plaintext_index,
                                  const MerkleWorkload &workload,
                                  const PirParams &params);
RlwePt make_layer_merkle_plaintext(size_t level, size_t plaintext_index,
                                   const MerkleWorkload &workload,
                                   const PirParams &params);

// Strided level layout for the merged-response layerwise variant. A plaintext
// holds rho = 64 node slots; slot j keeps digit i of its node at coefficient
// i * 64 + j (22 twelve-bit digits per 32-byte node, 10 zero digits of slack),
// so multiplying the level result by X^{-j} moves the wanted node onto the
// stride grid {i * 64} and the MLWE residue merge can pack up to 64 level
// results into one response ciphertext. The slot of a level-local node index
// is its top six bits; for every served level those are the same six leading
// leaf bits, so one gamma selector set rotates every level. The price is
// 64 instead of 96 nodes per plaintext (two thirds coefficient utilization).
constexpr size_t kStridedNodesPerPlaintext = 64;
constexpr size_t kStridedLevelSlotBits = 6;   // log2(64)
constexpr size_t kStridedDigitsPerNode = 22;  // ceil(256 / 12)
RlwePt encode_merkle_nodes_strided(std::span<const MerkleNode> nodes,
                                   const PirParams &params);
MerkleNode decode_merkle_node_strided(const RlwePt &pt, size_t slot,
                                      const PirParams &params);
// level <= 6 has a single plaintext (shift 0); above that the level's 2^level
// nodes split into 2^(level - 6) plaintexts of 64 slots.
size_t strided_level_shift(size_t level);
size_t strided_level_plaintext(size_t level, size_t local);
size_t strided_level_slot(size_t level, size_t local);
RlwePt make_layer_merkle_plaintext_strided(size_t level, size_t plaintext_index,
                                           const MerkleWorkload &workload,
                                           const PirParams &params);

// Query packing of the packed layerwise variant: segment i takes sizes[i]
// contiguous slots of a shared expansion; segments are placed in order and a
// new ciphertext starts whenever the next segment would cross `capacity`.
struct QuerySegmentPlacement {
  size_t ciphertext = 0;
  size_t offset = 0;
};
std::vector<QuerySegmentPlacement> pack_query_segments(
    const std::vector<size_t> &sizes, size_t capacity);

// Minimize padded plaintext count per level while retaining the reference
// scheme parameters and maximum remaining-dimension count.
TargetPlaintextLayout select_target_plaintext_layout(
    size_t target_num_pt, const PirParams &reference);
std::vector<LayerLayout> plan_layer_layouts(
    size_t tree_height, size_t nodes_per_pt, const PirParams &reference);
uint64_t sum_padded_bytes(const std::vector<LayerLayout> &layouts);
