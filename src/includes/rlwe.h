#pragma once
#include <cstdint>
#include <random>
#include <vector>
#include "csprng.h"

struct RlweCt {
    std::vector<uint64_t> c0;
    std::vector<uint64_t> c1;
    bool ntt_form = false;

    uint64_t       *data(size_t i)       { return i == 0 ? c0.data() : c1.data(); }
    const uint64_t *data(size_t i) const { return i == 0 ? c0.data() : c1.data(); }
    bool &is_ntt_form() { return ntt_form; }

    void resize(size_t n) { c0.assign(n, 0); c1.assign(n, 0); }

    size_t poly_size() const { return c0.size(); }
};

struct RlweSk {
    std::vector<uint64_t> data;
    size_t poly_size() const { return data.size(); }
};

struct RlwePt {
    std::vector<uint64_t> data;
    size_t coeff_count() const { return data.size(); }
};

RlweSk gen_secret_key(size_t N, uint64_t q, ChaChaRng &rng);

void encrypt_zero(const RlweSk &sk, size_t N, uint64_t q, double sigma,
                  ChaChaRng &rng, RlweCt &ct, bool ntt_form = false);

void encrypt_bfv(const std::vector<uint64_t> &m, const RlweSk &sk,
                 size_t N, uint64_t q, uint64_t t, double sigma,
                 ChaChaRng &rng, RlweCt &ct);

void decrypt(const RlweCt &ct, const RlweSk &sk, size_t N, uint64_t q,
             uint64_t t, RlwePt &pt);

int decrypt_and_budget(const RlweCt &ct, const RlweSk &sk, size_t N,
                       uint64_t q, uint64_t t, RlwePt &pt);

void rlwe_add_inplace(RlweCt &a, const RlweCt &b, uint64_t q);
void rlwe_sub_inplace(RlweCt &a, const RlweCt &b, uint64_t q);
void rlwe_add(const RlweCt &a, const RlweCt &b, RlweCt &c, uint64_t q);
void rlwe_sub(const RlweCt &a, const RlweCt &b, RlweCt &c, uint64_t q);

void rlwe_ntt_fwd_inplace(RlweCt &ct, uint64_t q, size_t N);
void rlwe_ntt_inv_inplace(RlweCt &ct, uint64_t q, size_t N);

void rlwe_shift(const RlweCt &src, RlweCt &dst, size_t index, uint64_t q, size_t N);

struct RnsTables;

RlweSk gen_secret_key_rns(size_t N, const std::vector<uint64_t> &qs,
                          ChaChaRng &rng);

void encrypt_zero_rns(const RlweSk &sk, size_t N,
                      const std::vector<uint64_t> &qs,
                      double sigma, ChaChaRng &rng,
                      RlweCt &ct, bool ntt_form = false);

void encrypt_zero_rns(const RlweSk &sk, size_t N,
                      const std::vector<uint64_t> &qs,
                      double sigma, ChaChaRng &mask_rng, ChaChaRng &error_rng,
                      RlweCt &ct, bool ntt_form = false);

void sample_uniform_rns(uint64_t *out, size_t N,
                        const std::vector<uint64_t> &qs, ChaChaRng &rng);

void encrypt_bfv_rns(const std::vector<uint64_t> &m, const RlweSk &sk,
                     size_t N, const std::vector<uint64_t> &qs, uint64_t t,
                     double sigma, ChaChaRng &rng, RlweCt &ct);

void decrypt_rns(const RlweCt &ct, const RlweSk &sk, size_t N,
                 const std::vector<uint64_t> &qs, uint64_t t,
                 const RnsTables &tables, RlwePt &pt);
