#include "tree_superrecord_test_util.h"

#include "tree_superrecord_response.h"

#include <algorithm>
#include <set>

using namespace tree_superrecord;
using namespace tree_superrecord_test;

// Full encrypted pipeline on small shapes: packed query, expansion, ragged
// pyramid, reference and m32 selection, shared-selector fold and rotation,
// projection, pooled multi-plane responses, and client decode; plus the
// client/session/tag bindings and the update path (design sections 5, 9,
// 12-15).
void PirTest::test_tree_superrecord_e2e() {
  print_func_name(__FUNCTION__);
  PirParams scheme;
  PirClient client(scheme);
  const bool composite = scheme.get_composite_rns().enabled;

  struct Shape {
    size_t E, N, h, G, b, max_responses, expected_chunks;
  };
  const std::vector<Shape> shapes = {
      {13, 64, 3, 2, 0, 1, 1},
      {256, 256, 3, 64, 0, 1, 1},
      {256, 1024, 3, 64, 2, 1, 1},
      {13, 8, 8, 4, 0, 4, 1},
      {40, 128, 5, 8, 0, 1, 1},
      {4096, 16, 1, 256, 0, 2, 2},
      {30000, 16, 1, 2048, 2, 16, 10},
  };
  for (const Shape &shape : shapes) {
    const TreeParams params = plan_for(scheme, shape.E, shape.N, shape.h,
                                       shape.G, shape.b,
                                       test_policy(shape.max_responses, 128));
    const CandidateShape &s = params.shape;
    const size_t L = s.tree_height;
    const EntrySource source = [&](size_t level, size_t node) {
      return synthetic_entry(shape.E, level, node);
    };
    const PirParams qparams = query_expansion_params(params, scheme);
    PirServer server(qparams);
    const TaggedTreeSessionKeys session =
        create_tagged_tree_session_keys(client, params, scheme);
    set_tree_client_session_keys(server, params, scheme, session);

    ReferencePreprocessedTree reference =
        preprocess_reference(params, scheme, source);
    PreprocessedTree m32;
    if (composite) m32 = m32_from_reference(reference, params, scheme, true);

    std::set<size_t> leaf_set = {0, shape.N - 1, 0x2BADBEEFULL % shape.N};
    for (size_t k = 1; k <= 2; ++k) {
      for (size_t boundary : {shape.h * k, shape.h * s.records_per_plaintext * k}) {
        if (boundary < shape.N) leaf_set.insert(boundary);
        if (boundary >= 1 && boundary - 1 < shape.N) leaf_set.insert(boundary - 1);
      }
    }
    int min_budget = 1 << 20;
    for (size_t leaf : leaf_set) {
      const PackedQuery query = make_query(client, session, scheme, params, leaf);
      TreeServerTiming timing{};
      const PathResponse response = serve_request_reference(
          query, reference, server, session, params, scheme, &timing);
      require_test(response.chunks.size() == shape.expected_chunks,
                   "response chunk count");
      require_test(response.tag == params.tag &&
                       response.client_id == client.get_client_id(),
                   "response carries the plan tag and client id");
      const DecodedPath decoded =
          extract_path_measured(response, client, params, scheme, leaf);
      require_test(decoded.entries.size() == L + 1, "path has L + 1 entries");
      for (size_t level = 0; level <= L; ++level) {
        const size_t node = leaf >> (L - level);
        require_test(decoded.entries[level] == source(level, node),
                     "extracted entry must match the tree node");
      }
      min_budget = std::min(min_budget, decoded.min_budget_bits);
      require_test(decoded.min_budget_bits >= 1,
                   "decryption must keep at least one bit of budget");
      require_test(timing.query_expand.count() > 0 &&
                       (s.selector_bits == 0 || timing.selector_completion.count() > 0) &&
                       timing.total >= timing.query_expand &&
                       timing.total >= timing.alpha_scan &&
                       timing.total >= timing.projection,
                   "stage timing is populated and bounded by the total");
      // Every lane of the selected bundle decodes to the real neighbours,
      // with zero tail lanes.
      const std::vector<std::vector<EntryBits>> lanes =
          debug::extract_all_lanes_for_test(response, client, params, scheme);
      for (size_t level = 0; level <= L; ++level) {
        const size_t node = leaf >> (L - level);
        const size_t start = (node / shape.h) * shape.h;
        for (size_t lane = 0; lane < shape.h; ++lane) {
          const size_t neighbour = start + lane;
          const EntryBits expected = neighbour < (size_t{1} << level)
                                         ? source(level, neighbour)
                                         : zero_entry(shape.E);
          require_test(lanes[level][lane] == expected, "bundle lanes decode");
        }
      }
      if (composite) {
        const PathResponse fast = serve_request_m32(query, m32, server, session,
                                                    params, scheme, nullptr);
        require_test(fast.chunks.size() == response.chunks.size(),
                     "m32 chunk count");
        for (size_t i = 0; i < fast.chunks.size(); ++i) {
          require_test(fast.chunks[i].c0 == response.chunks[i].c0 &&
                           fast.chunks[i].c1 == response.chunks[i].c1,
                       "m32 and reference responses are raw-ciphertext equal");
        }
      }
      if (response.small_q) {
        const std::optional<size_t> bytes =
            serialized_response_payload_bytes(response, server, params);
        std::stringstream one;
        const size_t per_chunk = server.save_resp_to_stream(response.chunks[0], one);
        require_test(bytes.has_value() && *bytes == per_chunk * response.chunks.size(),
                     "payload bytes sum one serializer call per chunk");
      }
    }
    // Fresh randomness and profiler transparency.
    {
      const size_t leaf = *leaf_set.rbegin();
      const PackedQuery first = make_query(client, session, scheme, params, leaf);
      const PackedQuery second = make_query(client, session, scheme, params, leaf);
      require_test(first.ciphertext.c1 != second.ciphertext.c1,
                   "repeated queries use fresh randomness");
      TreeServerTiming timing{};
      const PathResponse a = serve_request_reference(first, reference, server,
                                                     session, params, scheme, nullptr);
      const PathResponse b = serve_request_reference(first, reference, server,
                                                     session, params, scheme, &timing);
      const PathResponse c2 = serve_request_reference(second, reference, server,
                                                      session, params, scheme, nullptr);
      for (size_t i = 0; i < a.chunks.size(); ++i) {
        require_test(a.chunks[i].c0 == b.chunks[i].c0 && a.chunks[i].c1 == b.chunks[i].c1,
                     "timing sink does not change the response");
      }
      require_test(extract_path(a, client, params, scheme, leaf) ==
                       extract_path(c2, client, params, scheme, leaf),
                   "independent queries decode the same path");
    }
    // Update: rewrite the leaf-level bundle of leaf 0 and query it again.
    {
      const size_t leaf = 0;
      const EntrySource updated = [&](size_t level, size_t node) {
        return synthetic_entry(shape.E, level, node, level == L && node == 0 ? 7 : 0);
      };
      rewrite_reference_superrecord(reference, L, 0, params, scheme, updated);
      const PackedQuery query = make_query(client, session, scheme, params, leaf);
      const PathResponse response = serve_request_reference(
          query, reference, server, session, params, scheme, nullptr);
      const std::vector<EntryBits> path =
          extract_path(response, client, params, scheme, leaf);
      require_test(path[L] == updated(L, 0) && path[L] != source(L, 0),
                   "reference update is visible to the next query");
      if (composite) {
        rewrite_preprocessed_superrecord(m32, L, 0, params, scheme, updated);
        const PathResponse fast = serve_request_m32(query, m32, server, session,
                                                    params, scheme, nullptr);
        for (size_t i = 0; i < fast.chunks.size(); ++i) {
          require_test(fast.chunks[i].c0 == response.chunks[i].c0,
                       "updated m32 view matches the updated reference");
        }
      }
    }
    BENCH_PRINT("E=" << shape.E << " N=" << shape.N << " h=" << shape.h
                << " G=" << shape.G << " b=" << shape.b << " N0=" << s.N0
                << " m=" << s.selector_bits << " chunks=" << s.response_ciphertexts
                << " leaves=" << leaf_set.size() << " min budget " << min_budget
                << " bits");
  }

  // ---- bindings reject mismatches before any homomorphic work ----
  {
    const TreeParams params = plan_for(scheme, 256, 256, 3, 64, 1, test_policy());
    const TreeParams other_params = plan_for(scheme, 256, 256, 3, 64, 0, test_policy());
    const EntrySource source = [&](size_t level, size_t node) {
      return synthetic_entry(256, level, node);
    };
    PirServer server(query_expansion_params(params, scheme));
    const TaggedTreeSessionKeys session =
        create_tagged_tree_session_keys(client, params, scheme);
    set_tree_client_session_keys(server, params, scheme, session);
    const ReferencePreprocessedTree reference =
        preprocess_reference(params, scheme, source);
    const PackedQuery good = make_query(client, session, scheme, params, 5);
    PirClient other_client(scheme);
    require_test(throws_invalid([&] {
                   (void)make_query(other_client, session, scheme, params, 5);
                 }),
                 "another client's query must not use this session");
    PackedQuery wrong_client = good;
    wrong_client.client_id ^= 1;
    require_test(throws_invalid([&] {
                   (void)serve_request_reference(wrong_client, reference, server,
                                                 session, params, scheme);
                 }),
                 "client id mismatch rejects before expansion");
    PackedQuery wrong_tag = good;
    wrong_tag.tag.entry_bits = 255;
    require_test(throws_invalid([&] {
                   (void)serve_request_reference(wrong_tag, reference, server,
                                                 session, params, scheme);
                 }),
                 "PlanTag mismatch rejects before expansion");
    PirServer other_server(query_expansion_params(other_params, scheme));
    require_test(throws_invalid([&] {
                   require_query_server_binding(other_server, params, scheme);
                 }),
                 "a server with the wrong N0 is rejected");
    const PathResponse response = serve_request_reference(
        good, reference, server, session, params, scheme);
    require_test(throws_invalid([&] {
                   (void)extract_path(response, other_client, params, scheme, 5);
                 }),
                 "another client cannot decode this response");
    PathResponse wrong_response = response;
    wrong_response.tag.N0 += 1;
    require_test(throws_invalid([&] {
                   (void)extract_path(wrong_response, client, params, scheme, 5);
                 }),
                 "response tag mismatch rejects before decryption");
    // Key material inspection.
    TaggedTreeSessionKeys short_keys = session;
    short_keys.material = client.create_session_keys(params.shape.required_key_height);
    if (params.shape.required_key_height < params.shape.registered_key_height) {
      require_test(throws_invalid([&] {
                     require_session_material_binding(params, scheme, short_keys);
                   }),
                   "fewer keys than the registered height are rejected");
    }
    auto tamper = [&](auto &&mutate) {
      auto copy = std::make_shared<PirSessionKeys>(*session.material);
      mutate(*copy);
      TaggedTreeSessionKeys bad = session;
      bad.material = copy;
      return throws_invalid([&] {
        require_session_material_binding(params, scheme, bad);
      });
    };
    require_test(tamper([](PirSessionKeys &k) {
                   k.bv_galois_keys.keys.push_back(k.bv_galois_keys.keys[0]);
                 }),
                 "duplicate automorphism is rejected");
    require_test(tamper([](PirSessionKeys &k) { k.bv_galois_keys.keys[0].galois_k = 3; }),
                 "missing/unexpected automorphism is rejected");
    require_test(tamper([](PirSessionKeys &k) { k.bv_galois_keys.keys[1].cts.pop_back(); }),
                 "wrong BV row count is rejected");
    require_test(tamper([](PirSessionKeys &k) { k.bv_galois_keys.keys[2].cts[0].a.pop_back(); }),
                 "wrong BV row width is rejected");
    require_test(tamper([](PirSessionKeys &k) { k.gsw_key.pop_back(); }),
                 "wrong completion-key row count is rejected");
    require_test(tamper([](PirSessionKeys &k) { k.gsw_key[0].pop_back(); }),
                 "wrong completion-key row width is rejected");
    TaggedTreeSessionKeys wrong_scheme = session;
    wrong_scheme.scheme.payload_bits_per_coeff -= 1;
    require_test(throws_invalid([&] {
                   require_session_material_binding(params, scheme, wrong_scheme);
                 }),
                 "fingerprint mismatch is rejected");
    TaggedTreeSessionKeys unregistered = session;
    unregistered.material = client.create_session_keys(params.shape.registered_key_height);
    require_test(throws_invalid([&] {
                   require_registered_session_binding(server, params, scheme, unregistered);
                 }),
                 "material that is not the registered pointer is rejected");
    const BvKeyCoverage coverage = inspect_bv_key_coverage(
        session.material->bv_galois_keys, params.scheme.identity,
        params.shape.registered_key_height);
    require_test(coverage.contiguous_height == params.shape.registered_key_height &&
                     coverage.missing_required.empty() && coverage.duplicates.empty() &&
                     coverage.unexpected.empty() && coverage.malformed_row_shapes.empty(),
                 "the trusted session passes inspection");
  }
}
