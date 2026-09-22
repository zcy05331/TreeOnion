#include "merkle_benchmark.h"
#include "tests.h"

#include <cstring>
#include <memory>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

size_t parse_size_argument(const char *option, const char *value) {
  std::string text(value);
  if (text.empty() || text.front() == '-') {
    throw std::invalid_argument(std::string(option) +
                                " requires a non-negative integer");
  }
  size_t parsed_characters = 0;
  unsigned long long parsed = 0;
  try {
    parsed = std::stoull(text, &parsed_characters, 10);
  } catch (const std::exception &) {
    throw std::invalid_argument(std::string(option) +
                                " requires a non-negative integer");
  }
  if (parsed_characters != text.size() ||
      parsed > std::numeric_limits<size_t>::max()) {
    throw std::invalid_argument(std::string(option) +
                                " is outside the size_t range");
  }
  return static_cast<size_t>(parsed);
}

uint64_t parse_seed_argument(const char *value) {
  std::string text(value);
  if (text.empty() || text.front() == '-') {
    throw std::invalid_argument(
        "--trial-seed requires a non-negative integer");
  }
  size_t parsed_characters = 0;
  unsigned long long parsed = 0;
  try {
    parsed = std::stoull(text, &parsed_characters, 10);
  } catch (const std::exception &) {
    throw std::invalid_argument(
        "--trial-seed requires a non-negative integer");
  }
  if (parsed_characters != text.size()) {
    throw std::invalid_argument("--trial-seed is outside the uint64_t range");
  }
  return static_cast<uint64_t>(parsed);
}

const char *require_value(int argc, char *argv[], int &index) {
  if (index + 1 >= argc) {
    throw std::invalid_argument(std::string(argv[index]) +
                                " requires a value");
  }
  return argv[++index];
}

struct CommandLineOptions {
  std::string test_name = "pir";
  size_t num_experiments = 10;
  size_t warmup = 3;
  size_t leaf_count = size_t{1} << 24;
  uint64_t trial_seed = MerkleBenchmarkOptions{}.trial_seed;
  std::string benchmark_json;
  bool run_optional_4gb = false;
  size_t reference_expansion_height = 0;  // 0 = default rule
  size_t layer_nodes_per_plaintext = 96;  // layer_layout_sweep: 96 or 64
  BenchmarkCaseSelection benchmark_case = BenchmarkCaseSelection::all;
  // Layerwise layout planner / sweep flags.
  std::string layout_profile_json;
  bool layout_sweep_all = false;
  double layer_padding_budget = 1.01;
  std::string layer_layout_policy = "legacy";
  std::vector<std::string> layer_layout_profiles;  // repeatable
  bool allow_layout_profile_fallback = false;
  // The timed PirTest benchmarks only override their defaults for flags
  // that were actually given (see PirTest::bench_*).
  bool experiments_given = false, warmup_given = false;
  bool leaf_count_given = false, trial_seed_given = false;
};

CommandLineOptions parse_command_line(int argc, char *argv[]) {
  CommandLineOptions cli;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--test") == 0) {
      cli.test_name = require_value(argc, argv, i);
    } else if (std::strcmp(argv[i], "--experiments") == 0) {
      cli.num_experiments = parse_size_argument(
          "--experiments", require_value(argc, argv, i));
      cli.experiments_given = true;
    } else if (std::strcmp(argv[i], "--warmup") == 0) {
      cli.warmup = parse_size_argument("--warmup",
                                   require_value(argc, argv, i));
      cli.warmup_given = true;
    } else if (std::strcmp(argv[i], "--leaf-count") == 0) {
      cli.leaf_count = parse_size_argument(
          "--leaf-count", require_value(argc, argv, i));
      cli.leaf_count_given = true;
    } else if (std::strcmp(argv[i], "--trial-seed") == 0) {
      cli.trial_seed = parse_seed_argument(require_value(argc, argv, i));
      cli.trial_seed_given = true;
    } else if (std::strcmp(argv[i], "--benchmark-json") == 0) {
      cli.benchmark_json = require_value(argc, argv, i);
    } else if (std::strcmp(argv[i], "--benchmark-case") == 0) {
      const std::string value = require_value(argc, argv, i);
      if (value == "all") {
        cli.benchmark_case = BenchmarkCaseSelection::all;
      } else if (value == "standard_onionpir") {
        cli.benchmark_case = BenchmarkCaseSelection::standard_onionpir;
      } else if (value == "merkle_paths") {
        cli.benchmark_case = BenchmarkCaseSelection::merkle_paths;
      } else if (value == "merkle_layerwise") {
        cli.benchmark_case = BenchmarkCaseSelection::merkle_layerwise;
      } else if (value == "merkle_layerwise_merged") {
        cli.benchmark_case = BenchmarkCaseSelection::merkle_layerwise_merged;
      } else {
        throw std::invalid_argument(
            "--benchmark-case must be all, standard_onionpir, merkle_paths, "
            "merkle_layerwise, or merkle_layerwise_merged");
      }
    } else if (std::strcmp(argv[i], "--run-optional-4gb") == 0) {
      cli.run_optional_4gb = true;
    } else if (std::strcmp(argv[i], "--reference-expansion-height") == 0) {
      cli.reference_expansion_height = parse_size_argument(
          "--reference-expansion-height", require_value(argc, argv, i));
      if (cli.reference_expansion_height == 0 ||
          cli.reference_expansion_height > 10) {
        throw std::invalid_argument(
            "--reference-expansion-height must be in 1..10");
      }
    } else if (std::strcmp(argv[i], "--layer-nodes-per-plaintext") == 0) {
      cli.layer_nodes_per_plaintext = parse_size_argument(
          "--layer-nodes-per-plaintext", require_value(argc, argv, i));
      if (cli.layer_nodes_per_plaintext != 96 &&
          cli.layer_nodes_per_plaintext != 64) {
        throw std::invalid_argument(
            "--layer-nodes-per-plaintext must be 96 or 64");
      }
    } else if (std::strcmp(argv[i], "--layout-profile-json") == 0) {
      cli.layout_profile_json = require_value(argc, argv, i);
    } else if (std::strcmp(argv[i], "--layout-sweep-all") == 0) {
      cli.layout_sweep_all = true;
    } else if (std::strcmp(argv[i], "--layer-padding-budget") == 0) {
      cli.layer_padding_budget =
          parse_padding_budget(require_value(argc, argv, i));
    } else if (std::strcmp(argv[i], "--layer-layout-policy") == 0) {
      cli.layer_layout_policy = require_value(argc, argv, i);
      if (cli.layer_layout_policy != "legacy" &&
          cli.layer_layout_policy != "profiled") {
        throw std::invalid_argument(
            "--layer-layout-policy must be legacy or profiled");
      }
    } else if (std::strcmp(argv[i], "--layer-layout-profile") == 0) {
      cli.layer_layout_profiles.push_back(require_value(argc, argv, i));
    } else if (std::strcmp(argv[i], "--allow-layout-profile-fallback") == 0) {
      cli.allow_layout_profile_fallback = true;
    } else if (std::strcmp(argv[i], "--no-compress") == 0) {
      // Retained for run.py compatibility. Current query packing has one path.
    } else {
      throw std::invalid_argument(std::string("Unknown option: ") + argv[i]);
    }
  }
  return cli;
}

int run_layout_sweep(const CommandLineOptions &cli) {
  if (cli.layout_profile_json.empty()) {
    throw std::invalid_argument(
        "--test layer_layout_sweep requires --layout-profile-json <path>");
  }
  if (cli.num_experiments == 0) {
    throw std::invalid_argument("--experiments must be positive");
  }
  LayerLayoutSweepOptions sweep;
  sweep.leaf_count = cli.leaf_count;
  sweep.warmups = cli.warmup;
  sweep.measured_trials = cli.num_experiments;
  sweep.trial_seed = cli.trial_seed;
  sweep.padding_budget = cli.layer_padding_budget;
  sweep.include_dominated = cli.layout_sweep_all;
  sweep.nodes_per_plaintext = cli.layer_nodes_per_plaintext;
  sweep.reference_expansion_height = cli.reference_expansion_height;
  const LayerLayoutProfile profile = run_layer_layout_sweep(sweep);
  save_layer_layout_profile(profile, cli.layout_profile_json);
  std::cout << "Layer layout profile: " << cli.layout_profile_json << '\n';
  return 0;
}

int run_merkle_benchmarks(const CommandLineOptions &cli) {
  MerkleBenchmarkOptions options;
  options.leaf_count = cli.leaf_count;
  options.warmups = cli.warmup;
  options.measured_trials = cli.num_experiments;
  options.trial_seed = cli.trial_seed;
  options.run_optional_4gb = cli.run_optional_4gb;
  options.reference_expansion_height = cli.reference_expansion_height;
  options.case_selection = cli.benchmark_case;
  options.layer_planner.total_padding_budget = cli.layer_padding_budget;
  options.layer_planner.allow_profile_fallback =
      cli.allow_layout_profile_fallback;
  if (cli.layer_layout_policy == "profiled") {
    if (cli.layer_layout_profiles.empty()) {
      throw std::invalid_argument(
          "--layer-layout-policy profiled requires "
          "--layer-layout-profile <path> (repeatable, one per tree height)");
    }
    options.layer_planner.policy = LayerLayoutPolicy::profiled;
    for (const std::string &path : cli.layer_layout_profiles) {
      options.layer_profiles.push_back(
          std::make_shared<LayerLayoutProfile>(
              load_layer_layout_profile(path)));
    }
    options.layer_planner.profile = options.layer_profiles.front();
  }
  BenchmarkReport report = run_merkle_benchmark_suite(options);
  print_benchmark_report(report);
  if (!cli.benchmark_json.empty()) {
    write_benchmark_report_json(report, cli.benchmark_json);
    std::cout << "Benchmark JSON: " << cli.benchmark_json << '\n';
  }
  return 0;
}

int run_pir_test(const CommandLineOptions &cli) {
  if (cli.num_experiments >
      std::numeric_limits<size_t>::max() - cli.warmup) {
    throw std::overflow_error("experiment count plus warmup overflows");
  }
  TimerLogger::setWarmup(cli.warmup);
  PirTest test;
  test.num_experiments = cli.num_experiments + cli.warmup;
  if (cli.leaf_count_given) test.bench_leaf_count = cli.leaf_count;
  if (cli.experiments_given) test.bench_measured_trials = cli.num_experiments;
  if (cli.warmup_given) test.bench_warmups = cli.warmup;
  if (cli.trial_seed_given) test.bench_trial_seed = cli.trial_seed;
  if (cli.reference_expansion_height != 0) {
    test.bench_reference_expansion_height = cli.reference_expansion_height;
  }
  if (!cli.benchmark_json.empty()) test.bench_output_json = cli.benchmark_json;
  test.run_test(cli.test_name);
  return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
  try {
    const CommandLineOptions cli = parse_command_line(argc, argv);
    if (cli.test_name == "layer_layout_sweep") return run_layout_sweep(cli);
    if (cli.test_name == "merkle_benchmarks") return run_merkle_benchmarks(cli);
    return run_pir_test(cli);
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}
