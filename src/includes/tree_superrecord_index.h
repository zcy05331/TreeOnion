#pragma once

#include "tree_superrecord_codec.h"
#include "tree_superrecord_planner.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// Frozen plan identity, client-side index decomposition, and the public
// response placement (design specification, sections 6, 8, 14, 18).
//
// The bundled path index is sigma = floor(leaf / h). At level l with
// k = L - l the ancestor's super-record is sigma >> k, its plaintext is
// sigma >> (k + r), and its record position inside that plaintext is
// (sigma >> k) mod rho. The client encrypts only the alpha one-hot and the
// little-endian bits of sigma; every per-level target is derived by the
// server from the public LevelPlan plus those encrypted coordinates.

namespace tree_superrecord {

// Everything a database, query, or response must agree on before they may be
// combined. Compared exactly at every transition.
struct PlanTag {
  uint32_t plan_schema_version = 1;

  size_t entry_bits = 0;
  size_t leaf_count = 0;
  size_t bundle_arity = 0;
  size_t record_coeffs = 0;
  size_t plane_count = 0;
  size_t records_per_plaintext = 0;

  size_t N0 = 0;
  size_t beta_bits = 0;
  size_t selector_bits = 0;
  size_t expansion_height = 0;
  size_t expansion_capacity = 0;

  size_t required_key_height = 0;
  size_t registered_key_height = 0;

  SchemeFingerprint scheme;

  bool operator==(const PlanTag &) const = default;
};

struct TreeParams {
  CandidateShape shape;
  SchemeFingerprint scheme;
  PlanTag tag;
  std::vector<LevelPlan> level_plans;
};

PlanTag make_plan_tag(const CandidateShape &shape,
                      const SchemeFingerprint &scheme);
// Copies shape, fingerprint, and the recomputed LevelPlan vector; predicted
// or measured timings never enter runtime state.
TreeParams freeze_candidate(const Candidate &candidate);
// Recomputes plans and tag from the shape and rejects any drift.
void validate_tree_params(const TreeParams &params);

struct ClientCoordinates {
  size_t alpha = 0;
  std::vector<uint8_t> sigma_bits_le;  // selector_bits entries
};
ClientCoordinates client_index(size_t leaf, const TreeParams &params);

// Test oracle: direct per-level target coordinates. Never enters a query.
struct LevelOracle {
  size_t node_index = 0;
  size_t superrecord_index = 0;
  size_t lane_index = 0;
  size_t plaintext_index = 0;
  size_t record_position = 0;
};
LevelOracle level_oracle_for_test(size_t leaf, size_t level,
                                  const TreeParams &params);

// Pooled response placement: item = level * P + plane, chunk = item / rho,
// offset = item mod rho; plane digits then sit at offset + d * rho.
struct PlanePlacement {
  size_t response_chunk = 0;
  size_t residue_offset = 0;
};
PlanePlacement response_placement(size_t level, size_t plane,
                                  const TreeParams &params);

// The runtime scheme must have exactly the frozen identity and payload width.
void require_scheme_binding(const TreeParams &params,
                            const PirParams &runtime_scheme);

CodecSpec codec_spec_for(const TreeParams &params);

// Number of real entry slots in the bundle that holds `leaf`'s ancestor at
// `level` (min(h, 2^level - h * floor(j / h))); the worst case is h.
size_t real_bundle_slots(size_t leaf, size_t level, const TreeParams &params);

}  // namespace tree_superrecord
