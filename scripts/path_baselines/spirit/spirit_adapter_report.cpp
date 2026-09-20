#include "spirit_adapter_types.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace spirit_path_adapter {
namespace {

std::string json_escape(const std::string &value)
{
    std::ostringstream out;
    for (char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << ch; break;
        }
    }
    return out.str();
}

template <typename T>
void write_array(std::ostream &out, const std::vector<T> &values)
{
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) {
            out << ',';
        }
        out << values[i];
    }
    out << ']';
}

void write_string_array(std::ostream &out, const std::vector<std::string> &values)
{
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) {
            out << ',';
        }
        out << '"' << json_escape(values[i]) << '"';
    }
    out << ']';
}

struct Means {
    std::size_t count = 0;
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
    double query_bytes = 0.0;
    double response_bytes = 0.0;
    bool all_correct = true;
};

Means measured_means(const std::vector<TrialResult> &trials)
{
    Means means;
    for (const auto &trial : trials) {
        means.all_correct = means.all_correct && trial.correct;
        if (trial.warmup) {
            continue;
        }
        ++means.count;
        means.query_ms += trial.query_ms;
        means.server_query_precompute_ms += trial.server_query_precompute_ms;
        means.server_answer_ms += trial.server_answer_ms;
        means.server_secret_unpack_ms += trial.server_secret_unpack_ms;
        means.server_matvec_ms += trial.server_matvec_ms;
        means.server_modswitch_ms += trial.server_modswitch_ms;
        means.server_b_extract_ms += trial.server_b_extract_ms;
        means.server_lwe_pack_ms += trial.server_lwe_pack_ms;
        means.server_answer_glue_ms += trial.server_answer_glue_ms;
        means.server_rc_selector_unpack_ms += trial.server_rc_selector_unpack_ms;
        means.server_rc_rotate_ms += trial.server_rc_rotate_ms;
        means.server_rc_merge_ms += trial.server_rc_merge_ms;
        means.server_ms += trial.server_ms;
        means.extract_ms += trial.extract_ms;
        means.query_bytes += static_cast<double>(trial.query_bytes);
        means.response_bytes += static_cast<double>(trial.response_bytes);
    }
    if (means.count != 0) {
        means.query_ms /= means.count;
        means.server_query_precompute_ms /= means.count;
        means.server_answer_ms /= means.count;
        means.server_secret_unpack_ms /= means.count;
        means.server_matvec_ms /= means.count;
        means.server_modswitch_ms /= means.count;
        means.server_b_extract_ms /= means.count;
        means.server_lwe_pack_ms /= means.count;
        means.server_answer_glue_ms /= means.count;
        means.server_rc_selector_unpack_ms /= means.count;
        means.server_rc_rotate_ms /= means.count;
        means.server_rc_merge_ms /= means.count;
        means.server_ms /= means.count;
        means.extract_ms /= means.count;
        means.query_bytes /= means.count;
        means.response_bytes /= means.count;
    }
    return means;
}

} // namespace

void write_output(const std::string &path, const std::string &contents)
{
    if (path == "-") {
        std::cout << contents << '\n';
        return;
    }
    std::ofstream out(path);
    if (!out.is_open()) {
        throw std::runtime_error("unable to open output: " + path);
    }
    out << contents << '\n';
}

std::string render_json(const Options &options, const RunResult &result)
{
    const Means means = measured_means(result.trials);
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << '{';
    out << "\"schema_version\":\"path-baseline-v1\",";
    out << "\"scheme\":\"" << options.scheme << "\",";
    out << "\"path_mode\":\"ancestor_root_included\",";
    out << "\"upstream\":\"https://github.com/zhouzhangwalker/SPIRIT\",";
    out << "\"source_commit\":\"" << json_escape(SPIRIT_SOURCE_COMMIT) << "\",";
    out << "\"run_mode\":\"" << (options.validate_input_only ? "validate_input_only" : "real_spirit_batch_pir") << "\",";
    out << "\"height\":" << result.workload.height << ',';
    out << "\"leaf_count\":" << result.workload.leaf_count << ',';
    out << "\"record_bytes\":" << kRecordSizeBytes << ',';
    out << "\"logical_records\":" << result.workload.record_count << ',';
    out << "\"logical_db_bytes\":" << result.database_bytes << ',';
    out << "\"backend_records\":" << result.backend_records << ',';
    out << "\"batch_size\":" << result.workload.batch_size << ',';
    out << "\"batch_padding\":\"" << (options.pad_batch_to_power2 ? "public_dummy_indices_to_power_of_two" : "none") << "\",";
    out << "\"warmups\":" << result.workload.warmup << ',';
    out << "\"trials\":" << result.workload.trials << ',';
    out << "\"input\":{\"leaf_file\":\"" << json_escape(options.leaf_file) << "\"},";
    out << "\"parameters\":{\"scheme\":\"BFV\",\"poly_modulus_degree\":" << options.poly_modulus_degree
        << ",\"coeff_modulus_bits\":[56,60],\"coeff_modulus\":";
    write_array(out, result.bfv_coeff_modulus);
    out << ",\"plain_modulus\":" << (result.bfv_plain_modulus == 0 ? 65537 : result.bfv_plain_modulus)
        << ",\"rlwe_ciphertext_modulus\":\"SEAL BFV coeff_modulus product over listed coeff_modulus primes\""
        << ",\"seal_security_level\":\"" << json_escape(result.seal_security_level) << "\""
        << ",\"security_note\":\"Public SPIRIT artifact parameter line n=2048, coeff_modulus bits [56,60], t=65537 is run with SEAL sec_level=none; this adapter makes no tc128 claim.\""
        << ",\"lwe_modulus_bits\":32"
        << ",\"lwe_dimension\":" << options.lwe_dimension
        << ",\"lwe_secret_distribution\":\"uniform ternary {-1,0,1}\""
        << ",\"lwe_query_noise_distribution\":\"SPIRIT sample_poly_cbd_u32\""
        << ",\"rc_expansion_coeff_modulus_bits\":[56,60,60]"
        << ",\"rc_expansion_coeff_modulus\":";
    write_array(out, result.rc_expansion_coeff_modulus);
    out
        << ",\"record_plain_modulus\":" << options.record_plain_modulus
        << ",\"num_hash_funcs\":" << options.num_hash_funcs
        << ",\"cuckoo_factor\":" << options.cuckoo_factor
        << ",\"max_attempts\":" << options.max_attempts
        << ",\"rc_response_poly_degree\":" << options.poly_modulus_degree
        << ",\"rc_selector_query_packing\":\"packed_single_rgsw_query_ciphertext_plus_expansion_parameters_and_keys_counted\""
        << ",\"benchmark_randomize_hint_matrix\":false},";
    out << "\"layout\":{\"bucket_count\":" << result.bucket_count
        << ",\"variant\":\"rc_strided_one_polynomial_per_bucket\""
        << ",\"rc_byte_row_stride\":" << options.poly_modulus_degree / kRecordSizeBytes
        << ",\"physical_copied_entries\":" << result.backend_physical_copied_entries
        << ",\"matrix_byte_cells\":" << result.backend_matrix_byte_cells
        << ",\"matrix_32b_equivalent_records\":" << result.backend_records
        << ",\"actual_bucket_sizes\":";
    write_array(out, result.actual_bucket_sizes);
    out << "},";
    out << "\"setup\":{\"database_materialize_ms\":" << result.database_materialize_ms
        << ",\"preprocess_ms\":" << result.database_setup_ms
        << ",\"answer_precompute_ms\":" << result.answer_precompute_ms
        << ",\"helper_bytes\":null"
        << ",\"helper_bytes_model\":\"unavailable_no_setup_serializer\""
        << ",\"public_metadata_bytes\":" << result.setup_public_metadata_bytes
        << ",\"public_metadata_bytes_model\":\"current_in_memory_public_metadata_pbc_tables_bucket_layouts_and_public_matrix_seeds_not_wire_requirement\""
        << ",\"pbc_routing_table_bytes\":" << result.setup_pbc_routing_table_bytes
        << ",\"bucket_layout_metadata_bytes\":" << result.setup_bucket_layout_metadata_bytes
        << ",\"public_matrix_seed_bytes\":" << result.setup_public_matrix_seed_bytes
        << "},";
    out << "\"communication\":{\"query\":\"actual_serialized_including_rc_rgsw_selectors_and_merge_galois_keys_when_present\",\"response\":\"actual_serialized\"},";
    out << "\"profiling_coverage\":\"exclusive_stages\",";
    out << "\"measured_summary\":{\"count\":" << means.count
        << ",\"query_ms_mean\":" << means.query_ms
        << ",\"server_query_precompute_ms_mean\":" << means.server_query_precompute_ms
        << ",\"server_answer_ms_mean\":" << means.server_answer_ms
        << ",\"server_secret_unpack_ms_mean\":" << means.server_secret_unpack_ms
        << ",\"server_matvec_ms_mean\":" << means.server_matvec_ms
        << ",\"server_modswitch_ms_mean\":" << means.server_modswitch_ms
        << ",\"server_b_extract_ms_mean\":" << means.server_b_extract_ms
        << ",\"server_lwe_pack_ms_mean\":" << means.server_lwe_pack_ms
        << ",\"server_answer_glue_ms_mean\":" << means.server_answer_glue_ms
        << ",\"server_rc_selector_unpack_ms_mean\":" << means.server_rc_selector_unpack_ms
        << ",\"server_rc_rotate_ms_mean\":" << means.server_rc_rotate_ms
        << ",\"server_rc_merge_ms_mean\":" << means.server_rc_merge_ms
        << ",\"server_ms_mean\":" << means.server_ms
        << ",\"extract_ms_mean\":" << means.extract_ms
        << ",\"actual_query_payload_bytes_mean\":" << means.query_bytes
        << ",\"actual_response_payload_bytes_mean\":" << means.response_bytes
        << ",\"all_correct\":" << (means.all_correct ? "true" : "false") << "},";
    out << "\"limitations\":";
    write_string_array(out, result.limitations);
    out << ",\"warmup_samples\":[";
    bool first_warmup = true;
    auto write_sample = [&](const TrialResult &trial) {
        out << '{';
        out << "\"leaf\":" << trial.leaf
            << ",\"server_ms\":" << trial.server_ms
            << ",\"query_ms\":" << trial.query_ms
            << ",\"decode_ms\":" << trial.extract_ms
            << ",\"query_bytes\":" << trial.query_bytes
            << ",\"response_bytes\":" << trial.response_bytes
            << ",\"correct\":" << (trial.correct ? "true" : "false")
            << ",\"correct_entries\":" << trial.correct_entries
            << ",\"path_entries\":" << trial.path_entries
            << ",\"path_indices\":";
        write_array(out, trial.path_indices);
        out << ",\"query_indices\":";
        write_array(out, trial.query_indices);
        out << ",\"stages_ms\":{\"query_precompute\":" << trial.server_query_precompute_ms
            << ",\"secret_unpack\":" << trial.server_secret_unpack_ms
            << ",\"matvec\":" << trial.server_matvec_ms
            << ",\"modswitch\":" << trial.server_modswitch_ms
            << ",\"b_extract\":" << trial.server_b_extract_ms
            << ",\"lwe_pack\":" << trial.server_lwe_pack_ms
            << ",\"answer_glue\":" << trial.server_answer_glue_ms;
        out << ",\"selector_unpack\":" << trial.server_rc_selector_unpack_ms
            << ",\"response_rotation\":" << trial.server_rc_rotate_ms
            << ",\"response_merge\":" << trial.server_rc_merge_ms;
        out << "},\"details\":{\"query_vector_bytes\":" << trial.query_vector_bytes
            << ",\"query_packed_secret_bytes\":" << trial.query_packed_secret_bytes
            << ",\"query_galois_key_bytes\":" << trial.query_galois_key_bytes
            << ",\"query_rc_selector_bytes\":" << trial.query_rc_selector_bytes
            << ",\"query_rc_galois_key_bytes\":" << trial.query_rc_galois_key_bytes
            << ",\"query_rc_merge_galois_key_bytes\":" << trial.query_rc_merge_galois_key_bytes
            << ",\"query_rc_selector_ciphertext_bytes\":" << trial.query_rc_selector_ciphertext_bytes
            << ",\"query_rc_selector_parameter_bytes\":" << trial.query_rc_selector_parameter_bytes
            << ",\"query_rc_selector_key_bytes\":" << trial.query_rc_selector_key_bytes
            << ",\"query_rc_selector_bucket_count\":" << trial.query_rc_selector_bucket_count
            << ",\"query_rc_selector_value_bits\":" << trial.query_rc_selector_value_bits
            << ",\"response_ciphertexts\":" << trial.response_ciphertexts
            << ",\"response_noise_budget_min_bits\":" << trial.response_noise_budget_min_bits
            << ",\"rc_rotation_external_products\":" << trial.rc_rotation_external_products
            << ",\"rc_merge_keyswitches\":" << trial.rc_merge_keyswitches
            << "}}";
    };
    for (std::size_t i = 0; i < result.trials.size(); ++i) {
        const auto &trial = result.trials[i];
        if (!trial.warmup) {
            continue;
        }
        if (!first_warmup) {
            out << ',';
        }
        write_sample(trial);
        first_warmup = false;
    }
    out << "],\"samples\":[";
    bool first_sample = true;
    for (const auto &trial : result.trials) {
        if (trial.warmup) {
            continue;
        }
        if (!first_sample) {
            out << ',';
        }
        write_sample(trial);
        first_sample = false;
    }
    out << "]}";
    return out.str();
}

} // namespace spirit_path_adapter
