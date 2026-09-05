#include "tree_superrecord_planner.h"

#include "rlwe.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <tuple>

namespace tree_superrecord {

namespace {

size_t log2_ring(const PirSchemeIdentity &identity) {
  return exact_log2(identity.ring_degree, "ring degree");
}

bool sorted_unique(const std::vector<size_t> &values) {
  for (size_t i = 1; i < values.size(); ++i) {
    if (values[i] <= values[i - 1]) return false;
  }
  return true;
}

size_t rlwe_bytes(const PirSchemeIdentity &identity) {
  return checked_mul(checked_mul(checked_mul(size_t{2}, identity.rns_moduli.size(),
                                             "RLWE limb bytes"),
                                 identity.ring_degree, "RLWE bytes"),
                     sizeof(uint64_t), "RLWE bytes");
}

size_t ct_mod_width(const PirSchemeIdentity &identity) {
  size_t width = 0;
  for (size_t bits : identity.rns_modulus_bits) width += bits;
  return width;
}

std::vector<uint64_t> first_dim_limb_moduli(const PirSchemeIdentity &identity) {
  if (identity.composite_first_dim) {
    return {identity.composite_q1, identity.composite_q2};
  }
  return identity.rns_moduli;
}

size_t first_dim_limb_bytes(const PirSchemeIdentity &identity) {
  return identity.composite_first_dim ? sizeof(uint32_t) : sizeof(uint64_t);
}

FirstDimShapeKey first_dim_shape_key(const LevelPlan &plan,
                                     const PirSchemeIdentity &identity,
                                     uint64_t limb_modulus,
                                     const ParetoContext &context) {
  FirstDimShapeKey key;
  key.matrix_rows = plan.matrix_rows;
  key.matrix_cols = plan.matrix_cols;
  key.cols_mod_simd_width = plan.matrix_cols % 16;
  key.limb_modulus = limb_modulus;
  if (identity.composite_first_dim) {
    key.dispatch =
        select_matmul32_dispatch(plan.matrix_cols, limb_modulus, context.avx512);
  }
  return key;
}

FirstDimShapeKey first_dim_shape_key_assuming_avx(
    const LevelPlan &plan, const PirSchemeIdentity &identity,
    uint64_t limb_modulus) {
  ParetoContext context;
  context.avx512 = {true, true};
  return first_dim_shape_key(plan, identity, limb_modulus, context);
}

std::vector<FirstDimShapeKey> first_dim_shape_keys(
    const Candidate &candidate, const ParetoContext &context) {
  std::vector<FirstDimShapeKey> keys;
  const std::vector<LevelPlan> plans = build_level_plans(candidate.shape);
  const std::vector<uint64_t> moduli =
      first_dim_limb_moduli(candidate.scheme.identity);
  keys.reserve(candidate.shape.plane_count * plans.size() * moduli.size());
  for (const LevelPlan &plan : plans) {
    for (size_t plane = 0; plane < candidate.shape.plane_count; ++plane) {
      (void)plane;
      for (uint64_t modulus : moduli) {
        keys.push_back(first_dim_shape_key(plan, candidate.scheme.identity,
                                           modulus, context));
      }
    }
  }
  return keys;
}

void fill_u32_mod(std::vector<uint32_t> &values, uint64_t modulus,
                  uint64_t seed) {
  std::mt19937_64 rng(seed);
  for (uint32_t &value : values) {
    value = static_cast<uint32_t>(rng() % modulus);
  }
}

double measure_scan_bandwidth(const FirstDimShapeKey &key,
                              size_t ring_degree,
                              const ParetoContext &context,
                              const CostCalibrationOptions &options) {
  const size_t rows = key.matrix_rows;
  const size_t cols = key.matrix_cols;
  require(rows > 0 && cols > 0 && ring_degree > 0,
          "scan calibration shape must be nonempty");
  const size_t a_per_level = checked_mul(rows, cols, "calibration A level");
  const size_t b_per_level =
      checked_mul(cols, size_t{2}, "calibration B level");
  const size_t out_per_level =
      checked_mul(rows, size_t{2}, "calibration output level");
  const size_t bytes_per_level =
      checked_add(checked_add(checked_mul(a_per_level, sizeof(uint32_t),
                                          "calibration A bytes"),
                              checked_mul(b_per_level, sizeof(uint32_t),
                                          "calibration B bytes"),
                              "calibration bytes"),
                  checked_mul(out_per_level, sizeof(uint64_t),
                              "calibration output bytes"),
                  "calibration bytes");
  const size_t tile_levels = std::max(
      size_t{1},
      std::min(ring_degree,
               options.max_working_set_bytes_per_call / bytes_per_level));
  std::vector<uint32_t> A(checked_mul(tile_levels, a_per_level,
                                      "calibration A"));
  std::vector<uint32_t> B(checked_mul(tile_levels, b_per_level,
                                      "calibration B"));
  std::vector<uint64_t> out(checked_mul(tile_levels, out_per_level,
                                        "calibration output"));
  fill_u32_mod(A, key.limb_modulus, options.seed ^ key.limb_modulus ^
                                      (rows << 8) ^ cols);
  fill_u32_mod(B, key.limb_modulus, options.seed ^ 0xBADC0FFEEULL ^
                                      key.limb_modulus ^ cols);
  const size_t trials = std::max(size_t{1}, options.trials_per_shape);
  const auto start = std::chrono::steady_clock::now();
  for (size_t trial = 0; trial < trials; ++trial) {
    level_mat_mat_32_dispatch(A.data(), B.data(), out.data(), rows, cols,
                              tile_levels, key.limb_modulus, context.avx512);
  }
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start)
          .count();
  volatile uint64_t sink = std::accumulate(out.begin(), out.end(), uint64_t{0});
  (void)sink;
  const double bytes =
      static_cast<double>(rows) * static_cast<double>(cols) *
      static_cast<double>(tile_levels) *
      static_cast<double>(sizeof(uint32_t)) *
      static_cast<double>(trials);
  if (!(std::isfinite(elapsed_ms) && elapsed_ms > 0)) return 0;
  return bytes / elapsed_ms;
}

// Structural metrics of one accepted shape (design sections 11, 12.2, 19.2,
// 20). Every count is exact; every byte figure is a coefficient/object
// estimate rather than process memory.
CandidateMetrics compute_metrics(const CandidateShape &shape,
                                 std::span<const LevelPlan> plans,
                                 const PirSchemeIdentity &identity) {
  CandidateMetrics m;
  const size_t n = shape.ring_degree;
  const size_t K = identity.rns_moduli.size();
  const size_t P = shape.plane_count;
  const bool composite = identity.composite_first_dim;
  // Bytes per stored NTT coefficient: two u32 limbs on the composite path,
  // K u64 limbs on the plain NTT view.
  const size_t bytes_per_coeff = composite ? 2 * sizeof(uint32_t)
                                           : K * sizeof(uint64_t);
  size_t max_rows = 0;
  for (const LevelPlan &plan : plans) {
    m.logical_plaintexts = checked_add(
        m.logical_plaintexts,
        checked_mul(P, plan.logical_plaintexts_per_plane, "logical plaintexts"),
        "logical plaintexts");
    const size_t level_cells =
        checked_mul(P, plan.padded_plaintexts_per_plane, "matrix cells");
    m.physical_matrix_cells =
        checked_add(m.physical_matrix_cells, level_cells, "matrix cells");
    m.first_dim_output_ciphertexts += P * plan.matrix_rows;
    m.fold_cmux_count += P * (plan.matrix_rows - 1);
    m.rotate_cmux_count += P * plan.rotation_bit_count;
    m.projection_keyswitch_count += P * plan.projection_depth;
    for (uint64_t modulus : first_dim_limb_moduli(identity)) {
      const FirstDimShapeKey key =
          first_dim_shape_key_assuming_avx(plan, identity, modulus);
      const size_t bytes = checked_mul(
          checked_mul(P, plan.padded_plaintexts_per_plane, "scan cells"),
          checked_mul(n, first_dim_limb_bytes(identity), "scan coefficient"),
          "scan bytes");
      if (key.dispatch == MatMul32Dispatch::Avx512Lazy) {
        m.avx512_eligible_level_plane_limbs += P;
        m.avx512_eligible_scan_bytes =
            checked_add(m.avx512_eligible_scan_bytes, bytes, "avx scan bytes");
      } else {
        m.scalar_u128_required_level_plane_limbs += P;
        m.scalar_u128_required_scan_bytes = checked_add(
            m.scalar_u128_required_scan_bytes, bytes, "scalar scan bytes");
      }
    }
    max_rows = std::max(max_rows, plan.matrix_rows);
  }
  require(m.first_dim_output_ciphertexts ==
              m.fold_cmux_count + P * shape.path_levels,
          "first-dimension output count must equal folds plus P*H");
  const size_t cell_bytes =
      checked_mul(n, bytes_per_coeff, "plaintext view bytes");
  m.optimized_bytes = checked_mul(m.physical_matrix_cells, cell_bytes,
                                  "optimized database bytes");
  m.physical_scan_bytes = m.optimized_bytes;
  m.canonical_bytes = checked_mul(
      m.physical_matrix_cells, checked_mul(n, sizeof(uint64_t), "canonical"),
      "canonical database bytes");
  m.coefficient_payload_bytes = m.optimized_bytes;

  const std::vector<size_t> rows = ragged_pyramid_row_lengths(shape.N0);
  for (size_t len : rows) m.alpha_pyramid_ciphertexts += len;
  m.alpha_pyramid_upper_outputs = m.alpha_pyramid_ciphertexts - shape.N0;
  m.alpha_pyramid_additions = shape.N0 - 1;
  m.alpha_pyramid_carries =
      m.alpha_pyramid_upper_outputs - m.alpha_pyramid_additions;
  m.alpha_ntt_ciphertexts = m.alpha_pyramid_ciphertexts;
  m.alpha_split_logical_coefficients = m.alpha_pyramid_ciphertexts * 2 * n;
  m.alpha_split_u32_outputs =
      composite ? m.alpha_split_logical_coefficients * 2 : 0;

  m.populated_expansion_nodes = populated_expansion_node_count(
      shape.query_width, shape.expansion_capacity);
  m.expansion_heap_object_slots = 2 * shape.expansion_capacity;
  m.expansion_return_object_slots = shape.query_width;

  m.worst_case_retrieved_bundle_bits_per_level =
      shape.bundle_arity * shape.entry_bits;
  m.worst_case_extra_overfetch_bits_per_level =
      (shape.bundle_arity - 1) * shape.entry_bits;
  m.worst_case_public_overfetch_factor =
      static_cast<double>(shape.bundle_arity);
  m.application_payload_bits = shape.path_levels * shape.entry_bits;
  m.worst_case_retrieved_bundle_bits =
      shape.path_levels * m.worst_case_retrieved_bundle_bits_per_level;
  m.worst_case_extra_overfetch_bits =
      shape.path_levels * m.worst_case_extra_overfetch_bits_per_level;
  // plane_capacity_bits, padding_bits_per_bundle and the slot ratios need c,
  // which only the enumerator knows; it fills them after this call.

  // Query-evaluation PirServer constructor buffers (design section 20.4).
  const size_t query_server_db_elements = checked_mul(
      checked_mul(shape.N0, n, "query server elements"), K, "query server");
  if (composite) {
    m.query_eval_server_database_bytes_estimate = checked_mul(
        2 * query_server_db_elements, sizeof(uint32_t), "query server db");
    m.query_eval_server_intermediate_bytes_estimate =
        2 * (2 * n * K) * sizeof(uint64_t);
  } else {
    m.query_eval_server_database_bytes_estimate =
        checked_mul(query_server_db_elements, sizeof(db_coeff_t),
                    "query server db");
    m.query_eval_server_intermediate_bytes_estimate =
        (2 * n * K) * sizeof(inter_coeff_t);
  }
  m.query_eval_server_internal_bytes_estimate = checked_add(
      m.query_eval_server_database_bytes_estimate,
      m.query_eval_server_intermediate_bytes_estimate, "query server bytes");

  const size_t full_q = rlwe_bytes(identity);
  const size_t rgsw_bytes = 2 * identity.selector_gadget_rows * full_q;
  const size_t expansion_payload =
      checked_mul(m.populated_expansion_nodes, full_q, "expansion payload");
  const size_t expansion_objects =
      (m.expansion_heap_object_slots + m.expansion_return_object_slots) *
      sizeof(RlweCt);
  const size_t pyramid_bytes = m.alpha_pyramid_ciphertexts * full_q;
  const size_t alpha_m32_bytes =
      composite ? m.alpha_pyramid_ciphertexts * 2 * n * 2 * sizeof(uint32_t)
                : 0;
  const size_t selector_bytes = shape.selector_bits * rgsw_bytes;
  const size_t first_dim_candidate_bytes = P * max_rows * full_q;
  const size_t response_work_bytes = shape.response_ciphertexts * full_q;
  const size_t crypto_scratch =
      conservative_scratch_rlwe_count(identity.selector_gadget_rows) * full_q;
  size_t working = 0;
  for (size_t part : {expansion_payload, expansion_objects, pyramid_bytes,
                      alpha_m32_bytes, selector_bytes,
                      first_dim_candidate_bytes, response_work_bytes,
                      m.query_eval_server_internal_bytes_estimate,
                      crypto_scratch}) {
    working = checked_add(working, part, "temporary working set");
  }
  m.temporary_working_set_estimate = working;

  const size_t per_poly_bytes = ceil_div(n * ct_mod_width(identity), 8);
  const size_t seeded_bfv_bytes = 32 + per_poly_bytes;
  m.modeled_seeded_bv_key_bytes =
      shape.registered_key_height * identity.bv_ks_rows * (per_poly_bytes + 32);
  m.modeled_seeded_completion_key_bytes =
      2 * identity.completion_key_gadget_rows * seeded_bfv_bytes;
  m.modeled_per_client_eval_key_bytes =
      m.modeled_seeded_bv_key_bytes + m.modeled_seeded_completion_key_bytes;
  m.eval_key_coefficient_payload_bytes_estimate =
      (shape.registered_key_height * identity.bv_ks_rows +
       2 * identity.completion_key_gadget_rows) *
      full_q;

  m.validation_resident_bytes_estimate =
      checked_add(checked_add(m.canonical_bytes, m.optimized_bytes, "resident"),
                  checked_add(m.temporary_working_set_estimate,
                              m.eval_key_coefficient_payload_bytes_estimate,
                              "resident"),
                  "validation resident estimate");
  m.optimized_steady_state_bytes_estimate =
      checked_add(m.optimized_bytes,
                  checked_add(m.temporary_working_set_estimate,
                              m.eval_key_coefficient_payload_bytes_estimate,
                              "steady state"),
                  "steady-state estimate");
  m.matrix_padding_amplification =
      static_cast<double>(m.physical_matrix_cells) /
      static_cast<double>(m.logical_plaintexts);
  return m;
}

}  // namespace

SchemeFingerprint make_scheme_fingerprint(
    const PirSchemeIdentity &identity, size_t payload_bits_per_coeff,
    size_t registered_session_key_height,
    EvalKeyRegistrationPolicy registration) {
  validate_scheme_identity(identity);
  require(payload_bits_per_coeff >= 1 && payload_bits_per_coeff < 64,
          "payload bits per coefficient must be in [1, 63]");
  require((uint128{1} << payload_bits_per_coeff) <= identity.plaintext_modulus,
          "payload width exceeds the plaintext modulus");
  require(registered_session_key_height <= log2_ring(identity),
          "registered key height exceeds log2 of the ring degree");
  SchemeFingerprint fingerprint;
  fingerprint.identity = identity;
  fingerprint.payload_bits_per_coeff = payload_bits_per_coeff;
  fingerprint.registered_session_key_height = registered_session_key_height;
  fingerprint.eval_key_registration = registration;
  return fingerprint;
}

void validate_scheme_identity(const PirSchemeIdentity &identity) {
  require(identity.ring_degree >= 2 && std::has_single_bit(identity.ring_degree),
          "ring degree must be a power of two");
  require(identity.plaintext_modulus > 1, "plaintext modulus must exceed 1");
  require(!identity.rns_moduli.empty() &&
              identity.rns_moduli.size() == identity.rns_modulus_bits.size(),
          "RNS moduli and bit widths must agree");
  for (uint64_t q : identity.rns_moduli) {
    require(q > 1 && (q & 1ULL) == 1ULL, "every RNS modulus must be odd");
  }
  require(identity.selector_gadget_rows > 0 &&
              identity.completion_key_gadget_rows > 0 &&
              identity.bv_ks_rows > 0,
          "gadget row counts must be positive");
  if (identity.composite_first_dim) {
    require(identity.rns_moduli.size() == 1,
            "composite first dimension needs a single logical limb");
    require(identity.composite_q1 > 1 && identity.composite_q2 > 1 &&
                static_cast<uint128>(identity.composite_q1) *
                        identity.composite_q2 ==
                    identity.rns_moduli[0],
            "composite limbs must reconstruct the logical modulus");
  }
}

void validate_planner_input(const PlannerInput &input) {
  validate_scheme_identity(input.scheme_identity);
  const size_t native_max = log2_ring(input.scheme_identity);
  require(input.entry_bits > 0, "entry_bits must be positive");
  require(input.leaf_count > 0 && std::has_single_bit(input.leaf_count),
          "leaf_count must be a nonzero power of two");
  require(input.payload_bits_per_coeff >= 1 &&
              input.payload_bits_per_coeff < 64,
          "payload bits per coefficient must be in [1, 63]");
  require((uint128{1} << input.payload_bits_per_coeff) <=
              input.scheme_identity.plaintext_modulus,
          "payload width exceeds the plaintext modulus");
  const PlannerPolicy &policy = input.policy;
  require(policy.max_query_ciphertexts == 1,
          "generic super-record v1 supports exactly one packed query");
  require(policy.max_response_ciphertexts > 0,
          "max_response_ciphertexts must be positive");
  require(policy.max_bundle_arity > 0, "max_bundle_arity must be positive");
  require(policy.public_database,
          "generic super-record v1 requires a public database");
  require(policy.require_same_ring_response,
          "generic super-record v1 requires same-ring responses");
  require(!policy.expansion_heights.empty() &&
              sorted_unique(policy.expansion_heights),
          "expansion_heights must be nonempty, sorted, and unique");
  for (size_t height : policy.expansion_heights) {
    require(height <= native_max,
            "expansion height exceeds log2 of the ring degree");
  }
  require(!policy.alpha_alignments.empty() &&
              sorted_unique(policy.alpha_alignments) &&
              policy.alpha_alignments.front() > 0,
          "alpha_alignments must be nonempty, sorted, unique, and nonzero");
  if (policy.eval_key_registration ==
      EvalKeyRegistrationPolicy::FixedMaximumHeight) {
    require(policy.fixed_session_key_height <= native_max,
            "fixed session key height exceeds log2 of the ring degree");
  }
}

void require_selector_range(size_t begin, size_t count, size_t selector_bits,
                            const char *name) {
  if (count == 0) {
    require(begin == selector_bits, "empty selector range needs the sentinel");
    return;
  }
  require(checked_add(begin, count, "selector range") <= selector_bits, name);
}

size_t matrix_plaintext_index(size_t row, size_t col, const LevelPlan &plan) {
  require(row < plan.matrix_rows && col < plan.matrix_cols,
          "matrix coordinate out of range");
  return checked_add(checked_mul(col, plan.matrix_rows, "plaintext index"), row,
                     "plaintext index");
}

std::pair<size_t, size_t> matrix_coordinates(size_t plaintext_index,
                                             const LevelPlan &plan) {
  require(plaintext_index < plan.padded_plaintexts_per_plane,
          "plaintext index out of range");
  return {plaintext_index % plan.matrix_rows,
          plaintext_index / plan.matrix_rows};
}

std::vector<LevelPlan> build_level_plans(const CandidateShape &shape) {
  const size_t L = shape.tree_height;
  const size_t r = shape.record_index_bits;
  const size_t b = shape.beta_bits;
  const size_t m = shape.selector_bits;
  require(shape.bundle_arity > 0 && shape.records_per_plaintext > 0 &&
              shape.N0 > 0,
          "level plans need a positive arity, rho, and N0");
  std::vector<LevelPlan> plans;
  plans.reserve(L + 1);
  for (size_t level = 0; level <= L; ++level) {
    LevelPlan plan;
    plan.level = level;
    plan.shift = L - level;
    const size_t k = plan.shift;
    const size_t level_nodes = checked_pow2(level, "tree level width");
    const size_t S = ceil_div(level_nodes, shape.bundle_arity);
    const size_t Q = ceil_div(S, shape.records_per_plaintext);
    plan.logical_superrecords = S;
    plan.logical_plaintexts_per_plane = Q;
    if (k <= b) {
      plan.matrix_rows = checked_pow2(b - k, "level matrix rows");
      plan.matrix_cols = shape.N0;
      plan.select_case = SelectCase::AlphaIndex;
      plan.alpha_coarsen = 0;
      plan.fold_bit_count = b - k;
      plan.fold_bit_begin = plan.fold_bit_count == 0 ? m : r + k;
    } else {
      plan.alpha_coarsen = std::min(k - b, shape.alpha_height);
      plan.matrix_rows = 1;
      plan.matrix_cols = ceil_div(
          shape.N0, checked_pow2(plan.alpha_coarsen, "coarsened alpha width"));
      plan.select_case = plan.matrix_cols == 1 ? SelectCase::Single
                                               : SelectCase::CoarsenedAlpha;
      plan.fold_bit_begin = m;
      plan.fold_bit_count = 0;
    }
    plan.padded_plaintexts_per_plane = checked_mul(
        plan.matrix_rows, plan.matrix_cols, "level matrix coverage");
    require(plan.padded_plaintexts_per_plane >= Q,
            "level matrix does not cover its logical plaintexts");
    plan.rotation_bit_count = std::min(r, bit_width(S - 1));
    plan.rotation_bit_begin = plan.rotation_bit_count == 0 ? m : k;
    plan.projection_depth = plan.rotation_bit_count;
    require_selector_range(plan.fold_bit_begin, plan.fold_bit_count, m,
                           "fold selector range exceeds the sigma bits");
    require_selector_range(plan.rotation_bit_begin, plan.rotation_bit_count, m,
                           "rotation selector range exceeds the sigma bits");
    plans.push_back(plan);
  }
  return plans;
}

size_t max_projection_depth(std::span<const LevelPlan> plans) {
  size_t depth = 0;
  for (const LevelPlan &plan : plans) {
    depth = std::max(depth, plan.projection_depth);
  }
  return depth;
}

size_t populated_expansion_node_count(size_t useful_count, size_t capacity) {
  require(useful_count > 0, "expansion needs at least one useful leaf");
  require(std::has_single_bit(capacity), "expansion capacity must be a power of two");
  require(useful_count <= capacity, "useful leaves exceed the capacity");
  size_t internal = 0;
  for (size_t span = capacity; span > 1; span >>= 1) {
    internal = checked_add(internal, ceil_div(useful_count, span),
                           "expansion internal node count");
  }
  return checked_add(size_t{1}, checked_mul(size_t{2}, internal, "expansion nodes"),
                     "expansion nodes");
}

std::vector<size_t> ragged_pyramid_row_lengths(size_t N0) {
  require(N0 > 0, "pyramid needs a positive N0");
  std::vector<size_t> rows{N0};
  while (rows.back() > 1) rows.push_back(ceil_div(rows.back(), 2));
  return rows;
}

size_t ScratchRlweLiveness::selector_completion(size_t l_ep) {
  return checked_add(checked_mul(size_t{2}, l_ep, "scratch"), size_t{2},
                     "scratch");
}

size_t conservative_scratch_rlwe_count(size_t l_ep) {
  return std::max({ScratchRlweLiveness::expand_transient,
                   ScratchRlweLiveness::selector_completion(l_ep),
                   ScratchRlweLiveness::first_dim_finalize,
                   ScratchRlweLiveness::cmux, ScratchRlweLiveness::projection,
                   ScratchRlweLiveness::path_pack});
}

std::vector<Candidate> enumerate_candidates(const PlannerInput &input) {
  validate_planner_input(input);
  const PlannerPolicy &policy = input.policy;
  const PirSchemeIdentity &identity = input.scheme_identity;
  const size_t n = identity.ring_degree;
  const size_t native_max_height = log2_ring(identity);
  const size_t N = input.leaf_count;
  const size_t tree_height = bit_width(N) - 1;
  const size_t path_levels = checked_add(tree_height, size_t{1}, "path levels");
  const size_t E = input.entry_bits;
  const size_t c = input.payload_bits_per_coeff;
  const size_t l_ep = identity.selector_gadget_rows;

  std::vector<Candidate> out;
  for (size_t G = 1;;) {
    const size_t rho = n / G;
    const size_t r = exact_log2(rho, "records per plaintext");
    const size_t max_planes_by_response =
        checked_mul(policy.max_response_ciphertexts, rho,
                    "response plane capacity") /
        path_levels;
    if (max_planes_by_response > 0) {
      const size_t plane_bits = checked_mul(c, G, "plane payload bits");
      const uint128 response_payload_capacity = checked_mul_u128(
          checked_mul_u128(max_planes_by_response, c,
                           "response payload capacity"),
          G, "response payload capacity");
      const uint128 h_response_wide = response_payload_capacity / E;
      const size_t h_response_max =
          h_response_wide > std::numeric_limits<size_t>::max()
              ? std::numeric_limits<size_t>::max()
              : static_cast<size_t>(h_response_wide);
      size_t h_max = std::min({policy.max_bundle_arity, N, h_response_max});
      if (policy.max_retrieved_bundle_bits_per_level !=
          std::numeric_limits<size_t>::max()) {
        h_max = std::min(h_max, policy.max_retrieved_bundle_bits_per_level / E);
      }
      for (size_t h = 1; h <= h_max; ++h) {
        const size_t bundle_bits = checked_mul(h, E, "bundle payload bits");
        const size_t P = ceil_div(bundle_bits, plane_bits);
        const size_t response_items =
            checked_mul(P, path_levels, "response item count");
        const size_t response_ciphertexts = ceil_div(response_items, rho);
        if (response_ciphertexts > policy.max_response_ciphertexts) continue;

        const size_t leaf_superrecords = ceil_div(N, h);
        const size_t leaf_plaintexts = ceil_div(leaf_superrecords, rho);
        const size_t selector_bits = bit_width((N - 1) / h);
        const size_t beta_bits_max = bit_width(leaf_plaintexts - 1);
        for (size_t b = 0; b <= beta_bits_max; ++b) {
          if (!(r + b <= selector_bits || leaf_plaintexts == 1)) continue;
          const size_t rows_leaf = checked_pow2(b, "leaf matrix rows");
          const size_t N0_min = ceil_div(leaf_plaintexts, rows_leaf);
          for (size_t alignment : policy.alpha_alignments) {
            const size_t N0 = round_up(N0_min, alignment);
            require(N0 > 0, "aligned N0 must be positive");
            require(checked_mul(N0, rows_leaf, "leaf coverage") >=
                        leaf_plaintexts,
                    "first dimension does not cover the leaf plaintexts");
            const size_t query_width = checked_add(
                N0, checked_mul(l_ep, selector_bits, "selector query width"),
                "packed query width");
            const size_t min_expansion_capacity =
                checked_bit_ceil(query_width, "minimum expansion capacity");
            const size_t min_expansion_height =
                bit_width(min_expansion_capacity) - 1;
            for (size_t expansion_height : policy.expansion_heights) {
              if (expansion_height < min_expansion_height) continue;
              const size_t expansion_capacity =
                  checked_pow2(expansion_height, "expansion capacity");
              if (query_width > expansion_capacity ||
                  expansion_capacity > n) {
                continue;
              }
              CandidateShape shape;
              shape.entry_bits = E;
              shape.leaf_count = N;
              shape.tree_height = tree_height;
              shape.path_levels = path_levels;
              shape.ring_degree = n;
              shape.bundle_arity = h;
              shape.record_coeffs = G;
              shape.plane_count = P;
              shape.records_per_plaintext = rho;
              shape.record_index_bits = r;
              shape.leaf_superrecords = leaf_superrecords;
              shape.leaf_plaintexts = leaf_plaintexts;
              shape.selector_bits = selector_bits;
              shape.N0 = N0;
              shape.beta_bits = b;
              shape.beta_bits_max = beta_bits_max;
              shape.alpha_height = bit_width(N0 - 1);
              shape.alpha_alignment = alignment;
              shape.query_width = query_width;
              shape.min_expansion_height = min_expansion_height;
              shape.min_expansion_capacity = min_expansion_capacity;
              shape.expansion_height = expansion_height;
              shape.expansion_capacity = expansion_capacity;
              shape.query_ciphertexts = 1;
              shape.response_ciphertexts = response_ciphertexts;

              const std::vector<LevelPlan> plans = build_level_plans(shape);
              shape.projection_depth_max = max_projection_depth(plans);
              shape.required_key_height =
                  std::max(expansion_height, shape.projection_depth_max);
              if (shape.required_key_height > native_max_height) continue;
              switch (policy.eval_key_registration) {
                case EvalKeyRegistrationPolicy::FixedMaximumHeight:
                  shape.registered_key_height = policy.fixed_session_key_height;
                  break;
                case EvalKeyRegistrationPolicy::CandidateSpecificHeight:
                  shape.registered_key_height = shape.required_key_height;
                  break;
              }
              if (shape.registered_key_height < shape.required_key_height ||
                  shape.registered_key_height > native_max_height) {
                continue;
              }

              Candidate candidate;
              candidate.shape = shape;
              candidate.metrics = compute_metrics(shape, plans, identity);
              candidate.metrics.plane_capacity_bits =
                  checked_mul(P, plane_bits, "plane capacity bits");
              candidate.metrics.padding_bits_per_bundle =
                  candidate.metrics.plane_capacity_bits - bundle_bits;
              candidate.metrics.slot_utilization =
                  static_cast<double>(bundle_bits) /
                  static_cast<double>(candidate.metrics.plane_capacity_bits);
              candidate.metrics.slot_scan_amplification =
                  1.0 / candidate.metrics.slot_utilization;
              candidate.scheme = make_scheme_fingerprint(
                  identity, c, shape.registered_key_height,
                  policy.eval_key_registration);
              if (candidate.metrics.validation_resident_bytes_estimate >
                      policy.max_validation_resident_bytes ||
                  candidate.metrics.optimized_steady_state_bytes_estimate >
                      policy.max_steady_state_bytes) {
                continue;
              }
              out.push_back(std::move(candidate));
            }
          }
        }
      }
    }
    if (G == n) break;
    G = checked_mul(G, size_t{2}, "record coefficient width");
  }
  return out;
}

FirstDimDispatchSignature first_dim_dispatch_signature(
    const Candidate &candidate, const ParetoContext &context) {
  FirstDimDispatchSignature signature;
  const std::vector<FirstDimShapeKey> keys =
      first_dim_shape_keys(candidate, context);
  signature.limb_dispatch.reserve(keys.size());
  for (const FirstDimShapeKey &key : keys) {
    signature.limb_dispatch.push_back(key.dispatch);
  }
  return signature;
}

void apply_measured_control_timing(CostCalibration &calibration,
                                   const Candidate &candidate,
                                   const TreeServerTiming &timing) {
  const CandidateShape &s = candidate.shape;
  const CandidateMetrics &m = candidate.metrics;
  const auto per_unit = [](double ms, size_t units) {
    return units == 0 ? 0.0 : ms / static_cast<double>(units);
  };
  const double query_expand_ms = stage_ms(timing.query_expand);
  if (query_expand_ms > 0 && m.populated_expansion_nodes > 0) {
    calibration.expand_intercept_ms = 0;
    calibration.expand_per_populated_node_ms =
        per_unit(query_expand_ms, m.populated_expansion_nodes);
  }
  const double selector_ms = stage_ms(timing.selector_completion);
  if (selector_ms > 0 && s.selector_bits > 0) {
    calibration.to_rgsw_per_selector_ms = per_unit(selector_ms, s.selector_bits);
  }
  const double finalize_ms = stage_ms(timing.first_dim_finalize);
  if (finalize_ms > 0 && m.first_dim_output_ciphertexts > 0) {
    calibration.first_dim_output_ciphertext_ms =
        per_unit(finalize_ms, m.first_dim_output_ciphertexts);
  }
  const double fold_rotate_ms =
      stage_ms(timing.beta_fold) + stage_ms(timing.private_rotate);
  if (fold_rotate_ms > 0 && m.fold_cmux_count + m.rotate_cmux_count > 0) {
    calibration.cmux_ms =
        per_unit(fold_rotate_ms, m.fold_cmux_count + m.rotate_cmux_count);
  }
  const double projection_ms = stage_ms(timing.projection);
  if (projection_ms > 0 && m.projection_keyswitch_count > 0) {
    calibration.substitution_ms =
        per_unit(projection_ms, m.projection_keyswitch_count);
  }
  const double pack_ms = stage_ms(timing.path_pack);
  if (pack_ms > 0 && s.plane_count * s.path_levels > 0) {
    calibration.path_item_pack_ms =
        per_unit(pack_ms, s.plane_count * s.path_levels);
  }
  const double switch_ms = stage_ms(timing.modulus_switch);
  if (switch_ms > 0 && s.response_ciphertexts > 0) {
    calibration.modulus_switch_ms =
        per_unit(switch_ms, s.response_ciphertexts);
  }
}

CostCalibration calibrate_cost_model(
    std::span<const Candidate> candidates,
    const CostCalibration &base_calibration, const ParetoContext &context,
    const CostCalibrationOptions &options) {
  require(options.trials_per_shape > 0,
          "scan calibration needs at least one trial per shape");
  require(options.max_working_set_bytes_per_call > 0,
          "scan calibration needs a positive memory cap");
  CostCalibration calibration = base_calibration;
  for (const Candidate &candidate : candidates) {
    const std::vector<FirstDimShapeKey> keys =
        first_dim_shape_keys(candidate, context);
    for (const FirstDimShapeKey &key : keys) {
      if (calibration.scan_bandwidth_bytes_per_ms.contains(key)) continue;
      if (key.dispatch != MatMul32Dispatch::Avx512Lazy &&
          key.dispatch != MatMul32Dispatch::ScalarU128) {
        continue;
      }
      const double bandwidth = measure_scan_bandwidth(
          key, candidate.shape.ring_degree, context, options);
      if (std::isfinite(bandwidth) && bandwidth > 0) {
        calibration.scan_bandwidth_bytes_per_ms[key] = bandwidth;
      }
    }
  }
  return calibration;
}

double predict_server_ms(const Candidate &candidate,
                         const CostCalibration &cal,
                         const ParetoContext &context) {
  const CandidateShape &s = candidate.shape;
  const CandidateMetrics &m = candidate.metrics;
  double ms = cal.expand_intercept_ms;
  ms += cal.expand_per_populated_node_ms *
        static_cast<double>(m.populated_expansion_nodes);
  ms += cal.expand_per_level_ms * static_cast<double>(s.expansion_height);
  ms += cal.to_rgsw_per_selector_ms * static_cast<double>(s.selector_bits);
  ms += cal.alpha_pyramid_add_ms *
        static_cast<double>(m.alpha_pyramid_additions);
  ms += cal.alpha_carry_copy_ms * static_cast<double>(m.alpha_pyramid_carries);
  ms += cal.alpha_ntt_per_ciphertext_ms *
        static_cast<double>(m.alpha_ntt_ciphertexts);
  ms += cal.alpha_split_per_u32_output_ms *
        static_cast<double>(m.alpha_split_u32_outputs);
  const std::vector<LevelPlan> plans = build_level_plans(s);
  const std::vector<uint64_t> moduli =
      first_dim_limb_moduli(candidate.scheme.identity);
  for (const LevelPlan &plan : plans) {
    for (size_t plane = 0; plane < s.plane_count; ++plane) {
      (void)plane;
      for (uint64_t modulus : moduli) {
        const FirstDimShapeKey key =
            first_dim_shape_key(plan, candidate.scheme.identity, modulus,
                                context);
        double bandwidth = cal.default_scan_bandwidth_bytes_per_ms;
        const auto found = cal.scan_bandwidth_bytes_per_ms.find(key);
        if (found != cal.scan_bandwidth_bytes_per_ms.end()) {
          bandwidth = found->second;
        }
        if (!(std::isfinite(bandwidth) && bandwidth > 0)) {
          return std::numeric_limits<double>::infinity();
        }
        const size_t one_limb_bytes = checked_mul(
            checked_mul(plan.matrix_rows, plan.matrix_cols, "scan rows cols"),
            checked_mul(s.ring_degree, first_dim_limb_bytes(candidate.scheme.identity),
                        "scan limb bytes"),
            "scan bytes");
        ms += static_cast<double>(one_limb_bytes) / bandwidth;
      }
    }
  }
  ms += cal.first_dim_output_ciphertext_ms *
        static_cast<double>(m.first_dim_output_ciphertexts);
  ms += cal.cmux_ms *
        static_cast<double>(m.fold_cmux_count + m.rotate_cmux_count);
  ms += cal.substitution_ms *
        static_cast<double>(m.projection_keyswitch_count);
  ms += cal.path_item_pack_ms *
        static_cast<double>(s.plane_count * s.path_levels);
  ms += cal.modulus_switch_ms * static_cast<double>(s.response_ciphertexts);
  return ms;
}

void rank_by_prediction(std::vector<Candidate> &candidates,
                        const CostCalibration &calibration,
                        const ParetoContext &context) {
  for (Candidate &candidate : candidates) {
    candidate.metrics.predicted_server_ms =
        predict_server_ms(candidate, calibration, context);
    require(std::isfinite(candidate.metrics.predicted_server_ms),
            "predicted server time must be finite");
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate &a, const Candidate &b) {
                     return a.metrics.predicted_server_ms <
                            b.metrics.predicted_server_ms;
                   });
}

bool same_comparison_context(const Candidate &a, const Candidate &b,
                             const ParetoContext &context) {
  return a.shape.entry_bits == b.shape.entry_bits &&
         a.shape.leaf_count == b.shape.leaf_count &&
         a.scheme.identity == b.scheme.identity &&
         a.scheme.payload_bits_per_coeff == b.scheme.payload_bits_per_coeff &&
         a.scheme.eval_key_registration == b.scheme.eval_key_registration &&
         first_dim_dispatch_signature(a, context) ==
             first_dim_dispatch_signature(b, context);
}

bool dominates(const Candidate &a, const Candidate &b,
               const PlannerPolicy &policy, const ParetoContext &context) {
  (void)policy;
  if (!same_comparison_context(a, b, context)) return false;
  const CandidateMetrics &ma = a.metrics;
  const CandidateMetrics &mb = b.metrics;
  std::vector<std::pair<size_t, size_t>> dims = {
      {ma.physical_scan_bytes, mb.physical_scan_bytes},
      {ma.scalar_u128_required_scan_bytes, mb.scalar_u128_required_scan_bytes},
      {a.shape.query_width, b.shape.query_width},
      {a.shape.expansion_height, b.shape.expansion_height},
      {a.shape.selector_bits, b.shape.selector_bits},
      {a.shape.response_ciphertexts, b.shape.response_ciphertexts},
      {ma.first_dim_output_ciphertexts, mb.first_dim_output_ciphertexts},
      {ma.fold_cmux_count, mb.fold_cmux_count},
      {ma.rotate_cmux_count, mb.rotate_cmux_count},
      {ma.projection_keyswitch_count, mb.projection_keyswitch_count},
      {ma.alpha_pyramid_additions, mb.alpha_pyramid_additions},
      {ma.alpha_pyramid_carries, mb.alpha_pyramid_carries},
      {ma.alpha_ntt_ciphertexts, mb.alpha_ntt_ciphertexts},
      {ma.alpha_split_u32_outputs, mb.alpha_split_u32_outputs},
      {ma.populated_expansion_nodes, mb.populated_expansion_nodes},
      {ma.worst_case_retrieved_bundle_bits, mb.worst_case_retrieved_bundle_bits},
      {ma.optimized_steady_state_bytes_estimate,
       mb.optimized_steady_state_bytes_estimate},
  };
  if (a.scheme.eval_key_registration ==
      EvalKeyRegistrationPolicy::CandidateSpecificHeight) {
    dims.emplace_back(ma.modeled_per_client_eval_key_bytes,
                      mb.modeled_per_client_eval_key_bytes);
    dims.emplace_back(ma.eval_key_coefficient_payload_bytes_estimate,
                      mb.eval_key_coefficient_payload_bytes_estimate);
  }
  bool strictly_better = false;
  for (const auto &[va, vb] : dims) {
    if (va > vb) return false;
    if (va < vb) strictly_better = true;
  }
  return strictly_better;
}

std::vector<Candidate> pareto_frontier(std::vector<Candidate> candidates,
                                       const PlannerPolicy &policy,
                                       const ParetoContext &context) {
  std::vector<Candidate> frontier;
  for (size_t i = 0; i < candidates.size(); ++i) {
    bool dominated = false;
    for (size_t j = 0; j < candidates.size() && !dominated; ++j) {
      if (i != j &&
          dominates(candidates[j], candidates[i], policy, context)) {
        dominated = true;
      }
    }
    if (!dominated) frontier.push_back(candidates[i]);
  }
  return frontier;
}

namespace {

bool same_exact_plan(const Candidate &a, const Candidate &b,
                     const ParetoContext &context) {
  const CandidateShape &sa = a.shape;
  const CandidateShape &sb = b.shape;
  return std::tie(sa.entry_bits, sa.leaf_count, sa.bundle_arity,
                  sa.record_coeffs, sa.plane_count, sa.records_per_plaintext,
                  sa.N0, sa.beta_bits, sa.selector_bits, sa.expansion_height,
                  sa.expansion_capacity, sa.required_key_height,
                  sa.registered_key_height) ==
             std::tie(sb.entry_bits, sb.leaf_count, sb.bundle_arity,
                      sb.record_coeffs, sb.plane_count,
                      sb.records_per_plaintext, sb.N0, sb.beta_bits,
                      sb.selector_bits, sb.expansion_height,
                      sb.expansion_capacity, sb.required_key_height,
                      sb.registered_key_height) &&
         a.scheme == b.scheme &&
         build_level_plans(a.shape) == build_level_plans(b.shape) &&
         first_dim_dispatch_signature(a, context) ==
             first_dim_dispatch_signature(b, context);
}

void add_exact_unique(std::vector<Candidate> &out, const Candidate &candidate,
                      const ParetoContext &context) {
  for (const Candidate &existing : out) {
    if (same_exact_plan(existing, candidate, context)) return;
  }
  out.push_back(candidate);
}

template <typename Predicate, typename Less>
void add_best_control(std::vector<Candidate> &out,
                      const std::vector<Candidate> &candidates,
                      const ParetoContext &context, Predicate predicate,
                      Less less) {
  const Candidate *best = nullptr;
  for (const Candidate &candidate : candidates) {
    if (!predicate(candidate)) continue;
    if (best == nullptr || less(candidate, *best)) best = &candidate;
  }
  if (best != nullptr) add_exact_unique(out, *best, context);
}

bool predicted_less(const Candidate &a, const Candidate &b) {
  const bool fa = std::isfinite(a.metrics.predicted_server_ms);
  const bool fb = std::isfinite(b.metrics.predicted_server_ms);
  if (fa != fb) return fa;
  if (a.metrics.predicted_server_ms != b.metrics.predicted_server_ms) {
    return a.metrics.predicted_server_ms < b.metrics.predicted_server_ms;
  }
  return describe_candidate(a) < describe_candidate(b);
}

}  // namespace

std::vector<Candidate> deduplicate_exact_plans(
    std::vector<Candidate> candidates, const ParetoContext &context) {
  std::vector<Candidate> out;
  out.reserve(candidates.size());
  for (const Candidate &candidate : candidates) {
    add_exact_unique(out, candidate, context);
  }
  return out;
}

std::vector<Candidate> mandatory_controls(
    const std::vector<Candidate> &all, const PlannerInput &input,
    const CostCalibration &calibration, const ParetoContext &context) {
  std::vector<Candidate> ranked = all;
  rank_by_prediction(ranked, calibration, context);
  std::vector<Candidate> out;
  if (input.entry_bits == 256) {
    add_best_control(
        out, ranked, context,
        [](const Candidate &c) {
          return c.shape.bundle_arity == 3 && c.shape.record_coeffs == 64 &&
                 c.shape.plane_count == 1;
        },
        predicted_less);
  }
  add_best_control(
      out, ranked, context,
      [](const Candidate &c) { return c.shape.bundle_arity == 1; },
      predicted_less);
  add_best_control(
      out, ranked, context, [](const Candidate &) { return true; },
      [](const Candidate &a, const Candidate &b) {
        return a.metrics.optimized_steady_state_bytes_estimate <
               b.metrics.optimized_steady_state_bytes_estimate;
      });
  add_best_control(out, ranked, context, [](const Candidate &) { return true; },
                   [](const Candidate &a, const Candidate &b) {
                     return a.shape.response_ciphertexts <
                            b.shape.response_ciphertexts;
                   });
  return out;
}

std::vector<Candidate> top_nonmandatory(
    const std::vector<Candidate> &frontier,
    const std::vector<Candidate> &mandatory, size_t max_frontier,
    const ParetoContext &context) {
  std::vector<Candidate> out;
  if (max_frontier == 0) return out;
  for (const Candidate &candidate : frontier) {
    bool is_mandatory = false;
    for (const Candidate &control : mandatory) {
      if (same_exact_plan(candidate, control, context)) {
        is_mandatory = true;
        break;
      }
    }
    if (!is_mandatory) {
      add_exact_unique(out, candidate, context);
      if (out.size() >= max_frontier) break;
    }
  }
  return out;
}

std::string describe_candidate(const Candidate &candidate) {
  const CandidateShape &s = candidate.shape;
  const CandidateMetrics &m = candidate.metrics;
  std::ostringstream text;
  text << "E=" << s.entry_bits << " N=2^" << s.tree_height << " h="
       << s.bundle_arity << " G=" << s.record_coeffs << " P=" << s.plane_count
       << " rho=" << s.records_per_plaintext << " m=" << s.selector_bits
       << " b=" << s.beta_bits << " N0=" << s.N0 << "(align "
       << s.alpha_alignment << ") width=" << s.query_width << " h_q="
       << s.expansion_height << " keys req/reg=" << s.required_key_height
       << "/" << s.registered_key_height << " resp=" << s.response_ciphertexts
       << " scan=" << (m.physical_scan_bytes >> 20) << " MiB"
       << " folds=" << m.fold_cmux_count << " rot=" << m.rotate_cmux_count
       << " proj=" << m.projection_keyswitch_count
       << " util=" << m.slot_utilization;
  if (std::isfinite(m.predicted_server_ms)) {
    text << " pred=" << m.predicted_server_ms << " ms";
  }
  return text.str();
}

}  // namespace tree_superrecord
