#pragma once

#include "tree_superrecord_test_util.h"
#include "tree_superrecord_benchmark_data.h"

#include "merkle_benchmark.h"
#include "tree_index.h"
#include "tree_query.h"
#include "tree_rotate.h"
#include "tree_response.h"
#include "tree_select.h"
#include "tree_superrecord_noise.h"
#include "tree_superrecord_response.h"

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace tree_superrecord_benchmark_internal {

using Clock = std::chrono::steady_clock;

inline constexpr std::array<const char *, 11> kStageLabels = {
    "expand", "complete", "prepare", "scan", "finalize",
    "fold",   "rotate",   "project", "pack", "switch", "merge"};

struct Stats {
  double mean = 0, median = 0, p95 = 0, stddev = 0;
};

struct Sample {
  size_t block_id = 0;
  size_t leaf = 0;
  double server_total_ms = 0;
  std::array<double, 11> stages_ms{};
  size_t response_merge_keyswitches = 0;
  double client_query_ms = 0;
  double client_decode_ms = 0;
  double response_serialize_ms = 0;
  int min_budget_bits = 0;
  uint64_t max_noise = 0;
  size_t response_payload_bytes = 0;
  double real_non_target_neighbour_slots = 0;
};

struct KeyCoverageReport {
  size_t required_key_height = 0;
  size_t registered_key_height = 0;
  size_t observed_contiguous_height = 0;
  size_t observed_key_count = 0;
  size_t missing_required = 0;
  size_t duplicate = 0;
  size_t unexpected = 0;
  size_t malformed_row_shapes = 0;
  bool matches = false;
};

struct RunResult {
  std::string row_id;
  std::string label;
  std::string description;
  bool is_generic = false;
  bool is_h1 = false;
  bool is_mandatory = false;
  bool eligible = false;
  std::string reason;
  std::optional<tree_superrecord::Candidate> candidate;
  std::vector<Sample> samples;
  double setup_ms = 0;
  double session_setup_ms = 0;
  double noise_gate_ms = 0;
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
  KeyCoverageReport key_coverage;
  std::optional<tree_superrecord::CandidateNoiseResult> noise_result;
};

struct PeakRss {
  std::optional<size_t> bytes;
  std::string ru_maxrss_units;
};

struct BenchmarkOptions {
  size_t entry_bits = 0;
  size_t tree_height = 0;
  size_t leaf_count = 0;
  size_t warmups = 0;
  size_t trials = 0;
  uint64_t trial_seed = 0;
  uint64_t execution_seed = 0;
  tree_superrecord::ResponsePacking response_packing =
      tree_superrecord::ResponsePacking::Projection;
  bool plan_only = false;
  size_t max_frontier = 0;
  size_t h1_frontier = 0;
  bool include_legacy = false;
  tree_superrecord::PlannerPolicy policy;
  std::vector<size_t> bundle_arities;
  BenchmarkTrialPlan trial_plan;
};

struct CandidatePreparation {
  tree_superrecord::PlannerInput input;
  std::vector<tree_superrecord::Candidate> all;
  size_t enumerated = 0;
  size_t h1_enumerated = 0;
  tree_superrecord::ParetoContext context;
  tree_superrecord::CostCalibration calibration;
  std::optional<tree_superrecord::Candidate> calibration_control;
  tree_superrecord::TreeServerTiming calibration_timing{};
  std::vector<tree_superrecord::Candidate> frontier;
};

struct LegacyExecutionPlan {
  std::optional<TreePirParams> tree;
  size_t resident_estimate = 0;
};

struct ResidentSelection {
  std::vector<size_t> execution_order;
  size_t resident_sum = 0;
  size_t temporary_peak = 0;
};

using Runner = std::function<Sample(size_t)>;

double ms_since(Clock::time_point start);
Stats summarize(std::vector<double> v);
std::vector<double> server_samples(const RunResult &r);
std::vector<double> stage_samples(const RunResult &r, size_t stage);

size_t env_size(const char *name, size_t fallback);
size_t env_size2(const char *primary, const char *secondary, size_t fallback);
std::optional<size_t> env_optional_size(const char *name);
std::string env_string(const char *name, const std::string &fallback);
std::string env_string2(const char *primary, const char *secondary,
                        const std::string &fallback);
std::string key_mode_name(tree_superrecord::EvalKeyRegistrationPolicy policy);
BenchmarkOptions read_benchmark_options(const PirTest &test, size_t log_n);
void filter_candidates_by_bundle_arity(
    std::vector<tree_superrecord::Candidate> &candidates,
    const std::vector<size_t> &arities);
void filter_candidates_by_pinned_shape(
    std::vector<tree_superrecord::Candidate> &candidates);
tree_superrecord::EntryBits benchmark_entry(size_t entry_bits, size_t level,
                                            size_t node);

void write_run_json(const std::string &path, size_t entry_bits, size_t L,
                    size_t N, size_t warmups, size_t trials,
                    uint64_t trial_seed, uint64_t execution_seed,
                    tree_superrecord::ResponsePacking response_packing,
                    bool plan_only, size_t enumerated, size_t h1_enumerated,
                    size_t h1_measured,
                    const std::vector<size_t> &bundle_arities,
                    const tree_superrecord::PlannerPolicy &policy,
                    const tree_superrecord::CostCalibration &calibration,
                    const std::optional<tree_superrecord::Candidate>
                        &calibration_control,
                    const tree_superrecord::TreeServerTiming
                        &calibration_timing,
                    const std::vector<size_t> &warmup_leaf_indices,
                    const std::vector<size_t> &measured_leaf_indices,
                    const std::vector<size_t> &execution_order,
                    const std::vector<std::vector<size_t>> &block_orders,
                    size_t blocked_resident_estimate,
                    const std::vector<RunResult> &results,
                    const PeakRss &peak_rss);
PeakRss read_peak_rss();
void print_benchmark_summary(const std::vector<RunResult> &rows,
                             const BenchmarkOptions &options, uint64_t t,
                             const PirParams &scheme);

size_t count_h1(const std::vector<RunResult> &rows);
CandidatePreparation prepare_candidate_inventory(const BenchmarkOptions &options,
                                                 const PirParams &scheme);
void finish_candidate_preparation(CandidatePreparation &prepared,
                                  const BenchmarkOptions &options,
                                  const PirParams &scheme,
                                  PirClient &client);
std::vector<RunResult> prepare_benchmark_rows(
    const BenchmarkOptions &options, const CandidatePreparation &prepared);
std::vector<size_t> shuffled_execution_order(const std::vector<RunResult> &rows,
                                             uint64_t execution_seed,
                                             std::mt19937_64 &rng);
LegacyExecutionPlan prepare_legacy_execution_plan(
    const BenchmarkOptions &options, const PirParams &scheme, size_t c,
    size_t n);
ResidentSelection select_resident_execution_order(
    std::vector<RunResult> &rows, std::vector<size_t> execution_order,
    const tree_superrecord::PlannerPolicy &policy,
    const LegacyExecutionPlan &legacy, size_t n);
void prepare_runners(std::vector<Runner> &runners,
                     std::vector<RunResult> &rows,
                     const std::vector<size_t> &execution_order,
                     const BenchmarkOptions &options,
                     const LegacyExecutionPlan &legacy,
                     PirClient &client,
                     const PirParams &scheme,
                     const tree_superrecord::EntrySource &source,
                     uint64_t t,
                     size_t n);
void run_interleaved_blocks(std::vector<RunResult> &rows,
                            const std::vector<size_t> &execution_order,
                            std::vector<Runner> &runners,
                            const BenchmarkOptions &options,
                            std::mt19937_64 &rng,
                            std::vector<std::vector<size_t>> &block_orders);
void finalize_run_results(std::vector<RunResult> &rows, size_t trials);

}  // namespace tree_superrecord_benchmark_internal
