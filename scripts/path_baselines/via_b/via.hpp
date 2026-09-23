// VIA-B (Liu, Wang and Zhang, "VIA: Communication-Efficient Single-Server
// Private Information Retrieval", ePrint 2025/2074): VIA-C answers per query
// (LWE-to-RLWE query decompression, RLWE-to-RGSW, DMux, modulus switching,
// first dimension, CMux, CRot) and the MLWEs-to-RLWE homomorphic repacking
// that packs several answers into one ring-switched response ciphertext.
// Written from the paper: the authors' repository (owniai/VIA) has no VIA-B
// and its VIA-C does not produce correct answers (see README.md).
//
// Conventions follow the paper: RLWE_S(M) = (A, B) with B = A*S + E + M,
// RGSW_S(M) = (RLev_S(-S*M), RLev_S(M)), MLWE ranks times degrees equal n1.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "hexl/hexl.hpp"
#if defined(__AVX512IFMA__)
#include <immintrin.h>
#endif

namespace via {

using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;
using u128 = unsigned __int128;
using i128 = __int128;
using Vec = intel::hexl::AlignedVector64<u64>;

// ------------------------------------------------------------------ parameters
// Moduli, degrees, p and gadget lengths are the paper's VIA-C/VIA-B set
// (Section 5.1, Appendix B, Table 6); the gadget bases are the powers of two of
// the authors' implementation. README.md lists where and why this deviates.
constexpr size_t kN1 = 2048, kN2 = 512, kLogN1 = 11;
constexpr u64 kQ11 = 137438822401ULL, kQ12 = 274810798081ULL;  // q1 = q11 * q12 < 2^75
constexpr u64 kQ2 = 17175674881ULL;                            // < 2^34
constexpr u64 kQ3 = 8380417ULL;                                // < 2^23
constexpr unsigned kLogQ4 = 12, kLogP = 4;
constexpr u64 kQ4 = u64{1} << kLogQ4, kP = u64{1} << kLogP;
constexpr double kSigmaE1 = 1024.0, kSigmaS2 = 26.0, kSigmaE2 = 26.0;  // S1 is ternary

constexpr size_t kRecordBytes = 32;
constexpr size_t kRecordCoeffs = 2 * kRecordBytes;             // n3: one 4-bit coefficient per nibble
constexpr size_t kRecordsPerPoly = kN1 / kRecordCoeffs;        // 32 records interleaved per polynomial
constexpr size_t kRotBits = 5;                                 // log2(kRecordsPerPoly)
constexpr size_t kAnswersPerPacked = kN2 / kRecordCoeffs;      // 8 answers per ring-switched ciphertext
constexpr size_t kPackLeaves = kN1 / kRecordCoeffs;            // MLWE leaves of one repacking

struct Gadget {
  size_t len;
  unsigned log_base;
};
constexpr Gadget kCtrl{2, 16};  // DMux control bits, approximate, over q1
constexpr Gadget kSel{2, 7};    // CMux select and CRot bits, approximate, over q2 after switching
constexpr Gadget kConv{15, 5};  // conversion and RLWE-to-RGSW keys, exact, over q1
constexpr Gadget kRsk{8, 3};    // ring-switching key, exact, over q3
// Conversion-key components whose gadget value survives the q1 -> q2 switch:
// 2^(5u) * q2/q1 is about 2^(5u-41), so u = 9..14 give 16 .. 2^29.
constexpr size_t kRepackFirst = 9, kRepackLen = 6;
constexpr size_t kRepackFirstLevel = 6;  // repacking starts from rank-32 degree-64 MLWE

// ------------------------------------------------------------------ PRG
// xoshiro256** seeded through splitmix64; (seed, stream) names an expandable mask.
struct Prg {
  u64 s[4];
  Prg(u64 seed, u64 stream) {
    u64 x = seed ^ (0xd1b54a32d192ed03ULL * (stream + 1));
    for (auto &w : s) w = splitmix(x);
  }
  static u64 splitmix(u64 &x) {
    u64 z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  static u64 rotl(u64 x, int k) { return (x << k) | (x >> (64 - k)); }
  u64 next() {
    const u64 r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return r;
  }
  u64 uniform(u64 q) {
    const u64 mask = (u64{1} << (64 - __builtin_clzll(q - 1))) - 1;
    for (;;) {
      const u64 v = next() & mask;
      if (v < q) return v;
    }
  }
  i64 ternary() {
    for (;;) {
      const u64 v = next() & 3;
      if (v < 3) return static_cast<i64>(v) - 1;
    }
  }
  i64 gaussian(double sigma) {
    const double u1 = static_cast<double>((next() >> 11) + 1) * 0x1.0p-53;
    const double u2 = static_cast<double>(next() >> 11) * 0x1.0p-53;
    return static_cast<i64>(std::llround(sigma * std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2)));
  }
};

// ------------------------------------------------------------------ rings
inline u64 pow_mod(u64 b, u64 e, u64 q) {
  u64 r = 1;
  b %= q;
  for (; e; e >>= 1) {
    if (e & 1) r = static_cast<u64>(u128(r) * b % q);
    b = static_cast<u64>(u128(b) * b % q);
  }
  return r;
}

inline u64 mul_shoup(u64 x, u64 w, u64 w_shoup, u64 q) {
  const u64 hi = static_cast<u64>((u128(x) * w_shoup) >> 64);
  const u64 r = x * w - hi * q;
  return r >= q ? r - q : r;
}

// Z_Q[X]/(X^n + 1) with Q one prime or the product of two (RNS). A polynomial
// is k * n words, residue r at [r * n, (r + 1) * n).
struct Ring {
  size_t n = 0, k = 0;
  u64 q[2] = {0, 0};
  std::unique_ptr<intel::hexl::NTT> ntt[2];
  u128 Q = 0;
  double Qd = 0;
  u64 ginv = 0, ginv_shoup = 0;  // q[0]^-1 mod q[1] (Garner)
  u64 c52[2] = {0, 0};           // 2^52 mod q[r], for the IFMA accumulators

  Ring(size_t degree, u64 q0, u64 q1 = 0) : n(degree), k(q1 ? 2 : 1) {
    q[0] = q0;
    q[1] = q1;
    for (size_t r = 0; r < k; ++r) {
      ntt[r] = std::make_unique<intel::hexl::NTT>(n, q[r]);
      c52[r] = static_cast<u64>((u128{1} << 52) % q[r]);
    }
    Q = k == 2 ? u128(q0) * q1 : u128(q0);
    Qd = static_cast<double>(Q);
    if (k == 2) {
      if (q0 >= q1) throw std::invalid_argument("Garner lift expects q[0] < q[1]");
      ginv = pow_mod(q0 % q1, q1 - 2, q1);
      ginv_shoup = static_cast<u64>((u128(ginv) << 64) / q1);
    }
  }
  size_t words() const { return n * k; }
  // Garner: the representative r0 + q0 * t in [0, Q) together with t.
  u128 lift(u64 r0, u64 r1, u64 &t) const {
    const u64 d = r1 >= r0 ? r1 - r0 : r1 + q[1] - r0;
    t = mul_shoup(d, ginv, ginv_shoup, q[1]);
    return u128(r0) + u128(q[0]) * t;
  }
  i128 centered(const u64 *x, size_t j) const {
    if (k == 1) {
      const u64 v = x[j];
      return v > q[0] / 2 ? i128(v) - i128(q[0]) : i128(v);
    }
    u64 t;
    const u128 v = lift(x[j], x[n + j], t);
    return v > Q / 2 ? i128(v) - i128(Q) : i128(v);
  }
  double centered_double(const u64 *x, size_t j) const {
    if (k == 1) {
      const u64 v = x[j];
      return v > q[0] / 2 ? static_cast<double>(v) - static_cast<double>(q[0]) : static_cast<double>(v);
    }
    u64 t;
    lift(x[j], x[n + j], t);
    const double v = static_cast<double>(x[j]) + static_cast<double>(q[0]) * static_cast<double>(t);
    return v > Qd / 2 ? v - Qd : v;
  }
  void set_signed(u64 *x, size_t j, i128 v) const {
    for (size_t r = 0; r < k; ++r) {
      i128 m = v % i128(q[r]);
      if (m < 0) m += q[r];
      x[r * n + j] = static_cast<u64>(m);
    }
  }
};

inline u64 small_mod(i64 v, u64 q) { return v < 0 ? static_cast<u64>(v + static_cast<i64>(q)) : static_cast<u64>(v); }

inline void fwd(const Ring &R, u64 *x) {
  for (size_t r = 0; r < R.k; ++r) R.ntt[r]->ComputeForward(x + r * R.n, x + r * R.n, 1, 1);
}
inline void inv(const Ring &R, u64 *x) {
  for (size_t r = 0; r < R.k; ++r) R.ntt[r]->ComputeInverse(x + r * R.n, x + r * R.n, 1, 1);
}
inline void add(const Ring &R, u64 *o, const u64 *a, const u64 *b) {
  for (size_t r = 0; r < R.k; ++r) intel::hexl::EltwiseAddMod(o + r * R.n, a + r * R.n, b + r * R.n, R.n, R.q[r]);
}
inline void sub(const Ring &R, u64 *o, const u64 *a, const u64 *b) {
  for (size_t r = 0; r < R.k; ++r) intel::hexl::EltwiseSubMod(o + r * R.n, a + r * R.n, b + r * R.n, R.n, R.q[r]);
}
inline void mul(const Ring &R, u64 *o, const u64 *a, const u64 *b) {
  for (size_t r = 0; r < R.k; ++r) intel::hexl::EltwiseMultMod(o + r * R.n, a + r * R.n, b + r * R.n, R.n, R.q[r], 1);
}
inline void neg(const Ring &R, u64 *o, const u64 *a) {
  for (size_t r = 0; r < R.k; ++r)
    for (size_t j = 0; j < R.n; ++j) {
      const u64 v = a[r * R.n + j];
      o[r * R.n + j] = v ? R.q[r] - v : 0;
    }
}
inline u64 neg_mod(u64 v, u64 q) { return v ? q - v : 0; }

// oa[j] = sum_l xs[l][j] * ya[l][j] and ob[j] = sum_l xs[l][j] * yb[l][j] mod q,
// one pass over xs; operands below q < 2^50, L <= 2048.
inline void dot2(u64 *oa, u64 *ob, const u64 *const *xs, const u64 *const *ya, const u64 *const *yb, size_t L, size_t n,
                 u64 q, u64 c52) {
#if defined(__AVX512IFMA__)
  const __m512i vc = _mm512_set1_epi64(static_cast<long long>(c52)), zero = _mm512_setzero_si512();
  // lo + hi * 2^52 with hi < L * 2^48: fold hi * (2^52 mod q) back under 2^64.
  auto fold = [&](__m512i lo, __m512i hi) {
    const __m512i tlo = _mm512_madd52lo_epu64(zero, hi, vc), thi = _mm512_madd52hi_epu64(zero, hi, vc);
    return _mm512_add_epi64(_mm512_add_epi64(lo, tlo), _mm512_mullo_epi64(thi, vc));
  };
  for (size_t j = 0; j < n; j += 8) {
    __m512i la = zero, ha = zero, lb = zero, hb = zero;
    for (size_t l = 0; l < L; ++l) {
      const __m512i x = _mm512_load_si512(xs[l] + j);
      const __m512i a = _mm512_load_si512(ya[l] + j), b = _mm512_load_si512(yb[l] + j);
      la = _mm512_madd52lo_epu64(la, x, a);
      ha = _mm512_madd52hi_epu64(ha, x, a);
      lb = _mm512_madd52lo_epu64(lb, x, b);
      hb = _mm512_madd52hi_epu64(hb, x, b);
    }
    _mm512_store_si512(reinterpret_cast<__m512i *>(oa + j), fold(la, ha));
    _mm512_store_si512(reinterpret_cast<__m512i *>(ob + j), fold(lb, hb));
  }
  intel::hexl::EltwiseReduceMod(oa, oa, n, q, q, 1);
  intel::hexl::EltwiseReduceMod(ob, ob, n, q, q, 1);
#else
  (void)c52;
  for (size_t j = 0; j < n; ++j) {
    u128 sa = 0, sb = 0;
    for (size_t l = 0; l < L; ++l) {
      sa += u128(xs[l][j]) * ya[l][j];
      sb += u128(xs[l][j]) * yb[l][j];
    }
    oa[j] = static_cast<u64>(sa % q);
    ob[j] = static_cast<u64>(sb % q);
  }
#endif
}

// ------------------------------------------------------------------ gadgets
// Balanced base-2^w digits, least significant first, of v (exact) or of
// round(v * scale) (approximate); the top digit keeps the remainder.
struct Digits {
  size_t len;
  unsigned log_base;
  bool exact;
  double scale;
};
inline Digits exact_digits(const Gadget &g) { return {g.len, g.log_base, true, 1.0}; }
inline Digits approx_digits(const Gadget &g, const Ring &R) {
  return {g.len, g.log_base, false, std::ldexp(1.0, static_cast<int>(g.len * g.log_base)) / R.Qd};
}

inline void decompose_scalar(const Ring &R, const u64 *x, const Digits &d, u64 *const *out) {
  const i64 base = i64{1} << d.log_base, half = base >> 1, mask = base - 1;
  for (size_t j = 0; j < R.n; ++j) {
    i128 v = d.exact ? R.centered(x, j) : i128(std::llround(R.centered_double(x, j) * d.scale));
    for (size_t p = 0; p < d.len; ++p) {
      i64 dig;
      if (p + 1 < d.len) {
        dig = static_cast<i64>(v & mask);
        if (dig >= half) dig -= base;
        v = (v - dig) >> d.log_base;
      } else {
        dig = static_cast<i64>(v);
      }
      for (size_t r = 0; r < R.k; ++r) out[p][r * R.n + j] = small_mod(dig, R.q[r]);
    }
  }
}

// Garner coefficient t = (r1 - r0) * q0^-1 mod q1 of every coefficient (two-prime rings).
inline void garner_t(const Ring &R, const u64 *x, u64 *t) {
  intel::hexl::EltwiseSubMod(t, x + R.n, x, R.n, R.q[1]);
  intel::hexl::EltwiseFMAMod(t, t, R.ginv, nullptr, R.n, R.q[1], 1);
}

#if defined(__AVX512IFMA__)
// Residues of the signed digits in `dig` for lanes j..j+7.
inline void store_digit(const Ring &R, __m512i dig, u64 *const *out, size_t p, size_t j) {
  const __mmask8 negm = _mm512_cmplt_epi64_mask(dig, _mm512_setzero_si512());
  for (size_t r = 0; r < R.k; ++r) {
    const __m512i qv = _mm512_set1_epi64(static_cast<long long>(R.q[r]));
    _mm512_store_si512(reinterpret_cast<__m512i *>(out[p] + r * R.n + j), _mm512_mask_add_epi64(dig, negm, dig, qv));
  }
}

// Balanced digits of the integer y (|y| < 2^62) held in 64-bit lanes.
inline void store_digits_i64(const Ring &R, __m512i y, const Digits &d, u64 *const *out, size_t j) {
  const __m512i mask = _mm512_set1_epi64((1LL << d.log_base) - 1), half = _mm512_set1_epi64(1LL << (d.log_base - 1));
  const __m512i base = _mm512_set1_epi64(1LL << d.log_base);
  for (size_t p = 0; p + 1 < d.len; ++p) {
    __m512i dig = _mm512_and_si512(y, mask);
    dig = _mm512_mask_sub_epi64(dig, _mm512_cmpge_epi64_mask(dig, half), dig, base);
    store_digit(R, dig, out, p, j);
    y = _mm512_srai_epi64(_mm512_sub_epi64(y, dig), d.log_base);
  }
  store_digit(R, y, out, d.len - 1, j);
}

inline void decompose_avx512(const Ring &R, const u64 *x, const Digits &d, u64 *const *out, u64 *t_buf) {
  const __m512i zero = _mm512_setzero_si512();
  if (R.k == 2) garner_t(R, x, t_buf);
  const __m512d q0d = _mm512_set1_pd(static_cast<double>(R.q[0])), Qd = _mm512_set1_pd(R.Qd), Qh = _mm512_set1_pd(R.Qd / 2);
  const __m512d scale = _mm512_set1_pd(d.scale);
  if (!d.exact) {
    const __m512i q0 = _mm512_set1_epi64(static_cast<long long>(R.q[0])), q0h = _mm512_set1_epi64(static_cast<long long>(R.q[0] / 2));
    for (size_t j = 0; j < R.n; j += 8) {
      const __m512i r0 = _mm512_load_si512(x + j);
      __m512d v;
      if (R.k == 1) {
        const __m512i c = _mm512_mask_sub_epi64(r0, _mm512_cmpgt_epu64_mask(r0, q0h), r0, q0);
        v = _mm512_cvtepi64_pd(c);
      } else {
        const __m512i t = _mm512_load_si512(t_buf + j);
        v = _mm512_fmadd_pd(q0d, _mm512_cvtepu64_pd(t), _mm512_cvtepu64_pd(r0));
        v = _mm512_mask_sub_pd(v, _mm512_cmp_pd_mask(v, Qh, _CMP_GT_OQ), v, Qd);
      }
      const __m512i y = _mm512_cvtpd_epi64(_mm512_roundscale_pd(_mm512_mul_pd(v, scale), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
      store_digits_i64(R, y, d, out, j);
    }
    return;
  }
  // Exact digits of a two-prime value: v = r0 + q0 * t as (lo < 2^52, signed hi),
  // centred, plus the offset that turns plain base-2^w digits into balanced ones.
  const u64 M52 = (u64{1} << 52) - 1;
  const u128 Qh_int = R.Q / 2;
  u128 off = 0;
  for (size_t p = 0; p + 1 < d.len; ++p) off += u128{1} << (d.log_base * p + d.log_base - 1);
  const __m512i m52 = _mm512_set1_epi64(static_cast<long long>(M52)), q0 = _mm512_set1_epi64(static_cast<long long>(R.q[0]));
  const __m512i qh_lo = _mm512_set1_epi64(static_cast<long long>(static_cast<u64>(Qh_int) & M52)),
                qh_hi = _mm512_set1_epi64(static_cast<long long>(static_cast<u64>(Qh_int >> 52)));
  const __m512i Q_lo = _mm512_set1_epi64(static_cast<long long>(static_cast<u64>(R.Q) & M52)),
                Q_hi = _mm512_set1_epi64(static_cast<long long>(static_cast<u64>(R.Q >> 52)));
  const __m512i off_lo = _mm512_set1_epi64(static_cast<long long>(static_cast<u64>(off) & M52)),
                off_hi = _mm512_set1_epi64(static_cast<long long>(static_cast<u64>(off >> 52)));
  const __m512i dmask = _mm512_set1_epi64((1LL << d.log_base) - 1), half = _mm512_set1_epi64(1LL << (d.log_base - 1));
  const __m512i two52 = _mm512_set1_epi64(1LL << 52);
  for (size_t j = 0; j < R.n; j += 8) {
    const __m512i r0 = _mm512_load_si512(x + j), t = _mm512_load_si512(t_buf + j);
    __m512i lo = _mm512_madd52lo_epu64(r0, t, q0), hi = _mm512_madd52hi_epu64(zero, t, q0);
    hi = _mm512_add_epi64(hi, _mm512_srli_epi64(lo, 52));
    lo = _mm512_and_si512(lo, m52);
    const __mmask8 gt = _mm512_cmpgt_epu64_mask(hi, qh_hi) | (_mm512_cmpeq_epu64_mask(hi, qh_hi) & _mm512_cmpgt_epu64_mask(lo, qh_lo));
    __m512i lo2 = _mm512_sub_epi64(lo, Q_lo);
    const __mmask8 borrow = _mm512_cmplt_epi64_mask(lo2, zero);
    lo2 = _mm512_mask_add_epi64(lo2, borrow, lo2, two52);
    __m512i hi2 = _mm512_mask_sub_epi64(_mm512_sub_epi64(hi, Q_hi), borrow, _mm512_sub_epi64(hi, Q_hi), _mm512_set1_epi64(1));
    lo = _mm512_mask_mov_epi64(lo, gt, lo2);
    hi = _mm512_mask_mov_epi64(hi, gt, hi2);
    lo = _mm512_add_epi64(lo, off_lo);
    hi = _mm512_add_epi64(_mm512_add_epi64(hi, off_hi), _mm512_srli_epi64(lo, 52));
    lo = _mm512_and_si512(lo, m52);
    for (size_t p = 0; p + 1 < d.len; ++p) {
      const unsigned bit = d.log_base * static_cast<unsigned>(p);
      __m512i raw;
      if (bit + d.log_base <= 52) raw = _mm512_srli_epi64(lo, bit);
      else if (bit >= 52) raw = _mm512_srli_epi64(hi, bit - 52);
      else raw = _mm512_or_si512(_mm512_srli_epi64(lo, bit), _mm512_slli_epi64(hi, 52 - bit));
      store_digit(R, _mm512_sub_epi64(_mm512_and_si512(raw, dmask), half), out, p, j);
    }
    const unsigned top = d.log_base * static_cast<unsigned>(d.len - 1);
    store_digit(R, _mm512_srai_epi64(hi, top - 52), out, d.len - 1, j);
  }
}
#endif

inline void decompose(const Ring &R, const u64 *x, const Digits &d, u64 *const *out) {
#if defined(__AVX512IFMA__)
  if (R.n % 8 == 0 && (!d.exact || (R.k == 2 && d.log_base * (d.len - 1) >= 52))) {
    thread_local Vec t_buf;
    if (t_buf.size() < R.n) t_buf.resize(R.n);
    decompose_avx512(R, x, d, out, t_buf.data());
    return;
  }
#endif
  decompose_scalar(R, x, d, out);
}

// Gadget values g_p paired with digit p: B^p (exact) or round(Q / B^(len - p)).
inline std::vector<u128> gadget_values(const Ring &R, const Gadget &g, bool exact) {
  std::vector<u128> v(g.len);
  for (size_t p = 0; p < g.len; ++p) {
    if (exact) {
      v[p] = u128{1} << (g.log_base * p);
    } else {
      const unsigned sh = g.log_base * static_cast<unsigned>(g.len - p);
      v[p] = (R.Q + (u128{1} << (sh - 1))) >> sh;
    }
  }
  return v;
}

// ------------------------------------------------------------------ ciphertexts
struct Ct {
  Vec a, b;
  Ct() = default;
  explicit Ct(size_t words) : a(words, 0), b(words, 0) {}
};
struct Lev {
  std::vector<Ct> c;  // NTT form, one ciphertext per gadget digit
};
struct Gsw {
  Lev neg, msg;  // RLev(-S*b), RLev(b), NTT form
};

// Secret with small integer coefficients and its NTT image under a ring.
inline Vec secret_ntt(const Ring &R, const std::vector<i64> &s) {
  Vec out(R.words());
  for (size_t j = 0; j < R.n; ++j)
    for (size_t r = 0; r < R.k; ++r) out[r * R.n + j] = small_mod(s[j], R.q[r]);
  fwd(R, out.data());
  return out;
}

// b = a * s + e + m, with the mask a sampled directly in the NTT domain (the
// seeded-mask convention: a PRG seed stands for a). msg in coefficient form.
inline Ct encrypt(const Ring &R, const Vec &s_ntt, const Vec &msg, double sigma, Prg &mask, Prg &noise) {
  Ct c(R.words());
  for (size_t r = 0; r < R.k; ++r)
    for (size_t j = 0; j < R.n; ++j) c.a[r * R.n + j] = mask.uniform(R.q[r]);
  Vec e(R.words());
  for (size_t j = 0; j < R.n; ++j) {
    const i64 v = noise.gaussian(sigma);
    for (size_t r = 0; r < R.k; ++r) e[r * R.n + j] = small_mod(v, R.q[r]);
  }
  add(R, e.data(), e.data(), msg.data());
  fwd(R, e.data());
  mul(R, c.b.data(), c.a.data(), s_ntt.data());
  add(R, c.b.data(), c.b.data(), e.data());
  return c;
}

// Message plus noise (coefficient form) of an NTT-form ciphertext.
inline Vec decrypt(const Ring &R, const Vec &s_ntt, const Ct &c) {
  Vec m(R.words());
  mul(R, m.data(), c.a.data(), s_ntt.data());
  sub(R, m.data(), c.b.data(), m.data());
  inv(R, m.data());
  return m;
}

inline Lev encrypt_lev(const Ring &R, const Vec &s_ntt, const std::vector<i64> &m, const std::vector<u128> &g,
                       double sigma, Prg &mask, Prg &noise) {
  Lev lev;
  Vec msg(R.words());
  for (size_t p = 0; p < g.size(); ++p) {
    for (size_t r = 0; r < R.k; ++r) {
      const u64 gp = static_cast<u64>(g[p] % R.q[r]);
      for (size_t j = 0; j < R.n; ++j)
        msg[r * R.n + j] = static_cast<u64>(u128(small_mod(m[j], R.q[r])) * gp % R.q[r]);
    }
    lev.c.push_back(encrypt(R, s_ntt, msg, sigma, mask, noise));
  }
  return lev;
}

// Negacyclic product of small integer polynomials (client-side key material).
inline std::vector<i64> negacyclic_square(const std::vector<i64> &s) {
  const size_t n = s.size();
  std::vector<i64> out(n, 0);
  for (size_t i = 0; i < n; ++i) {
    if (!s[i]) continue;
    for (size_t j = 0; j < n; ++j) {
      const i64 v = s[i] * s[j];
      if (i + j < n) out[i + j] += v;
      else out[i + j - n] -= v;
    }
  }
  return out;
}

// ------------------------------------------------------------------ scratch
struct Scratch {
  std::vector<Vec> dig;  // digit polynomials
  Vec t0;
  Ct ext, ext2;
  std::vector<const u64 *> xs, ys_a, ys_b;
  std::vector<const Ct *> keys;
  Scratch(size_t max_digits, size_t words) : dig(max_digits, Vec(words, 0)), t0(words), ext(words), ext2(words) {}
};

// out (NTT) = sum_t NTT(S.dig[t]) * keys[t] for t < count.
inline void gadget_dot(const Ring &R, Scratch &S, const std::vector<const Ct *> &keys, size_t count, Ct &out) {
  for (size_t r = 0; r < R.k; ++r) {
    S.xs.clear();
    S.ys_a.clear();
    S.ys_b.clear();
    for (size_t t = 0; t < count; ++t) {
      S.xs.push_back(S.dig[t].data() + r * R.n);
      S.ys_a.push_back(keys[t]->a.data() + r * R.n);
      S.ys_b.push_back(keys[t]->b.data() + r * R.n);
    }
    dot2(out.a.data() + r * R.n, out.b.data() + r * R.n, S.xs.data(), S.ys_a.data(), S.ys_b.data(), count, R.n, R.q[r],
         R.c52[r]);
  }
}

// Decompose x into S.dig[first ..] and NTT the digits.
inline void digits_ntt(const Ring &R, Scratch &S, const u64 *x, const Digits &d, size_t first) {
  u64 *out[32];
  for (size_t p = 0; p < d.len; ++p) out[p] = S.dig[first + p].data();
  decompose(R, x, d, out);
  for (size_t p = 0; p < d.len; ++p) fwd(R, S.dig[first + p].data());
}

// External product C (x) c for c in coefficient form; result in NTT form.
inline void external_product(const Ring &R, Scratch &S, const Gsw &C, const Ct &c, const Digits &d, Ct &out) {
  digits_ntt(R, S, c.a.data(), d, 0);
  digits_ntt(R, S, c.b.data(), d, d.len);
  S.keys.clear();
  for (const auto &k : C.neg.c) S.keys.push_back(&k);
  for (const auto &k : C.msg.c) S.keys.push_back(&k);
  gadget_dot(R, S, S.keys, 2 * d.len, out);
}

// ------------------------------------------------------------------ MLWE maps
// An (m, n) MLWE mask is stored as m*n words per residue: component j,
// coefficient t at j*n + t. The helpers work on one residue.
//
// Insert of the MLWE(s) embedding of the i-th key half: components
// F_l = A0_{mi+l}(x^2) + x * A1_{mi+l}(x^2) (A1 absent for one ciphertext),
// inserted as iota(F_0, F_{m-1} X^-1, ..., F_1 X^-1) into R_{n1}.
inline void insert_half(const u64 *A0, const u64 *A1, size_t k, size_t n, size_t i, u64 *out, u64 q,
                        std::vector<u64> &F) {
  const size_t m = k / 2, n2 = 2 * n;
  F.resize(n2);
  std::fill(out, out + m * n2, 0);
  for (size_t j = 0; j < m; ++j) {
    const size_t src = (j == 0 ? m * i : m * i + m - j) * n;
    for (size_t t = 0; t < n; ++t) {
      F[2 * t] = A0[src + t];
      F[2 * t + 1] = A1 ? A1[src + t] : 0;
    }
    if (j == 0) {
      for (size_t u = 0; u < n2; ++u) out[m * u] = F[u];
    } else {
      for (size_t u = 0; u + 1 < n2; ++u) out[m * u + j] = F[u + 1];
      out[m * (n2 - 1) + j] = neg_mod(F[0], q);
    }
  }
}

// RLWE-to-MLWE extraction Extr_{n2}: (pi_0(A), pi_{m-1}(A) X, ..., pi_1(A) X, pi_0(B))
// with m = n1 / n2 projections.
inline void extract(const u64 *Ra, const u64 *Rb, size_t m, size_t n2, u64 *A, u64 *B, u64 q) {
  for (size_t l = 0; l < m; ++l) {
    const size_t proj = l == 0 ? 0 : m - l;
    u64 *dst = A + l * n2;
    if (l == 0) {
      for (size_t u = 0; u < n2; ++u) dst[u] = Ra[m * u];
    } else {
      for (size_t u = 0; u + 1 < n2; ++u) dst[u + 1] = Ra[m * u + proj];
      dst[0] = neg_mod(Ra[m * (n2 - 1) + proj], q);
    }
  }
  for (size_t u = 0; u < n2; ++u) B[u] = Rb[m * u];
}

// ------------------------------------------------------------------ public keys
// Conversion keys: level v turns (n1/2^v, 2^v)-MLWE into (n1/2^(v+1), 2^(v+1))-MLWE;
// key (v, i) switches iota(iota_0(S^(i))) to S1, where the rank-k key is
// pi^{n1 -> n1/k}(S1) (the paper's Remark in Section 4.6, so repacking needs no
// extra keys).
inline std::vector<i64> conversion_source_key(const std::vector<i64> &s1, size_t level, size_t i) {
  const size_t n = size_t{1} << level, k = kN1 / n, m = k / 2;
  std::vector<i64> src(kN1, 0);
  for (size_t t = 0; t < n; ++t)
    for (size_t j = 0; j < m; ++j) src[k * t + j] = s1[k * t + m * i + j];
  return src;
}

struct ClientKeys {
  std::vector<i64> s1, s2;  // ternary S1 over R_{n1}, Gaussian S2 over R_{n2}
  Vec s1_q1, s1_q2, s2_q3;  // NTT images
};

struct PublicParams {
  std::array<std::array<Lev, 2>, kLogN1> conv;  // over q1
  Lev to_rgsw;                                  // RLev(S1^2) over q1
  std::array<Lev, kN1 / kN2> rsk;               // RLev_{S2}(pi_l(S1)) over q3, n2
};

inline ClientKeys client_keygen(const Ring &R1, const Ring &R2, const Ring &R3, u64 seed) {
  ClientKeys K;
  Prg g(seed, 0x5ec2e7);
  K.s1.resize(kN1);
  for (auto &v : K.s1) v = g.ternary();
  K.s2.resize(kN2);
  for (auto &v : K.s2) v = g.gaussian(kSigmaS2);
  K.s1_q1 = secret_ntt(R1, K.s1);
  K.s1_q2 = secret_ntt(R2, K.s1);
  K.s2_q3 = secret_ntt(R3, K.s2);
  return K;
}

inline PublicParams client_public_params(const Ring &R1, const Ring &R3, const ClientKeys &K, u64 mask_seed,
                                         u64 noise_seed) {
  PublicParams pp;
  Prg mask(mask_seed, 1), noise(noise_seed, 2);
  const auto g_conv = gadget_values(R1, kConv, true);
  for (size_t v = 0; v < kLogN1; ++v)
    for (size_t i = 0; i < 2; ++i)
      pp.conv[v][i] = encrypt_lev(R1, K.s1_q1, conversion_source_key(K.s1, v, i), g_conv, kSigmaE1, mask, noise);
  pp.to_rgsw = encrypt_lev(R1, K.s1_q1, negacyclic_square(K.s1), g_conv, kSigmaE1, mask, noise);
  const auto g_rsk = gadget_values(R3, kRsk, true);
  const size_t d = kN1 / kN2;
  for (size_t l = 0; l < d; ++l) {
    std::vector<i64> proj(kN2);
    for (size_t t = 0; t < kN2; ++t) proj[t] = K.s1[d * t + l];
    pp.rsk[l] = encrypt_lev(R3, K.s2_q3, proj, g_rsk, kSigmaE2, mask, noise);
  }
  return pp;
}

// ------------------------------------------------------------------ modulus switching
// round(x * q2 / q1) for x over q1 = q11 * q12: with x = r0 + q11 * t this is
// round(r0 * q2/q1 + t * q2/q12), exact enough in double for a 34-bit result.
inline void modswitch_q1_q2(const Ring &R1, const u64 *x, u64 *y) {
  const double f0 = static_cast<double>(kQ2) / R1.Qd, f1 = static_cast<double>(kQ2) / static_cast<double>(kQ12);
#if defined(__AVX512IFMA__)
  thread_local Vec t_buf;
  if (t_buf.size() < R1.n) t_buf.resize(R1.n);
  garner_t(R1, x, t_buf.data());
  const __m512d vf0 = _mm512_set1_pd(f0), vf1 = _mm512_set1_pd(f1);
  const __m512i q2 = _mm512_set1_epi64(static_cast<long long>(kQ2));
  for (size_t j = 0; j < R1.n; j += 8) {
    const __m512d v = _mm512_fmadd_pd(_mm512_cvtepu64_pd(_mm512_load_si512(t_buf.data() + j)), vf1,
                                      _mm512_mul_pd(_mm512_cvtepu64_pd(_mm512_load_si512(x + j)), vf0));
    __m512i z = _mm512_cvtpd_epu64(_mm512_roundscale_pd(v, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
    z = _mm512_mask_sub_epi64(z, _mm512_cmpge_epu64_mask(z, q2), z, q2);
    _mm512_store_si512(reinterpret_cast<__m512i *>(y + j), z);
  }
#else
  for (size_t j = 0; j < R1.n; ++j) {
    u64 t;
    R1.lift(x[j], x[R1.n + j], t);
    u64 z = static_cast<u64>(std::llround(static_cast<double>(x[j]) * f0 + static_cast<double>(t) * f1));
    y[j] = z >= kQ2 ? z - kQ2 : z;
  }
#endif
}
inline void modswitch_single(const u64 *x, size_t n, u64 from, u64 to, u64 *y) {
  const double f = static_cast<double>(to) / static_cast<double>(from);
  for (size_t j = 0; j < n; ++j) {
    u64 z = static_cast<u64>(std::llround(static_cast<double>(x[j]) * f));
    y[j] = z >= to ? z - to : z;
  }
}

// ------------------------------------------------------------------ server
struct ServerKeys {
  const PublicParams *pp = nullptr;
  // Conversion keys of levels 6..10, switched to q2 (components 9..14), NTT form.
  std::array<std::array<Lev, 2>, kLogN1 - kRepackFirstLevel> repack;
  Digits repack_digits{kRepackLen, kConv.log_base, false, 0.0};
};

inline ServerKeys server_prepare(const Ring &R1, const Ring &R2, const PublicParams &pp) {
  ServerKeys S;
  S.pp = &pp;
  Vec tmp(R1.words());
  for (size_t v = kRepackFirstLevel; v < kLogN1; ++v)
    for (size_t i = 0; i < 2; ++i) {
      Lev &dst = S.repack[v - kRepackFirstLevel][i];
      for (size_t p = 0; p < kRepackLen; ++p) {
        const Ct &src = pp.conv[v][i].c[kRepackFirst + p];
        Ct c(R2.words());
        tmp = src.a;
        inv(R1, tmp.data());
        modswitch_q1_q2(R1, tmp.data(), c.a.data());
        tmp = src.b;
        inv(R1, tmp.data());
        modswitch_q1_q2(R1, tmp.data(), c.b.data());
        fwd(R2, c.a.data());
        fwd(R2, c.b.data());
        dst.c.push_back(std::move(c));
      }
    }
  // Component 9 + p carries gadget 2^(5(9+p)) * q2/q1 over q2.
  S.repack_digits.scale = R1.Qd / (static_cast<double>(kQ2) * std::ldexp(1.0, static_cast<int>(kConv.log_base * kRepackFirst)));
  return S;
}

struct ConvScratch {
  Vec A, B, ins0, ins1, Rb;
  Ct ks;
  std::vector<u64> F;
  ConvScratch() : A(2 * kN1), B(2 * kN1), ins0(2 * kN1), ins1(2 * kN1), Rb(2 * kN1), ks(2 * kN1) {}
};

// One MLWE(s)-to-MLWE level on the state (A, B) of rank k and degree n over ring R:
// pairs (A0, B0) with (A1, B1) when A1 is given, else converts (A0, B0) alone.
// The result (rank k/2, degree 2n) overwrites (A0, B0).
inline void conversion_level(const Ring &R, Scratch &S, ConvScratch &C, const std::array<Lev, 2> &keys,
                             const Digits &d, size_t k, size_t n, u64 *A0, u64 *B0, const u64 *A1, const u64 *B1) {
  const size_t m = k / 2, n2 = 2 * n;
  for (size_t r = 0; r < R.k; ++r) {
    const size_t o = r * R.n;
    insert_half(A0 + o, A1 ? A1 + o : nullptr, k, n, 0, C.ins0.data() + o, R.q[r], C.F);
    insert_half(A0 + o, A1 ? A1 + o : nullptr, k, n, 1, C.ins1.data() + o, R.q[r], C.F);
    u64 *rb = C.Rb.data() + o;
    std::fill(rb, rb + R.n, 0);
    for (size_t t = 0; t < n; ++t) {
      rb[m * 2 * t] = B0[o + t];
      if (B1) rb[m * (2 * t + 1)] = B1[o + t];
    }
  }
  digits_ntt(R, S, C.ins0.data(), d, 0);
  digits_ntt(R, S, C.ins1.data(), d, d.len);
  S.keys.clear();
  for (size_t i = 0; i < 2; ++i)
    for (size_t p = 0; p < d.len; ++p) S.keys.push_back(&keys[i].c[p]);
  Ct &ks = C.ks;
  gadget_dot(R, S, S.keys, 2 * d.len, ks);
  inv(R, ks.a.data());
  inv(R, ks.b.data());
  neg(R, ks.a.data(), ks.a.data());
  sub(R, ks.b.data(), C.Rb.data(), ks.b.data());
  for (size_t r = 0; r < R.k; ++r) {
    const size_t o = r * R.n;
    std::fill(B0 + o, B0 + o + R.n, 0);
    extract(ks.a.data() + o, ks.b.data() + o, m, n2, A0 + o, B0 + o, R.q[r]);
  }
}

// LWE ciphertext (a over q1 as residues, b) -> RLWE of the same message under S1,
// coefficient form (Section 4.1).
inline void lwe_to_rlwe(const Ring &R1, Scratch &S, ConvScratch &C, const PublicParams &pp, const u64 *a,
                        const u64 b[2], Ct &out) {
  std::copy(a, a + R1.words(), C.A.begin());
  std::fill(C.B.begin(), C.B.end(), 0);
  C.B[0] = b[0];
  C.B[R1.n] = b[1];
  const Digits d = exact_digits(kConv);
  size_t k = kN1, n = 1;
  for (size_t v = 0; v < kLogN1; ++v, k /= 2, n *= 2)
    conversion_level(R1, S, C, pp.conv[v], d, k, n, C.A.data(), C.B.data(), nullptr, nullptr);
  out.a = C.A;
  out.b = C.B;
}

// RLWE_S(M) (coefficient form) -> RLWE_S(-S M) = (B, 0) + A (.) RLev(S^2), NTT form.
inline void rlwe_to_rgsw(const Ring &R1, Scratch &S, const PublicParams &pp, const Ct &c, Ct &out) {
  const Digits d = exact_digits(kConv);
  digits_ntt(R1, S, c.a.data(), d, 0);
  S.keys.clear();
  for (const auto &k : pp.to_rgsw.c) S.keys.push_back(&k);
  gadget_dot(R1, S, S.keys, d.len, out);
  S.t0 = c.b;
  fwd(R1, S.t0.data());
  add(R1, out.a.data(), out.a.data(), S.t0.data());
}

// Negacyclic multiplication by X^-s in the coefficient domain (one residue).
inline void rotate_down(const u64 *x, size_t n, size_t s, u64 q, u64 *y) {
  for (size_t u = 0; u < n; ++u) y[u] = u + s < n ? x[u + s] : neg_mod(x[u + s - n], q);
}

// ------------------------------------------------------------------ database
// A bucket database: I x J polynomials over R_{n1}, NTT form mod q2, stored as
// 34-bit words (32-bit low plane plus 2-bit high plane) in blocks of 16
// coefficients laid out [block][j][i] for the first-dimension stream.
struct BucketDb {
  size_t I = 0, J = 0;
  std::vector<u32, intel::hexl::AlignedAllocator<u32, 64>> low;
  std::vector<u32> high;
  void allocate(size_t rows, size_t cols) {
    I = rows;
    J = cols;
    const size_t blocks = kN1 / 16;
    low.assign(blocks * J * I * 16, 0);
    high.assign(blocks * J * I, 0);
  }
  void store(size_t i, size_t j, const u64 *ntt_poly) {
    for (size_t c = 0; c < kN1; ++c) {
      const size_t blk = c / 16, lane = c % 16, cell = (blk * J + j) * I + i;
      low[cell * 16 + lane] = static_cast<u32>(ntt_poly[c]);
      high[cell] |= static_cast<u32>(ntt_poly[c] >> 32) << (2 * lane);
    }
  }
  uint64_t bytes() const { return low.size() * sizeof(u32) + high.size() * sizeof(u32); }
};

// First dimension: out_j = sum_i c_i * db[i][j] over R_{n1, q2} (NTT form).
// q_ab holds the I query ciphertexts as [block][i][a lanes 0..15, b lanes 16..31].
inline void first_dimension(const BucketDb &db, const u64 *q_ab, std::vector<Ct> &out) {
  const size_t I = db.I, J = db.J, blocks = kN1 / 16;
  const u64 c52 = static_cast<u64>((u128{1} << 52) % kQ2);
#if defined(__AVX512IFMA__)
  const __m512i vc = _mm512_set1_epi64(static_cast<long long>(c52)), zero = _mm512_setzero_si512();
  const __m512i three = _mm512_set1_epi64(3);
  const __m512i sh0 = _mm512_setr_epi64(0, 2, 4, 6, 8, 10, 12, 14), sh1 = _mm512_setr_epi64(16, 18, 20, 22, 24, 26, 28, 30);
  auto fold = [&](__m512i lo, __m512i hi) {
    const __m512i tlo = _mm512_madd52lo_epu64(zero, hi, vc), thi = _mm512_madd52hi_epu64(zero, hi, vc);
    return _mm512_add_epi64(_mm512_add_epi64(lo, tlo), _mm512_mullo_epi64(thi, vc));
  };
  for (size_t blk = 0; blk < blocks; ++blk) {
    const u64 *qb = q_ab + blk * I * 32;
    for (size_t j = 0; j < J; ++j) {
      const size_t cell0 = (blk * J + j) * I;
      const u32 *lp = db.low.data() + cell0 * 16;
      const u32 *hp = db.high.data() + cell0;
      __m512i la0 = zero, ha0 = zero, la1 = zero, ha1 = zero, lb0 = zero, hb0 = zero, lb1 = zero, hb1 = zero;
      for (size_t i = 0; i < I; ++i) {
        const __m512i lw = _mm512_load_si512(lp + i * 16);
        const __m512i hw = _mm512_set1_epi64(static_cast<long long>(hp[i]));
        const __m512i d0 = _mm512_or_si512(_mm512_cvtepu32_epi64(_mm512_castsi512_si256(lw)),
                                           _mm512_slli_epi64(_mm512_and_si512(_mm512_srlv_epi64(hw, sh0), three), 32));
        const __m512i d1 = _mm512_or_si512(_mm512_cvtepu32_epi64(_mm512_extracti64x4_epi64(lw, 1)),
                                           _mm512_slli_epi64(_mm512_and_si512(_mm512_srlv_epi64(hw, sh1), three), 32));
        const u64 *qi = qb + i * 32;
        const __m512i a0 = _mm512_load_si512(qi), a1 = _mm512_load_si512(qi + 8);
        const __m512i b0 = _mm512_load_si512(qi + 16), b1 = _mm512_load_si512(qi + 24);
        la0 = _mm512_madd52lo_epu64(la0, d0, a0);
        ha0 = _mm512_madd52hi_epu64(ha0, d0, a0);
        la1 = _mm512_madd52lo_epu64(la1, d1, a1);
        ha1 = _mm512_madd52hi_epu64(ha1, d1, a1);
        lb0 = _mm512_madd52lo_epu64(lb0, d0, b0);
        hb0 = _mm512_madd52hi_epu64(hb0, d0, b0);
        lb1 = _mm512_madd52lo_epu64(lb1, d1, b1);
        hb1 = _mm512_madd52hi_epu64(hb1, d1, b1);
      }
      u64 *oa = out[j].a.data() + blk * 16, *ob = out[j].b.data() + blk * 16;
      _mm512_store_si512(reinterpret_cast<__m512i *>(oa), fold(la0, ha0));
      _mm512_store_si512(reinterpret_cast<__m512i *>(oa + 8), fold(la1, ha1));
      _mm512_store_si512(reinterpret_cast<__m512i *>(ob), fold(lb0, hb0));
      _mm512_store_si512(reinterpret_cast<__m512i *>(ob + 8), fold(lb1, hb1));
    }
  }
  for (size_t j = 0; j < J; ++j) {
    intel::hexl::EltwiseReduceMod(out[j].a.data(), out[j].a.data(), kN1, kQ2, kQ2, 1);
    intel::hexl::EltwiseReduceMod(out[j].b.data(), out[j].b.data(), kN1, kQ2, kQ2, 1);
  }
#else
  (void)c52;
  for (size_t blk = 0; blk < blocks; ++blk)
    for (size_t j = 0; j < J; ++j)
      for (size_t lane = 0; lane < 16; ++lane) {
        u128 sa = 0, sb = 0;
        for (size_t i = 0; i < I; ++i) {
          const size_t cell = (blk * J + j) * I + i;
          const u64 w = db.low[cell * 16 + lane] | (static_cast<u64>((db.high[cell] >> (2 * lane)) & 3) << 32);
          const u64 *qi = q_ab + (blk * I + i) * 32;
          sa += u128(w) * qi[lane];
          sb += u128(w) * qi[16 + lane];
        }
        out[j].a[blk * 16 + lane] = static_cast<u64>(sa % kQ2);
        out[j].b[blk * 16 + lane] = static_cast<u64>(sb % kQ2);
      }
#endif
}

// ------------------------------------------------------------------ protocol
// Bucket shape: I = 2^log_i DMux rows, J columns (CMux over 2^log_j leaves),
// 32 records per polynomial; record at bucket position pos sits at
// (alpha, beta, gamma) with pos = gamma*I*J + alpha*J + beta (Section 3.1).
struct Shape {
  size_t log_i = 0, I = 1, J = 1, log_j = 0;
  static Shape make(size_t max_members, size_t log_i) {
    Shape s;
    s.log_i = log_i;
    s.I = size_t{1} << log_i;
    const size_t polys = (max_members + kRecordsPerPoly - 1) / kRecordsPerPoly;
    s.J = std::max<size_t>(1, (polys + s.I - 1) / s.I);
    while ((size_t{1} << s.log_j) < s.J) ++s.log_j;
    return s;
  }
  size_t bits() const { return log_i + log_j + kRotBits; }
  size_t lwes() const { return kCtrl.len * log_i + kSel.len * (log_j + kRotBits); }
  void locate(size_t pos, size_t &alpha, size_t &beta, size_t &gamma) const {
    gamma = pos / (I * J);
    alpha = (pos % (I * J)) / J;
    beta = pos % J;
  }
};

// LWE masks are expanded from (seed, stream) by both parties.
inline void lwe_mask(const Ring &R1, u64 seed, u64 stream, u64 *a) {
  Prg g(seed, stream);
  for (size_t r = 0; r < R1.k; ++r)
    for (size_t j = 0; j < R1.n; ++j) a[r * R1.n + j] = g.uniform(R1.q[r]);
}

// Query of one bucket: 2 residues of b per LWE, in the order ctrl bits (alpha,
// most significant first), sel bits (beta, least significant first), rot bits
// (gamma, least significant first), gadget digit p within each bit.
inline std::vector<u64> client_query(const Ring &R1, const ClientKeys &K, const Shape &sh, size_t alpha, size_t beta,
                                     size_t gamma, u64 seed, u64 first_stream, Prg &noise) {
  std::vector<u64> bits;
  for (size_t i = 0; i < sh.log_i; ++i) bits.push_back((alpha >> (sh.log_i - 1 - i)) & 1);
  for (size_t i = 0; i < sh.log_j; ++i) bits.push_back((beta >> i) & 1);
  for (size_t i = 0; i < kRotBits; ++i) bits.push_back((gamma >> i) & 1);
  const auto g_ctrl = gadget_values(R1, kCtrl, false), g_sel = gadget_values(R1, kSel, false);
  std::vector<u64> out;
  Vec a(R1.words());
  u64 stream = first_stream;
  for (size_t bit = 0; bit < bits.size(); ++bit) {
    const auto &g = bit < sh.log_i ? g_ctrl : g_sel;
    for (size_t p = 0; p < g.size(); ++p, ++stream) {
      lwe_mask(R1, seed, stream, a.data());
      const i64 e = noise.gaussian(kSigmaE1);
      for (size_t r = 0; r < R1.k; ++r) {
        const u64 q = R1.q[r];
        u128 acc = 0;
        for (size_t j = 0; j < R1.n; ++j) {
          if (K.s1[j] == 1) acc += a[r * R1.n + j];
          else if (K.s1[j] == -1) acc += q - a[r * R1.n + j];
        }
        const u64 mu = bits[bit] ? static_cast<u64>(g[p] % q) : 0;
        out.push_back(static_cast<u64>((acc + small_mod(e, q) + mu) % q));
      }
    }
  }
  return out;
}

struct BucketKeys {
  std::vector<Gsw> ctrl;      // over q1
  std::vector<Gsw> sel, rot;  // over q2
};

inline void switch_ct_to_q2(const Ring &R1, const Ring &R2, Scratch &S, const Ct &c_q1_coeff, Ct &out) {
  (void)S;
  out.a.resize(R2.words());
  out.b.resize(R2.words());
  modswitch_q1_q2(R1, c_q1_coeff.a.data(), out.a.data());
  modswitch_q1_q2(R1, c_q1_coeff.b.data(), out.b.data());
  fwd(R2, out.a.data());
  fwd(R2, out.b.data());
}

// QueryDecomp (Figure 6): every LWE -> RLWE (RLev(b) digit) -> RLWE(-S b) digit.
inline void server_decompress(const Ring &R1, const Ring &R2, Scratch &S, ConvScratch &C, const PublicParams &pp,
                              const Shape &sh, u64 seed, u64 first_stream, const u64 *b, BucketKeys &out) {
  out.ctrl.assign(sh.log_i, Gsw{});
  out.sel.assign(sh.log_j, Gsw{});
  out.rot.assign(kRotBits, Gsw{});
  Vec a(R1.words());
  Ct msg(R1.words()), negc(R1.words());
  u64 stream = first_stream;
  size_t z = 0;
  for (size_t bit = 0; bit < sh.bits(); ++bit) {
    const bool is_ctrl = bit < sh.log_i;
    Gsw &G = is_ctrl ? out.ctrl[bit] : (bit < sh.log_i + sh.log_j ? out.sel[bit - sh.log_i] : out.rot[bit - sh.log_i - sh.log_j]);
    const size_t len = is_ctrl ? kCtrl.len : kSel.len;
    for (size_t p = 0; p < len; ++p, ++stream, ++z) {
      lwe_mask(R1, seed, stream, a.data());
      const u64 bb[2] = {b[2 * z], b[2 * z + 1]};
      lwe_to_rlwe(R1, S, C, pp, a.data(), bb, msg);
      rlwe_to_rgsw(R1, S, pp, msg, negc);
      if (is_ctrl) {
        fwd(R1, msg.a.data());
        fwd(R1, msg.b.data());
        G.msg.c.push_back(msg);
        G.neg.c.push_back(negc);
      } else {
        Ct m2, n2;
        switch_ct_to_q2(R1, R2, S, msg, m2);
        inv(R1, negc.a.data());
        inv(R1, negc.b.data());
        switch_ct_to_q2(R1, R2, S, negc, n2);
        G.msg.c.push_back(std::move(m2));
        G.neg.c.push_back(std::move(n2));
      }
    }
  }
}

// Homomorphic demultiplexer (Algorithm 2) from the trivial (0, floor(q1/p)),
// in place: leaf alpha ends up holding Enc(floor(q1/p)). Coefficient form.
inline void dmux_tree(const Ring &R1, Scratch &S, const std::vector<Gsw> &ctrl, std::vector<Ct> &leaves) {
  const size_t I = size_t{1} << ctrl.size();
  if (leaves.size() != I) leaves.assign(I, Ct(R1.words()));
  for (auto &c : leaves) {
    std::fill(c.a.begin(), c.a.end(), 0);
    std::fill(c.b.begin(), c.b.end(), 0);
  }
  R1.set_signed(leaves[0].b.data(), 0, i128(R1.Q / kP));
  const Digits d = approx_digits(kCtrl, R1);
  for (size_t level = 0, stride = I; level < ctrl.size(); ++level, stride /= 2) {
    for (size_t p = 0; p < I; p += stride) {
      Ct &node = leaves[p], &child = leaves[p + stride / 2];
      external_product(R1, S, ctrl[level], node, d, child);
      inv(R1, child.a.data());
      inv(R1, child.b.data());
      sub(R1, node.a.data(), node.a.data(), child.a.data());
      sub(R1, node.b.data(), node.b.data(), child.b.data());
    }
  }
}

// q1 -> q2 switch of the DMux leaves, NTT, and the first-dimension layout.
inline void first_dim_input(const Ring &R1, const Ring &R2, const std::vector<Ct> &leaves, Vec &q_ab, Vec &tmp) {
  const size_t I = leaves.size(), blocks = kN1 / 16;
  q_ab.resize(blocks * I * 32);
  tmp.resize(2 * kN1);
  for (size_t i = 0; i < I; ++i) {
    modswitch_q1_q2(R1, leaves[i].a.data(), tmp.data());
    modswitch_q1_q2(R1, leaves[i].b.data(), tmp.data() + kN1);
    R2.ntt[0]->ComputeForward(tmp.data(), tmp.data(), 1, 1);
    R2.ntt[0]->ComputeForward(tmp.data() + kN1, tmp.data() + kN1, 1, 1);
    for (size_t blk = 0; blk < blocks; ++blk) {
      u64 *dst = q_ab.data() + (blk * I + i) * 32;
      std::memcpy(dst, tmp.data() + blk * 16, 16 * sizeof(u64));
      std::memcpy(dst + 16, tmp.data() + kN1 + blk * 16, 16 * sizeof(u64));
    }
  }
}

inline void cmux_into(const Ring &R2, Scratch &S, const Gsw &C, Ct &x0, const Ct *x1, const Digits &d) {
  Ct &diff = S.ext, &out = S.ext2;
  if (x1) sub(R2, diff.a.data(), x1->a.data(), x0.a.data());
  else neg(R2, diff.a.data(), x0.a.data());
  if (x1) sub(R2, diff.b.data(), x1->b.data(), x0.b.data());
  else neg(R2, diff.b.data(), x0.b.data());
  external_product(R2, S, C, diff, d, out);
  inv(R2, out.a.data());
  inv(R2, out.b.data());
  add(R2, x0.a.data(), x0.a.data(), out.a.data());
  add(R2, x0.b.data(), x0.b.data(), out.b.data());
}

// Homomorphic multiplexer (Algorithm 1) over the J first-dimension outputs
// (coefficient form, zero leaves beyond J), in place into cols[0].
inline void cmux_tree(const Ring &R2, Scratch &S, const std::vector<Gsw> &sel, std::vector<Ct> &cols, size_t J) {
  const Digits d = approx_digits(kSel, R2);
  for (size_t level = 0, stride = 1; level < sel.size(); ++level, stride *= 2)
    for (size_t p = 0; p < J; p += 2 * stride) cmux_into(R2, S, sel[level], cols[p], p + stride < J ? &cols[p + stride] : nullptr, d);
}

// CRot: c <- c * X^-gamma with gamma's bits (least significant first).
inline void crot(const Ring &R2, Scratch &S, const std::vector<Gsw> &rot, Ct &c) {
  const Digits d = approx_digits(kSel, R2);
  Ct shifted(R2.words());
  for (size_t k = 0; k < rot.size(); ++k) {
    rotate_down(c.a.data(), kN1, size_t{1} << k, kQ2, shifted.a.data());
    rotate_down(c.b.data(), kN1, size_t{1} << k, kQ2, shifted.b.data());
    cmux_into(R2, S, rot[k], c, &shifted, d);
  }
}

// Homomorphic repacking (Section 4.6), Repack_{n2} of up to 8 answers over q2
// (coefficient form): Extr_64 of each answer, then MLWEs-to-RLWE over 32
// leaves whose first answers.size() are real. Answer r's record lands at
// coefficients 8v + bitrev3(r) of the ring-switched message.
inline void repack(const Ring &R2, Scratch &S, ConvScratch &C, const ServerKeys &K, const std::vector<const Ct *> &answers,
                   Ct &out) {
  struct Leaf {
    Vec A, B;
  };
  std::vector<Leaf> leaves(answers.size());
  const size_t n0 = kRecordCoeffs, m0 = kN1 / n0;
  for (size_t r = 0; r < answers.size(); ++r) {
    leaves[r].A.assign(kN1, 0);
    leaves[r].B.assign(kN1, 0);
    extract(answers[r]->a.data(), answers[r]->b.data(), m0, n0, leaves[r].A.data(), leaves[r].B.data(), kQ2);
  }
  size_t live = answers.size(), k = m0, n = n0;
  for (size_t v = kRepackFirstLevel; v < kLogN1; ++v, k /= 2, n *= 2) {
    const auto &keys = K.repack[v - kRepackFirstLevel];
    for (size_t j = 0; 2 * j < live; ++j) {
      Leaf &l0 = leaves[2 * j];
      const Leaf *l1 = 2 * j + 1 < live ? &leaves[2 * j + 1] : nullptr;
      conversion_level(R2, S, C, keys, K.repack_digits, k, n, l0.A.data(), l0.B.data(), l1 ? l1->A.data() : nullptr,
                       l1 ? l1->B.data() : nullptr);
      if (j != 2 * j) std::swap(leaves[j], leaves[2 * j]);
    }
    live = (live + 1) / 2;
  }
  out.a = leaves[0].A;
  out.b = leaves[0].B;
}

struct Response {
  std::vector<u64> a;  // n2 coefficients mod q3
  std::vector<u64> b;  // n2 coefficients mod q4
};

// RespComp (Figure 7): switch to q3, ring switch R_{n1} -> R_{n2}, round B to q4.
inline void respond(const Ring &R3, Scratch &S3, const PublicParams &pp, const Ct &packed, Response &out) {
  const size_t d = kN1 / kN2;
  std::vector<u64> a3(kN1), b3(kN1), A(kN1), B(kN2);
  modswitch_single(packed.a.data(), kN1, kQ2, kQ3, a3.data());
  modswitch_single(packed.b.data(), kN1, kQ2, kQ3, b3.data());
  extract(a3.data(), b3.data(), d, kN2, A.data(), B.data(), kQ3);
  const Digits dg = exact_digits(kRsk);
  for (size_t l = 0; l < d; ++l) digits_ntt(R3, S3, A.data() + l * kN2, dg, l * kRsk.len);
  S3.keys.clear();
  for (size_t l = 0; l < d; ++l)
    for (const auto &c : pp.rsk[l].c) S3.keys.push_back(&c);
  Ct ks(R3.words());
  gadget_dot(R3, S3, S3.keys, d * kRsk.len, ks);
  inv(R3, ks.a.data());
  inv(R3, ks.b.data());
  out.a.resize(kN2);
  out.b.resize(kN2);
  const double f = static_cast<double>(kQ4) / static_cast<double>(kQ3);
  for (size_t t = 0; t < kN2; ++t) {
    out.a[t] = neg_mod(ks.a[t], kQ3);
    const u64 bt = B[t] >= ks.b[t] ? B[t] - ks.b[t] : B[t] + kQ3 - ks.b[t];
    out.b[t] = static_cast<u64>(std::llround(static_cast<double>(bt) * f)) & (kQ4 - 1);
  }
}

// RespCompRecover: nibble m and the rounding-domain error e (units of q4) of
// every coefficient.
inline void client_decode(const Ring &R3, const ClientKeys &K, const Response &resp, std::vector<u64> &nibbles,
                          std::vector<i64> &error) {
  Vec as(kN2);
  std::copy(resp.a.begin(), resp.a.end(), as.begin());
  fwd(R3, as.data());
  mul(R3, as.data(), as.data(), K.s2_q3.data());
  inv(R3, as.data());
  nibbles.resize(kN2);
  error.resize(kN2);
  const double f = static_cast<double>(kQ4) / static_cast<double>(kQ3);
  const u64 delta = kQ4 / kP;
  for (size_t t = 0; t < kN2; ++t) {
    const u64 r = static_cast<u64>(std::llround(static_cast<double>(as[t]) * f)) & (kQ4 - 1);
    const u64 x = (resp.b[t] + kQ4 - r) & (kQ4 - 1);
    const u64 m = ((x + delta / 2) / delta) % kP;
    nibbles[t] = m;
    i64 e = static_cast<i64>(x) - static_cast<i64>(m * delta);
    if (e > static_cast<i64>(kQ4 / 2)) e -= static_cast<i64>(kQ4);
    if (e < -static_cast<i64>(kQ4 / 2)) e += static_cast<i64>(kQ4);
    error[t] = e;
  }
}

inline size_t bitrev3(size_t r) { return ((r & 1) << 2) | (r & 2) | ((r >> 2) & 1); }
inline size_t packed_coeff(size_t answer, size_t v) { return kAnswersPerPacked * v + bitrev3(answer); }

// Polynomial (alpha, beta) of a bucket: record gamma's nibble v at coefficient
// 32v + gamma, centred in [-8, 8) and reduced mod q2.
template <typename RecordFn>
inline void encode_poly(const Shape &sh, size_t alpha, size_t beta, size_t members, RecordFn record, u64 *poly) {
  std::fill(poly, poly + kN1, 0);
  for (size_t gamma = 0; gamma < kRecordsPerPoly; ++gamma) {
    const size_t pos = gamma * sh.I * sh.J + alpha * sh.J + beta;
    if (pos >= members) break;
    const auto rec = record(pos);
    for (size_t v = 0; v < kRecordCoeffs; ++v) {
      const i64 nib = (rec[v / 2] >> (4 * (v % 2))) & 0xf;
      poly[kRecordsPerPoly * v + gamma] = small_mod(nib >= 8 ? nib - 16 : nib, kQ2);
    }
  }
}

}  // namespace via
