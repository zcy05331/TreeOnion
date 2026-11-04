#pragma once

#include "client.h"
#include "pir.h"
#include "server.h"
#include "tree_index.h"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <span>
#include <vector>

TreePirParams make_tree_pir_params_for_scheme(size_t L, size_t a,
                                              const PirParams &scheme);
TreePirParams make_tree_pir_params_for_scheme(size_t L, size_t a, size_t g,
                                              const PirParams &scheme);
TreePirParams make_tree_pir_params_for_scheme(size_t L, size_t a, size_t g,
                                              const PirParams &scheme,
                                              size_t session_key_height);

PirParams tree_query_expansion_params(const TreePirParams &tree,
                                      const PirParams &scheme);

void add_bfv_query_constant(RlweCt &ct, const PirParams &scheme,
                            size_t logical_coeff, size_t h_q,
                            uint64_t value_mod_t);

void add_rlwe_star_constant(RlweCt &ct, const PirParams &scheme,
                            size_t logical_coeff, size_t h_q,
                            std::span<const uint64_t> value_per_limb);

RlweCt make_tree_query(PirClient &client, const PirParams &scheme,
                       const TreePirParams &tree, size_t leaf,
                       QuerySeed *seed = nullptr);

struct ExpandedTreeQuery {
  std::vector<RlweCt> alpha;
  std::vector<GSWCt> beta_selectors;
  std::vector<GSWCt> gamma_selectors;
};

struct TreeQueryUnpackTiming {
  std::chrono::nanoseconds query_expand{};
  std::chrono::nanoseconds selector_completion{};
};

ExpandedTreeQuery unpack_tree_query(PirServer &server,
                                    const PirParams &scheme,
                                    const TreePirParams &tree,
                                    size_t client_id, RlweCt &query,
                                    TreeQueryUnpackTiming *timing = nullptr);
