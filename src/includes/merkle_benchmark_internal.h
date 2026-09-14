#pragma once

#include "merkle_benchmark.h"
#include "utils.h"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>

inline uint64_t checked_add(uint64_t left, uint64_t right, const char *field) {
  if (left > std::numeric_limits<uint64_t>::max() - right) {
    throw std::overflow_error(std::string(field) + " overflows uint64_t");
  }
  return left + right;
}

inline uint64_t checked_multiply(uint64_t left, uint64_t right,
                                 const char *field) {
  if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
    throw std::overflow_error(std::string(field) + " overflows uint64_t");
  }
  return left * right;
}

inline double milliseconds(BenchmarkDuration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

inline BenchmarkDuration elapsed_since(BenchmarkClock::time_point start) {
  return std::chrono::duration_cast<BenchmarkDuration>(
      BenchmarkClock::now() - start);
}

inline size_t flat_target_num_pt(const MerkleWorkload &workload) {
  if (workload.tree_height == 0 ||
      workload.tree_height >= std::numeric_limits<size_t>::digits - 1 ||
      workload.leaf_count != (size_t{1} << workload.tree_height) ||
      workload.node_bytes != sizeof(MerkleNode)) {
    throw std::invalid_argument("Invalid Merkle benchmark workload");
  }
  const size_t node_count = 2 * (workload.leaf_count - 1);
  return utils::roundup_div(node_count, size_t{96});
}

inline uint64_t physical_database_bytes(const PirParams &params) {
  const uint64_t coefficients = checked_multiply(
      params.get_num_pt(), params.get_coeff_val_cnt(),
      "preprocessed database coefficients");
  return checked_multiply(coefficients, sizeof(db_coeff_t),
                          "physical preprocessed database bytes");
}
