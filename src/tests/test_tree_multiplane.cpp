#include "tests.h"
#include "tree_multiplane.h"
#include "tree_entry_test_util.h"

#include <bit>
#include <span>
#include <utility>
#include <vector>

using tree_multiplane::EntryBits;

namespace {

EntryBits entry_for(size_t bits, size_t level, size_t node) {
  return tree_entry_test::synthetic_entry(bits, level, node);
}

tree_entry_codec::EncodedEntry encode_one(
    const EntryBits &entry, const tree_multiplane::Params &params) {
  return tree_entry_codec::encode_entry(entry, params.codec);
}

void require_response_plaintext(const tree_multiplane::Response &response,
                                PirClient &client,
                                const tree_multiplane::Params &params,
                                size_t leaf,
                                const tree_multiplane::EntrySource &source,
                                tree_packing::ResponsePacking packing) {
  const size_t planes = params.codec.plane_count;
  const size_t items = (params.tree.L + 1) * planes;
  const auto bounds = tree_packing::path_chunk_bounds(items, params.tree.rho);
  require_test(response.chunks.size() == bounds.size(),
               "multiplane response chunk count");
  require_test(response.item_offsets.size() == items,
               "multiplane response offset count");
  for (size_t chunk = 0; chunk < bounds.size(); ++chunk) {
    const RlwePt pt = response.small_q
                          ? client.decrypt_mod_q(response.chunks[chunk])
                          : client.decrypt_ct(response.chunks[chunk]);
    std::vector<uint64_t> expected(params.tree.n, 0);
    const auto [first, count] = bounds[chunk];
    for (size_t local = 0; local < count; ++local) {
      const size_t item = first + local;
      const size_t level = item / planes;
      const size_t plane = item % planes;
      const size_t node = leaf >> (params.tree.L - level);
      const auto encoded = encode_one(source(level, node), params);
      const size_t offset = tree_packing::packed_residue_offset(
          local, params.tree.rho, packing);
      require_test(response.item_offsets[item] == offset,
                   "multiplane public residue offset");
      for (size_t digit = 0; digit < params.tree.g; ++digit) {
        expected[offset + digit * params.tree.rho] = encoded[plane][digit];
      }
    }
    require_test(pt.data == expected,
                 "multiplane response plaintext includes zero padding");
  }
}

void require_chunk_bounds_arithmetic() {
  const auto bounds = tree_packing::path_chunk_bounds(17, 5);
  require_test(bounds.size() == 4, "path chunk bounds count");
  require_test(bounds[0] == std::pair<size_t, size_t>{0, 5},
               "path chunk first bound");
  require_test(bounds[1] == std::pair<size_t, size_t>{5, 5},
               "path chunk middle bound");
  require_test(bounds[2] == std::pair<size_t, size_t>{10, 5},
               "path chunk middle bound 2");
  require_test(bounds[3] == std::pair<size_t, size_t>{15, 2},
               "path chunk tail bound");
}

std::vector<EntryBits> answer_path(const tree_multiplane::Database &db,
                                   PirClient &client, PirServer &server,
                                   const PirParams &scheme,
                                   const tree_multiplane::Params &params,
                                   size_t leaf,
                                   tree_packing::ResponsePacking packing) {
  RlweCt query = make_tree_query(client, scheme, params.tree, leaf);
  ExpandedTreeQuery expanded = unpack_tree_query(
      server, scheme, params.tree, client.get_client_id(), query);
  tree_multiplane::Response response =
      tree_multiplane::answer(db, expanded, server, client.get_client_id(),
                              params, scheme, packing);
  return tree_multiplane::extract(response, client, params);
}

// Every AlphaBeta level of every plane: finishing its rows of the stacked scan
// gives the same ciphertext, bit for bit, as scanning its own m32 view.
void require_stacked_scan_matches_levels(
    const tree_multiplane::Database &db, PirClient &client, PirServer &server,
    const PirParams &scheme, const tree_multiplane::Params &params,
    const tree_multiplane::EntrySource &source, size_t leaf) {
  RlweCt query = make_tree_query(client, scheme, params.tree, leaf);
  ExpandedTreeQuery expanded = unpack_tree_query(
      server, scheme, params.tree, client.get_client_id(), query);
  const AlphaPyramidM32 pyramid = pyramid_to_m32(
      build_alpha_pyramid_ntt(expanded.alpha, params.tree, scheme), scheme);
  ScanScratch stacked_scan, level_scan;
  scan_stacked_alpha_beta(db.stacked, pyramid, scheme, stacked_scan);
  const std::span<GSWCt> beta(expanded.beta_selectors);
  size_t checked = 0;
  for (size_t plane = 0; plane < params.codec.plane_count; ++plane) {
    for (size_t level = 0; level <= params.tree.L; ++level) {
      const LevelPlan &plan = db.planes[plane].plans[level];
      if (plan.select_case != SelectCase::AlphaBeta) continue;
      const TreeLevelDatabaseM32 view = build_level_m32_view(
          tree_multiplane::pack_level(level, plane, params, source), plan,
          params.tree, scheme);
      const RlweCt stacked = finish_level_m32(
          stacked_scan, db.stacked.rows, db.stacked.first_row[plane][level],
          plan, beta, server, params.tree, scheme);
      const RlweCt own = select_level_m32(view, plan, pyramid, beta, server,
                                          params.tree, scheme, level_scan);
      require_test(stacked.c0 == own.c0 && stacked.c1 == own.c1,
                   "stacked AlphaBeta scan matches the per-level scan");
      ++checked;
    }
  }
  require_test(checked > 0, "test shape has AlphaBeta levels");
}

void exercise_e2e(tree_packing::ResponsePacking packing) {
  PirParams scheme;
  PirClient client(scheme);
  const size_t c = tree_entry_test::payload_bits(scheme);
  const size_t key_height = std::bit_width(DBConsts::PolyDegree) - 1;
  auto keys = client.create_session_keys(key_height);
  const size_t g = DBConsts::PolyDegree / 8;
  const size_t L = tree_height_for(2, 1, g);
  const size_t entry_bits = 2 * c * g + 1;
  const TreePirParams tree =
      make_tree_pir_params_for_scheme(L, 2, g, scheme, key_height);
  const tree_multiplane::Params params = tree_multiplane::make_params(
      tree, entry_bits);
  require_test(params.codec.plane_count == 3,
               "test shape must exercise three planes");
  require_test((tree.L + 1) * params.codec.plane_count > tree.rho,
               "test shape must cross response chunk boundaries");
  PirServer server(tree_query_expansion_params(tree, scheme));
  server.set_client_session_keys(client.get_client_id(), keys);
  const tree_multiplane::EntrySource source =
      [&](size_t level, size_t node) { return entry_for(entry_bits, level, node); };
  const tree_multiplane::Database db =
      tree_multiplane::preprocess(params, source, scheme);
  require_test(db.planes.size() == params.codec.plane_count,
               "preprocess builds one tree per plane");
  require_test(db.planes[0].canonical.empty(),
               "multiplane preprocessing releases canonical levels");

  for (size_t leaf : {size_t{0}, tree.N - 1,
                      static_cast<size_t>(0x2BADBEEFULL % tree.N)}) {
    RlweCt query = make_tree_query(client, scheme, tree, leaf);
    ExpandedTreeQuery expanded = unpack_tree_query(
        server, scheme, tree, client.get_client_id(), query);
    tree_multiplane::Response response =
        tree_multiplane::answer(db, expanded, server, client.get_client_id(),
                                params, scheme, packing);
    require_response_plaintext(response, client, params, leaf, source, packing);
    const std::vector<EntryBits> path =
        tree_multiplane::extract(response, client, params);
    require_test(path.size() == tree.L + 1, "multiplane decoded path length");
    for (size_t level = 0; level <= tree.L; ++level) {
      const size_t node = leaf >> (tree.L - level);
      require_test(path[level] == source(level, node),
                   "multiplane decoded entry");
    }
  }

  if (scheme.get_composite_rns().enabled) {
    require_stacked_scan_matches_levels(db, client, server, scheme, params,
                                        source, tree.N - 1);
  }

  const size_t repeated_leaf = static_cast<size_t>(0x51A7E5EEDULL % tree.N);
  const std::vector<EntryBits> first =
      answer_path(db, client, server, scheme, params, repeated_leaf, packing);
  const std::vector<EntryBits> second =
      answer_path(db, client, server, scheme, params, repeated_leaf, packing);
  require_test(first == second,
               "fresh query/session randomness preserves decoded path");
}

}  // namespace

void test_tree_multiplane() {
  require_chunk_bounds_arithmetic();

  PirParams scheme;
  const size_t key_height = std::bit_width(DBConsts::PolyDegree) - 1;

  {
    const size_t g = 32;
    const size_t entry_bits = 256;
    const size_t c = tree_entry_test::payload_bits(scheme);
    const TreePirParams tree = make_tree_pir_params_for_scheme(
        tree_height_for(2, 1, g), 2, g, scheme, key_height);
    const tree_multiplane::Params params =
        tree_multiplane::make_params(tree, entry_bits);
    require_test(params.codec.plane_count == 1,
                 "256-bit g32 entries fit in one old-layout plane");
    const tree_multiplane::EntrySource source = [&](size_t level, size_t node) {
      return tree_entry_codec::entry_from_bytes(
          synthetic_tree_node_bytes(level, node), entry_bits);
    };
    const size_t level = tree.r + 1;
    const TreeLevelDatabase packed =
        tree_multiplane::pack_level(level, 0, params, source);
    const TreeNodeChunkSource direct =
        [&](size_t l, size_t node, size_t chunk) {
          return synthetic_tree_node_bytes_chunk(l, node, chunk,
                                                 scheme.get_plain_mod());
        };
    const TreeLevelDatabase expected = pack_tree_level(level, tree, direct);
    require_test(packed.plaintexts == expected.plaintexts,
                 "P1 multiplane packing is exactly old pack_tree_level");

    const size_t node = packed.R * 3 + 1;
    require_test(packed.plaintexts[node % packed.R]
                                  [node / packed.R] ==
                     direct(level, node, 0),
                 "deep levels use old non-contiguous node=p+uR mapping");

    const tree_multiplane::Params p2 = tree_multiplane::make_params(
        tree, c * g + 1);
    require_test(p2.codec.plane_count == 2,
                 "c*g+1 bits require exactly two old-layout planes");
  }

  exercise_e2e(tree_packing::ResponsePacking::Projection);
  exercise_e2e(tree_packing::ResponsePacking::MlweMerge);
}
