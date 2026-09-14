#include "tree_superrecord_benchmark_internal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace tree_superrecord_benchmark_internal {

using namespace tree_superrecord;
using namespace tree_superrecord_test;

size_t env_size(const char *name, size_t fallback) {
  const char *value = std::getenv(name);
  if (!value || !*value) return fallback;
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (*end != '\0' ||
      parsed >
          static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
    throw std::invalid_argument(std::string(name) +
                                " must be a non-negative size_t integer");
  }
  return static_cast<size_t>(parsed);
}

size_t env_size2(const char *primary, const char *secondary, size_t fallback) {
  if (std::getenv(primary)) return env_size(primary, fallback);
  return env_size(secondary, fallback);
}

std::optional<size_t> env_optional_size(const char *name) {
  const char *value = std::getenv(name);
  if (!value || !*value) return std::nullopt;
  return env_size(name, 0);
}

bool env_bool(const char *name) {
  const char *value = std::getenv(name);
  if (!value || !*value) return false;
  const std::string text(value);
  return text != "0" && text != "false" && text != "FALSE" &&
         text != "no" && text != "NO";
}

ResponsePacking env_response_packing() {
  const char *value = std::getenv("SUPERRECORD_RESPONSE_PACKING");
  const std::string mode = value && *value ? std::string(value) : "projection";
  if (mode == "projection") return ResponsePacking::Projection;
  if (mode == "mlwe") return ResponsePacking::MlweMerge;
  throw std::invalid_argument(
      "SUPERRECORD_RESPONSE_PACKING must be projection or mlwe");
}

bool contains_size(const std::vector<size_t> &values, size_t value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<size_t> env_size_list(const char *name) {
  const char *value = std::getenv(name);
  if (!value || !*value) return {};
  std::vector<size_t> out;
  std::stringstream input(value);
  std::string token;
  while (std::getline(input, token, ',')) {
    if (token.empty()) continue;
    size_t consumed = 0;
    unsigned long long parsed = 0;
    try {
      parsed = std::stoull(token, &consumed, 10);
    } catch (const std::exception &) {
      throw std::invalid_argument(std::string(name) +
                                  " must be a comma-separated integer list");
    }
    if (consumed != token.size() ||
        parsed >
            static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
      throw std::invalid_argument(std::string(name) +
                                  " must be a comma-separated integer list");
    }
    out.push_back(static_cast<size_t>(parsed));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<size_t> benchmark_bundle_arities(const PlannerPolicy &policy) {
  std::vector<size_t> arities = env_size_list("SUPERRECORD_BUNDLE_ARITIES");
  for (size_t arity : arities) {
    if (arity == 0 || arity > policy.max_bundle_arity) {
      throw std::invalid_argument(
          "SUPERRECORD_BUNDLE_ARITIES must be within the bundle arity policy");
    }
  }
  return arities;
}

void filter_candidates_by_bundle_arity(std::vector<Candidate> &candidates,
                                       const std::vector<size_t> &arities) {
  if (arities.empty()) return;
  candidates.erase(
      std::remove_if(candidates.begin(), candidates.end(),
                     [&](const Candidate &candidate) {
                       return !contains_size(arities,
                                             candidate.shape.bundle_arity);
                     }),
      candidates.end());
}

std::string env_string(const char *name, const std::string &fallback) {
  const char *value = std::getenv(name);
  return value && *value ? std::string(value) : fallback;
}

void filter_candidates_by_pinned_shape(std::vector<Candidate> &candidates) {
  const char *value = std::getenv("SUPERRECORD_PIN_SHAPE");
  if (value == nullptr) return;
  std::istringstream in(value);
  std::array<size_t, 5> pin{};
  for (size_t i = 0; i < pin.size(); ++i) {
    char comma = 0;
    if (!(in >> pin[i]) ||
        (i + 1 < pin.size() && (!(in >> comma) || comma != ','))) {
      throw std::invalid_argument("SUPERRECORD_PIN_SHAPE must be h,G,P,N0,b");
    }
  }
  in >> std::ws;
  if (!in.eof()) {
    throw std::invalid_argument("unexpected suffix in SUPERRECORD_PIN_SHAPE");
  }
  std::erase_if(candidates, [&](const Candidate &candidate) {
    const auto &s = candidate.shape;
    return std::array<size_t, 5>{s.bundle_arity, s.record_coeffs,
                                 s.plane_count, s.N0, s.beta_bits} != pin;
  });
  if (candidates.empty()) {
    throw std::invalid_argument("pinned benchmark shape is not feasible");
  }
}

std::string env_string2(const char *primary, const char *secondary,
                        const std::string &fallback) {
  const char *value = std::getenv(primary);
  if (value && *value) return std::string(value);
  return env_string(secondary, fallback);
}

std::vector<size_t> default_expansion_heights(size_t log_n) {
  std::vector<size_t> heights;
  for (size_t h = 5; h <= std::min<size_t>(10, log_n); ++h) {
    heights.push_back(h);
  }
  return heights;
}

std::string key_mode_name(EvalKeyRegistrationPolicy policy) {
  switch (policy) {
    case EvalKeyRegistrationPolicy::FixedMaximumHeight:
      return "fixed-maximum-height";
    case EvalKeyRegistrationPolicy::CandidateSpecificHeight:
      return "candidate-specific-height";
  }
  return "unknown";
}

EvalKeyRegistrationPolicy env_key_mode() {
  const std::string mode = env_string("SUPERRECORD_KEY_MODE", "fixed");
  if (mode == "fixed" || mode == "fixed-maximum-height") {
    return EvalKeyRegistrationPolicy::FixedMaximumHeight;
  }
  if (mode == "candidate" || mode == "candidate-specific" ||
      mode == "candidate-specific-height") {
    return EvalKeyRegistrationPolicy::CandidateSpecificHeight;
  }
  throw std::invalid_argument(
      "SUPERRECORD_KEY_MODE must be fixed or candidate-specific");
}

EntryBits benchmark_entry(size_t entry_bits, size_t level, size_t node) {
  if (entry_bits == 256) {
    return entry_from_bytes(synthetic_tree_node_bytes(level, node), entry_bits);
  }
  return synthetic_entry(entry_bits, level, node);
}

BenchmarkOptions read_benchmark_options(const PirTest &test, size_t log_n) {
  BenchmarkOptions options;
  options.entry_bits = env_size("SUPERRECORD_ENTRY_BITS", 256);
  const size_t default_leaf_pow = env_size("SUPERRECORD_LEAF_POW", 22);
  options.tree_height = test.bench_tree_height(default_leaf_pow);
  options.leaf_count = checked_pow2(options.tree_height, "benchmark leaves");
  options.warmups = test.bench_warmups.value_or(3);
  options.trials = test.bench_measured_trials.value_or(16);
  options.trial_seed = test.bench_trial_seed.value_or(0x747265654733325fULL);
  options.execution_seed =
      env_size("SUPERRECORD_EXECUTION_SEED", 0x535265636f726431ULL);
  options.response_packing = env_response_packing();
  options.plan_only = env_bool("SUPERRECORD_PLAN_ONLY");
  options.trial_plan = make_benchmark_trial_plan(
      options.leaf_count, options.warmups, options.trials, options.trial_seed);
  options.max_frontier = env_size("SUPERRECORD_MAX_FRONTIER", 2);
  options.h1_frontier = env_size("SUPERRECORD_H1_FRONTIER", 2);
  options.include_legacy = options.entry_bits == 256 &&
                           std::getenv("SUPERRECORD_SKIP_LEGACY") == nullptr;

  options.policy.max_response_ciphertexts =
      env_size("SUPERRECORD_MAX_RESPONSES", 1);
  options.policy.max_bundle_arity = env_size("SUPERRECORD_MAX_ARITY", 8);
  options.policy.max_retrieved_bundle_bits_per_level = env_size(
      "SUPERRECORD_MAX_RETRIEVED_BUNDLE_BITS_PER_LEVEL",
      std::numeric_limits<size_t>::max());
  options.policy.max_validation_resident_bytes =
      env_size2("SUPERRECORD_MEMORY_BYTES",
                "SUPERRECORD_MAX_VALIDATION_RESIDENT_BYTES",
                std::numeric_limits<size_t>::max());
  options.policy.max_steady_state_bytes =
      env_size2("SUPERRECORD_MEMORY_BYTES", "SUPERRECORD_MAX_STEADY_STATE_BYTES",
                std::numeric_limits<size_t>::max());
  options.policy.min_noise_margin =
      env_size("SUPERRECORD_MIN_NOISE_MARGIN", 0);
  options.policy.expansion_heights =
      env_size_list("SUPERRECORD_EXPANSION_HEIGHTS");
  if (options.policy.expansion_heights.empty()) {
    options.policy.expansion_heights = default_expansion_heights(log_n);
  }
  options.policy.alpha_alignments =
      env_size_list("SUPERRECORD_ALPHA_ALIGNMENTS");
  if (options.policy.alpha_alignments.empty()) {
    options.policy.alpha_alignments = {1, 16};
  }
  options.policy.eval_key_registration = env_key_mode();
  options.policy.fixed_session_key_height =
      env_size("SUPERRECORD_FIXED_KEY_HEIGHT", log_n);
  options.bundle_arities = benchmark_bundle_arities(options.policy);
  return options;
}

}  // namespace tree_superrecord_benchmark_internal
