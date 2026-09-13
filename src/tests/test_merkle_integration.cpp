#include "tests.h"

#include <memory>
#include "merkle_baseline.h"
#include "merkle_benchmark.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

void test_merkle_profiled_override();
void test_layer_layout_sweep_dominated();

namespace {

// Expected authentication path from the 1-based heap index alone (root = 1,
// children 2i and 2i + 1), independent of the merkle_baseline index helpers
// the runners use to build their databases.
void require_expected_path(const MerklePath &path, size_t leaf,
                           size_t tree_height) {
  // Runners return leaf-to-root order (levels H..1), matching client use.
  require_test(path.size() == tree_height, "Merkle path length");
  size_t node = leaf + (size_t{1} << tree_height);
  for (size_t offset = 0; offset < tree_height; ++offset) {
    const size_t level = tree_height - offset;
    const size_t local = (node ^ 1U) - (size_t{1} << level);
    require_test(path[offset] == synthetic_merkle_node(level, local),
                 "Merkle path node mismatch");
    node >>= 1;
  }
}

template <typename Fn>
bool throws_invalid_argument(Fn &&fn) {
  try {
    fn();
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

// A synthetic profile that measures exactly one candidate per listed PIR
// level. With a single measured option per level the selector can only pick
// it (the sample value is irrelevant), so the profiled planner is forced off
// the legacy heights deterministically instead of following this machine's
// timings.
std::shared_ptr<LayerLayoutProfile> forced_height_profile(
    const PirParams &reference, size_t tree_height,
    const std::vector<std::pair<size_t, size_t>> &level_heights) {
  auto profile = std::make_shared<LayerLayoutProfile>();
  profile->environment = detect_layer_layout_environment(
      reference, "test-commit", "Benchmark", "CONFIG_N2048_K1_COMP");
  profile->tree_height = tree_height;
  profile->nodes_per_plaintext = 96;
  profile->measured_trials = 1;
  for (const auto &[level, height] : level_heights) {
    bool found = false;
    for (const LayerLayoutCandidate &c :
         enumerate_layer_layout_candidates(level, 96, reference)) {
      if (c.features.expansion_height != height) continue;
      LayerLayoutMeasurement m;
      m.level = level;
      m.features = c.features;
      m.server_samples_ms = {1.0};
      m.median_server_ms = 1.0;
      profile->measurements.push_back(std::move(m));
      found = true;
    }
    require_test(found, "fixture: level " + std::to_string(level) +
                            " has no candidate with expansion height " +
                            std::to_string(height));
  }
  return profile;
}

uint64_t padded_plaintexts_of(const std::vector<LayerLayoutRecord> &layers) {
  uint64_t total = 0;
  for (const LayerLayoutRecord &layer : layers) {
    total += layer.features.padded_plaintexts;
  }
  return total;
}

uint64_t padded_plaintexts_of(const std::vector<LayerLayout> &plan) {
  uint64_t total = 0;
  for (const LayerLayout &layout : plan) total += layout.params.get_num_pt();
  return total;
}

}  // namespace

void PirTest::test_merkle_integration() {
  require_applicable(paper_config_active(),
                     paper_config_reason("the Merkle benchmark suite"));
  // H=8 cheaply covers first, last, and interior leaves through codec, query,
  // server evaluation, response wire round-trip, and decryption.
  const MerkleWorkload small{size_t{1} << 8, 8, 32};
  const size_t small_target = utils::roundup_div(
      2 * (small.leaf_count - 1), size_t{96});
  PirParams small_reference =
      PirParams().with_layout({small_target, 3, true});
  const BenchmarkTrialPlan small_trials{
      {}, {0, small.leaf_count - 1, small.leaf_count / 2}};

  BenchmarkCaseExecution standard =
      run_standard_case(small, small_reference, small_trials);
  require_test(standard.result.correctness_passed,
               "standard deterministic case");
  require_test(standard.result.communication.pir_call_count == 1,
               "standard call count");
  require_test(!standard.result.pipeline_profile_ms.empty() &&
                   !standard.result.server_phase_ms.empty(),
               "standard case records its stage timings");

  BenchmarkCaseExecution flat =
      run_merkle_flat_case(small, small_reference, small_trials);
  BenchmarkCaseExecution layerwise =
      run_merkle_layerwise_case(small, small_reference, small_trials);
  require_test(flat.result.correctness_passed, "H=8 flat correctness");
  require_test(layerwise.result.correctness_passed,
               "H=8 layerwise correctness");
  require_test(flat.measured_paths == layerwise.measured_paths,
               "H=8 flat/layerwise path mismatch");
  require_test(flat.measured_paths.size() ==
                   small_trials.measured_leaf_indices.size(),
               "H=8 measured path count");
  for (size_t i = 0; i < flat.measured_paths.size(); ++i) {
    require_expected_path(flat.measured_paths[i],
                          small_trials.measured_leaf_indices[i],
                          small.tree_height);
  }
  // The flat case serves H calls on the same layout as the single standard
  // call, so its actual response bytes are exactly H times the standard's.
  require_test(flat.result.communication.pir_call_count == 8 &&
                   flat.result.communication.online_response_bytes_actual ==
                       8 * standard.result.communication
                               .online_response_bytes_actual,
               "flat response bytes are H standard responses");
  // Levels 1..6 (2..64 nodes) fit one 96-node plaintext and are returned in
  // the clear; levels 7 and 8 stay PIR calls.
  require_test(layerwise.result.direct_return_levels == 6,
               "H=8 direct-return level count");
  require_test(layerwise.result.communication.pir_call_count == 2,
               "H=8 layerwise PIR call count");
  require_test(layerwise.result.direct_return_response_bytes ==
                   (2 + 4 + 8 + 16 + 32 + 64) * 32,
               "H=8 direct-return plain bytes");
  validate_layerwise_path_communication(
      flat.result.communication, layerwise.result.communication,
      layerwise.result.direct_return_levels,
      layerwise.result.direct_return_response_bytes);

  // A tree whose every level fits one plaintext (H <= 6) would make no PIR
  // call at all; the layerwise case rejects it instead of reporting a
  // communication model with zero calls.
  {
    const MerkleWorkload tiny{size_t{1} << 6, 6, 32};
    const PirParams tiny_reference = PirParams().with_layout({2, 1, true});
    require_test(throws_invalid_argument([&] {
                   (void)run_merkle_layerwise_case(tiny, tiny_reference,
                                                   BenchmarkTrialPlan{{}, {5}});
                 }),
                 "accepted a layerwise tree with no PIR level");
  }

  // Profiled planner end to end at H=8: sweep the PIR levels on this machine,
  // then the profiled layerwise case must recover the same paths as the
  // legacy plan with identical communication accounting.
  std::shared_ptr<LayerLayoutProfile> profile8;
  {
    LayerLayoutSweepOptions sweep;
    sweep.leaf_count = small.leaf_count;
    sweep.warmups = 1;
    sweep.measured_trials = 2;
    sweep.trial_seed = 7;
    profile8 =
        std::make_shared<LayerLayoutProfile>(run_layer_layout_sweep(sweep));
    require_test(profile8->tree_height == 8 &&
                     profile8->selected_expansion_heights.size() == 8,
                 "H=8 sweep profile shape");
    require_test(!profile8->measurements.empty(),
                 "H=8 sweep measured the PIR levels");
    for (const LayerLayoutMeasurement &m : profile8->measurements) {
      require_test(m.level >= 7, "H=8 sweep skips direct-return levels");
      require_test(m.server_samples_ms.size() == 2,
                   "H=8 sweep keeps one sample per measured trial");
    }
    const PirParams sweep_reference = make_benchmark_reference(small);
    LayerPlannerConfig config;
    config.policy = LayerLayoutPolicy::profiled;
    config.profile = profile8;
    BenchmarkCaseExecution legacy_ref =
        run_merkle_layerwise_case(small, sweep_reference, small_trials);
    BenchmarkCaseExecution profiled = run_merkle_layerwise_case(
        small, sweep_reference, small_trials, config);
    require_test(profiled.result.correctness_passed,
                 "H=8 profiled layerwise correctness");
    require_test(profiled.measured_paths == legacy_ref.measured_paths,
                 "H=8 profiled/legacy path mismatch");
    validate_matching_path_communication(legacy_ref.result.communication,
                                         profiled.result.communication);
    require_test(profiled.result.layer_layout_policy == "profiled" &&
                     legacy_ref.result.layer_layout_policy == "legacy",
                 "layout policy recorded in the case result");
    require_test(profiled.result.layers.size() == 8 &&
                     profiled.result.layers[0].direct_return &&
                     !profiled.result.layers[7].direct_return,
                 "per-level layout records");
    require_test(!profiled.result.pipeline_profile_ms.empty() &&
                     profiled.result.pipeline_profile_ms.at("first_dim_core") >
                         0.0,
                 "pipeline profile recorded for the layerwise case");
    // The case runner honours the fallback flag on a mismatched profile and
    // rejects it without the flag.
    auto mismatched = std::make_shared<LayerLayoutProfile>(*profile8);
    mismatched->tree_height = 9;
    LayerPlannerConfig bad = config;
    bad.profile = mismatched;
    require_test(throws_invalid_argument([&] {
                   (void)run_merkle_layerwise_case(small, sweep_reference,
                                                   small_trials, bad);
                 }),
                 "case runner accepted a mismatched profile");
    bad.allow_profile_fallback = true;
    BenchmarkCaseExecution fallback = run_merkle_layerwise_case(
        small, sweep_reference, small_trials, bad);
    require_test(fallback.result.layer_layout_policy == "legacy" &&
                     fallback.measured_paths == legacy_ref.measured_paths,
                 "case runner fallback must produce the legacy plan");
  }

  // H=16 forces remaining-dimension RGSW MUX work; a small-only fixture could
  // otherwise pass while testing only the first-dimension path.
  const MerkleWorkload multidimensional{size_t{1} << 16, 16, 32};
  const size_t multidimensional_target = utils::roundup_div(
      2 * (multidimensional.leaf_count - 1), size_t{96});
  PirParams multidimensional_reference =
      PirParams().with_layout({multidimensional_target, 8, true});
  require_test(multidimensional_target > 1024,
               "H=16 fixture is not multidimensional");
  require_test(multidimensional_reference.get_num_other_dims() > 0,
               "H=16 fixture does not exercise MUX dimensions");
  const BenchmarkTrialPlan multidimensional_trials{
      {}, {48879, multidimensional.leaf_count - 1}};
  BenchmarkCaseExecution flat_multi = run_merkle_flat_case(
      multidimensional, multidimensional_reference,
      multidimensional_trials);
  BenchmarkCaseExecution layerwise_multi = run_merkle_layerwise_case(
      multidimensional, multidimensional_reference,
      multidimensional_trials);
  require_test(flat_multi.measured_paths == layerwise_multi.measured_paths,
               "H=16 flat/layerwise path mismatch");
  require_expected_path(flat_multi.measured_paths.at(0), 48879, 16);
  require_expected_path(flat_multi.measured_paths.at(1),
                        multidimensional.leaf_count - 1, 16);
  require_test(layerwise_multi.result.direct_return_levels == 6,
               "H=16 direct-return level count");
  validate_layerwise_path_communication(
      flat_multi.result.communication, layerwise_multi.result.communication,
      layerwise_multi.result.direct_return_levels,
      layerwise_multi.result.direct_return_response_bytes);

  // Profiled planner at H=16 with RGSW MUX levels: same paths, same bytes.
  std::shared_ptr<LayerLayoutProfile> profile16;
  {
    LayerLayoutSweepOptions sweep;
    sweep.leaf_count = multidimensional.leaf_count;
    sweep.warmups = 1;
    sweep.measured_trials = 2;
    sweep.trial_seed = 11;
    profile16 =
        std::make_shared<LayerLayoutProfile>(run_layer_layout_sweep(sweep));
    const PirParams sweep_reference =
        make_benchmark_reference(multidimensional);
    LayerPlannerConfig config;
    config.policy = LayerLayoutPolicy::profiled;
    config.profile = profile16;
    BenchmarkCaseExecution legacy_ref = run_merkle_layerwise_case(
        multidimensional, sweep_reference, multidimensional_trials);
    BenchmarkCaseExecution profiled = run_merkle_layerwise_case(
        multidimensional, sweep_reference, multidimensional_trials, config);
    require_test(profiled.measured_paths == legacy_ref.measured_paths,
                 "H=16 profiled/legacy path mismatch");
    require_expected_path(profiled.measured_paths.at(0), 48879, 16);
    validate_matching_path_communication(legacy_ref.result.communication,
                                         profiled.result.communication);
    uint64_t legacy_padded = 0, profiled_padded = 0;
    for (size_t i = 0; i < 16; ++i) {
      legacy_padded += legacy_ref.result.layers[i].features.padded_plaintexts;
      profiled_padded +=
          profiled.result.layers[i].features.padded_plaintexts;
    }
    require_test(profiled_padded <= legacy_padded * 1.01 + 1,
                 "H=16 profiled plan respects the padding budget");
  }

  // The benchmark suite with profiles loaded from disk, one per tree height:
  // the suite must pick the profile whose height matches the workload even
  // when the primary profile is the other one, reject a run with no matching
  // profile, and fall back to legacy only when asked. The full case set at
  // H=8 also runs the standard, flat and layerwise cases together.
  {
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::string path8 = (dir / "merkle-integration-h8.json").string();
    const std::string path16 = (dir / "merkle-integration-h16.json").string();
    save_layer_layout_profile(*profile8, path8);
    save_layer_layout_profile(*profile16, path16);
    auto loaded8 =
        std::make_shared<LayerLayoutProfile>(load_layer_layout_profile(path8));
    auto loaded16 = std::make_shared<LayerLayoutProfile>(
        load_layer_layout_profile(path16));
    std::filesystem::remove(path8);
    std::filesystem::remove(path16);
    require_test(loaded8->tree_height == 8 && loaded16->tree_height == 16,
                 "profiles reloaded from disk");

    MerkleBenchmarkOptions options;
    options.leaf_count = small.leaf_count;
    options.warmups = 0;
    options.measured_trials = 1;
    options.trial_seed = 0x6d65726b6c65ULL;
    options.case_selection = BenchmarkCaseSelection::all;
    options.layer_planner.policy = LayerLayoutPolicy::profiled;
    options.layer_planner.profile = loaded16;  // wrong height on purpose
    options.layer_profiles = {loaded16, loaded8};
    const BenchmarkReport report = run_merkle_benchmark_suite(options);
    require_test(report.cases.size() == 3 &&
                     report.cases[0].name == "standard_onionpir" &&
                     report.cases[1].name == "merkle_flat" &&
                     report.cases[2].name == "merkle_layerwise",
                 "full case set order");
    for (const BenchmarkCaseResult &c : report.cases) {
      require_test(c.correctness_passed, "full case set correctness");
    }
    require_test(report.cases[2].layer_layout_policy == "profiled" &&
                     report.cases[2].layers.size() == 8,
                 "suite selected the matching-height profile");
    require_test(report.workload.optional_workloads.size() == 1 &&
                     report.workload.optional_workloads[0].status ==
                         "not_requested",
                 "optional tier is reported as not requested");
    print_benchmark_report(report);

    options.case_selection = BenchmarkCaseSelection::merkle_layerwise;
    options.layer_profiles = {loaded16};
    require_test(throws_invalid_argument(
                     [&] { (void)run_merkle_benchmark_suite(options); }),
                 "suite accepted a run without a matching profile");
    options.layer_planner.allow_profile_fallback = true;
    const BenchmarkReport fallback = run_merkle_benchmark_suite(options);
    require_test(fallback.cases.size() == 1 &&
                     fallback.cases[0].layer_layout_policy == "legacy",
                 "suite fallback must report the legacy policy");
  }
}

// The profiled blocks of test_merkle_integration take their profiles from a
// real timing sweep, so whether a non-legacy layout is ever executed depends
// on this machine's noise (on the M4 the H=8 sweep selects exactly the legacy
// heights). Here the profile measures only one non-legacy candidate per PIR
// level, so the planner has to leave the legacy plan, and the case runner must
// build every level server and generate every query from the same overridden
// layout.params, retrieve the oracle path, and record the override in
// layers[] and in the padded-storage columns.
void test_merkle_profiled_override() {
  require_applicable(paper_config_active(),
                     paper_config_reason("the Merkle benchmark suite"));

  // H=8: legacy serves level 7 at h=1 (2 plaintexts) and level 8 at h=2 (4
  // plaintexts). Force the reference height h=3 at both: 8 plaintexts each,
  // so 6 of 8 and 5 of 8 stored plaintexts are padding, a shape the legacy
  // planner never produces at this height.
  const MerkleWorkload small{size_t{1} << 8, 8, 32};
  const PirParams reference = make_benchmark_reference(small);
  const std::vector<LayerLayout> legacy_plan =
      plan_layer_layouts(8, 96, reference);
  require_test(reference.get_expan_height() == 3 &&
                   legacy_plan[6].params.get_expan_height() == 1 &&
                   legacy_plan[7].params.get_expan_height() == 2,
               "H=8 fixture: reference height or legacy heights changed");
  LayerPlannerConfig config;
  config.policy = LayerLayoutPolicy::profiled;
  config.total_padding_budget = 2.0;  // 10 extra on a legacy total of 12
  config.profile = forced_height_profile(reference, 8, {{7, 3}, {8, 3}});
  const BenchmarkTrialPlan trials{
      {}, {0, small.leaf_count - 1, small.leaf_count / 2}};
  const BenchmarkCaseExecution flat =
      run_merkle_flat_case(small, reference, trials);
  const BenchmarkCaseExecution legacy =
      run_merkle_layerwise_case(small, reference, trials);
  const BenchmarkCaseExecution profiled =
      run_merkle_layerwise_case(small, reference, trials, config);
  require_test(profiled.result.correctness_passed,
               "H=8 overridden layerwise correctness");
  require_test(profiled.measured_paths == flat.measured_paths,
               "H=8 overridden layouts retrieved a different path");
  for (size_t i = 0; i < trials.measured_leaf_indices.size(); ++i) {
    require_expected_path(profiled.measured_paths.at(i),
                          trials.measured_leaf_indices[i], 8);
  }
  require_test(profiled.result.layer_layout_policy == "profiled" &&
                   profiled.result.layers.size() == 8,
               "H=8 override must be recorded as the profiled policy");
  for (size_t level = 7; level <= 8; ++level) {
    const LayerLayoutFeatures &f =
        profiled.result.layers[level - 1].features;
    require_test(f.expansion_height == 3 && f.first_dim_size == 8 &&
                     f.other_dim_size == 1 && f.padded_plaintexts == 8,
                 "H=8 level " + std::to_string(level) +
                     " did not record the forced h=3 layout");
  }
  require_test(padded_plaintexts_of(legacy.result.layers) == 12 &&
                   padded_plaintexts_of(profiled.result.layers) == 22,
               "H=8 padded totals must be 6+2+4 (legacy) and 6+8+8 (forced)");
  // The wire model is layout-independent: queries are modelled from the
  // reference and a response is one ciphertext whatever the level's shape.
  validate_layerwise_path_communication(
      flat.result.communication, profiled.result.communication,
      profiled.result.direct_return_levels,
      profiled.result.direct_return_response_bytes);
  // Storage accounting follows the executed layouts: the paper (target)
  // footprint does not move, the padded footprint grows by exactly the ten
  // forced padding plaintexts.
  require_test(profiled.result.paper_plaintext_database_bytes ==
                   legacy.result.paper_plaintext_database_bytes,
               "overridden layouts changed the paper plaintext footprint");
  require_test(profiled.result.logical_padded_database_bytes ==
                   legacy.result.logical_padded_database_bytes +
                       10 * reference.get_pt_size(),
               "padded footprint must grow by the 10 forced padding plaintexts");

  // H=16, level 13: legacy is h=5 (N0=8, 11 x 8 = 88 plaintexts over four
  // MUX dimensions); force h=6 (N0=32, 3 x 32 = 96 plaintexts over two), a
  // different first-dimension size and remaining-dimension shape that fits
  // the default 1% budget (8 extra on a legacy total of 1452).
  const MerkleWorkload multi{size_t{1} << 16, 16, 32};
  const PirParams reference16 = make_benchmark_reference(multi);
  const std::vector<LayerLayout> legacy16 =
      plan_layer_layouts(16, 96, reference16);
  require_test(legacy16[12].params.get_expan_height() == 5 &&
                   legacy16[12].params.get_num_other_dims() == 4,
               "H=16 fixture: legacy level-13 layout changed");
  LayerPlannerConfig config16;
  config16.policy = LayerLayoutPolicy::profiled;
  config16.total_padding_budget = 1.01;
  config16.profile = forced_height_profile(reference16, 16, {{13, 6}});
  const BenchmarkTrialPlan trials16{{}, {48879, multi.leaf_count - 1}};
  const BenchmarkCaseExecution profiled16 =
      run_merkle_layerwise_case(multi, reference16, trials16, config16);
  require_test(profiled16.result.correctness_passed &&
                   profiled16.result.layer_layout_policy == "profiled",
               "H=16 overridden layerwise correctness");
  require_expected_path(profiled16.measured_paths.at(0), 48879, 16);
  require_expected_path(profiled16.measured_paths.at(1),
                        multi.leaf_count - 1, 16);
  const LayerLayoutFeatures &level13 = profiled16.result.layers.at(12).features;
  require_test(level13.expansion_height == 6 && level13.first_dim_size == 32 &&
                   level13.other_dim_size == 3 && level13.other_dim_count == 2 &&
                   level13.padded_plaintexts == 96,
               "H=16 level 13 did not record the forced h=6 layout");
  for (size_t i = 0; i < 16; ++i) {
    if (i == 12) continue;
    require_test(profiled16.result.layers[i].features.expansion_height ==
                     legacy16[i].params.get_expan_height(),
                 "H=16 unmeasured levels must keep the legacy layout");
  }
  require_test(padded_plaintexts_of(profiled16.result.layers) ==
                   padded_plaintexts_of(legacy16) + 8,
               "H=16 forced layout must cost exactly 8 extra plaintexts");
  BENCH_PRINT("profiled override: H=8 levels 7,8 served at h=3 ("
              << padded_plaintexts_of(profiled.result.layers)
              << " padded plaintexts), H=16 level 13 served at h=6 ("
              << padded_plaintexts_of(profiled16.result.layers)
              << " padded plaintexts); paths match the heap oracle");
}

// --layout-sweep-all (LayerLayoutSweepOptions::include_dominated) times every
// legal candidate and flags the non-frontier ones; the default sweep times the
// frontier only. Nothing else passes include_dominated, and the `dominated`
// flag the loader reads (summarize_layer_layout_profile.py prints it as the
// "dominated" role) had no round-trip check.
void test_layer_layout_sweep_dominated() {
  require_applicable(paper_config_active(),
                     paper_config_reason("the Merkle benchmark suite"));
  const MerkleWorkload small{size_t{1} << 8, 8, 32};
  const PirParams reference = make_benchmark_reference(small);
  LayerLayoutSweepOptions sweep;
  sweep.leaf_count = small.leaf_count;
  sweep.warmups = 0;
  sweep.measured_trials = 1;
  sweep.trial_seed = 7;
  const LayerLayoutProfile frontier = run_layer_layout_sweep(sweep);
  sweep.include_dominated = true;
  const LayerLayoutProfile all = run_layer_layout_sweep(sweep);

  struct Entry {
    size_t level;
    size_t height;
    bool dominated;
    bool operator==(const Entry &o) const {
      return level == o.level && height == o.height && dominated == o.dominated;
    }
  };
  const auto entries_of = [](const LayerLayoutProfile &profile) {
    std::vector<Entry> out;
    for (const LayerLayoutMeasurement &m : profile.measurements) {
      out.push_back({m.level, m.features.expansion_height, m.dominated});
    }
    return out;
  };
  // Expected from the planner's own enumeration and Pareto filter: every
  // legal candidate of both PIR levels once, dominated iff the filter drops
  // it; direct-return levels 1..6 are not measured.
  std::vector<Entry> expected;
  for (size_t level = 7; level <= 8; ++level) {
    const std::vector<LayerLayoutCandidate> candidates =
        enumerate_layer_layout_candidates(level, 96, reference);
    const std::vector<LayerLayoutCandidate> pareto =
        pareto_layer_layout_candidates(candidates);
    for (const LayerLayoutCandidate &c : candidates) {
      bool on_frontier = false;
      for (const LayerLayoutCandidate &f : pareto) {
        on_frontier |= f.features.expansion_height == c.features.expansion_height;
      }
      expected.push_back({level, c.features.expansion_height, !on_frontier});
    }
  }
  require_test(entries_of(all) == expected,
               "--layout-sweep-all must time every legal candidate of every "
               "PIR level exactly once, flagged by the Pareto filter");
  // Pin today's H=8 candidate set so a with_layout legality change shows up
  // here rather than in a CloudLab sweep. The five entries are what
  // enumerate_layer_layout_candidates admits at the two PIR levels of this
  // height under the sweep's own make_benchmark_reference (h=3), flagged by a
  // Pareto filter that only compares cost features (scan bytes, substitutions,
  // NTTs, selector rows, CMUXes). Nothing here depends on a measurement, so a
  // change means with_layout accepts or rejects a different shape, not that
  // this machine timed the candidates differently. Update the pin only
  // alongside a deliberate legality or Pareto change -- an archived sweep
  // profile covering candidates this build no longer enumerates can no longer
  // be replayed.
  const std::vector<Entry> pinned = {{7, 1, false}, {7, 2, true}, {7, 3, true},
                                     {8, 2, false}, {8, 3, true}};
  require_test(expected == pinned,
               "H=8 candidate set drifted from h in {1,2,3} at level 7 and "
               "{2,3} at level 8 with only h=1 / h=2 on the frontier");
  std::vector<Entry> undominated;
  for (const Entry &e : entries_of(all)) {
    if (!e.dominated) undominated.push_back(e);
  }
  require_test(entries_of(frontier) == undominated,
               "default sweep must measure exactly the undominated candidates");
  for (const LayerLayoutMeasurement &m : all.measurements) {
    require_test(m.server_samples_ms.size() == 1 &&
                     m.median_server_ms == m.server_samples_ms[0],
                 "one measured trial must give one sample and that median");
  }
  // Each alternative costs at least two extra plaintexts on a legacy total of
  // 12, so the sweeps' default 1% budget keeps the legacy heights whatever
  // they timed: the selection is deterministic.
  const std::vector<size_t> legacy_heights{0, 0, 0, 0, 0, 0, 1, 2};
  require_test(all.selected_expansion_heights == legacy_heights &&
                   frontier.selected_expansion_heights == legacy_heights &&
                   all.legacy_total_padded_plaintexts == 12 &&
                   all.selected_total_padded_plaintexts == 12,
               "1% budget must keep the legacy heights at H=8");

  // JSON round trip keeps the flag, under the key the Python report reads.
  const std::string path =
      (std::filesystem::temp_directory_path() / "layer-layout-sweep-dominated.json")
          .string();
  save_layer_layout_profile(all, path);
  std::string text;
  {
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    text = buffer.str();
  }
  const LayerLayoutProfile loaded = load_layer_layout_profile(path);
  std::filesystem::remove(path);
  require_test(text.find("\"dominated\": true") != std::string::npos &&
                   text.find("\"dominated\": false") != std::string::npos,
               "profile JSON must spell the dominated flag under that key");
  require_test(entries_of(loaded) == entries_of(all),
               "dominated flags did not survive the profile JSON round trip");

  // The planner treats dominated measurements as ordinary legal options: a
  // reloaded profile carrying them drives the case runner, and under the 1%
  // budget it reproduces the legacy plan with the profiled policy recorded.
  LayerPlannerConfig config;
  config.policy = LayerLayoutPolicy::profiled;
  config.total_padding_budget = 1.01;
  config.profile = std::make_shared<LayerLayoutProfile>(loaded);
  const BenchmarkTrialPlan trials{{}, {0, small.leaf_count - 1}};
  const BenchmarkCaseExecution legacy_case =
      run_merkle_layerwise_case(small, reference, trials);
  const BenchmarkCaseExecution profiled_case =
      run_merkle_layerwise_case(small, reference, trials, config);
  require_test(profiled_case.result.correctness_passed &&
                   profiled_case.measured_paths == legacy_case.measured_paths,
               "all-candidates profile: path mismatch against the legacy plan");
  require_test(profiled_case.result.layer_layout_policy == "profiled" &&
                   profiled_case.result.layers.size() == 8,
               "all-candidates profile must run as the profiled policy");
  for (size_t i = 0; i < 8; ++i) {
    require_test(profiled_case.result.layers[i].features.expansion_height ==
                     legacy_heights[i],
                 "1% budget must keep the legacy heights in the executed plan");
  }
  BENCH_PRINT("layout sweep: " << all.measurements.size()
              << " candidates timed with --layout-sweep-all ("
              << all.measurements.size() - frontier.measurements.size()
              << " dominated), " << frontier.measurements.size()
              << " on the default frontier sweep; flags round-trip through JSON");
}
