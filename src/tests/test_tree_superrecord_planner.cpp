#include "tree_superrecord_test_util.h"

#include <algorithm>
#include <bit>

using namespace tree_superrecord;
using tree_superrecord_test::make_input;
using tree_superrecord_test::pick_candidate;
using tree_superrecord_test::test_policy;
using tree_superrecord_test::throws_invalid;

// Scheme identity, policy validation, the finite (h, G, P, N0, b, h_q)
// enumeration with its pinned Bundle3 widths, the shared LevelPlan builder,
// projection-aware key heights, expansion-node counts, memory estimates,
// Pareto dominance, and candidate freezing (design sections 4, 5, 9, 11,
// 19-21).
void PirTest::test_tree_superrecord_planner() {
  print_func_name(__FUNCTION__);
  constexpr size_t n = DBConsts::PolyDegree;
  const size_t log_n = std::bit_width(n) - 1;
  PirParams scheme;
  const size_t c = tree_superrecord_test::payload_bits(scheme);

  // ---- scheme identity is the single compatibility source ----
  {
    const PirSchemeIdentity id = scheme.scheme_identity();
    require_test(id == PirParams().scheme_identity(),
                 "identity must be deterministic across instances");
    require_test(id.ring_degree == n && id.rns_moduli == scheme.get_rns_mods() &&
                     id.selector_gadget_rows == scheme.get_l() &&
                     id.completion_key_gadget_rows == scheme.get_l_key() &&
                     id.bv_ks_rows == DBConsts::L_KS &&
                     id.bv_ks_base_log2 == scheme.get_ct_mod_width() / DBConsts::L_KS + 1 &&
                     id.response_modulus == scheme.get_small_q() &&
                     id.composite_first_dim == scheme.get_composite_rns().enabled,
                 "identity fields mirror the scheme");
    const PirParams view = scheme.with_query_shape({7, 3, 8});
    require_test(scheme.scheme_compatible(view) && view.scheme_compatible(scheme),
                 "query-shape views stay scheme-compatible");
    require_test(view.scheme_identity() == id,
                 "layout changes do not alter the identity");
    const SchemeFingerprint a = make_scheme_fingerprint(scheme, c, log_n,
        EvalKeyRegistrationPolicy::FixedMaximumHeight);
    const SchemeFingerprint b = make_scheme_fingerprint(id, c, log_n,
        EvalKeyRegistrationPolicy::FixedMaximumHeight);
    require_test(a == b, "identity-first and convenience fingerprints agree");
    SchemeFingerprint other = a;
    other.payload_bits_per_coeff = c - 1;
    require_test(!(other == a), "payload width is part of the fingerprint");
    require_test(throws_invalid([&] {
                   (void)make_scheme_fingerprint(id, c, log_n + 1,
                       EvalKeyRegistrationPolicy::FixedMaximumHeight);
                 }),
                 "fingerprint height above log2 n must be rejected");
  }

  // ---- policy validation ----
  {
    PlannerInput ok = make_input(scheme, 256, size_t{1} << 10, test_policy());
    validate_planner_input(ok);
    PlannerInput bad = ok;
    bad.policy.expansion_heights = {6, 5};
    require_test(throws_invalid([&] { validate_planner_input(bad); }),
                 "unsorted expansion heights must be rejected");
    bad = ok;
    bad.policy.expansion_heights = {5, 5};
    require_test(throws_invalid([&] { validate_planner_input(bad); }),
                 "duplicate expansion heights must be rejected");
    bad = ok;
    bad.policy.alpha_alignments = {0, 1};
    require_test(throws_invalid([&] { validate_planner_input(bad); }),
                 "zero alignment must be rejected");
    bad = ok;
    bad.policy.max_query_ciphertexts = 2;
    require_test(throws_invalid([&] { validate_planner_input(bad); }),
                 "more than one packed query must be rejected");
    bad = ok;
    bad.leaf_count = 12;
    require_test(throws_invalid([&] { validate_planner_input(bad); }),
                 "non-power-of-two leaf count must be rejected");
    bad = ok;
    bad.payload_bits_per_coeff = std::bit_width(scheme.get_plain_mod());
    require_test(throws_invalid([&] { validate_planner_input(bad); }),
                 "payload width above t must be rejected");
    bad = ok;
    bad.policy.fixed_session_key_height = log_n + 1;
    require_test(throws_invalid([&] { validate_planner_input(bad); }),
                 "fixed key height above log2 n must be rejected");
  }

  // ---- expansion node counts and pyramid rows ----
  require_test(populated_expansion_node_count(480, 512) == 961 &&
                   populated_expansion_node_count(821, 1024) == 1651 &&
                   populated_expansion_node_count(25, 32) == 55,
               "populated expansion node pins");
  require_test(ragged_pyramid_row_lengths(342) ==
                   std::vector<size_t>{342, 171, 86, 43, 22, 11, 6, 3, 2, 1},
               "ragged pyramid rows for N0 = 342");
  require_test(ragged_pyramid_row_lengths(1) == std::vector<size_t>{1},
               "N0 = 1 pyramid is one row");
  require_test(conservative_scratch_rlwe_count(scheme.get_l()) ==
                   2 * scheme.get_l() + 2,
               "scratch liveness maximum is the selector completion row");

  // ---- Bundle3 golden enumeration at N = 2^24 (design section 9.2) ----
  if (c == 12) {
    PlannerPolicy policy = test_policy();
    policy.expansion_heights = {9, 10};
    policy.alpha_alignments = {1, 16};
    policy.max_bundle_arity = 8;
    const std::vector<Candidate> all =
        enumerate_candidates(make_input(scheme, 256, size_t{1} << 24, policy));
    require_test(!all.empty(), "the 256-bit profile must have candidates");
    const auto n0_of = [&](size_t b, size_t align) {
      const std::optional<Candidate> found = pick_candidate(all, 3, 64, b, align);
      require_test(found.has_value(), "Bundle3 (h=3, G=64) candidate must exist");
      return found->shape.N0;
    };
    require_test(n0_of(9, 1) == 342 && n0_of(9, 16) == 352 &&
                     n0_of(8, 1) == 683 && n0_of(8, 16) == 688,
                 "Bundle3 minimal aligned N0 pins: 342/352/683/688");
    const Candidate golden = *pick_candidate(all, 3, 64, 9, 1);
    const CandidateShape &s = golden.shape;
    require_test(s.plane_count == 1 && s.records_per_plaintext == 32 &&
                     s.record_index_bits == 5 && s.selector_bits == 23 &&
                     s.query_width == 342 + 23 * scheme.get_l() &&
                     s.expansion_height == 9 && s.expansion_capacity == 512 &&
                     s.response_ciphertexts == 1 &&
                     s.projection_depth_max == 5 && s.required_key_height == 9 &&
                     s.registered_key_height == log_n,
                 "Bundle3 shape fields");
    require_test(golden.metrics.slot_utilization == 1.0 &&
                     golden.metrics.padding_bits_per_bundle == 0,
                 "Bundle3 fills its plane exactly");
    // N0 = 688 query-server database estimate pin (design section 20.4).
    const Candidate wide = *pick_candidate(all, 3, 64, 8, 16);
    if (scheme.get_composite_rns().enabled) {
      require_test(wide.metrics.query_eval_server_database_bytes_estimate ==
                       11272192,
                   "N0 = 688 composite query-server database estimate");
    }
    require_test(golden.metrics.temporary_working_set_estimate > 0 &&
                     golden.metrics.optimized_bytes > 0 &&
                     golden.metrics.populated_expansion_nodes ==
                         populated_expansion_node_count(s.query_width, 512),
                 "working-set and expansion estimates are populated");
    // Level plans: leaf AlphaIndex 2^9 x 342 with fold bits [5, 14) and
    // rotation bits [0, 5); upper levels coarsen; every level covers Q.
    const std::vector<LevelPlan> plans = build_level_plans(s);
    require_test(plans.size() == 25, "one plan per level");
    const LevelPlan &leaf = plans.back();
    require_test(leaf.select_case == SelectCase::AlphaIndex &&
                     leaf.matrix_rows == 512 && leaf.matrix_cols == 342 &&
                     leaf.fold_bit_begin == 5 && leaf.fold_bit_count == 9 &&
                     leaf.rotation_bit_begin == 0 && leaf.rotation_bit_count == 5 &&
                     leaf.projection_depth == 5 &&
                     leaf.logical_plaintexts_per_plane == 174763,
                 "Bundle3 leaf plan");
    const LevelPlan &root = plans.front();
    require_test(root.select_case == SelectCase::Single &&
                     root.matrix_rows == 1 && root.matrix_cols == 1 &&
                     root.fold_bit_begin == 23 && root.fold_bit_count == 0 &&
                     root.rotation_bit_begin == 23 &&
                     root.rotation_bit_count == 0 && root.projection_depth == 0,
                 "root plan uses the empty-range sentinels");
    size_t outputs = 0, folds = 0;
    for (const LevelPlan &plan : plans) {
      require_test(plan.padded_plaintexts_per_plane >=
                       plan.logical_plaintexts_per_plane,
                   "every level covers its logical plaintexts");
      if (plan.shift <= s.beta_bits) {
        require_test(plan.select_case == SelectCase::AlphaIndex &&
                         plan.matrix_rows == (size_t{1} << (s.beta_bits - plan.shift)) &&
                         plan.matrix_cols == s.N0,
                     "k <= b levels are AlphaIndex");
      } else {
        require_test(plan.matrix_rows == 1 &&
                         plan.alpha_coarsen == std::min(plan.shift - s.beta_bits, s.alpha_height) &&
                         plan.matrix_cols == ragged_pyramid_row_lengths(s.N0)[plan.alpha_coarsen],
                     "k > b levels consume the matching ragged pyramid row");
      }
      require_test(plan.projection_depth ==
                       std::min(s.record_index_bits,
                                static_cast<size_t>(std::bit_width(plan.logical_superrecords - 1))),
                   "projection depth is min(r, bitwidth(S-1))");
      outputs += plan.matrix_rows;
      folds += plan.matrix_rows - 1;
    }
    require_test(outputs == folds + 25 &&
                     golden.metrics.first_dim_output_ciphertexts == outputs &&
                     golden.metrics.fold_cmux_count == folds,
                 "first-dimension outputs equal folds plus P*H");
    require_test(max_projection_depth(plans) == 5, "max projection depth");
    // A mutated plan vector or tag must be rejected by freezing/validation.
    TreeParams params = freeze_candidate(golden);
    require_test(params.tag.N0 == 342 && params.tag.scheme == golden.scheme &&
                     params.level_plans == plans,
                 "freeze copies shape, fingerprint, plans, and tag");
    TreeParams mutated = params;
    mutated.level_plans[3].matrix_cols += 1;
    require_test(throws_invalid([&] { validate_tree_params(mutated); }),
                 "plan drift must be rejected");
    mutated = params;
    mutated.tag.entry_bits = 255;
    require_test(throws_invalid([&] { validate_tree_params(mutated); }),
                 "tag drift must be rejected");
    // Pareto: the golden point is retained by the frontier, dominance ignores
    // predictions, and different contexts are incomparable.
    std::vector<Candidate> ranked = all;
    rank_by_prediction(ranked, CostCalibration{});
    const std::vector<Candidate> frontier = pareto_frontier(all);
    require_test(!frontier.empty() && frontier.size() <= all.size(),
                 "frontier is a nonempty subset");
    Candidate worse = golden;
    worse.metrics.physical_scan_bytes += 1;
    require_test(dominates(golden, worse) && !dominates(worse, golden) &&
                     !dominates(golden, golden),
                 "strictly larger scan is dominated");
    Candidate other_context = worse;
    other_context.shape.entry_bits = 255;
    require_test(!dominates(golden, other_context),
                 "different entry sizes are incomparable");
    Candidate faster = worse;
    faster.metrics.predicted_server_ms = 0;
    require_test(dominates(golden, faster),
                 "predicted time never enters dominance");
    BENCH_PRINT("E=256, N=2^24: " << all.size() << " candidates, frontier "
                << frontier.size() << ", best predicted: "
                << describe_candidate(ranked.front()));
  }

  // ---- finite h enumeration and its bounds (design section 19.4) ----
  {
    // N = 2^6 (H = 7), one response: for G = 64 the response cap gives
    // P_max = floor(32 / 7) = 4 and h_max = floor(4 * c * 64 / 256).
    PlannerPolicy policy = test_policy(1, 64);
    const std::vector<Candidate> all =
        enumerate_candidates(make_input(scheme, 256, size_t{1} << 6, policy));
    const size_t h_resp = (4 * c * 64) / 256;
    size_t max_h = 0;
    for (const Candidate &cand : all) {
      if (cand.shape.record_coeffs == 64) {
        max_h = std::max(max_h, cand.shape.bundle_arity);
        require_test(cand.shape.response_ciphertexts == 1,
                     "every enumerated candidate meets the response cap");
      }
    }
    require_test(max_h == h_resp, "h_max is the response-derived bound");
    // h = leaf_count is a zero-selector candidate; h = N + 1 never appears.
    PlannerPolicy roomy = test_policy(64, 128);
    const std::vector<Candidate> tiny =
        enumerate_candidates(make_input(scheme, 13, 8, roomy));
    bool saw_all_in_one = false;
    for (const Candidate &cand : tiny) {
      require_test(cand.shape.bundle_arity <= 8, "h never exceeds N");
      if (cand.shape.bundle_arity == 8) {
        saw_all_in_one = true;
        require_test(cand.shape.selector_bits == 0 && cand.shape.N0 == 1 &&
                         cand.shape.query_width == 1 && cand.shape.beta_bits == 0,
                     "h = N has no selectors and a single alpha slot");
        // Every level is a 1 x 1 matrix (the leaf as a degenerate AlphaIndex,
        // the rest Single) with empty fold and rotation ranges.
        const std::vector<LevelPlan> plans = build_level_plans(cand.shape);
        for (const LevelPlan &plan : plans) {
          require_test(plan.fold_bit_begin == 0 && plan.fold_bit_count == 0 &&
                           plan.rotation_bit_begin == 0 &&
                           plan.rotation_bit_count == 0 &&
                           plan.matrix_rows == 1 && plan.matrix_cols == 1 &&
                           (plan.select_case == SelectCase::Single ||
                            (plan.shift == 0 &&
                             plan.select_case == SelectCase::AlphaIndex)),
                       "zero-selector plans use the 0/0 sentinel");
        }
      }
    }
    require_test(saw_all_in_one, "h = leaf_count must be enumerable");
    // A finite retrieved-bundle cap clamps h by floor(cap / E).
    PlannerPolicy capped = test_policy(1, 64);
    capped.max_retrieved_bundle_bits_per_level = 512;
    for (const Candidate &cand : enumerate_candidates(
             make_input(scheme, 256, size_t{1} << 6, capped))) {
      require_test(cand.shape.bundle_arity <= 2, "bundle cap clamps h");
    }
  }

  // ---- projection-aware key height (design section 4.2) ----
  if (log_n == 11) {
    // G = 1 at L = 24 needs projection depth 11 even though the packed query
    // fits height 9: fixed height 10 rejects it, candidate-specific accepts.
    PlannerPolicy fixed10 = test_policy(1, 8);
    fixed10.expansion_heights = {9, 10};
    fixed10.fixed_session_key_height = 10;
    const std::vector<Candidate> fixed =
        enumerate_candidates(make_input(scheme, 256, size_t{1} << 24, fixed10));
    bool saw_g1 = false, saw_g2 = false;
    for (const Candidate &cand : fixed) {
      if (cand.shape.record_coeffs == 1) saw_g1 = true;
      if (cand.shape.record_coeffs == 2 && cand.shape.projection_depth_max == 10) {
        saw_g2 = true;
      }
      require_test(cand.shape.required_key_height <= 10,
                   "fixed height 10 admits only depth <= 10");
    }
    require_test(!saw_g1 && saw_g2,
                 "G=1 (depth 11) rejected, G=2 (depth 10) accepted at height 10");
    PlannerPolicy specific = fixed10;
    specific.eval_key_registration =
        EvalKeyRegistrationPolicy::CandidateSpecificHeight;
    bool saw_g1_specific = false;
    for (const Candidate &cand : enumerate_candidates(
             make_input(scheme, 256, size_t{1} << 24, specific))) {
      if (cand.shape.record_coeffs == 1) {
        saw_g1_specific = true;
        require_test(cand.shape.required_key_height == 11 &&
                         cand.shape.registered_key_height == 11 &&
                         cand.scheme.registered_session_key_height == 11,
                     "candidate-specific registration uses height 11");
      }
    }
    require_test(saw_g1_specific, "G=1 exists under candidate-specific keys");
  }

  // ---- required policy examples (design section 29) ----
  {
    const std::vector<Candidate> two = enumerate_candidates(
        make_input(scheme, 512, size_t{1} << 24, test_policy(2, 8)));
    const std::optional<Candidate> e512 = pick_candidate(two, 3, 128, 8);
    require_test(e512.has_value() && e512->shape.response_ciphertexts == 2,
                 "E=512, h=3, G=128 uses two responses at H=25");
    const std::vector<Candidate> one = enumerate_candidates(
        make_input(scheme, 512, size_t{1} << 24, test_policy(1, 8)));
    bool absent = true;
    for (const Candidate &cand : one) {
      if (cand.shape.bundle_arity == 3 && cand.shape.record_coeffs == 128) {
        absent = false;
      }
    }
    require_test(absent, "the 512-bit point is absent under one response");
    const std::vector<Candidate> large = enumerate_candidates(
        make_input(scheme, 30000, size_t{1} << 4, test_policy(16, 8)));
    const std::optional<Candidate> big = pick_candidate(large, 1, n, 4);
    require_test(big.has_value() && big->shape.plane_count == (30000 + c * n - 1) / (c * n) &&
                     big->shape.response_ciphertexts ==
                         big->shape.plane_count * 5 / (n / n),
                 "E=30000, h=1, G=n gives P planes and P*H responses");
  }
}
