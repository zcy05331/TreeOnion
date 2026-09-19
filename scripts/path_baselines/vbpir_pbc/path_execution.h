#ifndef VBPIR_PATH_EXECUTION_H
#define VBPIR_PATH_EXECUTION_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "batchpirclient.h"
#include "batchpirserver.h"
#include "path_input.h"

struct SetupProfile {
    double raw_db_ms = 0.0;
    double bucket_map_ms = 0.0;
    double server_preprocess_ms = 0.0;
    double client_keygen_ms = 0.0;
    double eval_key_install_ms = 0.0;
    std::size_t replication_bytes = 0;
    std::size_t physical_replica_entries = 0;
    std::size_t padded_matrix_cells = 0;
    std::size_t helper_key_bytes = 0;
};

struct PathSample {
    size_t leaf = 0;
    std::vector<size_t> path_indices;
    std::vector<size_t> decoded_path_indices;
    std::vector<std::string> decoded_path_hex;
    bool correct = false;
    double query_ms = 0.0;
    double server_ms = 0.0;
    double decode_ms = 0.0;
    std::size_t query_bytes = 0;
    std::size_t response_bytes = 0;
    std::map<std::string, double> stages_ms;
};

struct RunnerContext {
    RawDB raw_db;
    seal::EncryptionParameters encryption_params;
    BatchPirParams params;
    BatchPIRServer server;
    BatchPIRClient client;
    SetupProfile setup;

    RunnerContext(RawDB db, seal::EncryptionParameters params_in,
                  BatchPirParams batch_params, BatchPIRServer batch_server,
                  BatchPIRClient batch_client, SetupProfile setup_profile);
};

RunnerContext initialize_runner(const PathOptions &options);
std::vector<PathSample> run_samples(const std::vector<size_t> &leaves,
                                    std::uint32_t height,
                                    RunnerContext &context);
std::size_t serialized_public_key_bytes(BatchPIRClient &client);

#endif
