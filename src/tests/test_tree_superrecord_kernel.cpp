#include "tests.h"

#include <limits>
#include <random>
#include <vector>

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

  require_matches_oracle(3, 683, 2, 65537, no_compile,
                         "multi-plane ragged scalar fallback");
  require_matches_oracle(2, 352, 2, q29, no_runtime,
                         "multi-plane production safe scalar fallback");
  require_matches_oracle(2, 683, 1, q29, yes,
                         "unsafe production width forced through wrapper");
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
}
