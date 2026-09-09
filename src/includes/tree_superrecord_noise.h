#pragma once

#include "tree_response_merge.h"
#include "tree_superrecord_query.h"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tree_superrecord {

enum class NoiseMeasureKind {
  RemainingBudgetBits,
  Log2AbsoluteNoise,
};

struct NoiseCheckpoint {
  std::string stage;
  NoiseMeasureKind kind = NoiseMeasureKind::RemainingBudgetBits;
  double value = 0;
  double decoder_threshold = 0;
  double margin = 0;
};

enum class NoiseEligibility {
  Untested,
  Passed,
  Failed,
};

enum class NoiseFixtureKind {
  SmallShapeSmoke,
  TargetShapeNoiseGate,
  FormalScaleConfirmation,
};

struct CandidateNoiseResult {
  PlanTag tag;
  size_t client_id = 0;
  NoiseFixtureKind fixture_kind = NoiseFixtureKind::SmallShapeSmoke;
  NoiseEligibility eligibility = NoiseEligibility::Untested;

  bool decrypts_correctly = false;
  bool observed_key_coverage_matches_tag = false;

  BvKeyCoverage observed_key_coverage;
  double min_noise_margin = 0;
  double minimum_margin = 0;
  ResponsePacking packing = ResponsePacking::Projection;

  std::vector<NoiseCheckpoint> checkpoints;
};

using NoiseEntrySource = std::function<EntryBits(size_t level,
                                                 size_t node_index)>;

struct TargetNoiseCase {
  size_t leaf = 0;
  NoiseEntrySource source;
  std::string label;
};

bool noise_result_unlocks_candidate(const CandidateNoiseResult &result,
                                    const TreeParams &params,
                                    size_t expected_client_id,
                                    ResponsePacking packing =
                                        ResponsePacking::Projection);
void require_target_noise_gate(const CandidateNoiseResult &result,
                               const TreeParams &params,
                               size_t expected_client_id,
                               ResponsePacking packing =
                                   ResponsePacking::Projection);
void require_trusted_target_noise_result(const CandidateNoiseResult &result,
                                         const TreeParams &params,
                                         std::optional<ResponsePacking> packing =
                                             std::nullopt);

CandidateNoiseResult run_target_shape_noise_gate(
    PirClient &client, PirServer &query_eval_server,
    const TaggedTreeSessionKeys &session, const TreeParams &params,
    const PirParams &runtime_scheme, std::span<const TargetNoiseCase> cases,
    // Zero means decrypt-correctness admission: keep all diagnostics, but do
    // not reject solely because a checkpoint has no remaining numeric margin.
    // Positive values retain the strict minimum-margin gate.
    double min_noise_margin = 0,
    ResponsePacking packing = ResponsePacking::Projection);

}  // namespace tree_superrecord
