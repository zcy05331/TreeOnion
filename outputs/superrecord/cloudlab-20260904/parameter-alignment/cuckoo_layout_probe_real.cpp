#include "cuckoo_batch.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "/Users/little_sun/Desktop/little_sun/26Summer/OnionPIRv2/.worktrees/generic-superrecord/src/cuckoo_batch.cpp"

PirParams::PirParams() {}

TargetPlaintextLayout select_target_plaintext_layout(
    std::size_t, const PirParams&) {
  return {};
}

namespace utils {
std::size_t roundup_div(const std::size_t numerator,
                        const std::size_t denominator) {
  return (numerator + denominator - 1) / denominator;
}
}  // namespace utils

namespace {

struct Shape {
  std::size_t target_num_pt = 0;
  std::size_t expansion_height = 0;
  std::size_t fst_dim_sz = 0;
  std::size_t num_other_dims = 0;
  std::size_t other_dim_sz = 0;
  std::size_t num_pt = 0;
};

std::size_t roundup_div(std::size_t numerator, std::size_t denominator) {
  return (numerator + denominator - 1) / denominator;
}

std::pair<std::size_t, std::size_t> calculate_db_shape(
    std::size_t target_num_pt, std::size_t l, std::size_t h,
    bool fst_dim_pow2) {
  const std::size_t capacity = std::size_t{1} << h;
  const std::size_t max_num_dims = 1 + (capacity - 1) / l;
  for (std::size_t num_dims = 1; num_dims <= max_num_dims + 1; ++num_dims) {
    const std::size_t reserved = l * (num_dims - 1);
    if (reserved >= capacity) break;
    const std::size_t slack = capacity - reserved;
    const std::size_t fst_dim_sz =
        fst_dim_pow2 ? (std::size_t{1} << (std::bit_width(slack) - 1))
                     : slack;
    if (fst_dim_sz * (std::size_t{1} << (num_dims - 1)) >= target_num_pt) {
      return {fst_dim_sz, num_dims};
    }
  }
  throw std::runtime_error("Failed to calculate database shape");
}

Shape make_layout(std::size_t target_num_pt, std::size_t h) {
  const auto [fst_dim_sz, num_dims] =
      calculate_db_shape(target_num_pt, 6, h, true);
  const std::size_t other_dim_sz = roundup_div(target_num_pt, fst_dim_sz);
  return {target_num_pt, h, fst_dim_sz, num_dims - 1, other_dim_sz,
          fst_dim_sz * other_dim_sz};
}

Shape first_feasible_flat_reference(std::size_t target_num_pt) {
  for (std::size_t h = 0; h <= 10; ++h) {
    try {
      return make_layout(target_num_pt, h);
    } catch (const std::runtime_error&) {
    }
  }
  throw std::runtime_error("no flat reference layout");
}

Shape layerwise_benchmark_reference(std::size_t target_num_pt,
                                    std::size_t leaf_count) {
  if (leaf_count >= (std::size_t{1} << 24)) {
    return make_layout(target_num_pt, 10);
  }
  return first_feasible_flat_reference(target_num_pt);
}

Shape select_layout(std::size_t target_num_pt, const Shape& reference) {
  bool found = false;
  std::tuple<std::size_t, std::size_t, std::size_t, std::size_t> best_score;
  Shape best;
  for (std::size_t h = 0; h <= reference.expansion_height; ++h) {
    try {
      Shape candidate = make_layout(target_num_pt, h);
      if (candidate.num_other_dims > reference.num_other_dims) continue;
      const std::size_t useful_expand =
          candidate.fst_dim_sz + 6 * candidate.num_other_dims;
      const auto score = std::make_tuple(candidate.num_pt, useful_expand,
                                         candidate.num_other_dims, h);
      if (!found || score < best_score) {
        found = true;
        best_score = score;
        best = candidate;
      }
    } catch (const std::runtime_error&) {
    }
  }
  if (!found) throw std::runtime_error("no layout");
  return best;
}

std::vector<uint32_t> count_members(const CuckooBatchParams& params) {
  std::vector<uint32_t> counts(params.num_buckets);
  for (uint64_t ordinal = 0; ordinal < params.item_count; ++ordinal) {
    std::size_t ids[3];
    for (std::size_t j = 0; j < params.num_hashes; ++j) {
      ids[j] = cuckoo_bucket_of(ordinal, j, params);
    }
    std::sort(ids, ids + params.num_hashes);
    std::size_t* end = std::unique(ids, ids + params.num_hashes);
    for (std::size_t* it = ids; it != end; ++it) {
      ++counts[*it];
    }
  }
  return counts;
}

void inspect(std::size_t H) {
  const std::size_t leaf_count = std::size_t{1} << H;
  const std::size_t item_count = 2 * (leaf_count - 1);
  CuckooBatchParams params =
      make_cuckoo_batch_params(item_count, H, 0x63756b32ULL);
  ++params.num_buckets;
  const std::vector<uint32_t> counts = count_members(params);
  const std::size_t flat_target = roundup_div(item_count, std::size_t{96});
  const Shape old_ref = first_feasible_flat_reference(flat_target);
  const Shape new_ref = layerwise_benchmark_reference(flat_target, leaf_count);

  uint64_t total = 0;
  for (uint32_t count : counts) total += count;
  const auto minmax_members =
      std::minmax_element(counts.begin(), counts.end());

  std::cout << "H=" << H << " buckets=" << params.num_buckets
            << " total_member_entries=" << total
            << " member_count_min=" << *minmax_members.first
            << " member_count_max=" << *minmax_members.second
            << " flat_target=" << flat_target
            << " old_ref_h=" << old_ref.expansion_height
            << " old_ref_N0=" << old_ref.fst_dim_sz
            << " old_ref_remaining_dims=" << old_ref.num_other_dims
            << " new_ref_h=" << new_ref.expansion_height
            << " new_ref_N0=" << new_ref.fst_dim_sz
            << " new_ref_remaining_dims=" << new_ref.num_other_dims
            << "\n";

  std::size_t changed = 0;
  for (std::size_t b = 0; b < counts.size(); ++b) {
    const std::size_t target = roundup_div(counts[b], std::size_t{96});
    const Shape old_layout = select_layout(target, old_ref);
    const Shape new_layout = select_layout(target, new_ref);
    const bool differs =
        std::tie(old_layout.expansion_height, old_layout.fst_dim_sz,
                 old_layout.other_dim_sz, old_layout.num_other_dims,
                 old_layout.num_pt) !=
        std::tie(new_layout.expansion_height, new_layout.fst_dim_sz,
                 new_layout.other_dim_sz, new_layout.num_other_dims,
                 new_layout.num_pt);
    changed += differs ? 1 : 0;
    std::cout << "bucket=" << b << " members=" << counts[b]
              << " target=" << target
              << " old_h=" << old_layout.expansion_height
              << " old_N0=" << old_layout.fst_dim_sz
              << " old_Nrest=" << old_layout.other_dim_sz
              << " old_remaining_dims=" << old_layout.num_other_dims
              << " old_padded=" << old_layout.num_pt
              << " new_h=" << new_layout.expansion_height
              << " new_N0=" << new_layout.fst_dim_sz
              << " new_Nrest=" << new_layout.other_dim_sz
              << " new_remaining_dims=" << new_layout.num_other_dims
              << " new_padded=" << new_layout.num_pt
              << " changed=" << (differs ? "true" : "false") << "\n";
  }
  std::cout << "H=" << H << " changed_buckets=" << changed << "\n";
}

}  // namespace

int main() {
  inspect(24);
  inspect(27);
}
