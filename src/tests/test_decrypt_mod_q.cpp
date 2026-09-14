#include "tests.h"
#include "rlwe.h"

#include <sstream>

// decrypt_mod_q is the client's small-q response decoder: it always works
// under small_q, so its input must be a modulus-switched ciphertext. This
// test runs the exact response path a real query produces after the last
// homomorphic operation: encrypt under full q, switch to small_q, serialize
// with the wire codec, load, decode. A dense plaintext with both extreme
// values makes any rounding or codec truncation visible.
void PirTest::test_decrypt_mod_q() {
  print_func_name(__FUNCTION__);
  PirParams pir_params;
  PirClient client(pir_params);
  PirServer server(pir_params);

  const size_t coeff_count = DBConsts::PolyDegree;
  const std::vector<uint64_t> qs(pir_params.get_rns_mods().begin(),
                                 pir_params.get_rns_mods().end());
  const uint64_t t = pir_params.get_plain_mod();
  const uint64_t small_q = pir_params.get_small_q();
  const double sigma = pir_params.get_noise_std_dev();
  std::mt19937_64 rng(0x6d6f6471ULL);

  std::vector<uint64_t> a(coeff_count, 0);
  for (size_t i = 0; i < coeff_count; ++i) a[i] = (7 * i + 1) % t;
  a[0] = 0;
  a[coeff_count - 1] = t - 1;

  RlweCt rlwe_ct;
  encrypt_bfv_rns(a, client.rlwe_sk_, coeff_count, qs, t, sigma, rng, rlwe_ct);
  server.mod_switch_inplace(rlwe_ct, small_q);

  // Direct decode of the switched ciphertext.
  const RlwePt direct = client.decrypt_mod_q(rlwe_ct);
  require_test(direct.data == a,
               "decrypt_mod_q must recover every coefficient after the switch");

  // Wire round trip: the codec writes small_q_width bits per coefficient and
  // the loader must hand back the identical single-limb ciphertext.
  std::stringstream wire;
  const size_t bytes = server.save_resp_to_stream(rlwe_ct, wire);
  const size_t small_q_bits = std::bit_width(small_q - 1);
  require_test(bytes == utils::roundup_div(2 * coeff_count * small_q_bits,
                                           size_t{8}),
               "response codec byte count");
  const RlweCt loaded = client.load_resp_from_stream(wire);
  require_test(loaded.c0 == rlwe_ct.c0 && loaded.c1 == rlwe_ct.c1,
               "response codec round trip altered the ciphertext");
  const RlwePt decoded = client.decrypt_mod_q(loaded);
  require_test(decoded.data == a,
               "decrypt_mod_q must recover the plaintext from the wire form");
  BENCH_PRINT("decoded " << coeff_count << " coefficients from " << bytes
              << " wire bytes");

  // Gauge semantics behind every reported noise margin: decrypt_mod_q_measured
  // feeds test_pir's canary, noise_budget the tree tests' min_budget_bits /
  // max_noise JSON fields. With c1 = 0 the phase is c0 whatever the secret
  // key, so each coefficient's residue is known exactly. The gauge must report
  // the largest centered residue |phase - round(q*m/t)| including the negative
  // branch (e = -3 at m = 0 wraps c0 to q - 3) and the m = t-1 edge, decode
  // every coefficient, and turn max_noise into floor(log2(q / (2*t*max))).
  // Comparing against floor(q/t)*m instead (the past bug the decoder comment
  // records) would report a residue near m*(q mod t)/t ~ 1000 at m = t-1.
  {
    std::vector<uint64_t> m(coeff_count);
    for (size_t i = 0; i < coeff_count; ++i) m[i] = (5 * i) % t;
    m[0] = 0;
    m[1] = t - 1;
    m[2] = 0;
    std::vector<int64_t> e(coeff_count, 0);
    e[0] = 7;
    e[1] = -7;
    e[2] = -3;
    constexpr uint64_t kMaxResidue = 7;
    auto build = [&](uint64_t q, size_t limbs) {
      RlweCt ct;
      ct.resize(limbs * coeff_count);  // c1 = 0: the secret key cancels
      ct.ntt_form = false;
      for (size_t i = 0; i < coeff_count; ++i) {
        const uint64_t lift =
            utils::round_div_u128(static_cast<uint128_t>(q) * m[i], t) % q;
        ct.c0[i] = e[i] >= 0
                       ? (lift + static_cast<uint64_t>(e[i])) % q
                       : (lift + q - static_cast<uint64_t>(-e[i])) % q;
      }
      return ct;
    };
    // budget == floor(log2(q / (2*t*max))) as an integer inequality.
    auto budget_matches = [&](int budget, uint64_t q) {
      if (budget < 0) return false;
      const uint128_t unit = static_cast<uint128_t>(2) * t * kMaxResidue;
      return (static_cast<uint128_t>(1) << budget) * unit <= q &&
             q < (static_cast<uint128_t>(1) << (budget + 1)) * unit;
    };

    const RlweCt switched = build(small_q, 1);
    uint64_t max_noise = 0;
    int budget = 0;
    const RlwePt gauged =
        client.decrypt_mod_q_measured(switched, max_noise, budget);
    require_test(gauged.data == m,
                 "decrypt_mod_q_measured must decode every coefficient of the "
                 "hand-built ciphertext");
    require_test(max_noise == kMaxResidue,
                 "gauge must report the largest centered residue 7, got " +
                     std::to_string(max_noise));
    require_test(budget_matches(budget, small_q),
                 "budget must be floor(log2(small_q / (2*t*max_noise))), got " +
                     std::to_string(budget));

    // The same residues under the full first limb, the debug gauge the tree
    // tests read as min_budget_bits.
    const uint64_t q0 = qs[0];
    const RlweCt full = build(q0, qs.size());
    const int full_budget = client.noise_budget(full);
    require_test(budget_matches(full_budget, q0),
                 "noise_budget must be floor(log2(q0 / (2*t*7))), got " +
                     std::to_string(full_budget));
    BENCH_PRINT("gauge: max residue " << kMaxResidue << " -> " << budget
                << " bits under small_q, " << full_budget << " bits under q0");
  }
}
