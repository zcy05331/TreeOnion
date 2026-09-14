#include "tree_superrecord_select.h"

#include "matrix.h"
#include "tree_rotate.h"
#include "tree_select.h"
#include "utils.h"

#include "hexl/hexl.hpp"

#include <algorithm>

namespace tree_superrecord {

AlphaPyramid build_ragged_alpha_pyramid(std::span<const RlweCt> alpha,
                                        const TreeParams &params,
                                        const PirParams &runtime_scheme) {
  require(alpha.size() == params.shape.N0,
          "alpha pyramid needs the N0 expanded one-hot ciphertexts");
  AlphaPyramid pyramid;
  pyramid.reserve(params.shape.alpha_height + 1);
  pyramid.emplace_back(alpha.begin(), alpha.end());
  while (pyramid.back().size() > 1) {
    const std::vector<RlweCt> &prev = pyramid.back();
    std::vector<RlweCt> next;
    next.reserve((prev.size() + 1) / 2);
    for (size_t j = 0; 2 * j < prev.size(); ++j) {
      RlweCt sum = prev[2 * j];
      if (2 * j + 1 < prev.size()) {
        tree_ct_add_inplace(sum, prev[2 * j + 1], runtime_scheme);
      }
      next.push_back(std::move(sum));
    }
    pyramid.push_back(std::move(next));
  }
  require(pyramid.size() == params.shape.alpha_height + 1,
          "ragged pyramid height does not match the plan");
  return pyramid;
}

namespace {

void require_alpha_ciphertext_shape(const RlweCt &ct,
                                    const PirParams &runtime_scheme) {
  const size_t width = checked_mul(runtime_scheme.K(),
                                   runtime_scheme.get_poly_degree(),
                                   "alpha ciphertext width");
  require(!ct.ntt_form && ct.c0.size() == width && ct.c1.size() == width,
          "alpha pyramid input must be coefficient-form with K*n coefficients");
}

void ntt_ct_add_inplace(RlweCt &acc, const RlweCt &rhs,
                        const PirParams &runtime_scheme) {
  const size_t degree = runtime_scheme.get_poly_degree();
  const auto &qs = runtime_scheme.get_rns_mods();
  const size_t width = checked_mul(qs.size(), degree, "alpha ciphertext width");
  require(acc.ntt_form && rhs.ntt_form && acc.c0.size() == width &&
              acc.c1.size() == width && rhs.c0.size() == width &&
              rhs.c1.size() == width,
          "alpha pyramid NTT addition needs matching NTT-form ciphertexts");
  for (size_t k = 0; k < qs.size(); ++k) {
    const size_t offset = k * degree;
    intel::hexl::EltwiseAddMod(acc.c0.data() + offset,
                               acc.c0.data() + offset,
                               rhs.c0.data() + offset, degree, qs[k]);
    intel::hexl::EltwiseAddMod(acc.c1.data() + offset,
                               acc.c1.data() + offset,
                               rhs.c1.data() + offset, degree, qs[k]);
  }
}

}  // namespace

AlphaPyramid build_ragged_alpha_pyramid_ntt(
    std::span<const RlweCt> alpha, const TreeParams &params,
    const PirParams &runtime_scheme) {
  require(alpha.size() == params.shape.N0,
          "alpha pyramid needs the N0 expanded one-hot ciphertexts");
  const size_t degree = runtime_scheme.get_poly_degree();
  const auto &qs = runtime_scheme.get_rns_mods();
  AlphaPyramid pyramid;
  pyramid.reserve(params.shape.alpha_height + 1);
  std::vector<RlweCt> base;
  base.reserve(alpha.size());
  for (const RlweCt &ct : alpha) {
    require_alpha_ciphertext_shape(ct, runtime_scheme);
    RlweCt lifted = ct;
    for (size_t k = 0; k < qs.size(); ++k) {
      const size_t offset = k * degree;
      utils::ntt_fwd(lifted.c0.data() + offset, degree, qs[k]);
      utils::ntt_fwd(lifted.c1.data() + offset, degree, qs[k]);
    }
    lifted.ntt_form = true;
    base.push_back(std::move(lifted));
  }
  pyramid.push_back(std::move(base));
  while (pyramid.back().size() > 1) {
    const std::vector<RlweCt> &prev = pyramid.back();
    std::vector<RlweCt> next;
    next.reserve((prev.size() + 1) / 2);
    for (size_t j = 0; 2 * j < prev.size(); ++j) {
      RlweCt sum = prev[2 * j];
      if (2 * j + 1 < prev.size()) {
        ntt_ct_add_inplace(sum, prev[2 * j + 1], runtime_scheme);
      }
      next.push_back(std::move(sum));
    }
    pyramid.push_back(std::move(next));
  }
  require(pyramid.size() == params.shape.alpha_height + 1,
          "ragged pyramid height does not match the plan");
  return pyramid;
}

PyramidCounters ragged_pyramid_counters(size_t N0) {
  PyramidCounters counters;
  for (size_t len : ragged_pyramid_row_lengths(N0)) counters.ciphertexts += len;
  counters.additions = N0 - 1;
  counters.carries = counters.ciphertexts - N0 - counters.additions;
  return counters;
}

std::pair<size_t, size_t> plan_pyramid_row_and_cols(const LevelPlan &plan) {
  switch (plan.select_case) {
    case SelectCase::AlphaIndex:
      return {size_t{0}, plan.matrix_cols};
    case SelectCase::CoarsenedAlpha:
    case SelectCase::Single:
      return {plan.alpha_coarsen, plan.matrix_cols};
  }
  fail("unknown level plan case");
}

static std::vector<uint64_t> ntt_reference_selectors(
    const std::vector<RlweCt> &selector, size_t cols, bool second,
    const PirParams &runtime_scheme) {
  const size_t degree = runtime_scheme.get_poly_degree();
  const auto &qs = runtime_scheme.get_rns_mods();
  const size_t K = qs.size();
  std::vector<uint64_t> out(checked_mul(checked_mul(cols, K, "selector limbs"),
                                        degree, "selector coefficients"));
  for (size_t col = 0; col < cols; ++col) {
    const RlweCt &ct = selector[col];
    require(!ct.ntt_form && ct.c0.size() == K * degree &&
                ct.c1.size() == K * degree,
            "selector ciphertext has the wrong shape");
    const std::vector<uint64_t> &src = second ? ct.c1 : ct.c0;
    for (size_t k = 0; k < K; ++k) {
      uint64_t *dst = out.data() + (col * K + k) * degree;
      std::copy(src.begin() + k * degree, src.begin() + (k + 1) * degree,
                dst);
      utils::ntt_fwd(dst, degree, qs[k]);
    }
  }
  return out;
}

RlweCt fold_candidates(std::vector<RlweCt> candidates, const LevelPlan &plan,
                       std::span<GSWCt> sigma_selectors, PirServer &mux,
                       const CiphertextDiagnostic *fold_checkpoint) {
  require(candidates.size() ==
              checked_pow2(plan.fold_bit_count, "fold candidates"),
          "candidate count must be 2^fold_bit_count");
  require(plan.fold_bit_count == 0 ||
              plan.fold_bit_begin + plan.fold_bit_count <=
                  sigma_selectors.size(),
          "fold needs a selector for every active sigma bit");
  // Candidates are in natural delta order, so the remaining MSB splits the
  // array into a lower (bit 0) and an upper (bit 1) half.
  for (size_t step = 0; step < plan.fold_bit_count; ++step) {
    const size_t v = plan.fold_bit_begin + plan.fold_bit_count - 1 - step;
    const size_t half = candidates.size() / 2;
    for (size_t j = 0; j < half; ++j) {
      mux.ext_prod_mux(candidates[j], candidates[j + half],
                       sigma_selectors[v], candidates[j]);
      if (fold_checkpoint) (*fold_checkpoint)(step, candidates[j]);
    }
    candidates.resize(half);
  }
  require(candidates.size() == 1, "fold must end with a single candidate");
  return std::move(candidates.front());
}

RlweCt select_level_reference(const LevelPlaneDatabase &db,
                              const LevelPlan &plan,
                              const AlphaPyramid &pyramid,
                              std::span<GSWCt> sigma_selectors, PirServer &mux,
                              const TreeParams &params,
                              const PirParams &runtime_scheme,
                              TreeServerTiming *timing,
                              const CiphertextDiagnostic *first_dim_checkpoint,
                              const CiphertextDiagnostic *fold_checkpoint) {
  require(db.level == plan.level && db.matrix_rows == plan.matrix_rows &&
              db.matrix_cols == plan.matrix_cols &&
              db.plaintexts.size() == plan.padded_plaintexts_per_plane,
          "level database does not match the plan");
  const auto [row_index, cols] = plan_pyramid_row_and_cols(plan);
  require(row_index < pyramid.size() && pyramid[row_index].size() == cols,
          "pyramid row does not match the plan");
  const std::vector<RlweCt> &selector = pyramid[row_index];
  StageClock clock(timing);
  // All products and sums stay modulo the original full-q limbs. Linearity
  // permits one inverse transform per row, preserving coefficient-exact
  // outputs for every diagnostic callback without the m32 noise-gate cycle.
  const size_t degree = runtime_scheme.get_poly_degree();
  const auto &qs = runtime_scheme.get_rns_mods();
  const size_t K = qs.size();
  const std::vector<uint64_t> selector_c0_ntt =
      ntt_reference_selectors(selector, cols, false, runtime_scheme);
  const std::vector<uint64_t> selector_c1_ntt =
      ntt_reference_selectors(selector, cols, true, runtime_scheme);
  std::vector<uint64_t> cached_plaintext;
  std::vector<uint64_t> pt_ntt(K * degree);
  std::vector<uint64_t> product(degree);
  std::vector<RlweCt> candidates;
  candidates.reserve(plan.matrix_rows);
  for (size_t row = 0; row < plan.matrix_rows; ++row) {
    RlweCt acc;
    acc.c0.assign(K * degree, 0);
    acc.c1.assign(K * degree, 0);
    acc.ntt_form = false;
    for (size_t col = 0; col < cols; ++col) {
      const std::vector<uint64_t> &pt =
          db.plaintexts[matrix_plaintext_index(row, col, plan)];
      require(pt.size() == degree, "canonical plaintext has the wrong degree");
      if (cached_plaintext != pt) {
        cached_plaintext = pt;
        for (size_t k = 0; k < K; ++k) {
          uint64_t *dst = pt_ntt.data() + k * degree;
          std::copy(pt.begin(), pt.end(), dst);
          utils::ntt_fwd(dst, degree, qs[k]);
        }
      }
      for (size_t k = 0; k < K; ++k) {
        const uint64_t q = qs[k];
        const uint64_t *pt_limb = pt_ntt.data() + k * degree;
        uint64_t *c0_limb = acc.c0.data() + k * degree;
        uint64_t *c1_limb = acc.c1.data() + k * degree;
        const size_t selector_at = (col * K + k) * degree;
        intel::hexl::EltwiseMultMod(
            product.data(), pt_limb, selector_c0_ntt.data() + selector_at,
            degree, q, 1);
        intel::hexl::EltwiseAddMod(c0_limb, c0_limb, product.data(), degree,
                                   q);
        intel::hexl::EltwiseMultMod(
            product.data(), pt_limb, selector_c1_ntt.data() + selector_at,
            degree, q, 1);
        intel::hexl::EltwiseAddMod(c1_limb, c1_limb, product.data(), degree,
                                   q);
      }
    }
    for (size_t k = 0; k < K; ++k) {
      utils::ntt_inv(acc.c0.data() + k * degree, degree, qs[k]);
      utils::ntt_inv(acc.c1.data() + k * degree, degree, qs[k]);
    }
    if (first_dim_checkpoint) (*first_dim_checkpoint)(row, acc);
    candidates.push_back(std::move(acc));
  }
  clock.lap(&TreeServerTiming::alpha_scan);
  if (plan.select_case != SelectCase::AlphaIndex) {
    return std::move(candidates.front());
  }
  RlweCt folded =
      fold_candidates(std::move(candidates), plan, sigma_selectors, mux,
                      fold_checkpoint);
  clock.lap(&TreeServerTiming::beta_fold);
  return folded;
}

RlweCt select_level_m32(const LevelPlaneM32 &db, const LevelPlan &plan,
                        const ::AlphaPyramidM32 &pyramid_m32,
                        std::span<GSWCt> sigma_selectors, PirServer &mux,
                        const TreeParams &params,
                        const PirParams &runtime_scheme,
                        TreeServerTiming *timing) {
  const size_t degree = runtime_scheme.get_poly_degree();
  const CompositeRnsTables &crt = runtime_scheme.get_composite_rns();
  require(crt.enabled, "select_level_m32 needs a composite configuration");
  require(db.level == plan.level && db.rows == plan.matrix_rows &&
              db.cols == plan.matrix_cols,
          "level m32 view does not match the plan");
  const auto [pyramid_row, cols] = plan_pyramid_row_and_cols(plan);
  require(pyramid_row < pyramid_m32.lo.size() &&
              pyramid_m32.lo[pyramid_row].size() == degree * cols * 2,
          "pyramid row does not match the m32 view");

  StageClock clock(timing);
  const size_t out_elems = degree * db.rows * 2;
  std::vector<uint64_t> out_lo(out_elems), out_hi(out_elems);
  level_mat_mat_32(db.lo.data(), pyramid_m32.lo[pyramid_row].data(),
                   out_lo.data(), db.rows, db.cols, degree, crt.q1);
  level_mat_mat_32(db.hi.data(), pyramid_m32.hi[pyramid_row].data(),
                   out_hi.data(), db.rows, db.cols, degree, crt.q2);
  clock.lap(&TreeServerTiming::alpha_scan);

  const uint64_t q = runtime_scheme.get_rns_mods()[0];
  std::vector<RlweCt> candidates;
  candidates.reserve(db.rows);
  for (size_t row = 0; row < db.rows; ++row) {
    RlweCt ct;
    ct.c0.assign(degree, 0);
    ct.c1.assign(degree, 0);
    ct.ntt_form = false;
    for (size_t coeff = 0; coeff < degree; ++coeff) {
      for (size_t comp = 0; comp < 2; ++comp) {
        const size_t at = (coeff * db.rows + row) * 2 + comp;
        const uint64_t x1 = out_lo[at];
        const uint64_t x2 = out_hi[at];
        // Garner: x = x1 + q1 * ((x2 - x1) * q1^{-1} mod q2), canonical in
        // [0, q1 q2), which is what makes the m32 and reference outputs equal.
        const uint64_t diff = (x2 + crt.q2 - x1 % crt.q2) % crt.q2;
        const uint64_t lift = static_cast<uint64_t>(
            (static_cast<uint128_t>(diff) * crt.q1_inv_mod_q2) % crt.q2);
        (comp == 0 ? ct.c0 : ct.c1)[coeff] = x1 + crt.q1 * lift;
      }
    }
    utils::ntt_inv(ct.c0.data(), degree, q);
    utils::ntt_inv(ct.c1.data(), degree, q);
    candidates.push_back(std::move(ct));
  }
  clock.lap(&TreeServerTiming::first_dim_finalize);

  if (plan.select_case != SelectCase::AlphaIndex) {
    return std::move(candidates.front());
  }
  RlweCt folded =
      fold_candidates(std::move(candidates), plan, sigma_selectors, mux);
  clock.lap(&TreeServerTiming::beta_fold);
  return folded;
}


}  // namespace tree_superrecord
