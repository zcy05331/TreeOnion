#pragma once

#include "tree_superrecord_planner.h"

namespace tree_superrecord::first_dimension_internal {

inline std::vector<uint64_t> limb_moduli(const PirSchemeIdentity &identity) {
  if (identity.composite_first_dim) {
    return {identity.composite_q1, identity.composite_q2};
  }
  return identity.rns_moduli;
}

inline size_t limb_bytes(const PirSchemeIdentity &identity) {
  return identity.composite_first_dim ? sizeof(uint32_t) : sizeof(uint64_t);
}

inline FirstDimShapeKey shape_key(const LevelPlan &plan,
                                  const PirSchemeIdentity &identity,
                                  uint64_t limb_modulus,
                                  const ParetoContext &context) {
  FirstDimShapeKey key;
  key.matrix_rows = plan.matrix_rows;
  key.matrix_cols = plan.matrix_cols;
  key.cols_mod_simd_width = plan.matrix_cols % 16;
  key.limb_modulus = limb_modulus;
  if (identity.composite_first_dim) {
    key.dispatch =
        select_matmul32_dispatch(plan.matrix_cols, limb_modulus, context.avx512);
  }
  return key;
}

inline std::vector<FirstDimShapeKey> shape_keys(
    const Candidate &candidate, const ParetoContext &context) {
  std::vector<FirstDimShapeKey> keys;
  const std::vector<LevelPlan> plans = build_level_plans(candidate.shape);
  const std::vector<uint64_t> moduli = limb_moduli(candidate.scheme.identity);
  keys.reserve(candidate.shape.plane_count * plans.size() * moduli.size());
  for (const LevelPlan &plan : plans) {
    for (size_t plane = 0; plane < candidate.shape.plane_count; ++plane) {
      (void)plane;
      for (uint64_t modulus : moduli) {
        keys.push_back(shape_key(plan, candidate.scheme.identity, modulus,
                                 context));
      }
    }
  }
  return keys;
}

}  // namespace tree_superrecord::first_dimension_internal
