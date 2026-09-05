#include "tests.h"
#include "merkle_benchmark.h"
#include "tree_index.h"
#include "tree_profile_json.h"
#include "tree_query.h"
#include "tree_response.h"
#include "tree_select.h"

#include <array>
#include <bit>
#include <chrono>
#include <numeric>
#include <utility>
#include <vector>

// Timed MVP path retrieval at the Merkle-baseline workload scale: N = 2^24
// leaves (H = L = 24), CONFIG_N2048_K1_COMP scheme — the same tree shape and
// cryptographic parameters as the 1 GB baseline row. Differences to state
// with every comparison: g = 1, so each node carries one Z_t scalar instead
// of the baselines' 32-byte value (payload extension is blueprint sec. 23.1),
// while the whole path returns in ONE small-q ciphertext (Milestone 5)
// computed over the NTT-view first dimension (Milestone 6).
void PirTest::test_tree_benchmark() {
  print_func_name(__FUNCTION__);
  constexpr size_t N = DBConsts::PolyDegree;
  using Clock = std::chrono::steady_clock;
  const auto ms_since = [](Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
        .count();
  };

  PirParams scheme;
  // a = 7 keeps h_q = 8 within the scheme expansion height while bounding the
  // beta fold work; L defaults to 24 (the baselines' 2^24-leaf tree) and
  // --leaf-count selects another power of two.
  const size_t L = bench_tree_height(24);
  const TreePirParams tree = make_tree_pir_params_for_scheme(L, 7, scheme);
  const uint64_t t = scheme.get_plain_mod();
  const TreeNodeSource source = [&](size_t level, size_t index) {
    return synthetic_tree_node_value(level, index, t);
  };

  PirClient client(scheme);
  SharedPirSessionKeys keys =
      client.create_session_keys(std::bit_width(N) - 1);
  const PirParams qparams = tree_query_expansion_params(tree, scheme);
  PirServer server(qparams);
  server.set_client_session_keys(client.get_client_id(), keys);

  const auto setup_start = Clock::now();
  const PreprocessedTree db = preprocess_tree_mvp(tree, source, scheme);
  const double setup_ms = ms_since(setup_start);
  size_t total_pt = 0;
  for (const auto &level_db : db.canonical) {
    total_pt += level_db.plaintexts.size();
  }

  // Distinct deterministic query leaves, same sampler as the baselines.
  const size_t kWarmups = bench_warmups.value_or(1);
  const size_t kTrials = bench_measured_trials.value_or(5);
  const uint64_t trial_seed = bench_trial_seed.value_or(0x74726565426e6368ULL);
  const BenchmarkTrialPlan plan =
      make_benchmark_trial_plan(tree.N, kWarmups, kTrials, trial_seed);

  std::vector<double> query_ms, unpack_ms, path_ms, extract_ms;
  size_t actual_response_bytes = 0;
  bool response_small_q = false;
  // Server-path stage breakdown via the TimerLogger sections that
  // answer_path_mvp and the select kernels bracket (one experiment per trial).
  std::array<double, 7> stage_sum{};
  std::vector<tree_profile_json::Sample> json_samples;
  CLEAN_TIMER();
  const auto run_trial = [&](size_t leaf, bool measured, size_t trial) {
    auto start = Clock::now();
    RlweCt query = make_tree_query(client, scheme, tree, leaf);
    const double q_ms = ms_since(start);

    TreeQueryUnpackTiming unpack_timing;
    start = Clock::now();
    ExpandedTreeQuery unpacked = unpack_tree_query(
        server, scheme, tree, client.get_client_id(), query, &unpack_timing);
    const double u_ms = ms_since(start);

    start = Clock::now();
    TreePathResponse response =
        answer_path_mvp(db, unpacked, server, client.get_client_id(), tree,
                        scheme);
    const double p_ms = ms_since(start);

    start = Clock::now();
    const std::vector<uint64_t> path =
        extract_path_mvp(response, client, tree);
    const double e_ms = ms_since(start);

    start = Clock::now();
    std::stringstream wire;
    actual_response_bytes =
        server.save_resp_to_stream(response.chunks[0], wire);
    const double response_serialize_ms = ms_since(start);
    END_EXPERIMENT();
    std::map<std::string, double> sample_stages;
    if (measured) {
      sample_stages["query_expand"] =
          tree_profile_json::ns_to_ms(unpack_timing.query_expand);
      sample_stages["selector_completion"] =
          tree_profile_json::ns_to_ms(unpack_timing.selector_completion);
      for (size_t i = 0; i < tree_profile_json::kLegacyTimerSections.size();
           ++i) {
        const double stage_ms =
            GET_LAST_TIME(tree_profile_json::kLegacyTimerSections[i]);
        stage_sum[i] += stage_ms;
      }
      for (size_t i = 0; i < tree_profile_json::kScanTimerSections.size();
           ++i) {
        sample_stages[tree_profile_json::kScanStageLabels[i]] =
            GET_LAST_TIME(tree_profile_json::kScanTimerSections[i]);
      }
      for (size_t i = 0; i < tree_profile_json::kPathTimerSections.size();
           ++i) {
        sample_stages[tree_profile_json::kPathStageLabels[i]] =
            GET_LAST_TIME(tree_profile_json::kPathTimerSections[i]);
      }
    }

    // Correctness gate outside every timer boundary consumer: hard-fail on
    // any wrong path value.
    require_test(path.size() == tree.L + 1, "benchmark path length");
    for (size_t level = 0; level <= tree.L; ++level) {
      require_test(path[level] ==
                       source(level, leaf >> (tree.L - level)),
                   "benchmark path value mismatch");
    }
    if (measured) {
      query_ms.push_back(q_ms);
      unpack_ms.push_back(u_ms);
      path_ms.push_back(p_ms);
      extract_ms.push_back(e_ms);
      response_small_q = response.small_q;
      tree_profile_json::Sample sample;
      sample.trial = trial;
      sample.leaf = leaf;
      sample.client_query_ms = q_ms;
      sample.server_total_ms =
          tree_profile_json::query_unpack_ms(unpack_timing) + p_ms;
      sample.stages_ms = std::move(sample_stages);
      sample.diagnostics_ms["scan_combined"] = GET_LAST_TIME(TREE_SCAN_TIME);
      sample.response_serialize_ms = response_serialize_ms;
      sample.client_decode_ms = e_ms;
      sample.correct = true;
      json_samples.push_back(std::move(sample));
    }
  };

  size_t trial = 0;
  for (size_t leaf : plan.warmup_leaf_indices) run_trial(leaf, false, trial++);
  trial = 0;
  for (size_t leaf : plan.measured_leaf_indices) run_trial(leaf, true, trial++);

  const auto avg = [](const std::vector<double> &v) {
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
  };
  const double server_avg = avg(unpack_ms) + avg(path_ms);

  // Communication accounting, same conventions as the baseline suite: query
  // and helper keys are seed-compressed models; the response byte count is
  // read from the real small-q wire codec.
  const size_t query_bytes = scheme.get_BFV_size(true);
  const size_t helper_bytes =
      scheme.with_query_shape({1, 0, std::bit_width(N) - 1})
          .get_bv_galois_key_size(true) +
      scheme.get_gsw_key_size(true);

  BENCH_PRINT("tree MVP benchmark: N=2^" << tree.L << " leaves, L=" << tree.L
              << ", N0=" << tree.N0
              << ", b=" << tree.b << ", w=" << tree.w << ", W=" << tree.W);
  BENCH_PRINT("database: " << total_pt << " packed plaintexts, g=1 payload "
              << (total_pt * N * 8) / (1 << 20) << " MiB coefficient storage,"
              " setup " << setup_ms << " ms");
  BENCH_PRINT("trials: " << kTrials << " measured after " << kWarmups
              << " warmup, leaves "
              << plan.measured_leaf_indices.size() << " distinct");
  BENCH_PRINT("trial seed: " << trial_seed);
  BENCH_PRINT("client query avg " << avg(query_ms) << " ms");
  BENCH_PRINT("server unpack (expand+convert) avg " << avg(unpack_ms)
              << " ms");
  BENCH_PRINT("server path (select+rotate+project+pack) avg "
              << avg(path_ms) << " ms");
  BENCH_PRINT("server total avg " << server_avg << " ms; samples(path):");
  for (double v : path_ms) BENCH_PRINT("  path " << v << " ms");
  {
    std::string breakdown = "server path breakdown avg (ms):";
    double stage_total = 0;
    for (size_t i = 0; i < tree_profile_json::kLegacyTimerSections.size();
         ++i) {
      const double v = stage_sum[i] / path_ms.size();
      stage_total += v;
      breakdown += std::string(" ") +
                   tree_profile_json::kLegacyStageLabels[i] + " " +
                   std::to_string(v);
    }
    BENCH_PRINT(breakdown);
    BENCH_PRINT("  stages sum " << stage_total << " ms of path avg "
                << avg(path_ms) << " ms (remainder = glue/alloc)");
  }
  BENCH_PRINT("client extract avg " << avg(extract_ms) << " ms");
  BENCH_PRINT("communication: query " << query_bytes
              << " B (modeled, 1 ciphertext), response "
              << actual_response_bytes << " B (actual wire codec, small_q="
              << (response_small_q ? "yes" : "no") << "), helper keys "
              << helper_bytes << " B (modeled, "
              << (std::bit_width(N) - 1) << " BV + GSW)");
  tree_profile_json::write_report(
      tree_profile_json::output_path_from_env(), "legacy-tree-g1", tree.N,
      12, (tree.N * 12 + 7) / 8, kWarmups, kTrials, trial_seed,
      true, "mvp",
      json_samples,
      {{"hardware_target", "CloudLab Xeon 6142 AVX-512 376GiB RAM"},
       {"payload_note", "g=1 carries one 12-bit Z_t scalar per node, not a 32-byte node"},
       {"response_small_q", response_small_q ? "true" : "false"}});
}
