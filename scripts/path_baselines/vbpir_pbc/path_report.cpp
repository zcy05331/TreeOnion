#include "path_report.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {

constexpr const char *kUpstreamCommit = "930063c5aefc441244abb4890fdf35383f3aa956";

void write_size_array(std::ostream &out, const std::vector<size_t> &values) {
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ",";
        out << values[i];
    }
    out << "]";
}

void write_string_array(std::ostream &out, const std::vector<std::string> &values) {
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ",";
        out << "\"" << values[i] << "\"";
    }
    out << "]";
}

void write_double_map(std::ostream &out, const std::map<std::string, double> &values) {
    out << "{";
    bool first = true;
    for (const auto &entry : values) {
        if (!first) out << ",";
        first = false;
        out << "\"" << entry.first << "\":" << entry.second;
    }
    out << "}";
}

void write_int_array(std::ostream &out, const std::vector<int> &values) {
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ",";
        out << values[i];
    }
    out << "]";
}

void write_sample(std::ostream &out, const PathSample &sample, std::size_t trial,
                  std::size_t path_entries) {
    out << "{";
    out << "\"trial\":" << trial << ",";
    out << "\"leaf\":" << sample.leaf << ",";
    out << "\"server_ms\":" << sample.server_ms << ",";
    out << "\"query_ms\":" << sample.query_ms << ",";
    out << "\"decode_ms\":" << sample.decode_ms << ",";
    out << "\"query_bytes\":" << sample.query_bytes << ",";
    out << "\"response_bytes\":" << sample.response_bytes << ",";
    out << "\"correct\":" << (sample.correct ? "true" : "false") << ",";
    out << "\"correct_entries\":" << sample.decoded_path_indices.size() << ",";
    out << "\"path_entries\":" << path_entries << ",";
    out << "\"path_indices\":";
    write_size_array(out, sample.path_indices);
    out << ",\"decoded_path_indices\":";
    write_size_array(out, sample.decoded_path_indices);
    out << ",\"decoded_path_hex\":";
    write_string_array(out, sample.decoded_path_hex);
    out << ",\"stages_ms\":";
    write_double_map(out, sample.stages_ms);
    out << "}";
}

void write_samples(std::ostream &out, const std::vector<PathSample> &samples,
                   std::size_t path_entries) {
    out << "[\n";
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (i) out << ",\n";
        write_sample(out, samples[i], i, path_entries);
    }
    out << "\n]";
}

std::vector<int> coeff_mod_bits(const seal::EncryptionParameters &params) {
    std::vector<int> bits;
    for (const auto &modulus : params.coeff_modulus()) {
        bits.push_back(modulus.bit_count());
    }
    return bits;
}

}  // namespace

void write_report(const PathOptions &options, const PathSchedule &schedule,
                  RunnerContext &context,
                  const std::vector<PathSample> &warmup_samples,
                  const std::vector<PathSample> &samples) {
    std::ofstream out(options.output);
    if (!out) {
        throw std::runtime_error("unable to open output: " + options.output);
    }
    const std::size_t logical_records = (size_t{1} << (options.height + 1)) - 1;
    const std::size_t path_entries = static_cast<std::size_t>(options.height) + 1;
    const std::size_t bucket_count = static_cast<std::size_t>(
        std::ceil(context.params.get_cuckoo_factor() * context.params.get_batch_size()));
    const std::size_t backend_records = context.setup.padded_matrix_cells;
    const std::size_t public_padding = bucket_count > path_entries ? bucket_count - path_entries : 0;

    out << std::fixed << std::setprecision(6);
    out << "{\n";
    out << "\"schema_version\":\"path-baseline-v1\",\n";
    out << "\"scheme\":\"vbpir_pbc\",\n";
    out << "\"path_mode\":\"ancestor_root_included\",\n";
    out << "\"height\":" << options.height << ",\n";
    out << "\"leaf_count\":" << (size_t{1} << options.height) << ",\n";
    out << "\"record_bytes\":32,\n";
    out << "\"logical_records\":" << logical_records << ",\n";
    out << "\"logical_db_bytes\":" << (logical_records * 32ULL) << ",\n";
    out << "\"backend_records\":" << backend_records << ",\n";
    out << "\"batch_size\":" << path_entries << ",\n";
    out << "\"warmups\":" << options.warmups << ",\n";
    out << "\"trials\":" << options.trials << ",\n";
    out << "\"source_commit\":\"" << kUpstreamCommit << "\",\n";
    out << "\"parameters\":{";
    out << "\"scheme\":\"BFV\",";
    out << "\"poly_modulus_degree\":" << context.encryption_params.poly_modulus_degree() << ",";
    out << "\"plain_modulus_bit_count\":" << context.encryption_params.plain_modulus().bit_count() << ",";
    out << "\"coeff_modulus_bits\":";
    write_int_array(out, coeff_mod_bits(context.encryption_params));
    out << ",\"num_hash_functions\":" << context.params.get_num_hash_funcs();
    out << ",\"cuckoo_factor\":" << context.params.get_cuckoo_factor();
    out << ",\"max_bucket_size\":" << context.params.get_max_bucket_size();
    out << ",\"first_dimension_size\":" << context.params.get_first_dimension_size();
    out << ",\"requested_first_dimension_size\":" << options.first_dim;
    out << ",\"num_slots_per_entry\":" << context.params.get_num_slots_per_entry();
    out << ",\"public_batch_buckets\":" << bucket_count;
    out << ",\"physical_replica_entries\":" << context.setup.physical_replica_entries;
    out << ",\"padded_matrix_cells\":" << context.setup.padded_matrix_cells;
    out << "},\n";
    out << "\"setup\":{";
    out << "\"raw_db_ms\":" << context.setup.raw_db_ms << ",";
    out << "\"bucket_map_ms\":" << context.setup.bucket_map_ms << ",";
    out << "\"preprocess_ms\":" << (context.setup.raw_db_ms + context.setup.server_preprocess_ms
                                    + context.setup.client_keygen_ms
                                    + context.setup.eval_key_install_ms) << ",";
    out << "\"server_preprocess_ms\":" << context.setup.server_preprocess_ms << ",";
    out << "\"client_keygen_ms\":" << context.setup.client_keygen_ms << ",";
    out << "\"eval_key_install_ms\":" << context.setup.eval_key_install_ms << ",";
    out << "\"replication_bytes\":" << context.setup.replication_bytes << ",";
    out << "\"physical_replica_entries\":" << context.setup.physical_replica_entries << ",";
    out << "\"padded_matrix_cells\":" << context.setup.padded_matrix_cells << ",";
    out << "\"helper_key_bytes\":" << context.setup.helper_key_bytes;
    out << "},\n";
    out << "\"communication\":{";
    out << "\"query\":\"actual_serialized\",";
    out << "\"response\":\"actual_serialized\",";
    out << "\"public_batch_padding\":" << public_padding;
    out << "},\n";
    out << "\"profiling_coverage\":\"exclusive_stages\",\n";
    out << "\"leaf_schedule\":{\"measured\":";
    write_size_array(out, schedule.measured_leaves);
    out << "},\n";
    out << "\"warmup_samples\":";
    write_samples(out, warmup_samples, path_entries);
    out << ",\n\"samples\":";
    write_samples(out, samples, path_entries);
    out << "\n}\n";
}
