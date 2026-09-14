#include "merkle_benchmark_internal.h"

#include "client.h"
#include "layer_layout_planner.h"
#include "logging.h"
#include "server.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace {

struct PirCallOutput {
  TrialTiming timing;
  size_t response_bytes = 0;
  RlwePt plaintext;
  std::optional<MerkleNode> node;
  PirPipelineProfile pipeline;
};

struct PathTrialOutput {
  TrialTiming timing;
  std::vector<size_t> response_bytes;  // one entry per PIR call
  MerklePath path;
  // Layerwise direct-return levels: served in the clear, no PIR call.
  size_t direct_levels = 0;
  uint64_t plain_response_bytes = 0;
  PirPipelineProfile pipeline;
};

void validate_trial_plan(const MerkleWorkload &workload,
                         const PirParams &reference,
                         const BenchmarkTrialPlan &trials) {
  const size_t target_num_pt = flat_target_num_pt(workload);
  if (reference.get_target_num_pt() != target_num_pt) {
    throw std::invalid_argument(
        "Reference layout target does not match the flat Merkle database");
  }
  if (reference.get_pt_size() != 96 * sizeof(MerkleNode)) {
    throw std::invalid_argument(
        "Merkle benchmark requires 96 nodes per v2 plaintext");
  }
  if (trials.measured_leaf_indices.empty()) {
    throw std::invalid_argument("At least one measured trial is required");
  }
  auto validate_leaf = [&](size_t leaf) {
    if (leaf >= workload.leaf_count) {
      throw std::invalid_argument("Benchmark leaf index is out of range");
    }
  };
  for (size_t leaf : trials.warmup_leaf_indices) validate_leaf(leaf);
  for (size_t leaf : trials.measured_leaf_indices) validate_leaf(leaf);
}

RlwePt standard_plaintext(size_t index, const PirParams &params) {
  RlwePt plaintext;
  plaintext.data.resize(params.get_poly_degree());
  for (size_t coefficient = 0; coefficient < plaintext.data.size();
       ++coefficient) {
    plaintext.data[coefficient] =
        (17 * index + 3 * coefficient + 11) % params.get_plain_mod();
  }
  return plaintext;
}

PirCallOutput timed_pir_call(PirClient &client,
                             const PirParams &query_params,
                             PirServer &server, size_t client_id,
                             size_t plaintext_index,
                             std::optional<size_t> node_offset) {
  PirCallOutput output;

  // All cases use identical timer boundaries. Callers perform oracle/path
  // comparison after this function so correctness work is outside the timer.
  auto start = BenchmarkClock::now();
  RlweCt query =
      client.fast_generate_query(query_params, plaintext_index);
  output.timing.client_query = elapsed_since(start);

  start = BenchmarkClock::now();
  RlweCt response =
      server.make_query_profiled(client_id, query, &output.pipeline);
  output.timing.server_compute = elapsed_since(start);

  std::stringstream response_stream;
  start = BenchmarkClock::now();
  output.response_bytes =
      server.save_resp_to_stream(response, response_stream);
  output.timing.response_serialize = elapsed_since(start);

  start = BenchmarkClock::now();
  RlweCt reconstructed = client.load_resp_from_stream(response_stream);
  output.plaintext = client.decrypt_mod_q(reconstructed);
  if (node_offset.has_value()) {
    output.node =
        decode_merkle_node(output.plaintext, *node_offset, query_params);
  }
  output.timing.response_load_decrypt_extract = elapsed_since(start);
  return output;
}

void add_online_timing(TrialTiming &total, const TrialTiming &sample) {
  total.client_query += sample.client_query;
  total.server_compute += sample.server_compute;
  total.response_serialize += sample.response_serialize;
  total.response_load_decrypt_extract +=
      sample.response_load_decrypt_extract;
}

TrialTiming average_online_timing(const TrialTiming &total, size_t count) {
  if (count == 0) {
    throw std::invalid_argument("Cannot average zero benchmark trials");
  }
  TrialTiming average;
  average.client_query = total.client_query / count;
  average.server_compute = total.server_compute / count;
  average.response_serialize = total.response_serialize / count;
  average.response_load_decrypt_extract =
      total.response_load_decrypt_extract / count;
  return average;
}

uint64_t raw_merkle_bytes(const MerkleWorkload &workload) {
  const uint64_t nodes = 2 * (static_cast<uint64_t>(workload.leaf_count) - 1);
  return checked_multiply(nodes, workload.node_bytes,
                          "raw Merkle database bytes");
}

void populate_reference_database_metadata(BenchmarkCaseResult &result,
                                          const MerkleWorkload &workload,
                                          const PirParams &reference) {
  const uint64_t target_num_pt = flat_target_num_pt(workload);
  const uint64_t plaintext_bytes = reference.get_pt_size();
  result.raw_dataset_bytes = raw_merkle_bytes(workload);
  result.paper_plaintext_database_bytes = checked_multiply(
      target_num_pt, plaintext_bytes, "paper plaintext database bytes");
  result.logical_padded_database_bytes = checked_multiply(
      reference.get_num_pt(), plaintext_bytes,
      "logical padded database bytes");
  result.physical_preprocessed_storage_bytes =
      physical_database_bytes(reference);
}

void require_response_shape(const std::vector<size_t> &expected,
                            const std::vector<size_t> &actual) {
  if (expected != actual) {
    throw std::runtime_error(
        "Actual response byte count changed between measured trials");
  }
}

}  // namespace

// Reads the server phase breakdown out of the process-wide TimerLogger.
// make_query brackets expand / convert / first_dim / other_dim / mod_switch;
// the logger sums repeated sections until END_EXPERIMENT(), so one experiment
// per trial gives per-trial phase totals even when a trial issues H PIR calls.
// Warm-up trials must be flushed too, or they would leak into the first
// measured experiment.
struct ServerPhaseAccumulator {
  static constexpr std::array<std::pair<const char *, const char *>, 5>
      kPhases{{{"expand", EXPAND_TIME},
               {"convert", CONVERT_TIME},
               {"first_dim", FST_DIM_TIME},
               {"other_dim", OTHER_DIM_TIME},
               {"mod_switch", MOD_SWITCH}}};

  std::map<std::string, double> sums;
  size_t measured = 0;

  ServerPhaseAccumulator() { CLEAN_TIMER(); }

  void end_trial(bool is_measured) {
    END_EXPERIMENT();
    if (!is_measured) return;
    for (const auto &[name, key] : kPhases) sums[name] += GET_LAST_TIME(key);
    ++measured;
  }

  std::map<std::string, double> averages() const {
    std::map<std::string, double> out;
    for (const auto &[name, key] : kPhases) {
      const auto it = sums.find(name);
      out[name] = (measured == 0 || it == sums.end())
                      ? 0.0
                      : it->second / static_cast<double>(measured);
    }
    return out;
  }
};

std::map<std::string, double> pipeline_profile_average(
    const PirPipelineProfile &total, size_t trials) {
  const auto ms = [&](std::chrono::nanoseconds d) {
    return trials == 0 ? 0.0 : milliseconds(d) / static_cast<double>(trials);
  };
  return {{"expand", ms(total.expand)},
          {"convert", ms(total.convert)},
          {"first_dim_query_ntt", ms(total.first_dim_query_ntt)},
          {"first_dim_query_pack", ms(total.first_dim_query_pack)},
          {"first_dim_core", ms(total.first_dim_core)},
          {"first_dim_finalize", ms(total.first_dim_finalize)},
          {"other_dim", ms(total.other_dim)},
          {"mod_switch", ms(total.mod_switch)}};
}

PipelineProfileSample pipeline_profile_sample(size_t leaf,
                                              const PirPipelineProfile &sample) {
  PipelineProfileSample output;
  output.leaf = leaf;
  output.stages = pipeline_profile_average(sample, 1);
  return output;
}

BenchmarkCaseExecution run_standard_case(
    const MerkleWorkload &workload, const PirParams &reference,
    const BenchmarkTrialPlan &trials) {
  validate_trial_plan(workload, reference, trials);

  const auto setup_start = BenchmarkClock::now();
  PirClient client(reference);
  SharedPirSessionKeys keys = client.create_session_keys();
  PirServer server(reference);
  server.set_client_session_keys(client.get_client_id(), keys);
  PlaintextSource source = [&](size_t index, RlwePt &out) {
    out = standard_plaintext(index, reference);
  };
  server.load_data(reference.get_target_num_pt(), source);
  const BenchmarkDuration setup_time = elapsed_since(setup_start);

  auto run_trial = [&](size_t leaf) {
    const size_t plaintext_index =
        leaf % reference.get_target_num_pt();
    PirCallOutput output = timed_pir_call(
        client, reference, server, client.get_client_id(), plaintext_index,
        std::nullopt);
    const RlwePt expected = standard_plaintext(plaintext_index, reference);
    if (!utils::plaintext_is_equal(output.plaintext, expected)) {
      throw std::runtime_error("Standard OnionPIR deterministic mismatch");
    }
    return output;
  };

  ServerPhaseAccumulator phases;
  for (size_t leaf : trials.warmup_leaf_indices) {
    (void)run_trial(leaf);
    phases.end_trial(false);
  }

  TrialTiming timing_total;
  std::vector<size_t> response_shape;
  std::vector<double> server_compute_samples_ms;
  server_compute_samples_ms.reserve(trials.measured_leaf_indices.size());
  PirPipelineProfile pipeline_total;
  BenchmarkCaseExecution execution;
  BenchmarkCaseResult &result = execution.result;
  for (size_t leaf : trials.measured_leaf_indices) {
    PirCallOutput output = run_trial(leaf);
    phases.end_trial(true);
    pipeline_total += output.pipeline;
    add_online_timing(timing_total, output.timing);
    result.sample_leaf_indices.push_back(static_cast<uint64_t>(leaf));
    server_compute_samples_ms.push_back(
        milliseconds(output.timing.server_compute));
    result.client_query_samples_ms.push_back(
        milliseconds(output.timing.client_query));
    result.response_serialize_samples_ms.push_back(
        milliseconds(output.timing.response_serialize));
    result.response_load_decrypt_extract_samples_ms.push_back(
        milliseconds(output.timing.response_load_decrypt_extract));
    result.pipeline_profile_samples_ms.push_back(
        pipeline_profile_sample(leaf, output.pipeline));
    const std::vector<size_t> current_shape{output.response_bytes};
    if (response_shape.empty()) {
      response_shape = current_shape;
    } else {
      require_response_shape(response_shape, current_shape);
    }
  }

  result.name = "standard_onionpir";
  result.correctness_passed = true;
  populate_reference_database_metadata(result, workload, reference);
  result.paper_plaintext_scan_bytes =
      result.paper_plaintext_database_bytes;
  result.logical_padded_scan_bytes =
      result.logical_padded_database_bytes;
  result.raw_application_scan_bytes =
      result.paper_plaintext_database_bytes;
  result.useful_response_bytes = reference.get_pt_size();
  result.timing = average_online_timing(
      timing_total, trials.measured_leaf_indices.size());
  result.timing.setup = setup_time;
  result.communication = communication_stats(reference, 1, response_shape);
  result.server_compute_samples_ms =
      std::move(server_compute_samples_ms);
  result.server_phase_ms = phases.averages();
  result.pipeline_profile_ms = pipeline_profile_average(
      pipeline_total, trials.measured_leaf_indices.size());
  finalize_case_statistics(result);
  return execution;
}

BenchmarkCaseExecution run_merkle_flat_case(
    const MerkleWorkload &workload, const PirParams &reference,
    const BenchmarkTrialPlan &trials) {
  validate_trial_plan(workload, reference, trials);

  const auto setup_start = BenchmarkClock::now();
  PirClient client(reference);
  SharedPirSessionKeys keys = client.create_session_keys();
  PirServer server(reference);
  server.set_client_session_keys(client.get_client_id(), keys);
  PlaintextSource source = [&](size_t index, RlwePt &out) {
    out = make_flat_merkle_plaintext(index, workload, reference);
  };
  server.load_data(reference.get_target_num_pt(), source);
  const BenchmarkDuration setup_time = elapsed_since(setup_start);

  auto run_trial = [&](size_t leaf) {
    PathTrialOutput trial;
    trial.response_bytes.reserve(workload.tree_height);
    trial.path.reserve(workload.tree_height);
    // One independent PIR over the complete flat tree per path level; only the
    // selected breadth-first ordinal changes between calls.
    for (size_t level = workload.tree_height; level >= 1; --level) {
      const size_t local =
          merkle_sibling_local(leaf, workload.tree_height, level);
      const size_t ordinal = merkle_flat_ordinal(level, local);
      const size_t plaintext_index = ordinal / 96;
      const size_t node_offset = ordinal % 96;
      PirCallOutput output = timed_pir_call(
          client, reference, server, client.get_client_id(), plaintext_index,
          node_offset);
      add_online_timing(trial.timing, output.timing);
      trial.pipeline += output.pipeline;
      trial.response_bytes.push_back(output.response_bytes);
      const MerkleNode expected = synthetic_merkle_node(level, local);
      if (!output.node.has_value() || *output.node != expected) {
        throw std::runtime_error("Flat Merkle PIR node mismatch");
      }
      trial.path.push_back(*output.node);
      if (level == 1) break;
    }
    return trial;
  };

  ServerPhaseAccumulator phases;
  for (size_t leaf : trials.warmup_leaf_indices) {
    (void)run_trial(leaf);
    phases.end_trial(false);
  }

  TrialTiming timing_total;
  std::vector<size_t> response_shape;
  BenchmarkCaseExecution execution;
  PirPipelineProfile pipeline_total;
  for (size_t leaf : trials.measured_leaf_indices) {
    PathTrialOutput trial = run_trial(leaf);
    phases.end_trial(true);
    pipeline_total += trial.pipeline;
    add_online_timing(timing_total, trial.timing);
    execution.result.sample_leaf_indices.push_back(
        static_cast<uint64_t>(leaf));
    execution.result.server_compute_samples_ms.push_back(
        milliseconds(trial.timing.server_compute));
    execution.result.client_query_samples_ms.push_back(
        milliseconds(trial.timing.client_query));
    execution.result.response_serialize_samples_ms.push_back(
        milliseconds(trial.timing.response_serialize));
    execution.result.response_load_decrypt_extract_samples_ms.push_back(
        milliseconds(trial.timing.response_load_decrypt_extract));
    execution.result.pipeline_profile_samples_ms.push_back(
        pipeline_profile_sample(leaf, trial.pipeline));
    if (response_shape.empty()) {
      response_shape = trial.response_bytes;
    } else {
      require_response_shape(response_shape, trial.response_bytes);
    }
    execution.measured_paths.push_back(std::move(trial.path));
  }

  BenchmarkCaseResult &result = execution.result;
  result.name = "merkle_flat";
  result.correctness_passed = true;
  populate_reference_database_metadata(result, workload, reference);
  // The database footprint remains one flat tree for paper throughput. Scan
  // diagnostics multiply by H because the complete-path trial reads that same
  // database H times; server_compute likewise sums all H calls.
  result.paper_plaintext_scan_bytes = checked_multiply(
      result.paper_plaintext_database_bytes, workload.tree_height,
      "flat paper plaintext scan bytes");
  result.logical_padded_scan_bytes = checked_multiply(
      result.logical_padded_database_bytes, workload.tree_height,
      "flat padded scan bytes");
  result.raw_application_scan_bytes = checked_multiply(
      result.raw_dataset_bytes, workload.tree_height,
      "flat raw application scan bytes");
  result.useful_response_bytes = checked_multiply(
      workload.tree_height, workload.node_bytes,
      "flat useful response bytes");
  result.timing = average_online_timing(
      timing_total, trials.measured_leaf_indices.size());
  result.timing.setup = setup_time;
  result.server_phase_ms = phases.averages();
  result.pipeline_profile_ms = pipeline_profile_average(
      pipeline_total, trials.measured_leaf_indices.size());
  result.communication = communication_stats(
      reference, workload.tree_height, response_shape);
  finalize_case_statistics(result);
  return execution;
}

BenchmarkCaseExecution run_merkle_layerwise_case(
    const MerkleWorkload &workload, const PirParams &reference,
    const BenchmarkTrialPlan &trials) {
  return run_merkle_layerwise_case(workload, reference, trials,
                                   LayerPlannerConfig{});
}

BenchmarkCaseExecution run_merkle_layerwise_case(
    const MerkleWorkload &workload, const PirParams &reference,
    const BenchmarkTrialPlan &trials, const LayerPlannerConfig &planner) {
  validate_trial_plan(workload, reference, trials);

  const auto setup_start = BenchmarkClock::now();
  bool used_fallback = false;
  std::vector<LayerLayout> layouts =
      plan_layer_layouts(workload.tree_height, 96, reference, planner,
                         &used_fallback);
  PirClient client(reference);
  SharedPirSessionKeys keys = client.create_session_keys();
  std::vector<std::unique_ptr<PirServer>> servers;
  servers.reserve(layouts.size());
  // Each level gets its own minimal shape, but every server points to the same
  // immutable scheme-level helper-key allocation.
  // Levels that fit one plaintext are not served by PIR at all: the server
  // keeps the level's node array and hands the whole level over in the clear.
  std::vector<std::vector<uint8_t>> direct_level_bytes(layouts.size());
  for (const LayerLayout &layout : layouts) {
    if (layout.direct_return) {
      std::vector<uint8_t> &bytes = direct_level_bytes.at(layout.level - 1);
      bytes.reserve(layout.node_count * sizeof(MerkleNode));
      for (size_t local = 0; local < layout.node_count; ++local) {
        const MerkleNode node = synthetic_merkle_node(layout.level, local);
        bytes.insert(bytes.end(), node.begin(), node.end());
      }
      servers.push_back(nullptr);
      continue;
    }
    auto server = std::make_unique<PirServer>(layout.params);
    server->set_client_session_keys(client.get_client_id(), keys);
    if (server->client_session_keys(client.get_client_id()).get() != keys.get()) {
      throw std::runtime_error("Layer server copied the shared key bundle");
    }
    PlaintextSource source = [&](size_t index, RlwePt &out) {
      out = make_layer_merkle_plaintext(layout.level, index, workload,
                                        layout.params);
    };
    server->load_data(layout.target_num_pt, source);
    servers.push_back(std::move(server));
  }
  const BenchmarkDuration setup_time = elapsed_since(setup_start);

  auto run_trial = [&](size_t leaf) {
    PathTrialOutput trial;
    trial.response_bytes.reserve(workload.tree_height);
    trial.path.reserve(workload.tree_height);
    // Still H independent PIR calls, but call l contains only the 2^l nodes of
    // that level instead of another copy of the full flattened tree.
    for (size_t level = workload.tree_height; level >= 1; --level) {
      const LayerLayout &layout = layouts.at(level - 1);
      const size_t local =
          merkle_sibling_local(leaf, workload.tree_height, level);
      const MerkleNode expected = synthetic_merkle_node(level, local);
      if (layout.direct_return) {
        // Direct return: no query; the server's "answer" is a copy of the
        // whole level, the client picks its node out of it. Both sides are
        // timed with the same boundaries as a PIR call.
        const std::vector<uint8_t> &level_bytes =
            direct_level_bytes.at(level - 1);
        auto start = BenchmarkClock::now();
        const std::vector<uint8_t> wire(level_bytes);
        trial.timing.server_compute += elapsed_since(start);
        start = BenchmarkClock::now();
        MerkleNode node;
        std::copy_n(wire.begin() + local * sizeof(MerkleNode),
                    sizeof(MerkleNode), node.begin());
        trial.timing.response_load_decrypt_extract += elapsed_since(start);
        if (node != expected) {
          throw std::runtime_error("Layerwise direct-return node mismatch");
        }
        trial.direct_levels += 1;
        trial.plain_response_bytes += wire.size();
        trial.path.push_back(node);
      } else {
        const size_t plaintext_index = local / 96;
        const size_t node_offset = local % 96;
        PirCallOutput output = timed_pir_call(
            client, layout.params, *servers.at(level - 1),
            client.get_client_id(), plaintext_index, node_offset);
        add_online_timing(trial.timing, output.timing);
        trial.pipeline += output.pipeline;
        trial.response_bytes.push_back(output.response_bytes);
        if (!output.node.has_value() || *output.node != expected) {
          throw std::runtime_error("Layerwise Merkle PIR node mismatch");
        }
        trial.path.push_back(*output.node);
      }
      if (level == 1) break;
    }
    return trial;
  };

  ServerPhaseAccumulator phases;
  for (size_t leaf : trials.warmup_leaf_indices) {
    (void)run_trial(leaf);
    phases.end_trial(false);
  }

  TrialTiming timing_total;
  std::vector<size_t> response_shape;
  BenchmarkCaseExecution execution;
  PirPipelineProfile pipeline_total;
  for (size_t leaf : trials.measured_leaf_indices) {
    PathTrialOutput trial = run_trial(leaf);
    phases.end_trial(true);
    pipeline_total += trial.pipeline;
    add_online_timing(timing_total, trial.timing);
    execution.result.sample_leaf_indices.push_back(
        static_cast<uint64_t>(leaf));
    execution.result.server_compute_samples_ms.push_back(
        milliseconds(trial.timing.server_compute));
    execution.result.client_query_samples_ms.push_back(
        milliseconds(trial.timing.client_query));
    execution.result.response_serialize_samples_ms.push_back(
        milliseconds(trial.timing.response_serialize));
    execution.result.response_load_decrypt_extract_samples_ms.push_back(
        milliseconds(trial.timing.response_load_decrypt_extract));
    execution.result.pipeline_profile_samples_ms.push_back(
        pipeline_profile_sample(leaf, trial.pipeline));
    if (response_shape.empty()) {
      response_shape = trial.response_bytes;
    } else {
      require_response_shape(response_shape, trial.response_bytes);
    }
    execution.measured_paths.push_back(std::move(trial.path));
  }

  BenchmarkCaseResult &result = execution.result;
  result.name = "merkle_layerwise";
  result.correctness_passed = true;
  result.raw_dataset_bytes = raw_merkle_bytes(workload);
  size_t direct_levels = 0;
  uint64_t direct_plain_bytes = 0;
  for (const LayerLayout &layout : layouts) {
    if (!layout.direct_return) continue;
    direct_levels += 1;
    direct_plain_bytes = checked_add(
        direct_plain_bytes,
        checked_multiply(layout.node_count, workload.node_bytes,
                         "direct-return level bytes"),
        "direct-return level total");
  }
  result.direct_return_levels = direct_levels;
  result.direct_return_response_bytes = direct_plain_bytes;
  // The stored plaintext database is the sum of all disjoint level databases.
  // Each is scanned once per path, so database and scan bytes coincide here.
  // Direct-return levels are stored and sent as raw node arrays, so they
  // count their raw bytes in every column.
  for (const LayerLayout &layout : layouts) {
    if (layout.direct_return) {
      const uint64_t raw = checked_multiply(
          layout.node_count, workload.node_bytes, "direct level raw bytes");
      result.paper_plaintext_database_bytes = checked_add(
          result.paper_plaintext_database_bytes, raw,
          "layer paper plaintext database total");
      result.logical_padded_database_bytes = checked_add(
          result.logical_padded_database_bytes, raw,
          "layer padded database total");
      result.physical_preprocessed_storage_bytes = checked_add(
          result.physical_preprocessed_storage_bytes, raw,
          "layer physical database total");
      result.raw_application_scan_bytes = checked_add(
          result.raw_application_scan_bytes, raw,
          "layer raw application total");
      continue;
    }
    result.paper_plaintext_database_bytes = checked_add(
        result.paper_plaintext_database_bytes,
        checked_multiply(layout.target_num_pt, layout.params.get_pt_size(),
                         "layer paper plaintext database bytes"),
        "layer paper plaintext database total");
    result.logical_padded_database_bytes = checked_add(
        result.logical_padded_database_bytes,
        checked_multiply(layout.params.get_num_pt(),
                         layout.params.get_pt_size(),
                         "layer padded database bytes"),
        "layer padded database total");
    result.physical_preprocessed_storage_bytes = checked_add(
        result.physical_preprocessed_storage_bytes,
        physical_database_bytes(layout.params),
        "layer physical database total");
    result.raw_application_scan_bytes = checked_add(
        result.raw_application_scan_bytes,
        checked_multiply(layout.node_count, workload.node_bytes,
                         "layer raw application bytes"),
        "layer raw application total");
  }
  result.paper_plaintext_scan_bytes =
      result.paper_plaintext_database_bytes;
  result.logical_padded_scan_bytes =
      result.logical_padded_database_bytes;
  result.useful_response_bytes = checked_multiply(
      workload.tree_height, workload.node_bytes,
      "layer useful response bytes");
  result.timing = average_online_timing(
      timing_total, trials.measured_leaf_indices.size());
  result.timing.setup = setup_time;
  result.server_phase_ms = phases.averages();
  result.pipeline_profile_ms = pipeline_profile_average(
      pipeline_total, trials.measured_leaf_indices.size());
  // Record the policy that actually produced the layouts: a profiled request
  // that fell back through allow_profile_fallback ran the legacy plan.
  result.layer_layout_policy = layer_layout_policy_name(
      used_fallback ? LayerLayoutPolicy::legacy_padding : planner.policy);
  for (const LayerLayout &layout : layouts) {
    LayerLayoutRecord record;
    record.level = layout.level;
    record.direct_return = layout.direct_return;
    record.features = compute_layer_layout_features(layout.params);
    result.layers.push_back(std::move(record));
  }
  result.communication = communication_stats(
      reference, workload.tree_height - direct_levels, response_shape,
      direct_plain_bytes);
  finalize_case_statistics(result);
  return execution;
}
