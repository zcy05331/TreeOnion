#include "merkle_benchmark_internal.h"

#include "client.h"
#include "layer_layout_planner.h"
#include "logging.h"
#include "server.h"
#include "tree_packing.h"
#include "tree_rotate.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <iterator>
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
          {"response_rotate", ms(total.response_rotate)},
          {"response_merge", ms(total.response_merge)},
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

// ============================================================================
// Layerwise with the tree scheme's response merging, optionally packed queries
// ============================================================================
//
// Same independent per-level OnionPIR calls as merkle_layerwise, but every
// level is stored in strided 64-slot plaintexts (merkle_baseline.h) so the
// level result can be treated like one tree-scheme item: the server rotates
// it privately by X^{-gamma} with a gamma selector set the client sends once
// per path (gamma = the six leading leaf bits, the slot of every served level),
// and merges all level results into one response ciphertext with
// tree_packing::mlwe_merge_residues, which also projects away the 63 other
// slots. One modulus switch and one serialized ciphertext follow. Levels that
// fit one plaintext are still returned in the clear.
//
// merkle_layerwise_packed additionally packs the queries: after expansion
// every slot is an independent ciphertext, so the gamma rows and every served
// level get a contiguous run of slots in a shared height-11 (W = 2048)
// expansion, [o, o + N0 + L_EP * d) per level, placed in order with a new
// ciphertext whenever the next run would cross W. Each run keeps its own
// content (the level's one-hot plus the gadget rows of its row bits); only
// BitRev and the W^-1 premultiplication use W. The server expands every
// packed ciphertext once, pruning right of its last used slot, and hands run
// l to level l; first dimension, fold, rotation and merge are unchanged.

namespace {

BenchmarkCaseExecution run_strided_layerwise_case(
    const MerkleWorkload &workload, const PirParams &reference,
    const BenchmarkTrialPlan &trials, const LayerPlannerConfig &planner,
    bool packed_queries) {
  validate_trial_plan(workload, reference, trials);
  constexpr size_t kRho = kStridedNodesPerPlaintext;
  constexpr size_t kGammaBits = kStridedLevelSlotBits;
  constexpr size_t N = DBConsts::PolyDegree;
  constexpr size_t kPackHeight = std::bit_width(N) - 1;  // W = N slots
  if (workload.tree_height <= kGammaBits) {
    throw std::invalid_argument(
        "Merged layerwise needs a tree taller than the 64-node slot height");
  }
  // Expansion view for the shared gamma selectors: one first-dimension slot
  // plus kGammaBits gadget row groups, in the smallest capacity that fits.
  size_t gamma_height = 0;
  while ((size_t{1} << gamma_height) < 1 + kGammaBits * reference.get_l()) {
    ++gamma_height;
  }
  const PirParams gamma_params =
      reference.with_query_shape({1, kGammaBits, gamma_height});
  // The BV Galois keys must cover the gamma expansion and the merge
  // automorphisms (N >> v) + 1, v < kGammaBits, even when a small reference
  // expands lower than that; production references (h = 10) already do.
  // Packed queries expand to the full W = 2^11.
  const size_t key_height =
      std::max({reference.get_expan_height(), gamma_height, kGammaBits,
                packed_queries ? kPackHeight : size_t{0}});

  const auto setup_start = BenchmarkClock::now();
  bool used_fallback = false;
  std::vector<LayerLayout> layouts =
      plan_layer_layouts(workload.tree_height, kRho, reference, planner,
                         &used_fallback);
  PirClient client(reference);
  SharedPirSessionKeys keys = client.create_session_keys(key_height);
  for (size_t v = 0; v < kGammaBits; ++v) {
    if (!keys->bv_galois_keys.contains(static_cast<uint32_t>((N >> v) + 1))) {
      throw std::runtime_error(
          "session keys do not cover the merge automorphisms");
    }
  }
  PirServer gamma_server(gamma_params);
  gamma_server.set_client_session_keys(client.get_client_id(), keys);
  std::vector<std::unique_ptr<PirServer>> servers;
  servers.reserve(layouts.size());
  std::vector<std::vector<uint8_t>> direct_level_bytes(layouts.size());
  PirServer *response_server = nullptr;
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
    PlaintextSource source = [&](size_t index, RlwePt &out) {
      out = make_layer_merkle_plaintext_strided(layout.level, index, workload,
                                                layout.params);
    };
    server->load_data(layout.target_num_pt, source);
    if (response_server == nullptr) response_server = server.get();
    servers.push_back(std::move(server));
  }
  if (response_server == nullptr) {
    throw std::runtime_error("Merged layerwise has no PIR-served level");
  }

  // Packed query geometry, fixed by the public database shape: the gamma rows
  // first (in gamma_params' layout: one unused first-dimension slot), then the
  // served levels from the leaves upward. Level 0 stands for gamma.
  struct QuerySegment {
    size_t level = 0;
    size_t size = 0;
    QuerySegmentPlacement at;
  };
  std::vector<QuerySegment> segments;
  std::vector<size_t> packed_used;  // leaves each packed ciphertext expands to
  if (packed_queries) {
    segments.push_back({0, 1 + kGammaBits * reference.get_l(), {}});
    for (size_t level = workload.tree_height; level >= 1; --level) {
      const LayerLayout &layout = layouts.at(level - 1);
      if (!layout.direct_return) {
        const PirParams &p = layout.params;
        segments.push_back(
            {level, p.get_fst_dim_sz() + p.get_l() * p.get_num_other_dims(), {}});
      }
      if (level == 1) break;
    }
    std::vector<size_t> sizes;
    for (const QuerySegment &segment : segments) sizes.push_back(segment.size);
    const std::vector<QuerySegmentPlacement> placements =
        pack_query_segments(sizes, size_t{1} << kPackHeight);
    packed_used.assign(placements.back().ciphertext + 1, 0);
    for (size_t i = 0; i < segments.size(); ++i) {
      segments[i].at = placements[i];
      packed_used[placements[i].ciphertext] =
          placements[i].offset + segments[i].size;
    }
  }
  const BenchmarkDuration setup_time = elapsed_since(setup_start);

  using ProfileClock = std::chrono::steady_clock;
  auto run_trial = [&](size_t leaf) {
    PathTrialOutput trial;
    trial.path.reserve(workload.tree_height);
    std::vector<MerkleNode> by_level(workload.tree_height + 1);
    const size_t client_id = client.get_client_id();
    const size_t gamma = leaf >> (workload.tree_height - kGammaBits);
    ProfileClock::time_point mark;
    const auto lap = [&](std::chrono::nanoseconds PirPipelineProfile::*slot) {
      const auto now = ProfileClock::now();
      trial.pipeline.*slot += now - mark;
      mark = now;
    };
    std::vector<size_t> gamma_indices{0};
    for (size_t v = 0; v < kGammaBits; ++v) {
      gamma_indices.push_back((gamma >> v) & 1U);
    }
    const auto level_plaintext = [&](size_t level) {
      const size_t local =
          merkle_sibling_local(leaf, workload.tree_height, level);
      if (strided_level_slot(level, local) != gamma) {
        throw std::logic_error(
            "strided slot of a served level differs from the shared gamma");
      }
      return strided_level_plaintext(level, local);
    };

    // Client queries, then their expansion on the server: the gamma rows and
    // each served level's expanded slots (first dimension, then row groups).
    std::vector<RlweCt> gamma_rows;
    std::vector<std::vector<RlweCt>> level_rows(workload.tree_height + 1);
    if (!packed_queries) {
      // One gamma selector query per path, expanded and completed like the
      // selector rows of an ordinary packed query, and one query per level.
      auto start = BenchmarkClock::now();
      RlweCt gamma_query = client.fresh_zero_ct();
      client.add_gsw_to_query(gamma_params, gamma_query, gamma_indices);
      std::vector<RlweCt> level_queries(workload.tree_height + 1);
      for (size_t level = workload.tree_height; level >= 1; --level) {
        const LayerLayout &layout = layouts.at(level - 1);
        if (!layout.direct_return) {
          level_queries[level] =
              client.fast_generate_query(layout.params, level_plaintext(level));
        }
        if (level == 1) break;
      }
      trial.timing.client_query += elapsed_since(start);
      start = BenchmarkClock::now();
      mark = ProfileClock::now();
      TIME_START(EXPAND_TIME);
      gamma_rows = gamma_server.expand_query(client_id, gamma_query);
      for (size_t level = workload.tree_height; level >= 1; --level) {
        if (!layouts.at(level - 1).direct_return) {
          level_rows[level] = servers.at(level - 1)->expand_query(
              client_id, level_queries[level]);
        }
        if (level == 1) break;
      }
      TIME_END(EXPAND_TIME);
      lap(&PirPipelineProfile::expand);
      trial.timing.server_compute += elapsed_since(start);
    } else {
      auto start = BenchmarkClock::now();
      std::vector<RlweCt> packed(packed_used.size());
      for (RlweCt &ct : packed) ct = client.fresh_zero_ct();
      for (const QuerySegment &segment : segments) {
        RlweCt &ct = packed.at(segment.at.ciphertext);
        if (segment.level == 0) {
          client.add_query_segment(ct, kPackHeight, segment.at.offset,
                                   gamma_params, gamma_indices,
                                   /*one_hot=*/false);
          continue;
        }
        const PirParams &params = layouts.at(segment.level - 1).params;
        const size_t plaintext_index = level_plaintext(segment.level);
        if (plaintext_index >= params.get_num_pt()) {
          throw std::out_of_range("level plaintext exceeds its layout");
        }
        client.add_query_segment(
            ct, kPackHeight, segment.at.offset, params,
            client.get_query_indices(params, plaintext_index),
            /*one_hot=*/true);
      }
      trial.timing.client_query += elapsed_since(start);
      start = BenchmarkClock::now();
      mark = ProfileClock::now();
      TIME_START(EXPAND_TIME);
      std::vector<std::vector<RlweCt>> leaves(packed.size());
      for (size_t c = 0; c < packed.size(); ++c) {
        leaves[c] = gamma_server.expand_query(client_id, packed[c], kPackHeight,
                                              packed_used[c]);
      }
      for (const QuerySegment &segment : segments) {
        std::vector<RlweCt> &source = leaves[segment.at.ciphertext];
        std::vector<RlweCt> &target =
            segment.level == 0 ? gamma_rows : level_rows[segment.level];
        target.assign(
            std::make_move_iterator(source.begin() + segment.at.offset),
            std::make_move_iterator(source.begin() + segment.at.offset +
                                    segment.size));
      }
      TIME_END(EXPAND_TIME);
      lap(&PirPipelineProfile::expand);
      trial.timing.server_compute += elapsed_since(start);
    }

    auto start = BenchmarkClock::now();
    mark = ProfileClock::now();
    TIME_START(CONVERT_TIME);
    std::vector<GSWCt> gamma_selectors =
        gamma_server.complete_selectors(client_id, gamma_rows);
    TIME_END(CONVERT_TIME);
    lap(&PirPipelineProfile::convert);
    trial.timing.server_compute += elapsed_since(start);

    std::vector<RlweCt> items;
    std::vector<size_t> item_levels;
    for (size_t level = workload.tree_height; level >= 1; --level) {
      const LayerLayout &layout = layouts.at(level - 1);
      const size_t local =
          merkle_sibling_local(leaf, workload.tree_height, level);
      if (layout.direct_return) {
        const std::vector<uint8_t> &level_bytes =
            direct_level_bytes.at(level - 1);
        start = BenchmarkClock::now();
        const std::vector<uint8_t> wire(level_bytes);
        trial.timing.server_compute += elapsed_since(start);
        start = BenchmarkClock::now();
        MerkleNode node;
        std::copy_n(wire.begin() + local * sizeof(MerkleNode),
                    sizeof(MerkleNode), node.begin());
        trial.timing.response_load_decrypt_extract += elapsed_since(start);
        by_level[level] = node;
        trial.direct_levels += 1;
        trial.plain_response_bytes += wire.size();
      } else {
        PirServer &server = *servers.at(level - 1);
        std::vector<RlweCt> &expanded = level_rows[level];
        start = BenchmarkClock::now();
        mark = ProfileClock::now();
        TIME_START(CONVERT_TIME);
        std::vector<GSWCt> selectors =
            server.complete_selectors(client_id, expanded);
        TIME_END(CONVERT_TIME);
        lap(&PirPipelineProfile::convert);
        TIME_START(FST_DIM_TIME);
        expanded.resize(layout.params.get_fst_dim_sz());
        std::vector<RlweCt> candidates =
            server.evaluate_first_dim(expanded, &trial.pipeline);
        TIME_END(FST_DIM_TIME);
        mark = ProfileClock::now();  // first-dim sub-stages recorded inside
        TIME_START(OTHER_DIM_TIME);
        RlweCt selected = server.evaluate_other_dim(candidates, selectors);
        TIME_END(OTHER_DIM_TIME);
        lap(&PirPipelineProfile::other_dim);
        // X^{-gamma}: bit v of gamma selects a shift by 2^v.
        for (size_t v = 0; v < kGammaBits; ++v) {
          selected = rot_select(selected, gamma_selectors[v], size_t{1} << v,
                                server, reference);
        }
        lap(&PirPipelineProfile::response_rotate);
        trial.timing.server_compute += elapsed_since(start);
        items.push_back(std::move(selected));
        item_levels.push_back(level);
      }
      if (level == 1) break;
    }

    // Merge every served level into one ciphertext, then one modulus switch.
    start = BenchmarkClock::now();
    mark = ProfileClock::now();
    RlweCt merged = tree_packing::mlwe_merge_residues(
        std::move(items), kRho, keys->bv_galois_keys, reference);
    lap(&PirPipelineProfile::response_merge);
    TIME_START(MOD_SWITCH);
    response_server->switch_response_to_small_q(merged);
    TIME_END(MOD_SWITCH);
    lap(&PirPipelineProfile::mod_switch);
    trial.timing.server_compute += elapsed_since(start);

    std::stringstream wire;
    start = BenchmarkClock::now();
    const size_t merged_bytes = response_server->save_resp_to_stream(merged, wire);
    trial.timing.response_serialize += elapsed_since(start);

    start = BenchmarkClock::now();
    RlweCt reconstructed = client.load_resp_from_stream(wire);
    const RlwePt plaintext = client.decrypt_mod_q(reconstructed);
    for (size_t i = 0; i < item_levels.size(); ++i) {
      const size_t offset = tree_packing::packed_residue_offset(
          i, kRho, tree_packing::ResponsePacking::MlweMerge);
      by_level[item_levels[i]] =
          decode_merkle_node_strided(plaintext, offset, reference);
    }
    trial.timing.response_load_decrypt_extract += elapsed_since(start);

    // Oracle comparison outside every timer, root-first like the other cases.
    for (size_t level = workload.tree_height; level >= 1; --level) {
      const size_t local =
          merkle_sibling_local(leaf, workload.tree_height, level);
      if (by_level[level] != synthetic_merkle_node(level, local)) {
        throw std::runtime_error("Merged layerwise Merkle node mismatch");
      }
      trial.path.push_back(by_level[level]);
      if (level == 1) break;
    }
    trial.response_bytes = {merged_bytes};
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
  result.name = packed_queries ? "merkle_layerwise_packed" : "merkle_layerwise_merged";
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
  result.paper_plaintext_scan_bytes = result.paper_plaintext_database_bytes;
  result.logical_padded_scan_bytes = result.logical_padded_database_bytes;
  result.useful_response_bytes = checked_multiply(
      workload.tree_height, workload.node_bytes,
      "layer useful response bytes");
  result.timing = average_online_timing(
      timing_total, trials.measured_leaf_indices.size());
  result.timing.setup = setup_time;
  result.server_phase_ms = phases.averages();
  result.pipeline_profile_ms = pipeline_profile_average(
      pipeline_total, trials.measured_leaf_indices.size());
  result.layer_layout_policy = layer_layout_policy_name(
      used_fallback ? LayerLayoutPolicy::legacy_padding : planner.policy);
  for (const LayerLayout &layout : layouts) {
    LayerLayoutRecord record;
    record.level = layout.level;
    record.direct_return = layout.direct_return;
    record.features = compute_layer_layout_features(layout.params);
    result.layers.push_back(std::move(record));
  }
  // Merged: every served level costs one packed query, plus the shared gamma
  // query. Packed: the packed ciphertexts, and helper keys for the full W
  // expansion. The response is one merged ciphertext plus the direct bytes.
  result.communication =
      packed_queries
          ? communication_stats_merged(
                reference.with_query_shape({1, 0, key_height}),
                packed_used.size(), response_shape.at(0), direct_plain_bytes)
          : communication_stats_merged(
                reference, workload.tree_height - direct_levels + 1,
                response_shape.at(0), direct_plain_bytes);
  finalize_case_statistics(result);
  return execution;
}

}  // namespace

BenchmarkCaseExecution run_merkle_layerwise_merged_case(
    const MerkleWorkload &workload, const PirParams &reference,
    const BenchmarkTrialPlan &trials) {
  return run_merkle_layerwise_merged_case(workload, reference, trials,
                                          LayerPlannerConfig{});
}

BenchmarkCaseExecution run_merkle_layerwise_merged_case(
    const MerkleWorkload &workload, const PirParams &reference,
    const BenchmarkTrialPlan &trials, const LayerPlannerConfig &planner) {
  return run_strided_layerwise_case(workload, reference, trials, planner,
                                    /*packed_queries=*/false);
}

BenchmarkCaseExecution run_merkle_layerwise_packed_case(
    const MerkleWorkload &workload, const PirParams &reference,
    const BenchmarkTrialPlan &trials) {
  return run_merkle_layerwise_packed_case(workload, reference, trials,
                                          LayerPlannerConfig{});
}

BenchmarkCaseExecution run_merkle_layerwise_packed_case(
    const MerkleWorkload &workload, const PirParams &reference,
    const BenchmarkTrialPlan &trials, const LayerPlannerConfig &planner) {
  return run_strided_layerwise_case(workload, reference, trials, planner,
                                    /*packed_queries=*/true);
}
