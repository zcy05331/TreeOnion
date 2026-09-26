#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <vector>

#include "pir.h"
#include "rlwe.h"

// Seeded query encoding, as in OnionPIR and Spiral: the uniform component c1
// of a fresh query is expanded from a public 32-byte seed with ChaCha20, so a
// query travels as the seed followed by c0. The seed is written as eight
// little-endian 32-bit words; c0 as N integers modulo the composite
// q = q_0 q_1 (K <= 2 limbs), each in ceil(log2 q) bits, LSB-first, with the
// last byte zero-padded. A query thus takes 32 + ceil(N ceil(log2 q) / 8)
// bytes. Like the response codec, the format has no authentication and the
// reader does not check trailing bytes.
using QuerySeed = std::array<uint32_t, 8>;

// Regenerates c1 (limb-major, coefficient form) from the seed, in the order
// encrypt_zero_rns draws its uniform component.
void expand_query_mask(const QuerySeed &seed, size_t N,
                       const std::vector<uint64_t> &qs, uint64_t *c1);

// Bytes of one seeded query under `params`.
size_t seeded_query_bytes(const PirParams &params);

// Writes a coefficient-form query whose c1 was expanded from `seed` and
// returns the number of bytes written. c1 itself is not written.
size_t save_seeded_query(const RlweCt &query, const QuerySeed &seed,
                         const PirParams &params, std::stringstream &stream);

// Reads a query written by save_seeded_query and regenerates its c1.
RlweCt load_seeded_query(std::stringstream &stream, const PirParams &params);
