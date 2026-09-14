#include "adapter_types.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace std;

namespace treepir_adapter {
namespace {

string json_escape(const string &value)
{
    ostringstream out;
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

void write_uint_array(ostream &out, const vector<uint64_t> &values)
{
    out << '[';
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << values[i];
    }
    out << ']';
}

} // namespace

void write_output(const string &path, const string &contents)
{
    if (path == "-") {
        cout << contents << '\n';
        return;
    }
    ofstream out(path);
    if (!out.is_open()) {
        throw runtime_error("unable to open output: " + path);
    }
    out << contents << '\n';
}

string render_json(
    const InputData &input,
    const Options &options,
    const vector<InputTrial> &selected,
    const vector<TrialAccum> &results,
    double offline_key_setup_ms,
    double database_setup_ms,
    uint64_t helper_key_bytes)
{
    size_t measured_count = 0;
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
    double query_bytes = 0.0;
    double response_bytes = 0.0;
    bool all_correct = true;
    for (const auto &r : results) {
        all_correct = all_correct && r.correct;
        if (!r.warmup) {
            ++measured_count;
            query_ms += r.query_ms;
            query_serialize_ms += r.query_serialize_ms;
            server_generate_reply_ms += r.server_generate_reply_ms;
            server_expansion_ms += r.server_expansion_ms;
            server_query_ntt_ms += r.server_query_ntt_ms;
            server_database_ntt_ms += r.server_database_ntt_ms;
            server_multiply_accumulate_ms += r.server_multiply_accumulate_ms;
            server_inverse_ntt_ms += r.server_inverse_ntt_ms;
            server_recursive_decomposition_modswitch_ms += r.server_recursive_decomposition_modswitch_ms;
            server_modswitch_ms += r.server_modswitch_ms;
            server_ms += r.server_ms;
            response_serialize_ms += r.response_serialize_ms;
            decode_ms += r.decode_ms;
            query_bytes += static_cast<double>(r.query_bytes);
            response_bytes += static_cast<double>(r.response_bytes);
        }
    }
    if (measured_count) {
        query_ms /= measured_count;
        query_serialize_ms /= measured_count;
        server_generate_reply_ms /= measured_count;
        server_expansion_ms /= measured_count;
        server_query_ntt_ms /= measured_count;
        server_database_ntt_ms /= measured_count;
        server_multiply_accumulate_ms /= measured_count;
        server_inverse_ntt_ms /= measured_count;
        server_recursive_decomposition_modswitch_ms /= measured_count;
        server_modswitch_ms /= measured_count;
        server_ms /= measured_count;
        response_serialize_ms /= measured_count;
        decode_ms /= measured_count;
        query_bytes /= measured_count;
        response_bytes /= measured_count;
    }

    ostringstream out;
    out << fixed << setprecision(3);
    out << '{';
    out << "\"model\":\"TreePIR+SealPIRplusCore\",";
    out << "\"input\":\"" << json_escape(options.input) << "\",";
    out << "\"run_mode\":\"" << (options.validate_input_only ? "validate_input_only" : "in_process_core") << "\",";
    out << "\"transport_mode\":\"manual_uncompressed_ciphertext_save\",";
    out << "\"query_payload_label\":\"actual_existing_query_ciphertexts_save_none\",";
    out << "\"response_payload_label\":\"server_modswitch_then_manual_ciphertext_save_none\",";
    out << "\"height\":" << input.height << ',';
    out << "\"leaf_count\":" << input.leaf_count << ',';
    out << "\"entry_size_bytes\":" << input.entry_size << ',';
    out << "\"warmups\":" << count_if(selected.begin(), selected.end(), [](const InputTrial &t) { return t.warmup; }) << ',';
    out << "\"trials\":" << count_if(selected.begin(), selected.end(), [](const InputTrial &t) { return !t.warmup; }) << ',';
    out << "\"layout\":{\"color_buckets\":" << input.height;
    out << ",\"full_max_bucket_entries\":" << input.max_bucket_entries;
    out << ",\"effective_max_bucket_entries\":" << input.effective_max_bucket_entries;
    out << ",\"smoke_cap_active\":" << (input.smoke_cap_active ? "true" : "false");
    out << ",\"full_color_bucket_sizes\":";
    write_uint_array(out, input.color_bucket_sizes);
    out << ",\"effective_color_bucket_sizes\":";
    write_uint_array(out, input.effective_color_bucket_sizes);
    out << "},";
    out << "\"seal_parameters\":{\"scheme\":\"BFV\",\"poly_modulus_degree\":" << options.poly_degree << ",\"plain_logt\":" << options.plain_logt << ",\"pir_d\":" << options.pir_d << ",\"coeff_modulus\":\"BFVDefault\"},";
    out << "\"regime\":\"sequential_color_processing; per-path server_ms sums one SealPIR generate_reply per color and is not an all-colors-hot-cache single wall time\",";
    out << "\"stage_definitions\":{\"offline_key_setup_ms\":\"sum of per-color PIRClient construction and Galois key generation\",\"database_setup_ms\":\"sum of per-color deterministic 32B database generation, SealPIR plaintext packing, and NTT preprocess\",\"query_color_bytes\":\"per-color bytes from saving the same generated PirQuery ciphertexts with compr_mode_type::none\",\"server_generate_reply_color_ms\":\"per-color PIRServer::generate_reply wall time in color order\",\"server_expansion_ms\":\"sum inside generate_reply expanding encrypted selection vectors\",\"server_query_ntt_ms\":\"sum inside generate_reply transforming expanded query ciphertexts to NTT\",\"server_database_ntt_ms\":\"sum inside generate_reply transforming current plaintext database/intermediate plaintexts to NTT when needed\",\"server_multiply_accumulate_ms\":\"sum inside generate_reply multiply_plain/add accumulation\",\"server_inverse_ntt_ms\":\"sum inside generate_reply transform_from_ntt on intermediate ciphertexts\",\"server_recursive_decomposition_modswitch_ms\":\"sum inside generate_reply recursive mod-switch and ciphertext decomposition to plaintexts\",\"server_modswitch_color_ms\":\"per-color time to apply the upstream response mod-switch step; included in server_ms\",\"server_color_ms\":\"per-color generate_reply plus server_modswitch time; sums to server_ms\",\"response_color_bytes\":\"per-color bytes after server mod-switch and Ciphertext::save(compr_mode_type::none)\"},";
    out << "\"offline_key_setup_ms\":" << offline_key_setup_ms << ',';
    out << "\"database_setup_ms\":" << database_setup_ms << ',';
    out << "\"actual_helper_key_bytes_uncompressed\":" << helper_key_bytes << ',';
    out << "\"measured_summary\":{\"count\":" << measured_count;
    out << ",\"query_ms_mean\":" << query_ms;
    out << ",\"query_serialize_ms_mean\":" << query_serialize_ms;
    out << ",\"server_generate_reply_ms_mean\":" << server_generate_reply_ms;
    out << ",\"server_expansion_ms_mean\":" << server_expansion_ms;
    out << ",\"server_query_ntt_ms_mean\":" << server_query_ntt_ms;
    out << ",\"server_database_ntt_ms_mean\":" << server_database_ntt_ms;
    out << ",\"server_multiply_accumulate_ms_mean\":" << server_multiply_accumulate_ms;
    out << ",\"server_inverse_ntt_ms_mean\":" << server_inverse_ntt_ms;
    out << ",\"server_recursive_decomposition_modswitch_ms_mean\":" << server_recursive_decomposition_modswitch_ms;
    out << ",\"server_modswitch_ms_mean\":" << server_modswitch_ms;
    out << ",\"server_ms_mean\":" << server_ms;
    out << ",\"response_serialize_ms_mean\":" << response_serialize_ms;
    out << ",\"decode_ms_mean\":" << decode_ms;
    out << ",\"actual_query_payload_bytes_mean\":" << query_bytes;
    out << ",\"actual_response_payload_bytes_mean\":" << response_bytes;
    out << ",\"all_correct\":" << (all_correct ? "true" : "false") << "},";
    auto write_double_array = [&out](const vector<double> &values) {
        out << '[';
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) out << ',';
            out << values[i];
        }
        out << ']';
    };
    out << "\"results\":[";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto &r = results[i];
        if (i) out << ',';
        out << '{';
        out << "\"iteration\":" << r.iteration;
        out << ",\"phase\":\"" << (r.warmup ? "warmup" : "measured") << "\"";
        out << ",\"leaf_id\":" << r.leaf_id;
        out << ",\"path_nodes\":";
        write_uint_array(out, r.path_nodes);
        out << ",\"sub_indices\":";
        write_uint_array(out, r.sub_indices);
        out << ",\"query_ms\":" << r.query_ms;
        out << ",\"query_serialize_ms\":" << r.query_serialize_ms;
        out << ",\"server_generate_reply_ms\":" << r.server_generate_reply_ms;
        out << ",\"server_generate_reply_color_ms\":[";
        for (size_t color = 0; color < r.server_generate_reply_color_ms.size(); ++color) {
            if (color) out << ',';
            out << r.server_generate_reply_color_ms[color];
        }
        out << "]";
        out << ",\"server_expansion_ms\":" << r.server_expansion_ms;
        out << ",\"server_expansion_color_ms\":";
        write_double_array(r.server_expansion_color_ms);
        out << ",\"server_query_ntt_ms\":" << r.server_query_ntt_ms;
        out << ",\"server_query_ntt_color_ms\":";
        write_double_array(r.server_query_ntt_color_ms);
        out << ",\"server_database_ntt_ms\":" << r.server_database_ntt_ms;
        out << ",\"server_database_ntt_color_ms\":";
        write_double_array(r.server_database_ntt_color_ms);
        out << ",\"server_multiply_accumulate_ms\":" << r.server_multiply_accumulate_ms;
        out << ",\"server_multiply_accumulate_color_ms\":";
        write_double_array(r.server_multiply_accumulate_color_ms);
        out << ",\"server_inverse_ntt_ms\":" << r.server_inverse_ntt_ms;
        out << ",\"server_inverse_ntt_color_ms\":";
        write_double_array(r.server_inverse_ntt_color_ms);
        out << ",\"server_recursive_decomposition_modswitch_ms\":" << r.server_recursive_decomposition_modswitch_ms;
        out << ",\"server_recursive_decomposition_modswitch_color_ms\":";
        write_double_array(r.server_recursive_decomposition_modswitch_color_ms);
        out << ",\"server_modswitch_ms\":" << r.server_modswitch_ms;
        out << ",\"server_modswitch_color_ms\":[";
        for (size_t color = 0; color < r.server_modswitch_color_ms.size(); ++color) {
            if (color) out << ',';
            out << r.server_modswitch_color_ms[color];
        }
        out << "]";
        out << ",\"server_ms\":" << r.server_ms;
        out << ",\"server_color_ms\":[";
        for (size_t color = 0; color < r.server_color_ms.size(); ++color) {
            if (color) out << ',';
            out << r.server_color_ms[color];
        }
        out << "]";
        out << ",\"response_serialize_ms\":" << r.response_serialize_ms;
        out << ",\"decode_ms\":" << r.decode_ms;
        out << ",\"actual_query_payload_bytes_unseeded\":" << r.query_bytes;
        out << ",\"query_color_bytes\":";
        write_uint_array(out, r.query_color_bytes);
        out << ",\"actual_response_payload_bytes\":" << r.response_bytes;
        out << ",\"response_color_bytes\":";
        write_uint_array(out, r.response_color_bytes);
        out << ",\"query_ciphertexts\":" << r.query_ciphertexts;
        out << ",\"response_ciphertexts\":" << r.response_ciphertexts;
        out << ",\"decoded_entries\":" << r.decoded_entries;
        out << ",\"correct\":" << (r.correct ? "true" : "false") << '}';
    }
    out << "]}";
    return out.str();
}

} // namespace treepir_adapter
