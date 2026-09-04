#include "tree_superrecord_test_util.h"

#include "merkle_benchmark.h"
#include "tree_index.h"
#include "tree_query.h"
#include "tree_response.h"
#include "tree_select.h"
#include "tree_superrecord_response.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <numeric>
#include <sstream>

using namespace tree_superrecord;
using namespace tree_superrecord_test;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Stats {
  double mean = 0, median = 0, p95 = 0, stddev = 0;
};

Stats summarize(std::vector<double> v) {
  Stats s;
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  s.mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
  s.median = v.size() % 2 ? v[v.size() / 2]
                          : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
  s.p95 = v[std::min(v.size() - 1, static_cast<size_t>(std::ceil(0.95 * v.size())) - 1)];
  double acc = 0;
  for (double x : v) acc += (x - s.mean) * (x - s.mean);
  s.stddev = v.size() > 1 ? std::sqrt(acc / (v.size() - 1)) : 0;
  return s;
}

constexpr std::array<const char *, 10> kStageLabels = {
    "expand", "complete", "prepare", "scan", "finalize",
    "fold",   "rotate",   "project", "pack", "switch"};

struct RunResult {
  std::string label;
  std::string description;
  bool eligible = false;
  std::string reason;
  std::vector<double> server_ms;
  std::array<double, 10> stage_sum{};
  double setup_ms = 0;
  double query_ms_avg = 0;
  double extract_ms_avg = 0;
  size_t db_bytes = 0;
  size_t scan_bytes = 0;
  size_t plaintexts = 0;
  size_t response_bytes = 0;
  size_t query_bytes = 0;
  int min_budget = 0;
  uint64_t max_noise = 0;
  double neighbour_slots_avg = 0;
};

size_t env_size(const char *name, size_t fallback) {
  const char *value = std::getenv(name);
  if (!value || !*value) return fallback;
  return static_cast<size_t>(std::strtoull(value, nullptr, 10));
}

std::string mib(size_t bytes) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(1)
       << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MiB";
  return text.str();
}

}  // namespace

// Timed comparison at N = 2^22 leaves by default (--leaf-count selects
// another power of two): the planner's candidates for 256-bit entries under a
// one-response policy, measured through the full generic server wrapper
// (expansion, selector completion, ragged pyramid, m32 scan, fold, rotation,
// projection, pooled packing, modulus switch), against the legacy g=32 path
// on the same leaf schedule. Every trial's decoded path is checked against
// the synthetic tree outside the timers; a candidate is timed only after the
// noise gate on boundary leaves decrypts correctly with the policy margin.
// Environment: SUPERRECORD_MAX_FRONTIER (extra Pareto candidates, default 2),
// SUPERRECORD_MAX_ARITY (default 8), SUPERRECORD_SKIP_LEGACY=1.
void PirTest::test_tree_superrecord_benchmark() {
  print_func_name(__FUNCTION__);
  constexpr size_t n = DBConsts::PolyDegree;
  const size_t log_n = std::bit_width(n) - 1;
  PirParams scheme;
  const uint64_t t = scheme.get_plain_mod();
  const size_t c = payload_bits(scheme);
  require_applicable(scheme.get_composite_rns().enabled,
                     "the generic m32 benchmark needs the composite first "
                     "dimension (CONFIG_N2048_K1_COMP)");

  const size_t L = bench_tree_height(22);
  const size_t N = size_t{1} << L;
  const size_t kWarmups = bench_warmups.value_or(3);
  const size_t kTrials = bench_measured_trials.value_or(16);
  const uint64_t trial_seed = bench_trial_seed.value_or(0x747265654733325fULL);
  const BenchmarkTrialPlan plan =
      make_benchmark_trial_plan(N, kWarmups, kTrials, trial_seed);
  const size_t max_frontier = env_size("SUPERRECORD_MAX_FRONTIER", 2);
  const bool include_legacy = std::getenv("SUPERRECORD_SKIP_LEGACY") == nullptr;
  const size_t noise_margin_bits = 1;

  PirClient client(scheme);
  const EntrySource source = [&](size_t level, size_t node) {
    return entry_from_bytes(synthetic_tree_node_bytes(level, node), 256);
  };

  // ---- plan ----
  PlannerPolicy policy;
  policy.max_response_ciphertexts = 1;
  policy.max_bundle_arity = env_size("SUPERRECORD_MAX_ARITY", 8);
  policy.expansion_heights = {8, 9, 10};
  policy.alpha_alignments = {1, 16};
  policy.eval_key_registration = EvalKeyRegistrationPolicy::FixedMaximumHeight;
  policy.fixed_session_key_height = log_n;
  policy.min_noise_margin = noise_margin_bits;
  const PlannerInput input = make_input(scheme, 256, N, policy);
  std::vector<Candidate> all = enumerate_candidates(input);
  const CostCalibration calibration;
  rank_by_prediction(all, calibration);
  std::vector<Candidate> frontier = pareto_frontier(all);
  rank_by_prediction(frontier, calibration);

  std::vector<Candidate> sweep;
  std::vector<std::string> labels;
  const auto add = [&](const Candidate &cand, const std::string &label) {
    const PlanTag tag = make_plan_tag(cand.shape, cand.scheme);
    for (const Candidate &existing : sweep) {
      if (make_plan_tag(existing.shape, existing.scheme) == tag) return;
    }
    sweep.push_back(cand);
    labels.push_back(label);
  };
  for (const Candidate &cand : all) {  // best predicted Bundle3 point
    if (cand.shape.bundle_arity == 3 && cand.shape.record_coeffs == 64 &&
        cand.shape.plane_count == 1) {
      add(cand, "bundle3");
      break;
    }
  }
  for (const Candidate &cand : all) {  // best predicted h = 1 control
    if (cand.shape.bundle_arity == 1) {
      add(cand, "generic-h1");
      break;
    }
  }
  // The legacy layout at this L: g slots per node and the widest power-of-two
  // N0 whose packed query fits the scheme's default expansion height. The
  // same (h = 1, G = g, N0, b) shape through the generic path isolates the
  // implementation overhead from the packing gain.
  const size_t legacy_chunk_bits = std::bit_width(t) - 1;
  const size_t legacy_g = std::bit_ceil((size_t{255} / legacy_chunk_bits) + 1);
  size_t legacy_a = 2;
  for (size_t a = 20; a >= 2; --a) {
    try {
      (void)make_tree_pir_params_for_scheme(L, a, legacy_g, scheme,
                                           scheme.get_expan_height());
      legacy_a = a;
      break;
    } catch (const std::invalid_argument &) {
    }
  }
  const TreePirParams legacy_tree = make_tree_pir_params_for_scheme(
      L, legacy_a, legacy_g, scheme, scheme.get_expan_height());
  for (const Candidate &cand : all) {
    if (cand.shape.bundle_arity == 1 && cand.shape.record_coeffs == legacy_g &&
        cand.shape.plane_count == 1 && cand.shape.N0 == legacy_tree.N0 &&
        cand.shape.beta_bits == legacy_tree.b) {
      add(cand, "generic-legacy-shape");
      break;
    }
  }
  size_t extra = 0;
  for (const Candidate &cand : frontier) {
    if (extra >= max_frontier) break;
    const size_t before = sweep.size();
    add(cand, "frontier-" + std::to_string(extra + 1));
    if (sweep.size() > before) ++extra;
  }
  BENCH_PRINT("planner: E=256 c=" << c << " N=2^" << L << ": " << all.size()
              << " candidates, Pareto frontier " << frontier.size()
              << ", sweep " << sweep.size());
  for (size_t i = 0; i < sweep.size(); ++i) {
    BENCH_PRINT("  [" << labels[i] << "] " << describe_candidate(sweep[i]));
  }

  std::vector<RunResult> results;

  // ---- legacy g = 32 control (same leaf schedule) ----
  if (include_legacy) {
    RunResult r;
    r.label = "legacy-g32";
    const size_t g = legacy_g;
    const TreePirParams &tree = legacy_tree;
    const TreeNodeChunkSource chunk_source = [&](size_t level, size_t index,
                                                 size_t chunk) {
      return synthetic_tree_node_bytes_chunk(level, index, chunk, t);
    };
    SharedPirSessionKeys keys = client.create_session_keys(log_n);
    PirServer server(tree_query_expansion_params(tree, scheme));
    server.set_client_session_keys(client.get_client_id(), keys);
    const auto setup_start = Clock::now();
    const ::PreprocessedTree legacy_db =
        preprocess_tree_mvp(tree, chunk_source, scheme);
    r.setup_ms = ms_since(setup_start);
    for (const auto &level_db : legacy_db.canonical) r.plaintexts += level_db.plaintexts.size();
    r.db_bytes = r.plaintexts * n * sizeof(uint64_t) * 2;  // canonical + m32
    r.scan_bytes = r.plaintexts * n * 2 * sizeof(uint32_t);
    {
      std::ostringstream d;
      d << "g=" << g << " N0=" << tree.N0 << " b=" << tree.b << " rho=" << tree.rho
        << " w=" << tree.w << " h_q=" << tree.h_q << " plaintexts=" << r.plaintexts;
      r.description = d.str();
    }
    constexpr std::array<const char *, 7> kLegacyStages = {
        TREE_PYRAMID_TIME, TREE_SCAN_TIME,   TREE_FOLD_TIME, TREE_ROTATE_TIME,
        TREE_PROJECT_TIME, TREE_PACK_TIME,   TREE_SWITCH_TIME};
    constexpr std::array<size_t, 7> kLegacySlots = {2, 3, 5, 6, 7, 8, 9};
    CLEAN_TIMER();
    double query_sum = 0, extract_sum = 0;
    int min_budget = 1 << 20;
    const auto run_trial = [&](size_t leaf, bool measured) {
      auto start = Clock::now();
      RlweCt query = make_tree_query(client, scheme, tree, leaf);
      const double q_ms = ms_since(start);
      start = Clock::now();
      ExpandedTreeQuery unpacked =
          unpack_tree_query(server, scheme, tree, client.get_client_id(), query);
      const double u_ms = ms_since(start);  // expansion + selector completion
      start = Clock::now();
      TreePathResponse response = answer_path_mvp(
          legacy_db, unpacked, server, client.get_client_id(), tree, scheme);
      const double s_ms = u_ms + ms_since(start);
      start = Clock::now();
      uint64_t noise = 0;
      int budget = 0;
      const RlwePt pt = client.decrypt_mod_q_measured(response.chunks[0], noise, budget);
      const double e_ms = ms_since(start);
      END_EXPERIMENT();
      for (size_t level = 0; level <= tree.L; ++level) {
        const size_t node = leaf >> (tree.L - level);
        for (size_t j = 0; j < tree.g; ++j) {
          require_test(pt.data[response.level_offsets[level] + j * tree.rho] ==
                           chunk_source(level, node, j),
                       "legacy path chunk mismatch");
        }
      }
      if (measured) {
        r.server_ms.push_back(s_ms);
        query_sum += q_ms;
        extract_sum += e_ms;
        r.stage_sum[0] += u_ms;  // legacy unpack is not split into expand/complete
        for (size_t i = 0; i < kLegacyStages.size(); ++i) {
          r.stage_sum[kLegacySlots[i]] += GET_LAST_TIME(kLegacyStages[i]);
        }
        min_budget = std::min(min_budget, budget);
        r.max_noise = std::max(r.max_noise, noise);
        std::stringstream wire;
        r.response_bytes = server.save_resp_to_stream(response.chunks[0], wire);
      }
    };
    for (size_t leaf : plan.warmup_leaf_indices) run_trial(leaf, false);
    for (size_t leaf : plan.measured_leaf_indices) run_trial(leaf, true);
    r.query_ms_avg = query_sum / kTrials;
    r.extract_ms_avg = extract_sum / kTrials;
    r.query_bytes = scheme.get_BFV_size(true);
    r.min_budget = min_budget;
    r.eligible = true;
    results.push_back(std::move(r));
  }

  // ---- generic candidates ----
  for (size_t i = 0; i < sweep.size(); ++i) {
    const Candidate &cand = sweep[i];
    RunResult r;
    r.label = labels[i];
    r.description = describe_candidate(cand);
    const TreeParams params = freeze_candidate(cand);
    PirServer server(query_expansion_params(params, scheme));
    const TaggedTreeSessionKeys session =
        create_tagged_tree_session_keys(client, params, scheme);
    set_tree_client_session_keys(server, params, scheme, session);
    const auto setup_start = Clock::now();
    const tree_superrecord::PreprocessedTree db =
        preprocess_m32(params, scheme, source, false);
    r.setup_ms = ms_since(setup_start);
    r.db_bytes = m32_database_bytes(db);
    r.scan_bytes = cand.metrics.physical_scan_bytes;
    r.plaintexts = cand.metrics.physical_matrix_cells;
    r.query_bytes = scheme.get_BFV_size(true);

    const auto verify = [&](size_t leaf, const DecodedPath &decoded) {
      require_test(decoded.entries.size() == L + 1, "generic path length");
      for (size_t level = 0; level <= L; ++level) {
        const size_t node = leaf >> (L - level);
        require_test(decoded.entries[level] == source(level, node),
                     "generic path entry mismatch");
      }
    };
    // Noise gate on the boundary leaves before any timing.
    int min_budget = 1 << 20;
    bool gate_ok = true;
    std::string gate_reason;
    for (size_t leaf : {size_t{0}, N - 1, plan.warmup_leaf_indices.front()}) {
      try {
        const PackedQuery query = make_query(client, session, scheme, params, leaf);
        const PathResponse response =
            serve_request_m32(query, db, server, session, params, scheme);
        const DecodedPath decoded =
            extract_path_measured(response, client, params, scheme, leaf);
        verify(leaf, decoded);
        min_budget = std::min(min_budget, decoded.min_budget_bits);
        r.max_noise = std::max(r.max_noise, decoded.max_noise);
      } catch (const std::exception &error) {
        gate_ok = false;
        gate_reason = error.what();
        break;
      }
    }
    if (gate_ok && min_budget < static_cast<int>(policy.min_noise_margin)) {
      gate_ok = false;
      gate_reason = "noise budget " + std::to_string(min_budget) +
                    " bits below the policy margin";
    }
    if (!gate_ok) {
      r.eligible = false;
      r.reason = gate_reason;
      r.min_budget = min_budget;
      BENCH_PRINT("[" << r.label << "] ineligible: " << gate_reason);
      results.push_back(std::move(r));
      continue;
    }

    double query_sum = 0, extract_sum = 0, neighbours = 0;
    const auto run_trial = [&](size_t leaf, bool measured) {
      auto start = Clock::now();
      const PackedQuery query = make_query(client, session, scheme, params, leaf);
      const double q_ms = ms_since(start);
      TreeServerTiming timing{};
      const PathResponse response =
          serve_request_m32(query, db, server, session, params, scheme, &timing);
      start = Clock::now();
      const DecodedPath decoded =
          extract_path_measured(response, client, params, scheme, leaf);
      const double e_ms = ms_since(start);
      verify(leaf, decoded);
      if (measured) {
        r.server_ms.push_back(stage_ms(timing.total));
        query_sum += q_ms;
        extract_sum += e_ms;
        const std::array<TreeStageDuration, 10> stages = {
            timing.query_expand, timing.selector_completion,
            timing.alpha_prepare, timing.alpha_scan, timing.first_dim_finalize,
            timing.beta_fold, timing.private_rotate, timing.projection,
            timing.path_pack, timing.modulus_switch};
        for (size_t k = 0; k < stages.size(); ++k) r.stage_sum[k] += stage_ms(stages[k]);
        min_budget = std::min(min_budget, decoded.min_budget_bits);
        r.max_noise = std::max(r.max_noise, decoded.max_noise);
        const std::optional<size_t> bytes =
            serialized_response_payload_bytes(response, server, params);
        r.response_bytes = bytes.value_or(0);
        for (size_t level = 0; level <= L; ++level) {
          neighbours += static_cast<double>(real_bundle_slots(leaf, level, params) - 1);
        }
      }
    };
    for (size_t leaf : plan.warmup_leaf_indices) run_trial(leaf, false);
    for (size_t leaf : plan.measured_leaf_indices) run_trial(leaf, true);
    r.query_ms_avg = query_sum / kTrials;
    r.extract_ms_avg = extract_sum / kTrials;
    r.neighbour_slots_avg = neighbours / kTrials;
    r.min_budget = min_budget;
    r.eligible = true;
    results.push_back(std::move(r));
  }

  // ---- report ----
  BENCH_PRINT("");
  BENCH_PRINT("generic super-record benchmark: N=2^" << L << " leaves, E=256 bits, "
              << kTrials << " measured trials after " << kWarmups
              << " warmup, trial seed " << trial_seed);
  double legacy_mean = 0;
  for (const RunResult &r : results) {
    if (r.label == "legacy-g32" && r.eligible) legacy_mean = summarize(r.server_ms).mean;
  }
  for (const RunResult &r : results) {
    BENCH_PRINT("[" << r.label << "] " << r.description);
    if (!r.eligible) {
      BENCH_PRINT("  ineligible: " << r.reason);
      continue;
    }
    const Stats s = summarize(r.server_ms);
    BENCH_PRINT("  database " << mib(r.db_bytes) << " (" << r.plaintexts
                << " plaintext cells, scan " << mib(r.scan_bytes)
                << " per query), setup " << r.setup_ms << " ms");
    BENCH_PRINT("  server total mean " << s.mean << " ms, median " << s.median
                << ", p95 " << s.p95 << ", stddev " << s.stddev
                << (legacy_mean > 0 ? " ; speedup vs legacy " +
                                          std::to_string(legacy_mean / s.mean) + "x"
                                    : std::string()));
    std::string breakdown = "  stages avg (ms):";
    double stage_total = 0;
    for (size_t k = 0; k < kStageLabels.size(); ++k) {
      const double v = r.stage_sum[k] / r.server_ms.size();
      stage_total += v;
      breakdown += std::string(" ") + kStageLabels[k] + " " + std::to_string(v);
    }
    BENCH_PRINT(breakdown);
    BENCH_PRINT("  stages sum " << stage_total << " ms; client query "
                << r.query_ms_avg << " ms, client extract " << r.extract_ms_avg
                << " ms");
    BENCH_PRINT("  throughput " << static_cast<double>(r.scan_bytes) /
                                       (1024.0 * 1024.0) / (s.mean / 1000.0)
                << " MiB/s scanned; communication: query " << r.query_bytes
                << " B (modeled), response " << r.response_bytes
                << " B (actual codec); noise: min budget " << r.min_budget
                << " bits, max residue " << r.max_noise << " of decoder bound "
                << static_cast<double>(scheme.get_small_q()) / (2.0 * t)
                << "; real non-target neighbour slots/trial "
                << r.neighbour_slots_avg);
    std::string samples = "  samples(server):";
    for (double v : r.server_ms) samples += " " + std::to_string(v);
    BENCH_PRINT(samples);
  }
}
