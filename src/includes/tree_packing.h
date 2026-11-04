#pragma once

#include "bv_keyswitch.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace tree_packing {

enum class ResponsePacking { Projection, MlweMerge };

const char *response_packing_name(ResponsePacking packing);

std::vector<std::pair<size_t, size_t>> path_chunk_bounds(size_t item_count,
                                                         size_t capacity);

size_t packed_residue_offset(size_t offset, size_t rho,
                             ResponsePacking packing);
size_t mlwe_merge_keyswitch_count(size_t input_count, size_t rho);

RlweCt mlwe_merge_residues(std::vector<RlweCt> inputs, size_t rho,
                           const bvks::BvGaloisKeys &keys,
                           const PirParams &scheme,
                           size_t *keyswitch_count = nullptr);

}
