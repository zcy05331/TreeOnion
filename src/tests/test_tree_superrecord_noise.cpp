#include "tree_superrecord_test_util.h"

#include "tree_superrecord_noise.h"
#include "tree_superrecord_response.h"

using namespace tree_superrecord;
using namespace tree_superrecord_test;

void PirTest::test_tree_superrecord_noise() {
  print_func_name(__FUNCTION__);
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
  const CandidateNoiseResult passed = run_target_shape_noise_gate(
      client, server, session, params, scheme,
      std::span<const TargetNoiseCase>(&target_case, 1), 1.0);
  require_test(noise_result_unlocks_candidate(passed, params,
                                              client.get_client_id()),
               "target-tag noise gate unlocks its own plan");
  const CandidateNoiseResult impossible_margin = run_target_shape_noise_gate(
      client, server, session, params, scheme,
      std::span<const TargetNoiseCase>(&target_case, 1), 100.0);
  require_test(impossible_margin.eligibility == NoiseEligibility::Failed,
               "impossible noise margin fails the target fixture");
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
