#include "bv_keyswitch.h"
#include "database_constants.h"
#include "logging.h"
#include "utils.h"
#include "hexl/hexl.hpp"
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace bvks {


void signed_gadget_decompose(uint64_t val, size_t base_log2,
                             uint64_t q, uint64_t *out, size_t num_digits,
                             size_t low_bits) {
  const uint64_t half_q = q >> 1;
  const uint64_t base = uint64_t{1} << base_log2;
  const uint64_t mask = base - 1;

  int64_t d = (val > half_q)
      ? static_cast<int64_t>(val) - static_cast<int64_t>(q)
      : static_cast<int64_t>(val);
  if (low_bits) d = (d + (int64_t{1} << (low_bits - 1))) >> low_bits;

  for (size_t i = 0; i < num_digits; ++i) {
    const uint64_t low = static_cast<uint64_t>(d) & mask;
    const int64_t r = low >= base / 2
        ? static_cast<int64_t>(low) - static_cast<int64_t>(base)
        : static_cast<int64_t>(low);
    d -= r;
    d >>= base_log2;
    out[i] = (r >= 0) ? static_cast<uint64_t>(r)
                      : static_cast<uint64_t>(r + static_cast<int64_t>(q));
  }
}

void signed_gadget_decompose_mp(uint128_t val, uint128_t Q, size_t base_log2,
                                int64_t *out, size_t num_digits) {
  using i128 = __int128_t;
  const uint128_t half_Q = Q >> 1;
  const uint64_t  B      = uint64_t(1) << base_log2;
  const uint64_t  half_B = B >> 1;
  const uint64_t  B_mask = B - 1;

  i128 d = (val > half_Q) ? static_cast<i128>(val) - static_cast<i128>(Q)
                          : static_cast<i128>(val);
  for (size_t i = 0; i < num_digits; ++i) {
    const uint64_t low = static_cast<uint64_t>(d) & B_mask;
    int64_t r;
    if (low > half_B) {
      r = static_cast<int64_t>(low) - static_cast<int64_t>(B);
      d = (d >> base_log2) + 1;
    } else {
      r = static_cast<int64_t>(low);
      d >>= base_log2;
    }
    out[i] = r;
  }
}

static inline size_t bv_base_log2(const PirParams &pir_params) {
  static_assert(L_KS > 0 && L_KS <= 16, "BV gadget rows must be in [1,16]");
  const size_t q_bits = pir_params.get_ct_mod_width();
  const size_t bits = (q_bits - low_bits_for_scheme(pir_params)) / L_KS + 1;
  if (bits >= 63) throw std::invalid_argument("BV gadget base exceeds signed digit range");
  return bits;
}

size_t gadget_rows() { return L_KS; }

size_t base_log2_for_scheme(const PirParams &pir_params) {
  return bv_base_log2(pir_params);
}

size_t low_bits_for_scheme(const PirParams &pir_params) {
#ifdef PIR_KS_LOW_BITS
  constexpr size_t low_bits = PIR_KS_LOW_BITS;
#else
  constexpr size_t low_bits = 0;
#endif
  if (low_bits >= pir_params.get_ct_mod_width() ||
      (low_bits && pir_params.K() != 1)) {
    throw std::invalid_argument("BV low-bit rounding requires K=1 and fewer than q bits");
  }
  return low_bits;
}

bool BvGaloisKeys::contains(uint32_t galois_k) const noexcept {
  for (const BvKeySwitchKey &k : keys) {
    if (k.galois_k == galois_k) return true;
  }
  return false;
}

static inline uint64_t power_of_two_mod(size_t exp_bits, uint64_t q) {
  uint64_t result = 1 % q;
  for (size_t b = 0; b < exp_bits; ++b) {
    result = (static_cast<uint128_t>(result) << 1) % q;
  }
  return result;
}

const BvKeySwitchKey &BvGaloisKeys::get(uint32_t galois_k) const {
  for (auto &k : keys) {
    if (k.galois_k == galois_k)
      return k;
  }
  throw std::out_of_range("BvGaloisKeys::get: galois_k not found");
}

static void sample_gaussian_shared_rns(uint64_t *out, size_t N,
                                       const std::vector<uint64_t> &qs,
                                       double sigma, ChaChaRng &rng) {
  std::normal_distribution<double> dist(0.0, sigma);
  std::vector<int64_t> e(N);
  for (size_t i = 0; i < N; ++i) e[i] = std::llround(dist(rng));
  for (size_t k = 0; k < qs.size(); ++k) {
    const uint64_t qk = qs[k];
    uint64_t *out_k = out + k * N;
    for (size_t i = 0; i < N; ++i) {
      const int64_t v = e[i];
      if (v >= 0) {
        out_k[i] = static_cast<uint64_t>(v) % qk;
      } else {
        const uint64_t abs_v = static_cast<uint64_t>(-v) % qk;
        out_k[i] = (abs_v == 0) ? 0 : qk - abs_v;
      }
    }
  }
}

static void build_ksk_row_rns(const std::vector<uint64_t> &msg_ntt,
                              const RlweSk &sk,
                              const std::vector<uint64_t> &qs,
                              double sigma, ChaChaRng &rng,
                              BvRlweCt &ct) {
  constexpr size_t N = DBConsts::PolyDegree;
  const size_t K = qs.size();
  ct.a.assign(N * K, 0);
  ct.b.assign(N * K, 0);

  for (size_t k = 0; k < K; ++k) {
    utils::sample_uniform_poly(ct.a.data() + k * N, N, qs[k], rng);
  }
  std::vector<uint64_t> e(N * K);
  sample_gaussian_shared_rns(e.data(), N, qs, sigma, rng);

  std::vector<uint64_t> as(N);
  for (size_t k = 0; k < K; ++k) {
    const uint64_t qk = qs[k];
    uint64_t *a_k = ct.a.data() + k * N;
    uint64_t *b_k = ct.b.data() + k * N;
    uint64_t *e_k = e.data() + k * N;
    utils::ntt_fwd(a_k, N, qk);
    utils::ntt_fwd(e_k, N, qk);
    intel::hexl::EltwiseMultMod(as.data(), a_k, sk.data.data() + k * N,
                                N, qk, 1);
    intel::hexl::EltwiseSubMod(b_k, msg_ntt.data() + k * N, as.data(), N, qk);
    intel::hexl::EltwiseAddMod(b_k, b_k, e_k, N, qk);
  }
}

BvKeySwitchKey gen_bv_ks_key(const PirParams &pir_params,
                             const RlweSk &sk, uint32_t galois_k,
                             ChaChaRng &rng) {
  const double sigma = pir_params.get_noise_std_dev();
  constexpr size_t N = DBConsts::PolyDegree;
  constexpr size_t K = DBConsts::RnsMods.size();
  static_assert(K <= 2, "MP keyswitch supports K <= 2 only");

  const auto &qs = pir_params.get_rns_mods();

  std::vector<uint64_t> sigma_s(N * K);
  for (size_t k = 0; k < K; ++k) {
    utils::automorphism_ntt(sk.data.data() + k * N, N, galois_k, qs[k],
                            sigma_s.data() + k * N);
  }

  BvKeySwitchKey ksk;
  ksk.galois_k = galois_k;

  const size_t base_log2 = bv_base_log2(pir_params);
  const size_t low_bits = low_bits_for_scheme(pir_params);
  ksk.cts.resize(L_KS);
  std::vector<uint64_t> msg(N * K);
  for (size_t i = 0; i < L_KS; ++i) {
    for (size_t k = 0; k < K; ++k) {
      const uint64_t qk = qs[k];
      const uint64_t Bi = power_of_two_mod(low_bits + i * base_log2, qk);
      intel::hexl::EltwiseFMAMod(msg.data() + k * N, sigma_s.data() + k * N,
                                 Bi, nullptr, N, qk, 1);
    }
    build_ksk_row_rns(msg, sk, qs, sigma, rng, ksk.cts[i]);
  }

  return ksk;
}

BvGaloisKeys gen_bv_galois_keys(const PirParams &pir_params,
                                const RlweSk &sk) {
  BvGaloisKeys result;
  const size_t expan_height = pir_params.get_expan_height();
  constexpr size_t N = DBConsts::PolyDegree;

  ChaChaRng rng;

  result.keys.reserve(expan_height);
  for (size_t i = 0; i < expan_height; ++i) {
    const uint32_t galois_k = static_cast<uint32_t>((N >> i) + 1);
    result.keys.push_back(gen_bv_ks_key(pir_params, sk, galois_k, rng));
  }
  return result;
}

namespace {
struct GaloisScratch {
  std::vector<uint64_t> c0_perm, c1_perm, delta_a, delta_b, tmp;
  std::vector<uint64_t> digits;
  std::vector<int64_t> work;
};
static GaloisScratch g_scratch;
}

static void bv_apply_galois_inplace_k1(RlweCt &ct, uint32_t galois_k,
                                       const BvKeySwitchKey &key,
                                       const PirParams &pir_params) {
  constexpr size_t N = DBConsts::PolyDegree;
  const uint64_t q_val = pir_params.get_rns_mods()[0];
  const size_t base_log2 = bv_base_log2(pir_params);
  const size_t low_bits = low_bits_for_scheme(pir_params);

  GaloisScratch &s = g_scratch;
  if (s.c0_perm.size() != N) {
    s.c0_perm.resize(N);
    s.c1_perm.resize(N);
    s.delta_a.resize(N);
    s.delta_b.resize(N);
    s.tmp.resize(N);
    s.digits.resize(L_KS * N);
    s.work.resize(N);
  }
  uint64_t *const c0_perm = s.c0_perm.data();
  uint64_t *const c1_perm = s.c1_perm.data();
  uint64_t *const delta_b = s.delta_b.data();
  uint64_t *const delta_a = s.delta_a.data();
  uint64_t *const tmp = s.tmp.data();
  uint64_t *const digits = s.digits.data();

  TIME_START(APPLY_GAL_SIGMA);
  utils::automorphism_coeff(ct.data(0), N, galois_k, q_val, c0_perm);
  utils::automorphism_coeff(ct.data(1), N, galois_k, q_val, c1_perm);
  TIME_END(APPLY_GAL_SIGMA);

  TIME_START(APPLY_GAL_DECOMP);
  const int64_t qi = static_cast<int64_t>(q_val);
  const uint64_t base = uint64_t{1} << base_log2;
  const uint64_t mask = base - 1;
  int64_t *const work = s.work.data();
  for (size_t k = 0; k < N; ++k) {
    work[k] = c1_perm[k] > q_val / 2
        ? static_cast<int64_t>(c1_perm[k]) - qi
        : static_cast<int64_t>(c1_perm[k]);
  }
  if (low_bits) {
    const int64_t half_step = int64_t{1} << (low_bits - 1);
    for (size_t k = 0; k < N; ++k) work[k] = (work[k] + half_step) >> low_bits;
  }
  for (size_t i = 0; i < L_KS; ++i) {
    uint64_t *const row = digits + i * N;
    for (size_t k = 0; k < N; ++k) {
      const uint64_t low = static_cast<uint64_t>(work[k]) & mask;
      const int64_t digit = low >= base / 2
          ? static_cast<int64_t>(low) - static_cast<int64_t>(base)
          : static_cast<int64_t>(low);
      work[k] = (work[k] - digit) >> base_log2;
      row[k] = digit < 0 ? static_cast<uint64_t>(digit + qi)
                         : static_cast<uint64_t>(digit);
    }
  }
  TIME_END(APPLY_GAL_DECOMP);

  {
    uint64_t *digit0 = digits;
    TIME_START(APPLY_GAL_NTT_FWD);
    utils::ntt_fwd(digit0, N, q_val);
    TIME_END(APPLY_GAL_NTT_FWD);
    TIME_START(APPLY_GAL_POINTWISE);
    intel::hexl::EltwiseMultMod(delta_b, digit0, key.cts[0].b.data(), N, q_val, 1);
    intel::hexl::EltwiseMultMod(delta_a, digit0, key.cts[0].a.data(), N, q_val, 1);
    TIME_END(APPLY_GAL_POINTWISE);
  }
  for (size_t i = 1; i < L_KS; ++i) {
    uint64_t *digit_i = digits + i * N;
    TIME_START(APPLY_GAL_NTT_FWD);
    utils::ntt_fwd(digit_i, N, q_val);
    TIME_END(APPLY_GAL_NTT_FWD);
    const auto &ksk_ct = key.cts[i];
    TIME_START(APPLY_GAL_POINTWISE);
    intel::hexl::EltwiseMultMod(tmp, digit_i, ksk_ct.b.data(), N, q_val, 1);
    intel::hexl::EltwiseAddMod(delta_b, delta_b, tmp, N, q_val);
    intel::hexl::EltwiseMultMod(tmp, digit_i, ksk_ct.a.data(), N, q_val, 1);
    intel::hexl::EltwiseAddMod(delta_a, delta_a, tmp, N, q_val);
    TIME_END(APPLY_GAL_POINTWISE);
  }

  TIME_START(APPLY_GAL_NTT_INV);
  utils::ntt_inv(delta_b, N, q_val);
  utils::ntt_inv(delta_a, N, q_val);
  TIME_END(APPLY_GAL_NTT_INV);

  intel::hexl::EltwiseAddMod(c0_perm, c0_perm, delta_b, N, q_val);

  std::memcpy(ct.data(0), c0_perm, N * sizeof(uint64_t));
  std::memcpy(ct.data(1), delta_a, N * sizeof(uint64_t));
}

static void bv_apply_galois_inplace_k2(RlweCt &ct, uint32_t galois_k,
                                       const BvKeySwitchKey &key,
                                       const PirParams &pir_params) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = pir_params.get_rns_mods();
  const RnsTables &tables = pir_params.get_rns_tables();
  const uint64_t q0 = qs[0], q1 = qs[1];
  const uint64_t q0_inv_mod_q1 = tables.q0_inv_mod_q1;
  const size_t base_log2 = bv_base_log2(pir_params);
  const uint64_t B_mask = (uint64_t(1) << base_log2) - 1;

  TIME_START(APPLY_GAL_SIGMA);
  std::vector<uint64_t> c0_perm(2 * N), c1_perm(2 * N);
  for (size_t k = 0; k < 2; ++k) {
    utils::automorphism_coeff(ct.data(0) + k * N, N, galois_k, qs[k],
                              c0_perm.data() + k * N);
    utils::automorphism_coeff(ct.data(1) + k * N, N, galois_k, qs[k],
                              c1_perm.data() + k * N);
  }
  TIME_END(APPLY_GAL_SIGMA);

  TIME_START(APPLY_GAL_DECOMP);
  std::vector<uint128_t> c1_mp(N);
  for (size_t j = 0; j < N; ++j) {
    const uint64_t r0 = c1_perm[0 * N + j];
    const uint64_t r1 = c1_perm[1 * N + j];
    const uint64_t r0_mod_q1 = r0 % q1;
    const uint64_t diff = (r1 + q1 - r0_mod_q1) % q1;
    const uint64_t s = static_cast<uint64_t>(
        (static_cast<uint128_t>(diff) * q0_inv_mod_q1) % q1);
    c1_mp[j] = static_cast<uint128_t>(q0) * s + r0;
  }

  const uint128_t Q_total = static_cast<uint128_t>(q0) * q1;
  std::vector<int64_t> sdigits(L_KS * N);
  int64_t buf[16];
  for (size_t j = 0; j < N; ++j) {
    signed_gadget_decompose_mp(c1_mp[j], Q_total, base_log2, buf, L_KS);
    for (size_t i = 0; i < L_KS; ++i) sdigits[i * N + j] = buf[i];
  }
  TIME_END(APPLY_GAL_DECOMP);

  std::vector<uint64_t> delta_a(2 * N, 0), delta_b(2 * N, 0);
  std::vector<uint64_t> digit_buf(N), prod(N);

  auto render_digits_for_limb = [&](size_t i, uint64_t qk) {
    const int64_t *src = sdigits.data() + i * N;
    for (size_t j = 0; j < N; ++j) {
      const int64_t s = src[j];
      digit_buf[j] = (s >= 0) ? static_cast<uint64_t>(s)
                              : qk - static_cast<uint64_t>(-s);
    }
  };

  for (size_t k = 0; k < 2; ++k) {
    const uint64_t qk = qs[k];
    uint64_t *db_k = delta_b.data() + k * N;
    uint64_t *da_k = delta_a.data() + k * N;

    TIME_START(APPLY_GAL_DECOMP);
    render_digits_for_limb(0, qk);
    TIME_END(APPLY_GAL_DECOMP);
    TIME_START(APPLY_GAL_NTT_FWD);
    utils::ntt_fwd(digit_buf.data(), N, qk);
    TIME_END(APPLY_GAL_NTT_FWD);
    TIME_START(APPLY_GAL_POINTWISE);
    intel::hexl::EltwiseMultMod(db_k, digit_buf.data(),
                                key.cts[0].b.data() + k * N, N, qk, 1);
    intel::hexl::EltwiseMultMod(da_k, digit_buf.data(),
                                key.cts[0].a.data() + k * N, N, qk, 1);
    TIME_END(APPLY_GAL_POINTWISE);

    for (size_t i = 1; i < L_KS; ++i) {
      TIME_START(APPLY_GAL_DECOMP);
      render_digits_for_limb(i, qk);
      TIME_END(APPLY_GAL_DECOMP);
      TIME_START(APPLY_GAL_NTT_FWD);
      utils::ntt_fwd(digit_buf.data(), N, qk);
      TIME_END(APPLY_GAL_NTT_FWD);
      TIME_START(APPLY_GAL_POINTWISE);
      intel::hexl::EltwiseMultMod(prod.data(), digit_buf.data(),
                                  key.cts[i].b.data() + k * N, N, qk, 1);
      intel::hexl::EltwiseAddMod(db_k, db_k, prod.data(), N, qk);
      intel::hexl::EltwiseMultMod(prod.data(), digit_buf.data(),
                                  key.cts[i].a.data() + k * N, N, qk, 1);
      intel::hexl::EltwiseAddMod(da_k, da_k, prod.data(), N, qk);
      TIME_END(APPLY_GAL_POINTWISE);
    }

    TIME_START(APPLY_GAL_NTT_INV);
    utils::ntt_inv(db_k, N, qk);
    utils::ntt_inv(da_k, N, qk);
    TIME_END(APPLY_GAL_NTT_INV);

    intel::hexl::EltwiseAddMod(c0_perm.data() + k * N, c0_perm.data() + k * N,
                               db_k, N, qk);
  }

  std::memcpy(ct.data(0), c0_perm.data(), 2 * N * sizeof(uint64_t));
  std::memcpy(ct.data(1), delta_a.data(), 2 * N * sizeof(uint64_t));
}


void bv_apply_galois_inplace(RlweCt &ct, uint32_t galois_k,
                             const BvKeySwitchKey &key,
                             const PirParams &pir_params) {
  assert(key.galois_k == galois_k);
  assert(!ct.ntt_form);

  constexpr size_t K = DBConsts::RnsMods.size();

  if constexpr (K == 1) {
    bv_apply_galois_inplace_k1(ct, galois_k, key, pir_params);
  } else {
    bv_apply_galois_inplace_k2(ct, galois_k, key, pir_params);
  }
}

}
