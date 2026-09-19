#include "spirit_rc.h"

#include "SPIRIT/he/packlwes.h"
#include "seal/util/polyarithsmallmod.h"
#include "seal/util/uintarithsmallmod.h"

#include <chrono>
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
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

template <typename Serializable>
std::uint64_t serialized_size_bytes(const Serializable &obj)
{
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    return static_cast<std::uint64_t>(obj.save(stream, seal::compr_mode_type::none));
}

std::size_t exact_log2(std::size_t value)
{
    if (value == 0 || (value & (value - 1)) != 0) {
        throw std::invalid_argument("SPIRIT RC residue capacity must be a power of two");
    }
    std::size_t bits = 0;
    while (value > 1) {
        value >>= 1;
        ++bits;
    }
    return bits;
}

std::size_t residue_capacity(
    const spirit::pir::BatchPublicParams &params, const seal::SEALContext &context)
{
    const std::size_t n = context.first_context_data()->parms().poly_modulus_degree();
    const std::size_t d = params.params.record_size_bytes;
    if (d == 0 || n % d != 0 || params.params.rc_response_poly_degree != n) {
        throw std::invalid_argument("SPIRIT RC requires its public strided one-polynomial bucket layout");
    }
    const std::size_t rho = n / d;
    exact_log2(rho);
    if (params.pbc_public_params.num_buckets == 0 || params.pbc_public_params.num_buckets > rho) {
        throw std::invalid_argument("SPIRIT RC bucket count exceeds response residue capacity");
    }
    for (const auto &layout : params.bucket_layouts) {
        if (layout.rows != n || layout.byte_row_stride != rho || layout.items_per_column != rho) {
            throw std::invalid_argument("SPIRIT RC bucket does not use the declared strided layout");
        }
    }
    return rho;
}

std::size_t bit_reverse(std::size_t value, std::size_t bits)
{
    std::size_t reversed = 0;
    for (std::size_t i = 0; i < bits; ++i) {
        reversed = (reversed << 1) | (value & 1);
        value >>= 1;
    }
    return reversed;
}

seal::Ciphertext private_rotate_to_zero(
    const seal::Ciphertext &input,
    const std::vector<spirit::RNSRGSWCiphertext> &offset_bits,
    const seal::SEALContext &context, seal::Evaluator &evaluator)
{
    seal::Ciphertext current = input;
    if (current.is_ntt_form()) {
        evaluator.transform_from_ntt_inplace(current);
    }
    for (std::size_t bit = 0; bit < offset_bits.size(); ++bit) {
        seal::Ciphertext rotated;
        spirit::multiply_rlwe_mono(current, rotated, std::size_t{1} << bit, false, context);
        evaluator.sub_inplace(rotated, current);
        seal::Ciphertext delta;
        // The upstream external-product interface does not modify the selector.
        spirit::external_product(rotated,
            const_cast<spirit::RNSRGSWCiphertext &>(offset_bits[bit]), delta, context, evaluator);
        evaluator.add_inplace(current, delta);
    }
    return current;
}

seal::Ciphertext merge_residues(
    std::vector<seal::Ciphertext> inputs, std::size_t rho,
    const seal::GaloisKeys &keys, const seal::SEALContext &context,
    seal::Evaluator &evaluator, std::size_t &keyswitches)
{
    const std::size_t n = inputs.front().poly_modulus_degree();
    const std::size_t levels = exact_log2(rho);
    // A full trace multiplies surviving coefficients by rho. Normalize in R_q
    // before key switching, so switch noise is never multiplied by rho^-1.
    // BFV multiply_plain(rho^-1 mod t) is a different operation and is unsuitable.
    for (auto &cipher : inputs) {
        const auto &moduli = context.get_context_data(cipher.parms_id())->parms().coeff_modulus();
        for (std::size_t k = 0; k < moduli.size(); ++k) {
            std::uint64_t inverse = 0;
            if (!seal::util::try_invert_uint_mod(rho, moduli[k], inverse)) {
                throw std::invalid_argument("SPIRIT RC trace normalization is not invertible");
            }
            for (std::size_t component = 0; component < cipher.size(); ++component) {
                auto *coefficients = cipher.data(component) + k * n;
                seal::util::multiply_poly_scalar_coeffmod(coefficients, n, inverse, moduli[k], coefficients);
            }
        }
    }
    keyswitches = 0;
    // Reverse trace order merges adjacent inputs with one switch per pair:
    // (A + X^w B) + tau_(n/w+1)(A - X^w B).
    // This projects all unwanted residues, even for dense input polynomials.
    for (std::size_t remaining = levels; remaining > 0; --remaining) {
        const std::size_t v = remaining - 1;
        const std::size_t w = std::size_t{1} << v;
        const auto eta = static_cast<std::uint32_t>((n >> v) + 1);
        std::vector<seal::Ciphertext> next;
        next.reserve((inputs.size() + 1) / 2);
        for (std::size_t i = 0; i < inputs.size(); i += 2) {
            seal::Ciphertext sum = std::move(inputs[i]);
            seal::Ciphertext difference = sum;
            if (i + 1 < inputs.size()) {
                spirit::multiply_rlwe_mono_inplace(inputs[i + 1], w, true, context);
                evaluator.add_inplace(sum, inputs[i + 1]);
                evaluator.sub_inplace(difference, inputs[i + 1]);
            }
            // Unary nodes still require their trace, including after the input
            // vector shrinks to one polynomial before the last round.
            evaluator.apply_galois_inplace(difference, eta, keys);
            evaluator.add_inplace(sum, difference);
            ++keyswitches;
            next.push_back(std::move(sum));
        }
        inputs = std::move(next);
    }
    return std::move(inputs.front());
}

std::uint8_t decode_lwe_slot(std::uint64_t coefficient, std::uint64_t lwe_modulus,
                             std::uint64_t plain_modulus)
{
    const std::uint64_t delta = (lwe_modulus + plain_modulus / 2) / plain_modulus;
    return static_cast<std::uint8_t>(((coefficient + delta / 2) / delta) % plain_modulus);
}

} // namespace

RcQuery build_rc_query(
    const spirit::pir::BatchPublicParams &public_params,
    const spirit::pir::BatchClientState &client_state,
    const seal::SEALContext &context, seal::KeyGenerator &keygen,
    seal::Encryptor &encryptor, seal::SecretKey &secret_key)
{
    const std::size_t rho = residue_capacity(public_params, context);
    const std::size_t bits = exact_log2(rho);
    const std::size_t n = context.first_context_data()->parms().poly_modulus_degree();
    const std::size_t buckets = public_params.pbc_public_params.num_buckets;
    RcQuery query;
    std::vector<std::uint32_t> elements;
    for (std::size_t v = 0; v < bits; ++v) {
        elements.push_back(static_cast<std::uint32_t>((n >> v) + 1));
    }
    keygen.create_galois_keys(elements, query.merge_galois_keys);
    query.merge_galois_key_bytes = serialized_size_bytes(query.merge_galois_keys);
    std::vector<std::uint64_t> offsets(buckets, 0);
    for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
        std::size_t offset = 0;
        const std::size_t position = client_state.query_plan.bucket_to_query_position[bucket];
        if (position != spirit::pir::pbc_invalid_index) {
            const auto &assignment = client_state.query_plan.assignments[position];
            offset = public_params.bucket_layouts[bucket].item_layouts[assignment.placement.slot_index].start_row_index;
        }
        if (offset >= rho) {
            throw std::invalid_argument("SPIRIT RC private offset exceeds the record residue range");
        }
        offsets[bucket] = offset;
    }
    query.packed_selectors = pack_rc_selectors(offsets, bits, context, secret_key);
    query.selector_bytes = query.packed_selectors.ciphertext_bytes + query.packed_selectors.parameter_bytes;
    query.galois_key_bytes = query.merge_galois_key_bytes + query.packed_selectors.key_bytes;
    (void)encryptor;
    return query;
}

RcResponse build_rc_response(
    const spirit::pir::BatchPublicParams &public_params,
    const spirit::pir::BatchResponseMessage &response, const RcQuery &rc_query,
    const seal::SEALContext &context, seal::Evaluator &evaluator)
{
    std::vector<std::vector<spirit::RNSRGSWCiphertext>> selectors;
    const double unpack_ms = measure_ms([&]() {
        selectors = unpack_rc_selectors(rc_query.packed_selectors, context);
    });
    auto result = rotate_and_merge_rc_response(public_params, response, selectors,
                                               rc_query.merge_galois_keys, context, evaluator);
    result.selector_unpack_ms = unpack_ms;
    return result;
}

RcResponse rotate_and_merge_rc_response(
    const spirit::pir::BatchPublicParams &public_params,
    const spirit::pir::BatchResponseMessage &response,
    const std::vector<std::vector<spirit::RNSRGSWCiphertext>> &encrypted_offset_bits,
    const seal::GaloisKeys &merge_galois_keys,
    const seal::SEALContext &context, seal::Evaluator &evaluator)
{
    const std::size_t rho = residue_capacity(public_params, context);
    const std::size_t buckets = public_params.pbc_public_params.num_buckets;
    if (response.bucket_responses.size() != buckets || encrypted_offset_bits.size() != buckets) {
        throw std::invalid_argument("SPIRIT RC response/query bucket count mismatch");
    }
    RcResponse out;
    std::vector<seal::Ciphertext> rotated(buckets);
    out.rotate_ms = measure_ms([&]() {
        for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
            const auto &chunks = response.bucket_responses[bucket].packed_columns;
            if (chunks.size() != 1 || encrypted_offset_bits[bucket].size() != exact_log2(rho)) {
                throw std::invalid_argument("SPIRIT RC requires one response chunk and log2(rho) selectors per bucket");
            }
            rotated[bucket] = private_rotate_to_zero(chunks[0], encrypted_offset_bits[bucket], context, evaluator);
            out.rotation_external_products += encrypted_offset_bits[bucket].size();
        }
    });
    out.merge_ms = measure_ms([&]() {
        out.merged = merge_residues(std::move(rotated), rho, merge_galois_keys,
                                   context, evaluator, out.merge_keyswitches);
    });
    out.response_bytes = serialized_size_bytes(out.merged);
    return out;
}

std::vector<std::vector<std::uint8_t>> extract_rc_records(
    const spirit::pir::BatchPublicParams &public_params,
    const spirit::pir::BatchClientState &client_state, const RcResponse &response,
    const seal::SEALContext &context, seal::Decryptor &decryptor)
{
    const std::size_t rho = residue_capacity(public_params, context);
    const std::size_t bits = exact_log2(rho);
    const std::size_t d = public_params.params.record_size_bytes;
    const std::uint64_t packed_modulus = context.first_context_data()->parms().plain_modulus().value();
    seal::Plaintext plain;
    decryptor.decrypt(response.merged, plain);
    std::vector<std::vector<std::uint8_t>> records(client_state.batch_indices.size(), std::vector<std::uint8_t>(d));
    for (const auto &assignment : client_state.query_plan.assignments) {
        const std::size_t residue = bit_reverse(assignment.placement.bucket_index, bits);
        auto &record = records[assignment.query_position];
        for (std::size_t byte = 0; byte < d; ++byte) {
            const std::size_t coefficient = residue + byte * rho;
            const std::uint64_t value = coefficient < plain.coeff_count() ? plain[coefficient] : 0;
            record[byte] = decode_lwe_slot(value, packed_modulus, public_params.params.record_plain_modulus);
        }
    }
    return records;
}

} // namespace spirit_path_adapter
