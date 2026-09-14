#include "tree_superrecord_noise.h"

#include "tree_response_merge.h"
#include "tree_project.h"
#include "tree_rotate.h"
#include "tree_select.h"
#include "tree_query.h"
#include "tree_superrecord_response.h"
#include "tree_superrecord_select.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace tree_superrecord {

namespace {

bool observed_coverage_matches_tag(const BvKeyCoverage &coverage,
                                   const TreeParams &params) {
  return coverage.contiguous_height == params.shape.registered_key_height &&
         coverage.key_count == params.shape.registered_key_height &&
         coverage.missing_required.empty() && coverage.unexpected.empty() &&
         coverage.duplicates.empty() && coverage.malformed_row_shapes.empty();
}

NoiseCheckpoint remaining_budget_checkpoint(const std::string &stage,
                                            PirClient &client,
                                            const RlweCt &ct) {
  const double budget = static_cast<double>(client.noise_budget(ct));
  return NoiseCheckpoint{.stage = stage,
                         .kind = NoiseMeasureKind::RemainingBudgetBits,
                         .value = budget,
                         .decoder_threshold = 0,
                         .margin = budget};
}

RlweCt multiply_ciphertext_scalar(RlweCt ct, uint64_t scalar,
                                  const PirParams &scheme) {
  const size_t degree = scheme.get_poly_degree();
  const auto &qs = scheme.get_rns_mods();
  require(!ct.ntt_form && ct.c0.size() == degree * qs.size() &&
              ct.c1.size() == degree * qs.size(),
          "noise checkpoint scalar normalization needs coefficient-form input");
  for (size_t k = 0; k < qs.size(); ++k) {
    const uint64_t q = qs[k];
    const uint64_t s = scalar % q;
    for (size_t i = 0; i < degree; ++i) {
      const size_t at = k * degree + i;
      ct.c0[at] = static_cast<uint64_t>(
          (static_cast<uint128>(ct.c0[at]) * s) % q);
      ct.c1[at] = static_cast<uint64_t>(
          (static_cast<uint128>(ct.c1[at]) * s) % q);
    }
  }
  return ct;
}

NoiseCheckpoint small_q_budget_checkpoint(const std::string &stage,
                                          PirClient &client,
                                          const RlweCt &ct) {
  uint64_t noise = 0;
  int budget = 0;
  (void)client.decrypt_mod_q_measured(ct, noise, budget);
  return NoiseCheckpoint{.stage = stage,
                         .kind = NoiseMeasureKind::RemainingBudgetBits,
                         .value = static_cast<double>(budget),
                         .decoder_threshold = 0,
                         .margin = static_cast<double>(budget)};
}

NoiseCheckpoint selector_probe_checkpoint(const std::string &stage,
                                          PirClient &client, PirServer &server,
                                          GSWCt &selector,
                                          const PirParams &scheme,
                                          uint8_t expected_bit,
                                          bool &decrypts_correctly) {
  RlweCt zero = client.fresh_zero_ct();
  RlweCt one = client.fresh_zero_ct();
  add_bfv_query_constant(one, scheme, 0, 0, 1);
  RlweCt out;
  out.c0.assign(zero.c0.size(), 0);
  out.c1.assign(zero.c1.size(), 0);
  out.ntt_form = false;
  server.ext_prod_mux(zero, one, selector, out);
  const RlwePt pt = client.decrypt_ct(out);
  if (pt.data.empty() || pt.data[0] != expected_bit) decrypts_correctly = false;
  return remaining_budget_checkpoint(stage, client, out);
}

double normalized_margin(NoiseCheckpoint &checkpoint,
                         bool require_nonnegative_margin) {
  require(std::isfinite(checkpoint.value),
          "noise checkpoint value must be finite");
  if (checkpoint.kind == NoiseMeasureKind::RemainingBudgetBits) {
    checkpoint.decoder_threshold = 0;
    checkpoint.margin = checkpoint.value;
  } else {
    require(std::isfinite(checkpoint.decoder_threshold),
            "absolute-noise threshold must be finite");
    checkpoint.margin = checkpoint.decoder_threshold - checkpoint.value;
  }
  require(std::isfinite(checkpoint.margin),
          "noise checkpoint margin must be finite");
  if (require_nonnegative_margin) {
    require(checkpoint.margin >= 0,
            "noise checkpoint margin must be finite and nonnegative");
  }
  return checkpoint.margin;
}

bool noise_margin_policy_passes(const CandidateNoiseResult &result) {
  if (!std::isfinite(result.min_noise_margin) || result.min_noise_margin < 0 ||
      !std::isfinite(result.minimum_margin)) {
    return false;
  }
  return result.min_noise_margin == 0 ||
         result.minimum_margin >= result.min_noise_margin;
}

void require_mlwe_response_keys(const bvks::BvGaloisKeys &keys,
                                const TreeParams &params) {
  for (size_t j = 0; j < params.shape.record_index_bits; ++j) {
    const uint32_t eta =
        static_cast<uint32_t>((params.shape.ring_degree >> j) + 1);
    require(keys.contains(eta),
            "MLWE response packing requires the full rho-trace BV key family");
  }
}

}  // namespace

bool noise_result_unlocks_candidate(const CandidateNoiseResult &result,
                                    const TreeParams &params,
                                    size_t expected_client_id,
                                    ResponsePacking packing) {
  return result.fixture_kind == NoiseFixtureKind::TargetShapeNoiseGate &&
         result.tag == params.tag &&
         result.client_id == expected_client_id &&
         result.packing == packing &&
         result.eligibility == NoiseEligibility::Passed &&
         result.decrypts_correctly &&
         !result.checkpoints.empty() &&
         noise_margin_policy_passes(result) &&
         result.observed_key_coverage_matches_tag &&
         observed_coverage_matches_tag(result.observed_key_coverage, params);
}

void require_target_noise_gate(const CandidateNoiseResult &result,
                               const TreeParams &params,
                               size_t expected_client_id,
                               ResponsePacking packing) {
  require_trusted_target_noise_result(result, params, packing);
  require(result.client_id == expected_client_id,
          "noise result belongs to a different client/key instance");
}

void require_trusted_target_noise_result(const CandidateNoiseResult &result,
                                         const TreeParams &params,
                                         std::optional<ResponsePacking> packing) {
  require(result.fixture_kind == NoiseFixtureKind::TargetShapeNoiseGate,
          "m32 requires a target-shape noise gate");
  require(result.tag == params.tag, "noise result PlanTag mismatch");
  if (packing) {
    require(result.packing == *packing,
            "noise result response packing does not match the request");
  }
  require(result.eligibility == NoiseEligibility::Passed,
          "target-shape noise gate did not pass");
  require(result.decrypts_correctly,
          "target-shape noise fixture did not decrypt correctly");
  require(!result.checkpoints.empty(),
          "target-shape noise gate has no checkpoints");
  require(noise_margin_policy_passes(result),
          "target-shape noise margin is below policy");
  require(result.observed_key_coverage_matches_tag,
          "noise fixture key coverage does not match plan");
  require(observed_coverage_matches_tag(result.observed_key_coverage, params),
          "noise fixture observed key coverage does not match plan");
}

CandidateNoiseResult build_target_noise_result(
    const TreeParams &params, const PirParams &runtime_scheme,
    const PirServer &query_eval_server, const TaggedTreeSessionKeys &session,
    bool decrypts_correctly, std::vector<NoiseCheckpoint> checkpoints,
    double min_noise_margin, ResponsePacking packing) {
  require_query_server_binding(query_eval_server, params, runtime_scheme);
  require_registered_session_binding(query_eval_server, params, runtime_scheme,
                                     session);
  if (packing == ResponsePacking::MlweMerge) {
    require_mlwe_response_keys(session.material->bv_galois_keys, params);
  }
  require(session.scheme == params.scheme,
          "session fingerprint differs from the plan");
  require(std::isfinite(min_noise_margin) && min_noise_margin >= 0,
          "minimum noise margin must be finite and nonnegative");

  CandidateNoiseResult result;
  result.tag = params.tag;
  result.client_id = session.client_id;
  result.fixture_kind = NoiseFixtureKind::TargetShapeNoiseGate;
  result.decrypts_correctly = decrypts_correctly;
  result.min_noise_margin = min_noise_margin;
  result.packing = packing;
  result.observed_key_coverage =
      inspect_bv_key_coverage(session.material->bv_galois_keys,
                              params.scheme.identity,
                              params.shape.registered_key_height);
  result.observed_key_coverage_matches_tag =
      observed_coverage_matches_tag(result.observed_key_coverage, params);
  result.checkpoints = std::move(checkpoints);
  const bool require_nonnegative_margin = min_noise_margin > 0;
  result.minimum_margin = std::numeric_limits<double>::infinity();
  for (NoiseCheckpoint &checkpoint : result.checkpoints) {
    result.minimum_margin = std::min(
        result.minimum_margin,
        normalized_margin(checkpoint, require_nonnegative_margin));
  }
  if (result.checkpoints.empty()) result.minimum_margin = 0;
  result.eligibility =
      result.decrypts_correctly && result.observed_key_coverage_matches_tag &&
              noise_margin_policy_passes(result)
          ? NoiseEligibility::Passed
          : NoiseEligibility::Failed;
  return result;
}

CandidateNoiseResult run_target_shape_noise_gate(
    PirClient &client, PirServer &query_eval_server,
    const TaggedTreeSessionKeys &session, const TreeParams &params,
    const PirParams &runtime_scheme, std::span<const TargetNoiseCase> cases,
    double min_noise_margin, ResponsePacking packing) {
  require(!cases.empty(), "target noise gate needs at least one fixture case");
  require(client.get_client_id() == session.client_id,
          "noise fixture client and session belong to different clients");
  require_query_server_binding(query_eval_server, params, runtime_scheme);
  require_registered_session_binding(query_eval_server, params, runtime_scheme,
                                     session);
  if (packing == ResponsePacking::MlweMerge) {
    require_mlwe_response_keys(session.material->bv_galois_keys, params);
  }

  std::vector<NoiseCheckpoint> checkpoints;
  bool decrypts_correctly = true;
  for (const TargetNoiseCase &fixture : cases) {
    require(fixture.source != nullptr, "target noise fixture source is null");
    const PackedQuery packed =
        make_query(client, session, runtime_scheme, params, fixture.leaf);
    TreeServerTiming timing{};
    ExpandedQuery query = unpack_query(query_eval_server, runtime_scheme,
                                       params, session, packed, &timing);
    const ClientCoordinates coords = client_index(fixture.leaf, params);
    for (size_t i = 0; i < query.alpha.size(); ++i) {
      checkpoints.push_back(remaining_budget_checkpoint(
          fixture.label + ": expansion alpha " + std::to_string(i), client,
          query.alpha[i]));
    }
    for (size_t i = 0; i < query.sigma_selectors.size(); ++i) {
      checkpoints.push_back(selector_probe_checkpoint(
          fixture.label + ": k1_comp experimental selector_completion " +
              std::to_string(i),
          client, query_eval_server, query.sigma_selectors[i], runtime_scheme,
          coords.sigma_bits_le[i], decrypts_correctly));
    }

    const AlphaPyramid pyramid =
        build_ragged_alpha_pyramid(query.alpha, params, runtime_scheme);
    PathResponse response;
    response.tag = params.tag;
    response.client_id = query.client_id;
    response.packing = packing;
    const size_t width =
        checked_mul(runtime_scheme.K(), runtime_scheme.get_poly_degree(),
                    "response ciphertext width");
    response.chunks.resize(params.shape.response_ciphertexts);
    for (RlweCt &chunk : response.chunks) {
      chunk.c0.assign(width, 0);
      chunk.c1.assign(width, 0);
      chunk.ntt_form = false;
    }
    std::vector<RlweCt> merge_inputs;
    size_t merge_chunk = 0;
    auto flush_merge = [&] {
      if (merge_inputs.empty()) return;
      size_t keyswitches = 0;
      response.chunks[merge_chunk] =
          mlwe_merge_residues(std::move(merge_inputs),
                              params.shape.records_per_plaintext,
                              session.material->bv_galois_keys,
                              runtime_scheme, &keyswitches);
      merge_inputs.clear();
      checkpoints.push_back(remaining_budget_checkpoint(
          fixture.label + ": response_merge chunk " +
              std::to_string(merge_chunk) + " keyswitches " +
              std::to_string(keyswitches),
          client, response.chunks[merge_chunk]));
    };

    for (size_t level = 0; level <= params.shape.tree_height; ++level) {
      const LevelPlan &plan = params.level_plans[level];
      const std::vector<LevelPlaneDatabase> planes =
          pack_level_planes(params, fixture.source, level);
      for (size_t plane = 0; plane < params.shape.plane_count; ++plane) {
        const CiphertextDiagnostic first_dim_checkpoint =
            [&](size_t row, const RlweCt &ct) {
              checkpoints.push_back(remaining_budget_checkpoint(
                  fixture.label + ": first_dimension level " +
                      std::to_string(level) + " plane " +
                      std::to_string(plane) + " row " + std::to_string(row),
                  client, ct));
            };
        const CiphertextDiagnostic fold_checkpoint =
            [&](size_t round, const RlweCt &ct) {
              checkpoints.push_back(remaining_budget_checkpoint(
                  fixture.label + ": fold level " + std::to_string(level) +
                      " plane " + std::to_string(plane) + " round " +
                      std::to_string(round),
                  client, ct));
            };
        RlweCt ct = select_level_reference(
            planes[plane], plan, pyramid, std::span<GSWCt>(query.sigma_selectors),
            query_eval_server, params, runtime_scheme, &timing,
            &first_dim_checkpoint, &fold_checkpoint);
        for (size_t idx = 0; idx < plan.rotation_bit_count; ++idx) {
          const size_t v = plan.rotation_bit_begin + idx;
          ct = rot_select(ct, query.sigma_selectors[v], size_t{1} << idx,
                          query_eval_server, runtime_scheme);
          checkpoints.push_back(remaining_budget_checkpoint(
              fixture.label + ": rotation level " + std::to_string(level) +
                  " plane " + std::to_string(plane) + " bit " +
                  std::to_string(idx),
              client, ct));
        }
        if (packing == ResponsePacking::MlweMerge) {
          const PlanePlacement at = response_placement(level, plane, params);
          if (!merge_inputs.empty() && at.response_chunk != merge_chunk) {
            flush_merge();
          }
          merge_chunk = at.response_chunk;
          merge_inputs.push_back(std::move(ct));
          if (merge_inputs.size() == params.shape.records_per_plaintext) {
            flush_merge();
          }
          continue;
        }
        const CiphertextDiagnostic projection_checkpoint =
            [&](size_t round, const RlweCt &raw) {
              const size_t remaining =
                  plan.projection_depth > round + 1
                      ? plan.projection_depth - round - 1
                      : 0;
              const RlweCt normalized =
                  multiply_ciphertext_scalar(raw, size_t{1} << remaining,
                                             runtime_scheme);
              checkpoints.push_back(remaining_budget_checkpoint(
                  fixture.label +
                      ": projection normalized-prescale level " +
                      std::to_string(level) + " plane " +
                      std::to_string(plane) + " round " +
                      std::to_string(round),
                  client, normalized));
            };
        ct = project_keep_stride(std::move(ct), plan.projection_depth,
                                 session.material->bv_galois_keys,
                                 runtime_scheme, &projection_checkpoint);
        const PlanePlacement at = response_placement(level, plane, params);
        RlweCt &chunk = response.chunks[at.response_chunk];
        if (at.residue_offset == 0) {
          tree_ct_add_inplace(chunk, ct, runtime_scheme);
        } else {
          const RlweCt shifted = mul_x_pow(ct, at.residue_offset,
                                           runtime_scheme);
          tree_ct_add_inplace(chunk, shifted, runtime_scheme);
        }
        checkpoints.push_back(remaining_budget_checkpoint(
            fixture.label + ": path_pack level " + std::to_string(level) +
                " plane " + std::to_string(plane),
            client, chunk));
      }
    }
    flush_merge();
    for (size_t i = 0; i < response.chunks.size(); ++i) {
      const bool switched =
          query_eval_server.switch_response_to_small_q(response.chunks[i]);
      response.small_q = response.small_q || switched;
      checkpoints.push_back(
          switched ? small_q_budget_checkpoint(
                         fixture.label + ": modulus_switch chunk " +
                             std::to_string(i),
                         client, response.chunks[i])
                   : remaining_budget_checkpoint(
                         fixture.label + ": modulus_switch chunk " +
                             std::to_string(i),
                         client, response.chunks[i]));
    }

    const DecodedPath decoded = extract_path_measured(
        response, client, params, runtime_scheme, fixture.leaf);
    if (decoded.entries.size() != params.shape.path_levels) {
      decrypts_correctly = false;
      continue;
    }
    for (size_t level = 0; level <= params.shape.tree_height; ++level) {
      const size_t node = fixture.leaf >> (params.shape.tree_height - level);
      if (decoded.entries[level] != fixture.source(level, node)) {
        decrypts_correctly = false;
      }
    }
  }

  return build_target_noise_result(params, runtime_scheme, query_eval_server,
                                   session, decrypts_correctly,
                                   std::move(checkpoints), min_noise_margin,
                                   packing);
}

}  // namespace tree_superrecord
