#pragma once

#include "pir.h"
#include "rlwe.h"
#include "database_constants.h"
#include <vector>
#include <cstdint>
#include <random>
#include "csprng.h"

namespace bvks {

constexpr size_t L_KS = DBConsts::L_KS;

struct BvRlweCt {
  std::vector<uint64_t> a;
  std::vector<uint64_t> b;
};

struct BvKeySwitchKey {
  uint32_t galois_k = 0;
  std::vector<BvRlweCt> cts;
};

class BvGaloisKeys {
public:
  std::vector<BvKeySwitchKey> keys;

  const BvKeySwitchKey &get(uint32_t galois_k) const;
  bool contains(uint32_t galois_k) const noexcept;
};

size_t gadget_rows();
size_t base_log2_for_scheme(const PirParams &pir_params);
size_t low_bits_for_scheme(const PirParams &pir_params);

BvKeySwitchKey gen_bv_ks_key(const PirParams &pir_params,
                             const RlweSk &sk, uint32_t galois_k,
                             ChaChaRng &rng);

BvGaloisKeys gen_bv_galois_keys(const PirParams &pir_params,
                                const RlweSk &sk);

void signed_gadget_decompose(uint64_t val, size_t base_log2,
                             uint64_t q, uint64_t *out, size_t num_digits,
                             size_t low_bits = 0);

void signed_gadget_decompose_mp(uint128_t val, uint128_t Q, size_t base_log2,
                                int64_t *out, size_t num_digits);

void bv_apply_galois_inplace(RlweCt &ct, uint32_t galois_k,
                             const BvKeySwitchKey &key,
                             const PirParams &pir_params);

}
