#include "tree_superrecord_benchmark_internal.h"

#include <bit>

using namespace tree_superrecord_benchmark_internal;
using namespace tree_superrecord;
using namespace tree_superrecord_test;

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

  const BenchmarkOptions options = read_benchmark_options(*this, log_n);
  CandidatePreparation prepared = prepare_candidate_inventory(options, scheme);
  PirClient client(scheme);
  finish_candidate_preparation(prepared, options, scheme, client);
  std::vector<RunResult> rows = prepare_benchmark_rows(options, prepared);
  std::mt19937_64 rng(options.execution_seed);
  std::vector<size_t> execution_order =
      shuffled_execution_order(rows, options.execution_seed, rng);

  BENCH_PRINT("planner: E=" << options.entry_bits << " c=" << c << " N=2^"
              << options.tree_height << ": " << prepared.all.size()
              << " candidates (" << prepared.h1_enumerated
              << " h=1), Pareto frontier " << prepared.frontier.size()
              << ", benchmark rows " << rows.size()
              << ", execution seed " << options.execution_seed
              << ", response packing "
              << response_packing_name(options.response_packing));

  const EntrySource source = [&](size_t level, size_t node) {
    return benchmark_entry(options.entry_bits, level, node);
  };

  const LegacyExecutionPlan legacy =
      prepare_legacy_execution_plan(options, scheme, c, n);
  const ResidentSelection resident_selection = select_resident_execution_order(
      rows, execution_order, options.policy, legacy, n);
  execution_order = resident_selection.execution_order;
  std::vector<std::vector<size_t>> block_orders;
  std::vector<Runner> runners(rows.size());
  if (!options.plan_only) {
    prepare_runners(runners, rows, execution_order, options, legacy, client,
                    scheme, source, t, n);
    // All preparation and target gates finish before the shared timed blocks.
    run_interleaved_blocks(rows, execution_order, runners, options, rng,
                           block_orders);
    finalize_run_results(rows, options.trials);
  }

  const PeakRss peak_rss = read_peak_rss();
  const std::string json_path =
      env_string2("SUPERRECORD_OUTPUT_JSON", "OUTPUT_JSON", "");
  if (!json_path.empty()) {
    write_run_json(
        json_path, options.entry_bits, options.tree_height, options.leaf_count,
        options.warmups, options.trials, options.trial_seed,
        options.execution_seed, options.response_packing, options.plan_only,
        prepared.enumerated, prepared.h1_enumerated, count_h1(rows),
        options.bundle_arities, options.policy, prepared.calibration,
        prepared.calibration_control, prepared.calibration_timing,
        options.trial_plan.warmup_leaf_indices,
        options.trial_plan.measured_leaf_indices, execution_order, block_orders,
        resident_selection.resident_sum + resident_selection.temporary_peak,
        rows, peak_rss);
    BENCH_PRINT("generic super-record JSON: " << json_path);
  }

  print_benchmark_summary(rows, options, t, scheme);
}
