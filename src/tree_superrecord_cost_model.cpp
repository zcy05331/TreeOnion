#include "tree_superrecord_planner.h"

#include "tree_superrecord_first_dimension.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>

namespace tree_superrecord {

namespace {

void fill_u32_mod(std::vector<uint32_t> &values, uint64_t modulus,
                  uint64_t seed) {
  std::mt19937_64 rng(seed);
  for (uint32_t &value : values) {
    value = static_cast<uint32_t>(rng() % modulus);
  }
}

double measure_scan_bandwidth(const FirstDimShapeKey &key, size_t ring_degree,
                              const ParetoContext &context,
                              const CostCalibrationOptions &options) {
  const size_t rows = key.matrix_rows;
  const size_t cols = key.matrix_cols;
  require(rows > 0 && cols > 0 && ring_degree > 0,
          "scan calibration shape must be nonempty");
  const size_t a_per_level = checked_mul(rows, cols, "calibration A level");
  const size_t b_per_level =
      checked_mul(cols, size_t{2}, "calibration B level");
  const size_t out_per_level =
      checked_mul(rows, size_t{2}, "calibration output level");
  const size_t bytes_per_level =
      checked_add(checked_add(checked_mul(a_per_level, sizeof(uint32_t),
                                          "calibration A bytes"),
                              checked_mul(b_per_level, sizeof(uint32_t),
                                          "calibration B bytes"),
                              "calibration bytes"),
                  checked_mul(out_per_level, sizeof(uint64_t),
                              "calibration output bytes"),
                  "calibration bytes");
  const size_t tile_levels = std::max(
      size_t{1},
      std::min(ring_degree,
               options.max_working_set_bytes_per_call / bytes_per_level));
  std::vector<uint32_t> A(checked_mul(tile_levels, a_per_level,
                                      "calibration A"));
  std::vector<uint32_t> B(checked_mul(tile_levels, b_per_level,
                                      "calibration B"));
  std::vector<uint64_t> out(checked_mul(tile_levels, out_per_level,
                                        "calibration output"));
  fill_u32_mod(A, key.limb_modulus, options.seed ^ key.limb_modulus ^
                                      (rows << 8) ^ cols);
  fill_u32_mod(B, key.limb_modulus, options.seed ^ 0xBADC0FFEEULL ^
                                      key.limb_modulus ^ cols);
  const size_t trials = std::max(size_t{1}, options.trials_per_shape);
  const auto start = std::chrono::steady_clock::now();
  for (size_t trial = 0; trial < trials; ++trial) {
    level_mat_mat_32_dispatch(A.data(), B.data(), out.data(), rows, cols,
                              tile_levels, key.limb_modulus, context.avx512);
  }
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start)
          .count();
  volatile uint64_t sink = std::accumulate(out.begin(), out.end(), uint64_t{0});
  (void)sink;
  const double bytes =
      static_cast<double>(rows) * static_cast<double>(cols) *
      static_cast<double>(tile_levels) *
      static_cast<double>(sizeof(uint32_t)) *
      static_cast<double>(trials);
  if (!(std::isfinite(elapsed_ms) && elapsed_ms > 0)) return 0;
  return bytes / elapsed_ms;
}

}  // namespace

void apply_measured_control_timing(CostCalibration &calibration,
                                   const Candidate &candidate,
                                   const TreeServerTiming &timing) {
  const CandidateShape &s = candidate.shape;
  const CandidateMetrics &m = candidate.metrics;
  const auto per_unit = [](double ms, size_t units) {
    return units == 0 ? 0.0 : ms / static_cast<double>(units);
  };
  const double query_expand_ms = stage_ms(timing.query_expand);
  if (query_expand_ms > 0 && m.populated_expansion_nodes > 0) {
    calibration.expand_intercept_ms = 0;
    calibration.expand_per_populated_node_ms =
        per_unit(query_expand_ms, m.populated_expansion_nodes);
  }
  const double selector_ms = stage_ms(timing.selector_completion);
  if (selector_ms > 0 && s.selector_bits > 0) {
    calibration.to_rgsw_per_selector_ms = per_unit(selector_ms, s.selector_bits);
  }
  const double finalize_ms = stage_ms(timing.first_dim_finalize);
  if (finalize_ms > 0 && m.first_dim_output_ciphertexts > 0) {
    calibration.first_dim_output_ciphertext_ms =
        per_unit(finalize_ms, m.first_dim_output_ciphertexts);
  }
  const double fold_rotate_ms =
      stage_ms(timing.beta_fold) + stage_ms(timing.private_rotate);
  if (fold_rotate_ms > 0 && m.fold_cmux_count + m.rotate_cmux_count > 0) {
    calibration.cmux_ms =
        per_unit(fold_rotate_ms, m.fold_cmux_count + m.rotate_cmux_count);
  }
  const double projection_ms = stage_ms(timing.projection);
  if (projection_ms > 0 && m.projection_keyswitch_count > 0) {
    calibration.substitution_ms =
        per_unit(projection_ms, m.projection_keyswitch_count);
  }
  const double pack_ms = stage_ms(timing.path_pack);
  if (pack_ms > 0 && s.plane_count * s.path_levels > 0) {
    calibration.path_item_pack_ms =
        per_unit(pack_ms, s.plane_count * s.path_levels);
  }
  const double switch_ms = stage_ms(timing.modulus_switch);
  if (switch_ms > 0 && s.response_ciphertexts > 0) {
    calibration.modulus_switch_ms =
        per_unit(switch_ms, s.response_ciphertexts);
  }
}

CostCalibration calibrate_cost_model(
    std::span<const Candidate> candidates,
    const CostCalibration &base_calibration, const ParetoContext &context,
    const CostCalibrationOptions &options) {
  require(options.trials_per_shape > 0,
          "scan calibration needs at least one trial per shape");
  require(options.max_working_set_bytes_per_call > 0,
          "scan calibration needs a positive memory cap");
  CostCalibration calibration = base_calibration;
  for (const Candidate &candidate : candidates) {
    const std::vector<FirstDimShapeKey> keys =
        first_dimension_internal::shape_keys(candidate, context);
    for (const FirstDimShapeKey &key : keys) {
      if (calibration.scan_bandwidth_bytes_per_ms.contains(key)) continue;
      if (key.dispatch != MatMul32Dispatch::Avx512Lazy &&
          key.dispatch != MatMul32Dispatch::ScalarU128) {
        continue;
      }
      const double bandwidth = measure_scan_bandwidth(
          key, candidate.shape.ring_degree, context, options);
      if (std::isfinite(bandwidth) && bandwidth > 0) {
        calibration.scan_bandwidth_bytes_per_ms[key] = bandwidth;
      }
    }
  }
  return calibration;
}

double predict_server_ms(const Candidate &candidate,
                         const CostCalibration &cal,
                         const ParetoContext &context) {
  const CandidateShape &s = candidate.shape;
  const CandidateMetrics &m = candidate.metrics;
  double ms = cal.expand_intercept_ms;
  ms += cal.expand_per_populated_node_ms *
        static_cast<double>(m.populated_expansion_nodes);
  ms += cal.expand_per_level_ms * static_cast<double>(s.expansion_height);
  ms += cal.to_rgsw_per_selector_ms * static_cast<double>(s.selector_bits);
  ms += cal.alpha_pyramid_add_ms *
        static_cast<double>(m.alpha_pyramid_additions);
  ms += cal.alpha_carry_copy_ms * static_cast<double>(m.alpha_pyramid_carries);
  ms += cal.alpha_ntt_per_ciphertext_ms *
        static_cast<double>(m.alpha_ntt_ciphertexts);
  ms += cal.alpha_split_per_u32_output_ms *
        static_cast<double>(m.alpha_split_u32_outputs);
  const std::vector<LevelPlan> plans = build_level_plans(s);
  const std::vector<uint64_t> moduli =
      first_dimension_internal::limb_moduli(candidate.scheme.identity);
  for (const LevelPlan &plan : plans) {
    for (size_t plane = 0; plane < s.plane_count; ++plane) {
      (void)plane;
      for (uint64_t modulus : moduli) {
        const FirstDimShapeKey key =
            first_dimension_internal::shape_key(
                plan, candidate.scheme.identity, modulus, context);
        double bandwidth = cal.default_scan_bandwidth_bytes_per_ms;
        const auto found = cal.scan_bandwidth_bytes_per_ms.find(key);
        if (found != cal.scan_bandwidth_bytes_per_ms.end()) {
          bandwidth = found->second;
        }
        if (!(std::isfinite(bandwidth) && bandwidth > 0)) {
          return std::numeric_limits<double>::infinity();
        }
        const size_t one_limb_bytes = checked_mul(
            checked_mul(plan.matrix_rows, plan.matrix_cols, "scan rows cols"),
            checked_mul(s.ring_degree,
                        first_dimension_internal::limb_bytes(
                            candidate.scheme.identity),
                        "scan limb bytes"),
            "scan bytes");
        ms += static_cast<double>(one_limb_bytes) / bandwidth;
      }
    }
  }
  ms += cal.first_dim_output_ciphertext_ms *
        static_cast<double>(m.first_dim_output_ciphertexts);
  ms += cal.cmux_ms *
        static_cast<double>(m.fold_cmux_count + m.rotate_cmux_count);
  ms += cal.substitution_ms *
        static_cast<double>(m.projection_keyswitch_count);
  ms += cal.path_item_pack_ms *
        static_cast<double>(s.plane_count * s.path_levels);
  ms += cal.modulus_switch_ms * static_cast<double>(s.response_ciphertexts);
  return ms;
}

void rank_by_prediction(std::vector<Candidate> &candidates,
                        const CostCalibration &calibration,
                        const ParetoContext &context) {
  for (Candidate &candidate : candidates) {
    candidate.metrics.predicted_server_ms =
        predict_server_ms(candidate, calibration, context);
    require(std::isfinite(candidate.metrics.predicted_server_ms),
            "predicted server time must be finite");
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate &a, const Candidate &b) {
                     return a.metrics.predicted_server_ms <
                            b.metrics.predicted_server_ms;
                   });
}

}  // namespace tree_superrecord
