#include "tree_superrecord_index.h"

#include <bit>

namespace tree_superrecord {

PlanTag make_plan_tag(const CandidateShape &shape,
                      const SchemeFingerprint &scheme) {
  PlanTag tag;
  tag.entry_bits = shape.entry_bits;
  tag.leaf_count = shape.leaf_count;
  tag.bundle_arity = shape.bundle_arity;
  tag.record_coeffs = shape.record_coeffs;
  tag.plane_count = shape.plane_count;
  tag.records_per_plaintext = shape.records_per_plaintext;
  tag.N0 = shape.N0;
  tag.beta_bits = shape.beta_bits;
  tag.selector_bits = shape.selector_bits;
  tag.expansion_height = shape.expansion_height;
  tag.expansion_capacity = shape.expansion_capacity;
  tag.required_key_height = shape.required_key_height;
  tag.registered_key_height = shape.registered_key_height;
  tag.scheme = scheme;
  return tag;
}

TreeParams freeze_candidate(const Candidate &candidate) {
  TreeParams params;
  params.shape = candidate.shape;
  params.scheme = candidate.scheme;
  params.level_plans = build_level_plans(candidate.shape);
  params.tag = make_plan_tag(candidate.shape, candidate.scheme);
  validate_tree_params(params);
  return params;
}

void validate_tree_params(const TreeParams &params) {
  const CandidateShape &shape = params.shape;
  require(shape.leaf_count > 0 && std::has_single_bit(shape.leaf_count),
          "leaf count must be a nonzero power of two");
  require(shape.tree_height == bit_width(shape.leaf_count) - 1 &&
              shape.path_levels == shape.tree_height + 1,
          "tree height does not match the leaf count");
  require(shape.records_per_plaintext > 0 &&
              std::has_single_bit(shape.records_per_plaintext) &&
              shape.record_coeffs > 0 &&
              shape.record_coeffs * shape.records_per_plaintext ==
                  shape.ring_degree,
          "G * rho must equal the ring degree with both powers of two");
  require(shape.record_index_bits ==
              bit_width(shape.records_per_plaintext) - 1,
          "record_index_bits does not match rho");
  require(shape.plane_count ==
              minimal_plane_count(shape.entry_bits,
                                  params.scheme.payload_bits_per_coeff,
                                  shape.bundle_arity, shape.record_coeffs),
          "plane count is not minimal");
  require(shape.selector_bits ==
              bit_width((shape.leaf_count - 1) / shape.bundle_arity),
          "selector bit count does not match N and h");
  require(shape.alpha_height == bit_width(shape.N0 - 1),
          "alpha height does not match N0");
  require(shape.expansion_capacity ==
                  checked_pow2(shape.expansion_height, "expansion capacity") &&
              shape.query_width <= shape.expansion_capacity &&
              shape.expansion_capacity <= shape.ring_degree,
          "expansion capacity does not fit the query");
  require(shape.required_key_height <= shape.registered_key_height &&
              shape.registered_key_height ==
                  params.scheme.registered_session_key_height,
          "registered key height does not match the fingerprint");
  require(params.level_plans == build_level_plans(shape),
          "level plans differ from the shape's recomputation");
  require(shape.projection_depth_max ==
              max_projection_depth(params.level_plans),
          "projection depth does not match the level plans");
  require(shape.required_key_height ==
              std::max(shape.expansion_height, shape.projection_depth_max),
          "required key height must cover expansion and projection");
  require(shape.response_ciphertexts ==
              ceil_div(checked_mul(shape.plane_count, shape.path_levels,
                                   "response items"),
                       shape.records_per_plaintext),
          "response ciphertext count is not ceil(P*H/rho)");
  require(params.tag == make_plan_tag(shape, params.scheme),
          "plan tag differs from the shape and fingerprint");
}

ClientCoordinates client_index(size_t leaf, const TreeParams &params) {
  const CandidateShape &shape = params.shape;
  require(leaf < shape.leaf_count, "leaf index is out of range");
  require(shape.records_per_plaintext > 0 &&
              std::has_single_bit(shape.records_per_plaintext),
          "rho must be a nonzero power of two");
  require(shape.record_index_bits ==
              bit_width(shape.records_per_plaintext) - 1,
          "record_index_bits does not match rho");
  const size_t sigma = leaf / shape.bundle_arity;
  const size_t q_bundle = sigma >> shape.record_index_bits;
  ClientCoordinates out;
  out.alpha = q_bundle >> shape.beta_bits;
  out.sigma_bits_le.assign(shape.selector_bits, 0);
  for (size_t v = 0; v < shape.selector_bits; ++v) {
    out.sigma_bits_le[v] = static_cast<uint8_t>((sigma >> v) & 1U);
  }
  require(out.alpha < shape.N0, "planned alpha exceeds N0");
  return out;
}

LevelOracle level_oracle_for_test(size_t leaf, size_t level,
                                  const TreeParams &params) {
  const CandidateShape &shape = params.shape;
  require(leaf < shape.leaf_count, "leaf index is out of range");
  require(level <= shape.tree_height, "level is out of range");
  const size_t k = shape.tree_height - level;
  LevelOracle oracle;
  oracle.node_index = leaf >> k;
  oracle.superrecord_index = oracle.node_index / shape.bundle_arity;
  oracle.lane_index = oracle.node_index % shape.bundle_arity;
  oracle.plaintext_index =
      oracle.superrecord_index >> shape.record_index_bits;
  oracle.record_position =
      oracle.superrecord_index & (shape.records_per_plaintext - 1);
  return oracle;
}

PlanePlacement response_placement(size_t level, size_t plane,
                                  const TreeParams &params) {
  const CandidateShape &shape = params.shape;
  require(level <= shape.tree_height, "level is out of range");
  require(plane < shape.plane_count, "plane is out of range");
  const size_t item = checked_add(
      checked_mul(level, shape.plane_count, "response item"), plane,
      "response item");
  PlanePlacement placement;
  placement.response_chunk = item / shape.records_per_plaintext;
  placement.residue_offset = item % shape.records_per_plaintext;
  require(placement.response_chunk < shape.response_ciphertexts,
          "response placement exceeds the chunk count");
  return placement;
}

void require_scheme_binding(const TreeParams &params,
                            const PirParams &runtime_scheme) {
  require(params.scheme.identity == runtime_scheme.scheme_identity(),
          "runtime scheme differs from the frozen scheme fingerprint");
  require((uint64_t{1} << params.scheme.payload_bits_per_coeff) <=
              runtime_scheme.get_plain_mod(),
          "payload width exceeds the runtime plaintext modulus");
  require(params.shape.ring_degree == runtime_scheme.get_poly_degree(),
          "ring degree differs from the runtime scheme");
}

CodecSpec codec_spec_for(const TreeParams &params) {
  CodecSpec spec;
  spec.entry_bits = params.shape.entry_bits;
  spec.payload_bits_per_coeff = params.scheme.payload_bits_per_coeff;
  spec.bundle_arity = params.shape.bundle_arity;
  spec.record_coeffs = params.shape.record_coeffs;
  spec.plane_count = params.shape.plane_count;
  spec.plaintext_modulus = params.scheme.identity.plaintext_modulus;
  validate_codec_spec(spec);
  return spec;
}

size_t real_bundle_slots(size_t leaf, size_t level, const TreeParams &params) {
  const CandidateShape &shape = params.shape;
  require(leaf < shape.leaf_count && level <= shape.tree_height,
          "leaf or level out of range");
  const size_t node = leaf >> (shape.tree_height - level);
  const size_t level_nodes = size_t{1} << level;
  const size_t bundle_start = (node / shape.bundle_arity) * shape.bundle_arity;
  return std::min(shape.bundle_arity, level_nodes - bundle_start);
}

}  // namespace tree_superrecord
