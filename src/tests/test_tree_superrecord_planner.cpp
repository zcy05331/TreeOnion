#include "tree_superrecord_test_util.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>

using namespace tree_superrecord;
using tree_superrecord_test::make_input;
using tree_superrecord_test::pick_candidate;
using tree_superrecord_test::test_policy;
using tree_superrecord_test::throws_invalid;

// Scheme identity, policy validation, the finite (h, G, P, N0, b, h_q)
// enumeration with its pinned Bundle3 widths, the shared LevelPlan builder,
// projection-aware key heights, expansion-node counts, memory estimates and
// caps, Pareto dominance, exact-plan deduplication, mandatory controls,
// measured-stage calibration, candidate freezing, and a whole-enumeration
// freeze/index sweep (design sections 4, 5, 9, 11, 19-21).
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
                     id.bv_ks_base_log2 == bvks::base_log2_for_scheme(scheme) &&
                     id.bv_ks_low_bits == bvks::low_bits_for_scheme(scheme) &&
                     id.selector_low_bits == scheme.get_ep_low_bits() &&
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
    const PlannerInput input =
        make_input(scheme, 256, size_t{1} << 24, policy);
    const std::vector<Candidate> all =
        enumerate_candidates(input);
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
      const Candidate aligned = *pick_candidate(all, 3, 64, 9, 16);
      require_test(aligned.metrics.alpha_pyramid_ciphertexts == 705 &&
                       aligned.metrics.alpha_ntt_ciphertexts == 352,
                   "NTT-domain pyramid transforms only its 352 bottom ciphertexts");
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
    ParetoContext context;
    context.avx512 = {true, true};
    std::vector<Candidate> ranked = all;
    rank_by_prediction(ranked, CostCalibration{}, context);
    const std::vector<Candidate> frontier = pareto_frontier(all, policy, context);
    require_test(!frontier.empty() && frontier.size() <= all.size(),
                 "frontier is a nonempty subset");
    Candidate worse = golden;
    worse.metrics.physical_scan_bytes += 1;
    require_test(dominates(golden, worse, policy, context) &&
                     !dominates(worse, golden, policy, context) &&
                     !dominates(golden, golden, policy, context),
                 "strictly larger scan is dominated");
    Candidate other_context = worse;
    other_context.shape.entry_bits = 255;
    require_test(!dominates(golden, other_context, policy, context),
                 "different entry sizes are incomparable");
    other_context = worse;
    other_context.scheme.identity.plaintext_modulus += 2;
    require_test(!dominates(golden, other_context, policy, context),
                 "different scheme identities are incomparable");
    other_context = worse;
    other_context.scheme.payload_bits_per_coeff = c - 1;
    require_test(!dominates(golden, other_context, policy, context),
                 "different payload widths are incomparable");
    other_context = worse;
    other_context.scheme.eval_key_registration =
        EvalKeyRegistrationPolicy::CandidateSpecificHeight;
    require_test(!dominates(golden, other_context, policy, context),
                 "different key-registration policies are incomparable");
    PlannerPolicy specific_policy = policy;
    specific_policy.eval_key_registration =
        EvalKeyRegistrationPolicy::CandidateSpecificHeight;
    Candidate shorter_keys = golden;
    Candidate taller_keys = worse;
    shorter_keys.scheme.eval_key_registration =
        EvalKeyRegistrationPolicy::CandidateSpecificHeight;
    taller_keys.scheme.eval_key_registration =
        EvalKeyRegistrationPolicy::CandidateSpecificHeight;
    shorter_keys.scheme.registered_session_key_height = s.required_key_height;
    taller_keys.scheme.registered_session_key_height = log_n;
    taller_keys.metrics.modeled_per_client_eval_key_bytes += 1;
    taller_keys.metrics.eval_key_coefficient_payload_bytes_estimate += 1;
    require_test(dominates(shorter_keys, taller_keys, specific_policy, context),
                 "candidate-specific registered heights remain comparable");
    Candidate faster = worse;
    faster.metrics.predicted_server_ms = 0;
    require_test(dominates(golden, faster, policy, context),
                 "predicted time never enters dominance");

    // ---- dispatch-aware costs, comparability, and exact sweep controls ----
    const std::vector<LevelPlan> golden_plans = build_level_plans(golden.shape);
    require_test(first_dim_dispatch_signature(golden, context).limb_dispatch.size() ==
                     golden.shape.plane_count * golden.shape.path_levels * 2,
                 "dispatch signature records both q1 and q2 for every level/plane");

    CostCalibration calibrated;
    calibrated.expand_intercept_ms = 1.0;
    calibrated.expand_per_populated_node_ms = 0.0;
    calibrated.expand_per_level_ms = 0.0;
    calibrated.to_rgsw_per_selector_ms = 0.0;
    calibrated.alpha_pyramid_add_ms = 0.0;
    calibrated.alpha_carry_copy_ms = 0.0;
    calibrated.alpha_ntt_per_ciphertext_ms = 0.0;
    calibrated.alpha_split_per_u32_output_ms = 0.0;
    calibrated.default_scan_bandwidth_bytes_per_ms = 10.0;
    calibrated.first_dim_output_ciphertext_ms = 0.0;
    calibrated.cmux_ms = 0.0;
    calibrated.substitution_ms = 0.0;
    calibrated.path_item_pack_ms = 0.0;
    calibrated.modulus_switch_ms = 0.0;

    for (const LevelPlan &plan : golden_plans) {
      FirstDimShapeKey q1_key;
      q1_key.matrix_rows = plan.matrix_rows;
      q1_key.matrix_cols = plan.matrix_cols;
      q1_key.cols_mod_simd_width = plan.matrix_cols % 16;
      q1_key.limb_modulus = scheme.get_composite_rns().q1;
      q1_key.dispatch =
          select_matmul32_dispatch(plan.matrix_cols, q1_key.limb_modulus,
                                   context.avx512);
      FirstDimShapeKey q2_key = q1_key;
      q2_key.limb_modulus = scheme.get_composite_rns().q2;
      q2_key.dispatch =
          select_matmul32_dispatch(plan.matrix_cols, q2_key.limb_modulus,
                                   context.avx512);
      const double q1_bw = 100.0 + static_cast<double>(plan.level);
      const double q2_bw = 200.0 + static_cast<double>(plan.level);
      calibrated.scan_bandwidth_bytes_per_ms[q1_key] = q1_bw;
      calibrated.scan_bandwidth_bytes_per_ms[q2_key] = q2_bw;
    }
    double expected_scan_ms = 0.0;
    for (const LevelPlan &plan : golden_plans) {
      FirstDimShapeKey q1_key;
      q1_key.matrix_rows = plan.matrix_rows;
      q1_key.matrix_cols = plan.matrix_cols;
      q1_key.cols_mod_simd_width = plan.matrix_cols % 16;
      q1_key.limb_modulus = scheme.get_composite_rns().q1;
      q1_key.dispatch =
          select_matmul32_dispatch(plan.matrix_cols, q1_key.limb_modulus,
                                   context.avx512);
      FirstDimShapeKey q2_key = q1_key;
      q2_key.limb_modulus = scheme.get_composite_rns().q2;
      q2_key.dispatch =
          select_matmul32_dispatch(plan.matrix_cols, q2_key.limb_modulus,
                                   context.avx512);
      const double q1_bw = calibrated.scan_bandwidth_bytes_per_ms.at(q1_key);
      const double q2_bw = calibrated.scan_bandwidth_bytes_per_ms.at(q2_key);
      const double one_limb_bytes = static_cast<double>(
          plan.matrix_rows * plan.matrix_cols * golden.shape.ring_degree *
          sizeof(uint32_t));
      expected_scan_ms += static_cast<double>(golden.shape.plane_count) *
                          one_limb_bytes / q1_bw;
      expected_scan_ms += static_cast<double>(golden.shape.plane_count) *
                          one_limb_bytes / q2_bw;
    }
    const double predicted = predict_server_ms(golden, calibrated, context);
    require_test(std::fabs(predicted - (1.0 + expected_scan_ms)) < 1e-6,
                 "prediction sums q1/q2 scan costs with actual level shapes");
    Candidate relabeled = golden;
    relabeled.shape.alpha_alignment = 16;
    require_test(predict_server_ms(relabeled, calibrated, context) == predicted,
                 "same physical shape with a different alignment label has the same prediction");

    Candidate scalar = worse;
    scalar.shape.N0 = 1024;
    scalar.shape.alpha_height = std::bit_width(scalar.shape.N0 - 1);
    scalar.shape.query_width = scalar.shape.N0 + scalar.shape.selector_bits * scheme.get_l();
    scalar.metrics.physical_scan_bytes = golden.metrics.physical_scan_bytes;
    scalar.metrics.scalar_u128_required_scan_bytes = sizeof(uint32_t);
    scalar.metrics.optimized_steady_state_bytes_estimate =
        golden.metrics.optimized_steady_state_bytes_estimate + 1;
    require_test(!dominates(golden, scalar, policy, context) &&
                     !dominates(scalar, golden, policy, context),
                 "different dispatch profiles are incomparable at Pareto time");
    const std::vector<LevelPlan> scalar_plans = build_level_plans(scalar.shape);
    const LevelPlan &scalar_leaf = scalar_plans.back();
    FirstDimShapeKey scalar_leaf_key;
    scalar_leaf_key.matrix_rows = scalar_leaf.matrix_rows;
    scalar_leaf_key.matrix_cols = scalar_leaf.matrix_cols;
    scalar_leaf_key.cols_mod_simd_width = scalar_leaf.matrix_cols % 16;
    scalar_leaf_key.dispatch = MatMul32Dispatch::ScalarU128;
    scalar_leaf_key.limb_modulus = scheme.get_composite_rns().q1;
    CostCalibration scalar_cal = calibrated;
    scalar_cal.scan_bandwidth_bytes_per_ms[scalar_leaf_key] = 1.0;
    require_test(predict_server_ms(scalar, scalar_cal, context) >
                     predict_server_ms(scalar, calibrated, context),
                 "unsafe limb uses ScalarU128 calibration");

    std::vector<Candidate> equal_metrics = {golden, golden};
    equal_metrics[1].shape.N0 += 1;
    equal_metrics[1].shape.alpha_height = std::bit_width(equal_metrics[1].shape.N0 - 1);
    equal_metrics[1].shape.query_width += 1;
    equal_metrics[1].shape.min_expansion_capacity =
        checked_bit_ceil(equal_metrics[1].shape.query_width, "test capacity");
    equal_metrics[1].shape.min_expansion_height =
        std::bit_width(equal_metrics[1].shape.min_expansion_capacity) - 1;
    if (equal_metrics[1].shape.expansion_capacity <
        equal_metrics[1].shape.min_expansion_capacity) {
      equal_metrics[1].shape.expansion_height =
          equal_metrics[1].shape.min_expansion_height;
      equal_metrics[1].shape.expansion_capacity =
          equal_metrics[1].shape.min_expansion_capacity;
    }
    equal_metrics[1].shape.required_key_height =
        std::max(equal_metrics[1].shape.expansion_height,
                 equal_metrics[1].shape.projection_depth_max);
    equal_metrics[1].shape.registered_key_height = log_n;
    equal_metrics[1].scheme = make_scheme_fingerprint(
        scheme, c, log_n, EvalKeyRegistrationPolicy::FixedMaximumHeight);
    const std::vector<Candidate> deduped =
        deduplicate_exact_plans(equal_metrics, context);
    require_test(deduped.size() == 2,
                 "equal aggregate metrics with different level plans are retained");
    // Label-only duplicates: with alignments {1, 16} a (h, G, b) whose minimal
    // N0 is already a multiple of 16 (h=1, G=64, b=10: 2^19 leaf plaintexts
    // over 2^10 rows = 512) is emitted once per alignment label. The frontier
    // keeps both (equal on every dimension); the exact-plan deduplication the
    // benchmark relies on must collapse them so one physical shape is not
    // measured twice.
    const std::optional<Candidate> label1 = pick_candidate(all, 1, 64, 10, 1);
    const std::optional<Candidate> label16 = pick_candidate(all, 1, 64, 10, 16);
    require_test(label1.has_value() && label16.has_value() &&
                     label1->shape.N0 == 512 && label16->shape.N0 == 512 &&
                     label1->shape.alpha_alignment == 1 &&
                     label16->shape.alpha_alignment == 16,
                 "h=1, G=64, b=10 is emitted under both alignment labels with N0 = 512");
    require_test(make_plan_tag(label1->shape, label1->scheme) ==
                         make_plan_tag(label16->shape, label16->scheme) &&
                     build_level_plans(label1->shape) ==
                         build_level_plans(label16->shape),
                 "the alignment label enters neither the plan tag nor the level plans");
    require_test(pareto_frontier({*label1, *label16}, policy, context).size() == 2,
                 "the frontier keeps both label-only duplicates (neither dominates)");
    require_test(deduplicate_exact_plans({*label1, *label16}, context).size() == 1,
                 "exact-plan deduplication collapses the label-only pair");
    require_test(deduplicate_exact_plans(all, context).size() < all.size(),
                 "the {1, 16} enumeration contains label-only duplicates to collapse");

    CostCalibration mandatory_cal = calibrated;
    mandatory_cal.default_scan_bandwidth_bytes_per_ms = 1e12;
    const std::vector<Candidate> mandatory =
        mandatory_controls(all, input, mandatory_cal, context);
    bool saw_bundle3 = false, saw_h1 = false;
    for (const Candidate &cand : mandatory) {
      saw_bundle3 |= cand.shape.bundle_arity == 3 && cand.shape.record_coeffs == 64 &&
                     cand.shape.plane_count == 1;
      saw_h1 |= cand.shape.bundle_arity == 1;
    }
    require_test(saw_bundle3 && saw_h1,
                 "mandatory controls retain Bundle3 and h=1 even under a tight frontier cap");
    const std::vector<Candidate> capped_none =
        top_nonmandatory(frontier, mandatory, 0, context);
    require_test(capped_none.empty(),
                 "max_frontier=0 adds no nonmandatory candidates");

    std::vector<Candidate> nonfinite = {golden, worse};
    nonfinite[1].shape.N0 += 1;
    nonfinite[1].shape.alpha_height = std::bit_width(nonfinite[1].shape.N0 - 1);
    nonfinite[1].shape.query_width += 1;
    CostCalibration bad_cal = calibrated;
    bad_cal.default_scan_bandwidth_bytes_per_ms =
        std::numeric_limits<double>::quiet_NaN();
    (void)pareto_frontier(nonfinite, policy, context);
    require_test(throws_invalid([&] { rank_by_prediction(nonfinite, bad_cal, context); }),
                 "ranking rejects nonfinite predictions after Pareto accepts structural input");

    CostCalibration stage_cal;
    stage_cal.alpha_pyramid_add_ms = 0.1;
    stage_cal.alpha_carry_copy_ms = 0.2;
    stage_cal.alpha_ntt_per_ciphertext_ms = 0.3;
    stage_cal.alpha_split_per_u32_output_ms = 0.4;
    TreeServerTiming measured{};
    measured.query_expand = std::chrono::milliseconds(10);
    measured.selector_completion = std::chrono::milliseconds(23);
    measured.alpha_prepare = std::chrono::milliseconds(19);
    measured.first_dim_finalize = std::chrono::milliseconds(7);
    apply_measured_control_timing(stage_cal, golden, measured);
    require_test(stage_cal.expand_per_populated_node_ms > 0 &&
                     stage_cal.to_rgsw_per_selector_ms == 1.0 &&
                     stage_cal.first_dim_output_ciphertext_ms > 0 &&
                     stage_cal.alpha_pyramid_add_ms == 0.1 &&
                     stage_cal.alpha_carry_copy_ms == 0.2 &&
                     stage_cal.alpha_ntt_per_ciphertext_ms == 0.3 &&
                     stage_cal.alpha_split_per_u32_output_ms == 0.4,
                 "measured control timing calibrates separable stages and preserves independent alpha costs");
    // The remaining stage attributions: fold + rotate calibrate one CMux cost
    // over both CMux counts (1013 + 105 for Bundle3), projection per key
    // switch (105), path_pack per (level, plane) item (25), modulus_switch
    // per response chunk (1). Attributing a stage to the wrong field or
    // divisor would silently re-rank the benchmark's frontier rows; stages
    // measured as zero must leave their defaults untouched.
    const size_t cmux_units =
        golden.metrics.fold_cmux_count + golden.metrics.rotate_cmux_count;
    const size_t pack_items = golden.shape.plane_count * golden.shape.path_levels;
    require_test(cmux_units == 1118 &&
                     golden.metrics.projection_keyswitch_count == 105 &&
                     pack_items == 25 && golden.shape.response_ciphertexts == 1,
                 "Bundle3 stage unit counts");
    CostCalibration late_cal;
    TreeServerTiming late{};
    late.beta_fold = std::chrono::milliseconds(5);
    late.private_rotate = std::chrono::milliseconds(3);
    late.projection = std::chrono::milliseconds(21);
    late.path_pack = std::chrono::milliseconds(5);
    late.modulus_switch = std::chrono::milliseconds(2);
    apply_measured_control_timing(late_cal, golden, late);
    require_test(std::fabs(late_cal.cmux_ms * static_cast<double>(cmux_units) - 8.0) < 1e-9,
                 "fold + rotate time is spread over both CMux counts");
    require_test(std::fabs(late_cal.substitution_ms - 21.0 / 105.0) < 1e-12,
                 "projection time is per projection key switch");
    require_test(std::fabs(late_cal.path_item_pack_ms - 5.0 / 25.0) < 1e-12,
                 "path_pack time is per (level, plane) item");
    require_test(late_cal.modulus_switch_ms == 2.0,
                 "modulus_switch time is per response chunk");
    const CostCalibration untouched;
    require_test(late_cal.expand_intercept_ms == untouched.expand_intercept_ms &&
                     late_cal.expand_per_populated_node_ms ==
                         untouched.expand_per_populated_node_ms &&
                     late_cal.to_rgsw_per_selector_ms ==
                         untouched.to_rgsw_per_selector_ms &&
                     late_cal.first_dim_output_ciphertext_ms ==
                         untouched.first_dim_output_ciphertext_ms,
                 "stages measured as zero keep their default calibration");

    const std::vector<Candidate> small_all = enumerate_candidates(
        make_input(scheme, 256, size_t{1} << 8, test_policy(1, 4)));
    require_test(!small_all.empty(), "small calibration fixture has candidates");
    const CostCalibration measured_cal = calibrate_cost_model(
        std::span<const Candidate>(&small_all.front(), 1), CostCalibration{},
        context, CostCalibrationOptions{.trials_per_shape = 1,
                                        .max_working_set_bytes_per_call =
                                            4 * 1024 * 1024,
                                        .seed = 7});
    require_test(!measured_cal.scan_bandwidth_bytes_per_ms.empty(),
                 "hardware calibration records measured per-limb shape bandwidths");
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

  // ---- every emitted candidate freezes and indexes end to end ----
  {
    // The benchmark freezes every row the enumerator emits. Sweeping the
    // whole enumeration (b-loop feasibility filter, alignment-16 rounding at
    // small N0, response/selector/alpha field derivations) against the index
    // module's independent recomputation catches planner/index drift at the
    // shapes no hand-picked fixture reaches.
    PlannerPolicy policy = test_policy(/*max_responses=*/4, /*max_arity=*/16);
    policy.alpha_alignments = {1, 16};
    size_t swept = 0, aligned16 = 0, rounded = 0;
    for (const size_t E : {size_t{13}, size_t{256}, size_t{4096}}) {
      for (const size_t N : {size_t{1} << 4, size_t{1} << 8}) {
        const std::vector<Candidate> all =
            enumerate_candidates(make_input(scheme, E, N, policy));
        require_test(!all.empty(), "every (E, N) sweep point has candidates");
        for (const Candidate &cand : all) {
          const CandidateShape &s = cand.shape;
          // Rejects any drift between the shape fields and the index module's
          // recomputation (minimal P, selector bits, capacity, key heights,
          // level plans, response count, tag).
          const TreeParams p = freeze_candidate(cand);
          (void)codec_spec_for(p);
          const size_t n0_min =
              ceil_div(s.leaf_plaintexts, size_t{1} << s.beta_bits);
          require_test(s.N0 % s.alpha_alignment == 0 && s.N0 >= n0_min &&
                           s.N0 - n0_min < s.alpha_alignment,
                       "N0 is the minimal aligned first-dimension width");
          require_test(s.query_width == s.N0 + scheme.get_l() * s.selector_bits,
                       "query width is N0 plus l_ep gadget rows per selector bit");
          // Both ends of the leaf range must decompose: client_index requires
          // alpha < N0, which is where a too-narrow first dimension shows up.
          (void)client_index(0, p);
          (void)client_index(N - 1, p);
          for (size_t level = 0; level <= s.tree_height; ++level) {
            require_test(level_oracle_for_test(N - 1, level, p).plaintext_index <
                             p.level_plans[level].padded_plaintexts_per_plane,
                         "the last leaf's target lies inside every level matrix");
          }
          require_test(response_placement(s.tree_height, s.plane_count - 1, p)
                               .response_chunk == s.response_ciphertexts - 1,
                       "the last (level, plane) item lands in the last response chunk");
          ++swept;
          aligned16 += s.alpha_alignment == 16;
          rounded += s.N0 != n0_min;
        }
      }
    }
    require_test(aligned16 > 0 && rounded > 0,
                 "the sweep exercises alignment 16 with actual rounding");
    BENCH_PRINT("enumerator/index sweep: " << swept << " candidates frozen, "
                << aligned16 << " with alignment 16 (" << rounded << " rounded)");
  }

  // ---- memory caps drop exactly the candidates above the cap ----
  {
    // SUPERRECORD_MEMORY_BYTES feeds both caps; a swapped comparison
    // (validation vs steady-state) or a > / >= slip would silently change
    // which shapes a memory-limited campaign run measures.
    PlannerPolicy base = test_policy(1, 8);
    base.alpha_alignments = {1, 16};
    const PlannerInput input = make_input(scheme, 256, size_t{1} << 10, base);
    const std::vector<Candidate> all = enumerate_candidates(input);
    require_test(all.size() >= 4, "the cap fixture needs several candidates");
    const auto same_candidate = [](const Candidate &a, const Candidate &b) {
      return make_plan_tag(a.shape, a.scheme) == make_plan_tag(b.shape, b.scheme) &&
             a.shape.alpha_alignment == b.shape.alpha_alignment;
    };
    const auto check_cap = [&](size_t CandidateMetrics::*estimate,
                               size_t PlannerPolicy::*cap_field,
                               const std::string &what) {
      std::vector<size_t> ests;
      for (const Candidate &cand : all) ests.push_back(cand.metrics.*estimate);
      std::sort(ests.begin(), ests.end());
      const size_t cap = ests[ests.size() / 2];
      require_test(ests.front() > 0 && cap < ests.back(),
                   what + ": the median cap must exclude some candidate");
      PlannerInput capped = input;
      capped.policy.*cap_field = cap;
      const std::vector<Candidate> filtered = enumerate_candidates(capped);
      std::vector<const Candidate *> expected;
      for (const Candidate &cand : all) {
        if (cand.metrics.*estimate <= cap) expected.push_back(&cand);
      }
      require_test(!filtered.empty() && filtered.size() < all.size() &&
                       filtered.size() == expected.size(),
                   what + ": the cap keeps exactly the candidates at or below it");
      bool at_cap = false;
      for (size_t i = 0; i < filtered.size(); ++i) {
        require_test(same_candidate(filtered[i], *expected[i]) &&
                         filtered[i].metrics.*estimate <= cap,
                     what + ": the capped enumeration is the in-order subsequence");
        at_cap |= filtered[i].metrics.*estimate == cap;
      }
      require_test(at_cap, what + ": a candidate exactly at the cap is kept");
      PlannerInput tiny = input;
      tiny.policy.*cap_field = ests.front() - 1;
      require_test(enumerate_candidates(tiny).empty(),
                   what + ": a cap below every estimate yields no candidates");
    };
    check_cap(&CandidateMetrics::optimized_steady_state_bytes_estimate,
              &PlannerPolicy::max_steady_state_bytes, "steady-state cap");
    check_cap(&CandidateMetrics::validation_resident_bytes_estimate,
              &PlannerPolicy::max_validation_resident_bytes,
              "validation-resident cap");
  }

  // ---- mandatory controls carry both argmin rows ----
  {
    // The benchmark labels these rows minimum-optimized-steady-state /
    // minimum-response-count by recomputing the argmin by value, and the
    // sweep analysis keys on those labels; two responses make the response
    // count non-constant so the second rule has something to choose.
    const PlannerPolicy policy = test_policy(/*max_responses=*/2, 8);
    const PlannerInput input = make_input(scheme, 256, size_t{1} << 10, policy);
    const std::vector<Candidate> all = enumerate_candidates(input);
    ParetoContext context;
    context.avx512 = {true, true};
    size_t min_steady = std::numeric_limits<size_t>::max();
    size_t min_resp = std::numeric_limits<size_t>::max(), max_resp = 0;
    for (const Candidate &cand : all) {
      min_steady = std::min(min_steady,
                            cand.metrics.optimized_steady_state_bytes_estimate);
      min_resp = std::min(min_resp, cand.shape.response_ciphertexts);
      max_resp = std::max(max_resp, cand.shape.response_ciphertexts);
    }
    require_test(min_resp == 1 && max_resp == 2,
                 "the control fixture mixes one- and two-response candidates");
    // The rule picks the first candidate in predicted order with the minimal
    // value; the returned control must be exact-plan equal to it.
    std::vector<Candidate> ranked = all;
    rank_by_prediction(ranked, CostCalibration{}, context);
    const auto first_ranked = [&](auto &&is_minimal) -> const Candidate & {
      for (const Candidate &cand : ranked) {
        if (is_minimal(cand)) return cand;
      }
      throw std::runtime_error("ranked fixture lost its argmin");
    };
    const Candidate &expected_steady = first_ranked([&](const Candidate &c) {
      return c.metrics.optimized_steady_state_bytes_estimate == min_steady;
    });
    const Candidate &expected_resp = first_ranked([&](const Candidate &c) {
      return c.shape.response_ciphertexts == min_resp;
    });
    const auto same_exact_plan = [](const Candidate &a, const Candidate &b) {
      return make_plan_tag(a.shape, a.scheme) == make_plan_tag(b.shape, b.scheme) &&
             build_level_plans(a.shape) == build_level_plans(b.shape);
    };
    const std::vector<Candidate> mandatory =
        mandatory_controls(all, input, CostCalibration{}, context);
    require_test(!mandatory.empty() && mandatory.size() <= 4,
                 "at most one control per rule");
    bool saw_steady = false, saw_resp = false;
    for (const Candidate &control : mandatory) {
      saw_steady |= same_exact_plan(control, expected_steady);
      saw_resp |= same_exact_plan(control, expected_resp);
      require_test(std::isfinite(control.metrics.predicted_server_ms),
                   "controls carry the ranking prediction");
    }
    require_test(saw_steady,
                 "the minimum-optimized-steady-state control is the fastest global argmin");
    require_test(saw_resp,
                 "the minimum-response-count control is the fastest minimal-chunk candidate");
    for (size_t i = 0; i < mandatory.size(); ++i) {
      for (size_t j = i + 1; j < mandatory.size(); ++j) {
        require_test(!same_exact_plan(mandatory[i], mandatory[j]),
                     "controls are exact-plan unique");
      }
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
    const std::vector<Candidate> e160_all = enumerate_candidates(
        make_input(scheme, 160, size_t{1} << 24, test_policy(1, 8)));
    const std::optional<Candidate> e160 = pick_candidate(e160_all, 4, 64, 8);
    require_test(e160.has_value() && e160->shape.plane_count == 1 &&
                     e160->shape.response_ciphertexts == 1,
                 "E=160, h=4, G=64 uses one response at H=25");
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
        make_input(scheme, 30000, size_t{1} << 4, test_policy(16, 1)));
    const std::optional<Candidate> big = pick_candidate(large, 1, n, 4);
    require_test(big.has_value() && n == 2048 && big->shape.record_coeffs == 2048 &&
                     big->shape.plane_count == 2 &&
                     big->shape.response_ciphertexts == 10,
                 "E=30000, h=1, G=2048 gives P=2 and 10 responses");
  }
}
