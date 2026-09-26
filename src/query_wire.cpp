#include "query_wire.h"

#include <bit>
#include <stdexcept>

#include "csprng.h"
#include "database_constants.h"
#include "utils.h"

namespace {

using u128 = unsigned __int128;

std::vector<uint64_t> moduli(const PirParams &params) {
  const auto &qs = params.get_rns_mods();
  return std::vector<uint64_t>(qs.begin(), qs.end());
}

// ceil(log2 q) for the composite q, i.e., the bit width of q - 1.
size_t coeff_bits(const std::vector<uint64_t> &qs) {
  u128 q = 1;
  for (uint64_t m : qs) q *= m;
  const u128 top = q - 1;
  const uint64_t hi = static_cast<uint64_t>(top >> 64);
  return hi != 0 ? 64 + std::bit_width(hi)
                 : std::bit_width(static_cast<uint64_t>(top));
}

void require_supported(const std::vector<uint64_t> &qs) {
  if (qs.empty() || qs.size() > 2) {
    throw std::invalid_argument("seeded query codec supports K = 1 or K = 2");
  }
}

// Garner's composition of (x mod q_0, x mod q_1) into x mod q_0 q_1.
struct Crt {
  uint64_t q0 = 0, q1 = 0, q0_inv_mod_q1 = 0;
  bool two = false;

  explicit Crt(const std::vector<uint64_t> &qs) : q0(qs[0]), two(qs.size() == 2) {
    if (!two) return;
    q1 = qs[1];
    if (!utils::try_invert_uint_mod(q0 % q1, q1, q0_inv_mod_q1)) {
      throw std::invalid_argument("RNS limbs must be coprime");
    }
  }

  u128 compose(uint64_t x0, uint64_t x1) const {
    if (!two) return x0;
    const uint64_t d = (x1 + q1 - x0 % q1) % q1;
    const uint64_t h = static_cast<uint64_t>(static_cast<u128>(d) * q0_inv_mod_q1 % q1);
    return static_cast<u128>(x0) + static_cast<u128>(q0) * h;
  }
};

}  // namespace

void expand_query_mask(const QuerySeed &seed, size_t N,
                       const std::vector<uint64_t> &qs, uint64_t *c1) {
  ChaChaRng rng(seed);
  sample_uniform_rns(c1, N, qs, rng);
}

size_t seeded_query_bytes(const PirParams &params) {
  const std::vector<uint64_t> qs = moduli(params);
  require_supported(qs);
  constexpr size_t N = DBConsts::PolyDegree;
  return sizeof(QuerySeed) + (N * coeff_bits(qs) + 7) / 8;
}

size_t save_seeded_query(const RlweCt &query, const QuerySeed &seed,
                         const PirParams &params, std::stringstream &stream) {
  const std::vector<uint64_t> qs = moduli(params);
  require_supported(qs);
  constexpr size_t N = DBConsts::PolyDegree;
  const size_t K = qs.size();
  if (query.ntt_form || query.c0.size() != K * N) {
    throw std::invalid_argument("seeded query must be a coefficient-form RNS ciphertext");
  }
  const size_t bits = coeff_bits(qs);
  const Crt crt(qs);

  size_t written = 0;
  for (uint32_t word : seed) {
    for (int b = 0; b < 4; ++b) stream.put(static_cast<char>(word >> (8 * b)));
    written += 4;
  }

  uint8_t byte = 0;
  size_t filled = 0;
  for (size_t i = 0; i < N; ++i) {
    u128 value = crt.compose(query.c0[i], K == 2 ? query.c0[N + i] : 0);
    for (size_t b = 0; b < bits; ++b) {
      byte |= static_cast<uint8_t>(value & 1) << filled;
      value >>= 1;
      if (++filled == 8) {
        stream.put(static_cast<char>(byte));
        ++written;
        byte = 0;
        filled = 0;
      }
    }
  }
  if (filled != 0) {
    stream.put(static_cast<char>(byte));
    ++written;
  }
  return written;
}

RlweCt load_seeded_query(std::stringstream &stream, const PirParams &params) {
  const std::vector<uint64_t> qs = moduli(params);
  require_supported(qs);
  constexpr size_t N = DBConsts::PolyDegree;
  const size_t K = qs.size();
  const size_t bits = coeff_bits(qs);

  const auto next_byte = [&]() -> uint8_t {
    const int ch = stream.get();
    if (ch == EOF) throw std::runtime_error("unexpected end of query stream");
    return static_cast<uint8_t>(ch);
  };

  QuerySeed seed{};
  for (uint32_t &word : seed) {
    for (int b = 0; b < 4; ++b) word |= static_cast<uint32_t>(next_byte()) << (8 * b);
  }

  RlweCt query;
  query.c0.assign(K * N, 0);
  query.c1.assign(K * N, 0);
  query.ntt_form = false;

  uint8_t byte = 0;
  size_t left = 0;
  for (size_t i = 0; i < N; ++i) {
    u128 value = 0;
    for (size_t b = 0; b < bits; ++b) {
      if (left == 0) {
        byte = next_byte();
        left = 8;
      }
      value |= static_cast<u128>(byte & 1) << b;
      byte >>= 1;
      --left;
    }
    for (size_t k = 0; k < K; ++k) {
      query.c0[k * N + i] = static_cast<uint64_t>(value % qs[k]);
    }
  }
  expand_query_mask(seed, N, qs, query.c1.data());
  return query;
}
