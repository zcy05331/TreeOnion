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

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <functional>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <set>
#include <string>
#include <sys/resource.h>

using namespace tree_superrecord;
using namespace tree_superrecord_test;

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<const char *, 10> kStageLabels = {
    "expand", "complete", "prepare", "scan", "finalize",
    "fold",   "rotate",   "project", "pack", "switch"};

double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Measure the four preparation operations in their own physical units.
void calibrate_alpha_operations(CostCalibration &calibration, PirClient &client,
                                 const PirParams &scheme) {
  constexpr size_t repetitions = 64;
  constexpr size_t batch = 16;
  volatile uint64_t sink = 0;
  const RlweCt seed = client.fresh_zero_ct();
  RlweCt accumulator = seed;
  auto start = Clock::now();
  for (size_t i = 0; i < repetitions; ++i) tree_ct_add_inplace(accumulator, seed, scheme);
  calibration.alpha_pyramid_add_ms = ms_since(start) / repetitions;
  sink = accumulator.c0[0];
  std::vector<RlweCt> carries;
  carries.reserve(repetitions);
  start = Clock::now();
  for (size_t i = 0; i < repetitions; ++i) carries.push_back(seed);
  calibration.alpha_carry_copy_ms = ms_since(start) / repetitions;
  sink = sink ^ carries.back().c0[0];
  const ::AlphaPyramid coefficients{std::vector<RlweCt>(batch, seed)};
  (void)::pyramid_to_ntt(coefficients, scheme);
  start = Clock::now();
  const ::AlphaPyramid ntt = ::pyramid_to_ntt(coefficients, scheme);
  calibration.alpha_ntt_per_ciphertext_ms = ms_since(start) / batch;
  (void)::pyramid_to_m32(ntt, scheme);
  start = Clock::now();
  const ::AlphaPyramidM32 split = ::pyramid_to_m32(ntt, scheme);
  calibration.alpha_split_per_u32_output_ms = ms_since(start) /
      static_cast<double>(batch * 4 * scheme.get_poly_degree());
  sink = sink ^ split.lo[0][0];
  (void)sink;
}

struct Stats {
  double mean = 0, median = 0, p95 = 0, stddev = 0;
};

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

struct Sample {
  size_t block_id = 0;
  size_t leaf = 0;
  double server_total_ms = 0;
  std::array<double, 10> stages_ms{};
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
  std::optional<Candidate> candidate;
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
  std::optional<CandidateNoiseResult> noise_result;
};

struct PeakRss {
  std::optional<size_t> bytes;
  std::string ru_maxrss_units;
};

size_t env_size(const char *name, size_t fallback) {
  const char *value = std::getenv(name);
  if (!value || !*value) return fallback;
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (*end != '\0' ||
      parsed > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
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

KeyCoverageReport inspect_keys(const TreeParams &params,
                               const TaggedTreeSessionKeys &session) {
  KeyCoverageReport out;
  out.required_key_height = params.shape.required_key_height;
  out.registered_key_height = params.shape.registered_key_height;
  const BvKeyCoverage coverage =
      inspect_bv_key_coverage(session.material->bv_galois_keys,
                              params.scheme.identity,
                              params.shape.registered_key_height);
  out.observed_contiguous_height = coverage.contiguous_height;
  out.observed_key_count = coverage.key_count;
  out.missing_required = coverage.missing_required.size();
  out.duplicate = coverage.duplicates.size();
  out.unexpected = coverage.unexpected.size();
  out.malformed_row_shapes = coverage.malformed_row_shapes.size();
  out.matches = out.observed_contiguous_height >= out.required_key_height &&
                out.observed_key_count == out.registered_key_height &&
                out.missing_required == 0 && out.duplicate == 0 &&
                out.unexpected == 0 && out.malformed_row_shapes == 0;
  return out;
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
                            const std::vector<tree_superrecord::LevelPlan> &plans) {
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
void json_number_field(std::ostream &out, bool &first, const char *name, const T &value) {
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
  json_number_field(out, first, "completion_key_gadget_rows", id.completion_key_gadget_rows);
  json_number_field(out, first, "completion_key_base_log2", id.completion_key_base_log2);
  json_number_field(out, first, "bv_ks_rows", id.bv_ks_rows);
  json_number_field(out, first, "bv_ks_base_log2", id.bv_ks_base_log2);
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

void write_fingerprint_json(std::ostream &out, const SchemeFingerprint &fingerprint) {
  out << "{\"fingerprint_schema_version\":" << fingerprint.fingerprint_schema_version
      << ",\"payload_bits_per_coeff\":" << fingerprint.payload_bits_per_coeff
      << ",\"registered_session_key_height\":" << fingerprint.registered_session_key_height
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
                    bool plan_only, size_t enumerated, size_t h1_enumerated,
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
      << ",\n  \"benchmark_bundle_arities\": ";
  if (bundle_arities.empty()) out << "null";
  else json_size_array(out, bundle_arities);
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
          << ",\"observed_key_coverage_matches_tag\":" << (noise.observed_key_coverage_matches_tag ? "true" : "false")
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

void add_candidate(std::vector<RunResult> &rows, const Candidate &cand,
                   const std::string &label, bool mandatory,
                   const ParetoContext &context) {
  const PlanTag tag = make_plan_tag(cand.shape, cand.scheme);
  const std::vector<tree_superrecord::LevelPlan> plans =
      build_level_plans(cand.shape);
  const FirstDimDispatchSignature dispatch =
      first_dim_dispatch_signature(cand, context);
  for (const RunResult &existing : rows) {
    if (!existing.candidate) continue;
    if (make_plan_tag(existing.candidate->shape, existing.candidate->scheme) ==
            tag &&
        existing.candidate->scheme == cand.scheme &&
        build_level_plans(existing.candidate->shape) == plans &&
        first_dim_dispatch_signature(*existing.candidate, context) ==
            dispatch) {
      return;
    }
  }
  RunResult r;
  r.row_id = "candidate-" + std::to_string(rows.size());
  r.label = label;
  if (std::any_of(rows.begin(), rows.end(), [&](const RunResult &existing) { return existing.label == r.label; }))
    r.label += "-variant-" + std::to_string(rows.size());
  r.description = describe_candidate(cand);
  r.is_generic = true;
  r.is_h1 = cand.shape.bundle_arity == 1;
  r.is_mandatory = mandatory;
  r.candidate = cand;
  rows.push_back(std::move(r));
}

size_t count_h1(const std::vector<RunResult> &rows) {
  size_t total = 0;
  for (const RunResult &r : rows) {
    if (r.is_generic && r.is_h1 && r.eligible && !r.samples.empty()) ++total;
  }
  return total;
}

}  // namespace

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

  const size_t entry_bits = env_size("SUPERRECORD_ENTRY_BITS", 256);
  const size_t default_leaf_pow = env_size("SUPERRECORD_LEAF_POW", 22);
  const size_t L = bench_tree_height(default_leaf_pow);
  const size_t N = checked_pow2(L, "benchmark leaves");
  const size_t kWarmups = bench_warmups.value_or(3);
  const size_t kTrials = bench_measured_trials.value_or(16);
  const uint64_t trial_seed = bench_trial_seed.value_or(0x747265654733325fULL);
  const uint64_t execution_seed =
      env_size("SUPERRECORD_EXECUTION_SEED", 0x535265636f726431ULL);
  const bool plan_only = env_bool("SUPERRECORD_PLAN_ONLY");
  const BenchmarkTrialPlan plan =
      make_benchmark_trial_plan(N, kWarmups, kTrials, trial_seed);
  const size_t max_frontier = env_size("SUPERRECORD_MAX_FRONTIER", 2);
  const size_t h1_frontier = env_size("SUPERRECORD_H1_FRONTIER", 2);
  const bool include_legacy =
      entry_bits == 256 && std::getenv("SUPERRECORD_SKIP_LEGACY") == nullptr;

  PlannerPolicy policy;
  policy.max_response_ciphertexts =
      env_size("SUPERRECORD_MAX_RESPONSES", 1);
  policy.max_bundle_arity = env_size("SUPERRECORD_MAX_ARITY", 8);
  policy.max_retrieved_bundle_bits_per_level = env_size(
      "SUPERRECORD_MAX_RETRIEVED_BUNDLE_BITS_PER_LEVEL",
      std::numeric_limits<size_t>::max());
  policy.max_validation_resident_bytes =
      env_size2("SUPERRECORD_MEMORY_BYTES",
                "SUPERRECORD_MAX_VALIDATION_RESIDENT_BYTES",
                std::numeric_limits<size_t>::max());
  policy.max_steady_state_bytes =
      env_size2("SUPERRECORD_MEMORY_BYTES", "SUPERRECORD_MAX_STEADY_STATE_BYTES",
                std::numeric_limits<size_t>::max());
  policy.min_noise_margin = env_size("SUPERRECORD_MIN_NOISE_MARGIN", 0);
  policy.expansion_heights = env_size_list("SUPERRECORD_EXPANSION_HEIGHTS");
  if (policy.expansion_heights.empty()) {
    policy.expansion_heights = default_expansion_heights(log_n);
  }
  policy.alpha_alignments = env_size_list("SUPERRECORD_ALPHA_ALIGNMENTS");
  if (policy.alpha_alignments.empty()) policy.alpha_alignments = {1, 16};
  policy.eval_key_registration = env_key_mode();
  policy.fixed_session_key_height =
      env_size("SUPERRECORD_FIXED_KEY_HEIGHT", log_n);
  const std::vector<size_t> bundle_arities = benchmark_bundle_arities(policy);

  const PlannerInput input = make_input(scheme, entry_bits, N, policy);
  std::vector<Candidate> all = enumerate_candidates(input);
  filter_candidates_by_bundle_arity(all, bundle_arities);
  const size_t enumerated = all.size();
  const size_t h1_enumerated =
      static_cast<size_t>(std::count_if(all.begin(), all.end(),
                                        [](const Candidate &c) {
                                          return c.shape.bundle_arity == 1;
                                        }));

  const ParetoContext context{detected_matmul32_availability()};
  CostCalibration calibration;
  PirClient client(scheme);
  std::optional<Candidate> calibration_control;
  TreeServerTiming calibration_timing{};
  if (!plan_only && !all.empty()) {
    const size_t pilot_leaves = std::min<size_t>(N, 4096);
    auto pilots = enumerate_candidates(make_input(scheme, entry_bits, pilot_leaves, policy));
    filter_candidates_by_bundle_arity(pilots, bundle_arities);
    rank_by_prediction(pilots, calibration, context);
    auto pilot_it = std::find_if(pilots.begin(), pilots.end(), [](const Candidate &candidate) {
      return candidate.shape.bundle_arity == 1 && candidate.shape.beta_bits >= 2;
    });
    if (pilot_it == pilots.end()) pilot_it = pilots.begin();
    require_test(pilot_it != pilots.end(), "no feasible calibration control");
    calibration_control = *pilot_it;
    const TreeParams pilot = freeze_candidate(*pilot_it);
    PirServer pilot_server(query_expansion_params(pilot, scheme));
    const auto pilot_session = create_tagged_tree_session_keys(client, pilot, scheme);
    set_tree_client_session_keys(pilot_server, pilot, scheme, pilot_session);
    const EntrySource pilot_source = [&](size_t level, size_t node) { return benchmark_entry(entry_bits, level, node); };
    const TargetNoiseCase fixture{pilot_leaves - 1, pilot_source, "calibration-control"};
    const auto noise = run_target_shape_noise_gate(client, pilot_server, pilot_session,
        pilot, scheme, std::span<const TargetNoiseCase>(&fixture, 1), policy.min_noise_margin);
    require_target_noise_gate(noise, pilot, client.get_client_id());
    const auto db = preprocess_m32(pilot, scheme, noise, pilot_source, false);
    const auto query = make_query(client, pilot_session, scheme, pilot, pilot_leaves - 1);
    (void)serve_request_m32(query, db, pilot_server, pilot_session, pilot, scheme, noise);
    const auto response = serve_request_m32(query, db, pilot_server, pilot_session,
        pilot, scheme, noise, &calibration_timing);
    const auto decoded = extract_path_measured(response, client, pilot, scheme, pilot_leaves - 1);
    for (size_t level = 0; level < pilot.shape.path_levels; ++level)
      require_test(decoded.entries[level] == pilot_source(level, (pilot_leaves - 1) >> (pilot.shape.tree_height - level)),
                   "calibration control decrypt mismatch");
    apply_measured_control_timing(calibration, *pilot_it, calibration_timing);
    calibrate_alpha_operations(calibration, client, scheme);
    BENCH_PRINT("calibrating first-dimension kernels for " << all.size() << " candidate shapes");
    calibration = calibrate_cost_model(all, calibration, context);
  }
  rank_by_prediction(all, calibration, context);
  std::vector<Candidate> frontier = pareto_frontier(all, policy, context);
  rank_by_prediction(frontier, calibration, context);

  std::vector<RunResult> rows;
  auto mandatory_label = [&](const Candidate &cand) {
    std::string label = "mandatory-control";
    if (entry_bits == 256 && cand.shape.bundle_arity == 3 &&
        cand.shape.record_coeffs == 64 && cand.shape.plane_count == 1) {
      label = "bundle3";
    } else if (!bundle_arities.empty() && cand.shape.bundle_arity == 3) {
      label = "generic-h3-mandatory";
    } else if (cand.shape.bundle_arity == 1) {
      label = "generic-h1-mandatory-" + std::to_string(rows.size());
    } else if (!all.empty() &&
               cand.metrics.optimized_steady_state_bytes_estimate ==
                   std::min_element(
                       all.begin(), all.end(),
                       [](const Candidate &a, const Candidate &b) {
                         return a.metrics.optimized_steady_state_bytes_estimate <
                                b.metrics.optimized_steady_state_bytes_estimate;
                       })
                       ->metrics.optimized_steady_state_bytes_estimate) {
      label = "minimum-optimized-steady-state";
    } else {
      label = "minimum-response-count";
    }
    return label;
  };
  if (bundle_arities.empty()) {
    for (const Candidate &cand :
         mandatory_controls(all, input, calibration, context)) {
      add_candidate(rows, cand, mandatory_label(cand), true, context);
    }
  } else {
    for (size_t arity : bundle_arities) {
      auto best = std::find_if(all.begin(), all.end(), [&](const Candidate &cand) {
        return cand.shape.bundle_arity == arity;
      });
      if (best != all.end()) {
        add_candidate(rows, *best, mandatory_label(*best), true, context);
      }
    }
  }
  size_t extra_h1 = 0;
  for (const Candidate &cand : frontier) {
    if (extra_h1 >= h1_frontier) break;
    if (cand.shape.bundle_arity != 1) continue;
    const size_t before = rows.size();
    add_candidate(rows, cand,
                  "generic-h1-frontier-" + std::to_string(extra_h1 + 1),
                  false, context);
    if (rows.size() != before) ++extra_h1;
  }
  size_t extra = 0;
  for (const Candidate &cand : frontier) {
    if (extra >= max_frontier) break;
    const size_t before = rows.size();
    add_candidate(rows, cand, "frontier-" + std::to_string(extra + 1), false,
                  context);
    if (rows.size() != before) ++extra;
  }

  if (include_legacy) {
    RunResult legacy;
    legacy.row_id = "candidate-" + std::to_string(rows.size());
    legacy.label = "legacy-g32";
    legacy.is_mandatory = true;
    rows.push_back(std::move(legacy));
  }

  std::vector<size_t> execution_order(rows.size());
  std::iota(execution_order.begin(), execution_order.end(), 0);
  std::mt19937_64 rng(execution_seed);
  std::shuffle(execution_order.begin(), execution_order.end(), rng);
  const std::optional<size_t> candidate_index =
      env_optional_size("SUPERRECORD_CANDIDATE_INDEX");
  if (candidate_index) {
    if (*candidate_index >= execution_order.size()) {
      throw std::invalid_argument(
          "SUPERRECORD_CANDIDATE_INDEX is outside candidate_execution_order");
    }
    execution_order = {execution_order[*candidate_index]};
  }

  BENCH_PRINT("planner: E=" << entry_bits << " c=" << c << " N=2^" << L
              << ": " << all.size() << " candidates (" << h1_enumerated
              << " h=1), Pareto frontier " << frontier.size()
              << ", benchmark rows " << rows.size()
              << ", execution seed " << execution_seed);

  const EntrySource source = [&](size_t level, size_t node) {
    return benchmark_entry(entry_bits, level, node);
  };

  std::optional<TreePirParams> legacy_plan;
  size_t legacy_resident_estimate = 0;
  if (include_legacy) {
    const size_t legacy_g = std::bit_ceil(ceil_div(size_t{256}, c));
    for (size_t a = 20;; --a) {
      try {
        legacy_plan = make_tree_pir_params_for_scheme(L, a, legacy_g, scheme, scheme.get_expan_height());
        break;
      } catch (const std::invalid_argument &) { if (a == 2) throw; }
    }
    for (const auto &level : ::build_level_plans(*legacy_plan))
      legacy_resident_estimate = checked_add(legacy_resident_estimate,
          checked_mul(level.R, n * 16, "legacy canonical and m32 bytes"), "legacy resident bytes");
    // Database payload dominates; include a query working-set allowance.
    legacy_resident_estimate = checked_add(legacy_resident_estimate, 128 * 1024 * 1024, "legacy working estimate");
  }
  const auto resident = [&](size_t index) {
    return rows[index].candidate ? rows[index].candidate->metrics.optimized_steady_state_bytes_estimate
                                 : legacy_resident_estimate;
  };
  const auto canonical_level_peak = [&](size_t index) {
    size_t peak = 0;
    if (rows[index].candidate) {
      const auto &cand = *rows[index].candidate;
      for (const auto &level : build_level_plans(cand.shape))
        peak = std::max(peak, checked_mul(level.padded_plaintexts_per_plane,
            checked_mul(cand.shape.plane_count, n * 8, "canonical level bytes"), "canonical level bytes"));
    }
    return peak;
  };
  const size_t resident_limit = policy.max_steady_state_bytes;
  std::vector<size_t> included;
  size_t resident_sum = 0, temporary_peak = 0;
  for (size_t index : execution_order) {
    if (!rows[index].is_mandatory) continue;
    included.push_back(index);
    resident_sum = checked_add(resident_sum, resident(index), "blocked resident estimate");
    temporary_peak = std::max(temporary_peak, canonical_level_peak(index));
  }
  require_test(checked_add(resident_sum, temporary_peak, "blocked peak estimate") <= resident_limit,
               "mandatory controls exceed the co-resident benchmark memory budget; use a smaller leaf count");
  auto optional_order = execution_order;
  std::stable_sort(optional_order.begin(), optional_order.end(), [&](size_t a, size_t b) {
    const double a_ms = rows[a].candidate ? rows[a].candidate->metrics.predicted_server_ms : 0;
    const double b_ms = rows[b].candidate ? rows[b].candidate->metrics.predicted_server_ms : 0;
    return a_ms < b_ms;
  });
  for (size_t index : optional_order) {
    if (rows[index].is_mandatory) continue;
    const size_t next_sum = checked_add(resident_sum, resident(index), "blocked resident estimate");
    const size_t next_peak = std::max(temporary_peak, canonical_level_peak(index));
    if (checked_add(next_sum, next_peak, "blocked peak estimate") <= resident_limit) {
      included.push_back(index); resident_sum = next_sum; temporary_peak = next_peak;
    } else rows[index].reason = "excluded by co-resident benchmark memory budget";
  }
  execution_order = included;
  std::vector<std::vector<size_t>> block_orders;
  std::vector<std::function<Sample(size_t)>> runners(rows.size());
  if (!plan_only) {
    for (size_t index : execution_order) {
      RunResult &r = rows[index];
      BENCH_PRINT("preparing [" << r.label << "] " << r.description);
      if (!r.is_generic) {
        const TreePirParams tree = *legacy_plan;
        const TreeNodeChunkSource chunk_source = [t](size_t level, size_t node, size_t chunk) {
          return synthetic_tree_node_bytes_chunk(level, node, chunk, t);
        };
        const size_t required = std::max(tree.h_q, tree.r);
        const size_t registered = policy.eval_key_registration == EvalKeyRegistrationPolicy::FixedMaximumHeight
            ? policy.fixed_session_key_height : required;
        const auto session_start = Clock::now();
        const auto keys = client.create_session_keys(registered);
        require_bv_key_coverage(keys->bv_galois_keys, scheme.scheme_identity(), required, registered);
        const auto coverage = inspect_bv_key_coverage(keys->bv_galois_keys, scheme.scheme_identity(), registered);
        r.key_coverage = {required, registered, coverage.contiguous_height, coverage.key_count,
            coverage.missing_required.size(), coverage.duplicates.size(), coverage.unexpected.size(),
            coverage.malformed_row_shapes.size(), true};
        auto server = std::make_shared<PirServer>(tree_query_expansion_params(tree, scheme));
        server->set_client_session_keys(client.get_client_id(), keys);
        r.session_setup_ms = ms_since(session_start);
        const auto setup_start = Clock::now();
        auto db = std::make_shared<::PreprocessedTree>(preprocess_tree_mvp(tree, chunk_source, scheme));
        r.setup_ms = ms_since(setup_start);
        for (const auto &level : db->canonical) r.plaintexts += level.plaintexts.size();
        r.db_bytes = r.plaintexts * n * 16;
        r.scan_bytes = r.plaintexts * n * 8;
        r.query_bytes = scheme.get_BFV_size(true);
        r.description = "E=256 g=" + std::to_string(tree.g) + " N0=" + std::to_string(tree.N0) +
            " b=" + std::to_string(tree.b) + " rho=" + std::to_string(tree.rho) + " h_q=" + std::to_string(tree.h_q);
        runners[index] = [&, tree, chunk_source, server, db](size_t leaf) {
          CLEAN_TIMER();
          Sample sample; sample.leaf = leaf;
          auto start = Clock::now();
          RlweCt query = make_tree_query(client, scheme, tree, leaf);
          sample.client_query_ms = ms_since(start);
          start = Clock::now();
          TreeQueryUnpackTiming unpack_timing{};
          auto expanded = unpack_tree_query(*server, scheme, tree, client.get_client_id(), query, &unpack_timing);
          sample.stages_ms[0] = stage_ms(unpack_timing.query_expand);
          sample.stages_ms[1] = stage_ms(unpack_timing.selector_completion);
          auto response = answer_path_mvp(*db, expanded, *server, client.get_client_id(), tree, scheme);
          sample.server_total_ms = ms_since(start);
          END_EXPERIMENT();
          start = Clock::now();
          const auto pt = client.decrypt_mod_q_measured(response.chunks[0], sample.max_noise, sample.min_budget_bits);
          sample.client_decode_ms = ms_since(start);
          for (size_t level = 0; level <= tree.L; ++level)
            for (size_t j = 0; j < tree.g; ++j)
              require_test(pt.data[response.level_offsets[level] + j * tree.rho] ==
                               chunk_source(level, leaf >> (tree.L - level), j), "legacy path chunk mismatch");
          if (policy.min_noise_margin > 0)
            require_test(sample.min_budget_bits >= static_cast<int>(policy.min_noise_margin), "legacy noise margin failed");
          const std::array<const char *, 7> names = {TREE_PYRAMID_TIME, TREE_SCAN_TIME, TREE_FOLD_TIME,
              TREE_ROTATE_TIME, TREE_PROJECT_TIME, TREE_PACK_TIME, TREE_SWITCH_TIME};
          const std::array<size_t, 7> slots = {2,3,5,6,7,8,9};
          for (size_t j=0; j<names.size(); ++j) sample.stages_ms[slots[j]] = GET_LAST_TIME(names[j]);
          start = Clock::now();
          std::stringstream wire;
          sample.response_payload_bytes = server->save_resp_to_stream(response.chunks[0], wire);
          sample.response_serialize_ms = ms_since(start);
          return sample;
        };
        r.eligible = true;
        continue;
      }
      const Candidate &cand = *r.candidate;
      const TreeParams params = freeze_candidate(cand);
      const auto session_start = Clock::now();
      auto server_ptr = std::make_shared<PirServer>(query_expansion_params(params, scheme));
      PirServer &server = *server_ptr;
      const auto session = create_tagged_tree_session_keys(client, params, scheme);
      set_tree_client_session_keys(server, params, scheme, session);
      r.key_coverage = inspect_keys(params, session);
      r.session_setup_ms = ms_since(session_start);
      if (!r.key_coverage.matches) { r.reason = "actual key coverage mismatch"; continue; }
        std::set<size_t> boundary_leaves{0, N - 1, plan.measured_leaf_indices.front()};
        for (size_t boundary : {params.shape.bundle_arity,
             checked_mul(params.shape.bundle_arity, params.shape.records_per_plaintext, "noise boundary")}) {
          if (boundary < N) boundary_leaves.insert(boundary);
          if (boundary > 0 && boundary - 1 < N) boundary_leaves.insert(boundary - 1);
        }
        const std::vector<size_t> gate_leaves(boundary_leaves.begin(), boundary_leaves.end());
        std::vector<TargetNoiseCase> noise_cases;
        const CodecSpec fixture_codec = codec_spec_for(params);
        // Cover every boundary class and every data pattern without their Cartesian product.
        for (size_t index = 0; index < std::max<size_t>(5, gate_leaves.size()); ++index) {
          const size_t pattern = index % 5;
          const NoiseEntrySource fixture_source = [=](size_t level, size_t node) {
            if (pattern == 1) return benchmark_entry(entry_bits, level, node);
            return tree_superrecord_benchmark_data::patterned_entry(
                fixture_codec, node, pattern);
          };
          noise_cases.push_back({gate_leaves[index % gate_leaves.size()], fixture_source,
                                 "leaf-" + std::to_string(gate_leaves[index % gate_leaves.size()]) + "-pattern-" + std::to_string(pattern)});
        }
        const auto noise_start = Clock::now();
        const CandidateNoiseResult noise_result = run_target_shape_noise_gate(
            client, server, session, params, scheme,
            std::span<const TargetNoiseCase>(noise_cases.data(),
                                             noise_cases.size()),
            static_cast<double>(policy.min_noise_margin));
        r.noise_gate_ms = ms_since(noise_start);
        r.noise_result = noise_result;
        if (!noise_result_unlocks_candidate(noise_result, params,
                                            client.get_client_id())) {
          r.reason = "target-shape noise gate failed";
          continue;
        }

      const auto setup_start = Clock::now();
      auto db = std::make_shared<tree_superrecord::PreprocessedTree>(
          preprocess_m32(params, scheme, noise_result, source, false));
      r.setup_ms = ms_since(setup_start);
      r.db_bytes = m32_database_bytes(*db);
      r.scan_bytes = cand.metrics.physical_scan_bytes;
      r.plaintexts = cand.metrics.physical_matrix_cells;
      r.query_bytes = scheme.get_BFV_size(true);
      runners[index] = [&, params, session, noise_result, server_ptr, db, source,
                        candidate_label = r.label](size_t leaf) {
        Sample sample; sample.leaf = leaf;
        auto start = Clock::now();
        auto query = make_query(client, session, scheme, params, leaf);
        sample.client_query_ms = ms_since(start);
        TreeServerTiming timing{};
        auto response = serve_request_m32(query, *db, *server_ptr, session, params, scheme, noise_result, &timing);
        sample.server_total_ms = stage_ms(timing.total);
        start = Clock::now();
        const auto decoded = extract_path_measured(response, client, params, scheme, leaf);
        sample.client_decode_ms = ms_since(start);
        require_test(decoded.entries.size() == params.shape.path_levels, "generic path length mismatch");
        for (size_t level=0; level<params.shape.path_levels; ++level)
          require_test(decoded.entries[level] == source(level, leaf >> (params.shape.tree_height-level)),
                       "generic path entry mismatch");
        sample.min_budget_bits = decoded.min_budget_bits; sample.max_noise = decoded.max_noise;
        if (policy.min_noise_margin > 0 &&
            sample.min_budget_bits < static_cast<int>(policy.min_noise_margin)) {
          std::ostringstream message;
          message << "measured noise margin failed: candidate=" << candidate_label
                  << " leaf=" << leaf
                  << " h=" << params.shape.bundle_arity
                  << " G=" << params.shape.record_coeffs
                  << " P=" << params.shape.plane_count
                  << " N0=" << params.shape.N0
                  << " b=" << params.shape.beta_bits
                  << " h_q=" << params.shape.expansion_height
                  << " min_budget_bits=" << sample.min_budget_bits
                  << " required_budget_bits=" << policy.min_noise_margin
                  << " max_residual=" << sample.max_noise
                  << " decoder_bound="
                  << static_cast<double>(scheme.get_small_q()) /
                         (2.0 * static_cast<double>(scheme.get_plain_mod()))
                  << " selected_path_correct=true";
          throw std::runtime_error(message.str());
        }
        const std::array<TreeStageDuration,10> stages = {timing.query_expand,timing.selector_completion,
            timing.alpha_prepare,timing.alpha_scan,timing.first_dim_finalize,timing.beta_fold,
            timing.private_rotate,timing.projection,timing.path_pack,timing.modulus_switch};
        for (size_t j=0;j<stages.size();++j) sample.stages_ms[j] = stage_ms(stages[j]);
        start = Clock::now();
        sample.response_payload_bytes = serialized_response_payload_bytes(response,*server_ptr,params).value_or(0);
        sample.response_serialize_ms = ms_since(start);
        for (size_t level=0;level<params.shape.path_levels;++level)
          sample.real_non_target_neighbour_slots += real_bundle_slots(leaf,level,params)-1;
        return sample;
      };
      r.eligible = true;
    }
    // All preparation and target gates finish before the shared timed blocks.
    std::vector<size_t> active;
    for (size_t index : execution_order) if (rows[index].eligible) active.push_back(index);
    for (size_t block=0; block<kWarmups+kTrials; ++block) {
      const bool measured = block >= kWarmups;
      const size_t leaf = measured ? plan.measured_leaf_indices[block-kWarmups] : plan.warmup_leaf_indices[block];
      std::shuffle(active.begin(),active.end(),rng);
      block_orders.push_back(active);
      for (size_t index : active) {
        Sample sample;
        try {
          sample = runners[index](leaf);
        } catch (const std::exception &error) {
          throw std::runtime_error(
              "generic block failure: phase=" +
              std::string(measured ? "measured" : "warmup") +
              " block=" + std::to_string(block) +
              " row_id=" + rows[index].row_id + ": " + error.what());
        }
        if (measured) {
          sample.block_id = block-kWarmups;
          rows[index].samples.push_back(std::move(sample));
        }
      }
      BENCH_PRINT("completed " << (measured ? "measured" : "warmup") << " block " << block << " leaf " << leaf);
    }
    for (RunResult &r : rows) {
      if (!r.eligible) continue;
      require_test(r.samples.size()==kTrials,"measured sample count mismatch");
      r.min_budget = std::numeric_limits<int>::max();
      for (const auto &sample : r.samples) {
        r.query_ms_avg += sample.client_query_ms; r.extract_ms_avg += sample.client_decode_ms;
        r.neighbour_slots_avg += sample.real_non_target_neighbour_slots;
        r.min_budget = std::min(r.min_budget,sample.min_budget_bits);
        r.max_noise = std::max(r.max_noise,sample.max_noise);
        r.response_bytes = sample.response_payload_bytes;
      }
      r.query_ms_avg /= kTrials; r.extract_ms_avg /= kTrials; r.neighbour_slots_avg /= kTrials;
    }
  }

  const PeakRss peak_rss = read_peak_rss();
  const std::string json_path =
      env_string2("SUPERRECORD_OUTPUT_JSON", "OUTPUT_JSON", "");
  if (!json_path.empty()) {
    write_run_json(json_path, entry_bits, L, N, kWarmups, kTrials, trial_seed,
                   execution_seed, plan_only, enumerated, h1_enumerated,
                   count_h1(rows), bundle_arities, policy, calibration, calibration_control, calibration_timing,
                   plan.warmup_leaf_indices, plan.measured_leaf_indices,
                   execution_order, block_orders, resident_sum + temporary_peak, rows, peak_rss);
    BENCH_PRINT("generic super-record JSON: " << json_path);
  }

  BENCH_PRINT("");
  BENCH_PRINT("generic super-record benchmark: N=2^" << L << " leaves, E="
              << entry_bits << " bits, " << kTrials
              << " measured trials after " << kWarmups
              << " warmup, trial seed " << trial_seed
              << (plan_only ? " (plan-only)" : ""));
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
                  << (r.reason.empty() ? std::string(plan_only ? "plan-only" :
                                                               "not executed")
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
                                          std::to_string(legacy_mean / s.mean) + "x"
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
