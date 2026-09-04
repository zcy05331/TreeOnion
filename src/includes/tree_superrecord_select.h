#pragma once

#include "rlwe.h"
#include "server.h"
#include "tree_profile.h"
#include "tree_superrecord_codec.h"
#include "tree_superrecord_index.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

// Super-record databases, the ragged alpha pyramid, and SelectLevel for the
// generic path (design specification, sections 10, 11, 12, 15, 17).
//
// Level l stores S = ceil(2^l / h) super-records in Q = ceil(S / rho)
// plaintexts per plane. Super-record x sits in plaintext p = floor(x / rho)
// at record position u = x mod rho; plane z's digit d is coefficient
// u + d * rho. Plaintexts are linearised as plaintext = col * rows + row, and
// the coefficient-major m32 address is (coeff * rows + row) * cols + col,
// identical for the reference evaluator, the update path, and the kernel.

// Per-query CRT-limb split of the NTT-form pyramid (tree_select.h); only the
// implementation files pull in the legacy tree headers.
struct AlphaPyramidM32;

namespace tree_superrecord {

// pyramid[row][j]; row 0 is the expanded one-hot, the top row is Enc(1).
using AlphaPyramid = std::vector<std::vector<RlweCt>>;

// Application entry for node (level, node_index); E bits each.
using EntrySource = std::function<EntryBits(size_t level, size_t node_index)>;

struct LevelPlaneDatabase {
  size_t level = 0;
  size_t plane_index = 0;
  size_t logical_superrecords = 0;
  size_t logical_plaintexts = 0;
  size_t matrix_rows = 0;
  size_t matrix_cols = 0;
  // padded_plaintexts_per_plane polynomials of n coefficients in [0, t).
  std::vector<std::vector<uint64_t>> plaintexts;
};

// canonical[level][plane]
using CanonicalTreeDatabase = std::vector<std::vector<LevelPlaneDatabase>>;

struct ReferencePreprocessedTree {
  PlanTag tag;
  CanonicalTreeDatabase canonical;
};

// Encodes the h entries of super-record x at `level` (tail lanes zero).
EncodedSuperRecord encode_superrecord_at(const TreeParams &params,
                                         const EntrySource &source,
                                         size_t level, size_t superrecord);
std::vector<LevelPlaneDatabase> pack_level_planes(const TreeParams &params,
                                                  const EntrySource &source,
                                                  size_t level);
ReferencePreprocessedTree preprocess_reference(const TreeParams &params,
                                               const PirParams &runtime_scheme,
                                               const EntrySource &source);
// Clears and rewrites only the residue support of one super-record.
void rewrite_reference_superrecord(ReferencePreprocessedTree &db, size_t level,
                                   size_t superrecord, const TreeParams &params,
                                   const PirParams &runtime_scheme,
                                   const EntrySource &source);
size_t canonical_database_bytes(const CanonicalTreeDatabase &canonical);

// Ragged carry-up pyramid: row c has ceil(N0 / 2^c) ciphertexts, the encrypted
// one at floor(alpha / 2^c); an unpaired last element is carried by copy.
// Rows 0..alpha_height; the top row is the constant Enc(1).
AlphaPyramid build_ragged_alpha_pyramid(std::span<const RlweCt> alpha,
                                        const TreeParams &params,
                                        const PirParams &runtime_scheme);
struct PyramidCounters {
  size_t ciphertexts = 0;
  size_t additions = 0;
  size_t carries = 0;
};
PyramidCounters ragged_pyramid_counters(size_t N0);
// Pyramid row and selection width consumed by a plan.
std::pair<size_t, size_t> plan_pyramid_row_and_cols(const LevelPlan &plan);

// MSB-first CMux fold over [fold_bit_begin, fold_bit_begin + count).
RlweCt fold_candidates(std::vector<RlweCt> candidates, const LevelPlan &plan,
                       std::span<GSWCt> sigma_selectors, PirServer &mux);

// Reference SelectLevel: plaintext-ciphertext products in the exact matrix
// orientation, then the fold. Returns Enc(D[p_l]) in coefficient form.
RlweCt select_level_reference(const LevelPlaneDatabase &db,
                              const LevelPlan &plan,
                              const AlphaPyramid &pyramid,
                              std::span<GSWCt> sigma_selectors, PirServer &mux,
                              const TreeParams &params,
                              const PirParams &runtime_scheme,
                              TreeServerTiming *timing = nullptr);

// ---- composite m32 view ----
struct LevelPlaneM32 {
  size_t level = 0;
  size_t plane_index = 0;
  size_t rows = 0;
  size_t cols = 0;
  std::vector<uint32_t> lo, hi;  // n * rows * cols each, coefficient-major
};

struct PreprocessedTree {
  PlanTag tag;
  CanonicalTreeDatabase canonical;  // empty once released
  std::vector<std::vector<LevelPlaneM32>> m32;  // [level][plane]
  bool has_canonical() const { return !canonical.empty(); }
};

// K == 1, composite split enabled, q1 and q2 fit uint32_t, q1 * q2 == q.
void require_m32_scheme(const PirParams &runtime_scheme);
LevelPlaneM32 build_level_plane_m32(const LevelPlaneDatabase &db,
                                    const LevelPlan &plan,
                                    const PirParams &runtime_scheme);
// Builds level by level; with retain_canonical == false only one level's
// canonical planes are alive at a time.
PreprocessedTree preprocess_m32(const TreeParams &params,
                                const PirParams &runtime_scheme,
                                const EntrySource &source,
                                bool retain_canonical = true);
PreprocessedTree m32_from_reference(ReferencePreprocessedTree reference,
                                    const TreeParams &params,
                                    const PirParams &runtime_scheme,
                                    bool retain_canonical = true);
void release_canonical(PreprocessedTree &db);
size_t m32_database_bytes(const PreprocessedTree &db);

// Same ring expression as select_level_reference on the m32 view: one
// 32x32->64 kernel pass per limb, CRT compose, one INTT per candidate row,
// then the fold. Bit-for-bit equal to the reference.
RlweCt select_level_m32(const LevelPlaneM32 &db, const LevelPlan &plan,
                        const ::AlphaPyramidM32 &pyramid_m32,
                        std::span<GSWCt> sigma_selectors, PirServer &mux,
                        const TreeParams &params,
                        const PirParams &runtime_scheme,
                        TreeServerTiming *timing = nullptr);

// Requires a retained canonical view; patches canonical and both limbs.
void rewrite_preprocessed_superrecord(PreprocessedTree &db, size_t level,
                                      size_t superrecord,
                                      const TreeParams &params,
                                      const PirParams &runtime_scheme,
                                      const EntrySource &source);

}  // namespace tree_superrecord
