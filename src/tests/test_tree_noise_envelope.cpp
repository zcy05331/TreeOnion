#include "tests.h"
#include "tree_index.h"
#include "tree_query.h"
#include "tree_response.h"
#include "tree_select.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

void test_tree_noise_envelope();

// Complete response plaintext at the paper's first-dimension width. At 2^22
// leaves tree_bench_g32 selects a = 9 (N0 = 512, h_q = 10 = the default
// expansion height), yet every in-process g = 32 fixture sums at most 8
// first-dimension terms and reads back only the payload coefficients. This
// test decodes all 2048 small-q coefficients of every response chunk on the
// smallest tree with that first dimension (L = 15, b = 0): the packed chunks
// must sit exactly on the lattice {level_offsets[l] + j * rho}, every other
// coefficient must decode to zero, and the noise must keep a bit of budget.
// The zero half is the part no other in-process test reaches at this shape.
// A projection depth one level too shallow at l >= r leaves records at
// rho/2 + j * rho; a level packed at offset z then puts them at
// z + rho/2 + j * rho, and with z <= L = 15 < rho/2 = 32 no level offset ever
// reads one, so test_tree_g32 and test_tree_e2e stay green while the client
// silently receives another node's chunks. test_tree_compress catches that
// same mutation, but only because its 2L = rho fixture is dense enough for
// the stray residue to land on a slot a later level reads
// (test_tree_compress.cpp:443, "MVP packer must still serve the shape
// compression rejects"); the production shape has 2L << rho and no such
// collision.
void test_tree_noise_envelope() {
  print_func_name(__FUNCTION__);
  constexpr size_t N = DBConsts::PolyDegree;

  PirParams scheme;
  require_applicable(
      paper_config_active(),
      "the N0 = 512 response noise envelope is pinned to CONFIG_N2048_K1_COMP "
      "(n = 2048, key height 10, 22-bit small q); this build uses another "
      "parameter point");
  const uint64_t t = scheme.get_plain_mod();

  // Pin the fixture: the smallest tree whose first dimension is the
  // production width, sitting exactly at the session-key expansion height.
  const TreePirParams tree = make_tree_pir_params_for_scheme(15, 9, 32, scheme);
  require_test(tree.N0 == 512 && tree.b == 0 &&
                   tree.h_q == scheme.get_expan_height(),
               "fixture must be the smallest tree at the paper's first-"
               "dimension width");

  PirClient client(scheme);
  SharedPirSessionKeys keys =
      client.create_session_keys(std::bit_width(N) - 1);
  const PirParams qparams = tree_query_expansion_params(tree, scheme);
  PirServer server(qparams);
  server.set_client_session_keys(client.get_client_id(), keys);
  const TreeNodeChunkSource source = [&](size_t level, size_t index,
                                         size_t chunk) {
    return synthetic_tree_node_bytes_chunk(level, index, chunk, t);
  };
  const PreprocessedTree db = preprocess_tree_mvp(tree, source, scheme);

  // Noise floor. decrypt_mod_q_measured reports the largest centered residue;
  // budget >= 1 means it stayed under q/(4t) = 254, half the decode limit
  // q/(2t) = 508. Noise comes from std::random_device per process, so this is
  // a floor with headroom: 260 processes (780 chunk decodes) put the per-run
  // maximum at 99-180, mean 134, sd 15. It is therefore a 2x canary, sized
  // for the class the g32 benchmark recorded when N0 was pushed to 1024 (max
  // noise 294 on unused coefficients, test_tree_benchmark_g32.cpp:36-41),
  // and the N0 <= 8 fixtures in test_tree_g32 (38-67) cannot stand in for it.
  // What it measures is the whole response noise at this width, most of which
  // comes from the query expansion and the 512-term first-dimension sum the
  // flat pipeline shares -- test_pir's response at fst_dim_sz = 512 measures
  // the same 114-167 -- and not from the fresh encryption noise: doubling the
  // Gaussian width in rlwe.cpp sample_gaussian_rns leaves the distribution
  // where it was and even 4096x only reaches 184-239, because the
  // q -> small_q rescale divides the carried noise by 2^36. Keyswitch-key
  // noise is the term that does move it, and by the time it matters the
  // decode check above fires first.
  const uint64_t small_q = scheme.get_small_q();
  uint64_t max_noise = 0;
  int min_budget = std::numeric_limits<int>::max();
  const std::vector<size_t> leaves = {
      0, tree.N - 1, static_cast<size_t>(0x2BADBEEFULL % tree.N)};
  for (size_t leaf : leaves) {
    RlweCt query = make_tree_query(client, scheme, tree, leaf);
    ExpandedTreeQuery unpacked = unpack_tree_query(
        server, scheme, tree, client.get_client_id(), query);
    const TreePathResponse response = answer_path_mvp(
        db, unpacked, server, client.get_client_id(), tree, scheme);
    require_test(response.small_q,
                 "noise envelope requires a small-q switched response");
    const auto bounds = path_chunk_bounds(response.level_count, tree.rho);
    require_test(bounds.size() == response.chunks.size(),
                 "response chunk count must follow the level partition");
    for (size_t c = 0; c < response.chunks.size(); ++c) {
      uint64_t noise = 0;
      int budget = 0;
      const RlwePt decoded =
          client.decrypt_mod_q_measured(response.chunks[c], noise, budget);
      // The complete plaintext: the path chunks on the placement lattice,
      // zero on every coefficient the projection is supposed to clear.
      std::vector<uint64_t> expected(N, 0);
      const auto [first_level, count] = bounds[c];
      for (size_t offset = 0; offset < count; ++offset) {
        const size_t level = first_level + offset;
        const size_t node = leaf >> (tree.L - level);
        for (size_t j = 0; j < tree.g; ++j) {
          expected[response.level_offsets[level] + j * tree.rho] =
              source(level, node, j);
        }
      }
      require_test(decoded.data == expected,
                   "N0 = 512 response must decode to the path chunks and "
                   "zero everywhere else");
      require_test(budget >= 1,
                   "N0 = 512 response noise left less than one bit of budget: "
                   "max noise " + std::to_string(noise) +
                       " over small_q/(4t) = " +
                       std::to_string(small_q / (4 * t)) + " (decode limit " +
                       std::to_string(small_q / (2 * t)) + ")");
      max_noise = std::max(max_noise, noise);
      min_budget = std::min(min_budget, budget);
    }
  }
  BENCH_PRINT("N0 = 512 noise envelope: max noise " << max_noise
              << " of bound " << small_q / (2 * t) << ", min budget "
              << min_budget << " bits over " << leaves.size() << " leaves");
}
