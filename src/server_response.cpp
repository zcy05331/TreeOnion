#include "server.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>

bool PirServer::switch_response_to_small_q(RlweCt &response) {
  // 守卫：SmallQWidth 与 RnsMods[0] 都是位宽；只有 q' 严格窄于首 limb 时切换才是
  // 真正的降模压缩，否则告知调用方保持全 q（与 make_query 尾段的判定一致）。
  if (DBConsts::SmallQWidth >= DBConsts::RnsMods[0]) {
    return false;
  }
  // 复用生产路径的 mod_switch_inplace 做居中重缩放，保证树路径与 flat 路径的
  // 噪声/编码行为逐字节一致。
  mod_switch_inplace(response, pir_params_.get_small_q());
  return true;
}

size_t PirServer::save_resp_to_stream(const RlweCt &response,
                                      std::stringstream &stream) {
  // 这里是 post-ModSwitch ciphertext 的 response codec。这里序列化 tests/prototype
  // transport 实际使用的 wire bytes：先 c0 后 c1；每个 coefficient 精确使用
  // small_q_width 个 low bits，并以 LSB-first 写入 byte stream。query/key bytes
  // 不在这里建模；make_query 直接接收 in-memory objects。
  //
  // 在 trust boundary 上，这个 research prototype format 不做 authentication
  // 或 integrity protection；对应 reader side 不检查 payload 后 trailing
  // bytes/padding。

  // --- 1.  Runtime parameters ------------------------------------------------
  const size_t small_q = pir_params_.get_small_q();
  const size_t small_q_width =
      static_cast<size_t>(std::ceil(std::log2(small_q)));
  constexpr size_t coeff_count = DBConsts::PolyDegree;

  // --- 2.  Bit-packing state -------------------------------------------------
  uint8_t byte_buf = 0;   // currently accumulated bits (LSB-first)
  size_t bits_filled = 0; // number of valid bits in byte_buf
  size_t total_bytes = 0; // bytes written so far

  auto flush_byte = [&]() {
    stream.put(static_cast<char>(byte_buf));
    ++total_bytes;
    byte_buf = 0;
    bits_filled = 0;
  };

  // --- 3.  Write every coefficient of the two polynomials -------------------
  for (size_t poly_id = 0; poly_id < 2; ++poly_id) {
    const uint64_t *data = response.data(poly_id);

    for (size_t i = 0; i < coeff_count; ++i) {
      uint64_t coeff = data[i] & ((1ULL << small_q_width) - 1); // keep LS bits only
      size_t bits_written = 0;

      while (bits_written < small_q_width) {
        const size_t room = 8 - bits_filled; // free bits in buffer
        const size_t bits_to_take = std::min(room, small_q_width - bits_written);

        const uint8_t chunk = static_cast<uint8_t>(
            (coeff >> bits_written) & ((1ULL << bits_to_take) - 1));

        byte_buf |= static_cast<uint8_t>(chunk << bits_filled);
        bits_filled += bits_to_take;
        bits_written += bits_to_take;

        if (bits_filled == 8)
          flush_byte();
      }
    }
  }

  // --- 4.  Flush padding byte (if any) --------------------------------------
  if (bits_filled != 0)
    flush_byte();

  return total_bytes;
}

void PirServer::mod_switch_inplace(RlweCt &ciphertext, const uint64_t q) {
  // 所有 homomorphic work 完成后才做 full-q -> small-q centered rescale。
  // 输入是 DBConsts::RnsMods 下的 coefficient-form response data；输出是 `q`
  // 下的 single-limb coefficient-form ciphertext，供 response codec 使用。
  // 不要把它移到 evaluation 前：early switching 会消耗 noise headroom，并让
  // ciphertext 与 full-q MUX/eval path 不兼容。
  constexpr size_t coeff_count = DBConsts::PolyDegree;
  constexpr size_t K = DBConsts::RnsMods.size();
  const auto &qs = pir_params_.get_rns_mods();

  if constexpr (K == 1) {
    // 在 logical K=1 path 中，每个 coefficient 直接 centered-rescale q_full -> q。
    const uint64_t Q = qs[0];
    uint64_t *data0 = ciphertext.c0.data();
    uint64_t *data1 = ciphertext.c1.data();
    for (size_t i = 0; i < coeff_count; i++) {
      data0[i] = utils::rescale(data0[i], Q, q);
      data1[i] = utils::rescale(data1[i], Q, q);
    }
  } else {
    // 在 actual K=2 path 中，先把 high limb q1 rounded-drop 到 q0，再复用 single-limb
    // centered rescale q0 -> q。这不是 composite first-dim q1*q2 helper path；
    // 它操作的是 logical two-RNS-limb response ciphertext，最终收缩成一个
    // small-q limb。
    const uint64_t q0 = qs[0];
    const uint64_t q1 = qs[1];
    const uint64_t q0_inv_mod_q1 = pir_params_.get_rns_tables().q0_inv_mod_q1;

    auto drop_q1 = [&](uint64_t r0, uint64_t r1) -> uint64_t {
      const uint64_t r0_mod_q1 = r0 % q1;
      const uint64_t diff = (r1 + q1 - r0_mod_q1) % q1;
      const uint64_t s = static_cast<uint64_t>(
          (static_cast<uint128_t>(diff) * q0_inv_mod_q1) % q1);
      const uint128_t x = static_cast<uint128_t>(q0) * s + r0;
      uint64_t out = static_cast<uint64_t>((x + (static_cast<uint128_t>(q1) >> 1)) / q1);
      return (out >= q0) ? (out - q0) : out;
    };

    uint64_t *c0_lo = ciphertext.c0.data();
    uint64_t *c0_hi = ciphertext.c0.data() + coeff_count;
    uint64_t *c1_lo = ciphertext.c1.data();
    uint64_t *c1_hi = ciphertext.c1.data() + coeff_count;
    for (size_t i = 0; i < coeff_count; ++i) {
      c0_lo[i] = utils::rescale(drop_q1(c0_lo[i], c0_hi[i]), q0, q);
      c1_lo[i] = utils::rescale(drop_q1(c1_lo[i], c1_hi[i]), q0, q);
    }
    // Output is single-limb under modulus q.
    ciphertext.c0.resize(coeff_count);
    ciphertext.c1.resize(coeff_count);
  }
}
