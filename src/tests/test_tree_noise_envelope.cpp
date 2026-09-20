#include "tests.h"
#include "tree_entry_test_util.h"
#include "tree_multiplane.h"
#include "tree_packing.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

void test_tree_noise_envelope();

namespace {

tree_entry_codec::EncodedEntry encode_one(
    const tree_entry_codec::EntryBits &entry,
    const tree_multiplane::Params &params) {
  return tree_entry_codec::encode_entry(entry, params.codec);
}

}  // namespace

// Complete response plaintext at the final h=1 32-byte layout with the paper's
// first-dimension width. The retained primary path uses the old
// (gamma, alpha, beta) tree layout with g = 8 and P = 3, so the smallest b = 0
// fixture with N0 = 512 has L = log2(n/g) + a = 8 + 9 = 17. This gate decrypts
// every small-q coefficient of every response chunk: selected entry planes must
// appear at the public residue lattice, and all padding coefficients must remain
// zero. The historical one-bit noise-margin assertion is intentionally gone;
// correctness is the contract for these prototype performance experiments.
void test_tree_noise_envelope() {
  print_func_name(__FUNCTION__);
  constexpr size_t N = DBConsts::PolyDegree;

  PirParams scheme;
  require_applicable(
      paper_config_active(),
      "the N0 = 512 response noise envelope is pinned to CONFIG_N2048_K1_COMP "
      "(n = 2048, key height 10, 22-bit small q); this build uses another "
      "parameter point");

  const size_t c = tree_entry_test::payload_bits(scheme);
  const size_t key_height = std::bit_width(N) - 1;
  constexpr size_t entry_bits = 256;
  constexpr size_t g = 8;
  const TreePirParams tree =
      make_tree_pir_params_for_scheme(17, 9, g, scheme, key_height);
  require_test(tree.N0 == 512 && tree.b == 0 && tree.g == g,
               "fixture must be the final h1 32B layout at N0 = 512");
  const tree_multiplane::Params params =
      tree_multiplane::make_params(tree, entry_bits);
  require_test(params.codec.payload_bits_per_coeff == c &&
                   params.codec.plane_count == 3,
               "32B h1 final layout must use g8/P3");

  PirClient client(scheme);
  SharedPirSessionKeys keys = client.create_session_keys(key_height);
  PirServer server(tree_query_expansion_params(tree, scheme));
  server.set_client_session_keys(client.get_client_id(), keys);

  const tree_multiplane::EntrySource source = [&](size_t level, size_t node) {
    return tree_entry_test::synthetic_entry(entry_bits, level, node);
  };
  const tree_multiplane::Database db =
      tree_multiplane::preprocess(params, source, scheme);

  const uint64_t small_q = scheme.get_small_q();
  const uint64_t t = scheme.get_plain_mod();
  uint64_t max_noise = 0;
  int min_budget = std::numeric_limits<int>::max();
  const std::vector<size_t> leaves = {
      0, tree.N - 1, static_cast<size_t>(0x2BADBEEFULL % tree.N)};
  for (size_t leaf : leaves) {
    RlweCt query = make_tree_query(client, scheme, tree, leaf);
    ExpandedTreeQuery expanded = unpack_tree_query(
        server, scheme, tree, client.get_client_id(), query);
    const tree_multiplane::Response response = tree_multiplane::answer(
        db, expanded, server, client.get_client_id(), params, scheme,
        tree_packing::ResponsePacking::MlweMerge);
    require_test(response.small_q,
                 "noise envelope requires a small-q switched response");
    const size_t planes = params.codec.plane_count;
    const size_t items = (tree.L + 1) * planes;
    const auto bounds = tree_packing::path_chunk_bounds(items, tree.rho);
    require_test(bounds.size() == response.chunks.size(),
                 "response chunk count must follow the item partition");
    require_test(response.item_offsets.size() == items,
                 "response offset map must cover every level-plane item");
    for (size_t chunk = 0; chunk < response.chunks.size(); ++chunk) {
      uint64_t noise = 0;
      int budget = 0;
      const RlwePt decoded =
          client.decrypt_mod_q_measured(response.chunks[chunk], noise, budget);
      std::vector<uint64_t> expected(N, 0);
      const auto [first_item, count] = bounds[chunk];
      for (size_t local = 0; local < count; ++local) {
        const size_t item = first_item + local;
        const size_t level = item / planes;
        const size_t plane = item % planes;
        const size_t node = leaf >> (tree.L - level);
        const auto encoded = encode_one(source(level, node), params);
        const size_t offset = response.item_offsets[item];
        require_test(offset < tree.rho,
                     "response residue offset stays within one chunk");
        for (size_t digit = 0; digit < tree.g; ++digit) {
          expected[offset + digit * tree.rho] = encoded[plane][digit];
        }
      }
      require_test(decoded.data == expected,
                   "N0 = 512 h1 response must decode to the selected planes "
                   "and zero everywhere else");
      max_noise = std::max(max_noise, noise);
      min_budget = std::min(min_budget, budget);
    }
  }
  BENCH_PRINT("N0 = 512 h1 noise envelope: max noise " << max_noise
              << " of decode bound " << small_q / (2 * t)
              << ", min measured budget " << min_budget << " bits over "
              << leaves.size() << " leaves");
}
