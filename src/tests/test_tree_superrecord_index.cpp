#include "tree_superrecord_test_util.h"

#include "tree_superrecord_select.h"

#include <algorithm>
#include <set>

using namespace tree_superrecord;
using namespace tree_superrecord_test;

// Exhaustive bundled-index algebra, real bundle slots at every level, the
// exact first-dimension orientation of the preprocessed databases, pooled
// response placement, the planner's storage estimates against the bytes the
// databases actually allocate, the steady-state (canonical released) m32 path
// against the retained one, and incremental updates for canonical and m32
// views (design sections 8, 10, 14, 15).
void PirTest::test_tree_superrecord_index() {
  print_func_name(__FUNCTION__);
  PirParams scheme;
  const bool composite = scheme.get_composite_rns().enabled;
  const size_t n = scheme.get_poly_degree();
  const size_t c = payload_bits(scheme);
  // Retain two-plane fixtures: rho = 8 spans two responses, while rho = 1
  // spans ten. The original n = 2048 entry sizes stay unchanged.
  const size_t two_chunk_g = n / 8;
  const size_t two_chunk_entry_bits = std::max(size_t{4096}, c * two_chunk_g + 1);
  const size_t large_entry_bits = std::max(size_t{30000}, c * n + 1);

  struct Shape {
    size_t E, N, h, G, b, max_responses;
    size_t expected_chunks;
  };
  const std::vector<Shape> shapes = {
      {13, 64, 3, 2, 0, 1, 1},       // N0 = 1, S_L = 22
      {256, 256, 3, 64, 1, 1, 1},    // Bundle3 with one beta bit
      {256, n / 2, 3, 64, 2, 1, 1},   // Bundle3, rows 4 x N0 3 (ragged pyramid)
      {13, 8, 8, 4, 0, 4, 1},        // all leaves in one bundle: zero selectors
      {40, 128, 5, 8, 0, 1, 1},      // non-power-of-two h with S_L = 26
      {two_chunk_entry_bits, 16, 1, two_chunk_g, 0, 2, 2},   // P = 2, H = 5, rho = 8: two chunks
      {large_entry_bits, 16, 1, n, 2, 16, 10},  // one entry above a full plaintext
  };
  for (const Shape &shape : shapes) {
    // The candidate itself is kept so its metrics can be checked against the
    // databases built from the frozen plan.
    const std::vector<Candidate> all = enumerate_candidates(make_input(
        scheme, shape.E, shape.N, test_policy(shape.max_responses, 128)));
    const std::optional<Candidate> picked =
        pick_candidate(all, shape.h, shape.G, shape.b);
    require_test(picked.has_value(), "test shape is not a feasible candidate");
    const Candidate &cand = *picked;
    const TreeParams params = freeze_candidate(cand);
    const CandidateShape &s = params.shape;
    const size_t L = s.tree_height;
    const size_t r = s.record_index_bits;
    const size_t rho = s.records_per_plaintext;
    require_test(s.response_ciphertexts == shape.expected_chunks,
                 "response chunk count for the shape");
    if (shape.expected_chunks > 1) {
      require_test(s.plane_count == 2, "multi-response fixtures retain two planes");
    }

    // ---- index algebra for every leaf and level ----
    for (size_t leaf = 0; leaf < shape.N; ++leaf) {
      const ClientCoordinates coords = client_index(leaf, params);
      const size_t sigma = leaf / shape.h;
      size_t sigma_from_bits = 0;
      for (size_t v = 0; v < s.selector_bits; ++v) {
        sigma_from_bits |= size_t{coords.sigma_bits_le[v]} << v;
      }
      require_test(sigma_from_bits == sigma && coords.alpha == (sigma >> r) >> s.beta_bits &&
                       coords.alpha < s.N0,
                   "client coordinates encode sigma and alpha");
      for (size_t level = 0; level <= L; ++level) {
        const size_t k = L - level;
        const size_t node = leaf >> k;
        const LevelOracle oracle = level_oracle_for_test(leaf, level, params);
        require_test(oracle.node_index == node &&
                         oracle.superrecord_index == node / shape.h &&
                         oracle.superrecord_index == (sigma >> k) &&
                         oracle.lane_index == node % shape.h &&
                         oracle.plaintext_index == (sigma >> (k + r)) &&
                         oracle.record_position == ((sigma >> k) & (rho - 1)),
                     "bundled prefix identities");
        const LevelPlan &plan = params.level_plans[level];
        // Server-side derivation from the public plan and the shared bits.
        size_t expected_plaintext = 0;
        if (plan.select_case == SelectCase::AlphaIndex) {
          const size_t delta =
              (sigma >> (r + k)) & ((size_t{1} << plan.fold_bit_count) - 1);
          expected_plaintext = matrix_plaintext_index(delta, coords.alpha, plan);
        } else {
          expected_plaintext = coords.alpha >> plan.alpha_coarsen;
        }
        require_test(expected_plaintext == oracle.plaintext_index,
                     "plan-derived plaintext index matches the oracle");
        const size_t u_from_bits =
            (sigma >> k) & ((size_t{1} << plan.rotation_bit_count) - 1);
        require_test(u_from_bits == oracle.record_position,
                     "rotation bits [k, k+d) encode the record position");
        require_test(oracle.plaintext_index < plan.padded_plaintexts_per_plane,
                     "target plaintext lies inside the level matrix");
        // Real slots of the bundle holding this ancestor: the nodes at this
        // level that share its bundle. Root levels (2^level < h) and interior
        // tail bundles exercise the min(h, level_nodes - bundle_start) clamp
        // that feeds the benchmark's real_non_target_neighbour_slots figure.
        size_t sharing = 0;
        for (size_t v = 0; v < (size_t{1} << level); ++v) {
          sharing += (v / shape.h) == (node / shape.h);
        }
        require_test(real_bundle_slots(leaf, level, params) == sharing,
                     "real bundle slots count the nodes sharing the bundle");
      }
    }

    // ---- response placement ----
    std::set<std::pair<size_t, size_t>> placements;
    for (size_t level = 0; level <= L; ++level) {
      for (size_t plane = 0; plane < s.plane_count; ++plane) {
        const PlanePlacement at = response_placement(level, plane, params);
        require_test(placements.insert({at.response_chunk, at.residue_offset}).second,
                     "placements must be unique");
        require_test(at.residue_offset + (s.record_coeffs - 1) * rho <= s.ring_degree - 1,
                     "placement never writes beyond coefficient n-1");
      }
    }
    require_test(placements.size() == s.plane_count * s.path_levels,
                 "one placement per (level, plane)");

    // ---- preprocessing orientation ----
    const EntrySource source = [&](size_t level, size_t node) {
      return synthetic_entry(shape.E, level, node);
    };
    const ReferencePreprocessedTree reference =
        preprocess_reference(params, scheme, source);
    bool orientation_observable = false;
    for (size_t level = 0; level <= L; ++level) {
      const LevelPlan &plan = params.level_plans[level];
      for (size_t x = 0; x < plan.logical_superrecords; ++x) {
        const EncodedSuperRecord encoded =
            encode_superrecord_at(params, source, level, x);
        const size_t p = x / rho;
        const size_t u = x % rho;
        const auto [row, col] = matrix_coordinates(p, plan);
        require_test(matrix_plaintext_index(row, col, plan) == p,
                     "matrix coordinates invert the linear index");
        for (size_t z = 0; z < s.plane_count; ++z) {
          const std::vector<uint64_t> &pt =
              reference.canonical[level][z].plaintexts[p];
          for (size_t d = 0; d < s.record_coeffs; ++d) {
            require_test(pt[u + d * rho] == encoded[z][d],
                         "digit lands at record_position + d * rho");
          }
        }
        if (plan.matrix_rows > 1 && plan.matrix_cols > 1) {
          const size_t transposed = row * plan.matrix_cols + col;
          if (transposed != p && transposed < plan.padded_plaintexts_per_plane) {
            orientation_observable = true;
            require_test(reference.canonical[level][0].plaintexts[transposed] !=
                             reference.canonical[level][0].plaintexts[p],
                         "row*cols+col addresses a different plaintext");
          }
        }
      }
    }
    if (shape.N == n / 2) {
      require_test(orientation_observable,
                   "the 4 x 3 leaf matrix must expose the orientation");
    }
    // The planner's canonical storage estimate is the bytes the canonical
    // database actually allocates.
    require_test(canonical_database_bytes(reference.canonical) ==
                     cand.metrics.canonical_bytes,
                 "planner canonical_bytes equals the allocated canonical database");
    // Tail bundle: the last bundle of a level whose width is not a multiple
    // of h has fewer real slots than h.
    if (shape.h > 1 && shape.h < shape.N) {
      require_test(real_bundle_slots(shape.N - 1, L, params) ==
                       std::min(shape.h, shape.N - ((shape.N - 1) / shape.h) * shape.h),
                   "real slots of the tail bundle");
    }

    // ---- incremental canonical update equals a rebuild ----
    const size_t target_level = L;
    const size_t target_node = (shape.N - 1) / 2;
    const EntrySource updated = [&](size_t level, size_t node) {
      return synthetic_entry(shape.E, level, node,
                             level == target_level && node == target_node ? 1 : 0);
    };
    ReferencePreprocessedTree incremental = reference;
    rewrite_reference_superrecord(incremental, target_level,
                                  target_node / shape.h, params, scheme, updated);
    const ReferencePreprocessedTree rebuilt =
        preprocess_reference(params, scheme, updated);
    for (size_t level = 0; level <= L; ++level) {
      for (size_t z = 0; z < s.plane_count; ++z) {
        require_test(incremental.canonical[level][z].plaintexts ==
                         rebuilt.canonical[level][z].plaintexts,
                     "incremental canonical state equals a full rebuild");
        if (level != target_level) {
          require_test(incremental.canonical[level][z].plaintexts ==
                           reference.canonical[level][z].plaintexts,
                       "other levels are untouched");
        }
      }
    }
    bool some_plane_changed = false;
    for (size_t z = 0; z < s.plane_count; ++z) {
      if (incremental.canonical[target_level][z].plaintexts !=
          reference.canonical[target_level][z].plaintexts) {
        some_plane_changed = true;
      }
    }
    require_test(some_plane_changed, "at least one plane changes");

    // ---- incremental m32 update equals a rebuild ----
    if (composite) {
      PirClient client(scheme);
      PirServer server(query_expansion_params(params, scheme));
      const TaggedTreeSessionKeys session =
          create_tagged_tree_session_keys(client, params, scheme);
      set_tree_client_session_keys(server, params, scheme, session);
      const CandidateNoiseResult noise =
          passing_target_noise_result(client, params, scheme, server, session,
                                      source, shape.N - 1);
      PreprocessedTree m32 =
          m32_from_reference(reference, params, scheme, noise, true);
      // The benchmark reports memory.database_bytes from the steady-state
      // path (retain_canonical == false) next to the planner's
      // optimized_bytes; both must be the bytes the m32 limbs allocate, and
      // the steady-state limbs must equal the retained path's limb for limb.
      require_test(m32_database_bytes(m32) == cand.metrics.optimized_bytes &&
                       cand.metrics.optimized_bytes ==
                           cand.metrics.physical_scan_bytes,
                   "planner optimized_bytes equals the allocated m32 limbs");
      const PreprocessedTree steady =
          preprocess_m32(params, scheme, noise, source, false);
      require_test(!steady.has_canonical() &&
                       steady.m32.size() == m32.m32.size() &&
                       m32_database_bytes(steady) == m32_database_bytes(m32),
                   "steady-state preprocessing releases canonical and allocates the same limbs");
      for (size_t level = 0; level <= L; ++level) {
        require_test(steady.m32[level].size() == s.plane_count,
                     "steady-state m32 has one view per plane");
        for (size_t z = 0; z < s.plane_count; ++z) {
          const LevelPlaneM32 &view = steady.m32[level][z];
          require_test(view.rows == params.level_plans[level].matrix_rows &&
                           view.cols == params.level_plans[level].matrix_cols &&
                           view.lo == m32.m32[level][z].lo &&
                           view.hi == m32.m32[level][z].hi,
                       "steady-state limbs equal the retained path limb for limb");
        }
      }
      rewrite_preprocessed_superrecord(m32, target_level, target_node / shape.h,
                                       params, scheme, updated);
      const PreprocessedTree fresh =
          preprocess_m32(params, scheme, noise, updated, true);
      for (size_t level = 0; level <= L; ++level) {
        for (size_t z = 0; z < s.plane_count; ++z) {
          require_test(m32.m32[level][z].lo == fresh.m32[level][z].lo &&
                           m32.m32[level][z].hi == fresh.m32[level][z].hi &&
                           m32.canonical[level][z].plaintexts ==
                               fresh.canonical[level][z].plaintexts,
                       "incremental m32 state equals a full rebuild");
        }
      }
      PreprocessedTree released = fresh;
      release_canonical(released);
      require_test(!released.has_canonical() &&
                       throws_invalid([&] {
                         rewrite_preprocessed_superrecord(
                             released, target_level, 0, params, scheme, updated);
                       }),
                   "steady-state objects reject updates");
    }
    BENCH_PRINT("shape E=" << shape.E << " N=" << shape.N << " h=" << shape.h
                << " G=" << shape.G << " b=" << shape.b << ": "
                << describe_candidate(Candidate{s, {}, params.scheme}));
  }
}
