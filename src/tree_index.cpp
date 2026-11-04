#include "tree_index.h"

#include <bit>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::invalid_argument(std::string("TreePirParams: ") + message);
  }
}

size_t exact_log2(size_t value, const char *name) {
  if (!std::has_single_bit(value)) {
    throw std::invalid_argument(std::string("TreePirParams: ") + name +
                                " must be a power of two");
  }
  return static_cast<size_t>(std::bit_width(value)) - 1;
}

size_t checked_packed_width(size_t N0, size_t ell_beta, size_t b,
                            size_t ell_gamma, size_t r) {
  const auto mul = [](size_t x, size_t y) {
    if (x != 0 && y > std::numeric_limits<size_t>::max() / x) {
      throw std::invalid_argument("TreePirParams: packed width overflows");
    }
    return x * y;
  };
  const auto add = [](size_t x, size_t y) {
    if (y > std::numeric_limits<size_t>::max() - x) {
      throw std::invalid_argument("TreePirParams: packed width overflows");
    }
    return x + y;
  };
  return add(N0, add(mul(ell_beta, b), mul(ell_gamma, r)));
}

}

TreePirParams make_tree_pir_params(size_t L, size_t a, size_t n,
                                   size_t ell_beta, size_t ell_gamma,
                                   uint64_t t,
                                   std::vector<uint64_t> rns_moduli) {
  return make_tree_pir_params(L, a, n, 1, ell_beta, ell_gamma, t,
                              std::move(rns_moduli));
}

TreePirParams make_tree_pir_params(size_t L, size_t a, size_t n, size_t g,
                                   size_t ell_beta, size_t ell_gamma,
                                   uint64_t t,
                                   std::vector<uint64_t> rns_moduli) {
  require(L > 0 && L < std::numeric_limits<size_t>::digits - 1,
          "tree height is out of range");
  require(a < std::numeric_limits<size_t>::digits - 1,
          "first-dimension width is out of range");
  require(n > 0, "ring degree must be positive");
  require(g > 0 && n % g == 0 && n / g >= 2,
          "g must divide n with at least two records per plaintext");
  require(ell_beta > 0 && ell_gamma > 0, "gadget lengths must be positive");

  TreePirParams params;
  params.L = L;
  params.N = size_t{1} << L;
  params.n = n;
  params.g = g;
  params.rho = n / g;
  params.r = exact_log2(params.rho, "records per plaintext");
  params.a = a;
  params.N0 = size_t{1} << a;

  require(params.L >= params.r + params.a,
          "tree height is below r + a; no beta bits remain");
  params.b = params.L - params.r - params.a;
  params.P = params.N / params.rho;
  params.B = size_t{1} << params.b;

  params.ell_beta = ell_beta;
  params.ell_gamma = ell_gamma;
  params.w = checked_packed_width(params.N0, ell_beta, params.b, ell_gamma,
                                  params.r);
  require(params.w <= params.n, "packed width exceeds the ring degree");
  params.W = std::bit_ceil(params.w);
  params.h_q = static_cast<size_t>(std::bit_width(params.W)) - 1;

  params.t = t;
  params.rns_moduli = rns_moduli;
  params.response_degree = n;
  params.response_moduli = std::move(rns_moduli);

  validate_tree_params(params);
  return params;
}

void validate_tree_params(const TreePirParams &params) {
  require(params.L > 0 && params.L < std::numeric_limits<size_t>::digits - 1,
          "tree height is out of range");
  require(params.a < std::numeric_limits<size_t>::digits - 1,
          "first-dimension width is out of range");
  require(std::has_single_bit(params.N), "N must be a power of two");
  require(std::has_single_bit(params.n), "n must be a power of two");
  require(std::has_single_bit(params.N0), "N0 must be a power of two");
  require(params.N == size_t{1} << params.L, "N must equal 2^L");
  require(params.N0 == size_t{1} << params.a, "N0 must equal 2^a");

  require(std::has_single_bit(params.g), "g must be a power of two");
  require(params.g <= params.n / 2, "g must leave at least two records");
  require(params.rho == params.n / params.g, "rho must equal n / g");
  require(params.r == exact_log2(params.rho, "rho"), "r must equal log2(rho)");
  require(params.rho <= params.N, "rho must fit below the leaf count");
  require(params.N0 >= 2, "N0 must be at least 2");
  require(params.N0 <= params.N / params.rho, "N0 exceeds N / rho");
  require(params.N % (params.rho * params.N0) == 0,
          "N must be divisible by rho * N0");

  require(params.L >= params.r + params.a, "L is below r + a");
  require(params.b == params.L - params.r - params.a, "b must be L - r - a");
  require(params.P == params.N / params.rho, "P must equal N / rho");
  require(params.B == size_t{1} << params.b, "B must equal 2^b");

  require(params.ell_beta > 0 && params.ell_gamma > 0,
          "gadget lengths must be positive");
  require(params.w == checked_packed_width(params.N0, params.ell_beta,
                                           params.b, params.ell_gamma,
                                           params.r),
          "w must equal N0 + ell_beta*b + ell_gamma*r");
  require(params.w <= params.n, "packed width exceeds the ring degree");
  require(params.W == std::bit_ceil(params.w),
          "W must be the next power of two of w");
  require(params.h_q == static_cast<size_t>(std::bit_width(params.W)) - 1,
          "h_q must equal log2(W)");
  require(params.W <= params.n, "W exceeds the ring degree");

  require(std::gcd(params.W, params.t) == 1,
          "W must be invertible modulo t");
  for (uint64_t qk : params.rns_moduli) {
    require((qk & 1ULL) == 1ULL, "every RNS modulus must be odd");
    require(std::gcd<uint64_t>(params.W, qk) == 1,
            "W must be invertible modulo every RNS modulus");
  }

  require(params.response_degree == params.n,
          "MVP response ring must equal the main ring");
  require(params.response_moduli == params.rns_moduli,
          "MVP response moduli must equal the main moduli");
}

ClientCoordinates client_index(size_t leaf, const TreePirParams &params) {
  if (leaf >= params.N) {
    throw std::invalid_argument("client_index leaf is out of range");
  }
  const size_t gamma = leaf / params.P;
  const size_t p = leaf % params.P;
  ClientCoordinates coords;
  coords.alpha = p / params.B;
  const size_t beta = p % params.B;
  coords.beta_bits_le.resize(params.b);
  for (size_t u = 0; u < params.b; ++u) {
    coords.beta_bits_le[u] = static_cast<uint8_t>((beta >> u) & 1U);
  }
  coords.gamma_bits_le.resize(params.r);
  for (size_t v = 0; v < params.r; ++v) {
    coords.gamma_bits_le[v] = static_cast<uint8_t>((gamma >> v) & 1U);
  }
  return coords;
}

std::vector<LevelPlan> build_level_plans(const TreePirParams &params) {
  std::vector<LevelPlan> plans;
  plans.reserve(params.L + 1);
  for (size_t level = 0; level <= params.L; ++level) {
    LevelPlan plan;
    plan.level = level;
    plan.R = level >= params.r ? size_t{1} << (level - params.r) : size_t{1};

    if (plan.R == 1) {
      plan.select_case = SelectCase::Single;
      plan.coarsen_count = params.a;
    } else if (plan.R < params.N0) {
      plan.select_case = SelectCase::CoarsenedAlpha;
      plan.coarsen_count = params.a - (level - params.r);
    } else {
      plan.select_case = SelectCase::AlphaBeta;
      plan.coarsen_count = 0;
    }

    if (plan.select_case == SelectCase::AlphaBeta) {
      const size_t s = params.L - level;
      plan.beta_begin = s;
      plan.beta_count = level - params.r - params.a;
    } else {
      plan.beta_begin = params.b;
      plan.beta_count = 0;
    }

    if (level >= params.r) {
      plan.gamma_begin = 0;
      plan.gamma_count = params.r;
    } else {
      plan.gamma_begin = params.r - level;
      plan.gamma_count = level;
    }

    plan.projection_depth = level < params.r ? level : params.r;
    plans.push_back(plan);
  }
  return plans;
}

LevelOracle build_level_oracle_for_test(size_t leaf, size_t level,
                                        const TreePirParams &params) {
  if (leaf >= params.N) {
    throw std::invalid_argument("level oracle leaf is out of range");
  }
  if (level > params.L) {
    throw std::invalid_argument("level oracle level is out of range");
  }
  LevelOracle oracle;
  oracle.node_index = leaf >> (params.L - level);
  const size_t gamma = leaf / params.P;
  const size_t p = leaf % params.P;
  if (level >= params.r) {
    oracle.packed_plaintext_index = p >> (params.L - level);
    oracle.record_position = gamma;
  } else {
    oracle.packed_plaintext_index = 0;
    oracle.record_position = gamma >> (params.r - level);
  }
  return oracle;
}

size_t level_record_position_from_coordinates(size_t alpha, size_t beta,
                                              size_t level,
                                              const TreePirParams &params) {
  if (level < params.r || level > params.L) {
    throw std::invalid_argument(
        "record position formula is defined for levels in [r, L]");
  }
  const size_t s = params.L - level;
  if (s <= params.b) {
    return alpha * (size_t{1} << (params.b - s)) + (beta >> s);
  }
  return alpha >> (s - params.b);
}
