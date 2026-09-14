#include "layer_layout_planner.h"
#include "merkle_baseline.h"
#include "merkle_benchmark.h"
#include "pir.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct BucketRow {
  size_t height = 0;
  size_t bucket = 0;
  size_t members = 0;
  size_t target_num_pt = 0;
};

struct LayoutRow {
  size_t height = 0;
  std::string variant;
  size_t forced_h = 0;
  size_t buckets = 0;
  size_t min_n0 = 0;
  size_t max_n0 = 0;
  size_t min_remaining_dims = 0;
  size_t max_remaining_dims = 0;
  size_t min_nrest = 0;
  size_t max_nrest = 0;
  uint64_t target_plaintexts = 0;
  uint64_t padded_plaintexts = 0;
  uint64_t padding_plaintexts = 0;
  uint64_t sum_nrest = 0;
  uint64_t cmux_count = 0;
  uint64_t expansion_substitutions = 0;
  uint64_t first_dim_query_ntts = 0;
  uint64_t physical_scan_bytes = 0;
};

std::vector<std::string> split_csv(const std::string& line) {
  std::vector<std::string> cells;
  std::stringstream stream(line);
  std::string cell;
  while (std::getline(stream, cell, ',')) cells.push_back(cell);
  return cells;
}

size_t parse_size(const std::string& value) {
  size_t consumed = 0;
  const unsigned long long parsed = std::stoull(value, &consumed, 10);
  if (consumed != value.size()) {
    throw std::runtime_error("bad integer field: " + value);
  }
  return static_cast<size_t>(parsed);
}

std::vector<BucketRow> read_bucket_rows(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open input CSV");
  std::string line;
  std::getline(input, line);
  std::vector<BucketRow> rows;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    const std::vector<std::string> cells = split_csv(line);
    if (cells.size() < 4) throw std::runtime_error("short CSV row");
    BucketRow row;
    row.height = parse_size(cells[0]);
    row.bucket = parse_size(cells[1]);
    row.members = parse_size(cells[2]);
    row.target_num_pt =
        utils::roundup_div(row.members, static_cast<size_t>(96));
    rows.push_back(row);
  }
  return rows;
}

void add_features(LayoutRow& total, const BucketRow& row,
                  const LayerLayoutFeatures& features) {
  if (total.buckets == 0) {
    total.height = row.height;
    total.min_n0 = total.max_n0 = features.first_dim_size;
    total.min_remaining_dims = total.max_remaining_dims =
        features.other_dim_count;
    total.min_nrest = total.max_nrest = features.other_dim_size;
  }
  ++total.buckets;
  total.min_n0 = std::min(total.min_n0, features.first_dim_size);
  total.max_n0 = std::max(total.max_n0, features.first_dim_size);
  total.min_remaining_dims =
      std::min(total.min_remaining_dims, features.other_dim_count);
  total.max_remaining_dims =
      std::max(total.max_remaining_dims, features.other_dim_count);
  total.min_nrest = std::min(total.min_nrest, features.other_dim_size);
  total.max_nrest = std::max(total.max_nrest, features.other_dim_size);
  total.target_plaintexts += row.target_num_pt;
  total.padded_plaintexts += features.padded_plaintexts;
  total.padding_plaintexts += features.padding_plaintexts;
  total.sum_nrest += features.other_dim_size;
  total.cmux_count += features.cmux_count;
  total.expansion_substitutions += features.expansion_substitutions;
  total.first_dim_query_ntts += features.first_dim_query_ntts;
  total.physical_scan_bytes += features.physical_scan_bytes;
}

LayoutRow summarize_baseline(const std::vector<BucketRow>& rows,
                             const PirParams& reference) {
  LayoutRow total;
  total.variant = "baseline_selector";
  for (const BucketRow& row : rows) {
    const TargetPlaintextLayout selected =
        select_target_plaintext_layout(row.target_num_pt, reference);
    add_features(total, row, compute_layer_layout_features(selected.params));
  }
  return total;
}

LayoutRow summarize_forced(const std::vector<BucketRow>& rows,
                           const PirParams& reference, size_t forced_h) {
  LayoutRow total;
  total.variant = "forced_h" + std::to_string(forced_h);
  total.forced_h = forced_h;
  for (const BucketRow& row : rows) {
    const PirParams params =
        reference.with_layout({row.target_num_pt, forced_h,
                               reference.get_fst_dim_pow2()});
    add_features(total, row, compute_layer_layout_features(params));
  }
  return total;
}

void write_csv(const std::string& path, const std::vector<LayoutRow>& rows) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("failed to write CSV");
  out << "H,variant,forced_h,buckets,N0_min,N0_max,remaining_dims_min,"
         "remaining_dims_max,Nrest_min,Nrest_max,target_plaintexts,"
         "padded_plaintexts,padding_plaintexts,sum_Nrest,cmux_count,"
         "expansion_substitutions,first_dim_query_ntts,physical_scan_bytes,"
         "delta_padding_vs_baseline,delta_sum_Nrest_vs_baseline,"
         "delta_cmux_vs_baseline,delta_expansion_substitutions_vs_baseline,"
         "delta_query_ntts_vs_baseline,delta_physical_bytes_vs_baseline\n";
  std::map<size_t, LayoutRow> baselines;
  for (const LayoutRow& row : rows) {
    if (row.variant == "baseline_selector") baselines[row.height] = row;
  }
  for (const LayoutRow& row : rows) {
    const LayoutRow& base = baselines.at(row.height);
    out << row.height << ',' << row.variant << ',' << row.forced_h << ','
        << row.buckets << ',' << row.min_n0 << ',' << row.max_n0 << ','
        << row.min_remaining_dims << ',' << row.max_remaining_dims << ','
        << row.min_nrest << ',' << row.max_nrest << ','
        << row.target_plaintexts << ',' << row.padded_plaintexts << ','
        << row.padding_plaintexts << ',' << row.sum_nrest << ','
        << row.cmux_count << ',' << row.expansion_substitutions << ','
        << row.first_dim_query_ntts << ',' << row.physical_scan_bytes << ','
        << static_cast<int64_t>(row.padding_plaintexts) -
               static_cast<int64_t>(base.padding_plaintexts)
        << ','
        << static_cast<int64_t>(row.sum_nrest) -
               static_cast<int64_t>(base.sum_nrest)
        << ','
        << static_cast<int64_t>(row.cmux_count) -
               static_cast<int64_t>(base.cmux_count)
        << ','
        << static_cast<int64_t>(row.expansion_substitutions) -
               static_cast<int64_t>(base.expansion_substitutions)
        << ','
        << static_cast<int64_t>(row.first_dim_query_ntts) -
               static_cast<int64_t>(base.first_dim_query_ntts)
        << ','
        << static_cast<int64_t>(row.physical_scan_bytes) -
               static_cast<int64_t>(base.physical_scan_bytes)
        << '\n';
  }
}

void write_json(const std::string& path, const std::vector<LayoutRow>& rows) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("failed to write JSON");
  std::map<size_t, LayoutRow> baselines;
  for (const LayoutRow& row : rows) {
    if (row.variant == "baseline_selector") baselines[row.height] = row;
  }
  out << "{\n  \"schema_version\": \"wider_n0_structure.v1\",\n"
      << "  \"rows\": [\n";
  for (size_t i = 0; i < rows.size(); ++i) {
    const LayoutRow& row = rows[i];
    const LayoutRow& base = baselines.at(row.height);
    if (i) out << ",\n";
    out << "    {\"H\": " << row.height << ", \"variant\": \""
        << row.variant << "\", \"forced_h\": " << row.forced_h
        << ", \"buckets\": " << row.buckets << ", \"N0_min\": "
        << row.min_n0 << ", \"N0_max\": " << row.max_n0
        << ", \"remaining_dims_min\": " << row.min_remaining_dims
        << ", \"remaining_dims_max\": " << row.max_remaining_dims
        << ", \"Nrest_min\": " << row.min_nrest
        << ", \"Nrest_max\": " << row.max_nrest
        << ", \"target_plaintexts\": " << row.target_plaintexts
        << ", \"padded_plaintexts\": " << row.padded_plaintexts
        << ", \"padding_plaintexts\": " << row.padding_plaintexts
        << ", \"sum_Nrest\": " << row.sum_nrest
        << ", \"cmux_count\": " << row.cmux_count
        << ", \"expansion_substitutions\": "
        << row.expansion_substitutions
        << ", \"first_dim_query_ntts\": " << row.first_dim_query_ntts
        << ", \"physical_scan_bytes\": " << row.physical_scan_bytes
        << ", \"delta_vs_baseline\": {\"padding_plaintexts\": "
        << static_cast<int64_t>(row.padding_plaintexts) -
               static_cast<int64_t>(base.padding_plaintexts)
        << ", \"sum_Nrest\": "
        << static_cast<int64_t>(row.sum_nrest) -
               static_cast<int64_t>(base.sum_nrest)
        << ", \"cmux_count\": "
        << static_cast<int64_t>(row.cmux_count) -
               static_cast<int64_t>(base.cmux_count)
        << ", \"expansion_substitutions\": "
        << static_cast<int64_t>(row.expansion_substitutions) -
               static_cast<int64_t>(base.expansion_substitutions)
        << ", \"first_dim_query_ntts\": "
        << static_cast<int64_t>(row.first_dim_query_ntts) -
               static_cast<int64_t>(base.first_dim_query_ntts)
        << ", \"physical_scan_bytes\": "
        << static_cast<int64_t>(row.physical_scan_bytes) -
               static_cast<int64_t>(base.physical_scan_bytes)
        << "}}";
  }
  out << "\n  ]\n}\n";
}

void write_markdown(const std::string& path,
                    const std::vector<LayoutRow>& rows) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("failed to write Markdown");
  std::map<size_t, LayoutRow> baselines;
  for (const LayoutRow& row : rows) {
    if (row.variant == "baseline_selector") baselines[row.height] = row;
  }
  out << "# Wider N0 Structure Check\n\n";
  out << "Input buckets come from real-selector-check.csv. Layouts are "
         "computed by the current linked PirParams::with_layout, "
         "make_benchmark_reference, select_target_plaintext_layout, and "
         "compute_layer_layout_features.\n\n";
  out << "| H | variant | N0 | padded | padding delta | sum Nrest | "
         "sum Nrest delta | CMux delta | substitutions delta | query NTT "
         "delta | physical bytes delta |\n";
  out << "|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
  for (const LayoutRow& row : rows) {
    const LayoutRow& base = baselines.at(row.height);
    out << "| " << row.height << " | " << row.variant << " | "
        << row.min_n0;
    if (row.min_n0 != row.max_n0) out << ".." << row.max_n0;
    out << " | " << row.padded_plaintexts << " | "
        << static_cast<int64_t>(row.padding_plaintexts) -
               static_cast<int64_t>(base.padding_plaintexts)
        << " | " << row.sum_nrest << " | "
        << static_cast<int64_t>(row.sum_nrest) -
               static_cast<int64_t>(base.sum_nrest)
        << " | "
        << static_cast<int64_t>(row.cmux_count) -
               static_cast<int64_t>(base.cmux_count)
        << " | "
        << static_cast<int64_t>(row.expansion_substitutions) -
               static_cast<int64_t>(base.expansion_substitutions)
        << " | "
        << static_cast<int64_t>(row.first_dim_query_ntts) -
               static_cast<int64_t>(base.first_dim_query_ntts)
        << " | "
        << static_cast<int64_t>(row.physical_scan_bytes) -
               static_cast<int64_t>(base.physical_scan_bytes)
        << " |\n";
  }
}

}  // namespace

int main() {
  const std::string base =
      "outputs/superrecord/cloudlab-20260904/parameter-alignment/"
      "wider-n0-structure";
  const std::vector<BucketRow> all_rows =
      read_bucket_rows("outputs/superrecord/cloudlab-20260904/"
                       "parameter-alignment/real-selector-check.csv");
  std::map<size_t, std::vector<BucketRow>> by_height;
  for (const BucketRow& row : all_rows) by_height[row.height].push_back(row);

  std::vector<LayoutRow> summaries;
  for (const auto& [height, rows] : by_height) {
    const size_t leaf_count = size_t{1} << height;
    const PirParams reference =
        make_benchmark_reference(make_benchmark_workload(leaf_count));
    summaries.push_back(summarize_baseline(rows, reference));
    for (size_t forced_h : {size_t{7}, size_t{8}, size_t{9}, size_t{10}}) {
      summaries.push_back(summarize_forced(rows, reference, forced_h));
    }
  }

  write_csv(base + ".csv", summaries);
  write_json(base + ".json", summaries);
  write_markdown(base + ".md", summaries);
  std::cout << "wrote " << base << ".{csv,json,md}\n";
  for (const LayoutRow& row : summaries) {
    std::cout << "H=" << row.height << " " << row.variant
              << " N0=" << row.min_n0;
    if (row.min_n0 != row.max_n0) std::cout << ".." << row.max_n0;
    std::cout << " padded=" << row.padded_plaintexts
              << " padding=" << row.padding_plaintexts
              << " sum_Nrest=" << row.sum_nrest
              << " cmux=" << row.cmux_count
              << " substitutions=" << row.expansion_substitutions
              << " query_ntts=" << row.first_dim_query_ntts
              << " physical_bytes=" << row.physical_scan_bytes << "\n";
  }
}
