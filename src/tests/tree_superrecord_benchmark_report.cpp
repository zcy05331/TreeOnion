#include "tree_superrecord_benchmark_internal.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <sys/resource.h>

namespace tree_superrecord_benchmark_internal {

using namespace tree_superrecord;

Stats summarize(std::vector<double> v) {
  Stats s;
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  s.mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
  s.median = v.size() % 2 ? v[v.size() / 2]
                          : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
  s.p95 = v[std::min(v.size() - 1,
                     static_cast<size_t>(std::ceil(0.95 * v.size())) - 1)];
  double acc = 0;
  for (double x : v) acc += (x - s.mean) * (x - s.mean);
  s.stddev = v.size() > 1 ? std::sqrt(acc / (v.size() - 1)) : 0;
  return s;
}

size_t actual_response_merge_keyswitches(const CandidateShape &shape) {
  size_t total = 0;
  size_t remaining = checked_mul(shape.path_levels, shape.plane_count,
                                 "response merge item count");
  while (remaining > 0) {
    const size_t group = std::min(remaining, shape.records_per_plaintext);
    total = checked_add(total,
                        mlwe_merge_keyswitch_count(group,
                                                   shape.records_per_plaintext),
                        "response merge keyswitch count");
    remaining -= group;
  }
  return total;
}

size_t response_merge_buffer_bytes(const CandidateShape &shape,
                                   const PirParams &scheme) {
  const size_t full_ct_bytes =
      checked_mul(2, checked_mul(shape.ring_degree,
                                 scheme.get_rns_mods().size() *
                                     sizeof(uint64_t),
                                 "full-q response limb bytes"),
                  "full-q response ciphertext bytes");
  const size_t item_count = checked_mul(shape.path_levels, shape.plane_count,
                                        "response merge item count");
  return checked_mul(std::min(item_count, shape.records_per_plaintext),
                     full_ct_bytes, "response merge buffer bytes");
}

PeakRss read_peak_rss() {
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return {};
  PeakRss peak;
#if defined(__APPLE__)
  peak.bytes = static_cast<size_t>(usage.ru_maxrss);
  peak.ru_maxrss_units = "bytes";
#else
  peak.bytes = static_cast<size_t>(usage.ru_maxrss) * size_t{1024};
  peak.ru_maxrss_units = "kilobytes";
#endif
  return peak;
}

void json_string(std::ostream &out, const std::string &value) {
  out << '"';
  for (char ch : value) {
    switch (ch) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(ch)) << std::dec
              << std::setfill(' ');
        } else {
          out << ch;
        }
    }
  }
  out << '"';
}

void json_size_array(std::ostream &out, const std::vector<size_t> &values) {
  out << '[';
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) out << ", ";
    out << values[i];
  }
  out << ']';
}

void json_double_array(std::ostream &out, const std::vector<double> &values) {
  out << '[';
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) out << ", ";
    out << values[i];
  }
  out << ']';
}

void json_optional_size(std::ostream &out, const std::optional<size_t> &value) {
  if (value) out << *value;
  else out << "null";
}

std::vector<double> server_samples(const RunResult &r) {
  std::vector<double> out;
  for (const Sample &s : r.samples) out.push_back(s.server_total_ms);
  return out;
}

std::vector<double> stage_samples(const RunResult &r, size_t stage) {
  std::vector<double> out;
  for (const Sample &s : r.samples) out.push_back(s.stages_ms[stage]);
  return out;
}

std::string mib(size_t bytes) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(1)
       << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MiB";
  return text.str();
}

std::string select_case_name(tree_superrecord::SelectCase value) {
  switch (value) {
    case tree_superrecord::SelectCase::Single: return "single";
    case tree_superrecord::SelectCase::CoarsenedAlpha:
      return "coarsened_alpha";
    case tree_superrecord::SelectCase::AlphaIndex: return "alpha_index";
  }
  return "unknown";
}

void write_stats_json(std::ostream &out, const std::vector<double> &samples) {
  const Stats stats = summarize(samples);
  out << "{\"raw_samples_ms\": ";
  json_double_array(out, samples);
  out << ", \"mean_ms\": " << stats.mean << ", \"median_ms\": "
      << stats.median << ", \"p95_ms\": " << stats.p95
      << ", \"stddev_ms\": " << stats.stddev << '}';
}

void write_level_plans_json(std::ostream &out,
                            const std::vector<tree_superrecord::LevelPlan>
                                &plans) {
  out << '[';
  for (size_t i = 0; i < plans.size(); ++i) {
    if (i) out << ',';
    const tree_superrecord::LevelPlan &p = plans[i];
    out << "{\"level\": " << p.level << ", \"shift\": " << p.shift
        << ", \"logical_superrecords\": " << p.logical_superrecords
        << ", \"logical_plaintexts_per_plane\": "
        << p.logical_plaintexts_per_plane
        << ", \"matrix_rows\": " << p.matrix_rows
        << ", \"matrix_cols\": " << p.matrix_cols
        << ", \"padded_plaintexts_per_plane\": "
        << p.padded_plaintexts_per_plane << ", \"select_case\": ";
    json_string(out, select_case_name(p.select_case));
    out << ", \"alpha_coarsen\": " << p.alpha_coarsen
        << ", \"fold_bit_begin\": " << p.fold_bit_begin
        << ", \"fold_bit_count\": " << p.fold_bit_count
        << ", \"rotation_bit_begin\": " << p.rotation_bit_begin
        << ", \"rotation_bit_count\": " << p.rotation_bit_count
        << ", \"projection_depth\": " << p.projection_depth << '}';
  }
  out << ']';
}

void write_shape_json(std::ostream &out, const CandidateShape &s) {
  out << "{\"entry_bits\": " << s.entry_bits
      << ", \"leaf_count\": " << s.leaf_count
      << ", \"tree_height\": " << s.tree_height
      << ", \"path_levels\": " << s.path_levels
      << ", \"ring_degree\": " << s.ring_degree
      << ", \"bundle_arity\": " << s.bundle_arity
      << ", \"record_coeffs\": " << s.record_coeffs
      << ", \"plane_count\": " << s.plane_count
      << ", \"records_per_plaintext\": " << s.records_per_plaintext
      << ", \"record_index_bits\": " << s.record_index_bits
      << ", \"projection_depth_max\": " << s.projection_depth_max
      << ", \"leaf_superrecords\": " << s.leaf_superrecords
      << ", \"leaf_plaintexts\": " << s.leaf_plaintexts
      << ", \"selector_bits\": " << s.selector_bits
      << ", \"N0\": " << s.N0
      << ", \"beta_bits\": " << s.beta_bits
      << ", \"beta_bits_max\": " << s.beta_bits_max
      << ", \"alpha_height\": " << s.alpha_height
      << ", \"alpha_alignment\": " << s.alpha_alignment
      << ", \"query_width\": " << s.query_width
      << ", \"min_expansion_height\": " << s.min_expansion_height
      << ", \"min_expansion_capacity\": " << s.min_expansion_capacity
      << ", \"expansion_height\": " << s.expansion_height
      << ", \"expansion_capacity\": " << s.expansion_capacity
      << ", \"required_key_height\": " << s.required_key_height
      << ", \"registered_key_height\": " << s.registered_key_height
      << ", \"query_ciphertexts\": " << s.query_ciphertexts
      << ", \"response_ciphertexts\": " << s.response_ciphertexts << '}';
}

template <typename T>
void json_number_field(std::ostream &out, bool &first, const char *name,
                       const T &value) {
  if (!first) out << ',';
  first = false;
  json_string(out, name);
  out << ':';
  if constexpr (std::is_same_v<T, bool>) out << (value ? "true" : "false");
  else if constexpr (std::is_arithmetic_v<T>) out << value;
  else { out << '['; for (size_t i=0; i<value.size(); ++i) { if (i) out << ','; out << value[i]; } out << ']'; }
}

void write_identity_json(std::ostream &out, const PirSchemeIdentity &id) {
  out << '{'; bool first = true;
  json_number_field(out, first, "schema_version", id.schema_version);
  json_number_field(out, first, "ring_degree", id.ring_degree);
  json_number_field(out, first, "plaintext_modulus", id.plaintext_modulus);
  json_number_field(out, first, "rns_modulus_bits", id.rns_modulus_bits);
  json_number_field(out, first, "rns_moduli", id.rns_moduli);
  json_number_field(out, first, "response_modulus", id.response_modulus);
  json_number_field(out, first, "response_modulus_bits", id.response_modulus_bits);
  json_number_field(out, first, "selector_gadget_rows", id.selector_gadget_rows);
  json_number_field(out, first, "selector_base_log2", id.selector_base_log2);
  json_number_field(out, first, "selector_low_bits", id.selector_low_bits);
  json_number_field(out, first, "completion_key_gadget_rows", id.completion_key_gadget_rows);
  json_number_field(out, first, "completion_key_base_log2", id.completion_key_base_log2);
  json_number_field(out, first, "bv_ks_rows", id.bv_ks_rows);
  json_number_field(out, first, "bv_ks_base_log2", id.bv_ks_base_log2);
  json_number_field(out, first, "bv_ks_low_bits", id.bv_ks_low_bits);
  json_number_field(out, first, "r64_mod_q", id.r64_mod_q);
  json_number_field(out, first, "q0_inv_mod_q1", id.q0_inv_mod_q1);
  json_number_field(out, first, "composite_first_dim", id.composite_first_dim);
  json_number_field(out, first, "composite_q1", id.composite_q1);
  json_number_field(out, first, "composite_q2", id.composite_q2);
  json_number_field(out, first, "composite_w1", id.composite_w1);
  json_number_field(out, first, "composite_w2", id.composite_w2);
  json_number_field(out, first, "composite_w_crt", id.composite_w_crt);
  json_number_field(out, first, "composite_q1_inv_mod_q2", id.composite_q1_inv_mod_q2);
  json_number_field(out, first, "noise_std_dev_ieee754", id.noise_std_dev_ieee754);
  out << '}';
}

void write_fingerprint_json(std::ostream &out,
                            const SchemeFingerprint &fingerprint) {
  out << "{\"fingerprint_schema_version\":"
      << fingerprint.fingerprint_schema_version
      << ",\"payload_bits_per_coeff\":" << fingerprint.payload_bits_per_coeff
      << ",\"registered_session_key_height\":"
      << fingerprint.registered_session_key_height
      << ",\"eval_key_registration\":";
  json_string(out, key_mode_name(fingerprint.eval_key_registration));
  out << ",\"identity\":"; write_identity_json(out, fingerprint.identity); out << '}';
}

void write_plan_tag_json(std::ostream &out, const PlanTag &tag) {
  out << "{\"plan_schema_version\": " << tag.plan_schema_version
      << ", \"entry_bits\": " << tag.entry_bits
      << ", \"leaf_count\": " << tag.leaf_count
      << ", \"bundle_arity\": " << tag.bundle_arity
      << ", \"record_coeffs\": " << tag.record_coeffs
      << ", \"plane_count\": " << tag.plane_count
      << ", \"records_per_plaintext\": " << tag.records_per_plaintext
      << ", \"N0\": " << tag.N0
      << ", \"beta_bits\": " << tag.beta_bits
      << ", \"selector_bits\": " << tag.selector_bits
      << ", \"expansion_height\": " << tag.expansion_height
      << ", \"expansion_capacity\": " << tag.expansion_capacity
      << ", \"required_key_height\": " << tag.required_key_height
      << ", \"registered_key_height\": " << tag.registered_key_height;
  out << ",\"scheme\":"; write_fingerprint_json(out, tag.scheme); out << '}';
}

void write_metrics_json(std::ostream &out, const CandidateMetrics &m) {
  out << '{'; bool first = true;
  json_number_field(out, first, "logical_plaintexts", m.logical_plaintexts);
  json_number_field(out, first, "physical_matrix_cells", m.physical_matrix_cells);
  json_number_field(out, first, "physical_scan_bytes", m.physical_scan_bytes);
  json_number_field(out, first, "scalar_u128_required_scan_bytes", m.scalar_u128_required_scan_bytes);
  json_number_field(out, first, "avx512_eligible_level_plane_limbs", m.avx512_eligible_level_plane_limbs);
  json_number_field(out, first, "scalar_u128_required_level_plane_limbs", m.scalar_u128_required_level_plane_limbs);
  json_number_field(out, first, "avx512_eligible_scan_bytes", m.avx512_eligible_scan_bytes);
  json_number_field(out, first, "first_dim_output_ciphertexts", m.first_dim_output_ciphertexts);
  json_number_field(out, first, "fold_cmux_count", m.fold_cmux_count);
  json_number_field(out, first, "rotate_cmux_count", m.rotate_cmux_count);
  json_number_field(out, first, "projection_keyswitch_count", m.projection_keyswitch_count);
  json_number_field(out, first, "alpha_pyramid_ciphertexts", m.alpha_pyramid_ciphertexts);
  json_number_field(out, first, "alpha_pyramid_upper_outputs", m.alpha_pyramid_upper_outputs);
  json_number_field(out, first, "alpha_pyramid_additions", m.alpha_pyramid_additions);
  json_number_field(out, first, "alpha_pyramid_carries", m.alpha_pyramid_carries);
  json_number_field(out, first, "alpha_ntt_ciphertexts", m.alpha_ntt_ciphertexts);
  json_number_field(out, first, "alpha_split_logical_coefficients", m.alpha_split_logical_coefficients);
  json_number_field(out, first, "alpha_split_u32_outputs", m.alpha_split_u32_outputs);
  json_number_field(out, first, "populated_expansion_nodes", m.populated_expansion_nodes);
  json_number_field(out, first, "expansion_heap_object_slots", m.expansion_heap_object_slots);
  json_number_field(out, first, "expansion_return_object_slots", m.expansion_return_object_slots);
  json_number_field(out, first, "worst_case_retrieved_bundle_bits_per_level", m.worst_case_retrieved_bundle_bits_per_level);
  json_number_field(out, first, "worst_case_extra_overfetch_bits_per_level", m.worst_case_extra_overfetch_bits_per_level);
  json_number_field(out, first, "worst_case_public_overfetch_factor", m.worst_case_public_overfetch_factor);
  json_number_field(out, first, "application_payload_bits", m.application_payload_bits);
  json_number_field(out, first, "worst_case_retrieved_bundle_bits", m.worst_case_retrieved_bundle_bits);
  json_number_field(out, first, "worst_case_extra_overfetch_bits", m.worst_case_extra_overfetch_bits);
  json_number_field(out, first, "plane_capacity_bits", m.plane_capacity_bits);
  json_number_field(out, first, "padding_bits_per_bundle", m.padding_bits_per_bundle);
  json_number_field(out, first, "canonical_bytes", m.canonical_bytes);
  json_number_field(out, first, "optimized_bytes", m.optimized_bytes);
  json_number_field(out, first, "coefficient_payload_bytes", m.coefficient_payload_bytes);
  json_number_field(out, first, "query_eval_server_database_bytes_estimate", m.query_eval_server_database_bytes_estimate);
  json_number_field(out, first, "query_eval_server_intermediate_bytes_estimate", m.query_eval_server_intermediate_bytes_estimate);
  json_number_field(out, first, "query_eval_server_internal_bytes_estimate", m.query_eval_server_internal_bytes_estimate);
  json_number_field(out, first, "temporary_working_set_estimate", m.temporary_working_set_estimate);
  json_number_field(out, first, "validation_resident_bytes_estimate", m.validation_resident_bytes_estimate);
  json_number_field(out, first, "optimized_steady_state_bytes_estimate", m.optimized_steady_state_bytes_estimate);
  json_number_field(out, first, "modeled_seeded_bv_key_bytes", m.modeled_seeded_bv_key_bytes);
  json_number_field(out, first, "modeled_seeded_completion_key_bytes", m.modeled_seeded_completion_key_bytes);
  json_number_field(out, first, "modeled_per_client_eval_key_bytes", m.modeled_per_client_eval_key_bytes);
  json_number_field(out, first, "eval_key_coefficient_payload_bytes_estimate", m.eval_key_coefficient_payload_bytes_estimate);
  json_number_field(out, first, "slot_utilization", m.slot_utilization);
  json_number_field(out, first, "slot_scan_amplification", m.slot_scan_amplification);
  json_number_field(out, first, "matrix_padding_amplification", m.matrix_padding_amplification);
  json_number_field(out, first, "predicted_server_ms", m.predicted_server_ms);
  out << '}';
}

void write_key_coverage_json(std::ostream &out, const KeyCoverageReport &k) {
  out << "{\"required_key_height\": " << k.required_key_height
      << ", \"registered_key_height\": " << k.registered_key_height
      << ", \"observed_contiguous_height\": " << k.observed_contiguous_height
      << ", \"observed_key_count\": " << k.observed_key_count
      << ", \"missing_required_key_count\": " << k.missing_required
      << ", \"duplicate_key_count\": " << k.duplicate
      << ", \"unexpected_key_count\": " << k.unexpected
      << ", \"malformed_row_shape_count\": " << k.malformed_row_shapes
      << ", \"matches\": " << (k.matches ? "true" : "false") << '}';
}

void write_run_json(const std::string &path, size_t entry_bits, size_t L,
                    size_t N, size_t warmups, size_t trials,
                    uint64_t trial_seed, uint64_t execution_seed,
                    ResponsePacking response_packing, bool plan_only,
                    size_t enumerated, size_t h1_enumerated,
                    size_t h1_measured,
                    const std::vector<size_t> &bundle_arities,
                    const PlannerPolicy &policy,
                    const CostCalibration &calibration,
                    const std::optional<Candidate> &calibration_control,
                    const TreeServerTiming &calibration_timing,
                    const std::vector<size_t> &warmup_leaf_indices,
                    const std::vector<size_t> &measured_leaf_indices,
                    const std::vector<size_t> &execution_order,
                    const std::vector<std::vector<size_t>> &block_orders,
                    size_t blocked_resident_estimate,
                    const std::vector<RunResult> &results,
                    const PeakRss &peak_rss) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("failed to open benchmark JSON output");
  out << std::setprecision(17);
  out << "{\n  \"schema_version\": \"tree_superrecord_benchmark.v1\"";
  out << ",\n  \"repository_commit\": ";
  json_string(out, env_string("SUPERRECORD_COMMIT",
                              "unavailable_from_test_binary"));
  out << ",\n  \"scheme_identity\":"; write_identity_json(out, PirParams().scheme_identity());
  out << ",\n  \"plan_only\": " << (plan_only ? "true" : "false")
      << ",\n  \"run_class\": ";
  json_string(out, plan_only ? "plan_only" : (trials < 16 || warmups < 3 ? "smoke_only" : "minimum_candidate_selection"));
  out << ",\n  \"smoke_only\": " << (trials < 16 || warmups < 3 ? "true" : "false")
      << ",\n  \"entry_bits\": " << entry_bits
      << ",\n  \"leaf_count\": " << N
      << ",\n  \"tree_height\": " << L
      << ",\n  \"warmup_count\": " << warmups
      << ",\n  \"measured_trials\": " << trials
      << ",\n  \"trial_seed\": " << trial_seed
      << ",\n  \"execution_seed\": " << execution_seed
      << ",\n  \"response_packing\": ";
  json_string(out, response_packing_name(response_packing));
  out
      << ",\n  \"benchmark_bundle_arities\": ";
  if (bundle_arities.empty()) out << "null";
  else json_size_array(out, bundle_arities);
  out << ",\n  \"benchmark_shape_pin\": ";
  if (const char *pin = std::getenv("SUPERRECORD_PIN_SHAPE")) json_string(out, pin);
  else out << "null";
  out << ",\n  \"bootstrap_seed\": "
      << env_size("SUPERRECORD_BOOTSTRAP_SEED", 0x5352426f6f747374ULL);
  out << ",\n  \"leaf_schedule\": {\"warmup\": ";
  json_size_array(out, warmup_leaf_indices);
  out << ", \"measured\": ";
  json_size_array(out, measured_leaf_indices);
  out << '}';
  out << ",\n  \"planner\": {\"candidate_count\": " << enumerated
      << ", \"h1_candidate_count\": " << h1_enumerated
      << ", \"h1_measured_count\": " << h1_measured
      << ", \"max_response_ciphertexts\": "
      << policy.max_response_ciphertexts
      << ", \"max_bundle_arity\": " << policy.max_bundle_arity
      << ", \"max_retrieved_bundle_bits_per_level\": ";
  json_optional_size(out, policy.max_retrieved_bundle_bits_per_level ==
                              std::numeric_limits<size_t>::max()
                          ? std::optional<size_t>()
                          : std::optional<size_t>(
                                policy.max_retrieved_bundle_bits_per_level));
  out << ", \"max_validation_resident_bytes\": ";
  json_optional_size(out, policy.max_validation_resident_bytes ==
                              std::numeric_limits<size_t>::max()
                          ? std::optional<size_t>()
                          : std::optional<size_t>(
                                policy.max_validation_resident_bytes));
  out << ", \"max_steady_state_bytes\": ";
  json_optional_size(out, policy.max_steady_state_bytes ==
                              std::numeric_limits<size_t>::max()
                          ? std::optional<size_t>()
                          : std::optional<size_t>(
                                policy.max_steady_state_bytes));
  out << ", \"min_noise_margin\": " << policy.min_noise_margin
      << ", \"acceptance_policy\": ";
  json_string(out, policy.min_noise_margin == 0
                       ? "decryption_correctness_only"
                       : "decryption_and_minimum_noise_margin");
  out << ", \"key_registration_mode\": ";
  json_string(out, key_mode_name(policy.eval_key_registration));
  out << ", \"fixed_session_key_height\": " << policy.fixed_session_key_height
      << ", \"expansion_heights\": ";
  json_size_array(out, policy.expansion_heights);
  out << ", \"alpha_alignments\": ";
  json_size_array(out, policy.alpha_alignments);
  out << ", \"calibration\": {\"source\": \"see_calibration_evidence\", "
      << "\"expand_intercept_ms\": " << calibration.expand_intercept_ms
      << ", \"expand_per_populated_node_ms\": "
      << calibration.expand_per_populated_node_ms
      << ", \"expand_per_level_ms\": " << calibration.expand_per_level_ms
      << ", \"to_rgsw_per_selector_ms\": "
      << calibration.to_rgsw_per_selector_ms
      << ", \"alpha_pyramid_add_ms\": "
      << calibration.alpha_pyramid_add_ms
      << ", \"alpha_carry_copy_ms\": "
      << calibration.alpha_carry_copy_ms
      << ", \"alpha_ntt_per_ciphertext_ms\": "
      << calibration.alpha_ntt_per_ciphertext_ms
      << ", \"alpha_split_per_u32_output_ms\": "
      << calibration.alpha_split_per_u32_output_ms
      << ", \"scan_bandwidth_entry_count\": "
      << calibration.scan_bandwidth_bytes_per_ms.size()
      << ", \"default_scan_bandwidth_bytes_per_ms\": "
      << calibration.default_scan_bandwidth_bytes_per_ms
      << ", \"first_dim_output_ciphertext_ms\": "
      << calibration.first_dim_output_ciphertext_ms
      << ", \"cmux_ms\": " << calibration.cmux_ms
      << ", \"substitution_ms\": " << calibration.substitution_ms
      << ", \"path_item_pack_ms\": " << calibration.path_item_pack_ms
      << ", \"modulus_switch_ms\": " << calibration.modulus_switch_ms
      << "}}";
  out << ",\n  \"calibration_evidence\": {\"source\":";
  json_string(out, plan_only ? "unmeasured plan preview" : "measured pilot stages, independent alpha operation microbenchmarks, and actual per-limb kernel shapes");
  out << ",\"pilot_candidate\":";
  if (calibration_control) write_shape_json(out, calibration_control->shape); else out << "null";
  out << ",\"pilot_total_ms\":" << stage_ms(calibration_timing.total)
      << ",\"kernel_trials_per_shape\":2,\"kernel_max_working_set_bytes_per_call\":67108864,\"kernel_bandwidths\":[";
  bool first_bandwidth=true;
  for (const auto &[key, bandwidth] : calibration.scan_bandwidth_bytes_per_ms) {
    if (!first_bandwidth) out << ',';
    first_bandwidth=false;
    out << "{\"rows\":" << key.matrix_rows << ",\"cols\":" << key.matrix_cols
        << ",\"cols_mod_simd_width\":" << key.cols_mod_simd_width << ",\"limb_modulus\":" << key.limb_modulus
        << ",\"dispatch\":";
    json_string(out, key.dispatch == MatMul32Dispatch::Avx512Lazy ? "Avx512Lazy" : "ScalarU128");
    out << ",\"bytes_per_ms\":" << bandwidth << '}';
  }
  out << "]}";
  out << ",\n  \"candidate_execution_order\": ";
  json_size_array(out, execution_order);
  out << ",\n  \"measurement_schedule\":\"interleaved_blocks\",\n  \"blocked_resident_estimate_bytes\":" << blocked_resident_estimate;
  out << ",\n  \"block_execution_orders\":[";
  for (size_t block=0;block<block_orders.size();++block) {
    if (block) out << ',';
    out << "{\"warmup\":" << (block<warmups ? "true" : "false")
        << ",\"block_id\":" << (block<warmups ? block : block-warmups)
        << ",\"leaf\":" << (block<warmups ? warmup_leaf_indices[block] : measured_leaf_indices[block-warmups])
        << ",\"candidate_indices\":";
    json_size_array(out,block_orders[block]); out << '}';
  }
  out << ']';
  out << ",\n  \"process_peak_rss_bytes\": ";
  json_optional_size(out, peak_rss.bytes);
  out << ",\n  \"ru_maxrss_units\": ";
  json_string(out, peak_rss.ru_maxrss_units);
  out << ",\n  \"candidates\": [";
  for (size_t i = 0; i < results.size(); ++i) {
    const RunResult &r = results[i];
    if (i) out << ',';
    out << "\n    {\"row_id\": ";
    json_string(out, r.row_id);
    out << ", \"label\": ";
    json_string(out, r.label);
    out << ", \"description\": ";
    json_string(out, r.description);
    out << ", \"is_generic\": " << (r.is_generic ? "true" : "false")
        << ", \"is_h1\": " << (r.is_h1 ? "true" : "false")
        << ", \"mandatory_control\": "
        << (r.is_mandatory ? "true" : "false")
        << ", \"eligible\": " << (r.eligible ? "true" : "false")
        << ", \"ineligible_reason\": ";
    if (r.reason.empty()) out << "null";
    else json_string(out, r.reason);
    out << ", \"setup_ms\": " << r.setup_ms
        << ", \"preprocess_ms\": " << r.setup_ms
        << ", \"session_setup_ms\": " << r.session_setup_ms
        << ", \"noise_gate_ms\": " << r.noise_gate_ms
        << ", \"query_ms_avg\": " << r.query_ms_avg
        << ", \"client_decode_ms_avg\": " << r.extract_ms_avg
        << ", \"memory\": {\"database_bytes\": " << r.db_bytes
        << ", \"process_peak_rss_bytes\": ";
    out << "null";
    out << ",\"peak_scope\":\"see run process peak; candidate peaks not isolated\"}, \"wire\": {\"modeled_seeded_query_bytes\": " << r.query_bytes
        << ", \"actual_query_wire_bytes\": null"
        << ", \"actual_response_ciphertext_payload_bytes\": ";
    if (r.response_bytes == 0) out << "null";
    else out << r.response_bytes;
    out << "}, \"workload\": {\"plaintext_cells\": " << r.plaintexts
        << ", \"physical_scan_bytes\": " << r.scan_bytes
        << ", \"min_budget_bits\": " << r.min_budget
        << ", \"max_noise\": " << r.max_noise
        << ", \"real_non_target_neighbour_slots_avg\": "
        << r.neighbour_slots_avg << '}';
    if (r.candidate) {
      out << ", \"response_packing_metadata\": {\"mode\": ";
      json_string(out, response_packing_name(response_packing));
      out << ", \"planner_projection_keyswitch_count\": "
          << r.candidate->metrics.projection_keyswitch_count
          << ", \"actual_mlwe_merge_keyswitch_count\": "
          << (response_packing == ResponsePacking::MlweMerge
                  ? actual_response_merge_keyswitches(r.candidate->shape)
                  : size_t{0})
          << ", \"response_merge_buffer_bytes\": "
          << (response_packing == ResponsePacking::MlweMerge
                  ? response_merge_buffer_bytes(r.candidate->shape,
                                                PirParams())
                  : size_t{0})
          << '}';
    } else {
      out << ", \"response_packing_metadata\": null";
    }
    out << ", \"key_coverage\": ";
    write_key_coverage_json(out, r.key_coverage);
    if (r.candidate) {
      const TreeParams params = freeze_candidate(*r.candidate);
      out << ", \"shape\": ";
      write_shape_json(out, r.candidate->shape);
      out << ", \"candidate_metrics\": ";
      write_metrics_json(out, r.candidate->metrics);
      out << ", \"plan_tag\": ";
      write_plan_tag_json(out, params.tag);
      out << ", \"scheme_fingerprint\": ";
      write_fingerprint_json(out, r.candidate->scheme);
      out << ", \"level_plans\": ";
      write_level_plans_json(out, params.level_plans);
    } else {
      out << ", \"shape\": null, \"candidate_metrics\": null, "
             "\"plan_tag\": null, \"scheme_fingerprint\": null, "
             "\"level_plans\": []";
    }
    out << ",\"dispatch_signature\":";
    if (r.candidate) {
      const auto availability = detected_matmul32_availability();
      const auto signature = first_dim_dispatch_signature(*r.candidate, {availability});
      out << "{\"compiled_avx512\":" << (availability.compiled_with_required_avx512_kernel ? "true" : "false")
          << ",\"runtime_avx512\":" << (availability.runtime_cpu_supports_required_avx512_features ? "true" : "false")
          << ",\"level_plane_limb_order\":[";
      for (size_t j=0; j<signature.limb_dispatch.size(); ++j) {
        if (j) out << ',';
        json_string(out, signature.limb_dispatch[j] == MatMul32Dispatch::Avx512Lazy ? "Avx512Lazy" : "ScalarU128");
      }
      out << "]}";
    } else out << "null";
    out << ",\"target_noise_result\":";
    if (r.noise_result) {
      const auto &noise = *r.noise_result;
      out << "{\"client_id\":" << noise.client_id
          << ",\"fixture_kind\":\"TargetShapeNoiseGate\",\"eligibility\":";
      json_string(out, noise.eligibility == NoiseEligibility::Passed ? "Passed" : "Failed");
      out << ",\"decrypts_correctly\":" << (noise.decrypts_correctly ? "true" : "false")
          << ",\"response_packing\":";
      json_string(out, response_packing_name(noise.packing));
      out << ",\"observed_key_coverage_matches_tag\":" << (noise.observed_key_coverage_matches_tag ? "true" : "false")
          << ",\"min_noise_margin\":" << noise.min_noise_margin
          << ",\"minimum_margin\":" << noise.minimum_margin << ",\"tag\":";
      write_plan_tag_json(out, noise.tag);
      out << ",\"checkpoints\":[";
      for (size_t j=0; j<noise.checkpoints.size(); ++j) {
        if (j) out << ',';
        const auto &checkpoint=noise.checkpoints[j];
        out << "{\"stage\":"; json_string(out, checkpoint.stage);
        out << ",\"kind\":";
        json_string(out, checkpoint.kind == NoiseMeasureKind::RemainingBudgetBits ? "RemainingBudgetBits" : "Log2AbsoluteNoise");
        out << ",\"value\":" << checkpoint.value << ",\"decoder_threshold\":" << checkpoint.decoder_threshold
            << ",\"margin\":" << checkpoint.margin << '}';
      }
      out << "]}";
    } else out << "null";
    out << ", \"statistics\": {\"server_total\": ";
    write_stats_json(out, server_samples(r));
    out << ", \"stages\": {";
    for (size_t s = 0; s < kStageLabels.size(); ++s) {
      if (s) out << ", ";
      json_string(out, kStageLabels[s]);
      out << ": ";
      write_stats_json(out, stage_samples(r, s));
    }
    out << "}}, \"samples\": [";
    for (size_t s = 0; s < r.samples.size(); ++s) {
      if (s) out << ',';
      const Sample &sample = r.samples[s];
      out << "{\"block_id\": " << sample.block_id
          << ", \"leaf\": " << sample.leaf
          << ", \"server_total_ms\": " << sample.server_total_ms
          << ", \"client_query_ms\": " << sample.client_query_ms
          << ", \"client_decode_ms\": " << sample.client_decode_ms
          << ", \"response_serialize_ms\": " << sample.response_serialize_ms
          << ", \"min_budget_bits\": " << sample.min_budget_bits
          << ", \"max_noise\": " << sample.max_noise
          << ", \"response_merge_keyswitches\": "
          << sample.response_merge_keyswitches
          << ", \"response_payload_bytes\": "
          << sample.response_payload_bytes
          << ", \"real_non_target_neighbour_slots\": "
          << sample.real_non_target_neighbour_slots
          << ", \"stages_ms\": {";
      for (size_t st = 0; st < kStageLabels.size(); ++st) {
        if (st) out << ", ";
        json_string(out, kStageLabels[st]);
        out << ": " << sample.stages_ms[st];
      }
      out << "}}";
    }
    out << "]}";
  }
  out << "\n  ]\n}\n";
}

void print_benchmark_summary(const std::vector<RunResult> &rows,
                             const BenchmarkOptions &options, uint64_t t,
                             const PirParams &scheme) {
  BENCH_PRINT("");
  BENCH_PRINT("generic super-record benchmark: N=2^" << options.tree_height
              << " leaves, E=" << options.entry_bits << " bits, "
              << options.trials << " measured trials after "
              << options.warmups << " warmup, trial seed "
              << options.trial_seed
              << (options.plan_only ? " (plan-only)" : ""));
  double legacy_mean = 0;
  for (const RunResult &r : rows) {
    if (r.label == "legacy-g32" && r.eligible) {
      legacy_mean = summarize(server_samples(r)).mean;
    }
  }
  double best_h1_mean = 0;
  for (const RunResult &r : rows) {
    if (r.is_h1 && r.eligible) {
      const double mean = summarize(server_samples(r)).mean;
      if (best_h1_mean == 0 || mean < best_h1_mean) best_h1_mean = mean;
    }
  }
  for (const RunResult &r : rows) {
    BENCH_PRINT("[" << r.row_id << " " << r.label << "] " << r.description);
    if (!r.eligible) {
      BENCH_PRINT("  ineligible/unmeasured: "
                  << (r.reason.empty()
                          ? std::string(options.plan_only ? "plan-only"
                                                          : "not executed")
                          : r.reason));
      continue;
    }
    const std::vector<double> totals = server_samples(r);
    const Stats s = summarize(totals);
    BENCH_PRINT("  database " << mib(r.db_bytes) << " (" << r.plaintexts
                << " plaintext cells, scan " << mib(r.scan_bytes)
                << " per query), setup " << r.setup_ms << " ms");
    BENCH_PRINT("  server total mean " << s.mean << " ms, median " << s.median
                << ", p95 " << s.p95 << ", stddev " << s.stddev
                << (legacy_mean > 0 ? " ; speedup vs legacy " +
                                          std::to_string(legacy_mean / s.mean) +
                                          "x"
                                    : std::string())
                << (best_h1_mean > 0 && r.is_generic && !r.is_h1
                        ? " ; speedup vs fastest eligible measured h1 " +
                              std::to_string(best_h1_mean / s.mean) + "x"
                        : std::string()));
    std::string breakdown = "  stages avg (ms):";
    double stage_total = 0;
    for (size_t k = 0; k < kStageLabels.size(); ++k) {
      const double v = summarize(stage_samples(r, k)).mean;
      stage_total += v;
      breakdown += std::string(" ") + kStageLabels[k] + " " +
                   std::to_string(v);
    }
    BENCH_PRINT(breakdown);
    BENCH_PRINT("  stages sum " << stage_total << " ms; client query "
                << r.query_ms_avg << " ms, client extract "
                << r.extract_ms_avg << " ms");
    BENCH_PRINT("  throughput " << static_cast<double>(r.scan_bytes) /
                                       (1024.0 * 1024.0) / (s.mean / 1000.0)
                << " MiB/s scanned; communication: modeled query "
                << r.query_bytes << " B, actual response payload "
                << r.response_bytes << " B; noise: min budget "
                << r.min_budget << " bits, max residue " << r.max_noise
                << " of decoder bound "
                << static_cast<double>(scheme.get_small_q()) / (2.0 * t)
                << "; real non-target neighbour slots/trial "
                << r.neighbour_slots_avg);
    std::string samples = "  samples(server):";
    for (double v : totals) samples += " " + std::to_string(v);
    BENCH_PRINT(samples);
  }
}

}  // namespace tree_superrecord_benchmark_internal
