#pragma once

#include "SPIRIT/he/rgsw_ciphertext.h"
#include "seal/seal.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace spirit_path_adapter {

// Public upload only: no offsets, secret key, or decryptor is retained here.
struct PackedRcSelectors {
    std::size_t bucket_count = 0;
    std::size_t value_bits = 0;
    seal::EncryptionParameters expansion_parameters{seal::scheme_type::bfv};
    std::vector<spirit::RNSRGSWCiphertext> packed_bits;
    seal::GaloisKeys expansion_galois_keys;
    seal::RelinKeys expansion_relin_keys;
    std::uint64_t ciphertext_bytes = 0;
    std::uint64_t key_bytes = 0;
    std::uint64_t parameter_bytes = 0;
};

PackedRcSelectors pack_rc_selectors(
    const std::vector<std::uint64_t> &private_offsets, std::size_t value_bits,
    const seal::SEALContext &response_context, const seal::SecretKey &response_secret);

std::vector<std::vector<spirit::RNSRGSWCiphertext>> unpack_rc_selectors(
    const PackedRcSelectors &query, const seal::SEALContext &response_context);

} // namespace spirit_path_adapter
