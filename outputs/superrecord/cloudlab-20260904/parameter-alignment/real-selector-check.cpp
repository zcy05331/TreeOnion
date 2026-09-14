#include "merkle_baseline.h"
#include "merkle_benchmark.h"
#include "pir.h"
#include "utils.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

struct ExpectedBucket {
  size_t height;
  size_t bucket;
  size_t members;
  size_t target_num_pt;
  size_t old_ref_h;
  size_t old_h;
  size_t old_n0;
  size_t old_nrest;
  size_t old_remaining_dims;
  size_t old_padded;
  size_t new_ref_h;
  size_t new_h;
  size_t new_n0;
  size_t new_nrest;
  size_t new_remaining_dims;
  size_t new_padded;
};

struct LayoutView {
  size_t h;
  size_t n0;
  size_t nrest;
  size_t remaining_dims;
  size_t padded;
};

struct HeightSummary {
  size_t height = 0;
  size_t bucket_count = 0;
  uint64_t total_member_entries = 0;
  size_t min_members = 0;
  size_t max_members = 0;
  size_t min_target_num_pt = 0;
  size_t max_target_num_pt = 0;
  size_t old_ref_h = 0;
  size_t old_ref_n0 = 0;
  size_t old_ref_remaining_dims = 0;
  size_t new_ref_h = 0;
  size_t new_ref_n0 = 0;
  size_t new_ref_remaining_dims = 0;
  size_t changed_buckets = 0;
  size_t old_selected_h = 0;
  size_t old_selected_n0 = 0;
  size_t old_selected_remaining_dims = 0;
  size_t old_min_nrest = 0;
  size_t old_max_nrest = 0;
  size_t old_min_padded = 0;
  size_t old_max_padded = 0;
  size_t new_selected_h = 0;
  size_t new_selected_n0 = 0;
  size_t new_selected_remaining_dims = 0;
  size_t new_min_nrest = 0;
  size_t new_max_nrest = 0;
  size_t new_min_padded = 0;
  size_t new_max_padded = 0;
};

constexpr ExpectedBucket kExpectedBuckets[] = {
    {24, 0, 2650351, 27608, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 1, 2649641, 27601, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 2, 2646541, 27569, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 3, 2644126, 27543, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 4, 2646937, 27573, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 5, 2646454, 27568, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 6, 2647015, 27574, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 7, 2647546, 27579, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 8, 2649648, 27601, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 9, 2645918, 27562, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 10, 2647482, 27578, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 11, 2649012, 27594, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 12, 2649212, 27596, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 13, 2646031, 27563, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 14, 2646805, 27571, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 15, 2647930, 27583, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 16, 2648971, 27594, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 17, 2647295, 27576, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 18, 2649053, 27595, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 19, 2651168, 27617, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 20, 2648160, 27585, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 21, 2646595, 27569, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 22, 2648542, 27589, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 23, 2646239, 27565, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 24, 2648664, 27591, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 25, 2647617, 27580, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 26, 2648453, 27589, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 27, 2647519, 27579, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 28, 2650496, 27610, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 29, 2649969, 27604, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 30, 2646241, 27566, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 31, 2647317, 27577, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 32, 2647826, 27582, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 33, 2648256, 27586, 7, 7, 64, 432, 9, 27648, 10, 7, 64, 432, 9, 27648},
    {24, 34, 2646465, 27568, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 35, 2647625, 27580, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {24, 36, 2647088, 27574, 7, 7, 64, 431, 9, 27584, 10, 7, 64, 431, 9, 27584},
    {27, 0, 18715471, 194953, 7, 7, 32, 6093, 13, 194976, 10, 7, 32, 6093, 13, 194976},
    {27, 1, 18718983, 194990, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 2, 18717940, 194979, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 3, 18718648, 194986, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 4, 18713339, 194931, 7, 7, 32, 6092, 13, 194944, 10, 7, 32, 6092, 13, 194944},
    {27, 5, 18730684, 195112, 7, 7, 32, 6098, 13, 195136, 10, 7, 32, 6098, 13, 195136},
    {27, 6, 18722485, 195026, 7, 7, 32, 6095, 13, 195040, 10, 7, 32, 6095, 13, 195040},
    {27, 7, 18720242, 195003, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 8, 18720105, 195002, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 9, 18721570, 195017, 7, 7, 32, 6095, 13, 195040, 10, 7, 32, 6095, 13, 195040},
    {27, 10, 18717047, 194970, 7, 7, 32, 6093, 13, 194976, 10, 7, 32, 6093, 13, 194976},
    {27, 11, 18718619, 194986, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 12, 18730048, 195105, 7, 7, 32, 6098, 13, 195136, 10, 7, 32, 6098, 13, 195136},
    {27, 13, 18720790, 195009, 7, 7, 32, 6095, 13, 195040, 10, 7, 32, 6095, 13, 195040},
    {27, 14, 18721177, 195013, 7, 7, 32, 6095, 13, 195040, 10, 7, 32, 6095, 13, 195040},
    {27, 15, 18729926, 195104, 7, 7, 32, 6097, 13, 195104, 10, 7, 32, 6097, 13, 195104},
    {27, 16, 18718676, 194987, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 17, 18709252, 194889, 7, 7, 32, 6091, 13, 194912, 10, 7, 32, 6091, 13, 194912},
    {27, 18, 18718382, 194984, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 19, 18722283, 195024, 7, 7, 32, 6095, 13, 195040, 10, 7, 32, 6095, 13, 195040},
    {27, 20, 18723855, 195041, 7, 7, 32, 6096, 13, 195072, 10, 7, 32, 6096, 13, 195072},
    {27, 21, 18722479, 195026, 7, 7, 32, 6095, 13, 195040, 10, 7, 32, 6095, 13, 195040},
    {27, 22, 18718129, 194981, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 23, 18721067, 195012, 7, 7, 32, 6095, 13, 195040, 10, 7, 32, 6095, 13, 195040},
    {27, 24, 18725919, 195062, 7, 7, 32, 6096, 13, 195072, 10, 7, 32, 6096, 13, 195072},
    {27, 25, 18727714, 195081, 7, 7, 32, 6097, 13, 195104, 10, 7, 32, 6097, 13, 195104},
    {27, 26, 18722526, 195027, 7, 7, 32, 6095, 13, 195040, 10, 7, 32, 6095, 13, 195040},
    {27, 27, 18719915, 195000, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 28, 18722219, 195024, 7, 7, 32, 6095, 13, 195040, 10, 7, 32, 6095, 13, 195040},
    {27, 29, 18720181, 195002, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 30, 18712131, 194919, 7, 7, 32, 6092, 13, 194944, 10, 7, 32, 6092, 13, 194944},
    {27, 31, 18720197, 195003, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 32, 18720890, 195010, 7, 7, 32, 6095, 13, 195040, 10, 7, 32, 6095, 13, 195040},
    {27, 33, 18720323, 195004, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 34, 18720160, 195002, 7, 7, 32, 6094, 13, 195008, 10, 7, 32, 6094, 13, 195008},
    {27, 35, 18723090, 195033, 7, 7, 32, 6095, 13, 195040, 10, 7, 32, 6095, 13, 195040},
    {27, 36, 18717137, 194971, 7, 7, 32, 6093, 13, 194976, 10, 7, 32, 6093, 13, 194976},
    {27, 37, 18721679, 195018, 7, 7, 32, 6095, 13, 195040, 10, 7, 32, 6095, 13, 195040},
    {27, 38, 18725186, 195055, 7, 7, 32, 6096, 13, 195072, 10, 7, 32, 6096, 13, 195072},
    {27, 39, 18717491, 194974, 7, 7, 32, 6093, 13, 194976, 10, 7, 32, 6093, 13, 194976},
    {27, 40, 18717141, 194971, 7, 7, 32, 6093, 13, 194976, 10, 7, 32, 6093, 13, 194976},
    {27, 41, 18726747, 195071, 7, 7, 32, 6096, 13, 195072, 10, 7, 32, 6096, 13, 195072},
};
LayoutView view(const PirParams& params) {
  return {params.get_expan_height(), params.get_fst_dim_sz(),
          params.get_other_dim_sz(), params.get_num_other_dims(),
          params.get_num_pt()};
}

bool same_layout(LayoutView left, LayoutView right) {
  return std::tie(left.h, left.n0, left.nrest, left.remaining_dims,
                  left.padded) ==
         std::tie(right.h, right.n0, right.nrest, right.remaining_dims,
                  right.padded);
}

PirParams old_flat_reference(size_t target_num_pt) {
  PirParams scheme;
  for (size_t h = 0; h <= 10; ++h) {
    try {
      return scheme.with_layout({target_num_pt, h, true});
    } catch (const std::runtime_error&) {
    }
  }
  throw std::runtime_error("no old flat reference layout");
}

void write_csv_row(std::ostream& out, const ExpectedBucket& expected,
                   const PirParams& old_reference,
                   const PirParams& new_reference, LayoutView old_layout,
                   LayoutView new_layout, size_t target_num_pt,
                   bool changed) {
  out << expected.height << ',' << expected.bucket << ',' << expected.members
      << ',' << target_num_pt << ','
      << old_reference.get_expan_height() << ','
      << old_layout.h << ',' << old_layout.n0 << ',' << old_layout.nrest
      << ',' << old_layout.remaining_dims << ',' << old_layout.padded << ','
      << new_reference.get_expan_height() << ',' << new_layout.h << ','
      << new_layout.n0 << ',' << new_layout.nrest << ','
      << new_layout.remaining_dims << ',' << new_layout.padded << ','
      << (changed ? "true" : "false") << '\n';
}

void update_summary(HeightSummary& summary, const ExpectedBucket& expected,
                    const PirParams& old_reference,
                    const PirParams& new_reference,
                    size_t target_num_pt,
                    LayoutView old_layout, LayoutView new_layout,
                    bool changed) {
  if (summary.bucket_count == 0) {
    summary.height = expected.height;
    summary.min_members = summary.max_members = expected.members;
    summary.min_target_num_pt = summary.max_target_num_pt = target_num_pt;
    summary.old_ref_h = old_reference.get_expan_height();
    summary.old_ref_n0 = old_reference.get_fst_dim_sz();
    summary.old_ref_remaining_dims = old_reference.get_num_other_dims();
    summary.new_ref_h = new_reference.get_expan_height();
    summary.new_ref_n0 = new_reference.get_fst_dim_sz();
    summary.new_ref_remaining_dims = new_reference.get_num_other_dims();
    summary.old_selected_h = old_layout.h;
    summary.old_selected_n0 = old_layout.n0;
    summary.old_selected_remaining_dims = old_layout.remaining_dims;
    summary.old_min_nrest = summary.old_max_nrest = old_layout.nrest;
    summary.old_min_padded = summary.old_max_padded = old_layout.padded;
    summary.new_selected_h = new_layout.h;
    summary.new_selected_n0 = new_layout.n0;
    summary.new_selected_remaining_dims = new_layout.remaining_dims;
    summary.new_min_nrest = summary.new_max_nrest = new_layout.nrest;
    summary.new_min_padded = summary.new_max_padded = new_layout.padded;
  }
  ++summary.bucket_count;
  summary.total_member_entries += expected.members;
  summary.min_members = std::min(summary.min_members, expected.members);
  summary.max_members = std::max(summary.max_members, expected.members);
  summary.min_target_num_pt =
      std::min(summary.min_target_num_pt, target_num_pt);
  summary.max_target_num_pt =
      std::max(summary.max_target_num_pt, target_num_pt);
  summary.changed_buckets += changed ? 1 : 0;
  summary.old_min_nrest = std::min(summary.old_min_nrest, old_layout.nrest);
  summary.old_max_nrest = std::max(summary.old_max_nrest, old_layout.nrest);
  summary.old_min_padded =
      std::min(summary.old_min_padded, old_layout.padded);
  summary.old_max_padded =
      std::max(summary.old_max_padded, old_layout.padded);
  summary.new_min_nrest = std::min(summary.new_min_nrest, new_layout.nrest);
  summary.new_max_nrest = std::max(summary.new_max_nrest, new_layout.nrest);
  summary.new_min_padded =
      std::min(summary.new_min_padded, new_layout.padded);
  summary.new_max_padded =
      std::max(summary.new_max_padded, new_layout.padded);
}

void write_summary_json(std::ostream& out,
                        const std::vector<HeightSummary>& summaries) {
  out << "{\n  \"schema_version\": \"real_selector_check.v1\",\n";
  out << "  \"bucket_rows_checked\": " << std::size(kExpectedBuckets)
      << ",\n  \"heights\": [\n";
  for (size_t i = 0; i < summaries.size(); ++i) {
    const HeightSummary& s = summaries[i];
    if (i != 0) out << ",\n";
    out << "    {\"H\": " << s.height
        << ", \"bucket_count\": " << s.bucket_count
        << ", \"total_member_entries\": " << s.total_member_entries
        << ", \"min_members\": " << s.min_members
        << ", \"max_members\": " << s.max_members
        << ", \"min_target_num_pt\": " << s.min_target_num_pt
        << ", \"max_target_num_pt\": " << s.max_target_num_pt
        << ", \"old_ref\": {\"h\": " << s.old_ref_h
        << ", \"N0\": " << s.old_ref_n0
        << ", \"remaining_dims\": " << s.old_ref_remaining_dims
        << "}, \"new_ref\": {\"h\": " << s.new_ref_h
        << ", \"N0\": " << s.new_ref_n0
        << ", \"remaining_dims\": " << s.new_ref_remaining_dims
        << "}, \"old_selected\": {\"h\": " << s.old_selected_h
        << ", \"N0\": " << s.old_selected_n0
        << ", \"remaining_dims\": " << s.old_selected_remaining_dims
        << ", \"Nrest_min\": " << s.old_min_nrest
        << ", \"Nrest_max\": " << s.old_max_nrest
        << ", \"padded_min\": " << s.old_min_padded
        << ", \"padded_max\": " << s.old_max_padded
        << "}, \"new_selected\": {\"h\": " << s.new_selected_h
        << ", \"N0\": " << s.new_selected_n0
        << ", \"remaining_dims\": " << s.new_selected_remaining_dims
        << ", \"Nrest_min\": " << s.new_min_nrest
        << ", \"Nrest_max\": " << s.new_max_nrest
        << ", \"padded_min\": " << s.new_min_padded
        << ", \"padded_max\": " << s.new_max_padded
        << "}, \"changed_buckets\": " << s.changed_buckets << "}";
  }
  out << "\n  ]\n}\n";
}

}  // namespace

int main() {
  const std::string base =
      "outputs/superrecord/cloudlab-20260904/parameter-alignment/"
      "real-selector-check";
  std::ofstream csv(base + ".csv", std::ios::binary | std::ios::trunc);
  csv << "H,bucket,members,target,old_ref_h,old_selected_h,old_N0,"
         "old_Nrest,old_remaining_dims,old_padded,new_ref_h,new_selected_h,"
         "new_N0,new_Nrest,new_remaining_dims,new_padded,changed\n";

  std::vector<HeightSummary> summaries;
  for (const ExpectedBucket& expected : kExpectedBuckets) {
    const size_t leaf_count = size_t{1} << expected.height;
    const size_t item_count = 2 * (leaf_count - 1);
    const size_t flat_target_num_pt =
        utils::roundup_div(item_count, size_t{96});
    const PirParams old_reference = old_flat_reference(flat_target_num_pt);
    const PirParams new_reference =
        make_benchmark_reference(make_benchmark_workload(leaf_count));
    const size_t target_num_pt =
        utils::roundup_div(expected.members, size_t{96});
    const TargetPlaintextLayout old_selected =
        select_target_plaintext_layout(target_num_pt, old_reference);
    const TargetPlaintextLayout new_selected =
        select_target_plaintext_layout(target_num_pt, new_reference);

    const LayoutView old_layout = view(old_selected.params);
    const LayoutView new_layout = view(new_selected.params);
    const bool changed = !same_layout(old_layout, new_layout);

    write_csv_row(csv, expected, old_reference, new_reference, old_layout,
                  new_layout, target_num_pt, changed);
    if (summaries.empty() || summaries.back().height != expected.height) {
      summaries.push_back({});
    }
    update_summary(summaries.back(), expected, old_reference, new_reference,
                   target_num_pt, old_layout, new_layout, changed);
  }

  std::ofstream json(base + ".json", std::ios::binary | std::ios::trunc);
  write_summary_json(json, summaries);
  if (!csv || !json) {
    throw std::runtime_error("failed to write selector check artifacts");
  }
  std::cout << "checked " << std::size(kExpectedBuckets)
            << " bucket layouts with real selectors\n";
  for (const HeightSummary& s : summaries) {
    std::cout << "H=" << s.height << " buckets=" << s.bucket_count
              << " changed=" << s.changed_buckets
              << " total_member_entries=" << s.total_member_entries
              << " max_bucket_members=" << s.max_members
              << " max_bucket_target_plaintexts=" << s.max_target_num_pt
              << "\n";
  }
}
