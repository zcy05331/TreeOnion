#pragma once

#include "pir.h"
#include "matrix.h"
#include "tree_superrecord_common.h"
#include "tree_profile.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <vector>

// Closed planner for entry-size-aware super-record tree PIR (design
// specification, sections 4, 9, 11, 19-22). Given an entry size E, a leaf
// count N = 2^L, the safe payload width c, a policy, and a scheme identity, it
// enumerates every feasible joint choice of
//   (h, G, P, N0, b, expansion_height)
// with h arbitrary, G dyadic, P = ceil(hE/(cG)) minimal, N0 the minimal
// aligned first-dimension width for each binary tail b, and the exact
// expansion depth used by QueryPack/ExpandBFV. It also owns the single public
// LevelPlan builder consumed by preprocessing, selection, rotation, and the
// response placement, and the structural metrics used for Pareto pruning and
// cost prediction. Nothing here touches ciphertexts.

namespace tree_superrecord {

enum class EvalKeyRegistrationPolicy {
  FixedMaximumHeight,       // every candidate registers one fixed BV family
  CandidateSpecificHeight,  // each candidate registers exactly what it needs
};

struct PlannerPolicy {
  size_t max_query_ciphertexts = 1;     // version 1: exactly one
  size_t max_response_ciphertexts = 1;
  size_t max_bundle_arity = 64;
  size_t max_retrieved_bundle_bits_per_level =
      std::numeric_limits<size_t>::max();

  size_t max_validation_resident_bytes = std::numeric_limits<size_t>::max();
  size_t max_steady_state_bytes = std::numeric_limits<size_t>::max();
  // Remaining-budget bits the noise gate must observe (larger is safer).
  size_t min_noise_margin = 0;

  std::vector<size_t> expansion_heights;  // sorted, unique, <= log2 n
  std::vector<size_t> alpha_alignments;   // sorted, unique, nonzero

  EvalKeyRegistrationPolicy eval_key_registration =
      EvalKeyRegistrationPolicy::FixedMaximumHeight;
  size_t fixed_session_key_height = 0;

  bool public_database = true;
  bool require_same_ring_response = true;
};

// Frozen scheme provenance carried by every plan, database, query, and
// response object; equality is exact.
struct SchemeFingerprint {
  uint32_t fingerprint_schema_version = 1;
  PirSchemeIdentity identity;
  size_t payload_bits_per_coeff = 0;
  size_t registered_session_key_height = 0;
  EvalKeyRegistrationPolicy eval_key_registration =
      EvalKeyRegistrationPolicy::FixedMaximumHeight;

  bool operator==(const SchemeFingerprint &) const = default;
};

// Identity-first constructor: the planner owns an identity, never a PirParams.
SchemeFingerprint make_scheme_fingerprint(
    const PirSchemeIdentity &identity, size_t payload_bits_per_coeff,
    size_t registered_session_key_height,
    EvalKeyRegistrationPolicy registration);
inline SchemeFingerprint make_scheme_fingerprint(
    const PirParams &scheme, size_t payload_bits_per_coeff,
    size_t registered_session_key_height,
    EvalKeyRegistrationPolicy registration) {
  return make_scheme_fingerprint(scheme.scheme_identity(),
                                 payload_bits_per_coeff,
                                 registered_session_key_height, registration);
}

struct PlannerInput {
  size_t entry_bits = 0;              // E
  size_t leaf_count = 0;              // N = 2^L
  size_t payload_bits_per_coeff = 0;  // c, with 2^c <= t
  PlannerPolicy policy;
  PirSchemeIdentity scheme_identity;
};

void validate_scheme_identity(const PirSchemeIdentity &identity);
void validate_planner_input(const PlannerInput &input);

struct CandidateShape {
  size_t entry_bits = 0;
  size_t leaf_count = 0;
  size_t tree_height = 0;  // L
  size_t path_levels = 0;  // H = L + 1
  size_t ring_degree = 0;  // n

  size_t bundle_arity = 0;            // h
  size_t record_coeffs = 0;           // G
  size_t plane_count = 0;             // P
  size_t records_per_plaintext = 0;   // rho = n / G
  size_t record_index_bits = 0;       // r = log2(rho)
  size_t projection_depth_max = 0;    // max over levels of min(r, bitwidth(S-1))

  size_t leaf_superrecords = 0;       // ceil(N / h)
  size_t leaf_plaintexts = 0;         // ceil(leaf_superrecords / rho)
  size_t selector_bits = 0;           // m = bitwidth(floor((N-1)/h))
  size_t N0 = 0;
  size_t beta_bits = 0;               // b
  size_t beta_bits_max = 0;           // bitwidth(leaf_plaintexts - 1)
  size_t alpha_height = 0;            // bitwidth(N0 - 1): ragged pyramid rows above row 0
  size_t alpha_alignment = 0;

  size_t query_width = 0;             // N0 + L_EP * m
  size_t min_expansion_height = 0;
  size_t min_expansion_capacity = 0;
  size_t expansion_height = 0;        // selected: BitRev width and W = 2^height
  size_t expansion_capacity = 0;

  size_t required_key_height = 0;     // max(expansion_height, projection_depth_max)
  size_t registered_key_height = 0;

  size_t query_ciphertexts = 1;
  size_t response_ciphertexts = 0;    // ceil(P * H / rho)
};

enum class SelectCase {
  Single,          // one plaintext: multiply by the pyramid's single Enc(1)
  CoarsenedAlpha,  // ragged pyramid row alpha_coarsen selects floor(alpha / 2^c)
  AlphaIndex,      // full N0 one-hot times a rows x N0 matrix, then fold
};

struct LevelPlan {
  size_t level = 0;
  size_t shift = 0;  // k = L - level

  size_t logical_superrecords = 0;         // S = ceil(2^level / h)
  size_t logical_plaintexts_per_plane = 0; // Q = ceil(S / rho)

  size_t matrix_rows = 0;
  size_t matrix_cols = 0;
  size_t padded_plaintexts_per_plane = 0;  // rows * cols >= Q

  SelectCase select_case = SelectCase::Single;
  size_t alpha_coarsen = 0;

  // Fold bits [r + k, r + b) consumed MSB-first; empty range uses the
  // sentinel begin = selector_bits, count = 0.
  size_t fold_bit_begin = 0;
  size_t fold_bit_count = 0;
  // Rotation bits [k, k + depth) with monomial magnitudes 1, 2, ..., 2^(d-1).
  size_t rotation_bit_begin = 0;
  size_t rotation_bit_count = 0;
  size_t projection_depth = 0;  // min(r, bitwidth(S - 1)) == rotation_bit_count

  bool operator==(const LevelPlan &) const = default;
};

// The only LevelPlan builder in the subsystem.
std::vector<LevelPlan> build_level_plans(const CandidateShape &shape);
size_t max_projection_depth(std::span<const LevelPlan> plans);
void require_selector_range(size_t begin, size_t count, size_t selector_bits,
                            const char *name);
// Linearisation shared by preprocessing, selection, update, and tests:
// plaintext_index = col * rows + row.
size_t matrix_plaintext_index(size_t row, size_t col, const LevelPlan &plan);
std::pair<size_t, size_t> matrix_coordinates(size_t plaintext_index,
                                             const LevelPlan &plan);

struct CandidateMetrics {
  size_t logical_plaintexts = 0;   // sum over levels and planes of Q
  size_t physical_matrix_cells = 0;
  size_t physical_scan_bytes = 0;  // both composite limbs (or K u64 limbs)
  size_t scalar_u128_required_scan_bytes = 0;
  size_t avx512_eligible_level_plane_limbs = 0;
  size_t scalar_u128_required_level_plane_limbs = 0;
  size_t avx512_eligible_scan_bytes = 0;

  size_t first_dim_output_ciphertexts = 0;
  size_t fold_cmux_count = 0;
  size_t rotate_cmux_count = 0;
  size_t projection_keyswitch_count = 0;

  size_t alpha_pyramid_ciphertexts = 0;
  size_t alpha_pyramid_upper_outputs = 0;
  size_t alpha_pyramid_additions = 0;
  size_t alpha_pyramid_carries = 0;
  size_t alpha_ntt_ciphertexts = 0;
  size_t alpha_split_logical_coefficients = 0;
  size_t alpha_split_u32_outputs = 0;

  size_t populated_expansion_nodes = 0;
  size_t expansion_heap_object_slots = 0;
  size_t expansion_return_object_slots = 0;

  size_t worst_case_retrieved_bundle_bits_per_level = 0;
  size_t worst_case_extra_overfetch_bits_per_level = 0;
  double worst_case_public_overfetch_factor = 0;

  size_t application_payload_bits = 0;
  size_t worst_case_retrieved_bundle_bits = 0;
  size_t worst_case_extra_overfetch_bits = 0;
  size_t plane_capacity_bits = 0;
  size_t padding_bits_per_bundle = 0;

  size_t canonical_bytes = 0;
  size_t optimized_bytes = 0;
  size_t coefficient_payload_bytes = 0;

  size_t query_eval_server_database_bytes_estimate = 0;
  size_t query_eval_server_intermediate_bytes_estimate = 0;
  size_t query_eval_server_internal_bytes_estimate = 0;

  size_t temporary_working_set_estimate = 0;
  size_t validation_resident_bytes_estimate = 0;
  size_t optimized_steady_state_bytes_estimate = 0;

  size_t modeled_seeded_bv_key_bytes = 0;
  size_t modeled_seeded_completion_key_bytes = 0;
  size_t modeled_per_client_eval_key_bytes = 0;
  size_t eval_key_coefficient_payload_bytes_estimate = 0;

  double slot_utilization = 0;
  double slot_scan_amplification = 0;
  double matrix_padding_amplification = 0;
  double predicted_server_ms = std::numeric_limits<double>::quiet_NaN();
};

struct Candidate {
  CandidateShape shape;
  CandidateMetrics metrics;
  SchemeFingerprint scheme;
};

struct ParetoContext {
  Avx512KernelAvailability avx512;
};

struct FirstDimDispatchSignature {
  std::vector<MatMul32Dispatch> limb_dispatch;

  bool operator==(const FirstDimDispatchSignature &) const = default;
};

struct FirstDimShapeKey {
  size_t matrix_rows = 0;
  size_t matrix_cols = 0;
  size_t cols_mod_simd_width = 0;
  MatMul32Dispatch dispatch = MatMul32Dispatch::ScalarU128;
  uint64_t limb_modulus = 0;

  auto operator<=>(const FirstDimShapeKey &) const = default;
};

FirstDimDispatchSignature first_dim_dispatch_signature(
    const Candidate &candidate, const ParetoContext &context);

// Every feasible candidate under the policy, in a deterministic order
// (G ascending, then h, b, alignment, expansion height).
std::vector<Candidate> enumerate_candidates(const PlannerInput &input);

// Mirrors fast_expand_qry's pruning: the RLWE nodes that receive a payload.
size_t populated_expansion_node_count(size_t useful_count, size_t capacity);
// Row lengths of the ragged carry-up pyramid: N0, ceil(N0/2), ..., 1.
std::vector<size_t> ragged_pyramid_row_lengths(size_t N0);

// Frozen scratch-liveness table (design section 20.4).
struct ScratchRlweLiveness {
  static constexpr size_t expand_transient = 1;
  static size_t selector_completion(size_t l_ep);  // 2 * l_ep + 2
  static constexpr size_t first_dim_finalize = 4;
  static constexpr size_t cmux = 4;
  static constexpr size_t projection = 3;
  static constexpr size_t path_pack = 2;
};
size_t conservative_scratch_rlwe_count(size_t l_ep);

// Per-operation calibration for ranking candidates before measurement. The
// defaults come from the local g=32 control run (Apple M4 under Rosetta,
// scalar u128 kernel); a run on other hardware should replace them.
struct CostCalibration {
  double expand_intercept_ms = 0;
  double expand_per_populated_node_ms = 0.10;
  double expand_per_level_ms = 0;
  double to_rgsw_per_selector_ms = 4.6;
  double alpha_pyramid_add_ms = 0.01;
  double alpha_carry_copy_ms = 0;
  double alpha_ntt_per_ciphertext_ms = 0.058;
  double alpha_split_per_u32_output_ms = 0;
  std::map<FirstDimShapeKey, double> scan_bandwidth_bytes_per_ms;
  double default_scan_bandwidth_bytes_per_ms = 3.8e6;
  double first_dim_output_ciphertext_ms = 0.05;    // CRT compose + INTT
  double cmux_ms = 0.19;
  double substitution_ms = 0.18;
  double path_item_pack_ms = 0.02;
  double modulus_switch_ms = 0.4;
};

struct CostCalibrationOptions {
  size_t trials_per_shape = 2;
  size_t max_working_set_bytes_per_call = 64 * 1024 * 1024;
  uint64_t seed = 0x7375706572526563ULL;
};

void apply_measured_control_timing(CostCalibration &calibration,
                                   const Candidate &candidate,
                                   const TreeServerTiming &timing);
CostCalibration calibrate_cost_model(
    std::span<const Candidate> candidates,
    const CostCalibration &base_calibration, const ParetoContext &context,
    const CostCalibrationOptions &options = {});

double predict_server_ms(const Candidate &candidate,
                         const CostCalibration &calibration,
                         const ParetoContext &context);
void rank_by_prediction(std::vector<Candidate> &candidates,
                        const CostCalibration &calibration,
                        const ParetoContext &context);

// Structural dominance: same entry size, leaf count, identity, payload width,
// and key policy; a dominates b when every lower-is-better metric is <= and at
// least one is <. Predicted or measured times never enter dominance.
bool same_comparison_context(const Candidate &a, const Candidate &b,
                             const ParetoContext &context);
bool dominates(const Candidate &a, const Candidate &b,
               const PlannerPolicy &policy, const ParetoContext &context);
std::vector<Candidate> pareto_frontier(std::vector<Candidate> candidates,
                                       const PlannerPolicy &policy,
                                       const ParetoContext &context);

std::vector<Candidate> deduplicate_exact_plans(
    std::vector<Candidate> candidates, const ParetoContext &context);
std::vector<Candidate> mandatory_controls(
    const std::vector<Candidate> &all, const PlannerInput &input,
    const CostCalibration &calibration, const ParetoContext &context);
std::vector<Candidate> top_nonmandatory(
    const std::vector<Candidate> &frontier,
    const std::vector<Candidate> &mandatory, size_t max_frontier,
    const ParetoContext &context);

std::string describe_candidate(const Candidate &candidate);

}  // namespace tree_superrecord
