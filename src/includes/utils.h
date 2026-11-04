#pragma once
#include "pir.h"
#include "rlwe.h"
#include <iostream>
#include <fstream>
#include <random>
#include "csprng.h"


#ifdef _DEBUG
#define PRINT_INT_ARRAY(arr_name, arr, size) \
    do {                                     \
        std::cout << arr_name << ": [";      \
        for (size_t i = 0; i < size; ++i) {     \
            std::cout << arr[i];             \
            if (i < size - 1)                \
                std::cout << ", ";           \
        }                                    \
        std::cout << "]" << std::endl;       \
    } while (0)
#endif

#ifdef _BENCHMARK
#define PRINT_INT_ARRAY(arr_name, arr, size) ;
#endif



inline void print_func_name(std::string func_name) {
  PRINT_BAR;
  #ifdef _DEBUG
    std::cout << "                    "<< func_name << "(Debug build)" << std::endl;
  #endif
  #ifdef _BENCHMARK
    std::cout << "                    "<< func_name << "(Benchmark build)" << std::endl;
  #endif
  PRINT_BAR;
}

template <typename T> std::string to_string(T x) {
  std::string ret;
  if (x == 0) {
    return "0";
  }
  while (x) {
    ret += (x % 10) + '0';
    x /= 10;
  }
  reverse(ret.begin(), ret.end());
  return ret;
}

namespace utils {

bool try_invert_uint_mod(uint64_t value, uint64_t modulus, uint64_t &result);

bool is_prime(uint64_t n);

void automorphism_coeff(const uint64_t *in, size_t N, uint32_t k, uint64_t q, uint64_t *out);

void automorphism_ntt(const uint64_t *in, size_t N, uint32_t k, uint64_t q, uint64_t *out);

inline uint64_t round_div_u128(uint128_t num, uint64_t den) {
  return static_cast<uint64_t>((num + (den >> 1)) / den);
}

inline void right_shift_uint128(uint64_t *operand, int shift, uint64_t *result) {
  if (shift == 0) {
    result[0] = operand[0]; result[1] = operand[1];
  } else if (shift < 64) {
    result[0] = (operand[0] >> shift) | (operand[1] << (64 - shift));
    result[1] = operand[1] >> shift;
  } else {
    result[0] = operand[1] >> (shift - 64);
    result[1] = 0;
  }
}

void ntt_fwd(uint64_t *data, size_t N, uint64_t q);
void ntt_inv(uint64_t *data, size_t N, uint64_t q);

void register_ntt_root(size_t N, uint64_t q, uint64_t root);

uint64_t crt_combine(uint64_t w1, uint64_t q1, uint64_t w2, uint64_t q2);

struct BarrettU64 {
  uint64_t q;
  uint64_t mu;
};

inline BarrettU64 barrett_u64_setup(uint64_t q) {
  const uint64_t mu = (q == 1)
      ? 0
      : static_cast<uint64_t>((static_cast<uint128_t>(1) << 64) / q);
  return BarrettU64{q, mu};
}

inline uint64_t barrett_reduce_u64(uint64_t x, const BarrettU64 &b) {
  if (b.q == 1) return 0;
  const uint64_t q_est = static_cast<uint64_t>(
      (static_cast<uint128_t>(x) * b.mu) >> 64);
  uint64_t r = x - q_est * b.q;
  if (r >= b.q) r -= b.q;
  return r;
}

struct BarrettU128 {
  uint64_t q;
  uint64_t mu_lo;
  uint64_t mu_hi;
};

inline BarrettU128 barrett_u128_setup(uint64_t q) {
  const uint128_t mu = (~static_cast<uint128_t>(0)) / q;
  return BarrettU128{q, static_cast<uint64_t>(mu),
                     static_cast<uint64_t>(mu >> 64)};
}

inline uint64_t barrett_reduce_u128(uint128_t x, const BarrettU128 &b) {
  const uint64_t x_lo = static_cast<uint64_t>(x);
  const uint64_t x_hi = static_cast<uint64_t>(x >> 64);

  const uint64_t carry1 = static_cast<uint64_t>(
      (static_cast<uint128_t>(x_lo) * b.mu_lo) >> 64);

  const uint128_t t1 = static_cast<uint128_t>(x_lo) * b.mu_hi + carry1;
  const uint64_t t1_lo = static_cast<uint64_t>(t1);
  const uint64_t t1_hi = static_cast<uint64_t>(t1 >> 64);

  const uint128_t t2 = static_cast<uint128_t>(x_hi) * b.mu_lo + t1_lo;
  const uint64_t carry2 = static_cast<uint64_t>(t2 >> 64);

  const uint64_t q_est_lo = x_hi * b.mu_hi + t1_hi + carry2;

  uint64_t r = x_lo - q_est_lo * b.q;
  if (r >= b.q) r -= b.q;
  return r;
}

void negacyclic_shift_poly_coeffmod(const uint64_t *poly,
                                    size_t coeff_count, size_t shift,
                                    uint64_t modulus,
                                    uint64_t *result);

std::string uint128_to_string(uint128_t value);

std::vector<std::vector<uint64_t>>
gsw_gadget(size_t l, uint64_t base_log2,
           const std::vector<uint64_t> &rns_mods,
           size_t low_bits = 0);

std::uint64_t generate_prime(size_t bit_width);

std::vector<uint64_t> generate_ntt_friendly_primes(const std::vector<size_t> &bit_widths,
                                                   size_t N);

void print_plaintext(const RlwePt &plaintext, size_t count = 10);

bool plaintext_is_equal(const RlwePt &plaintext1, const RlwePt &plaintext2);

void print_progress(size_t current, size_t total);

size_t next_pow_of_2(const size_t n);

size_t roundup_div(const size_t numerator, const size_t denominator);


void sample_gaussian(uint64_t *out, size_t N, uint64_t q, double sigma, ChaChaRng &rng);

void sample_uniform_poly(uint64_t *out, size_t N, uint64_t q, ChaChaRng &rng);

void sample_ternary(uint64_t *out, size_t N, uint64_t q, ChaChaRng &rng);

uint64_t rescale(uint64_t a, uint64_t inp_mod, uint64_t out_mod);

std::pair<size_t, size_t> calculate_db_shape(
    size_t target_num_pt, size_t l, size_t h,
    bool fst_dim_pow2 = DBConsts::FST_DIM_POW2);

inline size_t bit_reverse(size_t x, size_t logn) {
  size_t n = 1 << logn;
  size_t y = 0;
  for (size_t i = 0; i < logn; i++) {
    y = (y << 1) | (x & 1);
    x >>= 1;
  }
  return y;
  }

  inline size_t repeated_ceil_half(size_t x, size_t k) {
    size_t divisor = 1 << k;
    return (x + divisor - 1) / divisor;
  }


}
