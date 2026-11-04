#include "client.h"
#include "rlwe.h"
#include "utils.h"

#include <stdexcept>

std::vector<size_t> PirClient::get_query_indices(
    const PirParams &query_params, size_t pt_idx) const {
  const size_t col_idx = pt_idx % query_params.get_fst_dim_sz();
  const size_t row_idx = pt_idx / query_params.get_fst_dim_sz();
  const size_t other_dim_sz = query_params.get_other_dim_sz();
  const size_t d = query_params.get_num_dims();
  const size_t h = d - 1;

  std::vector<size_t> query_indices = {col_idx};

  if (d == 1) {
    DEBUG_PRINT("Single dimension case - returning col_idx: " << col_idx);
    return query_indices;
  }

  const size_t r = 2 * other_dim_sz - (1 << h);
  const size_t sl = other_dim_sz - r;

  size_t perfect_idx;
  if (row_idx < other_dim_sz - r) {
    query_indices.push_back(0);
    perfect_idx = row_idx;
  } else {
    size_t corrected_idx = row_idx - sl;
    query_indices.push_back(corrected_idx % 2);
    perfect_idx = sl + corrected_idx / 2;
  }

  if (h > 1) {
    for (size_t k = h - 2; k + 1 > 0; k--) {
      query_indices.push_back((perfect_idx >> k) & 1ULL);
      if (k == 0) break;
    }
  }

  return query_indices;
}




RlweCt PirClient::fast_generate_query(const size_t pt_idx) {
  return fast_generate_query(pir_params_, pt_idx);
}

RlweCt PirClient::fast_generate_query(const PirParams &query_params,
                                      const size_t pt_idx) {
  if (!pir_params_.scheme_compatible(query_params)) {
    throw std::invalid_argument(
        "PirClient query layout is not scheme-compatible with its session");
  }
  if (pt_idx >= query_params.get_num_pt()) {
    throw std::out_of_range("PIR plaintext index exceeds the runtime layout");
  }

  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs_arr = query_params.get_rns_mods();
  const std::vector<uint64_t> qs(qs_arr.begin(), qs_arr.end());
  const double sigma = query_params.get_noise_std_dev();

  std::vector<size_t> query_indices =
      get_query_indices(query_params, pt_idx);
  PRINT_INT_ARRAY("\t\tquery_indices", query_indices.data(), query_indices.size());

  RlweCt query;
  encrypt_zero_rns(rlwe_sk_, N, qs, sigma, rng_, query, false);
  add_query_segment(query, query_params.get_expan_height(), 0, query_params,
                    query_indices, true);
  return query;
}


void PirClient::add_gsw_to_query(
    RlweCt &query, const std::vector<size_t> &query_indices) {
  add_gsw_to_query(pir_params_, query, query_indices);
}

void PirClient::add_gsw_to_query(
    const PirParams &query_params, RlweCt &query,
    const std::vector<size_t> &query_indices) {
  add_query_segment(query, query_params.get_expan_height(), 0, query_params,
                    query_indices, false);
}

void PirClient::add_query_segment(RlweCt &query, size_t pack_height,
                                  size_t offset, const PirParams &query_params,
                                  const std::vector<size_t> &query_indices,
                                  bool one_hot) {
  if (!pir_params_.scheme_compatible(query_params)) {
    throw std::invalid_argument(
        "PirClient query layout is not scheme-compatible with its session");
  }
  const size_t capacity = size_t{1} << pack_height;
  const size_t fst_dim_sz = query_params.get_fst_dim_sz();
  const size_t l = query_params.get_l();
  if (query_indices.empty() ||
      offset + fst_dim_sz + l * (query_indices.size() - 1) > capacity) {
    throw std::invalid_argument("query segment exceeds its expansion capacity");
  }
  constexpr size_t N = DBConsts::PolyDegree;
  const size_t K = query_params.K();
  const auto rns_mods = query_params.get_rns_mods();
  const uint64_t t = query_params.get_plain_mod();

  if (one_hot) {
    uint64_t inverse = 0;
    utils::try_invert_uint_mod(capacity, t, inverse);
    const size_t reversed_index =
        utils::bit_reverse(offset + query_indices[0], pack_height);
    if (K == 1) {
      const uint64_t Q = rns_mods[0];
      const uint64_t scaled = utils::round_div_u128((uint128_t)Q * inverse, t) % Q;
      query.c0[reversed_index] = (query.c0[reversed_index] + scaled) % Q;
    } else {
      const uint128_t Q = static_cast<uint128_t>(rns_mods[0]) * rns_mods[1];
      const uint128_t Delta = Q / t;
      const uint64_t r = static_cast<uint64_t>(Q - Delta * t);
      const uint64_t r_inverse_round =
          static_cast<uint64_t>((static_cast<uint128_t>(r) * inverse + (t >> 1)) / t);
      const uint128_t scaled_mp = Delta * inverse + r_inverse_round;
      for (size_t k = 0; k < K; ++k) {
        const uint64_t scaled_k = static_cast<uint64_t>(scaled_mp % rns_mods[k]);
        const size_t idx = k * N + reversed_index;
        query.c0[idx] = (query.c0[idx] + scaled_k) % rns_mods[k];
      }
    }
  }
  if (query_indices.size() == 1) return;

  std::vector<uint64_t> inv(K);
  for (size_t k = 0; k < K; k++) {
    uint64_t result;
    utils::try_invert_uint_mod(capacity, rns_mods[k], result);
    inv[k] = result;
  }

  std::vector<std::vector<uint64_t>> gadget =
      utils::gsw_gadget(l, query_params.get_base_log2(), rns_mods,
                        query_params.get_ep_low_bits());

  auto q_head = query.data(0);
  for (size_t i = 1; i < query_indices.size(); i++) {
    if (query_indices[i] != 1) continue;

    for (size_t k = 0; k < l; k++) {
      const size_t coef_pos = offset + fst_dim_sz + (i - 1) * l + k;
      const size_t reversed_idx = utils::bit_reverse(coef_pos, pack_height);
      for (size_t mod_id = 0; mod_id < K; mod_id++) {
        const size_t pad = mod_id * DBConsts::PolyDegree;
        inter_coeff_t mod = rns_mods[mod_id];
        uint64_t coef = (inter_coeff_t)gadget[mod_id][k] * inv[mod_id] % mod;
        q_head[reversed_idx + pad] = (q_head[reversed_idx + pad] + coef) % mod;
      }
    }
  }
}

RlweCt PirClient::fresh_zero_ct() {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs_arr = pir_params_.get_rns_mods();
  const std::vector<uint64_t> qs(qs_arr.begin(), qs_arr.end());
  const double sigma = pir_params_.get_noise_std_dev();
  RlweCt ct;
  encrypt_zero_rns(rlwe_sk_, N, qs, sigma, rng_, ct, false);
  return ct;
}

RlweCt PirClient::fresh_zero_ct_seeded(QuerySeed &seed) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs_arr = pir_params_.get_rns_mods();
  const std::vector<uint64_t> qs(qs_arr.begin(), qs_arr.end());
  const double sigma = pir_params_.get_noise_std_dev();
  for (uint32_t &word : seed) word = static_cast<uint32_t>(rng_());
  ChaChaRng mask_rng(seed);
  RlweCt ct;
  encrypt_zero_rns(rlwe_sk_, N, qs, sigma, mask_rng, rng_, ct, false);
  return ct;
}
