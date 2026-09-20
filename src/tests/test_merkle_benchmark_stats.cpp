#include "tests.h"
#include <filesystem>
#include "merkle_benchmark.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

void test_merkle_server_phases();
void test_merkle_trial_schedule();
void test_merkle_single_sample_stats();

void require_trial_plan_and_basic_statistics() {
  BenchmarkTrialPlan distinct_queries =
      make_benchmark_trial_plan(8, 3, 5, 0x123456789abcdef0ULL);
  std::vector<size_t> all_query_ids = distinct_queries.warmup_leaf_indices;
  all_query_ids.insert(all_query_ids.end(),
                       distinct_queries.measured_leaf_indices.begin(),
                       distinct_queries.measured_leaf_indices.end());
  std::sort(all_query_ids.begin(), all_query_ids.end());
  require_test(all_query_ids.size() == 8,
               "trial planner omitted query IDs");
  require_test(std::adjacent_find(all_query_ids.begin(), all_query_ids.end()) ==
                   all_query_ids.end(),
               "trial planner repeated a query ID");

  bool rejected_too_many_distinct_queries = false;
  try {
    (void)make_benchmark_trial_plan(4, 2, 3, 7);
  } catch (const std::invalid_argument &) {
    rejected_too_many_distinct_queries = true;
  }
  require_test(rejected_too_many_distinct_queries,
               "trial planner accepted more queries than leaves");

  const MetricSampleStatistics basic_statistics =
      summarize_metric_samples({1.0, 2.0, 3.0, 4.0});
  require_test(basic_statistics.count == 4, "sample count");
  require_test(std::abs(basic_statistics.mean - 2.5) < 1e-12,
               "sample mean");
  require_test(
      std::abs(basic_statistics.population_variance - 1.25) < 1e-12,
      "population variance");
  require_test(basic_statistics.sample_variance.has_value() &&
                   std::abs(*basic_statistics.sample_variance -
                            (5.0 / 3.0)) < 1e-12,
               "sample variance");
}

BenchmarkReport make_and_check_report_statistics_fixture() {
  PirParams reference = PirParams().with_layout({349526, 10, true});
  // Freeze v2 seed-compressed request/helper models independently of the
  // response serializer, whose byte count is measured at runtime.
  require_test(reference.get_BFV_size(true) == 14880, "query bytes");
  require_test(modeled_helper_key_bytes(reference) == 1488000,
               "helper bytes");

  CommunicationStats standard =
      communication_stats(reference, 1, {11264});
  require_test(standard.online_total_bytes_mixed == 26144,
               "standard online");
  require_test(standard.first_session_total_bytes_mixed == 1514144,
               "standard first session");

  // Both naive path baselines make H calls and share one helper bundle, so
  // their online and first-session totals must match exactly.
  CommunicationStats path = communication_stats(
      reference, 24, std::vector<size_t>(24, 11264));
  require_test(path.online_total_bytes_mixed == 627456, "path online");
  require_test(path.first_session_total_bytes_mixed == 2115456,
               "path first session");
  validate_matching_path_communication(path, path);
  bool rejected_path_mismatch = false;
  try {
    CommunicationStats mismatched = path;
    --mismatched.online_response_bytes_actual;
    validate_matching_path_communication(path, mismatched);
  } catch (const std::invalid_argument &) {
    rejected_path_mismatch = true;
  }
  require_test(rejected_path_mismatch,
               "accepted flat/layerwise communication mismatch");

  // Model a flat H=24 trial: one ~1 GiB plaintext database, 24 repeated scans,
  // and one full-path server time. Paper throughput must use the database
  // footprint once; paper_scan_throughput retains the repeated-work diagnostic.
  BenchmarkCaseResult result;
  result.name = "merkle_flat";
  result.correctness_passed = true;
  result.communication = standard;
  result.raw_dataset_bytes = 1073741760;
  result.paper_plaintext_database_bytes = 1073743872;
  result.logical_padded_database_bytes = 1074266112;
  result.paper_plaintext_scan_bytes = 25769852928;
  result.logical_padded_scan_bytes = 25782386688;
  result.raw_application_scan_bytes = 25769852928;
  result.physical_preprocessed_storage_bytes = 5729419264;
  result.useful_response_bytes = 768;
  result.timing.setup = std::chrono::milliseconds(12);
  result.timing.client_query = std::chrono::milliseconds(1);
  result.timing.server_compute = std::chrono::milliseconds(250);
  result.timing.response_serialize = std::chrono::milliseconds(2);
  result.timing.response_load_decrypt_extract = std::chrono::milliseconds(3);
  result.server_compute_samples_ms = {200.0, 250.0, 300.0};
  finalize_case_statistics(result);
  require_test(result.paper_server_throughput_MBps > 4095.0 &&
                   result.paper_server_throughput_MBps < 4097.0,
               "paper throughput used repeated scan bytes as database size");
  require_test(result.server_compute_ms_statistics.has_value() &&
                   result.server_compute_ms_statistics->count == 3,
               "server sample statistics missing");
  require_test(
      result.paper_server_throughput_MBps_statistics.has_value() &&
          result.paper_server_throughput_samples_MBps.size() == 3,
      "throughput sample statistics missing");
  // JSON must keep the primary and scan metrics distinct and preserve bytes.
  BenchmarkReport report;
  report.schema_version = "onionpir-merkle-baselines-v2";
  report.environment.architecture = "x86_64";
  report.environment.hexl_enabled = true;
  report.environment.rosetta = true;
  report.environment.non_native_label =
      "x86_64 + Intel HEXL under Rosetta 2 on Apple M4; non-native result";
  report.paper_alignment.poly_degree = 2048;
  report.workload.leaf_count = size_t{1} << 24;
  report.workload.tree_height = 24;
  report.workload.node_bytes = 32;
  report.workload.nodes_per_plaintext = 96;
  report.workload.warmup_leaf_indices = {1, 2, 3};
  report.workload.trial_leaf_indices = {4, 5, 6, 7, 8};
  report.cases.push_back(result);

  const std::string path_name =
      (std::filesystem::temp_directory_path() /
       "onionpir-benchmark-stats-test.json")
          .string();
  write_benchmark_report_json(report, path_name);
  std::stringstream contents;
  {
    std::ifstream input(path_name);
    contents << input.rdbuf();
    require_test(input.good() || input.eof(),
                 "benchmark JSON was not readable");
  }
  std::filesystem::remove(path_name);
  require_test(contents.str().find("\"online_total_bytes_mixed\": 26144") !=
                   std::string::npos,
               "JSON omitted exact online bytes");
  require_test(contents.str().find(
                   "\"paper_server_throughput_MBps\": 4096.007812") !=
                   std::string::npos,
               "JSON did not use database size for paper throughput");
  require_test(contents.str().find(
                   "\"paper_scan_throughput_MBps\": 98304.187500") !=
                   std::string::npos,
               "JSON omitted the repeated-scan diagnostic throughput");
  require_test(contents.str().find(
                   "\"server_compute_samples_ms\": [200.000000, "
                   "250.000000, 300.000000]") != std::string::npos,
               "JSON omitted server-time samples");
  require_test(contents.str().find(
                   "\"population_variance\": 1666.666667") !=
                   std::string::npos,
               "JSON omitted server-time variance");
  require_test(contents.str().find(
                   "\"warmup_leaf_indices\": [1, 2, 3]") !=
                   std::string::npos,
               "JSON omitted warm-up query IDs");
  require_test(contents.str().find(
                   "\"trial_leaf_indices\": [4, 5, 6, 7, 8]") !=
                   std::string::npos,
               "JSON omitted measured query IDs");
  // Publication gates reject invalid denominators, inconsistent wire counts,
  // and attempts to run an oversized primary workload without the
  // resource-gated optional tier.
  bool rejected_zero_time = false;
  try {
    BenchmarkCaseResult invalid = result;
    invalid.timing.server_compute = BenchmarkDuration::zero();
    finalize_case_statistics(invalid);
  } catch (const std::invalid_argument &) {
    rejected_zero_time = true;
  }
  require_test(rejected_zero_time, "accepted a zero throughput denominator");

  bool rejected_count_mismatch = false;
  try {
    (void)communication_stats(reference, 2, {11264});
  } catch (const std::invalid_argument &) {
    rejected_count_mismatch = true;
  }
  require_test(rejected_count_mismatch,
               "accepted mismatched PIR call and response counts");
  return report;
}

void require_case_selection_cli_and_resource_guards(const BenchmarkReport &report) {
  PirParams reference = PirParams().with_layout({349526, 10, true});
  MerkleBenchmarkOptions standard_only_options;
  standard_only_options.leaf_count = 256;
  standard_only_options.warmups = 0;
  standard_only_options.measured_trials = 2;
  standard_only_options.trial_seed = 0x7374616e64617264ULL;
  standard_only_options.case_selection =
      BenchmarkCaseSelection::standard_onionpir;
  const BenchmarkReport standard_only_report =
      run_merkle_benchmark_suite(standard_only_options);
  require_test(standard_only_report.cases.size() == 1,
               "standard-only benchmark executed extra cases");
  require_test(standard_only_report.cases.front().name ==
                   "standard_onionpir",
               "standard-only benchmark omitted the requested case");
  const auto require_raw_profile_samples =
      [](const BenchmarkReport &profile_report, const char *label) {
        for (const BenchmarkCaseResult &profile_case : profile_report.cases) {
          const size_t measured =
              profile_report.workload.trial_leaf_indices.size();
          require_test(profile_case.sample_leaf_indices ==
                           profile_report.workload.trial_leaf_indices,
                       std::string(label) + " sample leaf mapping");
          require_test(profile_case.server_compute_samples_ms.size() ==
                           measured,
                       std::string(label) + " server sample count");
          require_test(profile_case.client_query_samples_ms.size() ==
                           measured,
                       std::string(label) + " client sample count");
          require_test(profile_case.response_serialize_samples_ms.size() ==
                           measured,
                       std::string(label) + " serialize sample count");
          require_test(
              profile_case.response_load_decrypt_extract_samples_ms.size() ==
                  measured,
              std::string(label) + " decode sample count");
          require_test(profile_case.pipeline_profile_samples_ms.size() ==
                           measured,
                       std::string(label) + " pipeline sample count");
          for (size_t i = 0; i < measured; ++i) {
            require_test(profile_case.pipeline_profile_samples_ms[i].leaf ==
                             profile_report.workload.trial_leaf_indices[i],
                         std::string(label) +
                             " pipeline sample leaf mapping");
            const auto &stages =
                profile_case.pipeline_profile_samples_ms[i].stages;
            require_test(stages.count("expand") == 1 &&
                             stages.count("mod_switch") == 1,
                         std::string(label) +
                             " pipeline sample stages missing");
            double stage_sum = 0.0;
            for (const auto &[stage, ms] : stages) stage_sum += ms;
            require_test(stage_sum >= 0.0 &&
                             stage_sum <=
                                 profile_case.server_compute_samples_ms[i],
                         std::string(label) +
                             " pipeline stages exceed server sample");
          }
        }
      };
  require_raw_profile_samples(standard_only_report, "standard");

  MerkleBenchmarkOptions paths_only_options = standard_only_options;
  paths_only_options.case_selection = BenchmarkCaseSelection::merkle_paths;
  const BenchmarkReport paths_only_report =
      run_merkle_benchmark_suite(paths_only_options);
  require_test(paths_only_report.cases.size() == 2,
               "paths-only benchmark executed the wrong case count");
  require_test(paths_only_report.cases[0].name == "merkle_flat" &&
                   paths_only_report.cases[1].name == "merkle_layerwise",
               "paths-only benchmark did not isolate both path cases");
  require_raw_profile_samples(paths_only_report, "paths");

  MerkleBenchmarkOptions layerwise_only_options = standard_only_options;
  layerwise_only_options.case_selection =
      BenchmarkCaseSelection::merkle_layerwise;
  const BenchmarkReport layerwise_only_report =
      run_merkle_benchmark_suite(layerwise_only_options);
  require_test(layerwise_only_report.cases.size() == 1,
               "layerwise-only selection must execute exactly one case");
  require_test(layerwise_only_report.cases[0].name == "merkle_layerwise",
               "layerwise-only selection must exclude Standard and Flat");

  // Planner CLI helpers and the new JSON fields.
  require_test(parse_padding_budget("1.01") == 1.01, "budget parses");
  const auto rejects_budget = [](const char *text) {
    try {
      (void)parse_padding_budget(text);
    } catch (const std::invalid_argument &) {
      return true;
    }
    return false;
  };
  require_test(rejects_budget("0.99"), "accepted a padding budget below 1.0");
  require_test(rejects_budget("abc"), "accepted a non-numeric padding budget");
  {
    const std::string path =
        (std::filesystem::temp_directory_path() / "merkle-stats-layers.json")
            .string();
    write_benchmark_report_json(layerwise_only_report, path);
    std::ifstream file(path);
    std::stringstream json;
    json << file.rdbuf();
    const std::string text = json.str();
    require_test(text.find("\"layer_layout_policy\": \"legacy\"") !=
                     std::string::npos,
                 "JSON omitted the layout policy");
    require_test(text.find("\"layers\": [{\"level\": 1") != std::string::npos,
                 "JSON omitted the per-level layouts");
    require_test(text.find("\"pipeline_profile_ms\": {") != std::string::npos,
                 "JSON omitted the pipeline profile");
    require_test(text.find("\"client_query_samples_ms\": [") !=
                     std::string::npos,
                 "JSON omitted client query samples");
    require_test(text.find("\"sample_leaf_indices\": [") !=
                     std::string::npos,
                 "JSON omitted case sample leaf IDs");
    require_test(text.find("\"pipeline_profile_samples_ms\": [") !=
                     std::string::npos,
                 "JSON omitted pipeline profile samples");
    require_test(text.find("\"leaf\": ") != std::string::npos,
                 "JSON omitted per-sample leaf mapping");
    std::filesystem::remove(path);
  }

  // Zero PIR calls have no communication model (the layerwise case rejects
  // an all-direct tree before reaching this point).
  bool rejected_zero_calls = false;
  try {
    (void)communication_stats(reference, 0, {}, 64);
  } catch (const std::invalid_argument &) {
    rejected_zero_calls = true;
  }
  require_test(rejected_zero_calls, "accepted a zero PIR call count");

  // The 4 GB resource gate's estimate grows with the workload and the
  // report printer accepts every case shape the suite produces.
  require_test(estimate_merkle_benchmark_peak_bytes(size_t{1} << 8) > 0 &&
                   estimate_merkle_benchmark_peak_bytes(size_t{1} << 12) >
                       estimate_merkle_benchmark_peak_bytes(size_t{1} << 8),
               "peak memory estimate is not monotone");
  print_benchmark_report(report);
  print_benchmark_report(layerwise_only_report);

  bool rejected_too_large_primary = false;
  try {
    MerkleBenchmarkOptions unsafe_options;
    unsafe_options.leaf_count = size_t{1} << 28;
    unsafe_options.warmups = 0;
    unsafe_options.measured_trials = 1;
    (void)run_merkle_benchmark_suite(unsafe_options);
  } catch (const std::invalid_argument &) {
    rejected_too_large_primary = true;
  }
  require_test(rejected_too_large_primary,
               "accepted a primary workload above 2^27 leaves");
}

void PirTest::test_merkle_benchmark_stats() {
  require_applicable(paper_config_active(),
                     paper_config_reason("the Merkle benchmark suite"));
  require_trial_plan_and_basic_statistics();


  const BenchmarkReport fixture_report = make_and_check_report_statistics_fixture();


  require_case_selection_cli_and_resource_guards(fixture_report);

}

// server_phase_ms is read out of the process-wide TimerLogger by
// ServerPhaseAccumulator: one END_EXPERIMENT per trial, warm-up trials flushed
// and discarded, sums divided by the measured-trial count. Every runner
// fixture in this file and in test_merkle_integration uses zero warm-ups, so
// the flush the accumulator exists for was never exercised. A warm-up leak
// (3 warm-ups + 2 measured over 2) reads ~2.5x, a missing per-trial
// END_EXPERIMENT reads a stale record or nothing, and dividing by all five
// trials reads 0.4x.
//
// The tight oracle is pipeline_profile_ms: make_query_profiled laps its own
// steady_clock on the same five section boundaries, and the runner sums that
// PirPipelineProfile over the measured trials only. Two independent
// accumulators over the same instructions, so a busy machine stretches both
// together; the two sums differ only by the timer bookkeeping inside the
// TimerLogger sections and by the part of evaluate_first_dim the pipeline does
// not lap, which is why the band is two-sided and slightly wider above 1.
// Measured on this 10-core box under 20-24 concurrent CPU hogs: 225 case
// readings, ratio 0.976-1.046, while server_compute itself moved 3 ms to
// 53 ms. The wall clock around make_query_profiled bounds the phases
// structurally -- they nest inside it, so exceeding it is a real defect -- but
// the glue outside them is allocation-heavy and absorbed up to 17% of a
// scheduling stall, so the wall-clock floor is a loose backstop rather than
// the check that does the work. Widening the pipeline band is not the fix for
// a failure here: these two clocks see the same code.
void test_merkle_server_phases() {
  require_applicable(paper_config_active(),
                     paper_config_reason("the Merkle benchmark suite"));
  const MerkleWorkload small{size_t{1} << 8, 8, 32};
  const PirParams reference = PirParams().with_layout({6, 3, true});
  const BenchmarkTrialPlan trials{{1, 2, 3}, {0, small.leaf_count - 1}};
  const auto check = [](const BenchmarkCaseExecution &execution) {
    const BenchmarkCaseResult &result = execution.result;
    double phase_sum = 0.0;
    for (const char *phase :
         {"expand", "convert", "first_dim", "other_dim", "mod_switch"}) {
      const auto it = result.server_phase_ms.find(phase);
      require_test(it != result.server_phase_ms.end() &&
                       std::isfinite(it->second) && it->second >= 0.0,
                   result.name + ": server_phase_ms lacks a finite " + phase);
      phase_sum += it->second;
    }
    require_test(result.server_phase_ms.size() == 5,
                 result.name + ": unexpected server phase keys");
    // The same five sections as timed by make_query_profiled itself: expand,
    // convert, the four first-dimension stages, other_dim, mod_switch.
    double pipeline_sum = 0.0;
    for (const auto &[stage, ms] : result.pipeline_profile_ms) {
      require_test(std::isfinite(ms) && ms >= 0.0,
                   result.name + ": pipeline_profile_ms stage " + stage +
                       " is not a finite duration");
      pipeline_sum += ms;
    }
    require_test(pipeline_sum > 0.0,
                 result.name + ": pipeline_profile_ms recorded nothing");
    require_test(phase_sum >= 0.8 * pipeline_sum &&
                     phase_sum <= 1.25 * pipeline_sum,
                 result.name +
                     ": server_phase_ms and pipeline_profile_ms disagree over "
                     "the same sections; the TimerLogger readings carry "
                     "warm-up trials, miss a per-trial END_EXPERIMENT, or were "
                     "averaged over the wrong trial count");
    const double server_ms =
        std::chrono::duration<double, std::milli>(result.timing.server_compute)
            .count();
    require_test(phase_sum <= 1.1 * server_ms + 0.05,
                 result.name +
                     ": phases exceed server_compute; warm-up sections leaked "
                     "into the measured experiments or the divisor is not the "
                     "measured-trial count");
    // Backstop for a defect that deflates both clocks at once; the glue
    // outside the phases is a few microseconds of timer bookkeeping plus the
    // layerwise direct-return copies, so only a gross accounting error gets
    // near this floor.
    require_test(phase_sum >= 0.25 * server_ms,
                 result.name +
                     ": phases cover under a quarter of server_compute; the "
                     "phase breakdown no longer explains the reported server "
                     "time");
    return std::make_tuple(server_ms, phase_sum, pipeline_sum);
  };
  const auto standard = check(run_standard_case(small, reference, trials));
  const auto flat = check(run_merkle_flat_case(small, reference, trials));
  const auto layerwise =
      check(run_merkle_layerwise_case(small, reference, trials));
  const auto line = [](const char *name, const auto &measured) {
    return std::string(name) + " " + std::to_string(std::get<1>(measured)) +
           "/" + std::to_string(std::get<2>(measured)) + "/" +
           std::to_string(std::get<0>(measured));
  };
  BENCH_PRINT("server phases/pipeline/server_compute (ms, 3 warm-ups + 2 "
              "measured): " << line("standard", standard) << ", "
              << line("flat", flat) << ", " << line("layerwise", layerwise));
}

// The Floyd sampler must keep reproducing the leaf schedule of the archived
// runs. The archive branch keeps the source JSON; the constants below copy
// the warmup_leaf_indices / trial_leaf_indices written by merkle_benchmarks
// with --leaf-count 16777216 --warmup 3 --experiments 16 and trial seed
// 3403547260970671391.
// run_cloudlab_numa_ab.sh pairs A/B runs by identical sample_leaf_indices and
// TreePIR replays the same leaves from leaves.txt, so a change to the splitmix
// constants, the modulo or the Floyd fallback would leave the distinctness
// checks above green while making every new run incomparable with the archived
// M4 and CloudLab schedules. make_benchmark_trial_plan has no parameter
// dependency, so this runs under every ACTIVE_CONFIG.
void test_merkle_trial_schedule() {
  const BenchmarkTrialPlan plan = make_benchmark_trial_plan(
      size_t{1} << 24, 3, 16, 3403547260970671391ULL);
  const std::vector<size_t> archived_warmups{1618911, 6774879, 5131033};
  const std::vector<size_t> archived_measured{
      2419157,  294839,   4718255,  7699409, 9435589, 1298722,
      15165185, 2726882,  2567763,  13314512, 13202926, 16052204,
      10960092, 107107,   326845,   14861253};
  require_test(plan.warmup_leaf_indices == archived_warmups,
               "warm-up leaf schedule drifted from the archived 2^24 artifact");
  require_test(plan.measured_leaf_indices == archived_measured,
               "measured leaf schedule drifted from the archived 2^24 artifact");
  BENCH_PRINT("trial schedule: the archived 3 + 16 leaf schedule at 2^24 "
              "(seed 3403547260970671391) is reproduced");
}

// --experiments 1 is what run_regression.sh passes to every merkle_benchmarks
// step and what a single-trial CloudLab case produces. With one sample the
// n-1 divisor has no value: the statistics must report count 1, zero
// population variance and no sample variance, and the JSON must emit null for
// the two sample fields rather than nan/inf, which json.load in the Python
// consumers rejects. The fixtures above use 3 and 4 samples, so the count > 1
// guard and the null emission were never exercised. Nothing here depends on
// the parameter point, so this runs under every ACTIVE_CONFIG.
void test_merkle_single_sample_stats() {
  const MetricSampleStatistics single = summarize_metric_samples({7.5});
  require_test(single.count == 1 && single.mean == 7.5 &&
                   single.population_variance == 0.0 &&
                   single.population_standard_deviation == 0.0,
               "single-sample statistics");
  require_test(!single.sample_variance.has_value() &&
                   !single.sample_standard_deviation.has_value(),
               "a single sample must not report an n-1 variance");

  // The H=24 flat fixture of test_merkle_benchmark_stats with one trial whose
  // sample is the trial itself.
  BenchmarkCaseResult result;
  result.name = "merkle_flat";
  result.correctness_passed = true;
  result.raw_dataset_bytes = 1073741760;
  result.paper_plaintext_database_bytes = 1073743872;
  result.logical_padded_database_bytes = 1074266112;
  result.paper_plaintext_scan_bytes = 25769852928;
  result.logical_padded_scan_bytes = 25782386688;
  result.raw_application_scan_bytes = 25769852928;
  result.physical_preprocessed_storage_bytes = 5729419264;
  result.useful_response_bytes = 768;
  result.timing.server_compute = std::chrono::milliseconds(250);
  result.sample_leaf_indices = {5};
  result.server_compute_samples_ms = {250.0};
  finalize_case_statistics(result);
  require_test(result.server_compute_ms_statistics.has_value() &&
                   result.server_compute_ms_statistics->count == 1 &&
                   result.server_compute_ms_statistics->mean == 250.0 &&
                   !result.server_compute_ms_statistics->sample_variance
                        .has_value(),
               "single-trial server statistics");
  require_test(result.paper_server_throughput_samples_MBps.size() == 1 &&
                   std::abs(result.paper_server_throughput_samples_MBps[0] -
                            result.paper_server_throughput_MBps) <=
                       1e-9 * result.paper_server_throughput_MBps,
               "with one trial the per-sample throughput must equal the "
               "headline throughput");
  require_test(result.paper_server_throughput_MBps_statistics.has_value() &&
                   result.paper_server_throughput_MBps_statistics->count == 1 &&
                   !result.paper_server_throughput_MBps_statistics
                        ->sample_variance.has_value(),
               "single-trial throughput statistics");

  BenchmarkReport report;
  report.schema_version = "onionpir-merkle-baselines-v2";
  report.workload.leaf_count = size_t{1} << 24;
  report.workload.tree_height = 24;
  report.workload.node_bytes = 32;
  report.workload.nodes_per_plaintext = 96;
  report.workload.warmups = 0;
  report.workload.measured_trials = 1;
  report.workload.trial_leaf_indices = {5};
  report.cases.push_back(result);
  const std::string text = benchmark_report_json(report);
  const auto occurrences = [&text](const std::string &needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
      ++count;
    }
    return count;
  };
  // One statistics object for the server time and one for the throughput.
  require_test(occurrences("\"count\": 1,") == 2 &&
                   occurrences("\"sample_variance\": null") == 2 &&
                   occurrences("\"sample_standard_deviation\": null") == 2,
               "single-trial JSON must spell null for both sample fields of "
               "both statistics objects");
  for (const char *token : {": nan", ": -nan", ": inf", ": -inf"}) {
    require_test(text.find(token) == std::string::npos,
                 std::string("single-trial JSON emitted a non-finite number (") +
                     token + ")");
  }
  BENCH_PRINT("single-sample statistics: count 1, population variance 0, "
              "sample variance null in the JSON");
}
