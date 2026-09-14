#include "tree_superrecord_test_util.h"

#include "tree_superrecord_noise.h"
#include "tree_superrecord_response.h"
#include "tree_superrecord_benchmark_data.h"

#include <algorithm>
#include <limits>

using namespace tree_superrecord;
using namespace tree_superrecord_test;

void PirTest::test_tree_superrecord_noise() {
  print_func_name(__FUNCTION__);
  for (const size_t bits : {size_t{1}, size_t{7}, size_t{13}, size_t{256}, size_t{30000}}) {
    for (const size_t c : {size_t{1}, size_t{7}, size_t{12}, size_t{31}, size_t{63}}) {
      for (const size_t h : {size_t{1}, size_t{3}, size_t{7}}) {
        for (const size_t G : {size_t{1}, size_t{8}, size_t{64}, size_t{2048}}) {
          const CodecSpec spec{bits, c, h, G, minimal_plane_count(bits, c, h, G), uint64_t{1} << c};
          const size_t plane_bits = c * G;
          for (const size_t node : {size_t{0}, h - 1, h, h + 1, size_t{4180873}}) {
            for (const size_t pattern : {size_t{0}, size_t{2}, size_t{3}, size_t{4}}) {
              EntryBits oracle = zero_entry(bits);
              for (size_t bit = 0; bit < bits; ++bit) {
                const size_t bundle_bit = (node % h) * bits + bit;
                if (pattern == 2 || (pattern == 3 && (bit + node) % 2 == 0) ||
                    (pattern == 4 && (bundle_bit % plane_bits == 0 ||
                                     bundle_bit % plane_bits + 1 == plane_bits))) {
                  set_bit(oracle, bit, 1);
                }
              }
              require_test(tree_superrecord_benchmark_data::patterned_entry(spec, node, pattern) == oracle,
                           "structured fixtures must match the original per-bit generator");
            }
          }
        }
      }
    }
  }
  PirParams scheme;
  PirClient client(scheme);
  const TreeParams params =
      plan_for(scheme, 256, 256, 3, 64, 1, test_policy());
  PirServer server(query_expansion_params(params, scheme));
  const TaggedTreeSessionKeys session =
      create_tagged_tree_session_keys(client, params, scheme);
  set_tree_client_session_keys(server, params, scheme, session);

  CandidateNoiseResult smoke;
  smoke.tag = params.tag;
  smoke.fixture_kind = NoiseFixtureKind::SmallShapeSmoke;
  smoke.eligibility = NoiseEligibility::Passed;
  smoke.decrypts_correctly = true;
  smoke.observed_key_coverage_matches_tag = true;
  smoke.minimum_margin = 1;

  require_test(!noise_result_unlocks_candidate(smoke, params,
                                               client.get_client_id()),
               "small-shape smoke noise does not unlock target m32");

  const EntrySource source = [](size_t level, size_t node) {
    return synthetic_entry(256, level, node);
  };
  const TargetNoiseCase target_case{.leaf = params.shape.leaf_count - 1,
                                    .source = source,
                                    .label = "noise-test"};
  // Full-Q noise_budget is a first-limb diagnostic. Multi-limb fixtures use
  // the existing correctness-only policy; K = 1 retains its positive margin.
  const double required_margin = scheme.K() == 1 ? 1.0 : 0.0;
  const CandidateNoiseResult passed = run_target_shape_noise_gate(
      client, server, session, params, scheme,
      std::span<const TargetNoiseCase>(&target_case, 1), required_margin);
  require_test(passed.decrypts_correctly &&
                   passed.observed_key_coverage_matches_tag,
               "target fixture decrypts correctly with its registered key coverage");
  require_test(noise_result_unlocks_candidate(passed, params,
                                              client.get_client_id()),
               "target-tag noise gate unlocks its own plan");
  const CandidateNoiseResult impossible_margin = run_target_shape_noise_gate(
      client, server, session, params, scheme,
      std::span<const TargetNoiseCase>(&target_case, 1), 100.0);
  require_test(impossible_margin.eligibility == NoiseEligibility::Failed,
               "impossible noise margin fails the target fixture");
  // The Failed verdict must come from the margin policy, not from a decrypt
  // or key-coverage failure that the eligibility check would also report.
  require_test(impossible_margin.decrypts_correctly &&
                   impossible_margin.observed_key_coverage_matches_tag &&
                   impossible_margin.minimum_margin < 100.0,
               "the margin-100 failure is a margin failure, not a decrypt failure");
  // minimum_margin summarises the checkpoints: every target-gate checkpoint
  // is a remaining-budget measure with threshold 0 and margin == value, and
  // the summary is their minimum. SUPERRECORD_MIN_NOISE_MARGIN gates on
  // minimum_margin >= policy, so the boundary is admitted and anything above
  // the observed minimum is rejected by both the predicate and the trusted
  // gate.
  double min_checkpoint_margin = std::numeric_limits<double>::infinity();
  for (const NoiseCheckpoint &checkpoint : passed.checkpoints) {
    require_test(checkpoint.kind == NoiseMeasureKind::RemainingBudgetBits &&
                     checkpoint.decoder_threshold == 0 &&
                     checkpoint.margin == checkpoint.value,
                 "target-gate checkpoints are remaining-budget measures with margin == value");
    min_checkpoint_margin = std::min(min_checkpoint_margin, checkpoint.margin);
  }
  require_test(passed.minimum_margin == min_checkpoint_margin &&
                   passed.minimum_margin >= required_margin,
               "minimum_margin is the minimum checkpoint margin");
  CandidateNoiseResult at_boundary = passed;
  at_boundary.min_noise_margin = passed.minimum_margin;
  require_test(noise_result_unlocks_candidate(at_boundary, params,
                                              client.get_client_id()) &&
                   !throws_invalid([&] {
                     require_target_noise_gate(at_boundary, params,
                                               client.get_client_id());
                   }),
               "a policy margin equal to the observed minimum is admitted");
  CandidateNoiseResult above_boundary = passed;
  above_boundary.min_noise_margin = passed.minimum_margin + 0.5;
  require_test(!noise_result_unlocks_candidate(above_boundary, params,
                                               client.get_client_id()) &&
                   throws_invalid([&] {
                     require_target_noise_gate(above_boundary, params,
                                               client.get_client_id());
                   }),
               "a policy margin above the observed minimum is rejected");
  BENCH_PRINT("target noise gate: " << passed.checkpoints.size()
              << " checkpoints, minimum remaining budget "
              << passed.minimum_margin << " bits");
  CandidateNoiseResult zero_margin_low_budget = passed;
  zero_margin_low_budget.min_noise_margin = 0.0;
  zero_margin_low_budget.minimum_margin = -2.0;
  zero_margin_low_budget.eligibility = NoiseEligibility::Passed;
  zero_margin_low_budget.checkpoints.front().kind =
      NoiseMeasureKind::RemainingBudgetBits;
  zero_margin_low_budget.checkpoints.front().value = -2.0;
  zero_margin_low_budget.checkpoints.front().decoder_threshold = 0.0;
  zero_margin_low_budget.checkpoints.front().margin = -2.0;
  require_test(noise_result_unlocks_candidate(zero_margin_low_budget, params,
                                              client.get_client_id()),
               "zero-margin noise gate admits correct decrypt with low diagnostic budget");
  require_test(!throws_invalid([&] {
                 require_target_noise_gate(zero_margin_low_budget, params,
                                           client.get_client_id());
               }),
               "zero-margin trusted gate admits correct decrypt with low diagnostic budget");
  CandidateNoiseResult zero_margin_bad_decrypt = zero_margin_low_budget;
  zero_margin_bad_decrypt.decrypts_correctly = false;
  require_test(!noise_result_unlocks_candidate(zero_margin_bad_decrypt, params,
                                               client.get_client_id()),
               "zero-margin noise gate still rejects failed decrypt diagnostics");
  CandidateNoiseResult positive_margin_low_budget = zero_margin_low_budget;
  positive_margin_low_budget.min_noise_margin = 1.0;
  require_test(!noise_result_unlocks_candidate(positive_margin_low_budget, params,
                                               client.get_client_id()),
               "positive-margin noise gate still enforces numeric margin");
  CandidateNoiseResult empty_checkpoints = passed;
  empty_checkpoints.checkpoints.clear();
  require_test(throws_invalid([&] {
                 (void)preprocess_m32(params, scheme, empty_checkpoints,
                                      source, true);
               }),
               "trusted noise gate rejects empty checkpoints");
  CandidateNoiseResult bad_decrypt = passed;
  bad_decrypt.decrypts_correctly = false;
  require_test(throws_invalid([&] {
                 (void)preprocess_m32(params, scheme, bad_decrypt, source,
                                      true);
               }),
               "trusted noise gate rejects failed decrypt diagnostics");
  require_test(!passed.checkpoints.empty() &&
                   passed.checkpoints.front().stage.find("k1_comp experimental") ==
                       std::string::npos,
               "first alpha checkpoint is not mislabeled as K-aware proof");
  bool saw_experimental_selector = false;
  for (const NoiseCheckpoint &checkpoint : passed.checkpoints) {
    if (checkpoint.stage.find("k1_comp experimental") != std::string::npos) {
      saw_experimental_selector = true;
    }
  }
  require_test(saw_experimental_selector,
               "selector diagnostics label the k1_comp experimental scope");

  CandidateNoiseResult wrong_tag = passed;
  wrong_tag.tag.N0 += 1;
  require_test(!noise_result_unlocks_candidate(wrong_tag, params,
                                               client.get_client_id()),
               "target noise gate rejects a changed shape field");

  if (scheme.get_composite_rns().enabled) {
    require_test(throws_invalid([&] {
                   (void)preprocess_m32(params, scheme, smoke, source, true);
                 }),
                 "preprocess_m32 rejects smoke noise");
    const PreprocessedTree db = preprocess_m32(params, scheme, passed, source, true);

    PirClient other_client(scheme);
    PirServer other_server(query_expansion_params(params, scheme));
    const TaggedTreeSessionKeys other_session =
        create_tagged_tree_session_keys(other_client, params, scheme);
    set_tree_client_session_keys(other_server, params, scheme, other_session);
    const CandidateNoiseResult other_noise = run_target_shape_noise_gate(
        other_client, other_server, other_session, params, scheme,
        std::span<const TargetNoiseCase>(&target_case, 1), 1.0);
    require_test(noise_result_unlocks_candidate(other_noise, params,
                                                other_client.get_client_id()),
                 "second client passes its own target noise gate");
    const PackedQuery other_query =
        make_query(other_client, other_session, scheme, params, 0);
    const PathResponse other_response = serve_request_m32(
        other_query, db, other_server, other_session, params, scheme,
        other_noise);
    const std::vector<EntryBits> other_path =
        extract_path(other_response, other_client, params, scheme, 0);
    for (size_t level = 0; level <= params.shape.tree_height; ++level) {
      require_test(other_path[level] == source(level, 0),
                   "second client reuses the key-independent m32 database");
    }
    TreeServerTiming timing{};
    require_test(throws_invalid([&] {
                   (void)serve_request_m32(other_query, db, other_server,
                                           other_session, params, scheme,
                                           passed, &timing);
                 }),
                 "same-shape m32 request rejects another client's noise gate");
    require_test(timing.query_expand.count() == 0,
                 "cross-client noise rejection happens before expansion");
  }
}
