#include "utils.h"
#include "hexl/hexl.hpp"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace {
size_t g_custom_N = 0;
uint64_t g_custom_q = 0;
uint64_t g_custom_root = 0;

intel::hexl::NTT &get_ntt(size_t N, uint64_t q) {
  struct Key {
    size_t N; uint64_t q;
    bool operator==(const Key &o) const noexcept { return N == o.N && q == o.q; }
  };
  struct Hash {
    size_t operator()(const Key &k) const noexcept {
      return std::hash<size_t>()(k.N) ^ (std::hash<uint64_t>()(k.q) * 0x9E3779B97F4A7C15ULL);
    }
  };
  static std::unordered_map<Key, std::unique_ptr<intel::hexl::NTT>, Hash> cache;
  auto it = cache.find({N, q});
  if (it != cache.end()) return *it->second;
  auto ntt = (N == g_custom_N && q == g_custom_q)
                 ? std::make_unique<intel::hexl::NTT>(N, q, g_custom_root)
                 : std::make_unique<intel::hexl::NTT>(N, q);
  auto ins = cache.emplace(Key{N, q}, std::move(ntt));
  return *ins.first->second;
}
}

void utils::register_ntt_root(size_t N, uint64_t q, uint64_t root) {
  if (g_custom_N != 0 &&
      (g_custom_N != N || g_custom_q != q || g_custom_root != root)) {
    throw std::invalid_argument(
        "register_ntt_root: a different (N, q, root) is already registered");
  }
  g_custom_N = N;
  g_custom_q = q;
  g_custom_root = root;
}

void utils::automorphism_coeff(const uint64_t *in, size_t N, uint32_t k,
                               uint64_t q, uint64_t *out) {
  const size_t two_n = 2 * N;
  for (size_t i = 0; i < N; i++) {
    size_t dest = (static_cast<uint64_t>(i) * k) % two_n;
    if (dest < N) {
      out[dest] = in[i];
    } else {
      out[dest - N] = (in[i] == 0) ? 0 : (q - in[i]);
    }
  }
}

void utils::automorphism_ntt(const uint64_t *in, size_t N, uint32_t k,
                              uint64_t q, uint64_t *out) {
  std::vector<uint64_t> coeff(in, in + N);
  ntt_inv(coeff.data(), N, q);
  automorphism_coeff(coeff.data(), N, k, q, out);
  ntt_fwd(out, N, q);
}

void utils::ntt_fwd(uint64_t *data, size_t N, uint64_t q) {
  get_ntt(N, q).ComputeForward(data, data, 1, 1);
}

void utils::ntt_inv(uint64_t *data, size_t N, uint64_t q) {
  get_ntt(N, q).ComputeInverse(data, data, 1, 1);
}

void utils::negacyclic_shift_poly_coeffmod(const uint64_t *poly, size_t coeff_count,
                                           size_t shift, uint64_t modulus,
                                           uint64_t *result) {
  if (shift == 0) {
    std::memcpy(result, poly, coeff_count * sizeof(uint64_t));
    return;
  }

  uint64_t index_raw = shift;
  const uint64_t coeff_count_mod_mask = static_cast<uint64_t>(coeff_count) - 1;
  for (size_t i = 0; i < coeff_count; i++, poly++, index_raw++) {
    uint64_t index = index_raw & coeff_count_mod_mask;
    if (!(index_raw & static_cast<uint64_t>(coeff_count)) || !*poly) {
      result[index] = *poly;
    } else {
      result[index] = modulus - *poly;
    }
  }
}
