#pragma once

#include "bv_keyswitch.h"

#include <cstddef>
#include <vector>

namespace tree_superrecord {

enum class ResponsePacking { Projection, MlweMerge };

const char *response_packing_name(ResponsePacking packing);

// Public offset in one response polynomial. MLWE's reverse trace order places
// group-local input i at BitRev_r(i), r=log2(rho).
size_t packed_residue_offset(size_t offset, size_t rho, ResponsePacking packing);

// Full-r trace with missing inputs treated as public zero padding. Unary nodes
// still trace until all r rounds complete.
size_t mlwe_merge_keyswitch_count(size_t input_count, size_t rho);

// Inputs are coefficient-form RLWE ciphertexts under the same key/scale/moduli.
// Every desired G=n/rho-coefficient fragment is at {0,rho,...,(G-1)rho}.
// The caller supplies keys for ALL r trace levels, even if its old per-level
// projection could use a shorter depth. No new evaluation key type is needed.
RlweCt mlwe_merge_residues(std::vector<RlweCt> inputs, size_t rho,
                           const bvks::BvGaloisKeys &keys,
                           const PirParams &scheme,
                           size_t *keyswitch_count = nullptr);

}  // namespace tree_superrecord
