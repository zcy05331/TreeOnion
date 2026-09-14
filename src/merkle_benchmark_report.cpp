#include "merkle_benchmark_internal.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace {

std::string json_escape(const std::string &value) {
  std::ostringstream escaped;
  for (unsigned char character : value) {
    switch (character) {
      case '"': escaped << "\\\""; break;
      case '\\': escaped << "\\\\"; break;
      case '\b': escaped << "\\b"; break;
      case '\f': escaped << "\\f"; break;
      case '\n': escaped << "\\n"; break;
      case '\r': escaped << "\\r"; break;
      case '\t': escaped << "\\t"; break;
      default:
        if (character < 0x20) {
          escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                  << static_cast<unsigned int>(character) << std::dec;
        } else {
          escaped << character;
        }
    }
  }
  return escaped.str();
}

void write_string(std::ostringstream &out, const std::string &value) {
  out << '"' << json_escape(value) << '"';
}

void write_string_array(std::ostringstream &out,
                        const std::vector<std::string> &values) {
  out << '[';
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ", ";
    write_string(out, values[i]);
  }
  out << ']';
}

void write_integer_array(std::ostringstream &out,
                         const std::vector<uint64_t> &values) {
  out << '[';
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ", ";
    out << values[i];
  }
  out << ']';
}

void write_double_array(std::ostringstream &out,
                        const std::vector<double> &values) {
  out << '[';
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ", ";
    out << values[i];
  }
  out << ']';
}

void write_pipeline_profile_samples(
    std::ostringstream &out,
    const std::vector<PipelineProfileSample> &samples) {
  out << '[';
  for (size_t i = 0; i < samples.size(); ++i) {
    if (i != 0) out << ", ";
    out << "{\"leaf\": " << samples[i].leaf << ", \"stages\": {";
    bool first_stage = true;
    for (const auto &[stage, ms] : samples[i].stages) {
      out << (first_stage ? "" : ", ") << '"' << stage << "\": " << ms;
      first_stage = false;
    }
    out << "}}";
  }
  out << ']';
}

void write_optional_double(std::ostringstream &out,
                           const std::optional<double> &value) {
  if (value.has_value()) {
    out << *value;
  } else {
    out << "null";
  }
}

void write_metric_statistics(
    std::ostringstream &out,
    const std::optional<MetricSampleStatistics> &statistics) {
  if (!statistics.has_value()) {
    out << "null";
    return;
  }
  out << "{\"count\": " << statistics->count
      << ", \"mean\": " << statistics->mean
      << ", \"population_variance\": "
      << statistics->population_variance
      << ", \"sample_variance\": ";
  write_optional_double(out, statistics->sample_variance);
  out << ", \"population_standard_deviation\": "
      << statistics->population_standard_deviation
      << ", \"sample_standard_deviation\": ";
  write_optional_double(out, statistics->sample_standard_deviation);
  out << '}';
}

}  // namespace

BenchmarkDuration TrialTiming::local_online_pipeline() const {
  BenchmarkDuration total = client_query;
  if (BenchmarkDuration::max() - total < server_compute) {
    throw std::overflow_error("local online timing overflows");
  }
  total += server_compute;
  if (BenchmarkDuration::max() - total < response_serialize) {
    throw std::overflow_error("local online timing overflows");
  }
  total += response_serialize;
  if (BenchmarkDuration::max() - total < response_load_decrypt_extract) {
    throw std::overflow_error("local online timing overflows");
  }
  return total + response_load_decrypt_extract;
}

uint64_t modeled_helper_key_bytes(const PirParams &reference) {
  const uint64_t bv = reference.get_bv_galois_key_size(true);
  const uint64_t gsw = reference.get_gsw_key_size(true);
  return checked_add(bv, gsw, "modeled helper key bytes");
}

CommunicationStats communication_stats(
    const PirParams &reference, size_t pir_call_count,
    const std::vector<size_t> &actual_response_bytes,
    uint64_t plain_response_bytes) {
  if (pir_call_count == 0) {
    throw std::invalid_argument("PIR call count must be positive");
  }
  if (actual_response_bytes.size() != pir_call_count) {
    throw std::invalid_argument(
        "PIR call count does not match actual response count");
  }

  CommunicationStats result;
  result.pir_call_count = pir_call_count;
  result.modeled_query_bytes_per_pir = reference.get_BFV_size(true);
  result.online_query_bytes_modeled = checked_multiply(
      result.pir_call_count, result.modeled_query_bytes_per_pir,
      "modeled online query bytes");
  for (size_t bytes : actual_response_bytes) {
    result.online_response_bytes_actual = checked_add(
        result.online_response_bytes_actual, bytes,
        "actual online response bytes");
  }
  result.online_response_bytes_actual = checked_add(
      result.online_response_bytes_actual, plain_response_bytes,
      "plain online response bytes");
  result.online_total_bytes_mixed = checked_add(
      result.online_query_bytes_modeled, result.online_response_bytes_actual,
      "mixed online bytes");
  result.helper_key_bytes_modeled = modeled_helper_key_bytes(reference);
  result.first_session_total_bytes_mixed = checked_add(
      result.helper_key_bytes_modeled, result.online_total_bytes_mixed,
      "mixed first-session bytes");
  return result;
}

void validate_matching_path_communication(const CommunicationStats &flat,
                                          const CommunicationStats &layerwise) {
  if (flat.pir_call_count != layerwise.pir_call_count ||
      flat.modeled_query_bytes_per_pir !=
          layerwise.modeled_query_bytes_per_pir ||
      flat.online_query_bytes_modeled !=
          layerwise.online_query_bytes_modeled ||
      flat.online_response_bytes_actual !=
          layerwise.online_response_bytes_actual ||
      flat.online_total_bytes_mixed != layerwise.online_total_bytes_mixed ||
      flat.helper_key_bytes_modeled != layerwise.helper_key_bytes_modeled ||
      flat.first_session_total_bytes_mixed !=
          layerwise.first_session_total_bytes_mixed) {
    throw std::invalid_argument(
        "Flat and layerwise paths have different communication totals");
  }
}

void validate_layerwise_path_communication(const CommunicationStats &flat,
                                           const CommunicationStats &layerwise,
                                           size_t direct_levels,
                                           uint64_t direct_plain_bytes) {
  if (direct_levels == 0) {
    validate_matching_path_communication(flat, layerwise);
    return;
  }
  if (flat.pir_call_count <= direct_levels ||
      layerwise.pir_call_count != flat.pir_call_count - direct_levels ||
      flat.modeled_query_bytes_per_pir !=
          layerwise.modeled_query_bytes_per_pir ||
      flat.helper_key_bytes_modeled != layerwise.helper_key_bytes_modeled ||
      flat.online_response_bytes_actual % flat.pir_call_count != 0) {
    throw std::invalid_argument(
        "Layerwise direct-return accounting does not match the flat path");
  }
  const uint64_t per_response =
      flat.online_response_bytes_actual / flat.pir_call_count;
  const uint64_t expected_response =
      per_response * layerwise.pir_call_count + direct_plain_bytes;
  const uint64_t expected_query =
      layerwise.pir_call_count * layerwise.modeled_query_bytes_per_pir;
  if (layerwise.online_response_bytes_actual != expected_response ||
      layerwise.online_query_bytes_modeled != expected_query ||
      layerwise.online_total_bytes_mixed !=
          expected_query + expected_response ||
      layerwise.first_session_total_bytes_mixed !=
          layerwise.helper_key_bytes_modeled +
              layerwise.online_total_bytes_mixed) {
    throw std::invalid_argument(
        "Layerwise direct-return communication totals are inconsistent");
  }
}

MetricSampleStatistics summarize_metric_samples(
    const std::vector<double> &samples) {
  if (samples.empty()) {
    throw std::invalid_argument("Cannot summarize zero metric samples");
  }

  // Welford's online update avoids the cancellation in E[x^2] - E[x]^2.
  long double mean = 0.0L;
  long double squared_deviation_sum = 0.0L;
  uint64_t count = 0;
  for (double sample : samples) {
    if (!std::isfinite(sample)) {
      throw std::invalid_argument("Metric samples must be finite");
    }
    ++count;
    const long double delta = static_cast<long double>(sample) - mean;
    mean += delta / static_cast<long double>(count);
    const long double adjusted_delta =
        static_cast<long double>(sample) - mean;
    squared_deviation_sum += delta * adjusted_delta;
  }

  MetricSampleStatistics result;
  result.count = count;
  result.mean = static_cast<double>(mean);
  result.population_variance = static_cast<double>(
      squared_deviation_sum / static_cast<long double>(count));
  result.population_standard_deviation =
      std::sqrt(result.population_variance);
  if (count > 1) {
    result.sample_variance = static_cast<double>(
        squared_deviation_sum / static_cast<long double>(count - 1));
    result.sample_standard_deviation =
        std::sqrt(*result.sample_variance);
  }
  return result;
}

void finalize_case_statistics(BenchmarkCaseResult &result) {
  if (!result.correctness_passed) {
    throw std::invalid_argument(
        "Cannot publish throughput for a failed correctness case");
  }
  const double server_seconds =
      std::chrono::duration<double>(result.timing.server_compute).count();
  if (!(server_seconds > 0.0) || !std::isfinite(server_seconds)) {
    throw std::invalid_argument(
        "Server compute time must be a finite positive duration");
  }
  constexpr double bytes_per_mebibyte = static_cast<double>(uint64_t{1} << 20);
  // The paper-defined metric uses the plaintext database footprint once. The
  // separate scan diagnostics below may count repeated passes (H for flat) and
  // therefore measure kernel scan efficiency rather than path throughput.
  result.paper_server_throughput_MBps =
      static_cast<double>(result.paper_plaintext_database_bytes) /
      server_seconds / bytes_per_mebibyte;
  result.paper_scan_throughput_MBps =
      static_cast<double>(result.paper_plaintext_scan_bytes) /
      server_seconds / bytes_per_mebibyte;
  result.padded_scan_throughput_MBps =
      static_cast<double>(result.logical_padded_scan_bytes) /
      server_seconds / bytes_per_mebibyte;
  result.useful_response_throughput_Bps =
      static_cast<double>(result.useful_response_bytes) / server_seconds;

  if (!result.server_compute_samples_ms.empty()) {
    result.server_compute_ms_statistics =
        summarize_metric_samples(result.server_compute_samples_ms);
    result.paper_server_throughput_samples_MBps.clear();
    result.paper_server_throughput_samples_MBps.reserve(
        result.server_compute_samples_ms.size());
    for (double server_ms : result.server_compute_samples_ms) {
      if (!(server_ms > 0.0) || !std::isfinite(server_ms)) {
        throw std::invalid_argument(
            "Server compute samples must be finite positive durations");
      }
      result.paper_server_throughput_samples_MBps.push_back(
          static_cast<double>(result.paper_plaintext_database_bytes) /
          (server_ms / 1000.0) / bytes_per_mebibyte);
    }
    result.paper_server_throughput_MBps_statistics =
        summarize_metric_samples(
            result.paper_server_throughput_samples_MBps);
  }
}

std::string benchmark_report_json(const BenchmarkReport &report) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6);
  out << "{\n  \"schema_version\": ";
  write_string(out, report.schema_version);

  const BenchmarkEnvironment &environment = report.environment;
  out << ",\n  \"environment\": {\n"
      << "    \"commit\": ";
  write_string(out, environment.commit);
  out << ",\n    \"branch\": ";
  write_string(out, environment.branch);
  out << ",\n    \"build_type\": ";
  write_string(out, environment.build_type);
  out << ",\n    \"config\": ";
  write_string(out, environment.config);
  out << ",\n    \"architecture\": ";
  write_string(out, environment.architecture);
  out << ",\n    \"process_architecture\": ";
  write_string(out, environment.process_architecture);
  out << ",\n    \"operating_system\": ";
  write_string(out, environment.operating_system);
  out << ",\n    \"cpu\": ";
  write_string(out, environment.cpu);
  out << ",\n    \"compiler\": ";
  write_string(out, environment.compiler);
  out << ",\n    \"cmake_version\": ";
  write_string(out, environment.cmake_version);
  out << ",\n    \"hexl_enabled\": "
      << (environment.hexl_enabled ? "true" : "false")
      << ",\n    \"hexl_version\": ";
  write_string(out, environment.hexl_version);
  out << ",\n    \"hexl_path\": ";
  write_string(out, environment.hexl_path);
  out << ",\n    \"rosetta\": " << (environment.rosetta ? "true" : "false")
      << ",\n    \"non_native_label\": ";
  write_string(out, environment.non_native_label);
  out << "\n  }";

  const PaperAlignment &paper = report.paper_alignment;
  out << ",\n  \"paper_alignment\": {\n    \"paper\": ";
  write_string(out, paper.paper);
  out << ",\n    \"revision\": ";
  write_string(out, paper.revision);
  out << ",\n    \"sections\": ";
  write_string_array(out, paper.sections);
  out << ",\n    \"poly_degree\": " << paper.poly_degree
      << ",\n    \"log_q\": " << paper.log_q
      << ",\n    \"log_t\": " << paper.log_t
      << ",\n    \"log_q_prime\": " << paper.log_q_prime
      << ",\n    \"L_KEY\": " << paper.L_KEY
      << ",\n    \"L_EP\": " << paper.L_EP
      << ",\n    \"L_KS\": " << paper.L_KS
      << ",\n    \"sigma\": " << paper.sigma
      << ",\n    \"estimated_security_bits\": "
      << paper.estimated_security_bits
      << ",\n    \"reference_hardware\": ";
  write_string(out, paper.reference_hardware);
  out << ",\n    \"local_result_is_hardware_replication\": "
      << (paper.local_result_is_hardware_replication ? "true" : "false")
      << "\n  }";

  const BenchmarkWorkload &workload = report.workload;
  out << ",\n  \"workload\": {\n"
      << "    \"leaf_count\": " << workload.leaf_count
      << ",\n    \"tree_height\": " << workload.tree_height
      << ",\n    \"node_bytes\": " << workload.node_bytes
      << ",\n    \"nodes_per_plaintext\": "
      << workload.nodes_per_plaintext
      << ",\n    \"paper_row\": ";
  write_string(out, workload.paper_row);
  out << ",\n    \"warmups\": " << workload.warmups
      << ",\n    \"measured_trials\": " << workload.measured_trials
      << ",\n    \"trial_seed\": " << workload.trial_seed
      << ",\n    \"warmup_leaf_indices\": ";
  write_integer_array(out, workload.warmup_leaf_indices);
  out << ",\n    \"trial_leaf_indices\": ";
  write_integer_array(out, workload.trial_leaf_indices);
  out << ",\n    \"optional_workloads\": [";
  for (size_t i = 0; i < workload.optional_workloads.size(); ++i) {
    if (i != 0) out << ',';
    const OptionalWorkloadResult &optional = workload.optional_workloads[i];
    out << "\n      {\"leaf_count\": " << optional.leaf_count
        << ", \"tree_height\": " << optional.tree_height
        << ", \"paper_row\": ";
    write_string(out, optional.paper_row);
    out << ", \"status\": ";
    write_string(out, optional.status);
    out << ", \"skip_reason\": ";
    write_string(out, optional.skip_reason);
    out << '}';
  }
  if (!workload.optional_workloads.empty()) out << '\n' << "    ";
  out << "]\n  }";

  out << ",\n  \"cases\": [";
  for (size_t i = 0; i < report.cases.size(); ++i) {
    if (i != 0) out << ',';
    const BenchmarkCaseResult &result = report.cases[i];
    const CommunicationStats &communication = result.communication;
    out << "\n    {\n      \"name\": ";
    write_string(out, result.name);
    out << ",\n      \"correctness_passed\": "
        << (result.correctness_passed ? "true" : "false")
        << ",\n      \"pir_call_count\": " << communication.pir_call_count
        << ",\n      \"direct_return_levels\": "
        << result.direct_return_levels
        << ",\n      \"direct_return_response_bytes\": "
        << result.direct_return_response_bytes
        << ",\n      \"raw_dataset_bytes\": " << result.raw_dataset_bytes
        << ",\n      \"paper_plaintext_database_bytes\": "
        << result.paper_plaintext_database_bytes
        << ",\n      \"logical_padded_database_bytes\": "
        << result.logical_padded_database_bytes
        << ",\n      \"paper_plaintext_scan_bytes\": "
        << result.paper_plaintext_scan_bytes
        << ",\n      \"logical_padded_scan_bytes\": "
        << result.logical_padded_scan_bytes
        << ",\n      \"raw_application_scan_bytes\": "
        << result.raw_application_scan_bytes
        << ",\n      \"physical_preprocessed_storage_bytes\": "
        << result.physical_preprocessed_storage_bytes
        << ",\n      \"setup_ms\": " << milliseconds(result.timing.setup)
        << ",\n      \"client_query_ms\": "
        << milliseconds(result.timing.client_query)
        << ",\n      \"server_compute_ms\": "
        << milliseconds(result.timing.server_compute)
        << ",\n      \"sample_leaf_indices\": ";
    write_integer_array(out, result.sample_leaf_indices);
    out << ",\n      \"server_compute_samples_ms\": ";
    write_double_array(out, result.server_compute_samples_ms);
    out << ",\n      \"client_query_samples_ms\": ";
    write_double_array(out, result.client_query_samples_ms);
    out << ",\n      \"response_serialize_samples_ms\": ";
    write_double_array(out, result.response_serialize_samples_ms);
    out << ",\n      \"response_load_decrypt_extract_samples_ms\": ";
    write_double_array(out,
                       result.response_load_decrypt_extract_samples_ms);
    out << ",\n      \"server_compute_ms_statistics\": ";
    write_metric_statistics(out, result.server_compute_ms_statistics);
    out << ",\n      \"server_phase_ms\": {";
    bool first_phase = true;
    for (const auto &[phase, ms] : result.server_phase_ms) {
      out << (first_phase ? "" : ", ") << '"' << phase << "\": " << ms;
      first_phase = false;
    }
    out << '}';
    out << ",\n      \"pipeline_profile_ms\": {";
    bool first_stage = true;
    for (const auto &[stage, ms] : result.pipeline_profile_ms) {
      out << (first_stage ? "" : ", ") << '"' << stage << "\": " << ms;
      first_stage = false;
    }
    out << '}';
    out << ",\n      \"pipeline_profile_samples_ms\": ";
    write_pipeline_profile_samples(out,
                                   result.pipeline_profile_samples_ms);
    out << ",\n      \"layer_layout_policy\": ";
    write_string(out, result.layer_layout_policy);
    out << ",\n      \"layers\": [";
    for (size_t li = 0; li < result.layers.size(); ++li) {
      const LayerLayoutRecord &layer = result.layers[li];
      const LayerLayoutFeatures &f = layer.features;
      out << (li ? ", " : "") << "{\"level\": " << layer.level
          << ", \"direct_return\": " << (layer.direct_return ? "true" : "false")
          << ", \"target_num_pt\": "
          << (f.padded_plaintexts - f.padding_plaintexts)
          << ", \"expansion_height\": " << f.expansion_height
          << ", \"fst_dim_sz\": " << f.first_dim_size
          << ", \"other_dim_sz\": " << f.other_dim_size
          << ", \"num_other_dims\": " << f.other_dim_count
          << ", \"num_pt\": " << f.padded_plaintexts
          << ", \"padding_plaintexts\": " << f.padding_plaintexts
          << ", \"physical_scan_bytes\": " << f.physical_scan_bytes
          << ", \"useful_expanded_ciphertexts\": "
          << f.useful_expanded_ciphertexts
          << ", \"expansion_substitutions\": " << f.expansion_substitutions
          << ", \"inverse_ntts\": " << f.inverse_ntts
          << ", \"cmux_count\": " << f.cmux_count << '}';
    }
    out << ']';
    out << ",\n      \"response_serialize_ms\": "
        << milliseconds(result.timing.response_serialize)
        << ",\n      \"response_load_decrypt_extract_ms\": "
        << milliseconds(result.timing.response_load_decrypt_extract)
        << ",\n      \"local_online_pipeline_ms\": "
        << milliseconds(result.timing.local_online_pipeline())
        << ",\n      \"online_query_bytes_modeled\": "
        << communication.online_query_bytes_modeled
        << ",\n      \"online_response_bytes_actual\": "
        << communication.online_response_bytes_actual
        << ",\n      \"online_total_bytes_mixed\": "
        << communication.online_total_bytes_mixed
        << ",\n      \"helper_key_bytes_modeled\": "
        << communication.helper_key_bytes_modeled
        << ",\n      \"first_session_total_bytes_mixed\": "
        << communication.first_session_total_bytes_mixed
        << ",\n      \"paper_server_throughput_MBps\": "
        << result.paper_server_throughput_MBps
        << ",\n      \"paper_server_throughput_samples_MBps\": ";
    write_double_array(out,
                       result.paper_server_throughput_samples_MBps);
    out << ",\n      \"paper_server_throughput_MBps_statistics\": ";
    write_metric_statistics(
        out, result.paper_server_throughput_MBps_statistics);
    out << ",\n      \"paper_scan_throughput_MBps\": "
        << result.paper_scan_throughput_MBps
        << ",\n      \"padded_scan_throughput_MBps\": "
        << result.padded_scan_throughput_MBps
        << ",\n      \"useful_response_bytes\": "
        << result.useful_response_bytes
        << ",\n      \"useful_response_throughput_Bps\": "
        << result.useful_response_throughput_Bps << "\n    }";
  }
  if (!report.cases.empty()) out << '\n' << "  ";
  out << "]\n}\n";
  return out.str();
}

void write_benchmark_report_json(const BenchmarkReport &report,
                                 const std::string &path) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Failed to open benchmark JSON output: " + path);
  }
  output << benchmark_report_json(report);
  if (!output) {
    throw std::runtime_error("Failed to write benchmark JSON output: " + path);
  }
}

void print_benchmark_report(const BenchmarkReport &report) {
  std::cout << report.environment.non_native_label << '\n';
  std::cout << "case,correct,server_ms,paper_MBps,paper_scan_MBps,"
               "padded_scan_MBps,online_bytes,first_session_bytes\n";
  for (const BenchmarkCaseResult &result : report.cases) {
    std::cout << result.name << ','
              << (result.correctness_passed ? "true" : "false") << ','
              << milliseconds(result.timing.server_compute) << ','
              << result.paper_server_throughput_MBps << ','
              << result.paper_scan_throughput_MBps << ','
              << result.padded_scan_throughput_MBps << ','
              << result.communication.online_total_bytes_mixed << ','
              << result.communication.first_session_total_bytes_mixed
              << '\n';
  }
  std::cout << "case,expand_ms,convert_ms,first_dim_ms,other_dim_ms,"
               "mod_switch_ms\n";
  for (const BenchmarkCaseResult &result : report.cases) {
    const auto phase = [&](const char *name) {
      const auto it = result.server_phase_ms.find(name);
      return it == result.server_phase_ms.end() ? 0.0 : it->second;
    };
    std::cout << result.name << ',' << phase("expand") << ','
              << phase("convert") << ',' << phase("first_dim") << ','
              << phase("other_dim") << ',' << phase("mod_switch") << '\n';
  }
  for (const BenchmarkCaseResult &result : report.cases) {
    if (result.layers.empty()) continue;
    uint64_t padded = 0;
    for (const LayerLayoutRecord &layer : result.layers) {
      padded += layer.features.padded_plaintexts;
    }
    std::cout << result.name << " layout policy " << result.layer_layout_policy
              << ": " << padded << " padded plaintexts, heights";
    for (const LayerLayoutRecord &layer : result.layers) {
      std::cout << ' ' << layer.features.expansion_height;
    }
    std::cout << '\n';
  }
  for (const OptionalWorkloadResult &optional :
       report.workload.optional_workloads) {
    std::cout << "optional " << optional.paper_row << ": " << optional.status;
    if (!optional.skip_reason.empty()) {
      std::cout << " (" << optional.skip_reason << ')';
    }
    std::cout << '\n';
  }
}
