#include "layer_layout_planner.h"

#include "utils.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace {

// ---------------------------------------------------------------------------
// Features and enumeration
// ---------------------------------------------------------------------------

uint64_t checked_mul(uint64_t a, uint64_t b, const char *what) {
  if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) {
    throw std::overflow_error(std::string(what) + " overflows uint64_t");
  }
  return a * b;
}

size_t node_count_of_level(size_t level) {
  if (level == 0 || level >= std::numeric_limits<size_t>::digits) {
    throw std::invalid_argument("layer planner: level out of range");
  }
  return size_t{1} << level;
}

std::tuple<size_t, size_t, size_t, size_t> legacy_score(
    const LayerLayoutCandidate &c) {
  const PirParams &p = c.layout.params;
  return std::make_tuple(p.get_num_pt(), c.features.useful_expanded_ciphertexts,
                         p.get_num_other_dims(), p.get_expan_height());
}

bool same_shape(const LayerLayoutFeatures &a, const LayerLayoutFeatures &b) {
  return a.expansion_height == b.expansion_height &&
         a.first_dim_size == b.first_dim_size &&
         a.other_dim_size == b.other_dim_size &&
         a.other_dim_count == b.other_dim_count &&
         a.padded_plaintexts == b.padded_plaintexts;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public geometry and enumeration
// ---------------------------------------------------------------------------

size_t count_pruned_expansion_substitutions(size_t useful, size_t height) {
  if (height >= std::numeric_limits<size_t>::digits) {
    throw std::invalid_argument("expansion height too large");
  }
  const size_t capacity = size_t{1} << height;
  size_t count = 0;
  for (size_t i = 1; i < capacity; ++i) {
    const size_t k = size_t{1} << (std::bit_width(i) - 1);
    const size_t left_leaf = i * capacity / k - capacity;
    if (left_leaf < useful) ++count;
  }
  return count;
}

LayerLayoutFeatures compute_layer_layout_features(const PirParams &params) {
  LayerLayoutFeatures f;
  f.expansion_height = params.get_expan_height();
  f.first_dim_size = params.get_fst_dim_sz();
  f.other_dim_size = params.get_other_dim_sz();
  f.other_dim_count = params.get_num_other_dims();
  f.padded_plaintexts = params.get_num_pt();
  f.padding_plaintexts = params.get_num_pt() - params.get_target_num_pt();
  f.useful_expanded_ciphertexts =
      params.get_fst_dim_sz() + params.get_l() * params.get_num_other_dims();
  f.expansion_substitutions = count_pruned_expansion_substitutions(
      f.useful_expanded_ciphertexts, f.expansion_height);
  f.first_dim_query_ntts = 2 * params.get_fst_dim_sz();
  f.selector_rows_to_complete = params.get_l() * params.get_num_other_dims();
  f.crt_coefficients_to_compose =
      params.get_composite_rns().enabled
          ? 2 * params.get_poly_degree() * params.get_other_dim_sz()
          : 0;
  f.inverse_ntts = 2 * params.get_other_dim_sz();
  f.cmux_count = params.get_other_dim_sz() - 1;
  f.logical_padded_bytes =
      checked_mul(params.get_num_pt(), params.get_pt_size(), "padded bytes");
  f.physical_scan_bytes = checked_mul(
      checked_mul(params.get_num_pt(), params.get_coeff_val_cnt(),
                  "scan coefficients"),
      sizeof(uint64_t), "physical scan bytes");
  return f;
}

std::vector<LayerLayoutCandidate> enumerate_layer_layout_candidates(
    size_t level, size_t nodes_per_pt, const PirParams &reference) {
  if (nodes_per_pt == 0) {
    throw std::invalid_argument("layer planner: nodes per plaintext must be positive");
  }
  const size_t node_count = node_count_of_level(level);
  const size_t target_num_pt = utils::roundup_div(node_count, nodes_per_pt);
  const size_t max_other_dims = reference.get_num_other_dims();

  std::vector<LayerLayoutCandidate> candidates;
  for (size_t height = 0; height <= reference.get_expan_height(); ++height) {
    PirParams params = reference;
    try {
      params = reference.with_layout(
          {target_num_pt, height, reference.get_fst_dim_pow2()});
    } catch (const std::runtime_error &) {
      continue;  // this height cannot represent the level
    }
    if (params.get_num_other_dims() > max_other_dims) continue;
    if (!reference.scheme_compatible(params)) continue;
    LayerLayoutCandidate c;
    c.layout = {level, node_count, target_num_pt, params};
    c.layout.direct_return = node_count <= nodes_per_pt;
    c.features = compute_layer_layout_features(params);
    candidates.push_back(std::move(c));
  }
  if (candidates.empty()) {
    throw std::runtime_error("No scheme-compatible PIR layout for Merkle layer");
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const LayerLayoutCandidate &a,
                      const LayerLayoutCandidate &b) {
                     return std::make_tuple(a.features.expansion_height,
                                            a.features.first_dim_size,
                                            a.features.other_dim_size) <
                            std::make_tuple(b.features.expansion_height,
                                            b.features.first_dim_size,
                                            b.features.other_dim_size);
                   });
  return candidates;
}

size_t legacy_layer_layout_candidate(
    const std::vector<LayerLayoutCandidate> &candidates) {
  if (candidates.empty()) {
    throw std::invalid_argument("layer planner: no candidates");
  }
  size_t best = 0;
  for (size_t i = 1; i < candidates.size(); ++i) {
    if (legacy_score(candidates[i]) < legacy_score(candidates[best])) best = i;
  }
  return best;
}

std::vector<LayerLayoutCandidate> pareto_layer_layout_candidates(
    const std::vector<LayerLayoutCandidate> &candidates) {
  const auto dominates = [](const LayerLayoutFeatures &a,
                            const LayerLayoutFeatures &b) {
    const bool no_worse = a.physical_scan_bytes <= b.physical_scan_bytes &&
                          a.expansion_substitutions <= b.expansion_substitutions &&
                          a.first_dim_query_ntts <= b.first_dim_query_ntts &&
                          a.selector_rows_to_complete <=
                              b.selector_rows_to_complete &&
                          a.inverse_ntts <= b.inverse_ntts &&
                          a.cmux_count <= b.cmux_count;
    const bool better = a.physical_scan_bytes < b.physical_scan_bytes ||
                        a.expansion_substitutions < b.expansion_substitutions ||
                        a.first_dim_query_ntts < b.first_dim_query_ntts ||
                        a.selector_rows_to_complete <
                            b.selector_rows_to_complete ||
                        a.inverse_ntts < b.inverse_ntts ||
                        a.cmux_count < b.cmux_count;
    return no_worse && better;
  };
  std::vector<LayerLayoutCandidate> frontier;
  for (size_t i = 0; i < candidates.size(); ++i) {
    bool dominated = false;
    for (size_t j = 0; j < candidates.size() && !dominated; ++j) {
      if (i != j && dominates(candidates[j].features, candidates[i].features)) {
        dominated = true;
      }
    }
    if (!dominated) frontier.push_back(candidates[i]);
  }
  return frontier;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

LayerLayoutSelection select_layer_layouts(const LayerLayoutProfile &profile,
                                          size_t tree_height,
                                          size_t nodes_per_pt,
                                          const PirParams &reference,
                                          double padding_budget) {
  if (!(padding_budget >= 1.0)) {
    throw std::invalid_argument("layer padding budget must be >= 1.0");
  }
  if (tree_height == 0) {
    throw std::invalid_argument("layer planner: empty tree");
  }

  struct Option {
    size_t height = 0;
    size_t extra_plaintexts = 0;  // relative to the legacy layout of the level
    double mean_ms = 0.0;
    LayerLayoutFeatures features;
  };
  struct LevelPlan {
    size_t legacy_height = 0;
    size_t legacy_num_pt = 0;
    double legacy_mean_ms = 0.0;  // 0 when unmeasured
    std::vector<Option> options;    // measured candidates only
  };

  std::vector<LevelPlan> plans(tree_height);
  uint64_t legacy_total = 0;
  for (size_t level = 1; level <= tree_height; ++level) {
    LevelPlan &plan = plans[level - 1];
    const std::vector<LayerLayoutCandidate> candidates =
        enumerate_layer_layout_candidates(level, nodes_per_pt, reference);
    const size_t legacy = legacy_layer_layout_candidate(candidates);
    plan.legacy_height = candidates[legacy].features.expansion_height;
    plan.legacy_num_pt = candidates[legacy].features.padded_plaintexts;
    legacy_total += plan.legacy_num_pt;
    const bool direct = candidates[legacy].layout.direct_return;
    for (const LayerLayoutMeasurement &m : profile.measurements) {
      if (m.level != level) continue;
      const auto it = std::find_if(
          candidates.begin(), candidates.end(),
          [&](const LayerLayoutCandidate &c) {
            return c.features.expansion_height == m.features.expansion_height;
          });
      if (it == candidates.end() || !same_shape(it->features, m.features)) {
        throw std::invalid_argument(
            "layer layout profile: level " + std::to_string(level) +
            " height " + std::to_string(m.features.expansion_height) +
            " does not match a legal runtime layout");
      }
      if (direct) continue;  // served in the clear: no PIR layout to choose
      Option option;
      option.height = m.features.expansion_height;
      option.extra_plaintexts =
          m.features.padded_plaintexts - plan.legacy_num_pt;
      if (m.server_samples_ms.empty()) {
        throw std::invalid_argument("layer layout profile: no server samples");
      }
      option.mean_ms = std::accumulate(m.server_samples_ms.begin(),
                                       m.server_samples_ms.end(), 0.0) /
                       m.server_samples_ms.size();
      option.features = it->features;
      if (option.height == plan.legacy_height) {
        plan.legacy_mean_ms = option.mean_ms;
      }
      plan.options.push_back(option);
    }
  }

  const uint64_t cap_total = static_cast<uint64_t>(
      std::floor(static_cast<double>(legacy_total) * padding_budget));
  const size_t cap_extra =
      cap_total > legacy_total ? static_cast<size_t>(cap_total - legacy_total)
                               : 0;

  // dp[e] = minimal predicted time using exactly e extra plaintexts so far.
  constexpr double kInf = std::numeric_limits<double>::infinity();
  std::vector<double> dp(cap_extra + 1, kInf);
  dp[0] = 0.0;
  std::vector<std::vector<int>> choice(tree_height,
                                       std::vector<int>(cap_extra + 1, -1));
  for (size_t level = 1; level <= tree_height; ++level) {
    const LevelPlan &plan = plans[level - 1];
    std::vector<double> next(cap_extra + 1, kInf);
    if (plan.options.empty()) {
      next = dp;  // legacy layout, no measured time
      continue;
    }
    for (size_t e = 0; e <= cap_extra; ++e) {
      if (dp[e] == kInf) continue;
      for (size_t o = 0; o < plan.options.size(); ++o) {
        const size_t ne = e + plan.options[o].extra_plaintexts;
        if (ne > cap_extra) continue;
        const double t = dp[e] + plan.options[o].mean_ms;
        if (t < next[ne]) {
          next[ne] = t;
          choice[level - 1][ne] = static_cast<int>(o);
        }
      }
    }
    dp = std::move(next);
  }
  size_t best_e = 0;
  for (size_t e = 0; e <= cap_extra; ++e) {
    if (dp[e] < dp[best_e]) best_e = e;
  }
  if (dp[best_e] == kInf) {
    throw std::runtime_error(
        "layer layout profile: no measured combination fits the budget");
  }

  LayerLayoutSelection selection;
  selection.expansion_heights.assign(tree_height, 0);
  selection.legacy_total_padded_plaintexts = legacy_total;
  std::vector<int> chosen(tree_height, -1);
  size_t e = best_e;
  for (size_t level = tree_height; level >= 1; --level) {
    const LevelPlan &plan = plans[level - 1];
    if (!plan.options.empty()) {
      const int o = choice[level - 1][e];
      if (o < 0) {
        throw std::logic_error("layer layout selection lost its DP trace");
      }
      chosen[level - 1] = o;
      e -= plan.options[o].extra_plaintexts;
    }
    if (level == 1) break;
  }

  // Storage breaks exact latency ties; it must not replace a faster layout.
  const auto tie_key = [](const Option &o) {
    return std::make_tuple(o.features.physical_scan_bytes,
                           o.features.expansion_substitutions,
                           o.features.inverse_ntts, o.height);
  };
  uint64_t selected_total = 0;
  for (size_t level = 1; level <= tree_height; ++level) {
    LevelPlan &plan = plans[level - 1];
    size_t height = plan.legacy_height;
    size_t num_pt = plan.legacy_num_pt;
    double mean = plan.legacy_mean_ms;
    if (chosen[level - 1] >= 0) {
      Option pick = plan.options[static_cast<size_t>(chosen[level - 1])];
      for (const Option &alt : plan.options) {
        if (alt.extra_plaintexts <= pick.extra_plaintexts &&
            alt.mean_ms == pick.mean_ms &&
            tie_key(alt) < tie_key(pick)) {
          pick = alt;
        }
      }
      height = pick.height;
      num_pt = plan.legacy_num_pt + pick.extra_plaintexts;
      mean = pick.mean_ms;
    }
    selection.expansion_heights[level - 1] = height;
    selected_total += num_pt;
    selection.predicted_selected_ms += mean;
    selection.predicted_legacy_ms += plan.legacy_mean_ms;
  }
  selection.selected_total_padded_plaintexts = selected_total;
  return selection;
}

// ---------------------------------------------------------------------------
// Policy-aware planner entry
// ---------------------------------------------------------------------------

const char *layer_layout_policy_name(LayerLayoutPolicy policy) {
  switch (policy) {
    case LayerLayoutPolicy::legacy_padding: return "legacy";
    case LayerLayoutPolicy::profiled: return "profiled";
  }
  return "unknown";
}

std::vector<LayerLayout> plan_layer_layouts(size_t tree_height,
                                            size_t nodes_per_pt,
                                            const PirParams &reference,
                                            const LayerPlannerConfig &config,
                                            bool *used_fallback) {
  if (used_fallback) *used_fallback = false;
  if (config.policy == LayerLayoutPolicy::legacy_padding) {
    return plan_layer_layouts(tree_height, nodes_per_pt, reference);
  }
  if (!config.profile) {
    throw std::invalid_argument(
        "profiled layer layout policy needs a layout profile");
  }
  const LayerLayoutProfileEnvironment runtime = detect_layer_layout_environment(
      reference, config.profile->environment.commit,
      config.profile->environment.build_type,
      config.profile->environment.config);
  const std::string mismatch = describe_layer_layout_profile_mismatch(
      *config.profile, runtime, tree_height, nodes_per_pt);
  if (!mismatch.empty()) {
    if (!config.allow_profile_fallback) {
      throw std::invalid_argument(mismatch);
    }
    if (used_fallback) *used_fallback = true;
    return plan_layer_layouts(tree_height, nodes_per_pt, reference);
  }
  const LayerLayoutSelection selection = select_layer_layouts(
      *config.profile, tree_height, nodes_per_pt, reference,
      config.total_padding_budget);

  std::vector<LayerLayout> layouts;
  layouts.reserve(tree_height);
  for (size_t level = 1; level <= tree_height; ++level) {
    const std::vector<LayerLayoutCandidate> candidates =
        enumerate_layer_layout_candidates(level, nodes_per_pt, reference);
    const size_t height = selection.expansion_heights[level - 1];
    const auto it = std::find_if(
        candidates.begin(), candidates.end(),
        [&](const LayerLayoutCandidate &c) {
          return c.features.expansion_height == height;
        });
    if (it == candidates.end()) {
      throw std::logic_error("selected expansion height is not a legal layout");
    }
    layouts.push_back(it->layout);
  }
  return layouts;
}
