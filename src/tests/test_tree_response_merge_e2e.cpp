#include "tree_superrecord_test_util.h"

#include "tree_superrecord_response.h"

#include <algorithm>
#include <set>

using namespace tree_superrecord;
using namespace tree_superrecord_test;

namespace {

size_t expected_merge_keyswitches(const TreeParams &params) {
  size_t total = 0;
  size_t remaining =
      params.shape.path_levels * params.shape.plane_count;
  while (remaining > 0) {
    const size_t group =
        std::min(remaining, params.shape.records_per_plaintext);
    total += mlwe_merge_keyswitch_count(group,
                                        params.shape.records_per_plaintext);
    remaining -= group;
  }
  return total;
}

void exercise_mlwe_shape(const PirParams &scheme, size_t entry_bits,
                         size_t leaves, size_t h, size_t G, size_t b,
                         size_t max_responses) {
  PirClient client(scheme);
  const TreeParams params = plan_for(scheme, entry_bits, leaves, h, G, b,
                                     test_policy(max_responses, 128));
  PirServer server(query_expansion_params(params, scheme));
  const TaggedTreeSessionKeys session =
      create_tagged_tree_session_keys(client, params, scheme);
  set_tree_client_session_keys(server, params, scheme, session);

  const EntrySource source = [&](size_t level, size_t node) {
    return synthetic_entry(entry_bits, level, node);
  };
  ReferencePreprocessedTree reference =
      preprocess_reference(params, scheme, source);
  const CandidateNoiseResult noise = passing_target_noise_result(
      client, params, scheme, server, session, source, leaves - 1, 1.0,
      ResponsePacking::MlweMerge);
  require_test(noise_result_unlocks_candidate(noise, params,
                                              client.get_client_id(),
                                              ResponsePacking::MlweMerge),
               "MLWE noise gate unlocks only the selected response mode");
  const PreprocessedTree m32 =
      m32_from_reference(reference, params, scheme, noise, true);

  const std::set<size_t> test_leaves = {
      0, leaves - 1, (0x2BADBEEFULL % leaves),
      std::min(leaves - 1, h), std::min(leaves - 1, h + 1)};
  for (size_t leaf : test_leaves) {
    const PackedQuery query = make_query(client, session, scheme, params, leaf);
    TreeServerTiming timing{};
    const PathResponse response =
        serve_request_m32(query, m32, server, session, params, scheme, noise,
                          &timing, ResponsePacking::MlweMerge);
    require_test(response.packing == ResponsePacking::MlweMerge,
                 "MLWE response records its packing mode");
    require_test(timing.response_merge.count() > 0,
                 "MLWE response path reports merge timing");
    require_test(timing.response_merge_keyswitches ==
                     expected_merge_keyswitches(params),
                 "MLWE response path reports the exact merge keyswitch count");
    const DecodedPath decoded =
        extract_path_measured(response, client, params, scheme, leaf);
    for (size_t level = 0; level < params.shape.path_levels; ++level) {
      const size_t node = leaf >> (params.shape.tree_height - level);
      require_test(decoded.entries[level] == source(level, node),
                   "MLWE response decodes the selected path");
    }

    const std::vector<std::vector<EntryBits>> lanes =
        debug::extract_all_lanes_for_test(response, client, params, scheme);
    for (size_t level = 0; level < params.shape.path_levels; ++level) {
      const size_t node = leaf >> (params.shape.tree_height - level);
      const size_t start = (node / h) * h;
      for (size_t lane = 0; lane < h; ++lane) {
        const size_t neighbour = start + lane;
        const EntryBits expected = neighbour < (size_t{1} << level)
                                       ? source(level, neighbour)
                                       : zero_entry(entry_bits);
        require_test(lanes[level][lane] == expected,
                     "MLWE debug extraction honors bit-reversed residues");
      }
    }
  }

  const CandidateNoiseResult projection_noise = passing_target_noise_result(
      client, params, scheme, server, session, source, leaves - 1, 1.0);
  require_test(!noise_result_unlocks_candidate(projection_noise, params,
                                               client.get_client_id(),
                                               ResponsePacking::MlweMerge),
               "projection noise result does not unlock MLWE serving");
  const PackedQuery query = make_query(client, session, scheme, params, 0);
  require_test(throws_invalid([&] {
                 (void)serve_request_m32(query, m32, server, session, params,
                                         scheme, projection_noise, nullptr,
                                         ResponsePacking::MlweMerge);
               }),
               "MLWE serving rejects a projection-mode noise gate");
}

void require_mlwe_full_r_keys() {
  PirParams scheme;
  PirClient client(scheme);
  PlannerPolicy policy = test_policy(1, 128);
  policy.eval_key_registration =
      EvalKeyRegistrationPolicy::CandidateSpecificHeight;
  policy.fixed_session_key_height = 0;
  const TreeParams params = plan_for(scheme, 13, 8, 8, 4, 0, policy);
  require_test(params.shape.registered_key_height <
                   params.shape.record_index_bits,
               "test fixture must register fewer than the full rho-trace");
  PirServer server(query_expansion_params(params, scheme));
  const TaggedTreeSessionKeys session =
      create_tagged_tree_session_keys(client, params, scheme);
  set_tree_client_session_keys(server, params, scheme, session);
  const EntrySource source = [&](size_t level, size_t node) {
    return synthetic_entry(13, level, node);
  };
  const TargetNoiseCase target_case{.leaf = 7,
                                    .source = source,
                                    .label = "short-keys"};
  require_test(throws_invalid([&] {
                 (void)run_target_shape_noise_gate(
                     client, server, session, params, scheme,
                     std::span<const TargetNoiseCase>(&target_case, 1), 1.0,
                     ResponsePacking::MlweMerge);
               }),
               "MLWE gate requires the full rho-trace key family");
}

}  // namespace

void test_tree_response_merge_e2e() {
  PirParams scheme;
  require_applicable(scheme.get_composite_rns().enabled,
                     "MLWE response merge E2E needs CONFIG_N2048_K1_COMP");
  exercise_mlwe_shape(scheme, 4096, 16, 1, 256, 0, 2);
  exercise_mlwe_shape(scheme, 256, 256, 3, 64, 0, 1);
  require_mlwe_full_r_keys();
}
