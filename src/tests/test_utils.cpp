#include "tests.h"

#include <algorithm>
#include <string>
#include <vector>

void test_ntt_friendly_primes();
void test_paper_moduli_pin();

void PirTest::test_utils_arith() {
  print_func_name(__FUNCTION__);

  BENCH_PRINT("--- utils::is_prime ---");
  const std::vector<uint64_t> primes = {
      2, 3, 5, 7, 11, 13, 97, 1000003,
      1152921504606748673ULL,
      1152921504606830593ULL,
      36028797018820609ULL,
      (1ULL << 61) - 1,
  };
  const std::vector<uint64_t> composites = {
      0, 1, 4, 6, 9, 15, 21, 25, 49, 1000000,
      1152921504606748673ULL - 2,
      1ULL << 61,
      (1ULL << 60) + 1,
      3215031751ULL,
  };

  int prime_fail = 0, comp_fail = 0;
  for (uint64_t p : primes) {
    if (!utils::is_prime(p)) {
      BENCH_PRINT("  MISS: " << p << " reported composite");
      ++prime_fail;
    }
  }
  for (uint64_t c : composites) {
    if (utils::is_prime(c)) {
      BENCH_PRINT("  MISS: " << c << " reported prime");
      ++comp_fail;
    }
  }
  BENCH_PRINT("  Primes recognized:     " << (primes.size() - prime_fail) << "/" << primes.size());
  BENCH_PRINT("  Composites recognized: " << (composites.size() - comp_fail) << "/" << composites.size());

  BENCH_PRINT("\n--- utils::try_invert_uint_mod ---");

  struct Case { uint64_t val, mod; };
  const std::vector<Case> invertible = {
      {3, 7},
      {2, 1152921504606748673ULL},
      {1234567, 1152921504606748673ULL},
      {7, 36028797018820609ULL},
      {1ULL << 20, 1152921504606830593ULL},
  };
  const std::vector<Case> non_invertible = {
      {0, 7},
      {6, 9},
      {10, 100},
  };

  int inv_fail = 0;
  for (auto [val, mod] : invertible) {
    uint64_t inv;
    bool ok = utils::try_invert_uint_mod(val, mod, inv);
    __uint128_t prod = static_cast<__uint128_t>(val) * inv;
    uint64_t check = static_cast<uint64_t>(prod % mod);
    if (!ok || check != 1) {
      BENCH_PRINT("  MISS: inv(" << val << ", " << mod << ") -> ok=" << ok
                  << " inv=" << inv << " val*inv%mod=" << check);
      ++inv_fail;
    }
  }
  int noninv_fail = 0;
  for (auto [val, mod] : non_invertible) {
    uint64_t inv;
    bool ok = utils::try_invert_uint_mod(val, mod, inv);
    if (ok) {
      BENCH_PRINT("  MISS: inv(" << val << ", " << mod << ") should have failed, got " << inv);
      ++noninv_fail;
    }
  }
  BENCH_PRINT("  Invertible cases:     " << (invertible.size() - inv_fail) << "/" << invertible.size());
  BENCH_PRINT("  Non-invertible cases: " << (non_invertible.size() - noninv_fail) << "/" << non_invertible.size());

  BENCH_PRINT("\n--- Summary ---");
  const int total = primes.size() + composites.size() + invertible.size() + non_invertible.size();
  const int failed = prime_fail + comp_fail + inv_fail + noninv_fail;
  BENCH_PRINT("Passed " << (total - failed) << "/" << total << " cases");
  require_test(failed == 0, "utils arithmetic checks failed");
}

void test_ntt_friendly_primes() {
  print_func_name(__FUNCTION__);
  constexpr size_t N = DBConsts::PolyDegree;
  const uint64_t step = 2 * static_cast<uint64_t>(N);
  size_t checked = 0;
  const std::vector<std::vector<size_t>> width_lists = {
      {29, 29}, {22}, {58}, {60}};
  for (const std::vector<size_t> &widths : width_lists) {
    const std::vector<uint64_t> out =
        utils::generate_ntt_friendly_primes(widths, N);
    require_test(out.size() == widths.size(),
                 "generate_ntt_friendly_primes must return one prime per width");
    for (size_t i = 0; i < out.size(); ++i) {
      const uint64_t p = out[i];
      const std::string width = " (width " + std::to_string(widths[i]) + ")";
      const uint64_t upper = uint64_t{1} << widths[i];
      require_test(utils::is_prime(p),
                   "generate_ntt_friendly_primes returned a composite" + width);
      require_test(p < upper, "NTT-friendly prime is not below 2^bw" + width);
      require_test(p % step == 1, "NTT-friendly prime is not 1 mod 2N" + width);
      uint64_t limit = upper;
      for (size_t k = 0; k < i; ++k) {
        if (widths[k] == widths[i]) {
          require_test(p < out[k],
                       "repeated width must continue downward" + width);
          limit = std::min(limit, out[k]);
        }
      }
      for (uint64_t c = p + step; c < limit; c += step) {
        require_test(!utils::is_prime(c),
                     "generate_ntt_friendly_primes skipped a larger prime of "
                     "the same class" + width);
      }
      ++checked;
    }
  }
  for (size_t bw : {size_t{9}, size_t{10}, size_t{13}, size_t{14}, size_t{40}}) {
    const uint64_t p = utils::generate_prime(bw);
    const uint64_t lower = uint64_t{1} << (bw - 1);
    const std::string width = " (width " + std::to_string(bw) + ")";
    require_test(utils::is_prime(p) && p > lower && p < 2 * lower,
                 "generate_prime result is not a prime of the requested width" +
                     width);
    for (uint64_t c = lower + 1; c < p; ++c) {
      require_test(!utils::is_prime(c),
                   "generate_prime skipped a smaller prime of the requested "
                   "width" + width);
    }
    ++checked;
  }
  std::cout << "PASS: " << checked
            << " generated moduli obey the largest/smallest-prime rules (N="
            << N << ")\n";
}

void test_paper_moduli_pin() {
  print_func_name(__FUNCTION__);
  require_applicable(paper_config_active(), paper_config_reason("the moduli pin"));
  PirParams params;
  const CompositeRnsTables &crt = params.get_composite_rns();
  constexpr uint64_t kQ1 = 536813569ULL;
  constexpr uint64_t kQ2 = 536752129ULL;
  constexpr uint64_t kQ = 288135826036838401ULL;
  require_test(crt.enabled,
               "the paper point must run the composite first dimension");
  require_test(crt.q1 == kQ1 && crt.q2 == kQ2,
               "composite limbs moved from the paper point: q1=" +
                   std::to_string(crt.q1) + " q2=" + std::to_string(crt.q2));
  require_test(params.get_rns_mods() == std::vector<uint64_t>{kQ},
               "logical ciphertext modulus moved from the paper point");
  require_test(params.get_plain_mod() == 4099,
               "plaintext modulus moved from the paper point: t=" +
                   std::to_string(params.get_plain_mod()));
  require_test(params.get_small_q() == 4169729,
               "small response modulus moved from the paper point: small_q=" +
                   std::to_string(params.get_small_q()));
  require_test(crt.w_crt < kQ && crt.w_crt % kQ1 == crt.w1 &&
                   crt.w_crt % kQ2 == crt.w2,
               "composite NTT root is not the CRT combination of the limb roots");
  std::cout << "PASS: paper-point moduli pinned: q1=" << crt.q1
            << " q2=" << crt.q2 << " q=" << kQ
            << " t=" << params.get_plain_mod()
            << " small_q=" << params.get_small_q() << "\n";
}
