#include "tests.h"
#include "tree_response_merge.h"

#include <array>
#include <bit>
#include <random>

namespace {

size_t reversed_index(size_t index, size_t bits) {
  size_t result = 0;
  for (size_t b = 0; b < bits; ++b) result |= ((index >> b) & 1) << (bits - b - 1);
  return result;
}

}  // namespace

void PirTest::test_tree_response_merge() {
  using namespace tree_superrecord;
  print_func_name(__FUNCTION__);
  PirParams scheme;
  PirClient client(scheme);
  const size_t n = scheme.get_poly_degree();
  const size_t logn = std::bit_width(n) - 1;
  const auto keys = client.create_session_keys(logn);
  const auto &qs = scheme.get_rns_mods();
  const uint64_t t = scheme.get_plain_mod();
  std::mt19937_64 rng(0x4d4c57454d455247ULL);
  const std::array<size_t, 8> offsets{0, 4, 2, 6, 1, 5, 3, 7};
  for (size_t i = 0; i < offsets.size(); ++i) {
    require_test(packed_residue_offset(i, 8, ResponsePacking::MlweMerge) == offsets[i],
                 "MLWE public placement is bit reversed");
    require_test(packed_residue_offset(i, 8, ResponsePacking::Projection) == i,
                 "legacy public placement remains linear");
  }
  require_test(mlwe_merge_keyswitch_count(25, 32) == 27 &&
                   mlwe_merge_keyswitch_count(75, 256) == 79,
               "partial groups include unary trace steps");

  const std::vector<std::pair<size_t, size_t>> cases = {
      {1, 1}, {2, 1}, {2, 2}, {4, 3}, {4, 4}, {8, 5}, {8, 8},
      {32, 25}, {32, 32}, {256, 75}, {256, 256}, {n, 1}, {n, 17}};
  for (const auto &[rho, count] : cases) {
    const size_t r = std::bit_width(rho) - 1;
    std::vector<RlweCt> inputs;
    RlwePt expected;
    expected.data.assign(n, 0);
    for (size_t i = 0; i < count; ++i) {
      std::vector<uint64_t> message(n);
      // Dense non-target coefficients catch missing projection rounds and
      // incorrect negacyclic signs, not just misplaced desired fragments.
      for (uint64_t &x : message) x = rng() % t;
      const size_t offset = reversed_index(i, r);
      for (size_t j = 0; j < n; j += rho) expected.data[j + offset] = message[j];
      RlweCt ct;
      encrypt_bfv_rns(message, client.rlwe_sk_, n, qs, t,
                      scheme.get_noise_std_dev(), rng, ct);
      inputs.push_back(std::move(ct));
    }
    const auto original_first = inputs.front();
    size_t actual_ks = 0;
    const RlweCt merged = mlwe_merge_residues(
        inputs, rho, keys->bv_galois_keys, scheme, &actual_ks);
    const auto plain = client.decrypt_ct(merged);
    require_test(plain.data == expected.data,
                 "MLWE merge must preserve scale and isolate every desired coefficient");
    require_test(actual_ks == mlwe_merge_keyswitch_count(count, rho),
                 "MLWE actual key-switch count differs from the tree count");
    require_test(inputs.front().c0 == original_first.c0 &&
                     inputs.front().c1 == original_first.c1,
                 "merge called with borrowed inputs must not overwrite them");
    std::cout << "rho=" << rho << " inputs=" << count << " KS=" << actual_ks
              << " all " << n << " coefficients correct\n";
  }
}
