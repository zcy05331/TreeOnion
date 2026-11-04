#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct TreePirParams {
  size_t N = 0;
  size_t L = 0;
  size_t n = 0;
  size_t g = 1;
  size_t rho = 0;
  size_t r = 0;
  size_t N0 = 0;
  size_t a = 0;
  size_t P = 0;
  size_t B = 0;
  size_t b = 0;
  size_t ell_beta = 0;
  size_t ell_gamma = 0;
  size_t w = 0;
  size_t h_q = 0;
  size_t W = 0;
  uint64_t t = 0;
  std::vector<uint64_t> rns_moduli;
  size_t response_degree = 0;
  std::vector<uint64_t> response_moduli;
};

TreePirParams make_tree_pir_params(size_t L, size_t a, size_t n,
                                   size_t ell_beta, size_t ell_gamma,
                                   uint64_t t,
                                   std::vector<uint64_t> rns_moduli);

TreePirParams make_tree_pir_params(size_t L, size_t a, size_t n, size_t g,
                                   size_t ell_beta, size_t ell_gamma,
                                   uint64_t t,
                                   std::vector<uint64_t> rns_moduli);

void validate_tree_params(const TreePirParams &params);

struct ClientCoordinates {
  size_t alpha = 0;
  std::vector<uint8_t> beta_bits_le;
  std::vector<uint8_t> gamma_bits_le;
};

enum class SelectCase {
  Single,
  CoarsenedAlpha,
  AlphaBeta,
};

struct LevelPlan {
  size_t level = 0;
  size_t R = 0;

  SelectCase select_case = SelectCase::Single;
  size_t coarsen_count = 0;

  size_t beta_begin = 0;
  size_t beta_count = 0;

  size_t gamma_begin = 0;
  size_t gamma_count = 0;

  size_t projection_depth = 0;
};

struct LevelOracle {
  size_t node_index = 0;
  size_t packed_plaintext_index = 0;
  size_t record_position = 0;
};

ClientCoordinates client_index(size_t leaf, const TreePirParams &params);

std::vector<LevelPlan> build_level_plans(const TreePirParams &params);

LevelOracle build_level_oracle_for_test(size_t leaf, size_t level,
                                        const TreePirParams &params);

size_t level_record_position_from_coordinates(size_t alpha, size_t beta,
                                              size_t level,
                                              const TreePirParams &params);
