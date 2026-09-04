#include "tree_superrecord_query.h"

#include "tree_query.h"
#include "utils.h"

#include <algorithm>
#include <stdexcept>

namespace tree_superrecord {

std::vector<uint32_t> required_bv_automorphisms(size_t ring_degree,
                                                size_t height) {
  require(height <= exact_log2(ring_degree, "ring degree"),
          "automorphism family height exceeds log2 of the ring degree");
  std::vector<uint32_t> out;
  out.reserve(height);
  for (size_t j = 0; j < height; ++j) {
    out.push_back(static_cast<uint32_t>((ring_degree >> j) + 1));
  }
  return out;
}

BvKeyCoverage inspect_bv_key_coverage(const bvks::BvGaloisKeys &keys,
                                      const PirSchemeIdentity &identity,
                                      size_t registered_height) {
  BvKeyCoverage coverage;
  coverage.key_count = keys.keys.size();
  const std::vector<uint32_t> expected =
      required_bv_automorphisms(identity.ring_degree, registered_height);
  const size_t expected_poly_width = checked_mul(
      identity.rns_moduli.size(), identity.ring_degree, "BV row width");
  for (uint32_t eta : expected) {
    size_t count = 0;
    for (const bvks::BvKeySwitchKey &key : keys.keys) {
      if (key.galois_k == eta) ++count;
    }
    if (count == 0) coverage.missing_required.push_back(eta);
    if (count > 1) coverage.duplicates.push_back(eta);
  }
  for (const bvks::BvKeySwitchKey &key : keys.keys) {
    if (std::find(expected.begin(), expected.end(), key.galois_k) ==
        expected.end()) {
      coverage.unexpected.push_back(key.galois_k);
    }
    bool malformed = key.cts.size() != identity.bv_ks_rows;
    for (const bvks::BvRlweCt &row : key.cts) {
      if (row.a.size() != expected_poly_width ||
          row.b.size() != expected_poly_width) {
        malformed = true;
      }
    }
    if (malformed) coverage.malformed_row_shapes.push_back(key.galois_k);
  }
  const size_t native_max = exact_log2(identity.ring_degree, "ring degree");
  for (size_t j = 0; j < native_max; ++j) {
    if (!keys.contains(static_cast<uint32_t>((identity.ring_degree >> j) + 1))) {
      break;
    }
    coverage.contiguous_height = j + 1;
  }
  return coverage;
}

void require_bv_key_coverage(const bvks::BvGaloisKeys &keys,
                             const PirSchemeIdentity &identity,
                             size_t required_height,
                             size_t registered_height) {
  require(required_height <= registered_height,
          "required key height exceeds the registered height");
  const BvKeyCoverage coverage =
      inspect_bv_key_coverage(keys, identity, registered_height);
  require(coverage.missing_required.empty(),
          "BV key family is missing a required automorphism");
  require(coverage.duplicates.empty(),
          "BV key family has a duplicated automorphism");
  require(coverage.unexpected.empty(),
          "BV key family has an unexpected automorphism");
  require(coverage.malformed_row_shapes.empty(),
          "BV key has a wrong gadget-row count or row width");
  require(coverage.key_count == registered_height,
          "BV key count differs from the registered height");
}

void require_completion_key_shape(const GSWCt &completion_key,
                                  const PirSchemeIdentity &identity) {
  require(completion_key.size() == 2 * identity.completion_key_gadget_rows,
          "completion key has the wrong row count");
  const size_t expected_row = checked_mul(
      2 * identity.rns_moduli.size(), identity.ring_degree, "RGSW row width");
  for (const std::vector<uint64_t> &row : completion_key) {
    require(row.size() == expected_row,
            "completion key row has the wrong width");
  }
}

PirParams query_expansion_params(const TreeParams &params,
                                 const PirParams &runtime_scheme) {
  require_scheme_binding(params, runtime_scheme);
  return runtime_scheme.with_query_shape(
      {params.shape.N0, params.shape.selector_bits,
       params.shape.expansion_height});
}

TaggedTreeSessionKeys create_tagged_tree_session_keys(
    PirClient &client, const TreeParams &params,
    const PirParams &runtime_scheme) {
  require_scheme_binding(params, runtime_scheme);
  require(client.scheme_compatible(runtime_scheme),
          "PirClient uses a different HE scheme");
  TaggedTreeSessionKeys session;
  session.scheme = params.scheme;
  session.client_id = client.get_client_id();
  session.material =
      client.create_session_keys(params.shape.registered_key_height);
  return session;
}

void require_query_server_binding(const PirServer &query_eval_server,
                                  const TreeParams &params,
                                  const PirParams &runtime_scheme) {
  require_scheme_binding(params, runtime_scheme);
  const PirParams &actual = query_eval_server.get_params();
  require(actual.get_fst_dim_sz() == params.shape.N0,
          "query server has the wrong N0");
  require(actual.get_num_other_dims() == params.shape.selector_bits,
          "query server has the wrong selector count");
  require(actual.get_expan_height() == params.shape.expansion_height,
          "query server has the wrong expansion height");
  require(runtime_scheme.scheme_compatible(actual),
          "query server has the wrong HE scheme");
}

void require_session_material_binding(const TreeParams &params,
                                      const PirParams &runtime_scheme,
                                      const TaggedTreeSessionKeys &session) {
  require(session.material != nullptr, "session material is null");
  require(session.scheme == params.scheme,
          "session fingerprint differs from the plan");
  require_scheme_binding(params, runtime_scheme);
  require_bv_key_coverage(session.material->bv_galois_keys,
                          params.scheme.identity,
                          params.shape.required_key_height,
                          params.shape.registered_key_height);
  require_completion_key_shape(session.material->gsw_key,
                               params.scheme.identity);
}

void require_registered_session_binding(const PirServer &query_eval_server,
                                        const TreeParams &params,
                                        const PirParams &runtime_scheme,
                                        const TaggedTreeSessionKeys &session) {
  require_query_server_binding(query_eval_server, params, runtime_scheme);
  require_session_material_binding(params, runtime_scheme, session);
  SharedPirSessionKeys stored;
  try {
    stored = query_eval_server.client_session_keys(session.client_id);
  } catch (const std::out_of_range &) {
    fail("no session is registered for the tagged client id");
  }
  require(stored == session.material,
          "server session material differs from the tagged registration");
}

void set_tree_client_session_keys(PirServer &query_eval_server,
                                  const TreeParams &params,
                                  const PirParams &runtime_scheme,
                                  const TaggedTreeSessionKeys &session) {
  require_query_server_binding(query_eval_server, params, runtime_scheme);
  require_session_material_binding(params, runtime_scheme, session);
  query_eval_server.set_client_session_keys(session.client_id,
                                            session.material);
  require_registered_session_binding(query_eval_server, params, runtime_scheme,
                                     session);
}

PackedQuery make_query(PirClient &client, const TaggedTreeSessionKeys &session,
                       const PirParams &runtime_scheme,
                       const TreeParams &params, size_t leaf) {
  require_scheme_binding(params, runtime_scheme);
  require(client.scheme_compatible(runtime_scheme),
          "PirClient uses a different HE scheme");
  require(session.scheme == params.scheme,
          "session fingerprint differs from the plan");
  require(client.get_client_id() == session.client_id,
          "query and evaluation keys belong to different clients");
  const CandidateShape &shape = params.shape;
  const ClientCoordinates coords = client_index(leaf, params);

  PackedQuery query;
  query.tag = params.tag;
  query.client_id = session.client_id;
  query.ciphertext = client.fresh_zero_ct();

  // Alpha one-hot: Delta * (W^{-1} mod t) at logical slot alpha; expansion
  // multiplies by W and leaves Enc(1) there and Enc(0) elsewhere.
  const uint64_t t = runtime_scheme.get_plain_mod();
  uint64_t w_inv_t = 0;
  require(utils::try_invert_uint_mod(shape.expansion_capacity, t, w_inv_t),
          "W is not invertible modulo t");
  add_bfv_query_constant(query.ciphertext, runtime_scheme, coords.alpha,
                         shape.expansion_height, w_inv_t);

  // Sigma bits: for every bit equal to one, L_EP RLWE' rows carrying
  // (W^{-1} mod q_k) * G[k][row]; zero bits keep the zero encryption.
  const auto &qs = runtime_scheme.get_rns_mods();
  const size_t K = runtime_scheme.K();
  std::vector<uint64_t> w_inv_q(K);
  for (size_t k = 0; k < K; ++k) {
    require(utils::try_invert_uint_mod(shape.expansion_capacity, qs[k],
                                       w_inv_q[k]),
            "W is not invertible modulo a RNS limb");
  }
  const size_t ell = runtime_scheme.get_l();
  const std::vector<std::vector<uint64_t>> gadget = utils::gsw_gadget(
      ell, runtime_scheme.get_base_log2(),
      std::vector<uint64_t>(qs.begin(), qs.end()));
  std::vector<uint64_t> value_per_limb(K);
  for (size_t v = 0; v < shape.selector_bits; ++v) {
    if (coords.sigma_bits_le[v] == 0) continue;
    for (size_t row = 0; row < ell; ++row) {
      const size_t logical = shape.N0 + v * ell + row;
      for (size_t k = 0; k < K; ++k) {
        value_per_limb[k] = static_cast<uint64_t>(
            (uint128_t)gadget[k][row] * w_inv_q[k] % qs[k]);
      }
      add_rlwe_star_constant(query.ciphertext, runtime_scheme, logical,
                             shape.expansion_height, value_per_limb);
    }
  }
  return query;
}

ExpandedQuery unpack_query(PirServer &query_eval_server,
                           const PirParams &runtime_scheme,
                           const TreeParams &params,
                           const TaggedTreeSessionKeys &session,
                           PackedQuery query, TreeServerTiming *timing) {
  require(query.tag == params.tag, "packed query PlanTag mismatch");
  require(query.client_id == session.client_id,
          "query and session belong to different clients");
  require_registered_session_binding(query_eval_server, params, runtime_scheme,
                                     session);
  const CandidateShape &shape = params.shape;

  StageClock clock(timing);
  std::vector<RlweCt> expanded =
      query_eval_server.expand_query(session.client_id, query.ciphertext);
  clock.lap(&TreeServerTiming::query_expand);
  require(expanded.size() == shape.query_width,
          "expansion returned an unexpected leaf count");

  std::vector<GSWCt> selectors =
      query_eval_server.complete_selectors(session.client_id, expanded);
  clock.lap(&TreeServerTiming::selector_completion);
  require(selectors.size() == shape.selector_bits,
          "selector completion returned an unexpected count");

  ExpandedQuery result;
  result.tag = params.tag;
  result.client_id = session.client_id;
  result.alpha.assign(std::make_move_iterator(expanded.begin()),
                      std::make_move_iterator(expanded.begin() + shape.N0));
  result.sigma_selectors = std::move(selectors);
  return result;
}

}  // namespace tree_superrecord
