#pragma once

#include "tests.h"
#include "tree_superrecord_codec.h"
#include "tree_superrecord_index.h"
#include "tree_superrecord_planner.h"

#include <bit>
#include <optional>
#include <stdexcept>
#include <vector>

// Shared fixtures for the generic super-record tests.

namespace tree_superrecord_test {

using namespace tree_superrecord;

template <typename Fn>
bool throws_invalid(Fn &&fn) {
  try {
    fn();
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

inline size_t payload_bits(const PirParams &scheme) {
  return static_cast<size_t>(std::bit_width(scheme.get_plain_mod())) - 1;
}

// Every expansion height from 5 up to log2 n, alignment 1, one response,
// session keys registered at the full ring height.
inline PlannerPolicy test_policy(size_t max_responses = 1,
                                 size_t max_arity = 64) {
  PlannerPolicy policy;
  policy.max_response_ciphertexts = max_responses;
  policy.max_bundle_arity = max_arity;
  const size_t log_n = std::bit_width(DBConsts::PolyDegree) - 1;
  for (size_t h = 5; h <= log_n; ++h) policy.expansion_heights.push_back(h);
  policy.alpha_alignments = {1};
  policy.eval_key_registration = EvalKeyRegistrationPolicy::FixedMaximumHeight;
  policy.fixed_session_key_height = log_n;
  return policy;
}

inline PlannerInput make_input(const PirParams &scheme, size_t entry_bits,
                               size_t leaf_count, PlannerPolicy policy) {
  PlannerInput input;
  input.entry_bits = entry_bits;
  input.leaf_count = leaf_count;
  input.payload_bits_per_coeff = payload_bits(scheme);
  input.policy = std::move(policy);
  input.scheme_identity = scheme.scheme_identity();
  return input;
}

// First candidate with the given (h, G, b, alignment); the smallest feasible
// expansion height unless one is requested.
inline std::optional<Candidate> pick_candidate(
    const std::vector<Candidate> &all, size_t h, size_t G, size_t b,
    size_t alignment = 1, std::optional<size_t> height = std::nullopt) {
  for (const Candidate &c : all) {
    if (c.shape.bundle_arity == h && c.shape.record_coeffs == G &&
        c.shape.beta_bits == b && c.shape.alpha_alignment == alignment &&
        (!height || c.shape.expansion_height == *height)) {
      return c;
    }
  }
  return std::nullopt;
}

inline TreeParams plan_for(const PirParams &scheme, size_t entry_bits,
                           size_t leaf_count, size_t h, size_t G, size_t b,
                           PlannerPolicy policy) {
  const std::vector<Candidate> all =
      enumerate_candidates(make_input(scheme, entry_bits, leaf_count, policy));
  const std::optional<Candidate> found = pick_candidate(all, h, G, b);
  if (!found) {
    throw std::runtime_error("test shape is not a feasible candidate");
  }
  return freeze_candidate(*found);
}

inline uint64_t splitmix64(uint64_t &state) {
  uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// Deterministic E-bit entry for (level, node); `salt` distinguishes updated
// database contents from the original.
inline EntryBits synthetic_entry(size_t entry_bits, size_t level, size_t node,
                                 uint64_t salt = 0) {
  EntryBits entry = zero_entry(entry_bits);
  uint64_t state = 0x53757065725265ULL ^ salt ^
                   (static_cast<uint64_t>(level) * 0xD6E8FEB86659FD93ULL) ^
                   (static_cast<uint64_t>(node) * 0xA5A3564E27F8862FULL);
  uint64_t word = 0;
  for (size_t bit = 0; bit < entry_bits; ++bit) {
    if (bit % 64 == 0) word = splitmix64(state);
    if ((word >> (bit % 64)) & 1U) set_bit(entry, bit, 1);
  }
  return entry;
}

}  // namespace tree_superrecord_test
