#include "tree_superrecord_benchmark_internal.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>

namespace tree_superrecord_benchmark_internal {

using namespace tree_superrecord;
using namespace tree_superrecord_test;

double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Measure the four preparation operations in their own physical units.
void calibrate_alpha_operations(CostCalibration &calibration, PirClient &client,
                                const PirParams &scheme) {
  constexpr size_t repetitions = 64;
  constexpr size_t batch = 16;
  volatile uint64_t sink = 0;
  const RlweCt seed = client.fresh_zero_ct();
  RlweCt accumulator = seed;
  auto start = Clock::now();
  for (size_t i = 0; i < repetitions; ++i) {
    tree_ct_add_inplace(accumulator, seed, scheme);
  }
  calibration.alpha_pyramid_add_ms = ms_since(start) / repetitions;
  sink = accumulator.c0[0];
  std::vector<RlweCt> carries;
  carries.reserve(repetitions);
  start = Clock::now();
  for (size_t i = 0; i < repetitions; ++i) carries.push_back(seed);
  calibration.alpha_carry_copy_ms = ms_since(start) / repetitions;
  sink = sink ^ carries.back().c0[0];
  const ::AlphaPyramid coefficients{std::vector<RlweCt>(batch, seed)};
  (void)::pyramid_to_ntt(coefficients, scheme);
  start = Clock::now();
  const ::AlphaPyramid ntt = ::pyramid_to_ntt(coefficients, scheme);
  calibration.alpha_ntt_per_ciphertext_ms = ms_since(start) / batch;
  (void)::pyramid_to_m32(ntt, scheme);
  start = Clock::now();
  const ::AlphaPyramidM32 split = ::pyramid_to_m32(ntt, scheme);
  calibration.alpha_split_per_u32_output_ms =
      ms_since(start) /
      static_cast<double>(batch * 4 * scheme.get_poly_degree());
  sink = sink ^ split.lo[0][0];
  (void)sink;
}

KeyCoverageReport inspect_keys(const TreeParams &params,
                               const TaggedTreeSessionKeys &session) {
  KeyCoverageReport out;
  out.required_key_height = params.shape.required_key_height;
  out.registered_key_height = params.shape.registered_key_height;
  const BvKeyCoverage coverage =
      inspect_bv_key_coverage(session.material->bv_galois_keys,
                              params.scheme.identity,
                              params.shape.registered_key_height);
  out.observed_contiguous_height = coverage.contiguous_height;
  out.observed_key_count = coverage.key_count;
  out.missing_required = coverage.missing_required.size();
  out.duplicate = coverage.duplicates.size();
  out.unexpected = coverage.unexpected.size();
  out.malformed_row_shapes = coverage.malformed_row_shapes.size();
  out.matches = out.observed_contiguous_height >= out.required_key_height &&
                out.observed_key_count == out.registered_key_height &&
                out.missing_required == 0 && out.duplicate == 0 &&
                out.unexpected == 0 && out.malformed_row_shapes == 0;
  return out;
}

void add_candidate(std::vector<RunResult> &rows, const Candidate &cand,
                   const std::string &label, bool mandatory,
                   const ParetoContext &context) {
  const PlanTag tag = make_plan_tag(cand.shape, cand.scheme);
  const std::vector<tree_superrecord::LevelPlan> plans =
      build_level_plans(cand.shape);
  const FirstDimDispatchSignature dispatch =
      first_dim_dispatch_signature(cand, context);
  for (const RunResult &existing : rows) {
    if (!existing.candidate) continue;
    if (make_plan_tag(existing.candidate->shape, existing.candidate->scheme) ==
            tag &&
        existing.candidate->scheme == cand.scheme &&
        build_level_plans(existing.candidate->shape) == plans &&
        first_dim_dispatch_signature(*existing.candidate, context) ==
            dispatch) {
      return;
    }
  }
  RunResult r;
  r.row_id = "candidate-" + std::to_string(rows.size());
  r.label = label;
  if (std::any_of(rows.begin(), rows.end(), [&](const RunResult &existing) {
        return existing.label == r.label;
      })) {
    r.label += "-variant-" + std::to_string(rows.size());
  }
  r.description = describe_candidate(cand);
  r.is_generic = true;
  r.is_h1 = cand.shape.bundle_arity == 1;
  r.is_mandatory = mandatory;
  r.candidate = cand;
  rows.push_back(std::move(r));
}

size_t count_h1(const std::vector<RunResult> &rows) {
  size_t total = 0;
  for (const RunResult &r : rows) {
    if (r.is_generic && r.is_h1 && r.eligible && !r.samples.empty()) ++total;
  }
  return total;
}

void calibrate_from_pilot(CandidatePreparation &prepared,
                          const BenchmarkOptions &options, PirClient &client,
                          const PirParams &scheme) {
  const size_t pilot_leaves = std::min<size_t>(options.leaf_count, 4096);
  auto pilots = enumerate_candidates(
      make_input(scheme, options.entry_bits, pilot_leaves, options.policy));
  filter_candidates_by_bundle_arity(pilots, options.bundle_arities);
  rank_by_prediction(pilots, prepared.calibration, prepared.context);
  auto pilot_it =
      std::find_if(pilots.begin(), pilots.end(), [](const Candidate &candidate) {
        return candidate.shape.bundle_arity == 1 &&
               candidate.shape.beta_bits >= 2;
      });
  if (pilot_it == pilots.end()) pilot_it = pilots.begin();
  require_test(pilot_it != pilots.end(), "no feasible calibration control");
  prepared.calibration_control = *pilot_it;
  const TreeParams pilot = freeze_candidate(*pilot_it);
  PirServer pilot_server(query_expansion_params(pilot, scheme));
  const auto pilot_session =
      create_tagged_tree_session_keys(client, pilot, scheme);
  set_tree_client_session_keys(pilot_server, pilot, scheme, pilot_session);
  const EntrySource pilot_source = [&](size_t level, size_t node) {
    return benchmark_entry(options.entry_bits, level, node);
  };
  const TargetNoiseCase fixture{pilot_leaves - 1, pilot_source,
                                "calibration-control"};
  const auto noise = run_target_shape_noise_gate(
      client, pilot_server, pilot_session, pilot, scheme,
      std::span<const TargetNoiseCase>(&fixture, 1),
      options.policy.min_noise_margin, ResponsePacking::Projection);
  require_target_noise_gate(noise, pilot, client.get_client_id());
  const auto db = preprocess_m32(pilot, scheme, noise, pilot_source, false);
  const auto query =
      make_query(client, pilot_session, scheme, pilot, pilot_leaves - 1);
  (void)serve_request_m32(query, db, pilot_server, pilot_session, pilot, scheme,
                          noise);
  const auto response = serve_request_m32(
      query, db, pilot_server, pilot_session, pilot, scheme, noise,
      &prepared.calibration_timing, ResponsePacking::Projection);
  const auto decoded =
      extract_path_measured(response, client, pilot, scheme, pilot_leaves - 1);
  for (size_t level = 0; level < pilot.shape.path_levels; ++level) {
    require_test(decoded.entries[level] ==
                     pilot_source(level, (pilot_leaves - 1) >>
                                             (pilot.shape.tree_height - level)),
                 "calibration control decrypt mismatch");
  }
  apply_measured_control_timing(prepared.calibration, *pilot_it,
                                prepared.calibration_timing);
  calibrate_alpha_operations(prepared.calibration, client, scheme);
  BENCH_PRINT("calibrating first-dimension kernels for "
              << prepared.all.size() << " candidate shapes");
  prepared.calibration =
      calibrate_cost_model(prepared.all, prepared.calibration,
                           prepared.context);
}

CandidatePreparation prepare_candidate_inventory(const BenchmarkOptions &options,
                                                 const PirParams &scheme) {
  CandidatePreparation prepared;
  prepared.input =
      make_input(scheme, options.entry_bits, options.leaf_count, options.policy);
  prepared.all = enumerate_candidates(prepared.input);
  filter_candidates_by_bundle_arity(prepared.all, options.bundle_arities);
  filter_candidates_by_pinned_shape(prepared.all);
  prepared.enumerated = prepared.all.size();
  prepared.h1_enumerated = static_cast<size_t>(
      std::count_if(prepared.all.begin(), prepared.all.end(),
                    [](const Candidate &c) { return c.shape.bundle_arity == 1; }));

  prepared.context = {detected_matmul32_availability()};
  return prepared;
}

void finish_candidate_preparation(CandidatePreparation &prepared,
                                  const BenchmarkOptions &options,
                                  const PirParams &scheme,
                                  PirClient &client) {
  if (!options.plan_only && !prepared.all.empty()) {
    calibrate_from_pilot(prepared, options, client, scheme);
  }
  rank_by_prediction(prepared.all, prepared.calibration, prepared.context);
  prepared.frontier =
      pareto_frontier(prepared.all, options.policy, prepared.context);
  rank_by_prediction(prepared.frontier, prepared.calibration, prepared.context);
}

std::vector<RunResult> prepare_benchmark_rows(
    const BenchmarkOptions &options, const CandidatePreparation &prepared) {
  std::vector<RunResult> rows;
  auto mandatory_label = [&](const Candidate &cand) {
    std::string label = "mandatory-control";
    if (options.entry_bits == 256 && cand.shape.bundle_arity == 3 &&
        cand.shape.record_coeffs == 64 && cand.shape.plane_count == 1) {
      label = "bundle3";
    } else if (!options.bundle_arities.empty() &&
               cand.shape.bundle_arity == 3) {
      label = "generic-h3-mandatory";
    } else if (cand.shape.bundle_arity == 1) {
      label = "generic-h1-mandatory-" + std::to_string(rows.size());
    } else if (!prepared.all.empty() &&
               cand.metrics.optimized_steady_state_bytes_estimate ==
                   std::min_element(
                       prepared.all.begin(), prepared.all.end(),
                       [](const Candidate &a, const Candidate &b) {
                         return a.metrics.optimized_steady_state_bytes_estimate <
                                b.metrics.optimized_steady_state_bytes_estimate;
                       })
                       ->metrics.optimized_steady_state_bytes_estimate) {
      label = "minimum-optimized-steady-state";
    } else {
      label = "minimum-response-count";
    }
    return label;
  };
  if (options.bundle_arities.empty()) {
    for (const Candidate &cand :
         mandatory_controls(prepared.all, prepared.input, prepared.calibration,
                            prepared.context)) {
      add_candidate(rows, cand, mandatory_label(cand), true, prepared.context);
    }
  } else {
    for (size_t arity : options.bundle_arities) {
      auto best = std::find_if(
          prepared.all.begin(), prepared.all.end(),
          [&](const Candidate &cand) { return cand.shape.bundle_arity == arity; });
      if (best != prepared.all.end()) {
        add_candidate(rows, *best, mandatory_label(*best), true,
                      prepared.context);
      }
    }
  }
  size_t extra_h1 = 0;
  for (const Candidate &cand : prepared.frontier) {
    if (extra_h1 >= options.h1_frontier) break;
    if (cand.shape.bundle_arity != 1) continue;
    const size_t before = rows.size();
    add_candidate(rows, cand,
                  "generic-h1-frontier-" + std::to_string(extra_h1 + 1),
                  false, prepared.context);
    if (rows.size() != before) ++extra_h1;
  }
  size_t extra = 0;
  for (const Candidate &cand : prepared.frontier) {
    if (extra >= options.max_frontier) break;
    const size_t before = rows.size();
    add_candidate(rows, cand, "frontier-" + std::to_string(extra + 1), false,
                  prepared.context);
    if (rows.size() != before) ++extra;
  }

  if (options.include_legacy) {
    RunResult legacy;
    legacy.row_id = "candidate-" + std::to_string(rows.size());
    legacy.label = "legacy-g32";
    legacy.is_mandatory = true;
    rows.push_back(std::move(legacy));
  }
  return rows;
}

std::vector<size_t> shuffled_execution_order(const std::vector<RunResult> &rows,
                                             uint64_t execution_seed,
                                             std::mt19937_64 &rng) {
  std::vector<size_t> execution_order(rows.size());
  std::iota(execution_order.begin(), execution_order.end(), 0);
  rng.seed(execution_seed);
  std::shuffle(execution_order.begin(), execution_order.end(), rng);
  const std::optional<size_t> candidate_index =
      env_optional_size("SUPERRECORD_CANDIDATE_INDEX");
  if (candidate_index) {
    if (*candidate_index >= execution_order.size()) {
      throw std::invalid_argument(
          "SUPERRECORD_CANDIDATE_INDEX is outside candidate_execution_order");
    }
    execution_order = {execution_order[*candidate_index]};
  }
  return execution_order;
}

LegacyExecutionPlan prepare_legacy_execution_plan(
    const BenchmarkOptions &options, const PirParams &scheme, size_t c,
    size_t n) {
  LegacyExecutionPlan legacy;
  if (!options.include_legacy) return legacy;
  const size_t legacy_g = std::bit_ceil(ceil_div(size_t{256}, c));
  for (size_t a = 20;; --a) {
    try {
      legacy.tree = make_tree_pir_params_for_scheme(
          options.tree_height, a, legacy_g, scheme, scheme.get_expan_height());
      break;
    } catch (const std::invalid_argument &) {
      if (a == 2) throw;
    }
  }
  for (const auto &level : ::build_level_plans(*legacy.tree)) {
    legacy.resident_estimate = checked_add(
        legacy.resident_estimate,
        checked_mul(level.R, n * 16, "legacy canonical and m32 bytes"),
        "legacy resident bytes");
  }
  // Database payload dominates; include a query working-set allowance.
  legacy.resident_estimate = checked_add(
      legacy.resident_estimate, 128 * 1024 * 1024, "legacy working estimate");
  return legacy;
}

size_t canonical_level_peak_bytes(const RunResult &row, size_t n) {
  size_t peak = 0;
  if (row.candidate) {
    const auto &cand = *row.candidate;
    for (const auto &level : build_level_plans(cand.shape)) {
      peak = std::max(
          peak,
          checked_mul(level.padded_plaintexts_per_plane,
                      checked_mul(cand.shape.plane_count, n * 8,
                                  "canonical level bytes"),
                      "canonical level bytes"));
    }
  }
  return peak;
}

ResidentSelection select_resident_execution_order(
    std::vector<RunResult> &rows, std::vector<size_t> execution_order,
    const PlannerPolicy &policy, const LegacyExecutionPlan &legacy, size_t n) {
  const auto resident = [&](size_t index) {
    return rows[index].candidate
               ? rows[index].candidate->metrics.optimized_steady_state_bytes_estimate
               : legacy.resident_estimate;
  };
  const auto canonical_level_peak = [&](size_t index) {
    return canonical_level_peak_bytes(rows[index], n);
  };
  const size_t resident_limit = policy.max_steady_state_bytes;
  ResidentSelection selection;
  for (size_t index : execution_order) {
    if (!rows[index].is_mandatory) continue;
    selection.execution_order.push_back(index);
    selection.resident_sum = checked_add(selection.resident_sum,
                                         resident(index),
                                         "blocked resident estimate");
    selection.temporary_peak =
        std::max(selection.temporary_peak, canonical_level_peak(index));
  }
  require_test(checked_add(selection.resident_sum, selection.temporary_peak,
                           "blocked peak estimate") <= resident_limit,
               "mandatory controls exceed the co-resident benchmark memory "
               "budget; use a smaller leaf count");
  auto optional_order = execution_order;
  std::stable_sort(optional_order.begin(), optional_order.end(),
                   [&](size_t a, size_t b) {
                     const double a_ms =
                         rows[a].candidate
                             ? rows[a].candidate->metrics.predicted_server_ms
                             : 0;
                     const double b_ms =
                         rows[b].candidate
                             ? rows[b].candidate->metrics.predicted_server_ms
                             : 0;
                     return a_ms < b_ms;
                   });
  for (size_t index : optional_order) {
    if (rows[index].is_mandatory) continue;
    const size_t next_sum = checked_add(selection.resident_sum,
                                        resident(index),
                                        "blocked resident estimate");
    const size_t next_peak =
        std::max(selection.temporary_peak, canonical_level_peak(index));
    if (checked_add(next_sum, next_peak, "blocked peak estimate") <=
        resident_limit) {
      selection.execution_order.push_back(index);
      selection.resident_sum = next_sum;
      selection.temporary_peak = next_peak;
    } else {
      rows[index].reason = "excluded by co-resident benchmark memory budget";
    }
  }
  return selection;
}

Runner make_legacy_runner(RunResult &r, const TreePirParams &tree,
                          PirClient &client, const PirParams &scheme,
                          const PlannerPolicy &policy, uint64_t t,
                          size_t n) {
  const TreeNodeChunkSource chunk_source =
      [t](size_t level, size_t node, size_t chunk) {
        return synthetic_tree_node_bytes_chunk(level, node, chunk, t);
      };
  const size_t required = std::max(tree.h_q, tree.r);
  const size_t registered =
      policy.eval_key_registration == EvalKeyRegistrationPolicy::FixedMaximumHeight
          ? policy.fixed_session_key_height
          : required;
  const auto session_start = Clock::now();
  const auto keys = client.create_session_keys(registered);
  require_bv_key_coverage(keys->bv_galois_keys, scheme.scheme_identity(),
                          required, registered);
  const auto coverage =
      inspect_bv_key_coverage(keys->bv_galois_keys, scheme.scheme_identity(),
                              registered);
  r.key_coverage = {required,
                    registered,
                    coverage.contiguous_height,
                    coverage.key_count,
                    coverage.missing_required.size(),
                    coverage.duplicates.size(),
                    coverage.unexpected.size(),
                    coverage.malformed_row_shapes.size(),
                    true};
  auto server =
      std::make_shared<PirServer>(tree_query_expansion_params(tree, scheme));
  server->set_client_session_keys(client.get_client_id(), keys);
  r.session_setup_ms = ms_since(session_start);
  const auto setup_start = Clock::now();
  auto db = std::make_shared<::PreprocessedTree>(
      preprocess_tree_mvp(tree, chunk_source, scheme));
  r.setup_ms = ms_since(setup_start);
  for (const auto &level : db->canonical) r.plaintexts += level.plaintexts.size();
  r.db_bytes = r.plaintexts * n * 16;
  r.scan_bytes = r.plaintexts * n * 8;
  r.query_bytes = scheme.get_BFV_size(true);
  r.description = "E=256 g=" + std::to_string(tree.g) +
                  " N0=" + std::to_string(tree.N0) +
                  " b=" + std::to_string(tree.b) +
                  " rho=" + std::to_string(tree.rho) +
                  " h_q=" + std::to_string(tree.h_q);
  r.eligible = true;
  return [&, tree, chunk_source, server, db](size_t leaf) {
    CLEAN_TIMER();
    Sample sample;
    sample.leaf = leaf;
    auto start = Clock::now();
    RlweCt query = make_tree_query(client, scheme, tree, leaf);
    sample.client_query_ms = ms_since(start);
    start = Clock::now();
    TreeQueryUnpackTiming unpack_timing{};
    auto expanded = unpack_tree_query(*server, scheme, tree,
                                      client.get_client_id(), query,
                                      &unpack_timing);
    sample.stages_ms[0] = stage_ms(unpack_timing.query_expand);
    sample.stages_ms[1] = stage_ms(unpack_timing.selector_completion);
    auto response =
        answer_path_mvp(*db, expanded, *server, client.get_client_id(), tree,
                        scheme);
    sample.server_total_ms = ms_since(start);
    END_EXPERIMENT();
    start = Clock::now();
    const auto pt = client.decrypt_mod_q_measured(
        response.chunks[0], sample.max_noise, sample.min_budget_bits);
    sample.client_decode_ms = ms_since(start);
    for (size_t level = 0; level <= tree.L; ++level) {
      for (size_t j = 0; j < tree.g; ++j) {
        require_test(pt.data[response.level_offsets[level] + j * tree.rho] ==
                         chunk_source(level, leaf >> (tree.L - level), j),
                     "legacy path chunk mismatch");
      }
    }
    if (policy.min_noise_margin > 0) {
      require_test(sample.min_budget_bits >=
                       static_cast<int>(policy.min_noise_margin),
                   "legacy noise margin failed");
    }
    const std::array<const char *, 7> names = {
        TREE_PYRAMID_TIME, TREE_SCAN_TIME,   TREE_FOLD_TIME, TREE_ROTATE_TIME,
        TREE_PROJECT_TIME, TREE_PACK_TIME,   TREE_SWITCH_TIME};
    const std::array<size_t, 7> slots = {2, 3, 5, 6, 7, 8, 9};
    for (size_t j = 0; j < names.size(); ++j) {
      sample.stages_ms[slots[j]] = GET_LAST_TIME(names[j]);
    }
    start = Clock::now();
    std::stringstream wire;
    sample.response_payload_bytes =
        server->save_resp_to_stream(response.chunks[0], wire);
    sample.response_serialize_ms = ms_since(start);
    return sample;
  };
}

Runner make_generic_runner(RunResult &r, const Candidate &cand,
                           PirClient &client, const PirParams &scheme,
                           const PlannerPolicy &policy,
                           const BenchmarkTrialPlan &trial_plan,
                           const EntrySource &source, size_t leaf_count,
                           ResponsePacking response_packing) {
  const TreeParams params = freeze_candidate(cand);
  const auto session_start = Clock::now();
  auto server_ptr =
      std::make_shared<PirServer>(query_expansion_params(params, scheme));
  PirServer &server = *server_ptr;
  const auto session = create_tagged_tree_session_keys(client, params, scheme);
  set_tree_client_session_keys(server, params, scheme, session);
  r.key_coverage = inspect_keys(params, session);
  r.session_setup_ms = ms_since(session_start);
  if (!r.key_coverage.matches) {
    r.reason = "actual key coverage mismatch";
    return {};
  }
  std::set<size_t> boundary_leaves{
      0, leaf_count - 1, trial_plan.measured_leaf_indices.front()};
  for (size_t boundary :
       {params.shape.bundle_arity,
        checked_mul(params.shape.bundle_arity,
                    params.shape.records_per_plaintext, "noise boundary")}) {
    if (boundary < leaf_count) boundary_leaves.insert(boundary);
    if (boundary > 0 && boundary - 1 < leaf_count) {
      boundary_leaves.insert(boundary - 1);
    }
  }
  const std::vector<size_t> gate_leaves(boundary_leaves.begin(),
                                        boundary_leaves.end());
  std::vector<TargetNoiseCase> noise_cases;
  const CodecSpec fixture_codec = codec_spec_for(params);
  // Cover every boundary class and every data pattern without their Cartesian
  // product.
  for (size_t index = 0; index < std::max<size_t>(5, gate_leaves.size());
       ++index) {
    const size_t pattern = index % 5;
    const NoiseEntrySource fixture_source = [=](size_t level, size_t node) {
      if (pattern == 1) return benchmark_entry(cand.shape.entry_bits, level, node);
      return tree_superrecord_benchmark_data::patterned_entry(fixture_codec,
                                                              node, pattern);
    };
    noise_cases.push_back(
        {gate_leaves[index % gate_leaves.size()], fixture_source,
         "leaf-" + std::to_string(gate_leaves[index % gate_leaves.size()]) +
             "-pattern-" + std::to_string(pattern)});
  }
  const auto noise_start = Clock::now();
  const CandidateNoiseResult noise_result = run_target_shape_noise_gate(
      client, server, session, params, scheme,
      std::span<const TargetNoiseCase>(noise_cases.data(), noise_cases.size()),
      static_cast<double>(policy.min_noise_margin), response_packing);
  r.noise_gate_ms = ms_since(noise_start);
  r.noise_result = noise_result;
  if (!noise_result_unlocks_candidate(noise_result, params,
                                      client.get_client_id(),
                                      response_packing)) {
    r.reason = "target-shape noise gate failed";
    return {};
  }

  const auto setup_start = Clock::now();
  auto db = std::make_shared<tree_superrecord::PreprocessedTree>(
      preprocess_m32(params, scheme, noise_result, source, false));
  r.setup_ms = ms_since(setup_start);
  r.db_bytes = m32_database_bytes(*db);
  r.scan_bytes = cand.metrics.physical_scan_bytes;
  r.plaintexts = cand.metrics.physical_matrix_cells;
  r.query_bytes = scheme.get_BFV_size(true);
  r.eligible = true;
  return [&, params, session, noise_result, server_ptr, db, source,
          candidate_label = r.label, response_packing](size_t leaf) {
    Sample sample;
    sample.leaf = leaf;
    auto start = Clock::now();
    auto query = make_query(client, session, scheme, params, leaf);
    sample.client_query_ms = ms_since(start);
    TreeServerTiming timing{};
    auto response = serve_request_m32(query, *db, *server_ptr, session, params,
                                      scheme, noise_result, &timing,
                                      response_packing);
    sample.server_total_ms = stage_ms(timing.total);
    sample.response_merge_keyswitches = timing.response_merge_keyswitches;
    start = Clock::now();
    const auto decoded =
        extract_path_measured(response, client, params, scheme, leaf);
    sample.client_decode_ms = ms_since(start);
    require_test(decoded.entries.size() == params.shape.path_levels,
                 "generic path length mismatch");
    for (size_t level = 0; level < params.shape.path_levels; ++level) {
      require_test(decoded.entries[level] ==
                       source(level, leaf >> (params.shape.tree_height - level)),
                   "generic path entry mismatch");
    }
    sample.min_budget_bits = decoded.min_budget_bits;
    sample.max_noise = decoded.max_noise;
    if (policy.min_noise_margin > 0 &&
        sample.min_budget_bits < static_cast<int>(policy.min_noise_margin)) {
      std::ostringstream message;
      message << "measured noise margin failed: candidate=" << candidate_label
              << " leaf=" << leaf
              << " h=" << params.shape.bundle_arity
              << " G=" << params.shape.record_coeffs
              << " P=" << params.shape.plane_count
              << " N0=" << params.shape.N0
              << " b=" << params.shape.beta_bits
              << " h_q=" << params.shape.expansion_height
              << " min_budget_bits=" << sample.min_budget_bits
              << " required_budget_bits=" << policy.min_noise_margin
              << " max_residual=" << sample.max_noise
              << " decoder_bound="
              << static_cast<double>(scheme.get_small_q()) /
                     (2.0 * static_cast<double>(scheme.get_plain_mod()))
              << " selected_path_correct=true";
      throw std::runtime_error(message.str());
    }
    const std::array<TreeStageDuration, 11> stages = {
        timing.query_expand,       timing.selector_completion,
        timing.alpha_prepare,      timing.alpha_scan,
        timing.first_dim_finalize, timing.beta_fold,
        timing.private_rotate,     timing.projection,
        timing.path_pack,          timing.modulus_switch,
        timing.response_merge};
    for (size_t j = 0; j < stages.size(); ++j) {
      sample.stages_ms[j] = stage_ms(stages[j]);
    }
    start = Clock::now();
    sample.response_payload_bytes =
        serialized_response_payload_bytes(response, *server_ptr, params)
            .value_or(0);
    sample.response_serialize_ms = ms_since(start);
    for (size_t level = 0; level < params.shape.path_levels; ++level) {
      sample.real_non_target_neighbour_slots +=
          real_bundle_slots(leaf, level, params) - 1;
    }
    return sample;
  };
}

void prepare_runners(std::vector<Runner> &runners,
                     std::vector<RunResult> &rows,
                     const std::vector<size_t> &execution_order,
                     const BenchmarkOptions &options,
                     const LegacyExecutionPlan &legacy,
                     PirClient &client,
                     const PirParams &scheme,
                     const EntrySource &source, uint64_t t,
                     size_t n) {
  for (size_t index : execution_order) {
    RunResult &r = rows[index];
    BENCH_PRINT("preparing [" << r.label << "] " << r.description);
    if (!r.is_generic) {
      runners[index] = make_legacy_runner(r, *legacy.tree, client, scheme,
                                          options.policy, t, n);
      continue;
    }
    const Candidate &cand = *r.candidate;
    runners[index] =
        make_generic_runner(r, cand, client, scheme, options.policy,
                            options.trial_plan, source, options.leaf_count,
                            options.response_packing);
  }
}

void run_interleaved_blocks(std::vector<RunResult> &rows,
                            const std::vector<size_t> &execution_order,
                            std::vector<Runner> &runners,
                            const BenchmarkOptions &options,
                            std::mt19937_64 &rng,
                            std::vector<std::vector<size_t>> &block_orders) {
  std::vector<size_t> active;
  for (size_t index : execution_order) {
    if (rows[index].eligible) active.push_back(index);
  }
  for (size_t block = 0; block < options.warmups + options.trials; ++block) {
    const bool measured = block >= options.warmups;
    const size_t leaf =
        measured ? options.trial_plan.measured_leaf_indices[block - options.warmups]
                 : options.trial_plan.warmup_leaf_indices[block];
    std::shuffle(active.begin(), active.end(), rng);
    block_orders.push_back(active);
    for (size_t index : active) {
      Sample sample;
      try {
        sample = runners[index](leaf);
      } catch (const std::exception &error) {
        throw std::runtime_error(
            "generic block failure: phase=" +
            std::string(measured ? "measured" : "warmup") +
            " block=" + std::to_string(block) +
            " row_id=" + rows[index].row_id + ": " + error.what());
      }
      if (measured) {
        sample.block_id = block - options.warmups;
        rows[index].samples.push_back(std::move(sample));
      }
    }
    BENCH_PRINT("completed " << (measured ? "measured" : "warmup")
                             << " block " << block << " leaf " << leaf);
  }
}

void finalize_run_results(std::vector<RunResult> &rows, size_t trials) {
  for (RunResult &r : rows) {
    if (!r.eligible) continue;
    require_test(r.samples.size() == trials, "measured sample count mismatch");
    r.min_budget = std::numeric_limits<int>::max();
    for (const auto &sample : r.samples) {
      r.query_ms_avg += sample.client_query_ms;
      r.extract_ms_avg += sample.client_decode_ms;
      r.neighbour_slots_avg += sample.real_non_target_neighbour_slots;
      r.min_budget = std::min(r.min_budget, sample.min_budget_bits);
      r.max_noise = std::max(r.max_noise, sample.max_noise);
      r.response_bytes = sample.response_payload_bytes;
    }
    r.query_ms_avg /= trials;
    r.extract_ms_avg /= trials;
    r.neighbour_slots_avg /= trials;
  }
}

}  // namespace tree_superrecord_benchmark_internal
