#pragma once

#include "client.h"
#include "tree_profile.h"
#include "tree_superrecord_query.h"
#include "tree_superrecord_select.h"

#include <cstddef>
#include <optional>
#include <vector>

// Full path answer and client decode for the generic super-record path
// (design specification, sections 12-14).
//
// For every (level, plane): SelectLevel -> private rotation by X^{-u} driven
// by sigma bits [k, k + d) -> projection pi_d keeping the stride lattice
// {0, rho, ..., (G-1) rho} -> public shift X^{offset} into the pooled chunk
// item / rho at residue item mod rho, item = level * P + plane. The largest
// occupied exponent is (rho - 1) + (G - 1) rho = n - 1, so no item wraps.
// Every chunk is modulus-switched once at the end. The client reconstructs
// all P planes of the selected super-record, decodes h lanes, and returns
// lane (leaf >> (L - level)) mod h.

namespace tree_superrecord {

struct PathResponse {
  PlanTag tag;
  size_t client_id = 0;
  std::vector<RlweCt> chunks;  // ceil(P * H / rho) same-ring ciphertexts
  bool small_q = false;        // response-wide modulus state
};

PathResponse answer_path_reference(const ReferencePreprocessedTree &db,
                                   ExpandedQuery &query, PirServer &server,
                                   const TaggedTreeSessionKeys &session,
                                   const TreeParams &params,
                                   const PirParams &runtime_scheme,
                                   TreeServerTiming *timing = nullptr);
PathResponse answer_path_m32(const PreprocessedTree &db, ExpandedQuery &query,
                             PirServer &server,
                             const TaggedTreeSessionKeys &session,
                             const TreeParams &params,
                             const PirParams &runtime_scheme,
                             TreeServerTiming *timing = nullptr);

// Full request: bindings, expansion, selector completion, answer path.
// `total` covers the whole wrapper; the other stages are exclusive.
PathResponse serve_request_reference(PackedQuery packed_query,
                                     const ReferencePreprocessedTree &db,
                                     PirServer &query_eval_server,
                                     const TaggedTreeSessionKeys &session,
                                     const TreeParams &params,
                                     const PirParams &runtime_scheme,
                                     TreeServerTiming *timing = nullptr);
PathResponse serve_request_m32(PackedQuery packed_query,
                               const PreprocessedTree &db,
                               PirServer &query_eval_server,
                               const TaggedTreeSessionKeys &session,
                               const TreeParams &params,
                               const PirParams &runtime_scheme,
                               TreeServerTiming *timing = nullptr);

struct DecodedPath {
  std::vector<EntryBits> entries;  // root first, H entries of E bits
  // Largest centered residue over all chunks (small-q units; 0 when the
  // response stayed at full q) and the smallest remaining budget in bits.
  uint64_t max_noise = 0;
  int min_budget_bits = 0;
};
DecodedPath extract_path_measured(const PathResponse &response,
                                  PirClient &client, const TreeParams &params,
                                  const PirParams &runtime_scheme, size_t leaf);
std::vector<EntryBits> extract_path(const PathResponse &response,
                                    PirClient &client, const TreeParams &params,
                                    const PirParams &runtime_scheme,
                                    size_t leaf);

namespace debug {
// All h lanes of every level's selected bundle; tests and diagnostics only.
std::vector<std::vector<EntryBits>> extract_all_lanes_for_test(
    const PathResponse &response, PirClient &client, const TreeParams &params,
    const PirParams &runtime_scheme);
}  // namespace debug

// Sum of one real serializer call per small-q chunk (ciphertext payload
// only, no tag or framing); empty while the response is still at full q.
std::optional<size_t> serialized_response_payload_bytes(
    const PathResponse &response, PirServer &response_codec_server,
    const TreeParams &params);

}  // namespace tree_superrecord
