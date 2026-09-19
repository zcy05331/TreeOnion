#include "tree_superrecord_test_util.h"
#include "tree_superrecord_select.h"
#include "tree_select.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <random>

using namespace tree_superrecord;
using namespace tree_superrecord_test;

namespace {

bool equal_pyramids(const AlphaPyramid &a, const AlphaPyramid &b) {
  if (a.size() != b.size()) return false;
  for (size_t row = 0; row < a.size(); ++row) {
    if (a[row].size() != b[row].size()) return false;
    for (size_t j = 0; j < a[row].size(); ++j)
      if (a[row][j].ntt_form != b[row][j].ntt_form ||
          a[row][j].c0 != b[row][j].c0 || a[row][j].c1 != b[row][j].c1)
        return false;
  }
  return true;
}

std::vector<uint32_t> interleaved_from_split_columns(
    const std::vector<uint32_t> &split, size_t cols, size_t levels) {
  std::vector<uint32_t> interleaved(split.size());
  for (size_t level = 0; level < levels; ++level) {
    const size_t level_off = level * cols * 2;
    for (size_t col = 0; col < cols; ++col) {
      interleaved[level_off + 2 * col] = split[level_off + col];
      interleaved[level_off + 2 * col + 1] = split[level_off + cols + col];
    }
  }
  return interleaved;
}

std::vector<RlweCt> alpha_fixture(size_t count, const PirParams &scheme,
                                 bool boundary_values) {
  const size_t n = scheme.get_poly_degree();
  const auto &qs = scheme.get_rns_mods();
  std::mt19937_64 rng(20260908 + count);
  std::vector<RlweCt> alpha(count);
  for (size_t j = 0; j < count; ++j) {
    RlweCt &ct = alpha[j];
    ct.c0.resize(n * qs.size());
    ct.c1.resize(n * qs.size());
    ct.ntt_form = false;
    for (size_t k = 0; k < qs.size(); ++k)
      for (size_t i = 0; i < n; ++i) {
        ct.c0[k*n+i] = boundary_values ? ((i+j)%2 ? qs[k]-1 : 0) : rng()%qs[k];
        ct.c1[k*n+i] = boundary_values ? ((i+j)%3 ? qs[k]-1 : 1) : rng()%qs[k];
      }
  }
  return alpha;
}

// This helper only consumes the two shape fields specified by its contract.
TreeParams pyramid_shape(size_t count) {
  TreeParams p;
  p.shape.N0 = count;
  p.shape.alpha_height = std::bit_width(count - 1);
  return p;
}

}  // namespace

void PirTest::test_tree_superrecord_alpha() {
  print_func_name(__FUNCTION__);
  PirParams scheme;
  for (size_t count : {size_t{1}, size_t{2}, size_t{3}, size_t{5}, size_t{64},
                       size_t{351}, size_t{352}, size_t{513}}) {
    const TreeParams params = pyramid_shape(count);
    for (bool boundaries : {false, true}) {
      const auto alpha = alpha_fixture(count, scheme, boundaries);
      const AlphaPyramid original{alpha};
      const AlphaPyramid reference = pyramid_to_ntt(
          build_ragged_alpha_pyramid(alpha, params, scheme), scheme);
      const AlphaPyramid optimized = build_ragged_alpha_pyramid_ntt(alpha, params, scheme);
      require_test(equal_pyramids(reference, optimized),
                   "NTT-domain ragged pyramid equals coefficient-domain oracle at every limb");
      require_test(equal_pyramids(AlphaPyramid{alpha}, original),
                   "NTT-domain pyramid leaves caller ciphertexts unchanged");
      const auto lengths = ragged_pyramid_row_lengths(count);
      for (size_t row = 0; row < lengths.size(); ++row) {
        require_test(optimized[row].size() == lengths[row], "ragged row size");
        if (row && lengths[row-1]%2) {
          const auto &carry = optimized[row].back();
          const auto &source = optimized[row-1].back();
          require_test(carry.c0 == source.c0 && carry.c1 == source.c1 && carry.ntt_form,
                       "unpaired NTT ciphertext is copied, not doubled");
        }
      }
      if (scheme.get_composite_rns().enabled) {
        const auto old_m32 = pyramid_to_m32(reference, scheme);
        const auto new_m32 = pyramid_to_m32(optimized, scheme);
        require_test(old_m32.lo == new_m32.lo && old_m32.hi == new_m32.hi,
                     "NTT-domain pyramid preserves both CRT scan buffers exactly");
        const auto split_m32 = pyramid_to_m32(
            optimized, scheme, Matrix32QueryLayout::SplitColumns);
        require_test(split_m32.query_layout ==
                         Matrix32QueryLayout::SplitColumns,
                     "split-column pyramid records its query layout");
        for (size_t row = 0; row < optimized.size(); ++row) {
          const size_t cols = optimized[row].size();
          const size_t levels = scheme.get_poly_degree();
          require_test(interleaved_from_split_columns(split_m32.lo[row], cols,
                                                      levels) == new_m32.lo[row],
                       "split-column lo row converts back to interleaved");
          require_test(interleaved_from_split_columns(split_m32.hi[row], cols,
                                                      levels) == new_m32.hi[row],
                       "split-column hi row converts back to interleaved");
        }
      }
    }
  }
  const TreeParams params = pyramid_shape(352);
  auto alpha = alpha_fixture(352, scheme, false);
  auto bad = alpha;
  bad[0].ntt_form = true;
  require_test(throws_invalid([&] { (void)build_ragged_alpha_pyramid_ntt(bad, params, scheme); }),
               "already transformed input must not be transformed again");
  bad = alpha;
  bad[0].c1.pop_back();
  require_test(throws_invalid([&] { (void)build_ragged_alpha_pyramid_ntt(bad, params, scheme); }),
               "truncated ciphertext component is rejected before NTT");

  if (!scheme.get_composite_rns().enabled) return;
  const size_t trials = bench_measured_trials.value_or(16);
  const size_t warmups = bench_warmups.value_or(3);
  std::vector<double> old_ms, new_ms;
  volatile uint32_t sink = 0;
  const auto measure = [&](bool optimized) {
    const auto start = std::chrono::steady_clock::now();
    AlphaPyramid coefficients;
    AlphaPyramid ntt;
    if (optimized) ntt = build_ragged_alpha_pyramid_ntt(alpha, params, scheme);
    else {
      coefficients = build_ragged_alpha_pyramid(alpha, params, scheme);
      ntt = pyramid_to_ntt(coefficients, scheme);
    }
    const auto packed = pyramid_to_m32(ntt, scheme);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    sink = packed.lo.back()[0];
    return ms;
  };
  for (size_t i = 0; i < warmups + trials; ++i) {
    double a, b;
    if (i%2) { b = measure(true); a = measure(false); }
    else { a = measure(false); b = measure(true); }
    if (i >= warmups) { old_ms.push_back(a); new_ms.push_back(b); }
  }
  (void)sink;
  if (const char *path = std::getenv("BENCHMARK_OUTPUT_JSON")) {
    std::ofstream out(path);
    require_test(bool(out), "open alpha preparation profile");
    out << std::setprecision(17)
        << "{\"N0\":352,\"pyramid_ciphertexts\":705,\"old_ntt_ciphertexts\":705,"
           "\"new_ntt_ciphertexts\":352,\"includes_m32_split\":true,"
           "\"all_equivalence_checks_passed\":true,\"warmups\":" << warmups
        << ",\"trials\":" << trials << ",\"old_ms\":[";
    for (size_t i=0; i<old_ms.size(); ++i) out << (i ? "," : "") << old_ms[i];
    out << "],\"new_ms\":[";
    for (size_t i=0; i<new_ms.size(); ++i) out << (i ? "," : "") << new_ms[i];
    out << "]}\n";
  }
  BENCH_PRINT("ragged alpha NTT equivalence passed; N0=352, transforms 705 -> 352");
}
