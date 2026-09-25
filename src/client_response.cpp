#include "client.h"
#include "rlwe.h"
#include "utils.h"
#include "hexl/hexl.hpp"

#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>

// Shared single-mod decryption under modulus `q` using the matching sk.
// Computes phase = c0 + c1*s (mod q), recovers m = round(phase * t / q),
// and returns (plaintext, noise_budget).
static void decrypt_phase_single_mod(const RlweCt &ct,
                                     const uint64_t *sk_ntt,
                                     uint64_t q, uint64_t t,
                                     RlwePt &out_pt,
                                     int &out_budget) {
  constexpr size_t N = DBConsts::PolyDegree;

  std::vector<uint64_t> phase(N);
  std::vector<uint64_t> c0(N), c1(N);
  for (size_t i = 0; i < N; i++) {
    c0[i] = ct.c0[i] % q;
    c1[i] = ct.c1[i] % q;
  }

  // Compute a * s (mod q) in NTT.
  if (ct.ntt_form) {
    intel::hexl::EltwiseMultMod(phase.data(), c1.data(), sk_ntt, N, q, 1);
    utils::ntt_inv(phase.data(), N, q);
    utils::ntt_inv(c0.data(), N, q);
  } else {
    utils::ntt_fwd(c1.data(), N, q);
    intel::hexl::EltwiseMultMod(phase.data(), c1.data(), sk_ntt, N, q, 1);
    utils::ntt_inv(phase.data(), N, q);
  }
  // Add c0 in coefficient form (mod q).
  intel::hexl::EltwiseAddMod(phase.data(), phase.data(), c0.data(), N, q);

  out_pt.data.assign(N, 0);
  const uint64_t delta = q / t;
  const uint64_t half_q = q / 2;
  uint64_t max_noise = 0;

  for (size_t i = 0; i < N; i++) {
    uint64_t m = utils::round_div_u128((uint128_t)phase[i] * t, q) % t;
    out_pt.data[i] = m;

    // Compare against round(q*m/t), not floor(q/t)*m: when q is not a multiple
    // of t, the latter undercounts by m*(q mod t)/t and inflates the residue.
    uint64_t approx = utils::round_div_u128((uint128_t)q * m, t) % q;
    uint64_t noise_pos = (phase[i] >= approx) ? (phase[i] - approx) : (q - approx + phase[i]);
    uint64_t noise_abs = (noise_pos > half_q) ? (q - noise_pos) : noise_pos;
    if (noise_abs > max_noise) max_noise = noise_abs;
  }

  out_budget = (max_noise > 0)
    ? static_cast<int>(std::log2(static_cast<double>(q) / (2.0 * t * max_noise)))
    : static_cast<int>(std::log2(static_cast<double>(q) / (2.0 * t)));
}

RlwePt PirClient::decrypt_ct(const RlweCt &ct) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs_arr = pir_params_.get_rns_mods();
  const std::vector<uint64_t> qs(qs_arr.begin(), qs_arr.end());
  const uint64_t t = pir_params_.get_plain_mod();
  RlwePt result;
  decrypt_rns(ct, rlwe_sk_, N, qs, t, pir_params_.get_rns_tables(), result);
  return result;
}

int PirClient::noise_budget(const RlweCt &ct) {
  // Debug/diagnostic single-mod estimate under the first limb. It derives a
  // phase/noise estimate from the K=1-style path; the K-aware decrypt_rns path
  // does not expose noise and this is not SEAL-backed invariant_noise_budget.
  const uint64_t q = pir_params_.get_rns_mods()[0];
  const uint64_t t = pir_params_.get_plain_mod();
  RlwePt tmp;
  int budget = 0;
  decrypt_phase_single_mod(ct, rlwe_sk_.data.data(), q, t, tmp, budget);
  return budget;
}



RlweCt PirClient::load_resp_from_stream(std::stringstream &resp_stream) {
  // 这里是 response codec inverse。server 写出的 single-limb small-q ciphertext
  // 其 layout 是先 c0 coefficients、再 c1 coefficients；每个 coefficient 精确使用
  // small_q_width bits，bit order 为 LSB-first。恢复出的 ciphertext 是
  // coefficient-form；query 和 key transport 位于这个 codec 之外。
  //
  // 在 Prototype trust boundary 上，expected payload 内部 EOF 会被拒绝，但不会
  // 验证 decoded coefficient < small_q，也不检查 expected payload 之后的额外
  // bytes（包括 padding），且没有 authentication 或 integrity protection。
  // decrypt path 后续会按 small_q 对 c0/c1 取模/规约。
  const size_t small_q = pir_params_.get_small_q();
  const size_t small_q_width =
      static_cast<size_t>(std::ceil(std::log2(small_q)));
  constexpr size_t coeff_count = DBConsts::PolyDegree;

  RlweCt result;
  result.c0.assign(coeff_count, 0);
  result.c1.assign(coeff_count, 0);

  uint8_t current_byte = 0;
  size_t bits_left = 0;
  auto next_bit = [&]() -> uint8_t {
    if (bits_left == 0) {
      int ch = resp_stream.get();
      if (ch == EOF)
        throw std::runtime_error("unexpected end of response stream");
      current_byte = static_cast<uint8_t>(ch);
      bits_left = 8;
    }
    uint8_t bit = current_byte & 1;
    current_byte >>= 1;
    --bits_left;
    return bit;
  };
  auto read_coeff = [&](uint64_t &dest) {
    dest = 0;
    // 位序与 save_resp_to_stream 相同：从下一个 LSB-first stream bit 读取
    // coefficient 的 bit j，并恢复到 position j。
    for (size_t j = 0; j < small_q_width; ++j)
      dest |= static_cast<uint64_t>(next_bit()) << j;
  };

  for (size_t i = 0; i < coeff_count; ++i) read_coeff(result.c0[i]);
  for (size_t i = 0; i < coeff_count; ++i) read_coeff(result.c1[i]);
  result.ntt_form = false;
  return result;
}

RlwePt PirClient::decrypt_mod_q(const RlweCt &ct) const {
  uint64_t max_noise = 0;
  int budget = 0;
  RlwePt result = decrypt_mod_q_measured(ct, max_noise, budget);
  BENCH_PRINT("Noise budget after decryption: " << budget
              << " (max noise: " << max_noise << ")");
  return result;
}

RlwePt PirClient::decrypt_mod_q_measured(const RlweCt &ct,
                                         uint64_t &max_noise_out,
                                         int &budget_out) const {
  // 这里是 Algorithm 4 response 的 custom small-q decryption。输入是
  // load_resp_from_stream 恢复出的 single-limb coefficient-form ciphertext。
  // 其中 ternary secret key 原本在旧 full-q limb 下生成，所以 multiplication 前
  // get_sk_ntt_small_q 会把 -1 == old_q-1 改写为 small_q-1。随后计算
  // phase = c0 + c1*s (mod small_q)，plaintext = round(phase*t/small_q)。
  constexpr size_t N = DBConsts::PolyDegree;
  const uint64_t q = pir_params_.get_small_q();
  const uint64_t t = pir_params_.get_plain_mod();

  std::vector<uint64_t> phase(N);
  std::vector<uint64_t> c0(N), c1_ntt(N);
  // Reduce mod q in case mod_switch_inplace produced values = q (from rounding)
  for (size_t i = 0; i < N; i++) {
    c0[i] = ct.c0[i] % q;
    c1_ntt[i] = ct.c1[i] % q;
  }
  std::vector<uint64_t> sk_ntt_small_q = get_sk_ntt_small_q(pir_params_.get_rns_mods()[0], q);
  utils::ntt_fwd(c1_ntt.data(), N, q);
  intel::hexl::EltwiseMultMod(phase.data(), c1_ntt.data(), sk_ntt_small_q.data(), N, q, 1);
  utils::ntt_inv(phase.data(), N, q);
  intel::hexl::EltwiseAddMod(phase.data(), phase.data(), c0.data(), N, q);

  RlwePt result;
  result.data.assign(N, 0);
  const uint64_t delta = q / t;
  const uint64_t half_q = q / 2;
  uint64_t max_noise = 0;

  for (size_t i = 0; i < N; i++) {
    uint64_t m = utils::round_div_u128((uint128_t)phase[i] * t, q) % t;
    result.data[i] = m;

    uint64_t approx = utils::round_div_u128((uint128_t)q * m, t) % q;
    uint64_t noise_pos = (phase[i] >= approx) ? (phase[i] - approx) : (q - approx + phase[i]);
    uint64_t noise_abs = (noise_pos > half_q) ? (q - noise_pos) : noise_pos;
    if (noise_abs > max_noise) max_noise = noise_abs;
  }

  budget_out = (max_noise > 0)
    ? static_cast<int>(std::log2(static_cast<double>(q) / (2.0 * t * max_noise)))
    : static_cast<int>(std::log2(static_cast<double>(q) / (2.0 * t)));
  max_noise_out = max_noise;
  return result;
}


std::vector<int64_t> PirClient::decrypt_mod_q_errors(const RlweCt &ct) const {
  // Mirrors decrypt_mod_q_measured, but keeps the sign and every coefficient.
  constexpr size_t N = DBConsts::PolyDegree;
  const uint64_t q = pir_params_.get_small_q();
  const uint64_t t = pir_params_.get_plain_mod();
  std::vector<uint64_t> phase(N), c0(N), c1_ntt(N);
  for (size_t i = 0; i < N; i++) {
    c0[i] = ct.c0[i] % q;
    c1_ntt[i] = ct.c1[i] % q;
  }
  std::vector<uint64_t> sk_ntt_small_q = get_sk_ntt_small_q(pir_params_.get_rns_mods()[0], q);
  utils::ntt_fwd(c1_ntt.data(), N, q);
  intel::hexl::EltwiseMultMod(phase.data(), c1_ntt.data(), sk_ntt_small_q.data(), N, q, 1);
  utils::ntt_inv(phase.data(), N, q);
  intel::hexl::EltwiseAddMod(phase.data(), phase.data(), c0.data(), N, q);
  const uint64_t half_q = q / 2;
  std::vector<int64_t> errors(N);
  for (size_t i = 0; i < N; i++) {
    const uint64_t m = utils::round_div_u128((uint128_t)phase[i] * t, q) % t;
    const uint64_t approx = utils::round_div_u128((uint128_t)q * m, t) % q;
    const uint64_t diff = (phase[i] >= approx) ? (phase[i] - approx) : (q - approx + phase[i]);
    errors[i] = (diff > half_q) ? -static_cast<int64_t>(q - diff) : static_cast<int64_t>(diff);
  }
  return errors;
}

std::vector<uint64_t> PirClient::get_sk_ntt_small_q(uint64_t old_q, uint64_t small_q) const {
  constexpr size_t N = DBConsts::PolyDegree;

  // rlwe_sk_ is K-limb in NTT form (limb k under q_k). The first limb under
  // q_0 = old_q is what we need; ternary coefficients reduce identically across
  // limbs so the first limb's coefficient form recovers {-1, 0, 1}.
  std::vector<uint64_t> sk_coef(rlwe_sk_.data.begin(),
                                rlwe_sk_.data.begin() + N);
  utils::ntt_inv(sk_coef.data(), N, old_q);

  // Rewrite -1 mod old_q as -1 mod small_q (sk is ternary: {0, 1, -1}).
  std::vector<uint64_t> sk_ntt_small_q_(N);
  for (size_t i = 0; i < N; i++) {
    sk_ntt_small_q_[i] = (sk_coef[i] > 1) ? (small_q - 1) : sk_coef[i];
  }
  utils::ntt_fwd(sk_ntt_small_q_.data(), N, small_q);
  return sk_ntt_small_q_;
}
