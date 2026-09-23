// VIA (Liu, Wang and Zhang, "VIA: Communication-Efficient Single-Server
// Private Information Retrieval", ePrint 2025/2074, Section 3 and Figure 4)
// with the Section 3.2 optimizations that the paper's evaluation and the
// authors' code (owniai/VIA, VIAClient.hpp and VIAServer.hpp) use: the client
// sends the first three DMux levels as eight RLWE ciphertexts, the server stops
// the CMux three levels early and returns eight answers, and blinded extraction
// returns only the record's coefficients of each answer. VIA has no offline
// communication; the ring-switching key travels with the query.
//
// Rings, NTT arithmetic, gadget decomposition, encryption, external products
// and the IFMA dot products come from the VIA-B adapter (../via_b/via.hpp).
// This header holds VIA's own parameters and answer path; inside via::base the
// names kQ2, kCtrl, BucketDb, ... are VIA's, not VIA-C's; functions whose
// parameter types match a via:: one have their own names, so argument-dependent
// lookup never picks the VIA-C version.
#pragma once

#include "via.hpp"

namespace via::base {

// ------------------------------------------------------------------ parameters
// Moduli, degrees, p, error widths and gadget lengths are the paper's VIA set
// (Section 5.1, Appendix B, Table 5); the gadget bases are the powers of two of
// the authors' code. README.md lists where and why this deviates.
constexpr u64 kQ11 = 268369921ULL, kQ12 = 536608769ULL;  // q1 = q11 * q12 < 2^57
constexpr u64 kQ2 = 34359214081ULL;                      // < 2^35
constexpr u64 kQ3 = 2147352577ULL;                       // < 2^31
constexpr unsigned kLogQ4 = 15, kLogP = 8;
constexpr u64 kQ4 = u64{1} << kLogQ4, kP = u64{1} << kLogP;
constexpr double kSigmaE1 = 1.0, kSigmaE2 = 4960.0;  // S1 uniform on [-2, 2], S2 ternary

constexpr size_t kD = kN1 / kN2;                         // ring-switching projections
constexpr size_t kRecordCoeffs = kRecordBytes;           // one byte per coefficient
constexpr size_t kRecordsPerPoly = kN1 / kRecordCoeffs;  // 64
constexpr size_t kClientLevels = 3;                      // DMux levels sent as 2^3 RLWE ciphertexts
constexpr size_t kSkippedLevels = 3;                     // CMux levels left undone: 2^3 answers
constexpr size_t kMaxLogRows = 11;                       // first-dimension IFMA accumulators hold 2^11 terms

constexpr Gadget kCtrl{2, 19};              // DMux control bits, approximate over q1
constexpr Gadget kSelA{4, 5}, kSelB{3, 5};  // CMux select bits: A and B digits, approximate over q2
constexpr Gadget kRsk{4, 5};                // ring-switching key, approximate over q2

// ------------------------------------------------------------------ layout
// Record gamma of a polynomial holds byte u at coefficient rotation(gamma) +
// kD * u: X^-rotation(gamma) moves it to kD * u, which ring switching projects
// to coefficient u of R_{n2}.
inline size_t rotation(size_t gamma) { return kD * kRecordCoeffs * (gamma / kD) + gamma % kD; }

// Bucket shape: I = 2^log_i DMux rows, J columns (CMux over 2^log_j leaves), 64
// records per polynomial; bucket position pos sits at (alpha, beta, gamma) with
// pos = gamma*I*J + alpha*J + beta (Section 3.1).
struct Shape {
  size_t log_i = 0, I = 1, J = 1, log_j = 0, client_levels = 0, skipped = 0;
  static Shape make(size_t max_members, size_t log_i) {
    if (log_i > kMaxLogRows) throw std::invalid_argument("log_rows above 11 overflows the first-dimension accumulators");
    Shape s;
    s.log_i = log_i;
    s.I = size_t{1} << log_i;
    const size_t polys = (max_members + kRecordsPerPoly - 1) / kRecordsPerPoly;
    s.J = std::max<size_t>(1, (polys + s.I - 1) / s.I);
    while ((size_t{1} << s.log_j) < s.J) ++s.log_j;
    s.client_levels = std::min(kClientLevels, log_i);
    s.skipped = std::min(kSkippedLevels, s.log_j);
    return s;
  }
  size_t sent_leaves() const { return size_t{1} << client_levels; }
  size_t ctrl_bits() const { return log_i - client_levels; }
  size_t sel_bits() const { return log_j - skipped; }
  size_t answers() const { return size_t{1} << skipped; }
  // Query ciphertexts of one bucket: sent leaves, then the RGSW rows.
  size_t ciphertexts() const { return sent_leaves() + ctrl_bits() * 2 * kCtrl.len + sel_bits() * (kSelA.len + kSelB.len); }
  void locate(size_t pos, size_t &alpha, size_t &beta, size_t &gamma) const {
    gamma = pos / (I * J);
    alpha = (pos % (I * J)) / J;
    beta = pos % J;
  }
  // The answer that holds column beta.
  size_t answer_of(size_t beta) const { return beta >> sel_bits(); }
};

// ------------------------------------------------------------------ keys and seeded ciphertexts
struct ClientKeys {
  std::vector<i64> s1, s2;  // S1 over R_{n1}, S2 over R_{n2}
  Vec s1_q1, s2_q2, s2_q3;  // NTT images
};

// Fresh keys for one path query (VIA samples them per query).
inline ClientKeys client_keygen(const Ring &R1, const Ring &R2s, const Ring &R3, Prg &g) {
  ClientKeys K;
  K.s1.resize(kN1);
  for (auto &v : K.s1) v = static_cast<i64>(g.uniform(5)) - 2;
  K.s2.resize(kN2);
  for (auto &v : K.s2) v = g.ternary();
  K.s1_q1 = secret_ntt(R1, K.s1);
  K.s2_q2 = secret_ntt(R2s, K.s2);
  K.s2_q3 = secret_ntt(R3, K.s2);
  return K;
}

// The uniform mask of query ciphertext `stream`, sampled in the NTT domain from
// the path's seed exactly as via::encrypt samples it; client and server expand
// it identically, so only b is sent.
inline void mask_of(const Ring &R, u64 seed, u64 stream, u64 *a) {
  Prg g(seed, stream);
  for (size_t r = 0; r < R.k; ++r)
    for (size_t j = 0; j < R.n; ++j) a[r * R.n + j] = g.uniform(R.q[r]);
}

inline Vec encrypt_b(const Ring &R, const Vec &s_ntt, const Vec &msg, double sigma, u64 seed, u64 stream, Prg &noise) {
  Prg mask(seed, stream);
  return encrypt(R, s_ntt, msg, sigma, mask, noise).b;
}

// b parts of RLev rows m * g_p, one mask stream per row.
inline void encrypt_lev_b(const Ring &R, const Vec &s_ntt, const std::vector<i64> &m, const std::vector<u128> &g,
                          double sigma, u64 seed, u64 &stream, Prg &noise, std::vector<Vec> &out) {
  Vec msg(R.words());
  for (size_t p = 0; p < g.size(); ++p) {
    for (size_t r = 0; r < R.k; ++r) {
      const u64 gp = static_cast<u64>(g[p] % R.q[r]);
      for (size_t j = 0; j < R.n; ++j) msg[r * R.n + j] = static_cast<u64>(u128(small_mod(m[j], R.q[r])) * gp % R.q[r]);
    }
    out.push_back(encrypt_b(R, s_ntt, msg, sigma, seed, stream++, noise));
  }
}

// RGSW_S(bit) rows (RLev(-S bit) with gadget gn, then RLev(bit) with gm).
inline void encrypt_gsw_b(const Ring &R, const Vec &s_ntt, const std::vector<i64> &s, u64 bit, const std::vector<u128> &gn,
                          const std::vector<u128> &gm, double sigma, u64 seed, u64 &stream, Prg &noise, std::vector<Vec> &out) {
  std::vector<i64> neg_m(R.n, 0), m(R.n, 0);
  if (bit) {
    for (size_t j = 0; j < R.n; ++j) neg_m[j] = -s[j];
    m[0] = 1;
  }
  encrypt_lev_b(R, s_ntt, neg_m, gn, sigma, seed, stream, noise, out);
  encrypt_lev_b(R, s_ntt, m, gm, sigma, seed, stream, noise, out);
}

// Ring-switching key RLev_{S2}(pi_l(S1)) for l < kD over R_{n2, q2}: kD * kRsk.len
// b parts from stream `stream` on.
inline std::vector<Vec> client_rsk(const Ring &R2s, const ClientKeys &K, u64 seed, u64 stream, Prg &noise) {
  const auto g = gadget_values(R2s, kRsk, false);
  std::vector<Vec> out;
  std::vector<i64> proj(kN2);
  for (size_t l = 0; l < kD; ++l) {
    for (size_t t = 0; t < kN2; ++t) proj[t] = K.s1[kD * t + l];
    encrypt_lev_b(R2s, K.s2_q2, proj, g, kSigmaE2, seed, stream, noise, out);
  }
  return out;
}

// ------------------------------------------------------------------ query
// b parts of one bucket's query, in stream order.
struct BucketQuery {
  std::vector<Vec> leaves;  // 2^c RLWE over (n1, q1): Enc(floor(q1/p) X^-t) at the top c bits of alpha, else 0
  std::vector<Vec> ctrl;    // RGSW rows of alpha's remaining bits over (n1, q1), most significant first
  std::vector<Vec> sel;     // RGSW rows of beta's low bits over (n2, q2), least significant first
};

inline BucketQuery client_bucket_query(const Ring &R1, const Ring &R2s, const ClientKeys &K, const Shape &sh, size_t alpha,
                                       size_t beta, size_t gamma, u64 seed, u64 stream, Prg &noise) {
  BucketQuery q;
  const size_t top = alpha >> sh.ctrl_bits(), t = rotation(gamma);
  Vec zero(R1.words()), rot(R1.words());
  const i128 delta = i128(R1.Q / kP);
  if (t == 0) R1.set_signed(rot.data(), 0, delta);
  else R1.set_signed(rot.data(), kN1 - t, -delta);  // X^-t = -X^(n1 - t)
  for (size_t v = 0; v < sh.sent_leaves(); ++v)
    q.leaves.push_back(encrypt_b(R1, K.s1_q1, v == top ? rot : zero, kSigmaE1, seed, stream++, noise));
  const auto g_ctrl = gadget_values(R1, kCtrl, false);
  for (size_t k = 0; k < sh.ctrl_bits(); ++k)
    encrypt_gsw_b(R1, K.s1_q1, K.s1, (alpha >> (sh.ctrl_bits() - 1 - k)) & 1, g_ctrl, g_ctrl, kSigmaE1, seed, stream, noise,
                  q.ctrl);
  const auto g_a = gadget_values(R2s, kSelA, false), g_b = gadget_values(R2s, kSelB, false);
  for (size_t k = 0; k < sh.sel_bits(); ++k)
    encrypt_gsw_b(R2s, K.s2_q2, K.s2, (beta >> k) & 1, g_a, g_b, kSigmaE2, seed, stream, noise, q.sel);
  return q;
}

// ------------------------------------------------------------------ server: query expansion
struct ServerQuery {
  std::vector<Ct> leaves;  // coefficient form
  std::vector<Gsw> ctrl, sel;
};

inline Ct expand_ct(const Ring &R, u64 seed, u64 stream, const Vec &b) {
  Ct c;
  c.a.resize(R.words());
  mask_of(R, seed, stream, c.a.data());
  c.b = b;
  return c;
}

inline void expand_gsw(const Ring &R, u64 seed, u64 &stream, const std::vector<Vec> &rows, size_t &row, size_t neg_len,
                       size_t msg_len, Gsw &out) {
  out.neg.c.clear();
  out.msg.c.clear();
  for (size_t p = 0; p < neg_len; ++p, ++row) out.neg.c.push_back(expand_ct(R, seed, stream++, rows[row]));
  for (size_t p = 0; p < msg_len; ++p, ++row) out.msg.c.push_back(expand_ct(R, seed, stream++, rows[row]));
}

inline void server_expand(const Ring &R1, const Ring &R2s, const Shape &sh, u64 seed, u64 stream, const BucketQuery &q,
                          ServerQuery &out) {
  out.leaves.resize(sh.sent_leaves());
  for (size_t v = 0; v < sh.sent_leaves(); ++v) {
    out.leaves[v] = expand_ct(R1, seed, stream++, q.leaves[v]);
    inv(R1, out.leaves[v].a.data());
    inv(R1, out.leaves[v].b.data());
  }
  out.ctrl.resize(sh.ctrl_bits());
  size_t row = 0;
  for (auto &G : out.ctrl) expand_gsw(R1, seed, stream, q.ctrl, row, kCtrl.len, kCtrl.len, G);
  out.sel.resize(sh.sel_bits());
  row = 0;
  for (auto &G : out.sel) expand_gsw(R2s, seed, stream, q.sel, row, kSelA.len, kSelB.len, G);
}

inline std::vector<Ct> server_rsk(const Ring &R2s, u64 seed, u64 stream, const std::vector<Vec> &b) {
  std::vector<Ct> out;
  for (const auto &row : b) out.push_back(expand_ct(R2s, seed, stream++, row));
  return out;
}

// ------------------------------------------------------------------ server: answer
// DMux (Algorithm 2) below the client's levels, in place: sent leaf v starts at
// v * (I >> c); each control bit splits node p into (p - C(x)p, C(x)p) at p and
// p + stride/2. Leaf alpha ends up holding Enc(floor(q1/p) X^-t). Coefficient form.
inline void dmux(const Ring &R1, Scratch &S, const ServerQuery &q, size_t I, std::vector<Ct> &leaves) {
  leaves.resize(I);
  const size_t first = I / q.leaves.size();
  for (size_t v = 0; v < q.leaves.size(); ++v) leaves[v * first] = q.leaves[v];
  const Digits d = approx_digits(kCtrl, R1);
  for (size_t level = 0, stride = first; level < q.ctrl.size(); ++level, stride /= 2)
    for (size_t p = 0; p < I; p += stride) {
      Ct &node = leaves[p], &child = leaves[p + stride / 2];
      child.a.resize(R1.words());
      child.b.resize(R1.words());
      external_product(R1, S, q.ctrl[level], node, d, child);
      inv(R1, child.a.data());
      inv(R1, child.b.data());
      sub(R1, node.a.data(), node.a.data(), child.a.data());
      sub(R1, node.b.data(), node.b.data(), child.b.data());
    }
}

// round(x * to / q1) for x over q1 = q11 * q12: with x = r0 + q11 * t this is
// round(r0 * to/q1 + t * to/q12), exact enough in double for a 35-bit result.
inline void modswitch_q1(const Ring &R1, const u64 *x, u64 to, u64 *y) {
  const double f0 = static_cast<double>(to) / R1.Qd, f1 = static_cast<double>(to) / static_cast<double>(R1.q[1]);
#if defined(__AVX512IFMA__)
  thread_local Vec t_buf;
  if (t_buf.size() < R1.n) t_buf.resize(R1.n);
  garner_t(R1, x, t_buf.data());
  const __m512d vf0 = _mm512_set1_pd(f0), vf1 = _mm512_set1_pd(f1);
  const __m512i vto = _mm512_set1_epi64(static_cast<long long>(to));
  for (size_t j = 0; j < R1.n; j += 8) {
    const __m512d v = _mm512_fmadd_pd(_mm512_cvtepu64_pd(_mm512_load_si512(t_buf.data() + j)), vf1,
                                      _mm512_mul_pd(_mm512_cvtepu64_pd(_mm512_load_si512(x + j)), vf0));
    __m512i z = _mm512_cvtpd_epu64(_mm512_roundscale_pd(v, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
    z = _mm512_mask_sub_epi64(z, _mm512_cmpge_epu64_mask(z, vto), z, vto);
    _mm512_store_si512(reinterpret_cast<__m512i *>(y + j), z);
  }
#else
  for (size_t j = 0; j < R1.n; ++j) {
    u64 t;
    R1.lift(x[j], x[R1.n + j], t);
    const u64 z = static_cast<u64>(std::llround(static_cast<double>(x[j]) * f0 + static_cast<double>(t) * f1));
    y[j] = z >= to ? z - to : z;
  }
#endif
}

// q1 -> q2 switch of the DMux leaves, NTT, and the first-dimension layout
// [block][i][a lanes 0..15, b lanes 16..31].
inline void first_dim_query(const Ring &R1, const Ring &R2, const std::vector<Ct> &leaves, Vec &q_ab, Vec &tmp) {
  const size_t I = leaves.size(), blocks = kN1 / 16;
  q_ab.resize(blocks * I * 32);
  tmp.resize(2 * kN1);
  for (size_t i = 0; i < I; ++i) {
    modswitch_q1(R1, leaves[i].a.data(), kQ2, tmp.data());
    modswitch_q1(R1, leaves[i].b.data(), kQ2, tmp.data() + kN1);
    R2.ntt[0]->ComputeForward(tmp.data(), tmp.data(), 1, 1);
    R2.ntt[0]->ComputeForward(tmp.data() + kN1, tmp.data() + kN1, 1, 1);
    for (size_t blk = 0; blk < blocks; ++blk) {
      u64 *dst = q_ab.data() + (blk * I + i) * 32;
      std::memcpy(dst, tmp.data() + blk * 16, 16 * sizeof(u64));
      std::memcpy(dst + 16, tmp.data() + kN1 + blk * 16, 16 * sizeof(u64));
    }
  }
}

// A bucket database: I x J polynomials over R_{n1}, NTT form mod q2, stored as
// 35-bit words (32-bit low plane plus a 4-bit high plane, one u64 per 16
// coefficients) in blocks of 16 coefficients laid out [block][j][i].
struct BucketDb {
  size_t I = 0, J = 0;
  std::vector<u32, intel::hexl::AlignedAllocator<u32, 64>> low;
  std::vector<u64> high;
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
      high[cell] |= (ntt_poly[c] >> 32) << (4 * lane);
    }
  }
  uint64_t bytes() const { return low.size() * sizeof(u32) + high.size() * sizeof(u64); }
};

// First dimension: out_j = sum_i c_i * db[i][j] over R_{n1, q2} (NTT form).
inline void first_dimension(const BucketDb &db, const u64 *q_ab, std::vector<Ct> &out) {
  const size_t I = db.I, J = db.J, blocks = kN1 / 16;
  const u64 c52 = static_cast<u64>((u128{1} << 52) % kQ2);
#if defined(__AVX512IFMA__)
  const __m512i vc = _mm512_set1_epi64(static_cast<long long>(c52)), zero = _mm512_setzero_si512();
  const __m512i nib = _mm512_set1_epi64(15);
  const __m512i sh0 = _mm512_setr_epi64(0, 4, 8, 12, 16, 20, 24, 28), sh1 = _mm512_setr_epi64(32, 36, 40, 44, 48, 52, 56, 60);
  // lo + hi * 2^52 with lo < I * 2^52 and hi < I * 2^18: fold hi * (2^52 mod q2) back under 2^64.
  auto fold = [&](__m512i lo, __m512i hi) {
    const __m512i tlo = _mm512_madd52lo_epu64(zero, hi, vc), thi = _mm512_madd52hi_epu64(zero, hi, vc);
    return _mm512_add_epi64(_mm512_add_epi64(lo, tlo), _mm512_mullo_epi64(thi, vc));
  };
  for (size_t blk = 0; blk < blocks; ++blk) {
    const u64 *qb = q_ab + blk * I * 32;
    for (size_t j = 0; j < J; ++j) {
      const size_t cell0 = (blk * J + j) * I;
      const u32 *lp = db.low.data() + cell0 * 16;
      const u64 *hp = db.high.data() + cell0;
      __m512i la0 = zero, ha0 = zero, la1 = zero, ha1 = zero, lb0 = zero, hb0 = zero, lb1 = zero, hb1 = zero;
      for (size_t i = 0; i < I; ++i) {
        const __m512i lw = _mm512_load_si512(lp + i * 16);
        const __m512i hw = _mm512_set1_epi64(static_cast<long long>(hp[i]));
        const __m512i d0 = _mm512_or_si512(_mm512_cvtepu32_epi64(_mm512_castsi512_si256(lw)),
                                           _mm512_slli_epi64(_mm512_and_si512(_mm512_srlv_epi64(hw, sh0), nib), 32));
        const __m512i d1 = _mm512_or_si512(_mm512_cvtepu32_epi64(_mm512_extracti64x4_epi64(lw, 1)),
                                           _mm512_slli_epi64(_mm512_and_si512(_mm512_srlv_epi64(hw, sh1), nib), 32));
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
          const u64 w = db.low[cell * 16 + lane] | (((db.high[cell] >> (4 * lane)) & 15) << 32);
          const u64 *qi = q_ab + (blk * I + i) * 32;
          sa += u128(w) * qi[lane];
          sb += u128(w) * qi[16 + lane];
        }
        out[j].a[blk * 16 + lane] = static_cast<u64>(sa % kQ2);
        out[j].b[blk * 16 + lane] = static_cast<u64>(sb % kQ2);
      }
#endif
}

// Ring switching of a first-dimension output (coefficient form over R_{n1, q2})
// to R_{n2, q2} under S2: the MLWE extraction Extr_{n2}, then a key switch of
// its kD mask components with RLev_{S2}(pi_l(S1)). Coefficient form out.
inline void ring_switch(const Ring &R2s, Scratch &S, const std::vector<Ct> &rsk, const Ct &c, Vec &A, Vec &B, Ct &out) {
  extract(c.a.data(), c.b.data(), kD, kN2, A.data(), B.data(), kQ2);
  const Digits d = approx_digits(kRsk, R2s);
  for (size_t l = 0; l < kD; ++l) digits_ntt(R2s, S, A.data() + l * kN2, d, l * kRsk.len);
  S.keys.clear();
  for (const auto &k : rsk) S.keys.push_back(&k);
  gadget_dot(R2s, S, S.keys, kD * kRsk.len, S.ext);
  inv(R2s, S.ext.a.data());
  inv(R2s, S.ext.b.data());
  neg(R2s, out.a.data(), S.ext.a.data());
  sub(R2s, out.b.data(), B.data(), S.ext.b.data());
}

// External product with separate digit sets for A (paired with RLev(-S b)) and
// B (paired with RLev(b)); result in NTT form.
inline void external_product2(const Ring &R, Scratch &S, const Gsw &C, const Ct &c, const Digits &da, const Digits &db,
                              Ct &out) {
  digits_ntt(R, S, c.a.data(), da, 0);
  digits_ntt(R, S, c.b.data(), db, da.len);
  S.keys.clear();
  for (const auto &k : C.neg.c) S.keys.push_back(&k);
  for (const auto &k : C.msg.c) S.keys.push_back(&k);
  gadget_dot(R, S, S.keys, da.len + db.len, out);
}

// x0 <- x0 + C (x) (x1 - x0), x1 = 0 when absent. Coefficient form.
inline void cmux_into(const Ring &R, Scratch &S, const Gsw &C, Ct &x0, const Ct *x1, const Digits &da, const Digits &db) {
  Ct &diff = S.ext, &prod = S.ext2;
  if (x1) {
    sub(R, diff.a.data(), x1->a.data(), x0.a.data());
    sub(R, diff.b.data(), x1->b.data(), x0.b.data());
  } else {
    neg(R, diff.a.data(), x0.a.data());
    neg(R, diff.b.data(), x0.b.data());
  }
  external_product2(R, S, C, diff, da, db, prod);
  inv(R, prod.a.data());
  inv(R, prod.b.data());
  add(R, x0.a.data(), x0.a.data(), prod.a.data());
  add(R, x0.b.data(), x0.b.data(), prod.b.data());
}

// CMux (Algorithm 1) over the J ring-switched columns (zero beyond J), stopping
// Shape::skipped levels early: answer k is cols[k << sel.size()], or zero when
// that column is past J.
inline void cmux_levels(const Ring &R2s, Scratch &S, const std::vector<Gsw> &sel, std::vector<Ct> &cols, size_t J) {
  const Digits da = approx_digits(kSelA, R2s), db = approx_digits(kSelB, R2s);
  for (size_t level = 0, stride = 1; level < sel.size(); ++level, stride *= 2)
    for (size_t p = 0; p < J; p += 2 * stride)
      cmux_into(R2s, S, sel[level], cols[p], p + stride < J ? &cols[p + stride] : nullptr, da, db);
}

// Blinded extraction: the mask switched to q3 and the record's coefficients
// of B switched to q4.
struct Answer {
  std::vector<u64> a;  // kN2 coefficients mod q3
  std::vector<u64> b;  // kRecordCoeffs coefficients mod q4
};

inline void respond(const Ct &c, Answer &out) {
  out.a.resize(kN2);
  out.b.resize(kRecordCoeffs);
  modswitch_single(c.a.data(), kN2, kQ2, kQ3, out.a.data());
  modswitch_single(c.b.data(), kRecordCoeffs, kQ2, kQ4, out.b.data());
}

// Coefficients u < kRecordCoeffs of B - A * S2: the record's bytes and the
// largest decoding error in q4 units (half a plaintext step is kQ4 / kP / 2).
inline void client_decode(const Ring &R3, const ClientKeys &K, const Answer &ans, std::array<uint8_t, kRecordBytes> &bytes,
                          i64 &worst) {
  Vec as(kN2);
  std::copy(ans.a.begin(), ans.a.end(), as.begin());
  fwd(R3, as.data());
  mul(R3, as.data(), as.data(), K.s2_q3.data());
  inv(R3, as.data());
  const double f = static_cast<double>(kQ4) / static_cast<double>(kQ3);
  const u64 delta = kQ4 / kP;
  for (size_t u = 0; u < kRecordCoeffs; ++u) {
    const u64 r = static_cast<u64>(std::llround(static_cast<double>(as[u]) * f)) & (kQ4 - 1);
    const u64 x = (ans.b[u] + kQ4 - r) & (kQ4 - 1);
    const u64 m = ((x + delta / 2) / delta) % kP;
    bytes[u] = static_cast<uint8_t>(m);
    i64 e = static_cast<i64>(x) - static_cast<i64>(m * delta);
    if (e > static_cast<i64>(kQ4 / 2)) e -= static_cast<i64>(kQ4);
    if (e < -static_cast<i64>(kQ4 / 2)) e += static_cast<i64>(kQ4);
    worst = std::max<i64>(worst, std::abs(e));
  }
}

// Polynomial (alpha, beta) of a bucket: record gamma's byte u at coefficient
// rotation(gamma) + kD * u, centred in [-128, 128) and reduced mod q2.
template <typename RecordFn>
inline void encode_poly(const Shape &sh, size_t alpha, size_t beta, size_t members, RecordFn record, u64 *poly) {
  std::fill(poly, poly + kN1, 0);
  for (size_t gamma = 0; gamma < kRecordsPerPoly; ++gamma) {
    const size_t pos = gamma * sh.I * sh.J + alpha * sh.J + beta;
    if (pos >= members) break;
    const auto rec = record(pos);
    for (size_t u = 0; u < kRecordCoeffs; ++u) {
      const i64 v = rec[u];
      poly[rotation(gamma) + kD * u] = small_mod(v >= 128 ? v - 256 : v, kQ2);
    }
  }
}

}  // namespace via::base
