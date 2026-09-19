#include "path_execution.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "path_workload.h"
#include "src/utils.h"

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::string hex_bytes(const std::vector<unsigned char> &bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char byte : bytes) {
        out << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return out.str();
}

template <typename Serializable>
std::size_t serialized_one(const Serializable &item) {
    std::ostringstream out(std::ios::binary);
    item.save(out, seal::compr_mode_type::none);
    return out.str().size();
}

template <typename CiphertextList>
std::size_t serialized_bytes(const CiphertextList &items) {
    std::size_t bytes = 0;
    for (const auto &item : items) {
        bytes += serialized_one(item);
    }
    return bytes;
}

std::size_t serialized_query_bytes(const std::vector<PIRQuery> &queries) {
    std::size_t bytes = 0;
    for (const auto &query : queries) {
        bytes += serialized_bytes(query);
    }
    return bytes;
}

std::vector<std::vector<unsigned char>> flatten_decoded(const std::vector<RawResponses> &decoded) {
    std::vector<std::vector<unsigned char>> flat;
    for (const auto &group : decoded) {
        for (const auto &entry : group) {
            flat.push_back(entry);
        }
    }
    return flat;
}

PathSample run_one(size_t leaf, std::uint32_t height, RunnerContext &context) {
    constexpr int kClientId = 0;
    PathSample sample;
    sample.leaf = leaf;
    sample.path_indices = path_workload::path_indices(height, leaf);
    std::vector<std::uint64_t> batch(sample.path_indices.begin(), sample.path_indices.end());

    const auto query_start = Clock::now();
    auto queries = context.client.create_queries(batch);
    const auto query_end = Clock::now();
    sample.query_ms = elapsed_ms(query_start, query_end);
    sample.query_bytes = serialized_query_bytes(queries);

    const auto server_start = Clock::now();
    PIRResponseList responses = context.server.generate_response(kClientId, queries);
    const auto server_end = Clock::now();
    sample.server_ms = elapsed_ms(server_start, server_end);
    sample.stages_ms = context.server.get_last_stage_ms();
    sample.response_bytes = serialized_bytes(responses);

    const auto decode_start = Clock::now();
    auto decoded = flatten_decoded(context.client.decode_responses_chunks(responses));
    const auto decode_end = Clock::now();
    sample.decode_ms = elapsed_ms(decode_start, decode_end);

    const auto cuckoo_keys = context.client.get_cuckoo_keys();
    sample.correct = true;
    for (size_t requested : sample.path_indices) {
        auto it = std::find(cuckoo_keys.begin(), cuckoo_keys.end(), requested);
        if (it == cuckoo_keys.end()) {
            sample.correct = false;
            continue;
        }
        const std::size_t bucket = static_cast<std::size_t>(std::distance(cuckoo_keys.begin(), it));
        if (bucket >= decoded.size() || decoded[bucket] != context.raw_db[requested]) {
            sample.correct = false;
            continue;
        }
        sample.decoded_path_indices.push_back(requested);
        sample.decoded_path_hex.push_back(hex_bytes(decoded[bucket]));
    }
    sample.correct = sample.correct && sample.decoded_path_indices == sample.path_indices;
    if (!sample.correct) {
        throw std::runtime_error("VBPIR decoded path did not match requested ancestors");
    }
    return sample;
}

}  // namespace

RunnerContext::RunnerContext(RawDB db, seal::EncryptionParameters params_in,
                             BatchPirParams batch_params, BatchPIRServer batch_server,
                             BatchPIRClient batch_client, SetupProfile setup_profile)
    : raw_db(std::move(db)),
      encryption_params(std::move(params_in)),
      params(std::move(batch_params)),
      server(std::move(batch_server)),
      client(std::move(batch_client)),
      setup(std::move(setup_profile)) {}

std::size_t serialized_public_key_bytes(BatchPIRClient &client) {
    auto keys = client.get_public_keys();
    return serialized_one(keys.first) + serialized_one(keys.second);
}

RunnerContext initialize_runner(const PathOptions &options) {
    SetupProfile setup;
    const auto db_start = Clock::now();
    RawDB raw_db = build_raw_db(options.height);
    setup.raw_db_ms = elapsed_ms(db_start, Clock::now());

    auto encryption_params = utils::create_encryption_parameters(
        std::to_string(options.height) + "," + std::to_string(raw_db.size()) + ",32");
    BatchPirParams params(static_cast<int>(options.height + 1), raw_db.size(), 32,
                          encryption_params);

    const auto server_start = Clock::now();
    BatchPIRServer server(params, raw_db, options.first_dim);
    setup.server_preprocess_ms = elapsed_ms(server_start, Clock::now());

    const auto client_start = Clock::now();
    BatchPIRClient client(params);
    setup.client_keygen_ms = elapsed_ms(client_start, Clock::now());

    const auto keys_start = Clock::now();
    client.set_map(server.get_hash_map());
    setup.helper_key_bytes = serialized_public_key_bytes(client);
    server.set_client_keys(0, client.get_public_keys());
    setup.eval_key_install_ms = elapsed_ms(keys_start, Clock::now());
    setup.bucket_map_ms = setup.server_preprocess_ms;
    const std::size_t bucket_count = static_cast<std::size_t>(
        std::ceil(params.get_cuckoo_factor() * params.get_batch_size()));
    setup.physical_replica_entries = bucket_count * params.get_max_bucket_size();
    const std::size_t dim = params.get_first_dimension_size();
    const std::size_t rounded_bucket_entries =
        dim * static_cast<std::size_t>(std::ceil(params.get_max_bucket_size() * 1.0 / dim));
    setup.padded_matrix_cells = bucket_count * rounded_bucket_entries;
    setup.replication_bytes = raw_db.size() * 32ULL * params.get_num_hash_funcs();

    return RunnerContext(std::move(raw_db), std::move(encryption_params), std::move(params),
                         std::move(server), std::move(client), std::move(setup));
}

std::vector<PathSample> run_samples(const std::vector<size_t> &leaves,
                                    std::uint32_t height,
                                    RunnerContext &context) {
    std::vector<PathSample> samples;
    samples.reserve(leaves.size());
    for (size_t leaf : leaves) {
        samples.push_back(run_one(leaf, height, context));
    }
    return samples;
}
