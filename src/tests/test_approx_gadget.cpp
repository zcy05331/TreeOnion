#include "tests.h"
#include "bv_keyswitch.h"
#include "rlwe.h"

#include <limits>
#include <random>
#include <string>
#include <vector>

void test_approx_gadget();
void test_gsw_decomp_shapes();

namespace {

int64_t center_mod(uint64_t value, uint64_t q) {
  const uint64_t half_q = q >> 1;
  const int64_t qi = static_cast<int64_t>(q);
  const int64_t vi = static_cast<int64_t>(value % q);
  return (value % q > half_q) ? vi - qi : vi;
}

uint64_t encode_signed(int64_t value, uint64_t q) {
  if (value >= 0) return static_cast<uint64_t>(value) % q;
  const uint64_t abs_v = static_cast<uint64_t>(-value) % q;
  return abs_v == 0 ? 0 : q - abs_v;
}

uint128_t signed_abs128(__int128 value) {
  return value < 0 ? static_cast<uint128_t>(-value)
                   : static_cast<uint128_t>(value);
}

size_t approx_base_log2(size_t qbits, size_t ell, size_t low_bits) {
  return (qbits - low_bits) / ell + 1;
}

__int128 reconstruct_component(
    const std::vector<std::vector<uint64_t>> &rows, size_t base_row,
    size_t ell, size_t base_log2, size_t low_bits, size_t coeff, uint64_t q) {
  __int128 accum = 0;
  for (size_t row = 0; row < ell; ++row) {
    const int64_t digit = center_mod(rows[base_row + row][coeff], q);
    const size_t power = low_bits + (ell - 1 - row) * base_log2;
    accum += static_cast<__int128>(digit) *
             (static_cast<__int128>(1) << power);
  }
  return accum;
}

void check_decomposition_shape(size_t ell, size_t low_bits) {
  PirParams params;
  require_applicable(params.K() == 1,
                     "approximate data GSW decomposition is a K=1 fixture");
  const size_t base_log2 =
      approx_base_log2(params.get_ct_mod_width(), ell, low_bits);
  GSWEval eval(params, ell, base_log2, low_bits);

  constexpr size_t N = DBConsts::PolyDegree;
  const uint64_t q = params.get_rns_mods()[0];
  RlweCt ct;
  ct.resize(N);
  std::mt19937_64 rng(0x6469676974626f75ULL);
  for (size_t i = 0; i < N; ++i) {
    ct.c0[i] = rng() % q;
    ct.c1[i] = rng() % q;
  }
  const int64_t half_step = low_bits ? int64_t{1} << (low_bits - 1) : 0;

  std::vector<int64_t> probes = {
      0,
      1,
      -1,
      static_cast<int64_t>(q / 2),
      -static_cast<int64_t>(q / 2),
      (int64_t{1} << low_bits) - 1,
      int64_t{1} << low_bits,
      (int64_t{1} << low_bits) + half_step - 1,
      (int64_t{1} << low_bits) + half_step,
      -((int64_t{1} << low_bits) + half_step),
  };
  for (size_t i = 0; i < probes.size(); ++i) {
    ct.c0[i] = encode_signed(probes[i], q);
    ct.c1[i] = encode_signed(-probes[i], q);
  }
  ct.c0[probes.size()] = q - 1;
  ct.c1[probes.size()] = q / 2 + 1;

  std::vector<std::vector<uint64_t>> rows(2 * ell,
                                          std::vector<uint64_t>(N, 0));
  eval.decomp_rlwe_single_mod(ct, rows);

  const int64_t digit_bound = int64_t{1} << (base_log2 - 1);
  for (const auto &row : rows) {
    for (size_t i = 0; i < N; ++i) {
      const int64_t digit = center_mod(row[i], q);
      require_test(digit >= -digit_bound && digit < digit_bound,
                   "approximate GSW digit stays balanced");
    }
  }

  const uint128_t max_error = static_cast<uint128_t>(half_step);
  for (size_t i = 0; i < N; ++i) {
    const __int128 c0_recon =
        reconstruct_component(rows, 0, ell, base_log2, low_bits, i, q);
    const __int128 c1_recon =
        reconstruct_component(rows, ell, ell, base_log2, low_bits, i, q);
    const __int128 c0_target = center_mod(ct.c0[i], q);
    const __int128 c1_target = center_mod(ct.c1[i], q);
    require_test(signed_abs128(c0_recon - c0_target) <= max_error,
                 "component 0 reconstruction is within half low-bit unit");
    require_test(signed_abs128(c1_recon - c1_target) <= max_error,
                 "component 1 reconstruction is within half low-bit unit");
  }
}

void check_gadget_table(size_t ell, size_t base_log2, size_t low_bits) {
  PirParams params;
  const uint64_t q = params.get_rns_mods()[0];
  const auto gadget = utils::gsw_gadget(
      ell, base_log2, std::vector<uint64_t>{q}, low_bits);
  for (size_t row = 0; row < ell; ++row) {
    const size_t power = low_bits + (ell - 1 - row) * base_log2;
    uint64_t expected = 1 % q;
    for (size_t bit = 0; bit < power; ++bit) {
      expected = static_cast<uint64_t>((static_cast<uint128_t>(expected) << 1) % q);
    }
    require_test(gadget[0][row] == expected,
                 "approximate gadget table uses low-bit shifted MSB rows");
  }
}

void check_ext_product(size_t ell, size_t low_bits) {
  PirParams params;
  require_applicable(params.K() == 1,
                     "approximate data GSW external-product test needs K=1");
  constexpr size_t N = DBConsts::PolyDegree;
  const uint64_t q = params.get_rns_mods()[0];
  const uint64_t t = params.get_plain_mod();
  const size_t base_log2 =
      approx_base_log2(params.get_ct_mod_width(), ell, low_bits);
  GSWEval eval(params, ell, base_log2, low_bits);

  std::mt19937_64 rng(0x415050524f584750ULL + ell);
  RlweSk sk = gen_secret_key(N, q, rng);
  std::vector<uint64_t> one(N, 0), zero(N, 0), msg(N, 0);
  one[0] = 1;
  for (size_t i = 0; i < N; ++i) {
    msg[i] = rng() % t;
  }

  GSWCt one_gsw = eval.plain_to_gsw(one, sk, rng);
  GSWCt zero_gsw = eval.plain_to_gsw(zero, sk, rng);
  RlweCt msg_ct;
  encrypt_bfv(msg, sk, N, q, t, params.get_noise_std_dev(), rng, msg_ct);

  RlweCt out;
  out.resize(N);
  eval.external_product(one_gsw, msg_ct, out);
  out.ntt_form = true;
  rlwe_ntt_inv_inplace(out, q, N);
  RlwePt pt;
  decrypt(out, sk, N, q, t, pt);
  for (size_t i = 0; i < N; ++i) {
    require_test(pt.data[i] == msg[i],
                 "approximate RGSW(1) preserves BFV plaintext");
  }

  eval.external_product(zero_gsw, msg_ct, out);
  out.ntt_form = true;
  rlwe_ntt_inv_inplace(out, q, N);
  decrypt(out, sk, N, q, t, pt);
  for (size_t i = 0; i < N; ++i) {
    require_test(pt.data[i] == 0,
                 "approximate RGSW(0) clears BFV plaintext");
  }
}


// One gadget shape this build ships. decomp_rlwe_single_mod must reconstruct
// every coefficient exactly (to within half a rounding step when low_bits > 0),
// keep every digit balanced, and agree digit for digit with
// bvks::signed_gadget_decompose, the scalar primitive it claims to mirror
// (gsw.cpp: "The math mirrors the scalar primitive exactly"). External
// products cannot see a wrong low digit (B <= 2^10 against a decode threshold
// near 2^45), so only this direct comparison catches one.
void check_shipped_shape(const PirParams &params, size_t ell,
                         size_t base_log2, size_t low_bits) {
  constexpr size_t N = DBConsts::PolyDegree;
  const uint64_t q = params.get_rns_mods()[0];
  GSWEval eval(params, ell, base_log2, low_bits);

  RlweCt ct;
  ct.resize(N);
  std::mt19937_64 rng(0x6469676974ULL);
  for (size_t i = 0; i < N; ++i) {
    ct.c0[i] = rng() % q;
    ct.c1[i] = rng() % q;
  }
  // Boundary coefficients written into c0 and negated into c1: the centering
  // split at q/2 and the balanced-digit tie at B/2 in both signs, plus
  // (B/2) * B^(ell-1) mod q, whose top digit sits on the tie.
  const uint64_t B = uint64_t{1} << base_log2;
  uint64_t top = (B / 2) % q;
  for (size_t r = 1; r < ell; ++r) {
    top = static_cast<uint64_t>(static_cast<uint128_t>(top) * B % q);
  }
  const uint64_t probes[10] = {0, 1, q - 1, q / 2, q / 2 + 1, B / 2, B / 2 - 1,
                               q - B / 2, top, q / 2 - 1};
  for (size_t i = 0; i < 10; ++i) {
    ct.c0[i] = probes[i];
    ct.c1[i] = probes[i] ? q - probes[i] : 0;
  }

  std::vector<std::vector<uint64_t>> rows(2 * ell,
                                          std::vector<uint64_t>(N, 0));
  eval.decomp_rlwe_single_mod(ct, rows);

  const std::string shape = " (ell=" + std::to_string(ell) + ", base_log2=" +
                            std::to_string(base_log2) + ", low_bits=" +
                            std::to_string(low_bits) + ")";
  const int64_t digit_bound = int64_t{1} << (base_log2 - 1);
  const __int128 max_error =
      low_bits ? (static_cast<__int128>(1) << (low_bits - 1)) : 0;
  std::vector<uint64_t> scalar(ell);
  for (size_t half = 0; half < 2; ++half) {
    const std::vector<uint64_t> &poly = half == 0 ? ct.c0 : ct.c1;
    for (size_t j = 0; j < N; ++j) {
      const __int128 recon = reconstruct_component(
          rows, half * ell, ell, base_log2, low_bits, j, q);
      const __int128 diff = recon - center_mod(poly[j], q);
      require_test(diff >= -max_error && diff <= max_error,
                   "shipped gadget shape does not reconstruct the coefficient" +
                       shape);
      for (size_t r = 0; r < ell; ++r) {
        const int64_t digit = center_mod(rows[half * ell + r][j], q);
        require_test(digit >= -digit_bound && digit < digit_bound,
                     "shipped gadget shape emits an unbalanced digit" + shape);
      }
      // The scalar primitive is LSB-first; the rows are stored MSB-first.
      bvks::signed_gadget_decompose(poly[j], base_log2, q, scalar.data(), ell,
                                    low_bits);
      for (size_t i = 0; i < ell; ++i) {
        require_test(rows[half * ell + (ell - 1 - i)][j] == scalar[i],
                     "vectorized decomp_rlwe_single_mod digit differs from "
                     "bvks::signed_gadget_decompose" + shape);
      }
    }
  }
}

}  // namespace

void test_approx_gadget() {
  print_func_name(__FUNCTION__);
  for (size_t ell : {size_t{2}, size_t{3}}) {
    for (size_t drop : {size_t{0}, size_t{8}, size_t{16}, size_t{24},
                        size_t{30}, size_t{32}}) {
      check_decomposition_shape(ell, drop);
      check_gadget_table(ell,
          approx_base_log2(PirParams().get_ct_mod_width(), ell, drop), drop);
      check_ext_product(ell, drop);
    }
  }
  PRINT_BAR;
}

// The data-selector gadget (L_EP rows, ep_low_bits) and the RGSW(s)
// completion-key gadget (L_KEY rows) as PirParams derives them for this build:
// the shapes every K=1 external product and query_to_gsw actually run, which
// test_approx_gadget's synthetic ell in {2,3} shapes do not reach.
void test_gsw_decomp_shapes() {
  print_func_name(__FUNCTION__);
  PirParams params;
  require_applicable(params.K() == 1,
                     "decomp_rlwe_single_mod is the K=1 gadget path");
  check_shipped_shape(params, params.get_l(), params.get_base_log2(),
                      params.get_ep_low_bits());
  check_shipped_shape(params, params.get_l_key(), params.get_base_log2_key(),
                      0);
  std::cout << "PASS: decomp_rlwe_single_mod matches bvks::signed_gadget_decompose "
               "digit for digit at L_EP=" << params.get_l() << "/base 2^"
            << params.get_base_log2() << " (ep_low_bits="
            << params.get_ep_low_bits() << ") and L_KEY=" << params.get_l_key()
            << "/base 2^" << params.get_base_log2_key() << "\n";
  PRINT_BAR;
}
