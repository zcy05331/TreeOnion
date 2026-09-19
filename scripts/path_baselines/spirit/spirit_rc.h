#pragma once

#include "SPIRIT/he/rgsw_ciphertext.h"
#include "spirit_rc_selector_pack.h"
#include "SPIRIT/pir/types.h"
#include "seal/seal.h"

#include <cstdint>
#include <vector>

namespace spirit_path_adapter {

struct RcQuery {
    PackedRcSelectors packed_selectors;
    seal::GaloisKeys merge_galois_keys;
    std::uint64_t selector_bytes = 0;
    std::uint64_t galois_key_bytes = 0;
    std::uint64_t merge_galois_key_bytes = 0;
};

struct RcResponse {
    seal::Ciphertext merged;
    std::uint64_t response_bytes = 0;
    double selector_unpack_ms = 0.0;
    double rotate_ms = 0.0;
    double merge_ms = 0.0;
    std::size_t rotation_external_products = 0;
    std::size_t merge_keyswitches = 0;
};

RcQuery build_rc_query(
    const spirit::pir::BatchPublicParams &public_params,
    const spirit::pir::BatchClientState &client_state,
    const seal::SEALContext &context,
    seal::KeyGenerator &keygen,
    seal::Encryptor &encryptor,
    seal::SecretKey &secret_key);

RcResponse build_rc_response(
    const spirit::pir::BatchPublicParams &public_params,
    const spirit::pir::BatchResponseMessage &response,
    const RcQuery &rc_query,
    const seal::SEALContext &context,
    seal::Evaluator &evaluator);

// Server primitive over already-expanded encrypted selectors. The public
// query route expands its packed upload before calling this same operation.
RcResponse rotate_and_merge_rc_response(
    const spirit::pir::BatchPublicParams &public_params,
    const spirit::pir::BatchResponseMessage &response,
    const std::vector<std::vector<spirit::RNSRGSWCiphertext>> &encrypted_offset_bits,
    const seal::GaloisKeys &merge_galois_keys,
    const seal::SEALContext &context,
    seal::Evaluator &evaluator);

std::vector<std::vector<std::uint8_t>> extract_rc_records(
    const spirit::pir::BatchPublicParams &public_params,
    const spirit::pir::BatchClientState &client_state,
    const RcResponse &response,
    const seal::SEALContext &context,
    seal::Decryptor &decryptor);

} // namespace spirit_path_adapter
