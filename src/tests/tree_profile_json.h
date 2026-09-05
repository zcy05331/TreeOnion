#pragma once

#include "tree_query.h"
#include "tree_select.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace tree_profile_json {

inline constexpr std::array<const char *, 6> kPathTimerSections = {
    TREE_PYRAMID_TIME, TREE_FOLD_TIME,   TREE_ROTATE_TIME,
    TREE_PROJECT_TIME, TREE_PACK_TIME,   TREE_SWITCH_TIME};

inline constexpr std::array<const char *, 6> kPathStageLabels = {
    "pyramid", "fold", "rotate", "project", "pack", "switch"};

inline constexpr std::array<const char *, 7> kLegacyTimerSections = {
    TREE_PYRAMID_TIME, TREE_SCAN_TIME,    TREE_FOLD_TIME, TREE_ROTATE_TIME,
    TREE_PROJECT_TIME, TREE_PACK_TIME,    TREE_SWITCH_TIME};

inline constexpr std::array<const char *, 7> kLegacyStageLabels = {
    "pyramid", "scan", "fold", "rotate", "project", "pack", "switch"};

inline constexpr std::array<const char *, 2> kScanTimerSections = {
    TREE_SCAN_CORE_TIME, TREE_SCAN_FINALIZE_TIME};

inline constexpr std::array<const char *, 2> kScanStageLabels = {
    "scan_core", "scan_finalize"};

struct Sample {
  size_t trial = 0;
  size_t leaf = 0;
  double client_query_ms = 0.0;
  double server_total_ms = 0.0;
  std::map<std::string, double> stages_ms;
  std::map<std::string, double> diagnostics_ms;
  double response_serialize_ms = 0.0;
  double client_decode_ms = 0.0;
  bool correct = false;
};

inline double ns_to_ms(std::chrono::nanoseconds ns) {
  return std::chrono::duration<double, std::milli>(ns).count();
}

inline double query_unpack_ms(const TreeQueryUnpackTiming &timing) {
  return ns_to_ms(timing.query_expand) + ns_to_ms(timing.selector_completion);
}

inline void json_string(std::ostream &out, const std::string &value) {
  out << '"';
  for (char ch : value) {
    switch (ch) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        out << ch;
        break;
    }
  }
  out << '"';
}

inline std::string output_path_from_env() {
  const char *path = std::getenv("BENCHMARK_OUTPUT_JSON");
  return path == nullptr ? std::string() : std::string(path);
}

inline void write_report(const std::string &path, const std::string &scheme,
                         size_t leaf_count, size_t node_entry_bits,
                         size_t actual_logical_bytes, size_t warmups,
                         size_t measured_trials, uint64_t trial_seed,
                         bool non_comparable_32b_payload,
                         const std::string &mode,
                         const std::vector<Sample> &samples,
                         const std::map<std::string, std::string> &metadata) {
  if (path.empty()) return;
  if (samples.size() != measured_trials) {
    throw std::runtime_error(
        "benchmark JSON sample count does not match measured trials");
  }
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("could not open BENCHMARK_OUTPUT_JSON path");
  }
  out << std::setprecision(17);
  out << "{\n  \"scheme\": ";
  json_string(out, scheme);
  out << ",\n  \"leaf_count\": " << leaf_count
      << ",\n  \"node_entry_bits\": " << node_entry_bits
      << ",\n  \"actual_logical_bytes\": " << actual_logical_bytes
      << ",\n  \"warmups\": " << warmups
      << ",\n  \"measured_trials\": " << measured_trials
      << ",\n  \"trial_seed\": " << trial_seed
      << ",\n  \"diagnostic_non_comparable_32B_payload\": "
      << (non_comparable_32b_payload ? "true" : "false")
      << ",\n  \"mode\": ";
  json_string(out, mode);
  out << ",\n  \"stage_definitions\": {"
      << "\n    \"query_expand\": \"ExpandBFV over the packed tree query\","
      << "\n    \"selector_completion\": \"RLWE-star selector rows completed to RGSW\","
      << "\n    \"pyramid\": \"alpha pyramid construction and server-domain preparation\","
      << "\n    \"scan_core\": \"composite m32 first-dimension matrix multiplication core\","
      << "\n    \"scan_finalize\": \"composite m32 CRT reconstruction and inverse NTT finalize\","
      << "\n    \"fold\": \"beta-dimension CMux folding\","
      << "\n    \"rotate\": \"private gamma rotation\","
      << "\n    \"project\": \"stride projection and BV key switching\","
      << "\n    \"pack\": \"public shifts and response accumulation\","
      << "\n    \"switch\": \"same-ring response modulus switch or compressed response ring switch\","
      << "\n    \"response_serialize\": \"actual response wire serialization outside server timing\","
      << "\n    \"client_decode\": \"client decryption and path extraction/decode\","
      << "\n    \"diagnostic_scan_combined\": \"legacy combined first-dimension scan timer; overlaps scan_core plus scan_finalize\","
      << "\n    \"diagnostic_response_ring_switch_key_switch\": \"compressed-response even/odd split plus small-ring gadget key switching; overlaps switch\","
      << "\n    \"diagnostic_response_ring_switch_modswitch\": \"compressed-response final full-q to small-q rescale; overlaps switch\""
      << "\n  },\n  \"metadata\": {";
  bool first = true;
  for (const auto &[key, value] : metadata) {
    if (!first) out << ',';
    first = false;
    out << "\n    ";
    json_string(out, key);
    out << ": ";
    json_string(out, value);
  }
  out << "\n  },\n  \"samples\": [";
  for (size_t i = 0; i < samples.size(); ++i) {
    const Sample &sample = samples[i];
    if (i) out << ',';
    out << "\n    {\"trial\": " << sample.trial
        << ", \"leaf\": " << sample.leaf
        << ", \"client_query_ms\": " << sample.client_query_ms
        << ", \"server_total_ms\": " << sample.server_total_ms
        << ", \"stages_ms\": {";
    first = true;
    for (const auto &[stage, value] : sample.stages_ms) {
      if (!first) out << ", ";
      first = false;
      json_string(out, stage);
      out << ": " << value;
    }
    out << "}, \"diagnostics_ms\": {";
    first = true;
    for (const auto &[diagnostic, value] : sample.diagnostics_ms) {
      if (!first) out << ", ";
      first = false;
      json_string(out, diagnostic);
      out << ": " << value;
    }
    out << "}, \"response_serialize_ms\": " << sample.response_serialize_ms
        << ", \"client_decode_ms\": " << sample.client_decode_ms
        << ", \"correct\": " << (sample.correct ? "true" : "false") << '}';
  }
  out << "\n  ]\n}\n";
}

}  // namespace tree_profile_json
