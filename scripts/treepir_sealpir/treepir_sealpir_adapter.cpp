#include "adapter_types.h"
#include "pir.hpp"
#include "pir_client.hpp"
#include "pir_server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;
using namespace seal;
using namespace treepir_adapter;
using Clock = chrono::high_resolution_clock;

namespace {

uint64_t mix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

vector<uint8_t> deterministic_entry(unsigned int height, unsigned int color, uint64_t index)
{
    uint64_t state = mix((static_cast<uint64_t>(height) << 48) ^ (static_cast<uint64_t>(color) << 40) ^ index);
    vector<uint8_t> entry(kEntrySize);
    for (size_t i = 0; i < entry.size(); ++i) {
        if ((i % 8) == 0) {
            state = mix(state + i);
        }
        entry[i] = static_cast<uint8_t>((state >> (8 * (i % 8))) & 0xff);
    }
    return entry;
}

unique_ptr<const uint8_t[]> make_color_database(unsigned int height, unsigned int color, uint64_t entries)
{
    auto db = unique_ptr<uint8_t[]>(new uint8_t[entries * kEntrySize]);
    for (uint64_t index = 0; index < entries; ++index) {
        vector<uint8_t> entry = deterministic_entry(height, color, index);
        copy(entry.begin(), entry.end(), db.get() + index * kEntrySize);
    }
    return unique_ptr<const uint8_t[]>(db.release());
}

double elapsed_ms(Clock::time_point start, Clock::time_point end)
{
    return chrono::duration_cast<chrono::duration<double, milli>>(end - start).count();
}

size_t count_query_ciphertexts(const PirQuery &query)
{
    size_t total = 0;
    for (const auto &part : query) {
        total += part.size();
    }
    return total;
}

uint64_t serialize_query_payload_none(const PirQuery &query)
{
    uint64_t total = 0;
    for (const auto &part : query) {
        for (const auto &ct : part) {
            ostringstream stream(ios::out | ios::binary);
            total += static_cast<uint64_t>(ct.save(stream, compr_mode_type::none));
        }
    }
    return total;
}


uint64_t serialize_reply_payload_none(const PirReply &reply)
{
    uint64_t total = 0;
    for (const auto &ct : reply) {
        ostringstream stream(ios::out | ios::binary);
        total += static_cast<uint64_t>(ct.save(stream, compr_mode_type::none));
    }
    return total;
}

int run(int argc, char **argv)
{
    Options options = parse_options(argc, argv);
    InputData input = parse_input(read_file(options.input));
    vector<InputTrial> selected = select_trials(input, options);

    input.effective_color_bucket_sizes = input.color_bucket_sizes;
    input.smoke_cap_active = options.max_color_entries != 0;
    if (input.smoke_cap_active) {
        for (auto &size : input.effective_color_bucket_sizes) {
            size = min<uint64_t>(size, options.max_color_entries);
        }
    }
    input.effective_max_bucket_entries = *max_element(input.effective_color_bucket_sizes.begin(), input.effective_color_bucket_sizes.end());
    for (const auto &trial : selected) {
        for (size_t color = 0; color < trial.sub_indices.size(); ++color) {
            uint64_t available = input.effective_color_bucket_sizes[color];
            if (trial.sub_indices[color] >= available) {
                throw runtime_error("sub_index outside effective color bucket; raise --max-color-entries or use full run");
            }
        }
    }

    if (options.validate_input_only) {
        vector<TrialAccum> empty;
        for (const auto &trial : selected) {
            TrialAccum acc;
            acc.iteration = trial.iteration;
            acc.warmup = trial.warmup;
            acc.leaf_id = trial.leaf_id;
            acc.path_nodes = trial.path_nodes;
            acc.sub_indices = trial.sub_indices;
            acc.server_generate_reply_color_ms.assign(input.height, 0.0);
            acc.server_expansion_color_ms.assign(input.height, 0.0);
            acc.server_query_ntt_color_ms.assign(input.height, 0.0);
            acc.server_database_ntt_color_ms.assign(input.height, 0.0);
            acc.server_multiply_accumulate_color_ms.assign(input.height, 0.0);
            acc.server_inverse_ntt_color_ms.assign(input.height, 0.0);
            acc.server_recursive_decomposition_modswitch_color_ms.assign(input.height, 0.0);
            acc.server_modswitch_color_ms.assign(input.height, 0.0);
            acc.server_color_ms.assign(input.height, 0.0);
            acc.query_color_bytes.assign(input.height, 0);
            acc.response_color_bytes.assign(input.height, 0);
            empty.push_back(std::move(acc));
        }
        write_output(options.output, render_json(input, options, selected, empty, 0.0, 0.0, 0));
        return 0;
    }

    EncryptionParameters enc_params(scheme_type::bfv);
    gen_encryption_params(options.poly_degree, options.plain_logt, enc_params);
    verify_encryption_params(enc_params);

    vector<TrialAccum> accum;
    accum.reserve(selected.size());
    for (const auto &trial : selected) {
        TrialAccum acc;
        acc.iteration = trial.iteration;
        acc.warmup = trial.warmup;
        acc.leaf_id = trial.leaf_id;
        acc.path_nodes = trial.path_nodes;
        acc.sub_indices = trial.sub_indices;
        acc.server_generate_reply_color_ms.assign(input.height, 0.0);
        acc.server_expansion_color_ms.assign(input.height, 0.0);
        acc.server_query_ntt_color_ms.assign(input.height, 0.0);
        acc.server_database_ntt_color_ms.assign(input.height, 0.0);
        acc.server_multiply_accumulate_color_ms.assign(input.height, 0.0);
        acc.server_inverse_ntt_color_ms.assign(input.height, 0.0);
        acc.server_recursive_decomposition_modswitch_color_ms.assign(input.height, 0.0);
        acc.server_modswitch_color_ms.assign(input.height, 0.0);
        acc.server_color_ms.assign(input.height, 0.0);
        acc.query_color_bytes.assign(input.height, 0);
        acc.response_color_bytes.assign(input.height, 0);
        accum.push_back(std::move(acc));
    }

    double offline_key_setup_ms = 0.0;
    double database_setup_ms = 0.0;
    uint64_t helper_key_bytes = 0;
    const uint32_t client_id = 0;
    for (unsigned int color = 0; color < input.height; ++color) {
        uint64_t entries = input.effective_color_bucket_sizes[color];
        PirParams pir_params;
        gen_pir_params(entries, kEntrySize, options.pir_d, enc_params, pir_params, true, true, true);
        uint64_t largest_dimension = *max_element(pir_params.nvec.begin(), pir_params.nvec.end());
        if (largest_dimension > options.poly_degree) {
            throw runtime_error("SealPIR expand_query cannot handle nvec dimension > poly degree; use --pir-d 2 or larger --poly-degree");
        }

        auto key_start = Clock::now();
        PIRClient client(enc_params, pir_params);
        GaloisKeys galois_keys = client.generate_galois_keys();
        auto key_end = Clock::now();
        offline_key_setup_ms += elapsed_ms(key_start, key_end);
        ostringstream key_stream(ios::out | ios::binary);
        helper_key_bytes += static_cast<uint64_t>(galois_keys.save(key_stream, compr_mode_type::none));

        auto db_start = Clock::now();
        unique_ptr<const uint8_t[]> db = make_color_database(input.height, color, entries);
        PIRServer server(enc_params, pir_params);
        server.set_galois_key(client_id, galois_keys);
        server.set_database(db, entries, kEntrySize);
        server.preprocess_database();
        auto db_end = Clock::now();
        database_setup_ms += elapsed_ms(db_start, db_end);

        for (size_t i = 0; i < selected.size(); ++i) {
            const uint64_t element_index = selected[i].sub_indices[color];
            const uint64_t fv_index = client.get_fv_index(element_index);
            const uint64_t fv_offset = client.get_fv_offset(element_index);

            auto query_start = Clock::now();
            PirQuery query = client.generate_query(fv_index);
            auto query_end = Clock::now();
            accum[i].query_ms += elapsed_ms(query_start, query_end);
            accum[i].query_ciphertexts += count_query_ciphertexts(query);

            auto query_ser_start = Clock::now();
            uint64_t query_bytes = serialize_query_payload_none(query);
            auto query_ser_end = Clock::now();
            accum[i].query_serialize_ms += elapsed_ms(query_ser_start, query_ser_end);
            accum[i].query_bytes += query_bytes;
            accum[i].query_color_bytes[color] = query_bytes;

            auto server_start = Clock::now();
            PirReply reply = server.generate_reply(query, client_id);
            auto server_end = Clock::now();
            double color_generate_reply_ms = elapsed_ms(server_start, server_end);
            const SealPirServerProfile &profile = server.last_profile();
            accum[i].server_generate_reply_ms += color_generate_reply_ms;
            accum[i].server_generate_reply_color_ms[color] = color_generate_reply_ms;
            accum[i].server_expansion_ms += profile.expansion_ms;
            accum[i].server_expansion_color_ms[color] = profile.expansion_ms;
            accum[i].server_query_ntt_ms += profile.query_ntt_ms;
            accum[i].server_query_ntt_color_ms[color] = profile.query_ntt_ms;
            accum[i].server_database_ntt_ms += profile.database_ntt_ms;
            accum[i].server_database_ntt_color_ms[color] = profile.database_ntt_ms;
            accum[i].server_multiply_accumulate_ms += profile.multiply_accumulate_ms;
            accum[i].server_multiply_accumulate_color_ms[color] = profile.multiply_accumulate_ms;
            accum[i].server_inverse_ntt_ms += profile.inverse_ntt_ms;
            accum[i].server_inverse_ntt_color_ms[color] = profile.inverse_ntt_ms;
            accum[i].server_recursive_decomposition_modswitch_ms += profile.recursive_decomposition_modswitch_ms;
            accum[i].server_recursive_decomposition_modswitch_color_ms[color] = profile.recursive_decomposition_modswitch_ms;
            accum[i].response_ciphertexts += reply.size();

            auto response_modswitch_start = Clock::now();
            server.mod_switch_reply(reply);
            auto response_modswitch_end = Clock::now();
            double modswitch_ms = elapsed_ms(response_modswitch_start, response_modswitch_end);
            accum[i].server_modswitch_ms += modswitch_ms;
            accum[i].server_modswitch_color_ms[color] = modswitch_ms;
            accum[i].server_ms += color_generate_reply_ms + modswitch_ms;
            accum[i].server_color_ms[color] = color_generate_reply_ms + modswitch_ms;

            auto response_ser_start = Clock::now();
            uint64_t reply_bytes = serialize_reply_payload_none(reply);
            auto response_ser_end = Clock::now();
            accum[i].response_serialize_ms += elapsed_ms(response_ser_start, response_ser_end);
            accum[i].response_bytes += reply_bytes;
            accum[i].response_color_bytes[color] = reply_bytes;

            auto decode_start = Clock::now();
            vector<uint8_t> decoded = client.decode_reply(reply, fv_offset);
            auto decode_end = Clock::now();
            accum[i].decode_ms += elapsed_ms(decode_start, decode_end);
            ++accum[i].decoded_entries;
            accum[i].correct = accum[i].correct && (decoded == deterministic_entry(input.height, color, element_index));
        }
    }

    write_output(options.output, render_json(input, options, selected, accum, offline_key_setup_ms, database_setup_ms, helper_key_bytes));
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        return run(argc, argv);
    } catch (const exception &e) {
        cerr << "treepir_sealpir_adapter error: " << e.what() << '\n';
        return 1;
    }
}
