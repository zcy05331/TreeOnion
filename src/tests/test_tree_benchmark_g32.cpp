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

// Real-scenario efficiency: g = 32 (full 32-byte nodes) at N = 2^22 leaves
// by default (--leaf-count selects another power of two), the largest workload whose canonical + NTT views fit this machine's
// memory budget (~4.3 GiB; the 2^24 row needs ~17 GiB and is reported by
// linear first-dimension extrapolation instead). Same scheme and statistics
// conventions as the baselines; the payload per node now matches theirs
// exactly (32 bytes), stored as 32 x 12-bit chunks (384-bit slots, the
// power-of-two rounding of 256 bits).
void PirTest::test_tree_benchmark_g32() {
  print_func_name(__FUNCTION__);
  constexpr size_t N = DBConsts::PolyDegree;
  using Clock = std::chrono::steady_clock;
  const auto ms_since = [](Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
        .count();
  };

  PirParams scheme;
  const uint64_t t = scheme.get_plain_mod();
  // Real-hash slot count for this scheme: 256 bits over (log2(t) - 1)-bit
  // chunks, rounded to a power of two (n=2048/12-bit -> g=32;
  // n=4096/39-bit -> g=8).
  const size_t chunk_bits = std::bit_width(t) - 1;
  const size_t g = std::bit_ceil((size_t{255} / chunk_bits) + 1);
  // Fold-halving experiment result (k1_comp, 2^22): pushing h_q to 11 via
  // key-height 11 admits a = 10, but the doubled expansion (+72 ms) and the
  // doubled pyramid eat the fold savings (path only -26 ms) while the 1024-
  // term first-dimension sum lifts max noise to 294 > 256 — over the small-q
  // bound on unused coefficients. The scan therefore stays inside the
  // scheme's default expansion height, which is the noise-safe envelope.
  const size_t L = bench_tree_height(22);
  const size_t key_height = scheme.get_expan_height();
  size_t chosen_a = 2;
  for (size_t a = 20; a >= 2; --a) {
    try {
      (void)make_tree_pir_params_for_scheme(L, a, g, scheme, key_height);
      chosen_a = a;
      break;
    } catch (const std::invalid_argument &) {
      // Shape infeasible; try a smaller first dimension.
    }
  }
  const TreePirParams tree =
      make_tree_pir_params_for_scheme(L, chosen_a, g, scheme, key_height);
  const TreeNodeChunkSource source = [&](size_t level, size_t index,
                                         size_t chunk) {
    return synthetic_tree_node_bytes_chunk(level, index, chunk, t);
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

  const size_t kWarmups = bench_warmups.value_or(3);
  const size_t kTrials = bench_measured_trials.value_or(16);
  const uint64_t trial_seed = bench_trial_seed.value_or(0x747265654733325fULL);
  const BenchmarkTrialPlan plan =
      make_benchmark_trial_plan(tree.N, kWarmups, kTrials, trial_seed);

  std::vector<double> query_ms, unpack_ms, path_ms, extract_ms;
  size_t actual_response_bytes = 0;
  // Server-path stage breakdown: answer_path_mvp and the select kernels
  // accumulate TimerLogger sections; each trial is closed with
  // END_EXPERIMENT() and its per-stage totals read back by key.
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
    const std::vector<std::vector<uint64_t>> path =
        extract_path_chunks_mvp(response, client, tree);
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

    require_test(path.size() == tree.L + 1, "benchmark path length");
    for (size_t level = 0; level <= tree.L; ++level) {
      const size_t node = leaf >> (tree.L - level);
      // Full 32-byte comparison chunk by chunk, outside every timer.
      for (size_t j = 0; j < tree.g; ++j) {
        require_test(path[level][j] == source(level, node, j),
                     "benchmark path chunk mismatch");
      }
    }
    if (measured) {
      query_ms.push_back(q_ms);
      unpack_ms.push_back(u_ms);
      path_ms.push_back(p_ms);
      extract_ms.push_back(e_ms);
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
  const size_t query_bytes = scheme.get_BFV_size(true);
  const size_t helper_bytes =
      scheme.with_query_shape({1, 0, std::bit_width(N) - 1})
          .get_bv_galois_key_size(true) +
      scheme.get_gsw_key_size(true);

  BENCH_PRINT("tree MVP 32-byte-node benchmark: N=2^" << tree.L
              << " leaves, L=" << tree.L << ", g="
              << tree.g << " (" << chunk_bits << "-bit chunks), N0="
              << tree.N0 << ", b=" << tree.b << ", rho=" << tree.rho
              << ", r=" << tree.r << ", w=" << tree.w << ", W=" << tree.W
              << ", h_q=" << tree.h_q);
  BENCH_PRINT("database: " << total_pt << " packed plaintexts, 32-byte nodes"
              " (" << (total_pt * scheme.get_pt_size()) / (1 << 20)
              << " MiB logical payload), setup " << setup_ms << " ms");
  BENCH_PRINT("trials: " << kTrials << " measured after " << kWarmups
              << " warmup");
  BENCH_PRINT("trial seed: " << trial_seed);
  BENCH_PRINT("client query avg " << avg(query_ms) << " ms");
  BENCH_PRINT("server unpack (expand+convert) avg " << avg(unpack_ms)
              << " ms");
  BENCH_PRINT("server path (select+rotate+project+pack) avg "
              << avg(path_ms) << " ms");
  BENCH_PRINT("server total avg " << avg(unpack_ms) + avg(path_ms)
              << " ms; samples(path):");
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
              << actual_response_bytes
              << " B (actual wire codec), helper keys " << helper_bytes
              << " B (modeled)");
  BENCH_PRINT("payload: " << (tree.L + 1) << " x 32-byte path nodes = "
              << (tree.L + 1) * 32 << " B useful");
  tree_profile_json::write_report(
      tree_profile_json::output_path_from_env(), "legacy-tree-g32", tree.N,
      256, tree.N * 32, kWarmups, kTrials, trial_seed, false, "mvp",
      json_samples,
      {{"hardware_target", "CloudLab Xeon 6142 AVX-512 376GiB RAM"},
       {"payload_note", "g=32 carries full 32-byte nodes in 12-bit chunks"},
       {"chunk_bits", std::to_string(chunk_bits)},
       {"g", std::to_string(tree.g)}});
}
