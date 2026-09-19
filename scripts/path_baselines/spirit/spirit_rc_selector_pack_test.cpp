#include "spirit_rc_selector_pack.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

int main()
{
    try {
        seal::EncryptionParameters params(seal::scheme_type::bfv);
        params.set_poly_modulus_degree(2048);
        params.set_coeff_modulus(seal::CoeffModulus::Create(2048, {56, 60}));
        params.set_plain_modulus(65537);
        seal::SEALContext context(params, true, seal::sec_level_type::none);
        seal::KeyGenerator keygen(context);
        auto secret = keygen.secret_key();
        seal::Encryptor encryptor(context, secret);
        seal::Evaluator evaluator(context);
        seal::Decryptor decryptor(context, secret);
        std::vector<std::uint64_t> offsets(48);
        for (std::size_t i = 0; i < offsets.size(); ++i) offsets[i] = (i * 19) % 64;
        const auto packed = spirit_path_adapter::pack_rc_selectors(offsets, 6, context, secret);
        const auto start = std::chrono::steady_clock::now();
        auto unpacked = spirit_path_adapter::unpack_rc_selectors(packed, context);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        seal::Plaintext plain(2048);
        for (std::size_t i = 0; i < 2048; ++i) plain[i] = 256 * (1 + i % 255);
        seal::Ciphertext encrypted;
        encryptor.encrypt_symmetric(plain, encrypted);
        for (std::size_t bucket = 0; bucket < offsets.size(); ++bucket) {
            for (std::size_t bit = 0; bit < 6; ++bit) {
                seal::Ciphertext selected;
                spirit::external_product(encrypted, unpacked[bucket][bit], selected, context, evaluator);
                seal::Plaintext result;
                decryptor.decrypt(selected, result);
                for (std::size_t i = 0; i < 2048; ++i) {
                    const auto expected = ((offsets[bucket] >> bit) & 1) ? plain[i] : 0;
                    const auto actual = i < result.coeff_count() ? result[i] : 0;
                    if (actual != expected) {
                        throw std::runtime_error("packed selector mismatch bucket=" + std::to_string(bucket) +
                                                 " bit=" + std::to_string(bit) + " coefficient=" + std::to_string(i));
                    }
                }
            }
        }
        std::cout << "packed selectors: bits=288 encrypted_external_products=288 PASS\n"
                  << "ciphertext_bytes=" << packed.ciphertext_bytes << " key_bytes=" << packed.key_bytes
                  << " parameter_bytes=" << packed.parameter_bytes << " unpack_ms=" << ms << '\n';
        for (const auto &prime : packed.expansion_parameters.coeff_modulus()) std::cout << "expansion_prime=" << prime.value() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
