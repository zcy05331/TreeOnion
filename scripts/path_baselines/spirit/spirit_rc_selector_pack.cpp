#include "spirit_rc_selector_pack.h"

#include "SPIRIT/he/query_unpack.h"
#include "seal/util/ntt.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace spirit_path_adapter {
namespace {

template <typename Serializable>
std::uint64_t serialized_bytes(const Serializable &value)
{
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    return static_cast<std::uint64_t>(value.save(stream, seal::compr_mode_type::none));
}

seal::EncryptionParameters expansion_parameters(const seal::SEALContext &response)
{
    auto params = response.key_context_data()->parms();
    auto moduli = params.coeff_modulus();
    // Expansion switches at q * P and therefore needs a distinct extra special
    // prime. The response context itself and its existing primes are unchanged.
    const auto candidates = seal::CoeffModulus::Create(params.poly_modulus_degree(),
                                                       std::vector<int>(moduli.size() + 1, 60));
    for (const auto &candidate : candidates) {
        if (std::find(moduli.begin(), moduli.end(), candidate) == moduli.end()) {
            moduli.push_back(candidate);
            params.set_coeff_modulus(moduli);
            return params;
        }
    }
    throw std::runtime_error("no distinct RC expansion special prime");
}

seal::SecretKey extend_secret(const seal::SecretKey &response_secret,
                             const seal::SEALContext &response,
                             const seal::SEALContext &expansion)
{
    const auto &small = *response.key_context_data();
    const auto &large = *expansion.key_context_data();
    const std::size_t n = small.parms().poly_modulus_degree();
    const auto &moduli = large.parms().coeff_modulus();
    std::vector<std::uint64_t> coefficients(response_secret.data().data(), response_secret.data().data() + n);
    seal::util::inverse_ntt_negacyclic_harvey(coefficients.data(), small.small_ntt_tables()[0]);
    const auto source_modulus = small.parms().coeff_modulus()[0].value();
    seal::SecretKey extended;
    extended.data().resize(n * moduli.size());
    for (std::size_t k = 0; k < moduli.size(); ++k) {
        auto *out = extended.data().data() + k * n;
        for (std::size_t j = 0; j < n; ++j) {
            // SEAL generates a ternary secret. Extend its signed coefficients,
            // not its NTT residues, into the added modulus.
            out[j] = coefficients[j] == source_modulus - 1 ? moduli[k].value() - 1 : coefficients[j];
        }
        seal::util::ntt_negacyclic_harvey(out, large.small_ntt_tables()[k]);
    }
    extended.data().parms_id() = expansion.key_parms_id();
    return extended;
}

} // namespace

PackedRcSelectors pack_rc_selectors(
    const std::vector<std::uint64_t> &private_offsets, std::size_t value_bits,
    const seal::SEALContext &response_context, const seal::SecretKey &response_secret)
{
    PackedRcSelectors query;
    query.bucket_count = private_offsets.size();
    query.value_bits = value_bits;
    query.expansion_parameters = expansion_parameters(response_context);
    seal::SEALContext expansion(query.expansion_parameters, true, seal::sec_level_type::none);
    if (expansion.first_parms_id() != response_context.key_parms_id()) {
        throw std::invalid_argument("RC expansion data primes must equal response key primes");
    }
    auto secret = extend_secret(response_secret, response_context, expansion);
    seal::KeyGenerator keys(expansion, secret);
    std::vector<std::uint32_t> elements;
    const std::size_t n = query.expansion_parameters.poly_modulus_degree();
    for (std::size_t step = 2; step <= n; step <<= 1) {
        elements.push_back(static_cast<std::uint32_t>(step + 1));
    }
    keys.create_galois_keys(elements, query.expansion_galois_keys);
    keys.create_relin_keys(query.expansion_relin_keys);
    // These client-only objects satisfy the legacy encrypt helper's unused
    // Encryptor/Decryptor parameters; the server API has neither argument.
    auto small_secret = response_secret;
    seal::Encryptor encryptor(response_context, small_secret);
    seal::Decryptor decryptor(response_context, small_secret);
    auto offsets = private_offsets;
    spirit::hint_query_encrypt_rns(offsets, value_bits, query.packed_bits,
                                  response_context, small_secret, encryptor, decryptor);
    for (const auto &cipher : query.packed_bits) query.ciphertext_bytes += serialized_bytes(cipher);
    query.key_bytes = serialized_bytes(query.expansion_galois_keys) + serialized_bytes(query.expansion_relin_keys);
    query.parameter_bytes = serialized_bytes(query.expansion_parameters);
    return query;
}

std::vector<std::vector<spirit::RNSRGSWCiphertext>> unpack_rc_selectors(
    const PackedRcSelectors &query, const seal::SEALContext &response_context)
{
    seal::SEALContext expansion(query.expansion_parameters, true, seal::sec_level_type::none);
    if (expansion.first_parms_id() != response_context.key_parms_id()) {
        throw std::invalid_argument("RC expansion data primes must equal response key primes");
    }
    seal::Evaluator evaluator(expansion);
    const std::size_t n = expansion.first_context_data()->parms().poly_modulus_degree();
    const std::size_t decomp_count = response_context.first_context_data()->parms().coeff_modulus().size();
    const std::size_t slot_count = query.bucket_count * query.value_bits;
    std::vector<std::vector<spirit::RNSRGSWCiphertext>> output(query.bucket_count);
    for (auto &bucket : output) {
        bucket.resize(query.value_bits);
        for (auto &selector : bucket) {
            selector.parms_id() = response_context.first_parms_id();
            selector.data().resize(2);
            selector.data()[0].resize(decomp_count);
            selector.data()[1].resize(decomp_count);
        }
    }
    // Equivalent to upstream unpack_hint_query_rns, using its actual public
    // offline/online operations directly instead of forwarding a decryptor.
    for (std::size_t packed = 0; packed < query.packed_bits.size(); ++packed) {
        for (std::size_t t = 0; t < decomp_count; ++t) {
            auto cipher = query.packed_bits[packed].data()[0][t].data();
            spirit::RLWEUnpackPrecomp precomputed;
            spirit::unpack_rlwe_precompute_offline(cipher, n, expansion, query.expansion_galois_keys, evaluator, precomputed);
            std::vector<seal::Ciphertext> bits;
            spirit::unpack_rlwe_apply_online(cipher, precomputed, expansion, query.expansion_galois_keys, evaluator, bits);
            for (std::size_t j = 0; j < n && packed * n + j < slot_count; ++j) {
                const std::size_t slot = packed * n + j;
                auto &selector = output[slot / query.value_bits][slot % query.value_bits];
                auto first = bits[j];
                if (first.is_ntt_form()) evaluator.transform_from_ntt_inplace(first);
                auto second = first;
                // secret_switch maps encryption of m to encryption of s*m,
                // generating the second RGSW row using a public relinearization key.
                spirit::secret_switch_inplace(second, evaluator, query.expansion_relin_keys);
                evaluator.transform_to_ntt_inplace(first);
                evaluator.transform_to_ntt_inplace(second);
                selector.data()[0][t].data() = std::move(first);
                selector.data()[1][t].data() = std::move(second);
            }
        }
    }
    return output;
}

} // namespace spirit_path_adapter
