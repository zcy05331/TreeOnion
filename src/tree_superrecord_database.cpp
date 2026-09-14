#include "tree_superrecord_select.h"

#include "tree_superrecord_noise.h"
#include "utils.h"

#include <algorithm>
#include <limits>
#include <utility>

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

PreprocessedTree build_m32_from_reference(ReferencePreprocessedTree reference,
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
                                const CandidateNoiseResult &noise_result,
                                const EntrySource &source,
                                bool retain_canonical) {
  require_scheme_binding(params, runtime_scheme);
  require_m32_scheme(runtime_scheme);
  require_trusted_target_noise_result(noise_result, params);
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
                                    const CandidateNoiseResult &noise_result,
                                    bool retain_canonical) {
  require_trusted_target_noise_result(noise_result, params);
  PreprocessedTree tree = build_m32_from_reference(
      std::move(reference), params, runtime_scheme, retain_canonical);
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
