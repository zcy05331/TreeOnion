#include "tests.h"

#include "tree_rotate.h"
#include "tree_select.h"
#include "tree_superrecord_query.h"
#include "tree_superrecord_select.h"
#include "tree_superrecord_test_util.h"

#include <chrono>
#include <limits>
#include <random>
#include <utility>
#include <vector>

using namespace tree_superrecord;
using namespace tree_superrecord_test;

namespace {

std::vector<uint64_t> oracle_m32(const std::vector<uint32_t> &A,
                                 const std::vector<uint32_t> &B,
                                 size_t m, size_t n, size_t levels,
                                 uint64_t q) {
  std::vector<uint64_t> out(m * 2 * levels, 0);
  const uint128_t q128 = static_cast<uint128_t>(q);
  for (size_t level = 0; level < levels; ++level) {
    const size_t a_off = level * m * n;
    const size_t b_off = level * n * 2;
    const size_t c_off = level * m * 2;
    for (size_t row = 0; row < m; ++row) {
      uint128_t t0 = 0;
      uint128_t t1 = 0;
      for (size_t col = 0; col < n; ++col) {
        const uint128_t a = A[a_off + row * n + col];
        t0 += a * B[b_off + 2 * col];
        t1 += a * B[b_off + 2 * col + 1];
      }
      out[c_off + 2 * row] = static_cast<uint64_t>(t0 % q128);
      out[c_off + 2 * row + 1] = static_cast<uint64_t>(t1 % q128);
    }
  }
  return out;
}

std::vector<uint32_t> split_columns_from_interleaved(
    const std::vector<uint32_t> &B, size_t n, size_t levels) {
  std::vector<uint32_t> split(B.size());
  for (size_t level = 0; level < levels; ++level) {
    const size_t level_off = level * n * 2;
    for (size_t col = 0; col < n; ++col) {
      split[level_off + col] = B[level_off + 2 * col];
      split[level_off + n + col] = B[level_off + 2 * col + 1];
    }
  }
  return split;
}

void fill_mod_q(std::vector<uint32_t> &v, uint64_t q, uint64_t seed) {
  std::mt19937_64 rng(seed);
  for (uint32_t &x : v) {
    x = static_cast<uint32_t>(rng() % q);
  }
}

void require_matches_oracle(size_t m, size_t n, size_t levels, uint64_t q,
                            Avx512KernelAvailability availability,
                            const std::string &label) {
  std::vector<uint32_t> A(m * n * levels);
  std::vector<uint32_t> B(n * 2 * levels);
  fill_mod_q(A, q, 0x5EED0000ULL + n + 17 * levels);
  fill_mod_q(B, q, 0xBEEF0000ULL + n + 31 * m);

  std::vector<uint64_t> got(m * 2 * levels, 0);
  level_mat_mat_32_dispatch(A.data(), B.data(), got.data(), m, n, levels, q,
                            availability);
  const std::vector<uint64_t> ref = oracle_m32(A, B, m, n, levels, q);
  require_test(got == ref, label + " must match independent uint128 oracle");

  const std::vector<uint32_t> split =
      split_columns_from_interleaved(B, n, levels);
  std::vector<uint64_t> got_split(m * 2 * levels, 0);
  level_mat_mat_32_dispatch(A.data(), split.data(), got_split.data(), m, n,
                            levels, q, availability,
                            Matrix32QueryLayout::SplitColumns);
  require_test(got_split == ref,
               label + " split-column layout must match independent oracle");
}

size_t first_unsafe_cols(uint64_t q) {
  const uint128_t q_minus_one = static_cast<uint128_t>(q - 1);
  const uint128_t per_group =
      static_cast<uint128_t>(2) * q_minus_one * q_minus_one;
  const uint128_t max_u64 =
      static_cast<uint128_t>(std::numeric_limits<uint64_t>::max());
  const uint128_t safe_groups = max_u64 / per_group;
  return static_cast<size_t>(safe_groups + 1) * 16;
}

std::pair<size_t, size_t> old_plan_pyramid_row_and_cols(
    const tree_superrecord::LevelPlan &plan) {
  switch (plan.select_case) {
    case tree_superrecord::SelectCase::AlphaIndex:
      return {size_t{0}, plan.matrix_cols};
    case tree_superrecord::SelectCase::CoarsenedAlpha:
    case tree_superrecord::SelectCase::Single:
      return {plan.alpha_coarsen, plan.matrix_cols};
  }
  throw std::runtime_error("unknown super-record plan case");
}

RlweCt old_select_level_reference_for_test(
    const LevelPlaneDatabase &db, const tree_superrecord::LevelPlan &plan,
    const AlphaPyramid &pyramid, std::span<GSWCt> sigma_selectors,
    PirServer &mux, const PirParams &scheme,
    const CiphertextDiagnostic *first_dim_checkpoint = nullptr,
    const CiphertextDiagnostic *fold_checkpoint = nullptr) {
  const auto [row_index, cols] = old_plan_pyramid_row_and_cols(plan);
  require_test(row_index < pyramid.size() && pyramid[row_index].size() == cols,
               "old reference fixture pyramid row mismatch");
  const std::vector<RlweCt> &selector = pyramid[row_index];
  std::vector<RlweCt> candidates;
  candidates.reserve(plan.matrix_rows);
  for (size_t row = 0; row < plan.matrix_rows; ++row) {
    RlweCt acc = tree_pt_ct_mul(
        db.plaintexts[matrix_plaintext_index(row, 0, plan)], selector[0],
        scheme);
    for (size_t col = 1; col < cols; ++col) {
      const RlweCt term = tree_pt_ct_mul(
          db.plaintexts[matrix_plaintext_index(row, col, plan)],
          selector[col], scheme);
      tree_ct_add_inplace(acc, term, scheme);
    }
    if (first_dim_checkpoint) (*first_dim_checkpoint)(row, acc);
    candidates.push_back(std::move(acc));
  }
  if (plan.select_case != tree_superrecord::SelectCase::AlphaIndex) {
    return std::move(candidates.front());
  }
  return fold_candidates(std::move(candidates), plan, sigma_selectors, mux,
                         fold_checkpoint);
}

using Clock = std::chrono::steady_clock;

long long elapsed_us(Clock::time_point start, Clock::time_point stop) {
  return std::chrono::duration_cast<std::chrono::microseconds>(stop - start)
      .count();
}

bool same_ciphertext(const RlweCt &a, const RlweCt &b) {
  return a.ntt_form == b.ntt_form && a.c0 == b.c0 && a.c1 == b.c1;
}

bool same_ciphertext_vector(const std::vector<RlweCt> &a,
                            const std::vector<RlweCt> &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!same_ciphertext(a[i], b[i])) return false;
  }
  return true;
}

bool same_pyramid(const AlphaPyramid &a, const AlphaPyramid &b) {
  if (a.size() != b.size()) return false;
  for (size_t row = 0; row < a.size(); ++row) {
    if (!same_ciphertext_vector(a[row], b[row])) return false;
  }
  return true;
}

void fill_plaintext(std::vector<uint64_t> &pt, uint64_t modulus,
                    uint64_t seed) {
  uint64_t state = seed;
  for (uint64_t &x : pt) {
    x = splitmix64(state) % modulus;
  }
}

}  // namespace

void PirTest::test_tree_superrecord_kernel() {
  print_func_name(__FUNCTION__);

  const Avx512KernelAvailability yes{true, true};
  const Avx512KernelAvailability no_compile{false, true};
  const Avx512KernelAvailability no_runtime{true, false};

  constexpr uint64_t q29 = (uint64_t{1} << 29) - 3;
  require_test(!avx512_lazy_accumulation_safe(16, 1),
               "modulus=1 is not a supported lazy-AVX arithmetic modulus");
  require_test(select_matmul32_dispatch(342, q29, no_compile) ==
                   MatMul32Dispatch::ScalarU128,
               "compiled=false/runtime=true chooses scalar");
  require_test(select_matmul32_dispatch(342, q29, no_runtime) ==
                   MatMul32Dispatch::ScalarU128,
               "compiled=true/runtime=false chooses scalar");
  require_test(select_matmul32_dispatch(342, q29, yes) ==
                   MatMul32Dispatch::Avx512Lazy,
               "available safe shape chooses AVX-512 lazy dispatch");

  require_test(avx512_lazy_accumulation_safe(1, q29),
               "cols=1 has no full SIMD group and is safe");
  require_test(avx512_lazy_accumulation_safe(342, q29),
               "cols=342 production shape is safe");
  require_test(avx512_lazy_accumulation_safe(352, q29),
               "cols=352 production shape is safe");
  require_test(!avx512_lazy_accumulation_safe(683, q29),
               "cols=683 production shape is unsafe for 29-bit limbs");
  require_test(!avx512_lazy_accumulation_safe(688, q29),
               "cols=688 production shape is unsafe for 29-bit limbs");
  require_test(!avx512_lazy_accumulation_safe(1024, q29),
               "cols=1024 production shape is unsafe for 29-bit limbs");

  const size_t unsafe = first_unsafe_cols(q29);
  require_test(avx512_lazy_accumulation_safe(unsafe - 1, q29),
               "last column before first unsafe full group remains safe");
  require_test(!avx512_lazy_accumulation_safe(unsafe, q29),
               "first unsafe full group is rejected");
  require_test(!avx512_lazy_accumulation_safe(unsafe + 1, q29),
               "first unsafe full group plus ragged tail is rejected");
  require_test(select_matmul32_dispatch(unsafe, q29, yes) ==
                   MatMul32Dispatch::ScalarU128,
               "unsafe arithmetic shape chooses scalar");

  constexpr uint64_t q32 = 4294967291ULL;
  require_test(avx512_lazy_accumulation_safe(15, q32),
               "large uint32 modulus tail-only shape is safe");
  require_test(!avx512_lazy_accumulation_safe(16, q32),
               "large uint32 modulus first SIMD group is unsafe");

  require_matches_oracle(2, 1, 3, 65537, no_compile,
                         "tiny scalar fallback");
  require_matches_oracle(3, 7, 2, 65537, no_compile,
                         "small ragged scalar fallback");
  require_matches_oracle(2, 511, 1, q29, no_compile,
                         "pre-boundary scalar fallback");
  require_matches_oracle(2, 512, 1, q29, no_compile,
                         "boundary scalar fallback");
  require_matches_oracle(3, 683, 2, 65537, no_compile,
                         "multi-plane ragged scalar fallback");
  require_matches_oracle(2, 352, 2, q29, no_runtime,
                         "multi-plane production safe scalar fallback");
  require_matches_oracle(2, 683, 1, q29, yes,
                         "unsafe production width forced through wrapper");
  require_matches_oracle(1, 688, 1, q29, yes,
                         "unsafe grouped width forced through wrapper");
  require_matches_oracle(1, 1024, 1, q29, yes,
                         "large unsafe width forced through wrapper");
  require_matches_oracle(1, 16, 1, q32, yes,
                         "large valid uint32 modulus forced through wrapper");

  const Avx512KernelAvailability detected = detected_matmul32_availability();
  if (detected.available()) {
    require_matches_oracle(3, 342, 2, q29, detected,
                           "detected AVX-512 production path");
  } else {
    require_test(select_matmul32_dispatch(342, q29, detected) ==
                     MatMul32Dispatch::ScalarU128,
                 "non-AVX hardware or non-AVX build chooses scalar");
    require_matches_oracle(3, 342, 2, q29, yes,
                           "synthetic AVX availability cannot bypass detection");
  }

  PirParams scheme;
  PirClient client(scheme);
  const TreeParams params =
      plan_for(scheme, 256, 1024, 3, 64, 1, test_policy());
  require_test((params.shape.N0 & (params.shape.N0 - 1)) != 0,
               "reference regression uses a ragged non-power-of-two N0");
  const EntrySource source = [](size_t level, size_t node) {
    return synthetic_entry(256, level, node);
  };
  PirServer server(query_expansion_params(params, scheme));
  const TaggedTreeSessionKeys session =
      create_tagged_tree_session_keys(client, params, scheme);
  set_tree_client_session_keys(server, params, scheme, session);
  const ReferencePreprocessedTree reference =
      preprocess_reference(params, scheme, source);
  const PackedQuery packed =
      make_query(client, session, scheme, params, params.shape.leaf_count - 1);
  ExpandedQuery unpacked =
      unpack_query(server, scheme, params, session, packed, nullptr);
  const AlphaPyramid pyramid =
      build_ragged_alpha_pyramid(unpacked.alpha, params, scheme);

  const size_t level = params.shape.tree_height;
  const tree_superrecord::LevelPlan &plan = params.level_plans[level];
  require_test(plan.select_case == tree_superrecord::SelectCase::AlphaIndex &&
                   plan.matrix_rows > 1 && plan.matrix_cols == params.shape.N0,
               "reference regression exercises first-dimension rows and fold");
  const LevelPlaneDatabase &db = reference.canonical[level][0];
  const std::vector<std::vector<uint64_t>> original_plaintexts =
      db.plaintexts;
  const AlphaPyramid original_pyramid = pyramid;

  std::vector<RlweCt> old_first_dim;
  std::vector<RlweCt> new_first_dim;
  std::vector<std::pair<size_t, RlweCt>> old_fold;
  std::vector<std::pair<size_t, RlweCt>> new_fold;
  const CiphertextDiagnostic old_first_cb = [&](size_t row, const RlweCt &ct) {
    require_test(row == old_first_dim.size(),
                 "old first-dimension callback order");
    old_first_dim.push_back(ct);
  };
  const CiphertextDiagnostic new_first_cb = [&](size_t row, const RlweCt &ct) {
    require_test(row == new_first_dim.size(),
                 "new first-dimension callback order");
    new_first_dim.push_back(ct);
  };
  const CiphertextDiagnostic old_fold_cb = [&](size_t step, const RlweCt &ct) {
    old_fold.push_back({step, ct});
  };
  const CiphertextDiagnostic new_fold_cb = [&](size_t step, const RlweCt &ct) {
    new_fold.push_back({step, ct});
  };

  std::vector<GSWCt> old_sigma = unpacked.sigma_selectors;
  std::vector<GSWCt> new_sigma = unpacked.sigma_selectors;
  const auto old_start = Clock::now();
  const RlweCt old_selected = old_select_level_reference_for_test(
      db, plan, pyramid, std::span<GSWCt>(old_sigma), server, scheme,
      &old_first_cb, &old_fold_cb);
  const auto old_stop = Clock::now();
  const AlphaPyramid pyramid_after_old = pyramid;
  const auto new_start = Clock::now();
  const std::vector<GSWCt> original_new_sigma = new_sigma;
  const RlweCt new_selected = select_level_reference(
      db, plan, pyramid, std::span<GSWCt>(new_sigma), server, params, scheme,
      nullptr, &new_first_cb, &new_fold_cb);
  const auto new_stop = Clock::now();

  require_test(old_first_dim.size() == plan.matrix_rows &&
                   new_first_dim.size() == old_first_dim.size(),
               "reference first-dimension callbacks fire once per row");
  for (size_t row = 0; row < old_first_dim.size(); ++row) {
    require_test(same_ciphertext(new_first_dim[row], old_first_dim[row]),
                 "optimized reference row equals old term oracle");
  }
  require_test(new_fold.size() == old_fold.size(),
               "reference fold callback count is preserved");
  for (size_t i = 0; i < old_fold.size(); ++i) {
    require_test(new_fold[i].first == old_fold[i].first &&
                     same_ciphertext(new_fold[i].second, old_fold[i].second),
                 "optimized reference fold equals old term oracle");
  }
  require_test(same_ciphertext(new_selected, old_selected),
               "optimized reference result equals old term oracle");
  require_test(db.plaintexts == original_plaintexts,
               "optimized reference does not mutate canonical plaintexts");
  require_test(same_pyramid(pyramid, original_pyramid) &&
                   same_pyramid(pyramid_after_old, original_pyramid),
               "reference selection does not mutate the alpha pyramid");
  require_test(new_sigma == original_new_sigma,
               "optimized reference does not mutate sigma selectors");

  const long long old_us = elapsed_us(old_start, old_stop);
  const long long new_us = elapsed_us(new_start, new_stop);
  BENCH_PRINT("super-record reference old_us=" << old_us
                                               << " optimized_us=" << new_us
                                               << " rows=" << plan.matrix_rows
                                               << " cols=" << plan.matrix_cols);

  const TreeParams perf_params = plan_for(
      scheme, 256, size_t{1} << 19, 3, 64, 4, test_policy());
  const tree_superrecord::LevelPlan &perf_plan = perf_params.level_plans.back();
  const size_t expected_rows = size_t{1} << 4;
  const size_t expected_cols = ceil_div(
      ceil_div(ceil_div(size_t{1} << 19, size_t{3}),
               scheme.get_poly_degree() / 64),
      expected_rows);
  require_test(perf_plan.matrix_rows == expected_rows &&
                   perf_plan.matrix_cols == expected_cols &&
                   !std::has_single_bit(expected_cols),
               "bounded performance fixture uses a feasible ragged plan");
  LevelPlaneDatabase perf_db;
  perf_db.level = perf_plan.level;
  perf_db.plane_index = 0;
  perf_db.logical_superrecords = perf_plan.logical_superrecords;
  perf_db.logical_plaintexts = perf_plan.logical_plaintexts_per_plane;
  perf_db.matrix_rows = perf_plan.matrix_rows;
  perf_db.matrix_cols = perf_plan.matrix_cols;
  perf_db.plaintexts.assign(perf_plan.padded_plaintexts_per_plane,
                            std::vector<uint64_t>(scheme.get_poly_degree(), 0));
  for (size_t row = 0; row < perf_plan.matrix_rows; ++row) {
    for (size_t col = 0; col < perf_plan.matrix_cols; ++col) {
      if (col % 7 == 0 || col % 7 == 1 ||
          matrix_plaintext_index(row, col, perf_plan) >=
              perf_plan.logical_plaintexts_per_plane) continue;
      fill_plaintext(
          perf_db.plaintexts[matrix_plaintext_index(row, col, perf_plan)],
          scheme.get_plain_mod(), 0xC0FFEE00ULL + row * 4099 + col);
    }
  }
  AlphaPyramid perf_pyramid(1);
  perf_pyramid[0].reserve(perf_plan.matrix_cols);
  for (size_t col = 0; col < perf_plan.matrix_cols; ++col) {
    perf_pyramid[0].push_back(client.fresh_zero_ct());
  }
  // The larger ring moves the four-bit fold from [5, 9) to [6, 10),
  // beyond the nine selectors in the earlier query. All performance alpha
  // inputs encrypt zero, so either selector bit is valid: reuse an encrypted
  // bit from this same client and scheme without consuming extra randomness.
  const size_t required_selectors =
      perf_plan.fold_bit_begin + perf_plan.fold_bit_count;
  while (new_sigma.size() < required_selectors) {
    new_sigma.push_back(unpacked.sigma_selectors.front());
  }
  const auto perf_old_start = Clock::now();
  const RlweCt perf_old = old_select_level_reference_for_test(
      perf_db, perf_plan, perf_pyramid, std::span<GSWCt>(new_sigma), server,
      scheme);
  const auto perf_old_stop = Clock::now();
  const auto perf_new_start = Clock::now();
  const RlweCt perf_new = select_level_reference(
      perf_db, perf_plan, perf_pyramid, std::span<GSWCt>(new_sigma), server,
      perf_params, scheme);
  const auto perf_new_stop = Clock::now();
  require_test(same_ciphertext(perf_new, perf_old),
               "bounded matrix optimized reference equals old term oracle");
  BENCH_PRINT("bounded-matrix reference old_us="
              << elapsed_us(perf_old_start, perf_old_stop)
              << " optimized_us=" << elapsed_us(perf_new_start, perf_new_stop)
              << " rows=" << perf_plan.matrix_rows
              << " cols=" << perf_plan.matrix_cols);
}
