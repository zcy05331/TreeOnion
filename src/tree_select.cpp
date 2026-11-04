#include "tree_select.h"

#include "tree_select_internal.h"

#include "matrix.h"
#include "utils.h"

#include "hexl/hexl.hpp"

#include <algorithm>
#include <memory>
#include <utility>

using tree_select_internal::plan_row_and_cols;
using tree_select_internal::require;

namespace {

void ct_add_inplace(RlweCt &acc, const RlweCt &x, const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = scheme.get_rns_mods();
  require(!acc.ntt_form && !x.ntt_form &&
              acc.c0.size() == x.c0.size() &&
              acc.c0.size() == qs.size() * N,
          "ciphertext addition needs matching coefficient-form inputs");
  for (size_t k = 0; k < qs.size(); ++k) {
    intel::hexl::EltwiseAddMod(acc.c0.data() + k * N, acc.c0.data() + k * N,
                               x.c0.data() + k * N, N, qs[k]);
    intel::hexl::EltwiseAddMod(acc.c1.data() + k * N, acc.c1.data() + k * N,
                               x.c1.data() + k * N, N, qs[k]);
  }
}

void ntt_ct_add_inplace(RlweCt &acc, const RlweCt &x,
                        const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = scheme.get_rns_mods();
  require(acc.ntt_form && x.ntt_form && acc.c0.size() == x.c0.size() &&
              acc.c1.size() == x.c1.size() &&
              acc.c0.size() == acc.c1.size() &&
              acc.c0.size() == qs.size() * N,
          "NTT ciphertext addition needs matching NTT-form inputs");
  for (size_t k = 0; k < qs.size(); ++k) {
    intel::hexl::EltwiseAddMod(acc.c0.data() + k * N, acc.c0.data() + k * N,
                               x.c0.data() + k * N, N, qs[k]);
    intel::hexl::EltwiseAddMod(acc.c1.data() + k * N, acc.c1.data() + k * N,
                               x.c1.data() + k * N, N, qs[k]);
  }
}


}

RlweCt tree_pt_ct_mul(const std::vector<uint64_t> &pt, const RlweCt &ct,
                      const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = scheme.get_rns_mods();
  const size_t K = qs.size();
  require(pt.size() == N, "plaintext polynomial has the wrong degree");
  require(!ct.ntt_form && ct.c0.size() == K * N && ct.c1.size() == K * N,
          "PtCtMul needs a coefficient-form full-q ciphertext");

  RlweCt result;
  result.c0.assign(K * N, 0);
  result.c1.assign(K * N, 0);
  result.ntt_form = false;
  std::vector<uint64_t> pt_ntt(N), component(N);
  for (size_t k = 0; k < K; ++k) {
    const uint64_t q = qs[k];
    std::copy(pt.begin(), pt.end(), pt_ntt.begin());
    utils::ntt_fwd(pt_ntt.data(), N, q);
    for (const bool second : {false, true}) {
      const uint64_t *src = (second ? ct.c1.data() : ct.c0.data()) + k * N;
      uint64_t *dst = (second ? result.c1.data() : result.c0.data()) + k * N;
      std::copy(src, src + N, component.data());
      utils::ntt_fwd(component.data(), N, q);
      intel::hexl::EltwiseMultMod(component.data(), component.data(),
                                  pt_ntt.data(), N, q, 1);
      utils::ntt_inv(component.data(), N, q);
      std::copy(component.begin(), component.end(), dst);
    }
  }
  return result;
}

AlphaPyramid build_alpha_pyramid(std::span<const RlweCt> alpha_cts,
                                 const TreePirParams &tree,
                                 const PirParams &scheme) {
  require(alpha_cts.size() == tree.N0,
          "alpha pyramid needs the N0 expanded one-hot ciphertexts");
  AlphaPyramid pyramid;
  pyramid.reserve(tree.a + 1);
  pyramid.emplace_back(alpha_cts.begin(), alpha_cts.end());
  for (size_t c = 0; c < tree.a; ++c) {
    const std::vector<RlweCt> &prev = pyramid.back();
    std::vector<RlweCt> next;
    next.reserve(prev.size() / 2);
    for (size_t j = 0; j < prev.size() / 2; ++j) {
      RlweCt sum = prev[2 * j];
      ct_add_inplace(sum, prev[2 * j + 1], scheme);
      next.push_back(std::move(sum));
    }
    pyramid.push_back(std::move(next));
  }
  return pyramid;
}

std::vector<RlweCt> evaluate_alpha_dimension(const TreeLevelDatabase &db,
                                             std::span<const RlweCt> alpha,
                                             const LevelPlan &plan,
                                             const TreePirParams &tree,
                                             const PirParams &scheme) {
  require(plan.select_case == SelectCase::AlphaBeta,
          "alpha dimension evaluation applies to the AlphaBeta case");
  require(db.level == plan.level && db.R == plan.R,
          "level database does not match the plan");
  require(alpha.size() == tree.N0, "alpha vector must have N0 entries");
  const size_t stride = size_t{1} << plan.beta_count;
  require(db.R == tree.N0 * stride, "AlphaBeta level has R = N0 * 2^d");

  std::vector<RlweCt> candidates;
  candidates.reserve(stride);
  for (size_t delta = 0; delta < stride; ++delta) {
    RlweCt acc = tree_pt_ct_mul(db.plaintexts[delta], alpha[0], scheme);
    for (size_t k = 1; k < tree.N0; ++k) {
      RlweCt term =
          tree_pt_ct_mul(db.plaintexts[k * stride + delta], alpha[k], scheme);
      ct_add_inplace(acc, term, scheme);
    }
    candidates.push_back(std::move(acc));
  }
  return candidates;
}

RlweCt fold_beta_dimension(std::vector<RlweCt> candidates,
                           const LevelPlan &plan,
                           std::span<GSWCt> beta_selectors, PirServer &mux) {
  require(candidates.size() == size_t{1} << plan.beta_count,
          "candidate count must be 2^{beta_count}");
  require(plan.beta_begin + plan.beta_count <= beta_selectors.size() ||
              plan.beta_count == 0,
          "fold needs a selector for every active beta bit");

  for (size_t step = 0; step < plan.beta_count; ++step) {
    const size_t u = plan.beta_begin + plan.beta_count - 1 - step;
    const size_t half = candidates.size() / 2;
    for (size_t j = 0; j < half; ++j) {
      mux.ext_prod_mux(candidates[j], candidates[j + half], beta_selectors[u],
                       candidates[j]);
    }
    candidates.resize(half);
  }
  require(candidates.size() == 1, "fold must end with a single candidate");
  return std::move(candidates.front());
}


AlphaPyramid pyramid_to_ntt(const AlphaPyramid &pyramid,
                            const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = scheme.get_rns_mods();
  AlphaPyramid ntt;
  ntt.reserve(pyramid.size());
  for (const std::vector<RlweCt> &row : pyramid) {
    std::vector<RlweCt> out_row;
    out_row.reserve(row.size());
    for (const RlweCt &ct : row) {
      require(!ct.ntt_form, "pyramid must be coefficient form before lifting");
      RlweCt lifted = ct;
      for (size_t k = 0; k < qs.size(); ++k) {
        utils::ntt_fwd(lifted.c0.data() + k * N, N, qs[k]);
        utils::ntt_fwd(lifted.c1.data() + k * N, N, qs[k]);
      }
      lifted.ntt_form = true;
      out_row.push_back(std::move(lifted));
    }
    ntt.push_back(std::move(out_row));
  }
  return ntt;
}

namespace {

std::vector<RlweCt> lift_alpha_to_ntt(std::span<const RlweCt> alpha_cts,
                                      const TreePirParams &tree,
                                      const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = scheme.get_rns_mods();
  require(alpha_cts.size() == tree.N0,
          "alpha pyramid needs the N0 expanded one-hot ciphertexts");
  std::vector<RlweCt> base;
  base.reserve(alpha_cts.size());
  for (const RlweCt &ct : alpha_cts) {
    require(!ct.ntt_form && ct.c0.size() == qs.size() * N &&
                ct.c1.size() == qs.size() * N,
            "alpha pyramid input must be coefficient-form");
    RlweCt lifted = ct;
    for (size_t k = 0; k < qs.size(); ++k) {
      utils::ntt_fwd(lifted.c0.data() + k * N, N, qs[k]);
      utils::ntt_fwd(lifted.c1.data() + k * N, N, qs[k]);
    }
    lifted.ntt_form = true;
    base.push_back(std::move(lifted));
  }
  return base;
}

}

AlphaPyramid build_alpha_pyramid_ntt(std::span<const RlweCt> alpha_cts,
                                     const TreePirParams &tree,
                                     const PirParams &scheme) {
  AlphaPyramid pyramid;
  pyramid.reserve(tree.a + 1);
  pyramid.push_back(lift_alpha_to_ntt(alpha_cts, tree, scheme));
  for (size_t c = 0; c < tree.a; ++c) {
    const std::vector<RlweCt> &prev = pyramid.back();
    std::vector<RlweCt> next;
    next.reserve(prev.size() / 2);
    for (size_t j = 0; j < prev.size() / 2; ++j) {
      RlweCt sum = prev[2 * j];
      ntt_ct_add_inplace(sum, prev[2 * j + 1], scheme);
      next.push_back(std::move(sum));
    }
    pyramid.push_back(std::move(next));
  }
  return pyramid;
}

namespace {

struct NttAccumulator {
  std::vector<uint64_t> c0, c1, tmp;
  void reset(size_t size) {
    c0.assign(size, 0);
    c1.assign(size, 0);
    tmp.assign(size, 0);
  }
  void add_product(const std::vector<uint64_t> &pt_ntt, const RlweCt &a_ntt,
                   const PirParams &scheme) {
    constexpr size_t N = DBConsts::PolyDegree;
    const auto &qs = scheme.get_rns_mods();
    for (size_t k = 0; k < qs.size(); ++k) {
      const uint64_t q = qs[k];
      intel::hexl::EltwiseMultMod(tmp.data() + k * N,
                                  pt_ntt.data() + k * N,
                                  a_ntt.c0.data() + k * N, N, q, 1);
      intel::hexl::EltwiseAddMod(c0.data() + k * N, c0.data() + k * N,
                                 tmp.data() + k * N, N, q);
      intel::hexl::EltwiseMultMod(tmp.data() + k * N,
                                  pt_ntt.data() + k * N,
                                  a_ntt.c1.data() + k * N, N, q, 1);
      intel::hexl::EltwiseAddMod(c1.data() + k * N, c1.data() + k * N,
                                 tmp.data() + k * N, N, q);
    }
  }
  RlweCt finish(const PirParams &scheme) {
    constexpr size_t N = DBConsts::PolyDegree;
    const auto &qs = scheme.get_rns_mods();
    RlweCt out;
    out.c0 = std::move(c0);
    out.c1 = std::move(c1);
    out.ntt_form = false;
    for (size_t k = 0; k < qs.size(); ++k) {
      utils::ntt_inv(out.c0.data() + k * N, N, qs[k]);
      utils::ntt_inv(out.c1.data() + k * N, N, qs[k]);
    }
    return out;
  }
};

}

AlphaPyramidM32 pyramid_to_m32(const AlphaPyramid &pyramid_ntt,
                               const PirParams &scheme,
                               Matrix32QueryLayout query_layout) {
  constexpr size_t N = DBConsts::PolyDegree;
  const CompositeRnsTables &crt = scheme.get_composite_rns();
  require(crt.enabled, "the m32 pyramid needs a composite configuration");
  AlphaPyramidM32 out;
  out.query_layout = query_layout;
  out.lo.reserve(pyramid_ntt.size());
  out.hi.reserve(pyramid_ntt.size());
  for (const std::vector<RlweCt> &row : pyramid_ntt) {
    const size_t cols = row.size();
    CacheAlignedVector<uint32_t> lo(N * cols * 2), hi(N * cols * 2);
    for (size_t k = 0; k < row.size(); ++k) {
      require(row[k].ntt_form, "the m32 pyramid consumes the NTT-form pyramid");
    }
    if (query_layout == Matrix32QueryLayout::SplitColumns) {
      constexpr size_t CoeffTile = 8;
      constexpr size_t ColTile = 16;
      for (size_t coeff_base = 0; coeff_base < N; coeff_base += CoeffTile) {
        const size_t coeff_end = std::min(coeff_base + CoeffTile, N);
        for (size_t col_base = 0; col_base < cols; col_base += ColTile) {
          const size_t col_end = std::min(col_base + ColTile, cols);
          for (size_t k = col_base; k < col_end; ++k) {
            const RlweCt &ct = row[k];
            for (size_t coeff = coeff_base; coeff < coeff_end; ++coeff) {
              const size_t level_base = coeff * cols * 2;
              const size_t c0_at = level_base + k;
              const size_t c1_at = level_base + cols + k;
              lo[c0_at] = static_cast<uint32_t>(ct.c0[coeff] % crt.q1);
              hi[c0_at] = static_cast<uint32_t>(ct.c0[coeff] % crt.q2);
              lo[c1_at] = static_cast<uint32_t>(ct.c1[coeff] % crt.q1);
              hi[c1_at] = static_cast<uint32_t>(ct.c1[coeff] % crt.q2);
            }
          }
        }
      }
    } else {
      for (size_t k = 0; k < row.size(); ++k) {
        for (size_t coeff = 0; coeff < N; ++coeff) {
          for (size_t comp = 0; comp < 2; ++comp) {
            const uint64_t value =
                comp == 0 ? row[k].c0[coeff] : row[k].c1[coeff];
            const size_t level_base = coeff * row.size() * 2;
            const size_t at = level_base + 2 * k + comp;
            lo[at] = static_cast<uint32_t>(value % crt.q1);
            hi[at] = static_cast<uint32_t>(value % crt.q2);
          }
        }
      }
    }
    out.lo.push_back(std::move(lo));
    out.hi.push_back(std::move(hi));
  }
  return out;
}

AlphaPyramidM32 build_alpha_pyramid_m32(std::span<const RlweCt> alpha_cts,
                                        const TreePirParams &tree,
                                        const PirParams &scheme,
                                        Matrix32QueryLayout query_layout) {
  AlphaPyramid row0;
  row0.push_back(lift_alpha_to_ntt(alpha_cts, tree, scheme));
  AlphaPyramidM32 out = pyramid_to_m32(row0, scheme, query_layout);
  out.lo.reserve(tree.a + 1);
  out.hi.reserve(tree.a + 1);
  const size_t w = query_layout == Matrix32QueryLayout::SplitColumns ? 1 : 2;
  const CompositeRnsTables &crt = scheme.get_composite_rns();
  const auto q1 = static_cast<uint32_t>(crt.q1);
  const auto q2 = static_cast<uint32_t>(crt.q2);
  for (size_t c = 0; c < tree.a; ++c) {
    const CacheAlignedVector<uint32_t> &prev_lo = out.lo.back();
    const CacheAlignedVector<uint32_t> &prev_hi = out.hi.back();
    CacheAlignedVector<uint32_t> lo(prev_lo.size() / 2), hi(prev_hi.size() / 2);
    for (size_t u = 0; u < lo.size() / w; ++u) {
      for (size_t e = 0; e < w; ++e) {
        const size_t x = 2 * w * u + e;
        const uint32_t s1 = prev_lo[x] + prev_lo[x + w];
        const uint32_t s2 = prev_hi[x] + prev_hi[x + w];
        lo[w * u + e] = s1 >= q1 ? s1 - q1 : s1;
        hi[w * u + e] = s2 >= q2 ? s2 - q2 : s2;
      }
    }
    out.lo.push_back(std::move(lo));
    out.hi.push_back(std::move(hi));
  }
  return out;
}

RlweCt select_level_m32(const TreeLevelDatabaseM32 &db, const LevelPlan &plan,
                        const AlphaPyramidM32 &pyramid_m32,
                        std::span<GSWCt> beta_selectors, PirServer &mux,
                        const TreePirParams &tree, const PirParams &scheme) {
  ScanScratch scratch;
  return select_level_m32(db, plan, pyramid_m32, beta_selectors, mux, tree,
                          scheme, scratch);
}

RlweCt select_level_m32(const TreeLevelDatabaseM32 &db, const LevelPlan &plan,
                        const AlphaPyramidM32 &pyramid_m32,
                        std::span<GSWCt> beta_selectors, PirServer &mux,
                        const TreePirParams &tree, const PirParams &scheme,
                        ScanScratch &scratch) {
  constexpr size_t N = DBConsts::PolyDegree;
  const CompositeRnsTables &crt = scheme.get_composite_rns();
  require(crt.enabled, "select_level_m32 needs a composite configuration");
  require(db.level == plan.level && db.R == plan.R,
          "level m32 view does not match the plan");
  const auto [pyramid_row, cols] = plan_row_and_cols(plan, tree);
  require(cols == db.cols && pyramid_row < pyramid_m32.lo.size(),
          "pyramid row does not match the m32 view");
  require(pyramid_m32.lo[pyramid_row].size() == N * cols * 2,
          "pyramid row width does not match the m32 view");

  TIME_START(TREE_SCAN_TIME);
  const size_t out_elems = N * db.rows * 2;
  if (scratch.lo.size() < out_elems) scratch.lo.resize(out_elems);
  if (scratch.hi.size() < out_elems) scratch.hi.resize(out_elems);
  TIME_START(TREE_SCAN_CORE_TIME);
  level_mat_mat_32(db.lo.data(), pyramid_m32.lo[pyramid_row].data(),
                   scratch.lo.data(), db.rows, db.cols, N, crt.q1,
                   pyramid_m32.query_layout);
  level_mat_mat_32(db.hi.data(), pyramid_m32.hi[pyramid_row].data(),
                   scratch.hi.data(), db.rows, db.cols, N, crt.q2,
                   pyramid_m32.query_layout);
  TIME_END(TREE_SCAN_CORE_TIME);
  TIME_END(TREE_SCAN_TIME);
  return finish_level_m32(scratch, db.rows, 0, plan, beta_selectors, mux, tree,
                          scheme);
}

void scan_stacked_alpha_beta(const StackedAlphaBetaM32 &db,
                             const AlphaPyramidM32 &pyramid_m32,
                             const PirParams &scheme, ScanScratch &scratch) {
  constexpr size_t N = DBConsts::PolyDegree;
  const CompositeRnsTables &crt = scheme.get_composite_rns();
  require(crt.enabled, "the stacked scan needs a composite configuration");
  require(!pyramid_m32.lo.empty() && pyramid_m32.lo[0].size() == N * db.cols * 2,
          "pyramid row 0 does not match the stacked matrix");
  TIME_START(TREE_SCAN_TIME);
  const size_t out_elems = N * db.rows * 2;
  if (scratch.lo.size() < out_elems) scratch.lo.resize(out_elems);
  if (scratch.hi.size() < out_elems) scratch.hi.resize(out_elems);
  TIME_START(TREE_SCAN_CORE_TIME);
  level_mat_mat_32(db.lo.data(), pyramid_m32.lo[0].data(), scratch.lo.data(),
                   db.rows, db.cols, N, crt.q1, pyramid_m32.query_layout);
  level_mat_mat_32(db.hi.data(), pyramid_m32.hi[0].data(), scratch.hi.data(),
                   db.rows, db.cols, N, crt.q2, pyramid_m32.query_layout);
  TIME_END(TREE_SCAN_CORE_TIME);
  TIME_END(TREE_SCAN_TIME);
}

RlweCt finish_level_m32(const ScanScratch &scan, size_t total_rows,
                        size_t first_row, const LevelPlan &plan,
                        std::span<GSWCt> beta_selectors, PirServer &mux,
                        const TreePirParams &tree, const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const CompositeRnsTables &crt = scheme.get_composite_rns();
  const size_t rows = level_m32_shape(plan, tree).first;
  require(first_row + rows <= total_rows &&
              scan.lo.size() >= N * total_rows * 2 &&
              scan.hi.size() >= N * total_rows * 2,
          "scan output does not cover the level's rows");

  const uint64_t q = scheme.get_rns_mods()[0];
  std::vector<RlweCt> candidates;
  candidates.reserve(rows);
  TIME_START(TREE_SCAN_TIME);
  TIME_START(TREE_SCAN_FINALIZE_TIME);
  for (size_t row = first_row; row < first_row + rows; ++row) {
    RlweCt ct;
    ct.c0.assign(N, 0);
    ct.c1.assign(N, 0);
    ct.ntt_form = false;
    for (size_t coeff = 0; coeff < N; ++coeff) {
      for (size_t comp = 0; comp < 2; ++comp) {
        const size_t at = (coeff * total_rows + row) * 2 + comp;
        const uint64_t x1 = scan.lo[at];
        const uint64_t x2 = scan.hi[at];
        const uint64_t diff = (x2 + crt.q2 - x1 % crt.q2) % crt.q2;
        const uint64_t lift =
            static_cast<uint64_t>((static_cast<uint128_t>(diff) *
                                   crt.q1_inv_mod_q2) % crt.q2);
        const uint64_t value = x1 + crt.q1 * lift;
        (comp == 0 ? ct.c0 : ct.c1)[coeff] = value;
      }
    }
    utils::ntt_inv(ct.c0.data(), N, q);
    utils::ntt_inv(ct.c1.data(), N, q);
    candidates.push_back(std::move(ct));
  }
  TIME_END(TREE_SCAN_FINALIZE_TIME);
  TIME_END(TREE_SCAN_TIME);

  if (plan.select_case != SelectCase::AlphaBeta) {
    return std::move(candidates.front());
  }
  TIME_START(TREE_FOLD_TIME);
  RlweCt folded = fold_beta_dimension(std::move(candidates), plan,
                                      beta_selectors, mux);
  TIME_END(TREE_FOLD_TIME);
  return folded;
}

RlweCt select_level_ntt(const TreeLevelDatabaseNtt &db, const LevelPlan &plan,
                        const AlphaPyramid &pyramid_ntt,
                        std::span<GSWCt> beta_selectors, PirServer &mux,
                        const TreePirParams &tree, const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const size_t K = scheme.K();
  require(db.level == plan.level && db.R == plan.R,
          "level NTT view does not match the plan");
  require(pyramid_ntt.size() == tree.a + 1 &&
              pyramid_ntt[0].size() == tree.N0,
          "alpha pyramid does not match the tree parameters");

  NttAccumulator acc;
  TIME_START(TREE_SCAN_TIME);
  switch (plan.select_case) {
    case SelectCase::Single: {
      acc.reset(K * N);
      acc.add_product(db.plaintexts[0], pyramid_ntt[tree.a][0], scheme);
      RlweCt out = acc.finish(scheme);
      TIME_END(TREE_SCAN_TIME);
      return out;
    }
    case SelectCase::CoarsenedAlpha: {
      const size_t c = plan.coarsen_count;
      require(pyramid_ntt[c].size() == db.R,
              "coarsened pyramid level does not match R");
      acc.reset(K * N);
      for (size_t k = 0; k < db.R; ++k) {
        acc.add_product(db.plaintexts[k], pyramid_ntt[c][k], scheme);
      }
      RlweCt out = acc.finish(scheme);
      TIME_END(TREE_SCAN_TIME);
      return out;
    }
    case SelectCase::AlphaBeta: {
      const size_t stride = size_t{1} << plan.beta_count;
      require(db.R == tree.N0 * stride, "AlphaBeta level has R = N0 * 2^d");
      std::vector<RlweCt> candidates;
      candidates.reserve(stride);
      for (size_t delta = 0; delta < stride; ++delta) {
        acc.reset(K * N);
        for (size_t k = 0; k < tree.N0; ++k) {
          acc.add_product(db.plaintexts[k * stride + delta],
                          pyramid_ntt[0][k], scheme);
        }
        candidates.push_back(acc.finish(scheme));
      }
      TIME_END(TREE_SCAN_TIME);
      TIME_START(TREE_FOLD_TIME);
      RlweCt folded = fold_beta_dimension(std::move(candidates), plan,
                                          beta_selectors, mux);
      TIME_END(TREE_FOLD_TIME);
      return folded;
    }
  }
  TIME_END(TREE_SCAN_TIME);
  throw std::invalid_argument("tree_select: unknown level plan case");
}

RlweCt select_level(const TreeLevelDatabase &db, const LevelPlan &plan,
                    const AlphaPyramid &pyramid,
                    std::span<GSWCt> beta_selectors, PirServer &mux,
                    const TreePirParams &tree, const PirParams &scheme) {
  require(db.level == plan.level && db.R == plan.R,
          "level database does not match the plan");
  require(pyramid.size() == tree.a + 1 && pyramid[0].size() == tree.N0,
          "alpha pyramid does not match the tree parameters");

  switch (plan.select_case) {
    case SelectCase::Single: {
      return tree_pt_ct_mul(db.plaintexts[0], pyramid[tree.a][0], scheme);
    }
    case SelectCase::CoarsenedAlpha: {
      const size_t c = plan.coarsen_count;
      require(pyramid[c].size() == db.R,
              "coarsened pyramid level does not match R");
      RlweCt acc = tree_pt_ct_mul(db.plaintexts[0], pyramid[c][0], scheme);
      for (size_t k = 1; k < db.R; ++k) {
        RlweCt term = tree_pt_ct_mul(db.plaintexts[k], pyramid[c][k], scheme);
        ct_add_inplace(acc, term, scheme);
      }
      return acc;
    }
    case SelectCase::AlphaBeta: {
      std::vector<RlweCt> candidates =
          evaluate_alpha_dimension(db, pyramid[0], plan, tree, scheme);
      return fold_beta_dimension(std::move(candidates), plan, beta_selectors,
                                 mux);
    }
  }
  throw std::invalid_argument("tree_select: unknown level plan case");
}
