#include "spirit_rc.h"
#include "SPIRIT/he/packlwes.h"

#include <iostream>
#include <stdexcept>

namespace {

std::uint8_t dense_byte(std::size_t bucket, std::size_t coefficient)
{
    return static_cast<std::uint8_t>(1 + (31 * coefficient + 47 * bucket + coefficient / 7) % 255);
}

std::size_t reverse_six(std::size_t value)
{
    std::size_t out = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        out = (out << 1) | (value & 1);
        value >>= 1;
    }
    return out;
}

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void run_dense_case(std::size_t bucket_count, const seal::SEALContext &context,
                    seal::KeyGenerator &keygen, seal::SecretKey &secret,
                    seal::Encryptor &encryptor, seal::Evaluator &evaluator,
                    seal::Decryptor &decryptor)
{
    constexpr std::size_t n = 2048;
    constexpr std::size_t d = 32;
    constexpr std::size_t rho = n / d;
    spirit::pir::BatchPublicParams params;
    params.params.record_size_bytes = d;
    params.params.record_plain_modulus = 256;
    params.params.rc_response_poly_degree = n;
    params.pbc_public_params.num_buckets = bucket_count;
    params.bucket_layouts.resize(bucket_count);
    spirit::pir::BatchClientState client;
    client.batch_indices.resize(bucket_count);
    client.query_plan.bucket_to_query_position.resize(bucket_count);
    spirit::pir::BatchResponseMessage response;
    response.bucket_responses.resize(bucket_count);
    std::vector<std::vector<std::uint8_t>> expected(bucket_count, std::vector<std::uint8_t>(d));
    seal::Ciphertext legacy_sum;
    for (std::size_t bucket = 0; bucket < bucket_count; ++bucket) {
        auto &layout = params.bucket_layouts[bucket];
        layout.rows = n;
        layout.cols = 2;
        layout.items_per_column = rho;
        layout.item_slots = 2 * rho;
        layout.record_size_bytes = d;
        layout.byte_row_stride = rho;
        layout.item_layouts.resize(2 * rho);
        for (std::size_t slot = 0; slot < 2 * rho; ++slot) {
            layout.item_layouts[slot] = {slot / rho, slot % rho};
        }
        // Includes offsets 0, 63 and internal residues; select both public columns.
        const std::size_t offset = bucket == 0 ? 0 : (bucket * 19 + 44) % rho;
        const std::size_t slot = (bucket % 2) * rho + offset;
        client.batch_indices[bucket] = bucket;
        client.query_plan.bucket_to_query_position[bucket] = bucket;
        client.query_plan.assignments.push_back({bucket, bucket, {bucket, slot}});
        seal::Plaintext plain(n);
        for (std::size_t coefficient = 0; coefficient < n; ++coefficient) {
            plain[coefficient] = static_cast<std::uint64_t>(dense_byte(bucket, coefficient)) * 256;
        }
        for (std::size_t byte = 0; byte < d; ++byte) {
            expected[bucket][byte] = dense_byte(bucket, offset + byte * rho);
        }
        auto &bucket_response = response.bucket_responses[bucket];
        bucket_response.bucket_index = bucket;
        bucket_response.packed_columns.resize(1);
        encryptor.encrypt_symmetric(plain, bucket_response.packed_columns[0]);
        // This negative control is the old shift-and-add idea, with plaintext
        // offsets only inside this test. Dense records must expose contamination.
        seal::Ciphertext legacy = bucket_response.packed_columns[0];
        if (offset != 0) spirit::multiply_rlwe_mono_inplace(legacy, offset, false, context);
        spirit::multiply_rlwe_mono_inplace(legacy, reverse_six(bucket), true, context);
        if (bucket == 0) legacy_sum = legacy;
        else evaluator.add_inplace(legacy_sum, legacy);
    }
    const auto query = spirit_path_adapter::build_rc_query(params, client, context, keygen, encryptor, secret);
    const auto compressed = spirit_path_adapter::build_rc_response(params, response, query, context, evaluator);
    const auto actual = spirit_path_adapter::extract_rc_records(params, client, compressed, context, decryptor);
    require(actual == expected, "dense RC record mismatch: private rotation or full merge projection is wrong");
    require(compressed.rotation_external_products == bucket_count * 6, "unexpected rotation external product count");
    std::size_t expected_switches = 0;
    std::size_t nodes = bucket_count;
    for (std::size_t level = 0; level < 6; ++level) {
        nodes = (nodes + 1) / 2;
        expected_switches += nodes;
    }
    require(compressed.merge_keyswitches == expected_switches, "unexpected merge switch count");
    if (bucket_count > 1) {
        seal::Plaintext legacy_plain;
        decryptor.decrypt(legacy_sum, legacy_plain);
        bool contaminated = false;
        for (std::size_t bucket = 0; bucket < bucket_count; ++bucket) {
            for (std::size_t byte = 0; byte < d; ++byte) {
                auto value = static_cast<std::uint8_t>(((legacy_plain[reverse_six(bucket) + byte * rho] + 128) / 256) % 256);
                contaminated |= value != expected[bucket][byte];
            }
        }
        require(contaminated, "dense negative control did not distinguish the old incorrect merge");
    }
    std::cout << "dense RC: buckets=" << bucket_count << " records=" << actual.size()
              << " external_products=" << compressed.rotation_external_products
              << " switches=" << compressed.merge_keyswitches << " PASS\n";
}

} // namespace

int main()
{
    try {
        seal::EncryptionParameters parms(seal::scheme_type::bfv);
        parms.set_poly_modulus_degree(2048);
        parms.set_coeff_modulus(seal::CoeffModulus::Create(2048, {56, 60}));
        parms.set_plain_modulus(65537);
        // Match the public SPIRIT artifact parameter set. SEAL's standard
        // security gate rejects its 116-bit modulus at degree 2048.
        seal::SEALContext context(parms, true, seal::sec_level_type::none);
        seal::KeyGenerator keygen(context);
        auto secret = keygen.secret_key();
        seal::Encryptor encryptor(context, secret);
        seal::Evaluator evaluator(context);
        seal::Decryptor decryptor(context, secret);
        for (std::size_t buckets : {1U, 2U, 3U, 48U, 64U}) {
            run_dense_case(buckets, context, keygen, secret, encryptor, evaluator, decryptor);
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
