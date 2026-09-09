#include "tree_response_merge.h"

#include "tree_superrecord_common.h"
#include "tree_rotate.h"
#include "utils.h"

#include "hexl/hexl.hpp"

namespace tree_superrecord {

const char *response_packing_name(ResponsePacking packing) {
  return packing == ResponsePacking::MlweMerge ? "mlwe" : "projection";
}

size_t packed_residue_offset(size_t offset, size_t rho, ResponsePacking packing) {
  const size_t r = exact_log2(rho, "response residue capacity");
  require(offset < rho, "response residue offset exceeds capacity");
  return packing == ResponsePacking::MlweMerge ? utils::bit_reverse(offset, r)
                                               : offset;
}

size_t mlwe_merge_keyswitch_count(size_t input_count, size_t rho) {
  const size_t r = exact_log2(rho, "response residue capacity");
  require(input_count > 0 && input_count <= rho, "merge input count exceeds capacity");
  size_t count = 0;
  for (size_t level = 0; level < r; ++level) {
    input_count = ceil_div(input_count, 2);
    count += input_count;
  }
  return count;
}

RlweCt mlwe_merge_residues(std::vector<RlweCt> inputs, size_t rho,
                           const bvks::BvGaloisKeys &keys,
                           const PirParams &scheme, size_t *keyswitch_count) {
  const size_t n = scheme.get_poly_degree();
  const auto &qs = scheme.get_rns_mods();
  const size_t r = exact_log2(rho, "response residue capacity");
  require(rho <= n && !inputs.empty() && inputs.size() <= rho,
          "MLWE merge requires between one and rho inputs, with rho <= n");
  if (keyswitch_count) *keyswitch_count = 0;
  for (const auto &ct : inputs) {
    require(!ct.ntt_form && ct.c0.size() == qs.size() * n &&
                ct.c1.size() == qs.size() * n,
            "MLWE merge requires coefficient-form full-q ciphertexts");
  }
  // Normalize before switching: the complete trace multiplies each surviving
  // coefficient by rho. Scaling after the switches would magnify their noise.
  for (size_t k = 0; k < qs.size(); ++k) {
    uint64_t inv = 0;
    require(utils::try_invert_uint_mod(rho, qs[k], inv),
            "MLWE trace normalization is not invertible");
    for (auto &ct : inputs) {
      intel::hexl::EltwiseFMAMod(ct.c0.data() + k * n,
                                ct.c0.data() + k * n, inv, nullptr, n, qs[k], 1);
      intel::hexl::EltwiseFMAMod(ct.c1.data() + k * n,
                                ct.c1.data() + k * n, inv, nullptr, n, qs[k], 1);
    }
  }
  // Reverse trace order lets each pair share one key switch:
  // (A + X^w B) + tau_eta(A - X^w B), since tau_eta(X^w) = -X^w.
  // Adjacent leaves then occupy BitRev_r(i), not the linear residue order.
  for (size_t remaining = r; remaining > 0; --remaining) {
    const size_t v = remaining - 1;
    const size_t w = size_t{1} << v;
    const uint32_t eta = static_cast<uint32_t>((n >> v) + 1);
    const auto &key = keys.get(eta);
    std::vector<RlweCt> next;
    next.reserve(ceil_div(inputs.size(), 2));
    for (size_t i = 0; i < inputs.size(); i += 2) {
      RlweCt sum = std::move(inputs[i]);
      RlweCt difference = sum;
      if (i + 1 < inputs.size()) {
        const RlweCt shifted = mul_x_pow(inputs[i + 1], w, scheme);
        tree_ct_add_inplace(sum, shifted, scheme);
        for (size_t k = 0; k < qs.size(); ++k) {
          intel::hexl::EltwiseSubMod(difference.c0.data() + k * n,
                                    difference.c0.data() + k * n,
                                    shifted.c0.data() + k * n, n, qs[k]);
          intel::hexl::EltwiseSubMod(difference.c1.data() + k * n,
                                    difference.c1.data() + k * n,
                                    shifted.c1.data() + k * n, n, qs[k]);
        }
      }
      // A missing right subtree is public zero. Unary nodes still need this
      // trace, including the final rounds after only one ciphertext remains.
      bvks::bv_apply_galois_inplace(difference, eta, key, scheme);
      tree_ct_add_inplace(sum, difference, scheme);
      if (keyswitch_count) ++*keyswitch_count;
      next.push_back(std::move(sum));
    }
    inputs = std::move(next);
  }
  return std::move(inputs.front());
}

}  // namespace tree_superrecord
