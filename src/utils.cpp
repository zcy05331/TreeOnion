#include "utils.h"

#include <bit>
#include <limits>
#include <stdexcept>
#include <unordered_map>

// Extended Euclidean algorithm: given a, b > 0, returns (gcd, s) such that
// s * a + t * b = gcd.  We only return s since that's the Bezout coefficient
// for `value` in try_invert_uint_mod.
// For moduli < 2^62 the coefficients stay within int64_t range.
static int64_t xgcd(int64_t a, int64_t b, int64_t &s) {
  int64_t s0 = 1, s1 = 0;
  int64_t r0 = a, r1 = b;
  while (r1 != 0) {
    int64_t q = r0 / r1;
    int64_t tmp = r1; r1 = r0 - q * r1; r0 = tmp;
    tmp = s1; s1 = s0 - q * s1; s0 = tmp;
  }
  s = s0;
  return r0; // gcd
}

// (a * b) mod m without overflow, via 128-bit multiply.
static inline uint64_t mulmod_u64(uint64_t a, uint64_t b, uint64_t m) {
  return static_cast<uint64_t>((static_cast<__uint128_t>(a) * b) % m);
}

// (base^exp) mod m via binary exponentiation.
static uint64_t powmod_u64(uint64_t base, uint64_t exp, uint64_t m) {
  uint64_t result = 1 % m;
  base %= m;
  while (exp > 0) {
    if (exp & 1) result = mulmod_u64(result, base, m);
    base = mulmod_u64(base, base, m);
    exp >>= 1;
  }
  return result;
}

// Miller-Rabin witness check: returns true if `a` is a witness that n is composite.
static bool mr_composite_witness(uint64_t n, uint64_t d, int r, uint64_t a) {
  uint64_t x = powmod_u64(a, d, n);
  if (x == 1 || x == n - 1) return false;
  for (int i = 0; i < r - 1; ++i) {
    x = mulmod_u64(x, x, n);
    if (x == n - 1) return false;
  }
  return true;
}

bool utils::is_prime(uint64_t n) {
  if (n < 2) return false;
  // Small-prime shortcut (also handles witnesses equal to n).
  static constexpr uint64_t small_primes[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
  for (uint64_t p : small_primes) {
    if (n == p) return true;
    if (n % p == 0) return false;
  }
  // Write n - 1 = d * 2^r with d odd.
  uint64_t d = n - 1;
  int r = 0;
  while ((d & 1) == 0) { d >>= 1; ++r; }
  // Deterministic for all 64-bit n.
  for (uint64_t a : small_primes) {
    if (mr_composite_witness(n, d, r, a)) return false;
  }
  return true;
}

bool utils::try_invert_uint_mod(uint64_t value, uint64_t modulus, uint64_t &result) {
  if (value == 0) return false;
  int64_t s;
  int64_t g = xgcd(static_cast<int64_t>(value), static_cast<int64_t>(modulus), s);
  if (g != 1) return false;
  result = (s < 0) ? static_cast<uint64_t>(s + static_cast<int64_t>(modulus))
                   : static_cast<uint64_t>(s);
  return true;
}


std::vector<std::vector<uint64_t>> utils::gsw_gadget(size_t l, uint64_t base_log2,
                const std::vector<uint64_t> &rns_mods, size_t low_bits) {
  if (l == 0) {
    throw std::invalid_argument("gsw_gadget: gadget length must be positive");
  }
  // Create RGSW gadget. Rows are MSB-first: row p is
  // 2^(low_bits + base_log2 * (l-1-p)).
  // QueryPack, RGSW encryption, and external-product decomposition must use
  // this same row order or gadget digits will multiply the wrong powers.
  const size_t K = rns_mods.size();
  std::vector<std::vector<uint64_t>> gadget(K, std::vector<uint64_t>(l));
  for (size_t i = 0; i < K; i++) {
    const uint64_t mod = rns_mods[i];
    uint64_t pow = 1 % mod;
    for (size_t bit = 0; bit < low_bits; ++bit) {
      pow = static_cast<uint64_t>((static_cast<uint128_t>(pow) << 1) % mod);
    }
    for (int j = l - 1; j >= 0; j--) {
      gadget[i][j] = pow;
      pow = static_cast<uint64_t>((static_cast<uint128_t>(pow) << base_log2) % mod);
    }
  }
  return gadget;
}

uint64_t utils::crt_combine(uint64_t w1, uint64_t q1,
                             uint64_t w2, uint64_t q2) {
  // Garner: w = w1 + q1 * ((w2 - w1) * q1^{-1} mod q2) ∈ [0, q1*q2).
  uint64_t q1_inv;
  if (!try_invert_uint_mod(q1 % q2, q2, q1_inv))
    throw std::invalid_argument("crt_combine: q1, q2 must be coprime");
  const uint64_t diff = (w2 + q2 - (w1 % q2)) % q2;
  const uint64_t k = mulmod_u64(diff, q1_inv, q2);
  return static_cast<uint64_t>(static_cast<uint128_t>(q1) * k + w1);
}

/**
 * @brief Generate the smallest prime that is at least bit_width bits long.
 * @param bit_width >= 2 and <= 64
 * @return std::uint64_t
 */
std::uint64_t utils::generate_prime(size_t bit_width) {
  if (bit_width < 2) throw std::invalid_argument("Bit width must be at least 2.");

  // Otherwise, generate a new prime
  std::uint64_t candidate = 1ULL << (bit_width - 1);
  do {
      candidate++;
      // Ensure candidate is odd, as even numbers greater than 2 cannot be prime
      candidate |= 1;
  } while (!utils::is_prime(candidate));
  return candidate;
}

std::vector<uint64_t> utils::generate_ntt_friendly_primes(
    const std::vector<size_t> &bit_widths, size_t N) {
  // For each bit width bw, return the largest prime p < 2^bw with p ≡ 1 mod 2N.
  // If the same bit width appears more than once, return distinct primes by
  // continuing the downward scan from where we stopped last time.
  const uint64_t step = 2 * static_cast<uint64_t>(N);
  std::unordered_map<size_t, uint64_t> next_candidate;
  std::vector<uint64_t> out;
  out.reserve(bit_widths.size());

  for (size_t bw : bit_widths) {
    if (bw < 2 || bw > 63)
      throw std::invalid_argument("bit width out of range [2, 63]");

    auto it = next_candidate.find(bw);
    uint64_t c;
    if (it == next_candidate.end()) {
      const uint64_t upper = (uint64_t{1} << bw);   // exclusive
      // Largest value < 2^bw with value ≡ 1 mod 2N.
      c = upper - 1;
      c -= ((c - 1) % step);
    } else {
      c = it->second;
    }

    while (true) {
      if (c < 2) throw std::runtime_error("No NTT-friendly prime found");
      if (utils::is_prime(c)) break;
      c -= step;
    }
    out.push_back(c);
    next_candidate[bw] = (c >= step) ? (c - step) : 0;
  }
  return out;
}

size_t utils::next_pow_of_2(const size_t n) {
  size_t p = 1;
  while (p < n) {
    p <<= 1;
  }
  return p;
}

size_t utils::roundup_div(const size_t numerator, const size_t denominator) {
  if (denominator == 0) {
    throw std::invalid_argument("roundup_div division by zero");
  }
  return (numerator + denominator - 1) / denominator;
}


// Pick (fst_dim_sz, num_dims) so the database holds ≥ target_num_pt plaintexts
// and the expansion output fits the chosen shape.
//
// The query expansion tree of height h produces capacity = 2^h BFV ciphertexts.
// Of those, the first num_dims - 1 "other" dimensions are each binary muxes
// driven by a full l-row GSW gadget reconstruction, so they consume
//   reserved = l * (num_dims - 1)
// expansion slots. The remaining
//   slack    = capacity - reserved
// slots feed the first dimension. fst_dim_sz is set from slack per the
// FST_DIM_POW2 policy. Database capacity (in plaintexts) is then
//   fst_dim_sz · 2^(num_dims - 1)
// since each "other" dim is binary and doubles the addressable plaintexts.
//
// Loop walks num_dims upward and returns the smallest one that meets target.
// max_num_dims is just a loop bound; the inner `reserved >= capacity` break
// is the real correctness guard against size_t underflow.
//
// Args:
//   target_num_pt: minimum plaintexts the DB must hold (= DB_SIZE_MB / pt size).
//   l            : GSW gadget length used for "other"-dim reconstruction (= l_ep).
//   h            : expansion tree height (= TREE_HEIGHT).
// Returns: {fst_dim_sz, num_dims}.
std::pair<size_t, size_t> utils::calculate_db_shape(
    size_t target_num_pt, size_t l, size_t h, bool fst_dim_pow2) {
  if (target_num_pt == 0) {
    throw std::invalid_argument("calculate_db_shape target must be positive");
  }
  if (l == 0) {
    throw std::invalid_argument("calculate_db_shape gadget length must be positive");
  }
  if (h >= std::numeric_limits<size_t>::digits) {
    throw std::invalid_argument("calculate_db_shape height is too large");
  }
  const size_t capacity = size_t{1} << h;
  const size_t max_num_dims = 1 + (capacity - 1) / l;
  for (size_t num_dims = 1; num_dims <= max_num_dims + 1; num_dims++) {
    const size_t reserved = l * (num_dims - 1);
    if (reserved >= capacity) break;  // guard size_t underflow
    const size_t slack = capacity - reserved;
    const size_t fst_dim_sz = fst_dim_pow2
        ? (size_t{1} << (std::bit_width(slack) - 1))
        : slack;
    if (fst_dim_sz * (size_t{1} << (num_dims - 1)) >= target_num_pt) {
      return {fst_dim_sz, num_dims};
    }
  }
  throw std::runtime_error("Failed to calculate database shape");
}

uint64_t utils::rescale(uint64_t a, uint64_t inp_mod, uint64_t out_mod) {
  const int64_t inp_mod_i64 = static_cast<int64_t>(inp_mod);
  const __int128 out_mod_i128 = static_cast<__int128>(out_mod);

  // Center first, then round by out_mod/inp_mod. Unsigned scaling would map
  // representatives near inp_mod as large positives and corrupt negative noise.
  int64_t v = static_cast<int64_t>(a % inp_mod);
  if (v >= inp_mod_i64 / 2) v -= inp_mod_i64;

  const int64_t sign = (v >= 0) ? 1 : -1;
  __int128 val = static_cast<__int128>(v) * static_cast<__int128>(out_mod);
  __int128 r = (val + static_cast<__int128>(sign * (inp_mod_i64 / 2))) /
               static_cast<__int128>(inp_mod);

  r = (r + static_cast<__int128>((inp_mod / out_mod) * out_mod) +
       2 * out_mod_i128) % out_mod_i128;
  return static_cast<uint64_t>((r + out_mod_i128) % out_mod_i128);
}
