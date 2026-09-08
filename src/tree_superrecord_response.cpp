#include "tree_superrecord_response.h"

#include "tree_project.h"
#include "tree_rotate.h"
#include "tree_select.h"
#include "tree_superrecord_noise.h"

#include <chrono>
#include <functional>
#include <sstream>

namespace tree_superrecord {

namespace {

// Shared level loop: rotation, projection, and pooled placement around a
// caller-provided SelectLevel; then the response-wide modulus switch.
PathResponse pack_path(const std::function<RlweCt(size_t, size_t)> &select,
                       ExpandedQuery &query, PirServer &server,
                       const TaggedTreeSessionKeys &session,
                       const TreeParams &params, const PirParams &scheme,
                       TreeServerTiming *timing) {
  const CandidateShape &shape = params.shape;
  const bvks::BvGaloisKeys &keys = session.material->bv_galois_keys;
  const size_t width = scheme.K() * scheme.get_poly_degree();
  std::span<GSWCt> sigma(query.sigma_selectors);

  PathResponse response;
  response.tag = params.tag;
  response.client_id = query.client_id;
  response.chunks.resize(shape.response_ciphertexts);
  for (RlweCt &chunk : response.chunks) {
    chunk.c0.assign(width, 0);
    chunk.c1.assign(width, 0);
    chunk.ntt_form = false;
  }

  StageClock clock(timing);
  for (size_t level = 0; level <= shape.tree_height; ++level) {
    const LevelPlan &plan = params.level_plans[level];
    for (size_t plane = 0; plane < shape.plane_count; ++plane) {
      RlweCt ct = select(level, plane);  // laps its own stages
      clock.restart();
      for (size_t idx = 0; idx < plan.rotation_bit_count; ++idx) {
        const size_t v = plan.rotation_bit_begin + idx;
        ct = rot_select(ct, sigma[v], size_t{1} << idx, server, scheme);
      }
      clock.lap(&TreeServerTiming::private_rotate);
      ct = project_keep_stride(std::move(ct), plan.projection_depth, keys,
                               scheme);
      clock.lap(&TreeServerTiming::projection);
      const PlanePlacement at = response_placement(level, plane, params);
      RlweCt &chunk = response.chunks[at.response_chunk];
      if (at.residue_offset == 0) {
        tree_ct_add_inplace(chunk, ct, scheme);
      } else {
        const RlweCt shifted = mul_x_pow(ct, at.residue_offset, scheme);
        tree_ct_add_inplace(chunk, shifted, scheme);
      }
      clock.lap(&TreeServerTiming::path_pack);
    }
  }
  clock.restart();
  for (RlweCt &chunk : response.chunks) {
    response.small_q = server.switch_response_to_small_q(chunk);
  }
  clock.lap(&TreeServerTiming::modulus_switch);
  return response;
}

void require_expanded_query(const ExpandedQuery &query,
                            const TaggedTreeSessionKeys &session,
                            const TreeParams &params) {
  require(query.tag == params.tag, "expanded query PlanTag mismatch");
  require(query.client_id == session.client_id,
          "expanded query and session belong to different clients");
  require(query.alpha.size() == params.shape.N0 &&
              query.sigma_selectors.size() == params.shape.selector_bits,
          "expanded query does not match the plan's shape");
}

}  // namespace

PathResponse answer_path_reference(const ReferencePreprocessedTree &db,
                                   ExpandedQuery &query, PirServer &server,
                                   const TaggedTreeSessionKeys &session,
                                   const TreeParams &params,
                                   const PirParams &runtime_scheme,
                                   TreeServerTiming *timing) {
  require_expanded_query(query, session, params);
  require(db.tag == params.tag, "reference database PlanTag mismatch");
  require(db.canonical.size() == params.shape.path_levels,
          "reference database is missing levels");
  require_registered_session_binding(server, params, runtime_scheme, session);

  StageClock clock(timing);
  const AlphaPyramid pyramid =
      build_ragged_alpha_pyramid(query.alpha, params, runtime_scheme);
  clock.lap(&TreeServerTiming::alpha_prepare);
  return pack_path(
      [&](size_t level, size_t plane) {
        require(db.canonical[level].size() == params.shape.plane_count,
                "reference level is missing planes");
        return select_level_reference(
            db.canonical[level][plane], params.level_plans[level], pyramid,
            std::span<GSWCt>(query.sigma_selectors), server, params,
            runtime_scheme, timing);
      },
      query, server, session, params, runtime_scheme, timing);
}

PathResponse answer_path_m32(const PreprocessedTree &db, ExpandedQuery &query,
                             PirServer &server,
                             const TaggedTreeSessionKeys &session,
                             const TreeParams &params,
                             const PirParams &runtime_scheme,
                             const CandidateNoiseResult &noise_result,
                             TreeServerTiming *timing) {
  require_expanded_query(query, session, params);
  require_target_noise_gate(noise_result, params, session.client_id);
  require(db.tag == params.tag, "m32 database PlanTag mismatch");
  require(db.m32.size() == params.shape.path_levels,
          "m32 database is missing levels");
  require_registered_session_binding(server, params, runtime_scheme, session);
  require_m32_scheme(runtime_scheme);

  StageClock clock(timing);
  const AlphaPyramid pyramid_ntt =
      build_ragged_alpha_pyramid_ntt(query.alpha, params, runtime_scheme);
  const ::AlphaPyramidM32 pyramid_m32 =
      pyramid_to_m32(pyramid_ntt, runtime_scheme);
  clock.lap(&TreeServerTiming::alpha_prepare);
  return pack_path(
      [&](size_t level, size_t plane) {
        require(db.m32[level].size() == params.shape.plane_count,
                "m32 level is missing planes");
        return select_level_m32(db.m32[level][plane],
                                params.level_plans[level], pyramid_m32,
                                std::span<GSWCt>(query.sigma_selectors),
                                server, params, runtime_scheme, timing);
      },
      query, server, session, params, runtime_scheme, timing);
}

namespace {

template <typename Answer>
PathResponse serve_request(PackedQuery packed_query, PirServer &server,
                           const TaggedTreeSessionKeys &session,
                           const TreeParams &params,
                           const PirParams &runtime_scheme,
                           TreeServerTiming *timing, Answer &&answer) {
  using Clock = std::chrono::steady_clock;
  const Clock::time_point start = Clock::now();
  require(packed_query.tag == params.tag, "packed query PlanTag mismatch");
  require(packed_query.client_id == session.client_id,
          "packed query and session belong to different clients");
  require_query_server_binding(server, params, runtime_scheme);
  require_registered_session_binding(server, params, runtime_scheme, session);
  ExpandedQuery expanded = unpack_query(server, runtime_scheme, params, session,
                                        std::move(packed_query), timing);
  PathResponse response = answer(expanded);
  if (timing) timing->total += Clock::now() - start;
  return response;
}

}  // namespace

PathResponse serve_request_reference(PackedQuery packed_query,
                                     const ReferencePreprocessedTree &db,
                                     PirServer &query_eval_server,
                                     const TaggedTreeSessionKeys &session,
                                     const TreeParams &params,
                                     const PirParams &runtime_scheme,
                                     TreeServerTiming *timing) {
  return serve_request(std::move(packed_query), query_eval_server, session,
                       params, runtime_scheme, timing,
                       [&](ExpandedQuery &expanded) {
                         return answer_path_reference(db, expanded,
                                                      query_eval_server,
                                                      session, params,
                                                      runtime_scheme, timing);
                       });
}

PathResponse serve_request_m32(PackedQuery packed_query,
                               const PreprocessedTree &db,
                               PirServer &query_eval_server,
                               const TaggedTreeSessionKeys &session,
                               const TreeParams &params,
                               const PirParams &runtime_scheme,
                               const CandidateNoiseResult &noise_result,
                               TreeServerTiming *timing) {
  require_target_noise_gate(noise_result, params, session.client_id);
  return serve_request(std::move(packed_query), query_eval_server, session,
                       params, runtime_scheme, timing,
                       [&](ExpandedQuery &expanded) {
                         return answer_path_m32(db, expanded,
                                                query_eval_server, session,
                                                params, runtime_scheme,
                                                noise_result,
                                                timing);
                       });
}

namespace {

struct DecryptedChunks {
  std::vector<RlwePt> plaintexts;
  uint64_t max_noise = 0;
  int min_budget_bits = 0;
};

DecryptedChunks decrypt_chunks(const PathResponse &response, PirClient &client,
                               const TreeParams &params,
                               const PirParams &runtime_scheme) {
  require_scheme_binding(params, runtime_scheme);
  require(client.scheme_compatible(runtime_scheme),
          "PirClient uses a different HE scheme");
  require(response.tag == params.tag, "response PlanTag mismatch");
  require(response.client_id == client.get_client_id(),
          "response belongs to a different client/key instance");
  require(response.chunks.size() == params.shape.response_ciphertexts,
          "response chunk count does not match the plan");
  DecryptedChunks out;
  out.min_budget_bits = 1 << 20;
  for (const RlweCt &chunk : response.chunks) {
    if (response.small_q) {
      uint64_t noise = 0;
      int budget = 0;
      out.plaintexts.push_back(
          client.decrypt_mod_q_measured(chunk, noise, budget));
      out.max_noise = std::max(out.max_noise, noise);
      out.min_budget_bits = std::min(out.min_budget_bits, budget);
    } else {
      out.plaintexts.push_back(client.decrypt_ct(chunk));
      out.min_budget_bits =
          std::min(out.min_budget_bits, client.noise_budget(chunk));
    }
  }
  return out;
}

std::vector<std::vector<EntryBits>> decode_levels(
    const DecryptedChunks &chunks, const TreeParams &params) {
  const CandidateShape &shape = params.shape;
  const CodecSpec spec = codec_spec_for(params);
  const size_t rho = shape.records_per_plaintext;
  std::vector<std::vector<EntryBits>> levels;
  levels.reserve(shape.path_levels);
  for (size_t level = 0; level <= shape.tree_height; ++level) {
    EncodedSuperRecord encoded(shape.plane_count,
                               PlaneDigits(shape.record_coeffs, 0));
    for (size_t plane = 0; plane < shape.plane_count; ++plane) {
      const PlanePlacement at = response_placement(level, plane, params);
      const RlwePt &pt = chunks.plaintexts[at.response_chunk];
      for (size_t d = 0; d < shape.record_coeffs; ++d) {
        encoded[plane][d] = pt.data[at.residue_offset + d * rho];
      }
    }
    levels.push_back(decode_super_record(encoded, spec));
  }
  return levels;
}

}  // namespace

DecodedPath extract_path_measured(const PathResponse &response,
                                  PirClient &client, const TreeParams &params,
                                  const PirParams &runtime_scheme,
                                  size_t leaf) {
  const CandidateShape &shape = params.shape;
  require(leaf < shape.leaf_count, "leaf index is out of range");
  const DecryptedChunks chunks =
      decrypt_chunks(response, client, params, runtime_scheme);
  const std::vector<std::vector<EntryBits>> levels =
      decode_levels(chunks, params);
  DecodedPath path;
  path.max_noise = chunks.max_noise;
  path.min_budget_bits = chunks.min_budget_bits;
  path.entries.reserve(shape.path_levels);
  for (size_t level = 0; level <= shape.tree_height; ++level) {
    const size_t lane = (leaf >> (shape.tree_height - level)) % shape.bundle_arity;
    path.entries.push_back(levels[level][lane]);
  }
  return path;
}

std::vector<EntryBits> extract_path(const PathResponse &response,
                                    PirClient &client, const TreeParams &params,
                                    const PirParams &runtime_scheme,
                                    size_t leaf) {
  return extract_path_measured(response, client, params, runtime_scheme, leaf)
      .entries;
}

namespace debug {
std::vector<std::vector<EntryBits>> extract_all_lanes_for_test(
    const PathResponse &response, PirClient &client, const TreeParams &params,
    const PirParams &runtime_scheme) {
  return decode_levels(decrypt_chunks(response, client, params, runtime_scheme),
                       params);
}
}  // namespace debug

std::optional<size_t> serialized_response_payload_bytes(
    const PathResponse &response, PirServer &response_codec_server,
    const TreeParams &params) {
  require(response.tag == params.tag, "response PlanTag mismatch");
  require_scheme_binding(params, response_codec_server.get_params());
  require(response.chunks.size() == params.shape.response_ciphertexts,
          "response chunk count does not match the plan");
  if (!response.small_q) return std::nullopt;
  const size_t degree = params.shape.ring_degree;
  const uint64_t small_q = response_codec_server.get_params().get_small_q();
  size_t total = 0;
  for (const RlweCt &chunk : response.chunks) {
    require(!chunk.ntt_form, "small-q chunk must be in coefficient form");
    require(chunk.c0.size() == degree && chunk.c1.size() == degree,
            "small-q chunk must be a single limb");
    for (uint64_t coeff : chunk.c0) {
      require(coeff < small_q, "small-q c0 coefficient is out of range");
    }
    for (uint64_t coeff : chunk.c1) {
      require(coeff < small_q, "small-q c1 coefficient is out of range");
    }
    std::stringstream stream;
    total = checked_add(total,
                        response_codec_server.save_resp_to_stream(chunk, stream),
                        "response payload bytes");
  }
  return total;
}

}  // namespace tree_superrecord
