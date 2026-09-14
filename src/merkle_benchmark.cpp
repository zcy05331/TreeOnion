#include "merkle_benchmark_internal.h"

#include "client.h"
#include "layer_layout_planner.h"
#include "logging.h"
#include "server.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

namespace {

std::string environment_value(const char *name,
                              const std::string &fallback = "") {
  const char *value = std::getenv(name);
  return value == nullptr ? fallback : std::string(value);
}

uint64_t splitmix64_next(uint64_t &state) {
  state += 0x9e3779b97f4a7c15ULL;
  uint64_t value = state;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

}  // namespace

MerkleWorkload make_benchmark_workload(size_t leaf_count) {
  if (leaf_count < 2 || !std::has_single_bit(leaf_count)) {
    throw std::invalid_argument(
        "Merkle benchmark leaf count must be a power of two >= 2");
  }
  return {leaf_count,
          static_cast<size_t>(std::bit_width(leaf_count) - 1), 32};
}

PirParams make_benchmark_reference(const MerkleWorkload &workload) {
  PirParams scheme;
  if (scheme.get_poly_degree() != 2048 || scheme.get_ct_mod_width() != 58 ||
      scheme.get_num_bits_per_coeff() != 12 || scheme.get_l() != 6 ||
      scheme.get_l_key() != 10 || DBConsts::L_KS != 8 ||
      std::bit_width(scheme.get_small_q() - 1) != 22 ||
      scheme.get_noise_std_dev() != 2.55) {
    throw std::invalid_argument(
        "Merkle benchmarks require CONFIG_N2048_K1_COMP v2 parameters");
  }

  const size_t target_num_pt = flat_target_num_pt(workload);
  if (workload.leaf_count >= (size_t{1} << 24)) {
    return scheme.with_layout({target_num_pt, 10, true});
  }
  for (size_t height = 0; height <= 10; ++height) {
    try {
      return scheme.with_layout({target_num_pt, height, true});
    } catch (const std::runtime_error &) {
      // Try the next expansion height.
    }
  }
  throw std::runtime_error("No v2 reference layout for benchmark workload");
}

namespace {

std::vector<BenchmarkCaseResult> execute_case_set(
    const MerkleWorkload &workload, const PirParams &reference,
    const BenchmarkTrialPlan &trials, BenchmarkCaseSelection selection,
    const std::string &name_suffix,
    const LayerPlannerConfig &planner) {
  if (selection != BenchmarkCaseSelection::all &&
      selection != BenchmarkCaseSelection::standard_onionpir &&
      selection != BenchmarkCaseSelection::merkle_paths &&
      selection != BenchmarkCaseSelection::merkle_layerwise) {
    throw std::invalid_argument("Unknown benchmark case selection");
  }

  std::vector<BenchmarkCaseResult> results;
  if (selection == BenchmarkCaseSelection::all ||
      selection == BenchmarkCaseSelection::standard_onionpir) {
    BenchmarkCaseExecution standard =
        run_standard_case(workload, reference, trials);
    standard.result.name += name_suffix;
    results.push_back(std::move(standard.result));
    // Return before constructing either Merkle-path database. This keeps a
    // Standard-only repetition independent of path setup costs.
    if (selection == BenchmarkCaseSelection::standard_onionpir) {
      return results;
    }
  }

  if (selection == BenchmarkCaseSelection::merkle_layerwise) {
    BenchmarkCaseExecution layerwise =
        run_merkle_layerwise_case(workload, reference, trials, planner);
    layerwise.result.name += name_suffix;
    results.push_back(std::move(layerwise.result));
    return results;
  }

  BenchmarkCaseExecution flat =
      run_merkle_flat_case(workload, reference, trials);
  BenchmarkCaseExecution layerwise =
      run_merkle_layerwise_case(workload, reference, trials, planner);
  validate_layerwise_path_communication(
      flat.result.communication, layerwise.result.communication,
      layerwise.result.direct_return_levels,
      layerwise.result.direct_return_response_bytes);
  if (flat.measured_paths != layerwise.measured_paths) {
    throw std::runtime_error(
        "Flat and layerwise measured Merkle paths differ");
  }
  flat.result.name += name_suffix;
  layerwise.result.name += name_suffix;
  results.push_back(std::move(flat.result));
  results.push_back(std::move(layerwise.result));
  return results;
}

uint64_t detected_physical_memory_bytes() {
#if defined(__APPLE__)
  uint64_t bytes = 0;
  size_t length = sizeof(bytes);
  if (sysctlbyname("hw.memsize", &bytes, &length, nullptr, 0) == 0) {
    return bytes;
  }
  return 0;
#else
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long page_size = sysconf(_SC_PAGESIZE);
  if (pages <= 0 || page_size <= 0) return 0;
  return checked_multiply(static_cast<uint64_t>(pages),
                          static_cast<uint64_t>(page_size),
                          "detected physical memory");
#endif
}

bool running_under_rosetta() {
#if defined(__APPLE__) && defined(__x86_64__)
  int translated = 0;
  size_t length = sizeof(translated);
  return sysctlbyname("sysctl.proc_translated", &translated, &length,
                      nullptr, 0) == 0 && translated == 1;
#else
  return false;
#endif
}

std::string paper_row_for(const MerkleWorkload &workload) {
  if (workload.leaf_count == (size_t{1} << 24)) return "1 GB";
  if (workload.leaf_count == (size_t{1} << 26)) return "4 GB";
  if (workload.leaf_count == (size_t{1} << 27)) return "8 GB";
  return "custom";
}

void append_case_set(std::vector<BenchmarkCaseResult> &destination,
                     std::vector<BenchmarkCaseResult> source) {
  destination.insert(destination.end(),
                     std::make_move_iterator(source.begin()),
                     std::make_move_iterator(source.end()));
}

}  // namespace

BenchmarkTrialPlan make_benchmark_trial_plan(size_t leaf_count, size_t warmups,
                                             size_t measured, uint64_t seed) {
  if (leaf_count == 0) {
    throw std::invalid_argument(
        "Merkle benchmark requires at least one query ID");
  }
  if (measured == 0) {
    throw std::invalid_argument(
        "Merkle benchmark requires at least one measured trial");
  }
  if (warmups > leaf_count || measured > leaf_count - warmups) {
    throw std::invalid_argument(
        "Merkle benchmark requires distinct query IDs for every trial");
  }

  const size_t query_count = warmups + measured;
  std::vector<size_t> selected;
  selected.reserve(query_count);
  uint64_t state = seed;

  // Floyd's algorithm samples without replacement in O(query_count^2) space
  // independent of the database size. query_count is only 8 in the frozen
  // benchmark, so the linear membership check is both bounded and explicit.
  for (size_t offset = 0; offset < query_count; ++offset) {
    const size_t upper = leaf_count - query_count + offset;
    const size_t candidate =
        static_cast<size_t>(splitmix64_next(state) % (upper + 1));
    selected.push_back(std::find(selected.begin(), selected.end(), candidate) ==
                               selected.end()
                           ? candidate
                           : upper);
  }

  BenchmarkTrialPlan plan;
  plan.warmup_leaf_indices.assign(selected.begin(),
                                  selected.begin() + warmups);
  plan.measured_leaf_indices.assign(selected.begin() + warmups,
                                    selected.end());
  return plan;
}

uint64_t estimate_merkle_benchmark_peak_bytes(size_t leaf_count) {
  const MerkleWorkload workload = make_benchmark_workload(leaf_count);
  const PirParams reference = make_benchmark_reference(workload);
  const uint64_t flat_bytes = physical_database_bytes(reference);
  const std::vector<LayerLayout> layouts =
      plan_layer_layouts(workload.tree_height, 96, reference);
  uint64_t layerwise_bytes = 0;
  for (const LayerLayout &layout : layouts) {
    layerwise_bytes = checked_add(layerwise_bytes,
                                  physical_database_bytes(layout.params),
                                  "layerwise peak bytes");
  }
  return std::max(flat_bytes, layerwise_bytes);
}

BenchmarkEnvironment detect_benchmark_environment() {
  BenchmarkEnvironment environment;
  environment.commit = environment_value("ONIONPIR_BENCH_COMMIT");
  environment.branch = environment_value("ONIONPIR_BENCH_BRANCH");
  environment.build_type = "Benchmark";
  environment.config = "CONFIG_N2048_K1_COMP";
#if defined(__x86_64__)
  environment.architecture = "x86_64";
#elif defined(__aarch64__) || defined(__arm64__)
  environment.architecture = "arm64";
#else
  environment.architecture = "unknown";
#endif
  environment.process_architecture =
      environment_value("ONIONPIR_BENCH_PROCESS_ARCH", "x86_64");
  environment.operating_system =
      environment_value("ONIONPIR_BENCH_OS");
  environment.cpu = environment_value("ONIONPIR_BENCH_CPU");
  environment.compiler =
      environment_value("ONIONPIR_BENCH_COMPILER");
  environment.cmake_version =
      environment_value("ONIONPIR_BENCH_CMAKE");
#if defined(ONIONPIR_USE_HEXL)
  environment.hexl_enabled = true;
#else
  environment.hexl_enabled = false;
#endif
  environment.hexl_version = "1.2.6";
  environment.hexl_path = environment_value("ONIONPIR_BENCH_HEXL_PATH");
  environment.rosetta = running_under_rosetta();
  environment.non_native_label = environment.rosetta
      ? "x86_64 + Intel HEXL under Rosetta 2 on Apple M4; non-native result"
      : "This run is not the frozen Apple M4 Rosetta benchmark environment";
  return environment;
}

// Per-workload planner config: the profile whose tree height matches, else the
// primary one (validation then reports the mismatch or falls back).
LayerPlannerConfig planner_for_workload(const MerkleBenchmarkOptions &options,
                                        const MerkleWorkload &workload) {
  LayerPlannerConfig config = options.layer_planner;
  if (config.policy != LayerLayoutPolicy::profiled) return config;
  for (const auto &profile : options.layer_profiles) {
    if (profile && profile->tree_height == workload.tree_height) {
      config.profile = profile;
      return config;
    }
  }
  return config;
}

BenchmarkReport run_merkle_benchmark_suite(
    const MerkleBenchmarkOptions &options) {
  constexpr size_t kPrimaryMaxLeaves = size_t{1} << 27;
  if (options.leaf_count > kPrimaryMaxLeaves) {
    throw std::invalid_argument(
        "Primary Merkle benchmark is capped at 2^27 leaves");
  }
  const MerkleWorkload workload =
      make_benchmark_workload(options.leaf_count);
  if (workload.leaf_count > (size_t{1} << 24)) {
    const uint64_t estimated =
        estimate_merkle_benchmark_peak_bytes(workload.leaf_count);
    const uint64_t physical_memory = detected_physical_memory_bytes();
    constexpr uint64_t safety_margin = uint64_t{2} << 30;
    if (physical_memory == 0 ||
        estimated > physical_memory ||
        safety_margin > physical_memory - estimated) {
      throw std::invalid_argument(
          "Primary Merkle benchmark estimated peak " +
          std::to_string(estimated) +
          " bytes plus 2147483648-byte safety margin exceeds detected " +
          std::to_string(physical_memory) + " bytes");
    }
  }
  const PirParams reference = make_benchmark_reference(workload);
  const BenchmarkTrialPlan trials = make_benchmark_trial_plan(
      workload.leaf_count, options.warmups, options.measured_trials,
      options.trial_seed);

  BenchmarkReport report;
  // v2 corrects paper_server_throughput_MBps to database_bytes/server_time and
  // retains repeated work as the explicit paper_scan_throughput diagnostic.
  report.schema_version = "onionpir-merkle-baselines-v2";
  report.environment = detect_benchmark_environment();

  report.paper_alignment.paper =
      "OnionPIRv2: Efficient Single-Server PIR (2025/1142)";
  report.paper_alignment.revision = "2025-1142.pdf";
  report.paper_alignment.sections = {"4.1", "4.2", "4.3"};
  report.paper_alignment.poly_degree = 2048;
  report.paper_alignment.log_q = 58;
  report.paper_alignment.log_t = 13;
  report.paper_alignment.log_q_prime = 22;
  report.paper_alignment.L_KEY = 10;
  report.paper_alignment.L_EP = 6;
  report.paper_alignment.L_KS = 8;
  report.paper_alignment.sigma = 2.55;
  report.paper_alignment.estimated_security_bits = 117;
  report.paper_alignment.reference_hardware =
      "Intel Xeon Platinum 8358 2.60 GHz, Ubuntu 22.04, AVX-512";
  report.paper_alignment.local_result_is_hardware_replication = false;

  report.workload.leaf_count = workload.leaf_count;
  report.workload.tree_height = workload.tree_height;
  report.workload.node_bytes = workload.node_bytes;
  report.workload.nodes_per_plaintext = 96;
  report.workload.paper_row = paper_row_for(workload);
  report.workload.warmups = options.warmups;
  report.workload.measured_trials = options.measured_trials;
  report.workload.trial_seed = options.trial_seed;
  report.workload.warmup_leaf_indices.assign(
      trials.warmup_leaf_indices.begin(), trials.warmup_leaf_indices.end());
  report.workload.trial_leaf_indices.assign(
      trials.measured_leaf_indices.begin(), trials.measured_leaf_indices.end());

  append_case_set(report.cases,
                  execute_case_set(workload, reference, trials,
                                   options.case_selection, "", planner_for_workload(options, workload)));

  OptionalWorkloadResult optional;
  optional.leaf_count = size_t{1} << 26;
  optional.tree_height = 26;
  optional.paper_row = "4 GB";
  if (workload.leaf_count == optional.leaf_count) {
    optional.status = "primary_workload";
  } else if (!options.run_optional_4gb) {
    optional.status = "not_requested";
    optional.skip_reason = "optional 4 GB run flag was not set";
  } else {
    const uint64_t estimated =
        estimate_merkle_benchmark_peak_bytes(optional.leaf_count);
    const uint64_t physical_memory = detected_physical_memory_bytes();
    constexpr uint64_t safety_margin = uint64_t{2} << 30;
    if (physical_memory == 0 ||
        estimated > physical_memory ||
        safety_margin > physical_memory - estimated) {
      optional.status = "skipped_resource_limit";
      optional.skip_reason =
          "estimated peak " + std::to_string(estimated) +
          " bytes plus 2147483648-byte safety margin exceeds detected " +
          std::to_string(physical_memory) + " bytes";
    } else {
      optional.status = "completed";
      const MerkleWorkload optional_workload =
          make_benchmark_workload(optional.leaf_count);
      const PirParams optional_reference =
          make_benchmark_reference(optional_workload);
      // The optional tier is a resource probe, not the statistical anchor:
      // it always measures a fixed 4 trials regardless of --experiments.
      constexpr size_t kOptionalMeasuredTrials = 4;
      const BenchmarkTrialPlan optional_trials = make_benchmark_trial_plan(
          optional_workload.leaf_count, options.warmups,
          kOptionalMeasuredTrials,
          options.trial_seed ^ 0x8b47424950524952ULL);
      append_case_set(
          report.cases,
          execute_case_set(optional_workload, optional_reference,
                           optional_trials, options.case_selection,
                           "_optional_4gb", planner_for_workload(options, optional_workload)));
    }
  }
  report.workload.optional_workloads.push_back(std::move(optional));
  return report;
}

double parse_padding_budget(const std::string &text) {
  size_t parsed = 0;
  double value = 0.0;
  try {
    value = std::stod(text, &parsed);
  } catch (const std::exception &) {
    throw std::invalid_argument("--layer-padding-budget requires a ratio");
  }
  if (parsed != text.size() || !(value >= 1.0) || !std::isfinite(value)) {
    throw std::invalid_argument(
        "--layer-padding-budget must be a finite ratio >= 1.0");
  }
  return value;
}

LayerLayoutProfile run_layer_layout_sweep(
    const LayerLayoutSweepOptions &options) {
  if (options.measured_trials == 0) {
    throw std::invalid_argument("layer layout sweep needs measured trials");
  }
  if (!(options.padding_budget >= 1.0)) {
    throw std::invalid_argument("layer padding budget must be >= 1.0");
  }
  const MerkleWorkload workload = make_benchmark_workload(options.leaf_count);
  const PirParams reference = make_benchmark_reference(workload);
  const BenchmarkTrialPlan trials = make_benchmark_trial_plan(
      workload.leaf_count, options.warmups, options.measured_trials,
      options.trial_seed);
  constexpr size_t kNodesPerPt = 96;

  LayerLayoutProfile profile;
  profile.environment = detect_layer_layout_environment(
      reference, environment_value("ONIONPIR_BENCH_COMMIT"), "Benchmark",
      "CONFIG_N2048_K1_COMP");
  profile.tree_height = workload.tree_height;
  profile.nodes_per_plaintext = kNodesPerPt;
  profile.warmups = options.warmups;
  profile.measured_trials = options.measured_trials;
  profile.trial_seed = options.trial_seed;
  profile.padding_budget = options.padding_budget;

  // One client and one shared helper-key bundle for every candidate server.
  PirClient client(reference);
  SharedPirSessionKeys keys = client.create_session_keys();

  for (size_t level = 1; level <= workload.tree_height; ++level) {
    const std::vector<LayerLayoutCandidate> candidates =
        enumerate_layer_layout_candidates(level, kNodesPerPt, reference);
    const size_t legacy = legacy_layer_layout_candidate(candidates);
    if (candidates[legacy].layout.direct_return) continue;  // no PIR here
    const std::vector<LayerLayoutCandidate> frontier =
        pareto_layer_layout_candidates(candidates);
    const auto on_frontier = [&](const LayerLayoutCandidate &c) {
      for (const LayerLayoutCandidate &f : frontier) {
        if (f.features.expansion_height == c.features.expansion_height) {
          return true;
        }
      }
      return false;
    };
    for (const LayerLayoutCandidate &candidate : candidates) {
      const bool dominated = !on_frontier(candidate);
      if (dominated && !options.include_dominated) continue;
      const PirParams &params = candidate.layout.params;
      // Sequential, memory-bounded: exactly one candidate server is alive.
      auto server = std::make_unique<PirServer>(params);
      server->set_client_session_keys(client.get_client_id(), keys);
      PlaintextSource source = [&](size_t index, RlwePt &out) {
        out = make_layer_merkle_plaintext(level, index, workload, params);
      };
      server->load_data(candidate.layout.target_num_pt, source);

      const auto run_query = [&](size_t leaf) {
        const size_t local =
            merkle_sibling_local(leaf, workload.tree_height, level);
        RlweCt query =
            client.fast_generate_query(params, local / kNodesPerPt);
        const auto start = BenchmarkClock::now();
        RlweCt response = server->make_query(client.get_client_id(), query);
        const double ms = milliseconds(elapsed_since(start));
        // Correctness outside the timer: the sibling must decode exactly.
        std::stringstream wire;
        (void)server->save_resp_to_stream(response, wire);
        const RlwePt pt = client.decrypt_mod_q(client.load_resp_from_stream(wire));
        if (decode_merkle_node(pt, local % kNodesPerPt, params) !=
            synthetic_merkle_node(level, local)) {
          throw std::runtime_error(
              "layer layout sweep: candidate response decoded to the wrong "
              "sibling at level " + std::to_string(level));
        }
        return ms;
      };
      for (size_t leaf : trials.warmup_leaf_indices) (void)run_query(leaf);
      LayerLayoutMeasurement measurement;
      measurement.level = level;
      measurement.features = candidate.features;
      measurement.dominated = dominated;
      for (size_t leaf : trials.measured_leaf_indices) {
        measurement.server_samples_ms.push_back(run_query(leaf));
      }
      measurement.median_server_ms =
          median_server_ms_of(measurement.server_samples_ms);
      BENCH_PRINT("layer " << level << " h=" << candidate.features.expansion_height
                  << " N0=" << candidate.features.first_dim_size
                  << " Nrest=" << candidate.features.other_dim_size
                  << " num_pt=" << candidate.features.padded_plaintexts
                  << (dominated ? " (dominated)" : "")
                  << ": median " << measurement.median_server_ms << " ms");
      profile.measurements.push_back(std::move(measurement));
    }
  }

  const LayerLayoutSelection selection = select_layer_layouts(
      profile, workload.tree_height, kNodesPerPt, reference,
      options.padding_budget);
  profile.selected_expansion_heights = selection.expansion_heights;
  profile.legacy_total_padded_plaintexts =
      selection.legacy_total_padded_plaintexts;
  profile.selected_total_padded_plaintexts =
      selection.selected_total_padded_plaintexts;
  BENCH_PRINT("layer layout sweep: legacy padded plaintexts "
              << selection.legacy_total_padded_plaintexts << ", selected "
              << selection.selected_total_padded_plaintexts
              << ", predicted server ms legacy "
              << selection.predicted_legacy_ms << " -> selected "
              << selection.predicted_selected_ms);
  return profile;
}
