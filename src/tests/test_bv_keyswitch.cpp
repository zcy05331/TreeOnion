#include "tests.h"
#include "bv_keyswitch.h"
#include "rlwe.h"
#include "utils.h"
#include <random>
#include <array>

static void test_rounded_bv_digits() {
  PirParams scheme;
  const uint64_t q = scheme.get_rns_mods()[0];
  for (size_t rows : {size_t{2}, size_t{3}}) {
    for (size_t drop : {size_t{0}, size_t{16}, size_t{24}}) {
      const size_t bits = (scheme.get_ct_mod_width() - drop) / rows + 1;
      const uint64_t half = drop ? uint64_t{1} << (drop - 1) : 0;
      std::mt19937_64 rng(0x627664726f70ULL);
      for (size_t sample = 0; sample < 2000; ++sample) {
        const std::array<uint64_t, 8> edges = {
            0, 1, q - 1, q / 2, q / 2 + 1, half,
            half ? half - 1 : 0, half ? q - half : q - 1};
        const uint64_t value = sample < edges.size() ? edges[sample] : rng() % q;
        uint64_t digits[3] = {};
        bvks::signed_gadget_decompose(value, bits, q, digits, rows, drop);
        __int128 reconstructed = 0;
        for (size_t i = 0; i < rows; ++i) {
          const int64_t digit = digits[i] > q / 2
              ? static_cast<int64_t>(digits[i]) - static_cast<int64_t>(q)
              : static_cast<int64_t>(digits[i]);
          require_test(digit >= -(int64_t{1} << (bits - 1)) &&
                           digit < (int64_t{1} << (bits - 1)),
                       "BV rounded digit exceeds its balanced range");
          reconstructed += static_cast<__int128>(digit) *
                           (static_cast<__int128>(1) << (drop + i * bits));
        }
        const int64_t centered = value > q / 2
            ? static_cast<int64_t>(value) - static_cast<int64_t>(q)
            : static_cast<int64_t>(value);
        const __int128 residual = static_cast<__int128>(centered) - reconstructed;
        require_test(residual >= -static_cast<__int128>(half) &&
                         residual <= static_cast<__int128>(half),
                     "BV approximate reconstruction exceeds half a rounding step");
      }
    }
  }
}

// ============================================================================
// Unit test: signed gadget decomposition
// ============================================================================
static void test_signed_decompose() {
  std::cout << "--- test_signed_decompose ---\n";

  PirParams pir_params;
  const size_t q_bits = pir_params.get_ct_mod_width();
  const size_t base_log2 = (q_bits + bvks::L_KS - 1) / bvks::L_KS;
  const uint64_t B = uint64_t(1) << base_log2;
  const uint64_t q = pir_params.get_rns_mods()[0];
  const uint64_t half_q = q >> 1;

  std::mt19937_64 rng(42);
  constexpr size_t num_tests = 10000;
  size_t max_digit_mag = 0;

  for (size_t t = 0; t < num_tests; ++t) {
    uint64_t val = rng() % q;
    uint64_t digits[bvks::L_KS];
    bvks::signed_gadget_decompose(val, base_log2, q, digits, bvks::L_KS);

    // 1. Reconstruction: Σ digits[i] * B^i ≡ val (mod q)
    uint128_t reconstructed = 0;
    uint128_t Bi = 1;
    for (size_t i = 0; i < bvks::L_KS; ++i) {
      reconstructed = (reconstructed + (static_cast<uint128_t>(digits[i]) * Bi) % q) % q;
      Bi = (Bi * B) % q;
    }
    require_test(static_cast<uint64_t>(reconstructed) == val,
                 "signed gadget decomposition does not reconstruct");

    // 2. Each digit has signed magnitude ≤ B/2
    for (size_t i = 0; i < bvks::L_KS; ++i) {
      uint64_t mag = (digits[i] > half_q) ? (q - digits[i]) : digits[i];
      if (mag > max_digit_mag) max_digit_mag = mag;
      require_test(mag <= B / 2,
                   "signed gadget digit magnitude exceeds B/2");
    }
  }

  std::cout << "PASS: " << num_tests << " random values reconstructed correctly\n";
  std::cout << "  base_log2=" << base_log2 << ", B=" << B
            << ", max digit magnitude=" << max_digit_mag << " (B/2=" << (B / 2) << ")\n";
}

// ============================================================================
// Unit test: BV key-switching correctness
//
// Encrypt a known plaintext, apply the galois automorphism σ_k via BV
// key-switching, decrypt natively, and verify the result matches the
// automorphism applied directly to the plaintext.
// ============================================================================
void PirTest::test_bv_keyswitch() {
  test_rounded_bv_digits();
  test_signed_decompose();
  print_func_name(__FUNCTION__);

  PirParams pir_params;
  constexpr size_t N  = DBConsts::PolyDegree;
  const std::vector<uint64_t> qs(pir_params.get_rns_mods().begin(),
                                 pir_params.get_rns_mods().end());
  const uint64_t t    = pir_params.get_plain_mod();
  const double sigma  = pir_params.get_noise_std_dev();
  const RnsTables &tables = pir_params.get_rns_tables();
  const uint32_t galois_k = 257; // 2^8 + 1

  BENCH_PRINT("K=" << qs.size() << " N=" << N << " t=" << t);

  std::mt19937_64 rng(0x62766b73ULL);
  RlweSk sk = gen_secret_key_rns(N, qs, rng);

  // Build a plaintext with a few distinctive coefficients.
  std::vector<uint64_t> pt(N, 0);
  pt[0] = 1; pt[1] = 2; pt[2] = 3;
  pt[10] = 10; pt[11] = 11; pt[12] = 12;

  // Encrypt (coeff form).
  RlweCt ct;
  encrypt_bfv_rns(pt, sk, N, qs, t, sigma, rng, ct);

  {
    RlwePt dec;
    decrypt_rns(ct, sk, N, qs, t, tables, dec);
    BENCH_PRINT("fresh decrypt[0..2]: " << dec.data[0] << ", " << dec.data[1] << ", " << dec.data[2]);
  }

  // Expected result after σ_k: apply automorphism to the plaintext directly.
  std::vector<uint64_t> pt_auto(N, 0);
  utils::automorphism_coeff(pt.data(), N, galois_k, t, pt_auto.data());

  // bv_apply_galois_inplace expects a coefficient-form ciphertext.
  auto bv_ksk = bvks::gen_bv_ks_key(pir_params, sk, galois_k, rng);
  bvks::bv_apply_galois_inplace(ct, galois_k, bv_ksk, pir_params);

  RlwePt dec_bv;
  decrypt_rns(ct, sk, N, qs, t, tables, dec_bv);
  // BENCH_PRINT("BV galois decrypt[0..2]: " << dec_bv.data[0] << ", " << dec_bv.data[1] << ", " << dec_bv.data[2]);
  // BENCH_PRINT("coeff at idx " << 1 << " maps to: " << galois_k << " got=" << dec_bv.data[galois_k] << " expected: " << pt_auto[galois_k]);
  // BENCH_PRINT("coeff at idx " << 2 << " maps to: " << galois_k * 2 << " got=" << dec_bv.data[galois_k * 2] << " expected: " << pt_auto[galois_k * 2]);
  // BENCH_PRINT("coeff at idx " << 3 << " maps to: " << galois_k * 3 << " got=" << dec_bv.data[galois_k * 3] << " expected: " << pt_auto[galois_k * 3]);
  // Compare against expected automorphism of the plaintext.
  size_t diffs = 0;
  for (size_t i = 0; i < N; i++) {
    if (dec_bv.data[i] != pt_auto[i]) {
      if (diffs < 5) {
        std::cout << "  [" << i << "] expected=" << pt_auto[i]
                  << "  got=" << dec_bv.data[i] << "\n";
      }
      ++diffs;
    }
  }
  require_test(diffs == 0,
               "BV key-switch differs from the plaintext automorphism");
  std::cout << "PASS: BV key-switch matches native automorphism of plaintext\n";
}
