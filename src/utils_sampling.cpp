#include "utils.h"

#include <cmath>
#include <random>

// ---------------------------------------------------------------------------
// Polynomial noise / randomness samplers
// ---------------------------------------------------------------------------

void utils::sample_gaussian(uint64_t *out, size_t N, uint64_t q, double sigma,
                             ChaChaRng &rng) {
  std::normal_distribution<double> dist(0.0, sigma);
  for (size_t i = 0; i < N; i++) {
    int64_t e = std::llround(dist(rng));
    if (e >= 0) {
      out[i] = static_cast<uint64_t>(e) % q;
    } else {
      uint64_t abs_e = static_cast<uint64_t>(-e) % q;
      out[i] = (abs_e == 0) ? 0 : (q - abs_e);
    }
  }
}

void utils::sample_uniform_poly(uint64_t *out, size_t N, uint64_t q,
                                 ChaChaRng &rng) {
  // Rejection sampling to avoid modular bias.
  const uint64_t limit = (~uint64_t{0}) - ((~uint64_t{0}) % q);
  for (size_t i = 0; i < N; i++) {
    uint64_t r;
    do { r = rng(); } while (r >= limit);
    out[i] = r % q;
  }
}

void utils::sample_ternary(uint64_t *out, size_t N, uint64_t q,
                            ChaChaRng &rng) {
  // Each coefficient is independently and uniformly in {0, 1, 2}.
  // Map: 0 → 0, 1 → 1, 2 → q-1 (represents -1 mod q).
  std::uniform_int_distribution<int> dist(0, 2);
  for (size_t i = 0; i < N; i++) {
    int v = dist(rng);
    if (v == 0)      out[i] = 0;
    else if (v == 1) out[i] = 1;
    else             out[i] = q - 1;
  }
}

