#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace spirit_path_adapter {

constexpr std::size_t kRecordSizeBytes = 32;

struct Options {
    std::string scheme = "spirit";
    unsigned height = 0;
    std::string leaf_file;
    std::string output = "-";
    std::size_t warmup = std::numeric_limits<std::size_t>::max();
    std::size_t trials = std::numeric_limits<std::size_t>::max();
    std::size_t lwe_dimension = 1024;
    std::size_t poly_modulus_degree = 2048;
    std::uint64_t record_plain_modulus = 256;
    std::size_t num_hash_funcs = 3;
    double cuckoo_factor = 1.5;
    std::size_t max_attempts = 500;
    bool pad_batch_to_power2 = true;
    bool validate_input_only = false;
};

struct WorkloadTrial {
    std::size_t iteration = 0;
    bool warmup = false;
    std::uint64_t leaf = 0;
    std::vector<std::uint64_t> path_indices;
    std::vector<std::uint64_t> query_indices;
};

struct Workload {
    unsigned height = 0;
    std::uint64_t leaf_count = 0;
    std::uint64_t record_count = 0;
    std::size_t path_record_count = 0;
    std::size_t batch_size = 0;
    std::size_t warmup = 0;
    std::size_t trials = 0;
    std::vector<WorkloadTrial> schedule;
};

struct TrialResult {
    std::size_t iteration = 0;
    bool warmup = false;
    std::uint64_t leaf = 0;
    std::vector<std::uint64_t> path_indices;
    std::vector<std::uint64_t> query_indices;
    double query_ms = 0.0;
    double server_query_precompute_ms = 0.0;
    double server_answer_ms = 0.0;
    double server_secret_unpack_ms = 0.0;
    double server_matvec_ms = 0.0;
    double server_modswitch_ms = 0.0;
    double server_b_extract_ms = 0.0;
    double server_lwe_pack_ms = 0.0;
    double server_answer_glue_ms = 0.0;
    double server_rc_selector_unpack_ms = 0.0;
    double server_rc_rotate_ms = 0.0;
    double server_rc_merge_ms = 0.0;
    double server_ms = 0.0;
    double extract_ms = 0.0;
    std::uint64_t query_vector_bytes = 0;
    std::uint64_t query_packed_secret_bytes = 0;
    std::uint64_t query_galois_key_bytes = 0;
    std::uint64_t query_rc_selector_bytes = 0;
    std::uint64_t query_rc_galois_key_bytes = 0;
    std::uint64_t query_rc_merge_galois_key_bytes = 0;
    std::uint64_t query_rc_selector_ciphertext_bytes = 0;
    std::uint64_t query_rc_selector_parameter_bytes = 0;
    std::uint64_t query_rc_selector_key_bytes = 0;
    std::size_t query_rc_selector_bucket_count = 0;
    std::size_t query_rc_selector_value_bits = 0;
    std::uint64_t query_bytes = 0;
    std::uint64_t response_ciphertexts = 0;
    std::uint64_t response_bytes = 0;
    int response_noise_budget_min_bits = -1;
    std::size_t rc_rotation_external_products = 0;
    std::size_t rc_merge_keyswitches = 0;
    std::size_t correct_entries = 0;
    std::size_t path_entries = 0;
    bool correct = true;
};

struct RunResult {
    Workload workload;
    double database_materialize_ms = 0.0;
    double database_setup_ms = 0.0;
    double answer_precompute_ms = 0.0;
    std::uint64_t database_bytes = 0;
    std::uint64_t backend_records = 0;
    std::uint64_t backend_matrix_byte_cells = 0;
    std::uint64_t backend_physical_copied_entries = 0;
    std::uint64_t setup_public_metadata_bytes = 0;
    std::uint64_t setup_public_matrix_seed_bytes = 0;
    std::uint64_t setup_pbc_routing_table_bytes = 0;
    std::uint64_t setup_bucket_layout_metadata_bytes = 0;
    std::uint64_t bfv_plain_modulus = 0;
    std::vector<std::uint64_t> bfv_coeff_modulus;
    std::string seal_security_level = "none";
    std::vector<std::uint64_t> rc_expansion_coeff_modulus;
    std::size_t bucket_count = 0;
    std::vector<std::uint64_t> actual_bucket_sizes;
    std::vector<std::string> limitations;
    std::vector<TrialResult> trials;
};

Options parse_options(int argc, char **argv);
Workload build_workload(const Options &options);
void write_output(const std::string &path, const std::string &contents);

RunResult run_spirit_path(const Options &options);
std::string render_json(const Options &options, const RunResult &result);

} // namespace spirit_path_adapter
