#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <vector>

#include "pir.h"
#include "rlwe.h"

using QuerySeed = std::array<uint32_t, 8>;

void expand_query_mask(const QuerySeed &seed, size_t N,
                       const std::vector<uint64_t> &qs, uint64_t *c1);

size_t seeded_query_bytes(const PirParams &params);

size_t save_seeded_query(const RlweCt &query, const QuerySeed &seed,
                         const PirParams &params, std::stringstream &stream);

RlweCt load_seeded_query(std::stringstream &stream, const PirParams &params);
