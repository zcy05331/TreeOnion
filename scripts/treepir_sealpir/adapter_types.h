#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// Shared input and result records for the adapter's parser, runner and report.
namespace treepir_adapter {
using std::string;
using std::vector;
using std::numeric_limits;

constexpr size_t kEntrySize = 32;

struct Options {
    string input;
    string output = "-";
    size_t warmups_override = numeric_limits<size_t>::max();
    size_t trials_override = numeric_limits<size_t>::max();
    bool has_warmups_override = false;
    bool has_trials_override = false;
    uint32_t poly_degree = 8192;
    uint32_t plain_logt = 20;
    uint32_t pir_d = 2;
    uint64_t max_color_entries = 0;
    bool validate_input_only = false;
};

struct InputTrial {
    size_t iteration = 0;
    bool warmup = false;
    uint64_t leaf_id = 0;
    vector<uint64_t> path_nodes;
    vector<uint64_t> sub_indices;
};

struct InputData {
    unsigned int height = 0;
    uint64_t leaf_count = 0;
    size_t entry_size = kEntrySize;
    size_t warmups = 0;
    size_t trials = 0;
    uint64_t max_bucket_entries = 0;
    vector<uint64_t> color_bucket_sizes;
    vector<uint64_t> effective_color_bucket_sizes;
    uint64_t effective_max_bucket_entries = 0;
    bool smoke_cap_active = false;
    vector<InputTrial> results;
};

struct TrialAccum {
    size_t iteration = 0;
    bool warmup = false;
    uint64_t leaf_id = 0;
    vector<uint64_t> path_nodes;
    vector<uint64_t> sub_indices;
    double query_ms = 0.0;
    double query_serialize_ms = 0.0;
    double server_generate_reply_ms = 0.0;
    double server_expansion_ms = 0.0;
    double server_query_ntt_ms = 0.0;
    double server_database_ntt_ms = 0.0;
    double server_multiply_accumulate_ms = 0.0;
    double server_inverse_ntt_ms = 0.0;
    double server_recursive_decomposition_modswitch_ms = 0.0;
    double server_modswitch_ms = 0.0;
    double server_ms = 0.0;
    double response_serialize_ms = 0.0;
    double decode_ms = 0.0;
    uint64_t query_bytes = 0;
    uint64_t response_bytes = 0;
    uint64_t query_ciphertexts = 0;
    uint64_t response_ciphertexts = 0;
    uint64_t decoded_entries = 0;
    bool correct = true;
    vector<double> server_generate_reply_color_ms;
    vector<double> server_expansion_color_ms;
    vector<double> server_query_ntt_color_ms;
    vector<double> server_database_ntt_color_ms;
    vector<double> server_multiply_accumulate_color_ms;
    vector<double> server_inverse_ntt_color_ms;
    vector<double> server_recursive_decomposition_modswitch_color_ms;
    vector<double> server_modswitch_color_ms;
    vector<double> server_color_ms;
    vector<uint64_t> query_color_bytes;
    vector<uint64_t> response_color_bytes;
};

string read_file(const string &path);
InputData parse_input(const string &json);
Options parse_options(int argc, char **argv);
vector<InputTrial> select_trials(const InputData &input, const Options &options);
void write_output(const string &path, const string &contents);
string render_json(const InputData &input, const Options &options,
                   const vector<InputTrial> &selected,
                   const vector<TrialAccum> &results,
                   double offline_key_setup_ms, double database_setup_ms,
                   uint64_t helper_key_bytes);

} // namespace treepir_adapter
