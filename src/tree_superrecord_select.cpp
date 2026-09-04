#include "tree_superrecord_select.h"

#include "matrix.h"
#include "tree_rotate.h"
#include "tree_select.h"
#include "utils.h"

#include "hexl/hexl.hpp"

#include <algorithm>
#include <limits>

namespace tree_superrecord {

namespace {

void require_level(const TreeParams &params, size_t level) {
  require(level <= params.shape.tree_height, "level is out of range");
}

// Coefficient positions of super-record x inside its plaintext: u + d * rho.
void write_support(std::vector<uint64_t> &plaintext, const PlaneDigits &digits,
                   size_t record_position, size_t rho) {
  for (size_t d = 0; d < digits.size(); ++d) {
    plaintext[record_position + d * rho] = digits[d];
  }
}

}  // namespace

EncodedSuperRecord encode_superrecord_at(const TreeParams &params,
                                         const EntrySource &source,
                                         size_t level, size_t superrecord) {
  const CandidateShape &shape = params.shape;
  require_level(params, level);
  const size_t level_nodes = checked_pow2(level, "tree level width");
  const size_t S = ceil_div(level_nodes, shape.bundle_arity);
  require(superrecord < S, "super-record index is out of range");
  const CodecSpec spec = codec_spec_for(params);
  std::vector<EntryBits> entries;
  entries.reserve(shape.bundle_arity);
  for (size_t lane = 0; lane < shape.bundle_arity; ++lane) {
    const size_t node = superrecord * shape.bundle_arity + lane;
    if (node >= level_nodes) break;  // tail lanes stay zero
    EntryBits entry = source(level, node);
    require(entry.bit_length == shape.entry_bits,
            "entry source returned the wrong bit length");
    entries.push_back(std::move(entry));
  }
  return encode_super_record(entries, spec);
}

std::vector<LevelPlaneDatabase> pack_level_planes(const TreeParams &params,
                                                  const EntrySource &source,
                                                  size_t level) {
  const CandidateShape &shape = params.shape;
  require_level(params, level);
  const LevelPlan &plan = params.level_plans[level];
  const size_t rho = shape.records_per_plaintext;
  std::vector<LevelPlaneDatabase> planes(shape.plane_count);
  for (size_t z = 0; z < shape.plane_count; ++z) {
    LevelPlaneDatabase &db = planes[z];
    db.level = level;
    db.plane_index = z;
    db.logical_superrecords = plan.logical_superrecords;
    db.logical_plaintexts = plan.logical_plaintexts_per_plane;
    db.matrix_rows = plan.matrix_rows;
    db.matrix_cols = plan.matrix_cols;
    db.plaintexts.assign(plan.padded_plaintexts_per_plane,
                         std::vector<uint64_t>(shape.ring_degree, 0));
  }
  for (size_t x = 0; x < plan.logical_superrecords; ++x) {
    const EncodedSuperRecord encoded =
        encode_superrecord_at(params, source, level, x);
    const size_t p = x / rho;
    const size_t u = x % rho;
    for (size_t z = 0; z < shape.plane_count; ++z) {
      write_support(planes[z].plaintexts[p], encoded[z], u, rho);
    }
  }
  return planes;
}

ReferencePreprocessedTree preprocess_reference(const TreeParams &params,
                                               const PirParams &runtime_scheme,
                                               const EntrySource &source) {
  require_scheme_binding(params, runtime_scheme);
  ReferencePreprocessedTree tree;
  tree.tag = params.tag;
  tree.canonical.reserve(params.shape.path_levels);
  for (size_t level = 0; level <= params.shape.tree_height; ++level) {
    tree.canonical.push_back(pack_level_planes(params, source, level));
  }
  return tree;
}

void rewrite_reference_superrecord(ReferencePreprocessedTree &db, size_t level,
                                   size_t superrecord, const TreeParams &params,
                                   const PirParams &runtime_scheme,
                                   const EntrySource &source) {
  require_scheme_binding(params, runtime_scheme);
  require(db.tag == params.tag, "database PlanTag mismatch");
  require_level(params, level);
  require(db.canonical.size() == params.shape.path_levels,
          "canonical database is missing levels");
  const size_t rho = params.shape.records_per_plaintext;
  const EncodedSuperRecord encoded =
      encode_superrecord_at(params, source, level, superrecord);
  const size_t p = superrecord / rho;
  const size_t u = superrecord % rho;
  for (size_t z = 0; z < params.shape.plane_count; ++z) {
    write_support(db.canonical[level][z].plaintexts[p], encoded[z], u, rho);
  }
}

size_t canonical_database_bytes(const CanonicalTreeDatabase &canonical) {
  size_t bytes = 0;
  for (const auto &planes : canonical) {
    for (const LevelPlaneDatabase &db : planes) {
      for (const auto &pt : db.plaintexts) bytes += pt.size() * sizeof(uint64_t);
    }
  }
  return bytes;
}

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

RlweCt fold_candidates(std::vector<RlweCt> candidates, const LevelPlan &plan,
                       std::span<GSWCt> sigma_selectors, PirServer &mux) {
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
                              TreeServerTiming *timing) {
  require(db.level == plan.level && db.matrix_rows == plan.matrix_rows &&
              db.matrix_cols == plan.matrix_cols &&
              db.plaintexts.size() == plan.padded_plaintexts_per_plane,
          "level database does not match the plan");
  const auto [row_index, cols] = plan_pyramid_row_and_cols(plan);
  require(row_index < pyramid.size() && pyramid[row_index].size() == cols,
          "pyramid row does not match the plan");
  const std::vector<RlweCt> &selector = pyramid[row_index];
  StageClock clock(timing);
  std::vector<RlweCt> candidates;
  candidates.reserve(plan.matrix_rows);
  for (size_t row = 0; row < plan.matrix_rows; ++row) {
    RlweCt acc = tree_pt_ct_mul(
        db.plaintexts[matrix_plaintext_index(row, 0, plan)], selector[0],
        runtime_scheme);
    for (size_t col = 1; col < cols; ++col) {
      const RlweCt term = tree_pt_ct_mul(
          db.plaintexts[matrix_plaintext_index(row, col, plan)], selector[col],
          runtime_scheme);
      tree_ct_add_inplace(acc, term, runtime_scheme);
    }
    candidates.push_back(std::move(acc));
  }
  clock.lap(&TreeServerTiming::alpha_scan);
  if (plan.select_case != SelectCase::AlphaIndex) {
    return std::move(candidates.front());
  }
  RlweCt folded =
      fold_candidates(std::move(candidates), plan, sigma_selectors, mux);
  clock.lap(&TreeServerTiming::beta_fold);
  return folded;
}

void require_m32_scheme(const PirParams &runtime_scheme) {
  const CompositeRnsTables &crt = runtime_scheme.get_composite_rns();
  require(runtime_scheme.K() == 1, "generic m32 v1 requires logical K = 1");
  require(crt.enabled, "generic m32 v1 requires a composite first dimension");
  require(crt.q1 <= std::numeric_limits<uint32_t>::max() &&
              crt.q2 <= std::numeric_limits<uint32_t>::max(),
          "composite limbs do not fit m32 storage");
  require(static_cast<uint128>(crt.q1) * crt.q2 ==
              runtime_scheme.get_rns_mods().at(0),
          "composite limbs do not reconstruct the logical modulus");
}

LevelPlaneM32 build_level_plane_m32(const LevelPlaneDatabase &db,
                                    const LevelPlan &plan,
                                    const PirParams &runtime_scheme) {
  require_m32_scheme(runtime_scheme);
  require(db.level == plan.level && db.matrix_rows == plan.matrix_rows &&
              db.matrix_cols == plan.matrix_cols,
          "level database does not match the plan");
  const size_t degree = runtime_scheme.get_poly_degree();
  const CompositeRnsTables &crt = runtime_scheme.get_composite_rns();
  const uint64_t q = runtime_scheme.get_rns_mods()[0];
  LevelPlaneM32 view;
  view.level = db.level;
  view.plane_index = db.plane_index;
  view.rows = plan.matrix_rows;
  view.cols = plan.matrix_cols;
  const size_t cells = checked_mul(view.rows, view.cols, "m32 matrix cells");
  const size_t coefficients = checked_mul(degree, cells, "m32 coefficients");
  view.lo.assign(coefficients, 0);
  view.hi.assign(coefficients, 0);
  std::vector<uint64_t> ntt(degree);
  for (size_t row = 0; row < view.rows; ++row) {
    for (size_t col = 0; col < view.cols; ++col) {
      const std::vector<uint64_t> &pt =
          db.plaintexts[matrix_plaintext_index(row, col, plan)];
      require(pt.size() == degree, "canonical plaintext has the wrong degree");
      std::copy(pt.begin(), pt.end(), ntt.begin());
      utils::ntt_fwd(ntt.data(), degree, q);
      for (size_t coeff = 0; coeff < degree; ++coeff) {
        const size_t at = (coeff * view.rows + row) * view.cols + col;
        view.lo[at] = static_cast<uint32_t>(ntt[coeff] % crt.q1);
        view.hi[at] = static_cast<uint32_t>(ntt[coeff] % crt.q2);
      }
    }
  }
  return view;
}

PreprocessedTree preprocess_m32(const TreeParams &params,
                                const PirParams &runtime_scheme,
                                const EntrySource &source,
                                bool retain_canonical) {
  require_scheme_binding(params, runtime_scheme);
  require_m32_scheme(runtime_scheme);
  PreprocessedTree tree;
  tree.tag = params.tag;
  tree.m32.reserve(params.shape.path_levels);
  if (retain_canonical) tree.canonical.reserve(params.shape.path_levels);
  for (size_t level = 0; level <= params.shape.tree_height; ++level) {
    std::vector<LevelPlaneDatabase> planes =
        pack_level_planes(params, source, level);
    std::vector<LevelPlaneM32> views;
    views.reserve(planes.size());
    for (const LevelPlaneDatabase &db : planes) {
      views.push_back(build_level_plane_m32(db, params.level_plans[level],
                                            runtime_scheme));
    }
    tree.m32.push_back(std::move(views));
    if (retain_canonical) tree.canonical.push_back(std::move(planes));
  }
  return tree;
}

PreprocessedTree m32_from_reference(ReferencePreprocessedTree reference,
                                    const TreeParams &params,
                                    const PirParams &runtime_scheme,
                                    bool retain_canonical) {
  require_scheme_binding(params, runtime_scheme);
  require(reference.tag == params.tag, "reference database PlanTag mismatch");
  PreprocessedTree tree;
  tree.tag = params.tag;
  tree.m32.reserve(reference.canonical.size());
  for (size_t level = 0; level < reference.canonical.size(); ++level) {
    std::vector<LevelPlaneM32> views;
    for (const LevelPlaneDatabase &db : reference.canonical[level]) {
      views.push_back(build_level_plane_m32(db, params.level_plans[level],
                                            runtime_scheme));
    }
    tree.m32.push_back(std::move(views));
  }
  if (retain_canonical) tree.canonical = std::move(reference.canonical);
  return tree;
}

void release_canonical(PreprocessedTree &db) {
  CanonicalTreeDatabase().swap(db.canonical);
}

size_t m32_database_bytes(const PreprocessedTree &db) {
  size_t bytes = 0;
  for (const auto &views : db.m32) {
    for (const LevelPlaneM32 &view : views) {
      bytes += (view.lo.size() + view.hi.size()) * sizeof(uint32_t);
    }
  }
  return bytes;
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

void rewrite_preprocessed_superrecord(PreprocessedTree &db, size_t level,
                                      size_t superrecord,
                                      const TreeParams &params,
                                      const PirParams &runtime_scheme,
                                      const EntrySource &source) {
  require_scheme_binding(params, runtime_scheme);
  require_m32_scheme(runtime_scheme);
  require(db.tag == params.tag, "database PlanTag mismatch");
  require(db.has_canonical(),
          "incremental update needs the retained canonical view");
  require_level(params, level);
  const CandidateShape &shape = params.shape;
  const LevelPlan &plan = params.level_plans[level];
  const size_t rho = shape.records_per_plaintext;
  const size_t degree = runtime_scheme.get_poly_degree();
  const CompositeRnsTables &crt = runtime_scheme.get_composite_rns();
  const uint64_t q = runtime_scheme.get_rns_mods()[0];
  const EncodedSuperRecord encoded =
      encode_superrecord_at(params, source, level, superrecord);
  const size_t p = superrecord / rho;
  const size_t u = superrecord % rho;
  const auto [row, col] = matrix_coordinates(p, plan);
  std::vector<uint64_t> ntt(degree);
  for (size_t z = 0; z < shape.plane_count; ++z) {
    std::vector<uint64_t> &pt = db.canonical[level][z].plaintexts[p];
    write_support(pt, encoded[z], u, rho);
    std::copy(pt.begin(), pt.end(), ntt.begin());
    utils::ntt_fwd(ntt.data(), degree, q);
    LevelPlaneM32 &view = db.m32[level][z];
    for (size_t coeff = 0; coeff < degree; ++coeff) {
      const size_t at = (coeff * view.rows + row) * view.cols + col;
      view.lo[at] = static_cast<uint32_t>(ntt[coeff] % crt.q1);
      view.hi[at] = static_cast<uint32_t>(ntt[coeff] % crt.q2);
    }
  }
}

}  // namespace tree_superrecord
