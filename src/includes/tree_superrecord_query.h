#pragma once

#include "client.h"
#include "pir_session.h"
#include "server.h"
#include "tree_profile.h"
#include "tree_superrecord_index.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// Packed query, expansion, and client/session binding for the generic
// super-record path (design specification, sections 5.4 and 9).
//
// Packed layout (logical slots; each slot is written at BitRev(slot, h_q)):
//   [alpha one-hot: N0 BFV constants]
//   [sigma bit 0: L_EP RLWE' gadget rows] ... [sigma bit m-1: L_EP rows]
// Both kinds of constants are pre-scaled by W^{-1}, W = 2^{expansion_height},
// because ExpandBFV multiplies every slot by W. The same m RGSW selectors
// drive plaintext-index folding and private record rotation on every level.
//
// Key material is inspected, not trusted: a session is usable only when its
// BV Galois family contains exactly eta_j = (n >> j) + 1 for every
// j < registered height with the right row shapes, and its completion key has
// 2 L_KEY rows of 2 K n coefficients. `client_id` ties the query-generating
// PirClient, the registered session, the packed query, and the response
// together so two clients' material cannot be mixed by accident.

namespace tree_superrecord {

struct TaggedTreeSessionKeys {
  SchemeFingerprint scheme;
  size_t client_id = 0;
  SharedPirSessionKeys material;
};

struct PackedQuery {
  PlanTag tag;
  size_t client_id = 0;
  RlweCt ciphertext;
};

struct ExpandedQuery {
  PlanTag tag;
  size_t client_id = 0;
  std::vector<RlweCt> alpha;           // N0 BFV one-hot constants
  std::vector<GSWCt> sigma_selectors;  // selector_bits RGSW bits
};

struct BvKeyCoverage {
  size_t contiguous_height = 0;
  size_t key_count = 0;
  std::vector<uint32_t> missing_required;
  std::vector<uint32_t> unexpected;
  std::vector<uint32_t> duplicates;
  std::vector<uint32_t> malformed_row_shapes;
};

std::vector<uint32_t> required_bv_automorphisms(size_t ring_degree,
                                                size_t height);
BvKeyCoverage inspect_bv_key_coverage(const bvks::BvGaloisKeys &keys,
                                      const PirSchemeIdentity &identity,
                                      size_t registered_height);
void require_bv_key_coverage(const bvks::BvGaloisKeys &keys,
                             const PirSchemeIdentity &identity,
                             size_t required_height,
                             size_t registered_height);
// Structural shape only (2 L_KEY rows of 2 K n coefficients); modulus and
// NTT-domain provenance come from the trusted generation path.
void require_completion_key_shape(const GSWCt &completion_key,
                                  const PirSchemeIdentity &identity);

// Query-shaped view: fst_dim_sz = N0, selector_bits selectors, the selected
// expansion height. Construct the query-evaluation PirServer from it.
PirParams query_expansion_params(const TreeParams &params,
                                 const PirParams &runtime_scheme);

TaggedTreeSessionKeys create_tagged_tree_session_keys(
    PirClient &client, const TreeParams &params,
    const PirParams &runtime_scheme);

void require_query_server_binding(const PirServer &query_eval_server,
                                  const TreeParams &params,
                                  const PirParams &runtime_scheme);
void require_session_material_binding(const TreeParams &params,
                                      const PirParams &runtime_scheme,
                                      const TaggedTreeSessionKeys &session);
void require_registered_session_binding(const PirServer &query_eval_server,
                                        const TreeParams &params,
                                        const PirParams &runtime_scheme,
                                        const TaggedTreeSessionKeys &session);
// Validates, registers under session.client_id, then re-validates.
void set_tree_client_session_keys(PirServer &query_eval_server,
                                  const TreeParams &params,
                                  const PirParams &runtime_scheme,
                                  const TaggedTreeSessionKeys &session);

// Fresh encryption randomness on every call; only the alpha one-hot and the
// sigma bits enter the ciphertext.
PackedQuery make_query(PirClient &client, const TaggedTreeSessionKeys &session,
                       const PirParams &runtime_scheme,
                       const TreeParams &params, size_t leaf);

// ExpandBFV with useful-leaf pruning followed by RGSW completion, under the
// registered session named by the query's client_id.
ExpandedQuery unpack_query(PirServer &query_eval_server,
                           const PirParams &runtime_scheme,
                           const TreeParams &params,
                           const TaggedTreeSessionKeys &session,
                           PackedQuery query,
                           TreeServerTiming *timing = nullptr);

}  // namespace tree_superrecord
