#include "tree_query.h"

#include "utils.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void require_capacity_in_ring(size_t h_q) {
  if (h_q >= std::numeric_limits<size_t>::digits ||
      (size_t{1} << h_q) > DBConsts::PolyDegree) {
    throw std::invalid_argument(
        "query capacity 2^h_q exceeds the ring degree");
  }
}

void require_scheme_binding(const TreePirParams &tree,
                            const PirParams &scheme) {
  validate_tree_params(tree);
  const auto &scheme_mods = scheme.get_rns_mods();
  if (tree.n != scheme.get_poly_degree() ||
      tree.t != scheme.get_plain_mod() ||
      tree.ell_beta != scheme.get_l() || tree.ell_gamma != scheme.get_l() ||
      !std::equal(tree.rns_moduli.begin(), tree.rns_moduli.end(),
                  scheme_mods.begin(), scheme_mods.end())) {
    throw std::invalid_argument(
        "tree parameters are not bound to this scheme");
  }
}

}

TreePirParams make_tree_pir_params_for_scheme(size_t L, size_t a,
                                              const PirParams &scheme) {
  return make_tree_pir_params_for_scheme(L, a, 1, scheme);
}

TreePirParams make_tree_pir_params_for_scheme(size_t L, size_t a, size_t g,
                                              const PirParams &scheme) {
  return make_tree_pir_params_for_scheme(L, a, g, scheme,
                                         scheme.get_expan_height());
}

TreePirParams make_tree_pir_params_for_scheme(size_t L, size_t a, size_t g,
                                              const PirParams &scheme,
                                              size_t session_key_height) {
  const std::vector<uint64_t> &mods = scheme.get_rns_mods();
  TreePirParams params = make_tree_pir_params(
      L, a, scheme.get_poly_degree(), g, scheme.get_l(), scheme.get_l(),
      scheme.get_plain_mod(),
      std::vector<uint64_t>(mods.begin(), mods.end()));
  if (params.h_q > session_key_height) {
    throw std::invalid_argument(
        "tree query expansion height exceeds the session-key coverage");
  }
  return params;
}

PirParams tree_query_expansion_params(const TreePirParams &tree,
                                      const PirParams &scheme) {
  return scheme.with_query_shape(
      {tree.N0, tree.b + tree.r, tree.h_q});
}

void add_bfv_query_constant(RlweCt &ct, const PirParams &scheme,
                            size_t logical_coeff, size_t h_q,
                            uint64_t value_mod_t) {
  constexpr size_t N = DBConsts::PolyDegree;
  const size_t K = scheme.K();
  const auto &qs = scheme.get_rns_mods();
  const uint64_t t = scheme.get_plain_mod();
  require_capacity_in_ring(h_q);
  if (ct.ntt_form || ct.c0.size() != K * N) {
    throw std::invalid_argument(
        "add_bfv_query_constant needs a coefficient-form full-q ciphertext");
  }
  if (logical_coeff >= (size_t{1} << h_q)) {
    throw std::invalid_argument("BFV query constant is outside the capacity");
  }
  const size_t reversed = utils::bit_reverse(logical_coeff, h_q);

  if (K == 1) {
    const uint64_t Q = qs[0];
    const uint64_t scaled =
        utils::round_div_u128((uint128_t)Q * value_mod_t, t) % Q;
    ct.c0[reversed] = (ct.c0[reversed] + scaled) % Q;
  } else if (K == 2) {
    const uint128_t Q = static_cast<uint128_t>(qs[0]) * qs[1];
    const uint128_t Delta = Q / t;
    const uint64_t rem = static_cast<uint64_t>(Q - Delta * t);
    const uint64_t rem_value_round = static_cast<uint64_t>(
        (static_cast<uint128_t>(rem) * value_mod_t + (t >> 1)) / t);
    const uint128_t scaled_mp = Delta * value_mod_t + rem_value_round;
    for (size_t k = 0; k < K; ++k) {
      const uint64_t scaled_k = static_cast<uint64_t>(scaled_mp % qs[k]);
      const size_t idx = k * N + reversed;
      ct.c0[idx] = (ct.c0[idx] + scaled_k) % qs[k];
    }
  } else {
    throw std::invalid_argument("BFV query lift supports only K <= 2");
  }
}

void add_rlwe_star_constant(RlweCt &ct, const PirParams &scheme,
                            size_t logical_coeff, size_t h_q,
                            std::span<const uint64_t> value_per_limb) {
  constexpr size_t N = DBConsts::PolyDegree;
  const size_t K = scheme.K();
  const auto &qs = scheme.get_rns_mods();
  require_capacity_in_ring(h_q);
  if (ct.ntt_form || ct.c0.size() != K * N) {
    throw std::invalid_argument(
        "add_rlwe_star_constant needs a coefficient-form full-q ciphertext");
  }
  if (value_per_limb.size() != K) {
    throw std::invalid_argument(
        "add_rlwe_star_constant needs one value per RNS limb");
  }
  if (logical_coeff >= (size_t{1} << h_q)) {
    throw std::invalid_argument("RLWE* constant is outside the capacity");
  }
  const size_t reversed = utils::bit_reverse(logical_coeff, h_q);

  for (size_t k = 0; k < K; ++k) {
    const size_t idx = k * N + reversed;
    ct.c0[idx] = (ct.c0[idx] + value_per_limb[k] % qs[k]) % qs[k];
  }
}

RlweCt make_tree_query(PirClient &client, const PirParams &scheme,
                       const TreePirParams &tree, size_t leaf,
                       QuerySeed *seed) {
  require_scheme_binding(tree, scheme);
  const ClientCoordinates coords = client_index(leaf, tree);

  RlweCt query = seed != nullptr ? client.fresh_zero_ct_seeded(*seed)
                                 : client.fresh_zero_ct();

  const uint64_t t = scheme.get_plain_mod();
  uint64_t w_inv_t = 0;
  if (!utils::try_invert_uint_mod(tree.W, t, w_inv_t)) {
    throw std::invalid_argument("W is not invertible modulo t");
  }
  add_bfv_query_constant(query, scheme, coords.alpha, tree.h_q, w_inv_t);

  const auto &qs = scheme.get_rns_mods();
  const size_t K = scheme.K();
  std::vector<uint64_t> w_inv_q(K);
  for (size_t k = 0; k < K; ++k) {
    if (!utils::try_invert_uint_mod(tree.W, qs[k], w_inv_q[k])) {
      throw std::invalid_argument("W is not invertible modulo a RNS limb");
    }
  }
  const size_t ell = scheme.get_l();
  const std::vector<std::vector<uint64_t>> gadget = utils::gsw_gadget(
      ell, scheme.get_base_log2(),
      std::vector<uint64_t>(qs.begin(), qs.end()), scheme.get_ep_low_bits());

  std::vector<uint64_t> value_per_limb(K);
  const auto append_selector_rows = [&](size_t group_index) {
    for (size_t row = 0; row < ell; ++row) {
      const size_t logical = tree.N0 + group_index * ell + row;
      for (size_t k = 0; k < K; ++k) {
        value_per_limb[k] = static_cast<uint64_t>(
            (uint128_t)gadget[k][row] * w_inv_q[k] % qs[k]);
      }
      add_rlwe_star_constant(query, scheme, logical, tree.h_q,
                             value_per_limb);
    }
  };

  for (size_t u = 0; u < tree.b; ++u) {
    if (coords.beta_bits_le[u] == 1) append_selector_rows(u);
  }
  for (size_t v = 0; v < tree.r; ++v) {
    if (coords.gamma_bits_le[v] == 1) append_selector_rows(tree.b + v);
  }
  return query;
}

ExpandedTreeQuery unpack_tree_query(PirServer &server,
                                    const PirParams &scheme,
                                    const TreePirParams &tree,
                                    size_t client_id, RlweCt &query,
                                    TreeQueryUnpackTiming *timing) {
  using Clock = std::chrono::steady_clock;
  require_scheme_binding(tree, scheme);
  const PirParams &view = server.get_params();
  if (view.get_fst_dim_sz() != tree.N0 ||
      view.get_num_dims() != 1 + tree.b + tree.r ||
      view.get_expan_height() != tree.h_q ||
      !scheme.scheme_compatible(view)) {
    throw std::invalid_argument(
        "unpack_tree_query server was not built on this tree's query shape");
  }

  auto timing_start = Clock::now();
  std::vector<RlweCt> expanded = server.expand_query(client_id, query);
  if (timing != nullptr) {
    timing->query_expand = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - timing_start);
  }
  if (expanded.size() != tree.w) {
    throw std::runtime_error(
        "unpack_tree_query expansion returned an unexpected leaf count");
  }

  timing_start = Clock::now();
  std::vector<GSWCt> selectors =
      server.complete_selectors(client_id, expanded);
  if (timing != nullptr) {
    timing->selector_completion =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - timing_start);
  }
  if (selectors.size() != tree.b + tree.r) {
    throw std::runtime_error(
        "unpack_tree_query completion returned an unexpected selector count");
  }

  ExpandedTreeQuery result;
  result.alpha.assign(std::make_move_iterator(expanded.begin()),
                      std::make_move_iterator(expanded.begin() + tree.N0));
  result.beta_selectors.assign(
      std::make_move_iterator(selectors.begin()),
      std::make_move_iterator(selectors.begin() + tree.b));
  result.gamma_selectors.assign(
      std::make_move_iterator(selectors.begin() + tree.b),
      std::make_move_iterator(selectors.end()));
  return result;
}
