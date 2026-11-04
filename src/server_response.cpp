#include "server.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>

bool PirServer::switch_response_to_small_q(RlweCt &response) {
  if (DBConsts::SmallQWidth >= DBConsts::RnsMods[0]) {
    return false;
  }
  mod_switch_inplace(response, pir_params_.get_small_q());
  return true;
}

size_t PirServer::save_resp_to_stream(const RlweCt &response,
                                      std::stringstream &stream) {
  const size_t small_q = pir_params_.get_small_q();
  const size_t small_q_width =
      static_cast<size_t>(std::ceil(std::log2(small_q)));
  constexpr size_t coeff_count = DBConsts::PolyDegree;

  uint8_t byte_buf = 0;
  size_t bits_filled = 0;
  size_t total_bytes = 0;

  auto flush_byte = [&]() {
    stream.put(static_cast<char>(byte_buf));
    ++total_bytes;
    byte_buf = 0;
    bits_filled = 0;
  };

  for (size_t poly_id = 0; poly_id < 2; ++poly_id) {
    const uint64_t *data = response.data(poly_id);

    for (size_t i = 0; i < coeff_count; ++i) {
      uint64_t coeff = data[i] & ((1ULL << small_q_width) - 1);
      size_t bits_written = 0;

      while (bits_written < small_q_width) {
        const size_t room = 8 - bits_filled;
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

  if (bits_filled != 0)
    flush_byte();

  return total_bytes;
}

void PirServer::mod_switch_inplace(RlweCt &ciphertext, const uint64_t q) {
  constexpr size_t coeff_count = DBConsts::PolyDegree;
  constexpr size_t K = DBConsts::RnsMods.size();
  const auto &qs = pir_params_.get_rns_mods();

  if constexpr (K == 1) {
    const uint64_t Q = qs[0];
    uint64_t *data0 = ciphertext.c0.data();
    uint64_t *data1 = ciphertext.c1.data();
    for (size_t i = 0; i < coeff_count; i++) {
      data0[i] = utils::rescale(data0[i], Q, q);
      data1[i] = utils::rescale(data1[i], Q, q);
    }
  } else {
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
    ciphertext.c0.resize(coeff_count);
    ciphertext.c1.resize(coeff_count);
  }
}
