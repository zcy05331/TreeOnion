#include "spirit_rc.h"
#include "spirit_rc_selector_pack.h"
#include "../common/path_workload.h"
#include "SPIRIT/pir/batch_protocol.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

int main()
{
    try {
        constexpr std::size_t height = 8;
        constexpr std::size_t record_count = (std::size_t{1} << (height + 1)) - 1;
        constexpr std::size_t n = 2048;
        std::vector<std::vector<std::uint8_t>> database(record_count);
        for (std::size_t i = 0; i < record_count; ++i) {
            auto bytes = path_workload::record_bytes(i);
            database[i].assign(bytes.begin(), bytes.end());
        }
        seal::EncryptionParameters encryption(seal::scheme_type::bfv);
        encryption.set_poly_modulus_degree(n);
        encryption.set_coeff_modulus(seal::CoeffModulus::Create(n, {56, 60}));
        encryption.set_plain_modulus(65537);
        seal::SEALContext context(encryption, true, seal::sec_level_type::none);
        seal::KeyGenerator keygen(context);
        auto secret = keygen.secret_key();
        seal::Encryptor encryptor(context, secret);
        seal::Evaluator evaluator(context);
        seal::Decryptor decryptor(context, secret);

        spirit::pir::BatchParams params;
        params.pbc_params.num_entries = record_count;
        params.pbc_params.batch_size = 16;
        params.pbc_params.cuckoo_factor = 1.5;
        params.lwe_dimension = 1024;
        params.record_plain_modulus = 256;
        params.record_size_bytes = 32;
        params.rc_response_poly_degree = n;
        params.benchmark_randomize_hint_matrix = false;
        spirit::pir::BatchPublicParams public_params;
        spirit::pir::BatchServerState server;
        spirit::pir::BatchDBSetup(database, params, public_params, server);
        spirit::pir::BatchAnswerPrecompState precomputed;
        spirit::pir::BatchAnswerPrecompute(public_params, server, context, evaluator, precomputed);
        for (std::size_t leaf : {0U, 17U, 23U, 255U}) {
            auto indices = path_workload::path_indices(height, leaf);
            for (std::size_t dummy = 0; indices.size() < 16; ++dummy) {
                if (std::find(indices.begin(), indices.end(), dummy) == indices.end()) indices.push_back(dummy);
            }
            spirit::pir::BatchQueryMessage query;
            spirit::pir::BatchClientState client;
            spirit::pir::BatchQuery(public_params, indices, context, keygen, encryptor, evaluator, query, client);
            auto rc_query = spirit_path_adapter::build_rc_query(public_params, client, context, keygen, encryptor, secret);
            spirit::pir::BatchQueryAnswerPrecompState query_precomputed;
            spirit::pir::BatchAnswerQueryPrecompute(public_params, precomputed, query, context, evaluator, query_precomputed);
            spirit::pir::BatchResponseMessage response;
            spirit::pir::BatchAnswer(public_params, server, precomputed, query_precomputed, query,
                                     context, encryptor, evaluator, decryptor, response);
            std::vector<std::vector<spirit::RNSRGSWCiphertext>> direct_selectors(public_params.pbc_public_params.num_buckets);
            std::vector<std::uint64_t> offsets(public_params.pbc_public_params.num_buckets, 0);
            for (const auto &assignment : client.query_plan.assignments) {
                const auto bucket = assignment.placement.bucket_index;
                offsets[bucket] = public_params.bucket_layouts[bucket].item_layouts[assignment.placement.slot_index].start_row_index;
            }
            // Independently encrypted per-bit selectors are a test control;
            // the production query contains only its packed selector upload.
            for (std::size_t bucket = 0; bucket < offsets.size(); ++bucket) {
                direct_selectors[bucket].resize(6);
                for (std::size_t bit = 0; bit < 6; ++bit) {
                    std::vector<std::int64_t> polynomial(n, 0);
                    polynomial[0] = (offsets[bucket] >> bit) & 1;
                    spirit::encrypt_rgsw_rns(polynomial, direct_selectors[bucket][bit], encryptor, secret, context);
                }
            }
            const auto control_response = spirit_path_adapter::rotate_and_merge_rc_response(
                public_params, response, direct_selectors, rc_query.merge_galois_keys, context, evaluator);
            const auto unpacked_records = spirit_path_adapter::extract_rc_records(public_params, client, control_response, context, decryptor);
            const auto compressed = spirit_path_adapter::build_rc_response(public_params, response, rc_query, context, evaluator);
            const auto extracted = spirit_path_adapter::extract_rc_records(public_params, client, compressed, context, decryptor);
            if (extracted != unpacked_records) throw std::runtime_error("packed and unpacked selector path results differ");
            if (extracted.size() != indices.size()) throw std::runtime_error("wrong record count");
            for (std::size_t i = 0; i < indices.size(); ++i) {
                // Use the level/node oracle for path records, independently of
                // the BFS-flat database input conversion.
                auto expected = i <= height
                    ? path_workload::node_bytes(i, leaf >> (height - i))
                    : path_workload::record_bytes(indices[i]);
                if (!std::equal(expected.begin(), expected.end(), extracted[i].begin(), extracted[i].end())) {
                    throw std::runtime_error("H8 SPIRIT RC path mismatch at leaf " + std::to_string(leaf) + " entry " + std::to_string(i));
                }
            }
            std::cout << "H8 RC packed+unpacked leaf=" << leaf << " path=9/9 dummy=7/7 response_ct=1"
                      << " noise_budget_bits=" << decryptor.invariant_noise_budget(compressed.merged) << " PASS\n";
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
