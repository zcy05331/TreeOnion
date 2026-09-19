#include "spirit_adapter_types.h"
#include "spirit_rc.h"

#include "../common/path_workload.h"

#include "SPIRIT/pir/batch_protocol.h"

#include "seal/seal.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace spirit_path_adapter {
namespace {

using Clock = std::chrono::steady_clock;

template <typename Callable>
double measure_ms(Callable &&callable)
{
    const auto start = Clock::now();
    callable();
    const auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

template <typename SerializableLike>
std::uint64_t serialized_size_bytes(
    const SerializableLike &obj,
    seal::compr_mode_type mode = seal::compr_mode_type::none)
{
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    return static_cast<std::uint64_t>(obj.save(stream, mode));
}

std::uint64_t query_vector_size_bytes(const spirit::pir::BatchQueryMessage &query)
{
    std::uint64_t total = 0;
    for (const auto &bucket_query : query.bucket_queries) {
        total += static_cast<std::uint64_t>(bucket_query.query_vector.size() * sizeof(std::uint32_t));
    }
    return total;
}

std::uint64_t response_size_bytes(const spirit::pir::BatchResponseMessage &response)
{
    std::uint64_t total = 0;
    for (const auto &bucket_response : response.bucket_responses) {
        for (const auto &cipher : bucket_response.packed_columns) {
            total += serialized_size_bytes(cipher);
        }
    }
    return total;
}

std::uint64_t response_ciphertext_count(const spirit::pir::BatchResponseMessage &response)
{
    std::uint64_t total = 0;
    for (const auto &bucket_response : response.bucket_responses) {
        total += bucket_response.packed_columns.size();
    }
    return total;
}

int response_min_noise_budget_bits(
    const spirit::pir::BatchResponseMessage &response,
    seal::Decryptor &decryptor)
{
    int min_budget = std::numeric_limits<int>::max();
    bool saw_ciphertext = false;
    for (const auto &bucket_response : response.bucket_responses) {
        for (const auto &cipher : bucket_response.packed_columns) {
            min_budget = std::min(min_budget, decryptor.invariant_noise_budget(cipher));
            saw_ciphertext = true;
        }
    }
    return saw_ciphertext ? min_budget : -1;
}

int ciphertext_noise_budget_bits(const seal::Ciphertext &cipher, seal::Decryptor &decryptor)
{
    return decryptor.invariant_noise_budget(cipher);
}

std::vector<std::vector<std::uint8_t>> materialize_database(std::uint64_t record_count)
{
    if (record_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::invalid_argument("record count does not fit in size_t");
    }
    std::vector<std::vector<std::uint8_t>> database(
        static_cast<std::size_t>(record_count),
        std::vector<std::uint8_t>(kRecordSizeBytes));
    for (std::uint64_t i = 0; i < record_count; ++i) {
        const auto bytes = path_workload::record_bytes(static_cast<std::size_t>(i));
        std::copy(bytes.begin(), bytes.end(), database[static_cast<std::size_t>(i)].begin());
    }
    return database;
}

bool check_path_records(
    const WorkloadTrial &trial,
    const std::vector<std::vector<std::uint8_t>> &extracted,
    std::size_t &correct_path_entries)
{
    correct_path_entries = 0;
    if (extracted.size() != trial.query_indices.size()) {
        return false;
    }
    bool all_correct = true;
    for (std::size_t i = 0; i < trial.query_indices.size(); ++i) {
        const auto expected = path_workload::record_bytes(static_cast<std::size_t>(trial.query_indices[i]));
        if (extracted[i].size() != expected.size()) {
            all_correct = false;
            continue;
        }
        if (!std::equal(expected.begin(), expected.end(), extracted[i].begin())) {
            all_correct = false;
            continue;
        }
        if (i < trial.path_indices.size()) {
            ++correct_path_entries;
        }
    }
    return all_correct;
}

void record_backend_geometry(
    const spirit::pir::BatchPublicParams &public_params,
    const spirit::pir::BatchServerState &server_state,
    RunResult &result)
{
    std::uint64_t matrix_byte_cells = 0;
    std::uint64_t physical_copied_entries = 0;
    std::uint64_t bucket_layout_metadata_bytes = 0;

    for (std::size_t bucket = 0; bucket < public_params.bucket_layouts.size(); ++bucket) {
        const auto &layout = public_params.bucket_layouts[bucket];
        matrix_byte_cells += static_cast<std::uint64_t>(layout.rows) * layout.cols;
        physical_copied_entries += server_state.pbc_server_state.actual_bucket_sizes[bucket];
        bucket_layout_metadata_bytes += 6ULL * sizeof(std::size_t);
        bucket_layout_metadata_bytes +=
            static_cast<std::uint64_t>(layout.item_layouts.size()) * 2ULL * sizeof(std::size_t);
    }

    std::uint64_t pbc_routing_table_bytes = 0;
    for (const auto &candidates : public_params.pbc_public_params.candidate_buckets) {
        pbc_routing_table_bytes += static_cast<std::uint64_t>(candidates.size()) * sizeof(std::size_t);
    }
    for (const auto &placements : public_params.pbc_public_params.index_placements) {
        pbc_routing_table_bytes +=
            static_cast<std::uint64_t>(placements.size()) * 2ULL * sizeof(std::size_t);
    }
    pbc_routing_table_bytes += 6ULL * sizeof(std::size_t);

    const std::uint64_t public_matrix_seed_bytes =
        static_cast<std::uint64_t>(public_params.bucket_public_params.size()) *
        sizeof(seal::prng_seed_type);

    result.backend_matrix_byte_cells = matrix_byte_cells;
    result.backend_records = (matrix_byte_cells + kRecordSizeBytes - 1) / kRecordSizeBytes;
    result.backend_physical_copied_entries = physical_copied_entries;
    result.setup_public_matrix_seed_bytes = public_matrix_seed_bytes;
    result.setup_pbc_routing_table_bytes = pbc_routing_table_bytes;
    result.setup_bucket_layout_metadata_bytes = bucket_layout_metadata_bytes;
    result.setup_public_metadata_bytes =
        public_matrix_seed_bytes + pbc_routing_table_bytes + bucket_layout_metadata_bytes;
}

} // namespace

RunResult run_spirit_path(const Options &options)
{
    RunResult result;
    result.workload = build_workload(options);
    result.database_bytes = result.workload.record_count * kRecordSizeBytes;
    result.backend_records = result.workload.record_count;
    if (options.scheme == "spirit_rc") {
        result.limitations.push_back(
            "SPIRIT+RC prototype implements packed encrypted selector upload, server selector expansion, encrypted response rotation, and MultiResMerge over a strided one-polynomial-per-bucket public layout.");
        result.limitations.push_back(
            "SPIRIT+RC uses a different public database layout from plain SPIRIT: bytes of a record are strided by n/32 residues to allow one compressed response ciphertext.");
        result.rc_expansion_coeff_modulus = {
            72057594037641217ULL,
            1152921504606830593ULL,
            1152921504606683137ULL,
        };
    } else {
        result.limitations.push_back(
            "Plain SPIRIT baseline over upstream balanced bucket layout; SPIRIT+RC uses a separate strided RC layout when --scheme spirit_rc is selected.");
    }
    result.limitations.push_back(
        "SEAL is configured with sec_level=none because the public SPIRIT artifact parameter line n=2048, coeff_modulus bits [56,60], t=65537 exceeds SEAL's degree-2048 tc128 modulus limit; no 128-bit security claim is made by this adapter.");

    if (options.validate_input_only) {
        for (const auto &trial : result.workload.schedule) {
            TrialResult row;
            row.iteration = trial.iteration;
            row.warmup = trial.warmup;
            row.leaf = trial.leaf;
            row.path_indices = trial.path_indices;
            row.query_indices = trial.query_indices;
            row.path_entries = trial.path_indices.size();
            result.trials.push_back(std::move(row));
        }
        return result;
    }

    seal::EncryptionParameters parms(seal::scheme_type::bfv);
    parms.set_poly_modulus_degree(options.poly_modulus_degree);
    parms.set_coeff_modulus(seal::CoeffModulus::Create(options.poly_modulus_degree, {56, 60}));
    parms.set_plain_modulus(65537);

    seal::SEALContext context(parms, true, seal::sec_level_type::none);
    seal::KeyGenerator keygen(context);
    seal::SecretKey secret_key = keygen.secret_key();
    seal::Encryptor encryptor(context, secret_key);
    seal::Evaluator evaluator(context);
    seal::Decryptor decryptor(context, secret_key);
    result.bfv_plain_modulus = context.first_context_data()->parms().plain_modulus().value();
    const auto &coeff_modulus = context.first_context_data()->parms().coeff_modulus();
    result.bfv_coeff_modulus.reserve(coeff_modulus.size());
    for (const auto &prime : coeff_modulus) {
        result.bfv_coeff_modulus.push_back(prime.value());
    }

    std::vector<std::vector<std::uint8_t>> database;
    result.database_materialize_ms = measure_ms([&]() {
        database = materialize_database(result.workload.record_count);
    });

    spirit::pir::BatchParams params;
    params.pbc_params.num_entries = static_cast<std::size_t>(result.workload.record_count);
    params.pbc_params.batch_size = result.workload.batch_size;
    params.pbc_params.num_hash_funcs = options.num_hash_funcs;
    params.pbc_params.cuckoo_factor = options.cuckoo_factor;
    params.pbc_params.max_attempts = options.max_attempts;
    params.pbc_params.padding_value = 0;
    params.lwe_dimension = options.lwe_dimension;
    params.record_plain_modulus = options.record_plain_modulus;
    params.record_size_bytes = kRecordSizeBytes;
    params.benchmark_randomize_hint_matrix = false;
    if (options.scheme == "spirit_rc") {
        params.rc_response_poly_degree = options.poly_modulus_degree;
    }

    spirit::pir::BatchPublicParams public_params;
    spirit::pir::BatchServerState server_state;
    spirit::pir::BatchAnswerPrecompState answer_precomp_state;

    result.database_setup_ms = measure_ms([&]() {
        spirit::pir::BatchDBSetup(database, params, public_params, server_state);
    });
    result.bucket_count = public_params.pbc_public_params.num_buckets;
    result.actual_bucket_sizes.assign(
        server_state.pbc_server_state.actual_bucket_sizes.begin(),
        server_state.pbc_server_state.actual_bucket_sizes.end());
    record_backend_geometry(public_params, server_state, result);

    result.answer_precompute_ms = measure_ms([&]() {
        spirit::pir::BatchAnswerPrecompute(public_params, server_state, context, evaluator, answer_precomp_state);
    });

    std::cerr << "SPIRIT database preprocessing complete" << std::endl;
    for (const auto &trial : result.workload.schedule) {
        TrialResult row;
        row.iteration = trial.iteration;
        row.warmup = trial.warmup;
        row.leaf = trial.leaf;
        row.path_indices = trial.path_indices;
        row.query_indices = trial.query_indices;
        row.path_entries = trial.path_indices.size();

        std::vector<std::size_t> batch_indices;
        batch_indices.reserve(trial.query_indices.size());
        for (std::uint64_t index : trial.query_indices) {
            batch_indices.push_back(static_cast<std::size_t>(index));
        }

        spirit::pir::BatchQueryMessage query;
        spirit::pir::BatchClientState client_state;
        spirit::pir::BatchQueryAnswerPrecompState query_answer_precomp_state;
        spirit::pir::BatchResponseMessage response;
        std::vector<std::vector<std::uint8_t>> extracted_records;
        RcQuery rc_query;

        row.query_ms = measure_ms([&]() {
            spirit::pir::BatchQuery(
                public_params,
                batch_indices,
                context,
                keygen,
                encryptor,
                evaluator,
                query,
                client_state);
        });
        row.query_vector_bytes = query_vector_size_bytes(query);
        row.query_packed_secret_bytes = serialized_size_bytes(query.packed_secret);
        row.query_galois_key_bytes = serialized_size_bytes(query.galois_keys);
        if (options.scheme == "spirit_rc") {
            rc_query = build_rc_query(public_params, client_state, context, keygen, encryptor, secret_key);
            row.query_rc_selector_bytes = rc_query.selector_bytes;
            row.query_rc_galois_key_bytes = rc_query.galois_key_bytes;
            row.query_rc_merge_galois_key_bytes = rc_query.merge_galois_key_bytes;
            row.query_rc_selector_ciphertext_bytes = rc_query.packed_selectors.ciphertext_bytes;
            row.query_rc_selector_parameter_bytes = rc_query.packed_selectors.parameter_bytes;
            row.query_rc_selector_key_bytes = rc_query.packed_selectors.key_bytes;
            row.query_rc_selector_bucket_count = rc_query.packed_selectors.bucket_count;
            row.query_rc_selector_value_bits = rc_query.packed_selectors.value_bits;
        }
        row.query_bytes = row.query_vector_bytes + row.query_packed_secret_bytes +
            row.query_galois_key_bytes + row.query_rc_selector_bytes + row.query_rc_galois_key_bytes;

        row.server_query_precompute_ms = measure_ms([&]() {
            spirit::pir::BatchAnswerQueryPrecompute(
                public_params,
                answer_precomp_state,
                query,
                context,
                evaluator,
                query_answer_precomp_state);
        });

        row.server_answer_ms = measure_ms([&]() {
            spirit::pir::BatchAnswer(
                public_params,
                server_state,
                answer_precomp_state,
                query_answer_precomp_state,
                query,
                context,
                encryptor,
                evaluator,
                decryptor,
                response);
        });
        row.server_secret_unpack_ms = response.profile.secret_unpack_ms;
        row.server_matvec_ms = response.profile.matvec_ms;
        row.server_modswitch_ms = response.profile.modswitch_ms;
        row.server_b_extract_ms = response.profile.b_extract_ms;
        row.server_lwe_pack_ms = response.profile.lwe_pack_ms;
        row.server_answer_ms = response.profile.total_ms;
        const double profiled_answer_ms =
            row.server_secret_unpack_ms +
            row.server_matvec_ms +
            row.server_modswitch_ms +
            row.server_b_extract_ms +
            row.server_lwe_pack_ms;
        row.server_answer_glue_ms = std::max(0.0, row.server_answer_ms - profiled_answer_ms);
        row.server_ms = row.server_query_precompute_ms + row.server_answer_ms;
        row.response_ciphertexts = response_ciphertext_count(response);
        if (options.scheme == "spirit_rc") {
            RcResponse rc_response = build_rc_response(public_params, response, rc_query, context, evaluator);
            row.server_rc_selector_unpack_ms = rc_response.selector_unpack_ms;
            row.server_rc_rotate_ms = rc_response.rotate_ms;
            row.server_rc_merge_ms = rc_response.merge_ms;
            row.server_ms += row.server_rc_selector_unpack_ms + row.server_rc_rotate_ms + row.server_rc_merge_ms;
            row.response_ciphertexts = 1;
            row.response_bytes = rc_response.response_bytes;
            row.response_noise_budget_min_bits = ciphertext_noise_budget_bits(rc_response.merged, decryptor);
            row.rc_rotation_external_products = rc_response.rotation_external_products;
            row.rc_merge_keyswitches = rc_response.merge_keyswitches;
            row.extract_ms = measure_ms([&]() {
                extracted_records = extract_rc_records(public_params, client_state, rc_response, context, decryptor);
            });
        } else {
            row.response_noise_budget_min_bits = response_min_noise_budget_bits(response, decryptor);
            row.response_bytes = response_size_bytes(response);
            row.extract_ms = measure_ms([&]() {
                spirit::pir::BatchExtract(public_params, client_state, response, context, decryptor, extracted_records);
            });
        }

        row.correct = check_path_records(trial, extracted_records, row.correct_entries);
        if (!row.correct || row.correct_entries != row.path_entries) {
            std::ostringstream message;
            message << "SPIRIT decoded path mismatch for leaf " << row.leaf
                << ": correct_entries=" << row.correct_entries
                << " path_entries=" << row.path_entries
                << " response_noise_budget_min_bits=" << row.response_noise_budget_min_bits;
            throw std::runtime_error(message.str());
        }
        result.trials.push_back(std::move(row));
        std::cerr << "completed path " << trial.iteration << " leaf " << trial.leaf << std::endl;
    }

    return result;
}

} // namespace spirit_path_adapter
