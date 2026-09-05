#include "tests.h"
#include "cuckoo_batch.h"
#include "merkle_baseline.h"
#include "merkle_benchmark.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Flat root-excluded heap decode: ordinal -> (level, level-local index).
std::pair<size_t, size_t> node_of_ordinal(size_t ordinal) {
  const size_t heap = ordinal + 2;
  const size_t level = std::bit_width(heap) - 1;
  return {level, heap - (size_t{1} << level)};
}

// First feasible flat reference layout, as in the baseline suite.
PirParams flat_reference(size_t target_num_pt) {
  PirParams scheme;
  for (size_t height = 0; height <= 10; ++height) {
    try {
      return scheme.with_layout({target_num_pt, height, true});
    } catch (const std::runtime_error &) {
    }
  }
  throw std::runtime_error("no flat reference layout for this workload");
}

// The k sibling ordinals of one leaf (levels H..1), the same work unit the
// flat and layerwise baselines retrieve.
std::vector<uint32_t> sibling_ordinals(size_t leaf, size_t tree_height) {
  std::vector<uint32_t> ordinals;
  ordinals.reserve(tree_height);
  for (size_t level = tree_height; level >= 1; --level) {
    const size_t local = merkle_sibling_local(leaf, tree_height, level);
    ordinals.push_back(
        static_cast<uint32_t>(merkle_flat_ordinal(level, local)));
    if (level == 1) break;
  }
  return ordinals;
}

// Plaintext source packing one bucket's sorted members with synthetic nodes.
PlaintextSource bucket_source(const CuckooBucket &bucket,
                              const PirParams &params) {
  return [&bucket, &params](size_t index, RlwePt &out) {
    const size_t first = index * 96;
    const size_t count =
        std::min(size_t{96}, bucket.members.size() - first);
    std::vector<MerkleNode> nodes;
    nodes.reserve(count);
    for (size_t offset = 0; offset < count; ++offset) {
      const auto [level, local] =
          node_of_ordinal(bucket.members[first + offset]);
      nodes.push_back(synthetic_merkle_node(level, local));
    }
    out = encode_merkle_nodes(nodes, params);
  };
}

template <typename Fn>
bool throws_invalid_argument(Fn &&fn) {
  try {
    fn();
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

double profile_ms(std::chrono::nanoseconds duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

struct CuckooTrialSample {
  size_t trial = 0;
  size_t leaf = 0;
  double bucket_routing_ms = 0.0;
  double client_query_ms = 0.0;
  double server_total_ms = 0.0;
  double response_serialize_ms = 0.0;
  double client_decode_ms = 0.0;
  PirPipelineProfile stages;
  bool correct = false;
};

void write_json_string(std::ostream &out, const std::string &value) {
  out << '"';
  for (char ch : value) {
    switch (ch) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        out << ch;
        break;
    }
  }
  out << '"';
}

void write_stage_json(std::ostream &out, const PirPipelineProfile &profile) {
  out << "{"
      << "\"expand\":" << profile_ms(profile.expand)
      << ",\"convert\":" << profile_ms(profile.convert)
      << ",\"first_dim_query_ntt\":"
      << profile_ms(profile.first_dim_query_ntt)
      << ",\"first_dim_query_pack\":"
      << profile_ms(profile.first_dim_query_pack)
      << ",\"first_dim_core\":" << profile_ms(profile.first_dim_core)
      << ",\"first_dim_finalize\":"
      << profile_ms(profile.first_dim_finalize)
      << ",\"other_dim\":" << profile_ms(profile.other_dim)
      << ",\"mod_switch\":" << profile_ms(profile.mod_switch)
      << "}";
}

void write_cuckoo_profile_json(const std::string &path, size_t leaf_count,
                               size_t tree_height, size_t warmups,
                               size_t measured_trials,
                               const CuckooBatchParams &params,
                               const CuckooStorageStats &storage,
                               uint64_t trial_seed,
                               uint64_t response_bytes,
                               uint64_t modeled_query_bytes,
                               uint64_t modeled_helper_key_bytes,
                               double setup_ms,
                               const std::vector<CuckooTrialSample> &samples) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open cuckoo benchmark JSON output");
  }
  out << std::fixed << std::setprecision(6);
  out << "{\n  \"schema_version\": \"cuckoo_raw_profile.v1\"";
  out << ",\n  \"scheme\": ";
  write_json_string(out, "cuckoo_batch_pir");
  out << ",\n  \"leaf_count\": " << leaf_count
      << ",\n  \"tree_height\": " << tree_height
      << ",\n  \"node_entry_bits\": 256"
      << ",\n  \"actual_logical_bytes\": " << params.item_count * 32ULL
      << ",\n  \"root_excluded_node_count\": " << params.item_count
      << ",\n  \"warmups\": " << warmups
      << ",\n  \"measured_trials\": " << measured_trials
      << ",\n  \"trial_seed\": " << trial_seed
      << ",\n  \"batch_size\": " << params.batch_size
      << ",\n  \"num_buckets\": " << params.num_buckets
      << ",\n  \"num_hashes\": " << params.num_hashes
      << ",\n  \"offline_setup_ms\": " << setup_ms
      << ",\n  \"setup_ms\": " << setup_ms
      << ",\n  \"stage_map\": [\"expand\", \"convert\", "
         "\"first_dim_query_ntt\", \"first_dim_query_pack\", "
         "\"first_dim_core\", \"first_dim_finalize\", \"other_dim\", "
         "\"mod_switch\"]"
      << ",\n  \"communication\": {\"modeled_query_bytes\": "
      << modeled_query_bytes << ", \"actual_response_bytes\": "
      << response_bytes << ", \"modeled_helper_key_bytes\": "
      << modeled_helper_key_bytes << "}";
  out << ",\n  \"storage\": {\"bucket_count\": " << storage.bucket_count
      << ", \"total_member_entries\": " << storage.total_member_entries
      << ", \"total_target_plaintexts\": "
      << storage.total_target_plaintexts
      << ", \"total_padded_plaintexts\": "
      << storage.total_padded_plaintexts
      << ", \"max_bucket_members\": " << storage.max_bucket_members
      << ", \"max_bucket_target_plaintexts\": "
      << storage.max_bucket_target_plaintexts
      << ", \"max_bucket_plaintexts\": " << storage.max_bucket_plaintexts
      << ", \"logical_replicated_bytes\": "
      << storage.logical_replicated_bytes
      << ", \"logical_padded_bytes\": " << storage.logical_padded_bytes
      << ", \"public_metadata_bytes\": " << storage.public_metadata_bytes
      << ", \"estimated_preprocessed_bytes\": "
      << storage.estimated_preprocessed_bytes
      << ", \"estimated_peak_resident_bytes\": "
      << storage.estimated_peak_resident_bytes << "}";
  out << ",\n  \"samples\": [";
  for (size_t i = 0; i < samples.size(); ++i) {
    const CuckooTrialSample &sample = samples[i];
    if (i) out << ',';
    out << "\n    {\"trial\": " << sample.trial
        << ", \"leaf\": " << sample.leaf
        << ", \"bucket_routing_ms\": " << sample.bucket_routing_ms
        << ", \"server_total_ms\": " << sample.server_total_ms
        << ", \"client_query_ms\": " << sample.client_query_ms
        << ", \"response_serialize_ms\": "
        << sample.response_serialize_ms
        << ", \"client_decode_ms\": " << sample.client_decode_ms
        << ", \"stages_ms\": ";
    write_stage_json(out, sample.stages);
    out << ", \"correct\": " << (sample.correct ? "true" : "false") << "}";
  }
  out << "\n  ]\n}\n";
  if (!out) {
    throw std::runtime_error("failed to write cuckoo benchmark JSON output");
  }
}

// Every ordinal placed exactly once, in one of its candidate buckets.
void require_valid_placement(
    const std::vector<std::optional<uint32_t>> &assignment,
    const std::vector<uint32_t> &ordinals, const CuckooBatchParams &params) {
  require_test(assignment.size() == params.num_buckets, "assignment size");
  std::vector<uint32_t> placed;
  for (size_t b = 0; b < assignment.size(); ++b) {
    if (!assignment[b].has_value()) continue;
    placed.push_back(*assignment[b]);
    bool candidate = false;
    for (size_t j = 0; j < params.num_hashes; ++j) {
      candidate |= cuckoo_bucket_of(*assignment[b], j, params) == b;
    }
    require_test(candidate, "assignment respects the candidate buckets");
  }
  std::vector<uint32_t> expected = ordinals;
  std::sort(placed.begin(), placed.end());
  std::sort(expected.begin(), expected.end());
  require_test(placed == expected, "every batch item is placed exactly once");
}

}  // namespace

// Correctness gate at H = 13: bucket membership, deterministic cuckoo
// placement, and a full encrypted batch round recovering every sibling.
void PirTest::test_cuckoo_batch() {
  require_applicable(paper_config_active(),
                     paper_config_reason("the cuckoo batch baseline"));
  print_func_name(__FUNCTION__);
  const size_t tree_height = 13;
  const size_t leaf_count = size_t{1} << tree_height;
  const size_t item_count = 2 * (leaf_count - 1);
  const CuckooBatchParams params =
      make_cuckoo_batch_params(item_count, tree_height, 0x63756b31ULL);
  require_test(params.num_buckets == (3 * tree_height + 1) / 2,
               "1.5x bucket sizing");

  const PirParams reference =
      flat_reference(utils::roundup_div(item_count, size_t{96}));
  std::vector<CuckooBucket> buckets =
      build_cuckoo_buckets(params, reference);

  // Membership: every sampled ordinal is present in each hashed bucket, and
  // positions are consistent with the sorted member lists.
  for (uint32_t ordinal = 0; ordinal < item_count;
       ordinal += 977) {
    for (size_t j = 0; j < params.num_hashes; ++j) {
      const CuckooBucket &bucket =
          buckets[cuckoo_bucket_of(ordinal, j, params)];
      const size_t position = cuckoo_position(bucket, ordinal);
      require_test(bucket.members[position] == ordinal,
                   "position rank addresses the ordinal");
    }
  }

  PirClient client(reference);
  SharedPirSessionKeys keys = client.create_session_keys();
  std::vector<std::unique_ptr<PirServer>> servers;
  servers.reserve(buckets.size());
  for (const CuckooBucket &bucket : buckets) {
    auto server = std::make_unique<PirServer>(bucket.params);
    server->set_client_session_keys(client.get_client_id(), keys);
    server->load_data(bucket.target_num_pt,
                      bucket_source(bucket, bucket.params));
    servers.push_back(std::move(server));
  }

  bool profile_checked = false;
  for (size_t leaf : {size_t{0}, leaf_count / 2 + 3, leaf_count - 1}) {
    const std::vector<uint32_t> ordinals =
        sibling_ordinals(leaf, tree_height);
    const auto assignment = cuckoo_place(ordinals, params);

    require_valid_placement(assignment, ordinals, params);

    // One PIR round per bucket; unassigned buckets ask a dummy index.
    for (size_t b = 0; b < buckets.size(); ++b) {
      const size_t position =
          assignment[b].has_value()
              ? cuckoo_position(buckets[b], *assignment[b])
              : 0;
      RlweCt query = client.fast_generate_query(buckets[b].params,
                                                position / 96);
      if (!profile_checked && assignment[b].has_value()) {
        RlweCt profiled_query = query;
        PirPipelineProfile profile;
        RlweCt profiled_response = servers[b]->make_query_profiled(
            client.get_client_id(), profiled_query, &profile);
        const RlwePt profiled_pt = client.decrypt_mod_q(profiled_response);
        const auto [level, local] = node_of_ordinal(*assignment[b]);
        require_test(decode_merkle_node(profiled_pt, position % 96,
                                        buckets[b].params) ==
                         synthetic_merkle_node(level, local),
                     "profiled cuckoo response must decode exactly");
        require_test(profile.total() > std::chrono::nanoseconds::zero(),
                     "profiled cuckoo response recorded server stages");
        profile_checked = true;
      }
      RlweCt response = servers[b]->make_query(client.get_client_id(), query);
      std::stringstream wire;
      (void)servers[b]->save_resp_to_stream(response, wire);
      RlweCt loaded = client.load_resp_from_stream(wire);
      const RlwePt pt = client.decrypt_mod_q(loaded);
      if (assignment[b].has_value()) {
        const auto [level, local] = node_of_ordinal(*assignment[b]);
        require_test(decode_merkle_node(pt, position % 96,
                                        buckets[b].params) ==
                         synthetic_merkle_node(level, local),
                     "batched sibling must decode exactly");
      }
    }
  }
  require_test(profile_checked, "profile transparency fixture executed");

  // Parameter and placement validation.
  require_test(throws_invalid_argument([&] {
                 (void)make_cuckoo_batch_params(0, tree_height, 1);
               }),
               "accepted an empty database");
  require_test(throws_invalid_argument([&] {
                 (void)make_cuckoo_batch_params(item_count, 0, 1);
               }),
               "accepted an empty batch");
  require_test(throws_invalid_argument([&] {
                 (void)make_cuckoo_batch_params(
                     size_t{std::numeric_limits<uint32_t>::max()} + 1,
                     tree_height, 1);
               }),
               "accepted ordinals beyond uint32");
  require_test(make_cuckoo_batch_params(item_count, 1, 1).num_buckets == 2,
               "k = 1 still gets two buckets");
  require_test(throws_invalid_argument([&] {
                 (void)cuckoo_bucket_of(0, params.num_hashes, params);
               }),
               "accepted a hash index beyond num_hashes");
  {
    // An ordinal that hashes nowhere near bucket 0 is not positioned there.
    uint32_t outsider = 0;
    for (;; ++outsider) {
      bool hits = false;
      for (size_t j = 0; j < params.num_hashes; ++j) {
        hits |= cuckoo_bucket_of(outsider, j, params) == 0;
      }
      if (!hits) break;
    }
    require_test(throws_invalid_argument(
                     [&] { (void)cuckoo_position(buckets[0], outsider); }),
                 "positioned an ordinal outside its bucket");
  }
  {
    std::vector<uint32_t> duplicated = sibling_ordinals(0, tree_height);
    duplicated[1] = duplicated[0];
    require_test(throws_invalid_argument(
                     [&] { (void)cuckoo_place(duplicated, params); }),
                 "placed a batch with a repeated ordinal");
    std::vector<uint32_t> short_batch = sibling_ordinals(0, tree_height);
    short_batch.pop_back();
    require_test(throws_invalid_argument(
                     [&] { (void)cuckoo_place(short_batch, params); }),
                 "placed a batch of the wrong size");
  }

  // Placement is not tied to k = H or to one seed: a different seed, a
  // smaller batch and a batch larger than the tree height all place.
  {
    const CuckooBatchParams reseeded =
        make_cuckoo_batch_params(item_count, tree_height, 0x6f746865ULL);
    const std::vector<uint32_t> ordinals = sibling_ordinals(4099, tree_height);
    require_valid_placement(cuckoo_place(ordinals, reseeded), ordinals,
                            reseeded);

    const CuckooBatchParams small_batch =
        make_cuckoo_batch_params(item_count, 5, 0x63756b31ULL);
    const std::vector<uint32_t> five(ordinals.begin(), ordinals.begin() + 5);
    require_valid_placement(cuckoo_place(five, small_batch), five,
                            small_batch);
  }
  {
    // 20 distinct ordinals: the sibling sets of the first and the last leaf
    // are disjoint (right children versus left children at every level).
    std::vector<uint32_t> twenty = sibling_ordinals(0, tree_height);
    const std::vector<uint32_t> tail =
        sibling_ordinals(leaf_count - 1, tree_height);
    twenty.insert(twenty.end(), tail.begin(), tail.begin() + 7);
    require_test(twenty.size() == 20, "fixture batch size");
    const CuckooBatchParams large_batch =
        make_cuckoo_batch_params(item_count, 20, 0x63756b31ULL);
    require_test(large_batch.num_buckets == 30, "1.5x sizing at k = 20");
    const std::vector<CuckooBucket> large_buckets =
        build_cuckoo_buckets(large_batch, reference);
    const auto assignment = cuckoo_place(twenty, large_batch);
    require_valid_placement(assignment, twenty, large_batch);
    // Encrypted round over the 30 buckets: every placed ordinal decodes.
    std::vector<std::unique_ptr<PirServer>> large_servers;
    for (const CuckooBucket &bucket : large_buckets) {
      auto server = std::make_unique<PirServer>(bucket.params);
      server->set_client_session_keys(client.get_client_id(), keys);
      server->load_data(bucket.target_num_pt,
                        bucket_source(bucket, bucket.params));
      large_servers.push_back(std::move(server));
    }
    size_t recovered = 0;
    for (size_t b = 0; b < large_buckets.size(); ++b) {
      if (!assignment[b].has_value()) continue;
      const size_t position =
          cuckoo_position(large_buckets[b], *assignment[b]);
      RlweCt query = client.fast_generate_query(large_buckets[b].params,
                                                position / 96);
      RlweCt response =
          large_servers[b]->make_query(client.get_client_id(), query);
      const RlwePt pt = client.decrypt_mod_q(response);
      const auto [level, local] = node_of_ordinal(*assignment[b]);
      require_test(decode_merkle_node(pt, position % 96,
                                      large_buckets[b].params) ==
                       synthetic_merkle_node(level, local),
                   "k = 20 batched node must decode exactly");
      ++recovered;
    }
    require_test(recovered == 20, "every item of the k = 20 batch recovered");
  }
}

// Timed batch retrieval at the comparison workload. Flags select the leaf
// count, warmup count and measured trial count; one trial retrieves the full
// root-excluded sibling batch for one leaf and records raw server stages.
void PirTest::test_cuckoo_benchmark() {
  require_applicable(paper_config_active(),
                     paper_config_reason("the cuckoo batch baseline"));
  print_func_name(__FUNCTION__);
  using Clock = std::chrono::steady_clock;
  const auto ms_since = [](Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
        .count();
  };

  // H defaults to 22 (2^22 leaves); --leaf-count selects another power of two.
  const size_t tree_height = bench_tree_height(22);
  const size_t leaf_count = size_t{1} << tree_height;
  const size_t item_count = 2 * (leaf_count - 1);
  const CuckooBatchParams params =
      make_cuckoo_batch_params(item_count, tree_height, 0x63756b32ULL);
  const PirParams reference =
      flat_reference(utils::roundup_div(item_count, size_t{96}));

  const auto setup_start = Clock::now();
  std::vector<CuckooBucket> buckets =
      build_cuckoo_buckets(params, reference);
  PirClient client(reference);
  SharedPirSessionKeys keys = client.create_session_keys();
  std::vector<std::unique_ptr<PirServer>> servers;
  servers.reserve(buckets.size());
  for (const CuckooBucket &bucket : buckets) {
    auto server = std::make_unique<PirServer>(bucket.params);
    server->set_client_session_keys(client.get_client_id(), keys);
    server->load_data(bucket.target_num_pt,
                      bucket_source(bucket, bucket.params));
    servers.push_back(std::move(server));
  }
  const double setup_ms = ms_since(setup_start);
  const CuckooStorageStats storage = summarize_cuckoo_storage(buckets);

  const size_t kWarmups = bench_warmups.value_or(3);
  const size_t kTrials = bench_measured_trials.value_or(16);
  const uint64_t trial_seed = bench_trial_seed.value_or(0x63756b6f6f504952ULL);
  const BenchmarkTrialPlan plan =
      make_benchmark_trial_plan(leaf_count, kWarmups, kTrials, trial_seed);

  std::vector<double> client_ms, server_ms, serialize_ms, decode_ms;
  size_t response_bytes_total = 0;
  PirPipelineProfile stage_sum;
  std::vector<CuckooTrialSample> samples;
  samples.reserve(plan.measured_leaf_indices.size());
  const auto run_trial = [&](size_t leaf, bool measured, size_t trial_index) {
    const auto routing_start = Clock::now();
    const std::vector<uint32_t> ordinals =
        sibling_ordinals(leaf, tree_height);
    const auto assignment = cuckoo_place(ordinals, params);
    const double routing_ms = ms_since(routing_start);
    double c_ms = 0, s_ms = 0, r_ms = 0, d_ms = 0;
    size_t resp_bytes = 0;
    size_t recovered = 0;
    PirPipelineProfile trial_profile;
    for (size_t b = 0; b < buckets.size(); ++b) {
      const size_t position =
          assignment[b].has_value()
              ? cuckoo_position(buckets[b], *assignment[b])
              : 0;
      auto start = Clock::now();
      RlweCt query = client.fast_generate_query(buckets[b].params,
                                                position / 96);
      c_ms += ms_since(start);

      start = Clock::now();
      PirPipelineProfile bucket_profile;
      RlweCt response = servers[b]->make_query_profiled(
          client.get_client_id(), query, &bucket_profile);
      s_ms += ms_since(start);
      trial_profile += bucket_profile;

      start = Clock::now();
      std::stringstream wire;
      resp_bytes += servers[b]->save_resp_to_stream(response, wire);
      r_ms += ms_since(start);
      start = Clock::now();
      RlweCt loaded = client.load_resp_from_stream(wire);
      const RlwePt pt = client.decrypt_mod_q(loaded);
      d_ms += ms_since(start);
      if (assignment[b].has_value()) {
        const auto [level, local] = node_of_ordinal(*assignment[b]);
        require_test(decode_merkle_node(pt, position % 96,
                                        buckets[b].params) ==
                         synthetic_merkle_node(level, local),
                     "benchmark batch sibling mismatch");
        ++recovered;
      }
    }
    require_test(recovered == tree_height,
                 "every sibling recovered in the batch");
    if (measured) {
      client_ms.push_back(c_ms);
      server_ms.push_back(s_ms);
      serialize_ms.push_back(r_ms);
      decode_ms.push_back(d_ms);
      if (response_bytes_total == 0) {
        response_bytes_total = resp_bytes;
      } else {
        require_test(response_bytes_total == resp_bytes,
                     "cuckoo response shape changed between trials");
      }
      stage_sum += trial_profile;
      samples.push_back({trial_index, leaf, routing_ms, c_ms, s_ms, r_ms,
                         d_ms, trial_profile, true});
    }
  };

  for (size_t i = 0; i < plan.warmup_leaf_indices.size(); ++i) {
    run_trial(plan.warmup_leaf_indices[i], false, i);
  }
  for (size_t i = 0; i < plan.measured_leaf_indices.size(); ++i) {
    run_trial(plan.measured_leaf_indices[i], true, i);
  }

  const auto avg = [](const std::vector<double> &v) {
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
  };
  uint64_t query_bytes = 0;
  for (const CuckooBucket &bucket : buckets) {
    query_bytes += bucket.params.get_BFV_size(true);
  }

  BENCH_PRINT("cuckoo batch PIR: L=" << tree_height << ", k=" << tree_height
              << ", buckets=" << params.num_buckets << " (3 hashes, 1.5x)");
  BENCH_PRINT("database: " << storage.total_padded_plaintexts
              << " padded PIR matrix plaintexts across buckets ("
              << storage.total_target_plaintexts
              << " target packed plaintexts, "
              << storage.total_member_entries
              << " actual replicated nodes, "
              << storage.logical_replicated_bytes / (1 << 20)
              << " MiB replicated, "
              << storage.logical_padded_bytes / (1 << 20)
              << " MiB padded), largest bucket "
              << storage.max_bucket_members
              << " nodes, setup " << setup_ms << " ms");
  BENCH_PRINT("estimated preprocessed bucket storage "
              << storage.estimated_preprocessed_bytes / (1 << 20)
              << " MiB; public metadata "
              << storage.public_metadata_bytes / (1 << 20)
              << " MiB; estimated resident peak "
              << storage.estimated_peak_resident_bytes / (1 << 20)
              << " MiB");
  BENCH_PRINT("trials: " << kTrials << " measured after " << kWarmups
              << " warmup");
  BENCH_PRINT("trial seed: " << trial_seed);
  BENCH_PRINT("bucket routing avg " << [&] {
    double total = 0.0;
    for (const CuckooTrialSample &sample : samples) {
      total += sample.bucket_routing_ms;
    }
    return total / samples.size();
  }() << " ms");
  BENCH_PRINT("client query avg " << avg(client_ms) << " ms");
  BENCH_PRINT("server batch avg " << avg(server_ms)
              << " ms; samples:");
  for (double v : server_ms) BENCH_PRINT("  batch " << v << " ms");
  {
    const auto stage_avg = [&](std::chrono::nanoseconds duration) {
      return profile_ms(duration) / server_ms.size();
    };
    const double phase_total = stage_avg(stage_sum.total());
    std::string breakdown =
        "server profile breakdown avg (ms, sum over bucket requests):";
    breakdown += " expand " + std::to_string(stage_avg(stage_sum.expand));
    breakdown += " convert " + std::to_string(stage_avg(stage_sum.convert));
    breakdown += " first_dim_query_ntt " +
                 std::to_string(stage_avg(stage_sum.first_dim_query_ntt));
    breakdown += " first_dim_query_pack " +
                 std::to_string(stage_avg(stage_sum.first_dim_query_pack));
    breakdown += " first_dim_core " +
                 std::to_string(stage_avg(stage_sum.first_dim_core));
    breakdown += " first_dim_finalize " +
                 std::to_string(stage_avg(stage_sum.first_dim_finalize));
    breakdown += " other_dim " +
                 std::to_string(stage_avg(stage_sum.other_dim));
    breakdown += " mod_switch " +
                 std::to_string(stage_avg(stage_sum.mod_switch));
    BENCH_PRINT(breakdown);
    BENCH_PRINT("  phases sum " << phase_total << " ms of server batch avg "
                << avg(server_ms) << " ms");
  }
  BENCH_PRINT("response serialize avg " << avg(serialize_ms) << " ms");
  BENCH_PRINT("client decode avg " << avg(decode_ms) << " ms");
  BENCH_PRINT("communication: queries " << query_bytes << " B (modeled, "
              << params.num_buckets << " ciphertexts), responses "
              << response_bytes_total << " B (actual wire codec), online "
              << query_bytes + response_bytes_total
              << " B; helper keys shared bundle "
              << modeled_helper_key_bytes(reference) << " B (modeled)");
  BENCH_PRINT("payload: " << tree_height << " x 32-byte siblings = "
              << tree_height * 32 << " B useful; position metadata public");
  const char *json_path = std::getenv("BENCHMARK_OUTPUT_JSON");
  if (json_path != nullptr && json_path[0] != '\0') {
    write_cuckoo_profile_json(
        json_path, leaf_count, tree_height, kWarmups, kTrials, params,
        storage, trial_seed, response_bytes_total, query_bytes,
        modeled_helper_key_bytes(reference), setup_ms, samples);
    BENCH_PRINT("cuckoo raw profile JSON: " << json_path);
  }
}
