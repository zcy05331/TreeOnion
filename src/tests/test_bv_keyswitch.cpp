#include "tests.h"
#include "bv_keyswitch.h"
#include "rlwe.h"
#include "utils.h"
#include <random>
#include <array>
#include <string>
#include <vector>

void test_bv_keyswitch_reference();

static void test_rounded_bv_digits() {
  PirParams scheme;
  const uint64_t q = scheme.get_rns_mods()[0];
  for (size_t rows : {size_t{2}, size_t{3}}) {
    for (size_t drop : {size_t{0}, size_t{16}, size_t{24}}) {
      const size_t bits = (scheme.get_ct_mod_width() - drop) / rows + 1;
      const uint64_t half = drop ? uint64_t{1} << (drop - 1) : 0;
      ChaChaRng rng(0x627664726f70ULL);
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

  ChaChaRng rng(42);
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

  ChaChaRng rng(0x62766b73ULL);
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

// ============================================================================
// Unit test: the vectorized K=1 key switch is bit-exact against the scalar
// primitive.
//
// bv_apply_galois_inplace_k1 re-implements the digit extraction of
// bvks::signed_gadget_decompose as one loop per digit over all coefficients
// (bv_keyswitch.cpp, APPLY_GAL_DECOMP) so the inner loop vectorizes.
// Decryption cannot tell the two apart: a wrong least-significant digit
// (~B * sqrt(N) of phase), a dropped +half_step rounding, a flipped tie rule
// or a reversed row order all stay inside the ordinary key-switch noise and
// far below q/(2t). This test rebuilds the key switch from the public pieces
// (automorphism, scalar decomposition, NTT, the KSK rows) and requires the
// production ciphertext to equal it exactly, for three automorphisms.
// ============================================================================
void test_bv_keyswitch_reference() {
  print_func_name(__FUNCTION__);
  PirParams params;
  require_applicable(params.K() == 1,
                     "the vectorized BV key switch under test is the K=1 body");
  constexpr size_t N = DBConsts::PolyDegree;
  const uint64_t q = params.get_rns_mods()[0];
  const uint64_t t = params.get_plain_mod();
  const std::vector<uint64_t> qs{q};
  const size_t base_log2 = bvks::base_log2_for_scheme(params);
  const size_t low_bits = bvks::low_bits_for_scheme(params);
  const uint64_t B = uint64_t{1} << base_log2;

  ChaChaRng rng(0x62766b73ULL);
  const RlweSk sk = gen_secret_key_rns(N, qs, rng);
  std::vector<uint64_t> pt(N);
  for (size_t i = 0; i < N; ++i) pt[i] = (3 * i + 1) % t;

  // c1 coefficients on the decomposition's decision boundaries: the centering
  // split at q/2, the balanced-digit tie at B/2 in both signs, and (in a
  // PIR_KS_LOW_BITS build) a value exactly half a rounding step above a
  // multiple of 2^low_bits. The remaining coefficients are the encryption's
  // uniform randomness.
  const uint64_t probes[8] = {
      0, 1, q - 1, q / 2, q / 2 + 1, B / 2, q - B / 2,
      low_bits ? (uint64_t{1} << low_bits) + (uint64_t{1} << (low_bits - 1))
               : uint64_t{2}};

  auto mul_mod = [q](uint64_t a, uint64_t b) {
    return static_cast<uint64_t>(static_cast<uint128_t>(a) * b % q);
  };

  const uint32_t galois_ks[3] = {static_cast<uint32_t>(N + 1),
                                 static_cast<uint32_t>(N / 2 + 1), 5u};
  for (const uint32_t galois_k : galois_ks) {
    const bvks::BvKeySwitchKey ksk =
        bvks::gen_bv_ks_key(params, sk, galois_k, rng);
    RlweCt ct;
    encrypt_bfv_rns(pt, sk, N, qs, t, params.get_noise_std_dev(), rng, ct);
    for (size_t i = 0; i < 8; ++i) ct.c1[i] = probes[i];
    const RlweCt ref = ct;

    // Reference: sigma_k on both polynomials; scalar signed decomposition of
    // sigma_k(c1) with digit i feeding KSK row i (the row encrypting
    // sigma_k(s) * 2^low_bits * B^i); pointwise MAC in the NTT domain; then
    // c0' = sigma_k(c0) + sum_i d_i * b_i and c1' = sum_i d_i * a_i.
    std::vector<uint64_t> c0p(N), c1p(N);
    utils::automorphism_coeff(ref.c0.data(), N, galois_k, q, c0p.data());
    utils::automorphism_coeff(ref.c1.data(), N, galois_k, q, c1p.data());
    std::vector<std::vector<uint64_t>> rows(bvks::L_KS,
                                            std::vector<uint64_t>(N));
    uint64_t digits[bvks::L_KS];
    for (size_t j = 0; j < N; ++j) {
      bvks::signed_gadget_decompose(c1p[j], base_log2, q, digits, bvks::L_KS,
                                    low_bits);
      for (size_t i = 0; i < bvks::L_KS; ++i) rows[i][j] = digits[i];
    }
    std::vector<uint64_t> acc_b(N, 0), acc_a(N, 0);
    for (size_t i = 0; i < bvks::L_KS; ++i) {
      utils::ntt_fwd(rows[i].data(), N, q);
      const std::vector<uint64_t> &b = ksk.cts[i].b;
      const std::vector<uint64_t> &a = ksk.cts[i].a;
      for (size_t j = 0; j < N; ++j) {
        acc_b[j] = (acc_b[j] + mul_mod(rows[i][j], b[j])) % q;
        acc_a[j] = (acc_a[j] + mul_mod(rows[i][j], a[j])) % q;
      }
    }
    utils::ntt_inv(acc_b.data(), N, q);
    utils::ntt_inv(acc_a.data(), N, q);
    std::vector<uint64_t> expected_c0(N);
    for (size_t j = 0; j < N; ++j) expected_c0[j] = (c0p[j] + acc_b[j]) % q;

    bvks::bv_apply_galois_inplace(ct, galois_k, ksk, params);
    const std::string where = " (galois_k=" + std::to_string(galois_k) + ")";
    require_test(!ct.ntt_form,
                 "BV key switch must return a coefficient-form ciphertext" + where);
    require_test(ct.c0 == expected_c0,
                 "vectorized BV key switch c0 differs from the scalar-primitive "
                 "reference" + where);
    require_test(ct.c1 == acc_a,
                 "vectorized BV key switch c1 differs from the scalar-primitive "
                 "reference" + where);
  }
  std::cout << "PASS: vectorized BV key switch is bit-exact against the scalar "
               "reference for galois_k in {" << N + 1 << ", " << N / 2 + 1
            << ", 5} (L_KS=" << bvks::L_KS << ", base_log2=" << base_log2
            << ", low_bits=" << low_bits << ")\n";
}
