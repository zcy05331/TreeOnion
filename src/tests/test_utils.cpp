#include "tests.h"

#include <algorithm>
#include <string>
#include <vector>

void test_ntt_friendly_primes();
void test_paper_moduli_pin();

// Quick sanity tests for self-contained arithmetic in utils:
//   - utils::is_prime (deterministic Miller-Rabin)
//   - utils::try_invert_uint_mod (extended Euclidean modular inverse)
void PirTest::test_utils_arith() {
  print_func_name(__FUNCTION__);

  // ===================== is_prime =====================
  BENCH_PRINT("--- utils::is_prime ---");
  const std::vector<uint64_t> primes = {
      2, 3, 5, 7, 11, 13, 97, 1000003,
      1152921504606748673ULL, // 60-bit PIR q
      1152921504606830593ULL, // second PIR prime
      36028797018820609ULL,   // small_q
      (1ULL << 61) - 1,       // Mersenne prime M61
  };
  const std::vector<uint64_t> composites = {
      0, 1, 4, 6, 9, 15, 21, 25, 49, 1000000,
      1152921504606748673ULL - 2,
      1ULL << 61,
      (1ULL << 60) + 1,
      3215031751ULL, // Carmichael-like; smallest strong pseudoprime to bases 2,3,5,7
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

  // ===================== try_invert_uint_mod =====================
  BENCH_PRINT("\n--- utils::try_invert_uint_mod ---");

  // Pairs where gcd(value, modulus) == 1 (invertible).
  struct Case { uint64_t val, mod; };
  const std::vector<Case> invertible = {
      {3, 7},
      {2, 1152921504606748673ULL},
      {1234567, 1152921504606748673ULL},
      {7, 36028797018820609ULL},
      {1ULL << 20, 1152921504606830593ULL},
  };
  // Pairs where gcd != 1 (should fail).
  const std::vector<Case> non_invertible = {
      {0, 7},
      {6, 9},      // gcd = 3
      {10, 100},   // gcd = 10
  };

  int inv_fail = 0;
  for (auto [val, mod] : invertible) {
    uint64_t inv;
    bool ok = utils::try_invert_uint_mod(val, mod, inv);
    // verify: (val * inv) mod modulus == 1
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

  // ===================== Summary =====================
  BENCH_PRINT("\n--- Summary ---");
  const int total = primes.size() + composites.size() + invertible.size() + non_invertible.size();
  const int failed = prime_fail + comp_fail + inv_fail + noninv_fail;
  BENCH_PRINT("Passed " << (total - failed) << "/" << total << " cases");
  require_test(failed == 0, "utils arithmetic checks failed");
}

// ============================================================================
// utils::generate_ntt_friendly_primes / utils::generate_prime: the rules that
// choose every modulus of every config.
//
// Each PirParams construction calls them with the config's bit widths, and
// any NTT-friendly prime keeps every round-trip test green. A regression in
// the "largest prime below 2^bw in the class 1 mod 2N" alignment
// (utils.cpp: c -= (c - 1) % step), in the repeat-width continuation, or in
// generate_prime's upward scan would silently move the parameter point, so
// the rules are asserted directly: primality, the bound, the residue class,
// and that no candidate of the same class between the result and its limit
// is prime.
// ============================================================================
void test_ntt_friendly_primes() {
  print_func_name(__FUNCTION__);
  constexpr size_t N = DBConsts::PolyDegree;
  const uint64_t step = 2 * static_cast<uint64_t>(N);
  size_t checked = 0;
  // The width lists the configs use: {29,29} composite limbs / K=2 limbs,
  // {22} small_q, {58} and {60} single-limb moduli.
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
      // "Largest" rule. The limit is 2^bw, or for a repeated width the prime
      // returned for its previous occurrence: the new one must be the next
      // prime of the class below it.
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
  // generate_prime(bw): the smallest prime above 2^(bw-1). The configs take
  // the plaintext modulus t from it (PlainMod widths 9, 10, 13, 14, 40).
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

// ============================================================================
// The paper parameter point (CONFIG_N2048_K1_COMP) pinned by value: the
// composite limbs q1, q2, the logical modulus q = q1*q2, t, the small
// response modulus, and the CRT-combined 2N-th root init_composite_rns
// registers for the composite NTT. Every archived benchmark and the frozen
// planner constants were produced with these numbers; a change in prime
// generation or in crt_combine would otherwise leave --test all green.
// ============================================================================
void test_paper_moduli_pin() {
  print_func_name(__FUNCTION__);
  require_applicable(paper_config_active(), paper_config_reason("the moduli pin"));
  PirParams params;
  const CompositeRnsTables &crt = params.get_composite_rns();
  constexpr uint64_t kQ1 = 536813569ULL;          // largest prime < 2^29, 1 mod 4096
  constexpr uint64_t kQ2 = 536752129ULL;          // the next such prime down
  constexpr uint64_t kQ = 288135826036838401ULL;  // q1 * q2, 58 bits
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
