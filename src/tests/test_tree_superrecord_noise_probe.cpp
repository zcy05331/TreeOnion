#include "tests.h"

#include "tree_project.h"
#include "tree_rotate.h"
#include "tree_superrecord_noise.h"
#include "tree_select.h"
#include "tree_superrecord_query.h"
#include "tree_superrecord_response.h"
#include "tree_superrecord_select.h"
#include "tree_superrecord_test_util.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "hexl/hexl.hpp"

using namespace tree_superrecord;
using namespace tree_superrecord_test;

namespace {

struct ProbeShape {
  std::string label;
  size_t leaf_count;
  size_t entry_bits;
  size_t bundle_arity;
  size_t record_coeffs;
  size_t plane_count;
  size_t N0;
  size_t beta_bits;
  size_t expansion_height;
  size_t alpha_alignment;
  std::vector<size_t> leaves;
};

struct BudgetObservation {
  std::string stage;
  double budget_bits = 0.0;
  double margin = 0.0;
  uint64_t absolute_noise = 0;
  bool small_q = false;
};

struct QueryObservation {
  size_t query_index = 0;
  size_t leaf = 0;
  bool correct_path = false;
  bool correct_selected_bundle = false;
  bool decrypt_ok = false;
  bool margin_ok = false;
  uint64_t max_noise = 0;
  int min_budget_bits = 0;
  double min_margin = 0.0;
  int min_stage_budget_bits = 0;
  double min_stage_margin = 0.0;
  int min_final_budget_bits = 0;
  double min_final_margin = 0.0;
  bool expected_coefficients_match = false;
  uint64_t max_used_plaintext_residual = 0;
  uint64_t max_unused_plaintext_residual = 0;
  uint64_t max_used_phase_residual = 0;
  uint64_t max_unused_phase_residual = 0;
  TreeServerTiming timing{};
  std::vector<BudgetObservation> budgets;
  std::string error;
};

struct ProbeResult {
  ProbeShape shape;
  bool diagnostic_only = true;
  bool formal_benchmark_record = false;
  bool validate_gate_certificate = false;
  double fixed_threshold_bits = 1.0;
  size_t registered_key_height = 0;
  bool used_env_shape = false;
  bool all_correct = true;
  bool all_margins_ok = true;
  std::vector<QueryObservation> queries;
};

using PhaseResidualFn = std::function<std::vector<uint64_t>(const PirClient &, const PirParams &, const RlweCt &, const RlwePt &)>;

size_t bit_width_minus_one(size_t value, const char *label) {
  require_test(value > 0 && std::has_single_bit(value),
               std::string(label) + " must be a positive power of two");
  return std::bit_width(value) - 1;
}

size_t ceil_div_size(size_t a, size_t b) {
  return (a + b - 1) / b;
}

EntryBits benchmark_entry_for_probe(size_t entry_bits, size_t level,
                                    size_t node) {
  if (entry_bits == 256) {
    return entry_from_bytes(synthetic_tree_node_bytes(level, node),
                            entry_bits);
  }
  return synthetic_entry(entry_bits, level, node);
}

std::vector<ProbeShape> clemson_shapes() {
  const size_t N = size_t{1} << 27;
  const std::vector<size_t> leaves{15830119, 113195322, 40120694};
  return {
      {"H27_E256_h1_G8_P3_N0_512_b10_hq10", N, 256, 1, 8, 3, 512, 10, 10, 1, leaves},
      {"H27_E256_h2_G16_P3_N0_512_b10_hq10", N, 256, 2, 16, 3, 512, 10, 10, 1, leaves},
      {"H27_E256_h3_G64_P1_N0_352_b12_hq9", N, 256, 3, 64, 1, 352, 12, 9, 16, leaves},
      {"H27_E256_h3_G64_P1_N0_683_b11_hq10", N, 256, 3, 64, 1, 683, 11, 10, 1, leaves},
      {"H27_E256_h2_G8_P6_N0_512_b9_hq10", N, 256, 2, 8, 6, 512, 9, 10, 1, leaves},
      {"H27_E256_h3_G32_P2_N0_352_b11_hq9", N, 256, 3, 32, 2, 352, 11, 9, 16, leaves},
  };
}

ProbeShape smoke_shape(const PirParams &scheme) {
  PlannerPolicy policy = test_policy(/*max_responses=*/1, /*max_arity=*/8);
  policy.expansion_heights = {5, 6, 7, 8, 9, 10, 11};
  policy.fixed_session_key_height =
      bit_width_minus_one(scheme.get_poly_degree(), "ring degree");
  const size_t leaf_count = size_t{1} << 8;
  const std::vector<Candidate> candidates = enumerate_candidates(
      make_input(scheme, 256, leaf_count, std::move(policy)));
  for (const Candidate &candidate : candidates) {
    if (candidate.shape.bundle_arity == 2 &&
        candidate.shape.record_coeffs == 8 &&
        candidate.shape.plane_count > 0) {
      return {"small_H_smoke", leaf_count, 256,
              candidate.shape.bundle_arity,
              candidate.shape.record_coeffs,
              candidate.shape.plane_count,
              candidate.shape.N0,
              candidate.shape.beta_bits,
              candidate.shape.expansion_height,
              candidate.shape.alpha_alignment,
              {0, 17, leaf_count - 1}};
    }
  }
  require_test(!candidates.empty(), "noise probe smoke needs a feasible candidate");
  const Candidate &candidate = candidates.front();
  return {"small_H_smoke", leaf_count, 256,
          candidate.shape.bundle_arity,
          candidate.shape.record_coeffs,
          candidate.shape.plane_count,
          candidate.shape.N0,
          candidate.shape.beta_bits,
          candidate.shape.expansion_height,
          candidate.shape.alpha_alignment,
          {0, 17, leaf_count - 1}};
}

std::optional<size_t> env_probe_index() {
  const char *raw = std::getenv("SUPERRECORD_NOISE_PROBE_INDEX");
  if (!raw || std::string(raw).empty()) return std::nullopt;
  const std::string text(raw);
  size_t pos = 0;
  const unsigned long value = std::stoul(text, &pos, 10);
  require_test(pos == text.size(),
               "SUPERRECORD_NOISE_PROBE_INDEX must be an integer 0..5");
  require_test(value < 6, "SUPERRECORD_NOISE_PROBE_INDEX must be in 0..5");
  return static_cast<size_t>(value);
}

std::string output_path() {
  const char *raw = std::getenv("SUPERRECORD_NOISE_PROBE_OUTPUT");
  if (raw && std::string(raw) == "-") return "-";
  if (raw && std::string(raw).size() > 0) return raw;
  return "outputs/superrecord/tree_superrecord_noise_probe.json";
}

TreeParams forced_params(const PirParams &scheme, const ProbeShape &spec) {
  const size_t n = scheme.get_poly_degree();
  const size_t payload_width = payload_bits(scheme);
  CandidateShape shape;
  shape.entry_bits = spec.entry_bits;
  shape.leaf_count = spec.leaf_count;
  shape.tree_height = bit_width_minus_one(spec.leaf_count, "leaf count");
  shape.path_levels = shape.tree_height + 1;
  shape.ring_degree = n;
  shape.bundle_arity = spec.bundle_arity;
  shape.record_coeffs = spec.record_coeffs;
  shape.plane_count = spec.plane_count;
  shape.records_per_plaintext = n / shape.record_coeffs;
  shape.record_index_bits = bit_width_minus_one(shape.records_per_plaintext,
                                                "records_per_plaintext");
  shape.leaf_superrecords = ceil_div_size(shape.leaf_count,
                                          shape.bundle_arity);
  shape.leaf_plaintexts = ceil_div_size(shape.leaf_superrecords,
                                        shape.records_per_plaintext);
  shape.selector_bits = std::bit_width((shape.leaf_count - 1) /
                                       shape.bundle_arity);
  shape.N0 = spec.N0;
  shape.beta_bits = spec.beta_bits;
  shape.beta_bits_max = std::bit_width(shape.leaf_plaintexts - 1);
  shape.alpha_height = std::bit_width(shape.N0 - 1);
  shape.alpha_alignment = spec.alpha_alignment;
  shape.query_width = shape.N0 + scheme.get_l() * shape.selector_bits;
  shape.min_expansion_capacity = std::bit_ceil(shape.query_width);
  shape.min_expansion_height = bit_width_minus_one(shape.min_expansion_capacity,
                                                   "min expansion capacity");
  shape.expansion_height = spec.expansion_height;
  shape.expansion_capacity = size_t{1} << shape.expansion_height;
  shape.query_ciphertexts = 1;
  shape.response_ciphertexts = ceil_div_size(shape.plane_count * shape.path_levels,
                                             shape.records_per_plaintext);

  std::vector<tree_superrecord::LevelPlan> plans = build_level_plans(shape);
  shape.projection_depth_max = max_projection_depth(plans);
  shape.required_key_height = std::max(shape.expansion_height,
                                       shape.projection_depth_max);
  shape.registered_key_height = bit_width_minus_one(n, "ring degree");
  require_test(shape.plane_count ==
                   minimal_plane_count(shape.entry_bits, payload_width,
                                       shape.bundle_arity,
                                       shape.record_coeffs),
               "noise probe shape plane count differs from minimal codec plane count");

  Candidate candidate;
  candidate.shape = shape;
  candidate.scheme = make_scheme_fingerprint(
      scheme, payload_width, shape.registered_key_height,
      EvalKeyRegistrationPolicy::FixedMaximumHeight);
  return freeze_candidate(candidate);
}

void observe_budget(QueryObservation &query, const std::string &stage,
                    PirClient &client, const RlweCt &ct,
                    double threshold_bits) {
  BudgetObservation obs;
  obs.stage = stage;
  obs.budget_bits = static_cast<double>(client.noise_budget(ct));
  obs.margin = obs.budget_bits - threshold_bits;
  query.min_stage_margin = std::min(query.min_stage_margin, obs.margin);
  query.min_stage_budget_bits = std::min(query.min_stage_budget_bits,
                                         static_cast<int>(obs.budget_bits));
  query.budgets.push_back(std::move(obs));
}

void observe_small_q_budget(QueryObservation &query, const std::string &stage,
                            PirClient &client, const RlweCt &ct,
                            double threshold_bits) {
  uint64_t noise = 0;
  int budget = 0;
  (void)client.decrypt_mod_q_measured(ct, noise, budget);
  BudgetObservation obs;
  obs.stage = stage;
  obs.budget_bits = static_cast<double>(budget);
  obs.margin = obs.budget_bits - threshold_bits;
  obs.absolute_noise = noise;
  obs.small_q = true;
  query.max_noise = std::max(query.max_noise, noise);
  query.min_stage_margin = std::min(query.min_stage_margin, obs.margin);
  query.min_stage_budget_bits = std::min(query.min_stage_budget_bits, budget);
  if (stage.rfind("after_modulus_switch", 0) == 0) {
    query.min_final_margin = std::min(query.min_final_margin, obs.margin);
    query.min_final_budget_bits = std::min(query.min_final_budget_bits, budget);
    query.min_margin = query.min_final_margin;
    query.min_budget_bits = query.min_final_budget_bits;
  }
  query.budgets.push_back(std::move(obs));
}

PathResponse answer_path_m32_probe_checked(const EntrySource &source,
                                           ExpandedQuery &expanded,
                                           PirClient &client,
                                           PirServer &server,
                                           const TaggedTreeSessionKeys &session,
                                           const TreeParams &params,
                                           const PirParams &scheme,
                                           QueryObservation &obs,
                                           double threshold_bits,
                                           const PhaseResidualFn &phase_residuals) {
  (void)session;
  const CandidateShape &shape = params.shape;
  const size_t width = scheme.K() * scheme.get_poly_degree();
  PathResponse response;
  response.tag = params.tag;
  response.client_id = expanded.client_id;
  response.chunks.resize(shape.response_ciphertexts);
  for (RlweCt &chunk : response.chunks) {
    chunk.c0.assign(width, 0);
    chunk.c1.assign(width, 0);
    chunk.ntt_form = false;
  }

  StageClock clock(&obs.timing);
  const AlphaPyramid pyramid =
      build_ragged_alpha_pyramid(expanded.alpha, params, scheme);
  const AlphaPyramid pyramid_ntt = pyramid_to_ntt(pyramid, scheme);
  const ::AlphaPyramidM32 pyramid_m32 = pyramid_to_m32(pyramid_ntt, scheme);
  clock.lap(&TreeServerTiming::alpha_prepare);

  std::span<GSWCt> sigma(expanded.sigma_selectors);
  const bvks::BvGaloisKeys &keys = session.material->bv_galois_keys;
  for (size_t level = 0; level <= shape.tree_height; ++level) {
    const tree_superrecord::LevelPlan &plan = params.level_plans[level];
    const std::vector<LevelPlaneDatabase> planes =
        pack_level_planes(params, source, level);
    for (size_t plane = 0; plane < shape.plane_count; ++plane) {
      const LevelPlaneM32 m32 = build_level_plane_m32(planes[plane], plan,
                                                     scheme);
      RlweCt ct = select_level_m32(m32, plan, pyramid_m32, sigma, server,
                                   params, scheme, &obs.timing);
      observe_budget(obs, "select_level_m32 level " + std::to_string(level) +
                              " plane " + std::to_string(plane),
                     client, ct, threshold_bits);

      clock.restart();
      for (size_t idx = 0; idx < plan.rotation_bit_count; ++idx) {
        const size_t v = plan.rotation_bit_begin + idx;
        ct = rot_select(ct, sigma[v], size_t{1} << idx, server, scheme);
        observe_budget(obs, "rotate level " + std::to_string(level) +
                                " plane " + std::to_string(plane) +
                                " bit " + std::to_string(idx),
                       client, ct, threshold_bits);
      }
      clock.lap(&TreeServerTiming::private_rotate);

      const std::function<void(size_t, const RlweCt &)> projection_checkpoint = [&](size_t round, const RlweCt &raw) {
        observe_budget(obs, "projection raw level " +
                                std::to_string(level) + " plane " +
                                std::to_string(plane) + " round " +
                                std::to_string(round),
                       client, raw, threshold_bits);
      };
      ct = project_keep_stride(std::move(ct), plan.projection_depth, keys,
                               scheme, &projection_checkpoint);
      clock.lap(&TreeServerTiming::projection);

      const PlanePlacement at = response_placement(level, plane, params);
      RlweCt &chunk = response.chunks[at.response_chunk];
      if (at.residue_offset == 0) {
        tree_ct_add_inplace(chunk, ct, scheme);
      } else {
        const RlweCt shifted = mul_x_pow(ct, at.residue_offset, scheme);
        tree_ct_add_inplace(chunk, shifted, scheme);
      }
      observe_budget(obs, "path_pack level " + std::to_string(level) +
                              " plane " + std::to_string(plane),
                     client, chunk, threshold_bits);
      clock.lap(&TreeServerTiming::path_pack);
    }
  }

  clock.restart();
  for (size_t chunk = 0; chunk < response.chunks.size(); ++chunk) {
    observe_budget(obs, "before_modulus_switch chunk " +
                            std::to_string(chunk),
                   client, response.chunks[chunk], threshold_bits);
    const bool switched = server.switch_response_to_small_q(response.chunks[chunk]);
    response.small_q = response.small_q || switched;
    if (switched) {
      observe_small_q_budget(obs, "after_modulus_switch chunk " +
                                      std::to_string(chunk),
                             client, response.chunks[chunk], threshold_bits);
    } else {
      observe_budget(obs, "after_modulus_switch chunk " +
                              std::to_string(chunk),
                     client, response.chunks[chunk], threshold_bits);
    }
  }
  clock.lap(&TreeServerTiming::modulus_switch);
  return response;
}


uint64_t coefficient_residual(uint64_t got, uint64_t expected, uint64_t t) {
  const uint64_t up = (got + t - expected) % t;
  const uint64_t down = (expected + t - got) % t;
  return std::min(up, down);
}

void compare_response_coefficients(QueryObservation &obs,
                                   const PathResponse &response,
                                   PirClient &client,
                                   const EntrySource &source,
                                   const TreeParams &params,
                                   const PirParams &scheme, size_t leaf,
                                   const PhaseResidualFn &phase_residuals) {
  const CandidateShape &shape = params.shape;
  const uint64_t t = scheme.get_plain_mod();
  const size_t n = scheme.get_poly_degree();
  std::vector<std::vector<uint64_t>> expected(response.chunks.size(),
                                              std::vector<uint64_t>(n, 0));
  std::vector<std::vector<uint8_t>> used(response.chunks.size(),
                                         std::vector<uint8_t>(n, 0));
  for (size_t level = 0; level <= shape.tree_height; ++level) {
    const size_t node = leaf >> (shape.tree_height - level);
    const size_t superrecord = node / shape.bundle_arity;
    const EncodedSuperRecord encoded =
        encode_superrecord_at(params, source, level, superrecord);
    for (size_t plane = 0; plane < shape.plane_count; ++plane) {
      const PlanePlacement at = response_placement(level, plane, params);
      for (size_t d = 0; d < shape.record_coeffs; ++d) {
        const size_t coeff = at.residue_offset + d * shape.records_per_plaintext;
        expected[at.response_chunk][coeff] = encoded[plane][d] % t;
        used[at.response_chunk][coeff] = 1;
      }
    }
  }

  obs.expected_coefficients_match = true;
  for (size_t chunk = 0; chunk < response.chunks.size(); ++chunk) {
    const RlwePt plaintext = response.small_q
                                 ? client.decrypt_mod_q(response.chunks[chunk])
                                 : client.decrypt_ct(response.chunks[chunk]);
    if (plaintext.data.size() != n) {
      obs.expected_coefficients_match = false;
      continue;
    }
    for (size_t coeff = 0; coeff < n; ++coeff) {
      const uint64_t got = plaintext.data[coeff] % t;
      const uint64_t residual =
          coefficient_residual(got, expected[chunk][coeff], t);
      if (used[chunk][coeff]) {
        obs.max_used_plaintext_residual = std::max(obs.max_used_plaintext_residual, residual);
      } else {
        obs.max_unused_plaintext_residual = std::max(obs.max_unused_plaintext_residual, residual);
      }
      if (residual != 0) obs.expected_coefficients_match = false;
    }
    if (response.small_q) {
      const std::vector<uint64_t> phase =
          phase_residuals(client, scheme, response.chunks[chunk], plaintext);
      require_test(phase.size() == n,
                   "phase residual vector must match the ring degree");
      for (size_t coeff = 0; coeff < n; ++coeff) {
        if (used[chunk][coeff]) {
          obs.max_used_phase_residual =
              std::max(obs.max_used_phase_residual, phase[coeff]);
        } else {
          obs.max_unused_phase_residual =
              std::max(obs.max_unused_phase_residual, phase[coeff]);
        }
      }
    }
  }
}

bool selected_bundle_matches(const std::vector<std::vector<EntryBits>> &lanes,
                             const EntrySource &source,
                             const TreeParams &params, size_t leaf) {
  const CandidateShape &shape = params.shape;
  if (lanes.size() != shape.path_levels) return false;
  for (size_t level = 0; level <= shape.tree_height; ++level) {
    if (lanes[level].size() != shape.bundle_arity) return false;
    const size_t node = leaf >> (shape.tree_height - level);
    const size_t bundle_start =
        (node / shape.bundle_arity) * shape.bundle_arity;
    const size_t level_nodes = size_t{1} << level;
    const size_t real_slots = std::min(shape.bundle_arity,
                                       level_nodes - bundle_start);
    for (size_t lane = 0; lane < shape.bundle_arity; ++lane) {
      const EntryBits expected = lane < real_slots
                                     ? source(level, bundle_start + lane)
                                     : zero_entry(shape.entry_bits);
      if (lanes[level][lane] != expected) return false;
    }
  }
  return true;
}

QueryObservation run_query_probe(size_t index, size_t leaf,
                                 const EntrySource &source,
                                 PirClient &client, PirServer &server,
                                 const TaggedTreeSessionKeys &session,
                                 const TreeParams &params,
                                 const PirParams &scheme,
                                 double threshold_bits,
                                 const PhaseResidualFn &phase_residuals) {
  QueryObservation obs;
  obs.query_index = index;
  obs.leaf = leaf;
  obs.min_budget_bits = 1 << 20;
  obs.min_margin = 1.0e9;
  obs.min_stage_budget_bits = 1 << 20;
  obs.min_stage_margin = 1.0e9;
  obs.min_final_budget_bits = 1 << 20;
  obs.min_final_margin = 1.0e9;
  try {
    const PackedQuery packed = make_query(client, session, scheme, params, leaf);
    ExpandedQuery expanded = unpack_query(server, scheme, params, session,
                                          packed, &obs.timing);
    for (size_t i = 0; i < expanded.alpha.size(); ++i) {
      observe_budget(obs, "query_expand alpha " + std::to_string(i), client,
                     expanded.alpha[i], threshold_bits);
    }
    PathResponse response = answer_path_m32_probe_checked(
        source, expanded, client, server, session, params, scheme, obs,
        threshold_bits, phase_residuals);
    compare_response_coefficients(obs, response, client, source, params,
                                  scheme, leaf, phase_residuals);
    const DecodedPath decoded = extract_path_measured(response, client, params,
                                                     scheme, leaf);
    obs.max_noise = std::max(obs.max_noise, decoded.max_noise);
    obs.min_stage_budget_bits = std::min(obs.min_stage_budget_bits, decoded.min_budget_bits);
    obs.min_stage_margin = std::min(obs.min_stage_margin,
                                    static_cast<double>(decoded.min_budget_bits) -
                                        threshold_bits);
    if (obs.min_final_margin == 1.0e9) {
      obs.min_final_margin = static_cast<double>(decoded.min_budget_bits) -
                             threshold_bits;
      obs.min_final_budget_bits = decoded.min_budget_bits;
      obs.min_margin = obs.min_final_margin;
      obs.min_budget_bits = obs.min_final_budget_bits;
    }
    obs.correct_path = decoded.entries.size() == params.shape.path_levels;
    for (size_t level = 0; obs.correct_path && level <= params.shape.tree_height;
         ++level) {
      const size_t node = leaf >> (params.shape.tree_height - level);
      obs.correct_path = decoded.entries[level] == source(level, node);
    }
    const auto lanes = debug::extract_all_lanes_for_test(response, client,
                                                         params, scheme);
    obs.correct_selected_bundle =
        selected_bundle_matches(lanes, source, params, leaf);
    obs.decrypt_ok = obs.correct_path && obs.correct_selected_bundle &&
                     obs.expected_coefficients_match;
    obs.margin_ok = obs.min_final_margin >= 0.0;
  } catch (const std::exception &error) {
    obs.error = error.what();
    obs.correct_path = false;
    obs.correct_selected_bundle = false;
    obs.decrypt_ok = false;
    obs.margin_ok = false;
    obs.min_margin = -1.0e9;
  }
  return obs;
}

void json_escape(std::ostream &out, const std::string &text) {
  out << '"';
  for (char ch : text) {
    switch (ch) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch; break;
    }
  }
  out << '"';
}

void write_timing_json(std::ostream &out, const TreeServerTiming &t) {
  out << "{\"query_expand_ms\":" << stage_ms(t.query_expand)
      << ",\"selector_completion_ms\":" << stage_ms(t.selector_completion)
      << ",\"alpha_prepare_ms\":" << stage_ms(t.alpha_prepare)
      << ",\"alpha_scan_ms\":" << stage_ms(t.alpha_scan)
      << ",\"first_dim_finalize_ms\":" << stage_ms(t.first_dim_finalize)
      << ",\"beta_fold_ms\":" << stage_ms(t.beta_fold)
      << ",\"private_rotate_ms\":" << stage_ms(t.private_rotate)
      << ",\"projection_ms\":" << stage_ms(t.projection)
      << ",\"path_pack_ms\":" << stage_ms(t.path_pack)
      << ",\"modulus_switch_ms\":" << stage_ms(t.modulus_switch)
      << '}';
}

void write_probe_json(const ProbeResult &result, const std::string &path) {
  std::ostringstream buffer;
  buffer << std::fixed << std::setprecision(3);
  buffer << "{\n  \"diagnostic_only\": true,\n"
         << "  \"formal_benchmark_record\": false,\n"
         << "  \"validate_gate_certificate\": false,\n"
         << "  \"fixed_threshold_bits\": " << result.fixed_threshold_bits << ",\n"
         << "  \"used_env_shape\": " << (result.used_env_shape ? "true" : "false") << ",\n"
         << "  \"all_correct\": " << (result.all_correct ? "true" : "false") << ",\n"
         << "  \"all_margins_ok\": " << (result.all_margins_ok ? "true" : "false") << ",\n"
         << "  \"shape\": {";
  buffer << "\"label\":";
  json_escape(buffer, result.shape.label);
  buffer << ",\"leaf_count\":" << result.shape.leaf_count
         << ",\"entry_bits\":" << result.shape.entry_bits
         << ",\"bundle_arity\":" << result.shape.bundle_arity
         << ",\"record_coeffs\":" << result.shape.record_coeffs
         << ",\"plane_count\":" << result.shape.plane_count
         << ",\"N0\":" << result.shape.N0
         << ",\"beta_bits\":" << result.shape.beta_bits
         << ",\"expansion_height\":" << result.shape.expansion_height
         << ",\"alpha_alignment\":" << result.shape.alpha_alignment
         << ",\"registered_key_height\":" << result.registered_key_height
         << "},\n  \"queries\": [";
  for (size_t i = 0; i < result.queries.size(); ++i) {
    const QueryObservation &q = result.queries[i];
    if (i) buffer << ',';
    buffer << "\n    {\"query_index\":" << q.query_index
           << ",\"leaf\":" << q.leaf
           << ",\"correct_path\":" << (q.correct_path ? "true" : "false")
           << ",\"correct_selected_bundle\":" << (q.correct_selected_bundle ? "true" : "false")
           << ",\"decrypt_ok\":" << (q.decrypt_ok ? "true" : "false")
           << ",\"margin_ok\":" << (q.margin_ok ? "true" : "false")
           << ",\"min_margin\":" << q.min_margin
           << ",\"min_budget_bits\":" << q.min_budget_bits
           << ",\"min_stage_margin\":" << q.min_stage_margin
           << ",\"min_stage_budget_bits\":" << q.min_stage_budget_bits
           << ",\"min_final_margin\":" << q.min_final_margin
           << ",\"min_final_budget_bits\":" << q.min_final_budget_bits
           << ",\"max_noise\":" << q.max_noise
           << ",\"expected_coefficients_match\":" << (q.expected_coefficients_match ? "true" : "false")
           << ",\"max_used_plaintext_residual\":" << q.max_used_plaintext_residual
           << ",\"max_unused_plaintext_residual\":" << q.max_unused_plaintext_residual
           << ",\"max_used_phase_residual\":" << q.max_used_phase_residual
           << ",\"max_unused_phase_residual\":" << q.max_unused_phase_residual
           << ",\"timing\":";
    write_timing_json(buffer, q.timing);
    if (!q.error.empty()) {
      buffer << ",\"error\":";
      json_escape(buffer, q.error);
    }
    buffer << ",\"budgets\":[";
    for (size_t j = 0; j < q.budgets.size(); ++j) {
      const BudgetObservation &b = q.budgets[j];
      if (j) buffer << ',';
      buffer << "{\"stage\":";
      json_escape(buffer, b.stage);
      buffer << ",\"budget_bits\":" << b.budget_bits
             << ",\"margin\":" << b.margin
             << ",\"absolute_noise\":" << b.absolute_noise
             << ",\"small_q\":" << (b.small_q ? "true" : "false")
             << '}';
    }
    buffer << "]}";
  }
  buffer << "\n  ]\n}\n";

  if (path == "-") {
    std::cout << buffer.str();
    return;
  }
  const std::filesystem::path out(path);
  if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path());
  std::ofstream file(out);
  require_test(file.is_open(), "unable to open noise probe output path");
  file << buffer.str();
  std::cout << "Tree superrecord noise probe JSON: " << path << '\n';
}

ProbeResult run_probe(const PhaseResidualFn &phase_residuals) {
  PirParams scheme;
  require_m32_scheme(scheme);
  const std::optional<size_t> index = env_probe_index();
  ProbeShape shape = index ? clemson_shapes().at(*index) : smoke_shape(scheme);
  TreeParams params = forced_params(scheme, shape);
  shape.N0 = params.shape.N0;
  shape.beta_bits = params.shape.beta_bits;
  shape.expansion_height = params.shape.expansion_height;

  ProbeResult result;
  result.shape = shape;
  result.used_env_shape = index.has_value();
  result.registered_key_height = params.shape.registered_key_height;

  const EntrySource source = [&](size_t level, size_t node) {
    return benchmark_entry_for_probe(shape.entry_bits, level, node);
  };

  PirClient client(scheme);
  PirServer server(query_expansion_params(params, scheme));
  const TaggedTreeSessionKeys session =
      create_tagged_tree_session_keys(client, params, scheme);
  set_tree_client_session_keys(server, params, scheme, session);

  const size_t query_count = std::min<size_t>(3, shape.leaves.size());
  for (size_t i = 0; i < query_count; ++i) {
    QueryObservation obs = run_query_probe(i, shape.leaves[i], source, client,
                                           server, session, params, scheme,
                                           result.fixed_threshold_bits,
                                           phase_residuals);
    result.all_correct = result.all_correct && obs.decrypt_ok;
    result.all_margins_ok = result.all_margins_ok && obs.margin_ok;
    result.queries.push_back(std::move(obs));
  }
  return result;
}

}  // namespace

void PirTest::test_tree_superrecord_noise_probe() {
  print_func_name(__FUNCTION__);
  const PhaseResidualFn phase_residuals = [](const PirClient &client,
                                              const PirParams &scheme,
                                              const RlweCt &ct,
                                              const RlwePt &decoded) {
    const size_t N = scheme.get_poly_degree();
    const uint64_t small_q = scheme.get_small_q();
    std::vector<uint64_t> phase(N);
    std::vector<uint64_t> c0(N), c1_ntt(N);
    for (size_t i = 0; i < N; ++i) {
      c0[i] = ct.c0[i] % small_q;
      c1_ntt[i] = ct.c1[i] % small_q;
    }
    std::vector<uint64_t> sk_ntt_small_q =
        client.get_sk_ntt_small_q(scheme.get_rns_mods()[0], small_q);
    utils::ntt_fwd(c1_ntt.data(), N, small_q);
    intel::hexl::EltwiseMultMod(phase.data(), c1_ntt.data(),
                                sk_ntt_small_q.data(), N, small_q, 1);
    utils::ntt_inv(phase.data(), N, small_q);
    intel::hexl::EltwiseAddMod(phase.data(), phase.data(), c0.data(), N,
                               small_q);

    const uint64_t t = scheme.get_plain_mod();
    const uint64_t half_q = small_q / 2;
    std::vector<uint64_t> residuals(N, 0);
    for (size_t i = 0; i < N; ++i) {
      const uint64_t m = decoded.data[i] % t;
      const uint64_t approx =
          utils::round_div_u128((uint128_t)small_q * m, t) % small_q;
      const uint64_t pos = phase[i] >= approx
                               ? phase[i] - approx
                               : small_q - approx + phase[i];
      residuals[i] = pos > half_q ? small_q - pos : pos;
    }
    return residuals;
  };
  ProbeResult result = run_probe(phase_residuals);
  const std::string path = output_path();
  write_probe_json(result, path);
  require_test(result.all_correct, "tree superrecord noise probe decode failed; see diagnostic JSON");
  require_test(result.all_margins_ok,
               "tree superrecord noise probe margin below threshold; see diagnostic JSON");
}
