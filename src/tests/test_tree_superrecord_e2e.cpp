#include "tree_superrecord_test_util.h"

#include "tree_superrecord_response.h"

#include <algorithm>
#include <set>

using namespace tree_superrecord;
using namespace tree_superrecord_test;

namespace {

// SUPERRECORD_KEY_MODE=candidate: each plan registers exactly what it needs.
PlannerPolicy candidate_key_policy() {
  PlannerPolicy policy = test_policy();
  policy.eval_key_registration =
      EvalKeyRegistrationPolicy::CandidateSpecificHeight;
  policy.fixed_session_key_height = 0;
  return policy;
}

// Serves every leaf under `packing` with a session that holds exactly
// registered_key_height BV keys: reference path always, m32 path when the
// caller built one (the m32 database is packing-agnostic; the gate result is
// not). A required_key_height that undercounts the family, or a registration
// that hands out fewer automorphisms than the plan says, surfaces here as a
// keys.get throw inside project_keep_stride / mlwe_merge_residues or as a
// wrong decoded entry, exactly where a candidate-mode sweep would hit it.
void serve_with_exact_keys(PirClient &client, PirServer &server,
                           const TaggedTreeSessionKeys &session,
                           const TreeParams &params, const PirParams &scheme,
                           const ReferencePreprocessedTree &reference,
                           const PreprocessedTree *m32,
                           const CandidateNoiseResult *m32_noise,
                           const EntrySource &source,
                           const std::set<size_t> &leaves,
                           ResponsePacking packing) {
  const CandidateShape &s = params.shape;
  const size_t L = s.tree_height;
  const char *label = packing == ResponsePacking::MlweMerge ? "mlwe" : "projection";
  int min_budget = 1 << 20;
  for (size_t leaf : leaves) {
    const PackedQuery query = make_query(client, session, scheme, params, leaf);
    const PathResponse response = serve_request_reference(
        query, reference, server, session, params, scheme, nullptr, packing);
    const DecodedPath decoded =
        extract_path_measured(response, client, params, scheme, leaf);
    require_test(decoded.entries.size() == L + 1,
                 "exact-key path has L + 1 entries");
    for (size_t level = 0; level <= L; ++level) {
      const size_t node = leaf >> (L - level);
      require_test(decoded.entries[level] == source(level, node),
                   "every level decodes with exactly the registered key family");
    }
    require_test(decoded.min_budget_bits >= 1,
                 "exact-key decryption must keep at least one bit of budget");
    min_budget = std::min(min_budget, decoded.min_budget_bits);
    if (m32) {
      const PathResponse fast =
          serve_request_m32(query, *m32, server, session, params, scheme,
                            *m32_noise, nullptr, packing);
      require_test(fast.chunks.size() == response.chunks.size(),
                   "exact-key m32 chunk count");
      for (size_t i = 0; i < fast.chunks.size(); ++i) {
        require_test(fast.chunks[i].c0 == response.chunks[i].c0 &&
                         fast.chunks[i].c1 == response.chunks[i].c1,
                     "exact-key m32 and reference responses are raw-ciphertext equal");
      }
    }
  }
  BENCH_PRINT("E=" << s.entry_bits << " N=" << s.leaf_count
              << " h=" << s.bundle_arity << " G=" << s.record_coeffs
              << " keys req/reg=" << s.required_key_height << "/"
              << s.registered_key_height << " h_q=" << s.expansion_height
              << " proj=" << s.projection_depth_max << " r=" << s.record_index_bits
              << " " << label << (m32 ? " ref+m32" : " ref") << " leaves="
              << leaves.size() << " min budget " << min_budget << " bits");
}

// Candidate-specific key registration end to end (design section 9;
// SUPERRECORD_KEY_MODE=candidate in the sweep). Every other serving test
// registers test_policy's fixed log2 n family, so the planned
// required_key_height never decides what the server actually holds. Here each
// fixture registers exactly required_key_height automorphisms and must still
// serve and decode every level on both paths.
//   A: h = 1 with N = rho = n / G, so the leaf level holds one super-record
//      per record slot and its rotation spans the whole rho-trace:
//      projection_depth_max == r == log2(n / G), which this shape's one-plane
//      query never needs in expansion depth. The projection term of the max
//      therefore decides the family, and that family is exactly what MLWE
//      merging asks for, so the merge packing is served at its key boundary.
//   B: E=256, N=256, h=3, G=64 -> the expansion height decides instead (it
//      ties the projection depth at n = 4096, hence the >= in its check).
// Fixture A's leaf count is derived from the ring so both facts hold at every
// ACTIVE_CONFIG, not only at n = 2048.
void exercise_candidate_specific_keys(PirClient &client,
                                      const PirParams &scheme) {
  const bool composite = scheme.get_composite_rns().enabled;
  const size_t log_n = std::bit_width(DBConsts::PolyDegree) - 1;
  constexpr size_t kFixtureAG = 4;

  struct Fixture {
    size_t E, N, h, G, b;
    bool projection_decides;  // otherwise the expansion height decides
    bool mlwe;                // additionally serve with MlweMerge
    bool unique_shape;        // served nowhere else: sample more leaves
  };
  const std::vector<Fixture> fixtures = {
      {13, DBConsts::PolyDegree / kFixtureAG, 1, kFixtureAG, 0, true, true, true},
      {256, 256, 3, 64, 0, false, false, false},
  };
  for (const Fixture &f : fixtures) {
    const TreeParams params =
        plan_for(scheme, f.E, f.N, f.h, f.G, f.b, candidate_key_policy());
    const CandidateShape &s = params.shape;
    // Fixture premises only: freeze_candidate already ties required_key_height
    // to max(expansion, projection) and to the fingerprint, but it demands no
    // more than registered >= required, so nothing outside candidate mode says
    // the two are equal or that the family is smaller than the fixed one.
    require_test(s.registered_key_height == s.required_key_height &&
                     params.scheme.eval_key_registration ==
                         EvalKeyRegistrationPolicy::CandidateSpecificHeight,
                 "candidate-specific plans register exactly the required height");
    require_test(s.registered_key_height < log_n,
                 "the fixture must register fewer keys than the fixed log2 n family");
    if (f.projection_decides) {
      require_test(s.projection_depth_max > s.expansion_height,
                   "fixture A: the deepest projection, not the expansion height, "
                   "must decide the family");
      require_test(s.registered_key_height == s.record_index_bits,
                   "fixture A: the family must be exactly the rho-trace MLWE merging needs");
    } else {
      require_test(s.expansion_height >= s.projection_depth_max,
                   "fixture B: the expansion height must decide the family");
    }

    PirServer server(query_expansion_params(params, scheme));
    const TaggedTreeSessionKeys session =
        create_tagged_tree_session_keys(client, params, scheme);
    // Registration itself validates the material against the plan's own
    // registered height, so a create_session_keys that handed out the fixed
    // maximum family (or one key short) throws here, before any homomorphic
    // work; the serves below are what the plan's number has to survive.
    set_tree_client_session_keys(server, params, scheme, session);

    const EntrySource source = [&](size_t level, size_t node) {
      return synthetic_entry(f.E, level, node);
    };
    const ReferencePreprocessedTree reference =
        preprocess_reference(params, scheme, source);
    // Both ends of the leaf range, plus a pseudo-random leaf for fixture A,
    // whose shape is served nowhere else. Fixture B repeats a shape the
    // fixed-policy section above already serves over eleven leaves (plaintext
    // boundaries included); the only thing new here is the key family, and the
    // family is consumed identically for every leaf, so two leaves carry it.
    std::set<size_t> leaves = {0, f.N - 1};
    if (f.unique_shape) leaves.insert(0x2BADBEEFULL % f.N);
    std::optional<PreprocessedTree> m32;
    std::optional<CandidateNoiseResult> projection_noise;
    if (composite) {
      // The gate re-derives the expected automorphism family from the plan
      // tag, which in candidate mode says `registered`, not log2 n, and
      // m32_from_reference refuses a result whose observed coverage does not
      // match it: a gate that compared against the ring's native height would
      // reject this session while every fixed-policy fixture kept passing.
      projection_noise = passing_target_noise_result(
          client, params, scheme, server, session, source, f.N - 1);
      m32 = m32_from_reference(reference, params, scheme, *projection_noise,
                               true);
    }
    serve_with_exact_keys(client, server, session, params, scheme, reference,
                          m32 ? &*m32 : nullptr,
                          projection_noise ? &*projection_noise : nullptr,
                          source, leaves, ResponsePacking::Projection);
    if (f.mlwe) {
      std::optional<CandidateNoiseResult> merge_noise;
      if (composite) {
        // serve_request_m32 rejects a gate result taken under another packing,
        // so the merge path needs its own run of the gate.
        merge_noise = passing_target_noise_result(
            client, params, scheme, server, session, source, f.N - 1, 1.0,
            ResponsePacking::MlweMerge);
      }
      serve_with_exact_keys(client, server, session, params, scheme, reference,
                            m32 ? &*m32 : nullptr,
                            merge_noise ? &*merge_noise : nullptr, source,
                            leaves, ResponsePacking::MlweMerge);
    }
  }
}

}  // namespace

// Full encrypted pipeline on small shapes: packed query, expansion, ragged
// pyramid, reference and m32 selection, shared-selector fold and rotation,
// projection, pooled multi-plane responses, and client decode; plus the
// client/session/tag bindings, the update path, and serving with a
// candidate-specific key family (design sections 5, 9, 12-15).
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
    CandidateNoiseResult noise_result;
    if (composite) {
      noise_result = passing_target_noise_result(client, params, scheme, server,
                                                 session, source, shape.N - 1);
      m32 = m32_from_reference(reference, params, scheme, noise_result, true);
    }

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
                                                    params, scheme,
                                                    noise_result, nullptr);
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
        PathResponse truncated = response;
        truncated.chunks.pop_back();
        require_test(throws_invalid([&] {
                       (void)serialized_response_payload_bytes(truncated,
                                                               server, params);
                     }),
                     "response serialization rejects a truncated chunk list");
        PathResponse out_of_range = response;
        out_of_range.chunks[0].c0[0] = scheme.get_small_q();
        require_test(throws_invalid([&] {
                       (void)serialized_response_payload_bytes(out_of_range,
                                                               server, params);
                     }),
                     "response serialization rejects non-small-q coefficients");
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
                                                    params, scheme,
                                                    noise_result, nullptr);
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

  // ---- candidate-specific key registration serves at its key boundary ----
  exercise_candidate_specific_keys(client, scheme);
}
