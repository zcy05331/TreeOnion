#include "server.h"
#include "bv_keyswitch.h"
#include "gsw.h"
#include "utils.h"
#include "hexl/hexl.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iterator>

RlweCt PirServer::evaluate_other_dim(std::vector<RlweCt> &mid_db, std::vector<GSWCt> &selectors) {
  if (pir_params_.get_num_dims() == 1) {
    return mid_db[0];
  }

  size_t h = pir_params_.get_num_dims() - 1;
  const size_t other_dim_sz = pir_params_.get_other_dim_sz();
  const size_t perfect_size = (1 << (h - 1));

  if (other_dim_sz < perfect_size) {
    throw std::invalid_argument(
        "evaluate_other_dim: layout has fewer candidates than its selector "
        "tree; query-shape views cannot answer make_query");
  }

  const size_t last_level_sz = 2 * other_dim_sz - (1 << h);
  const size_t offset = other_dim_sz - last_level_sz;

  for (size_t i = 0; i < last_level_sz; i += 2) {
    size_t corrected_idx = i + offset;
    auto &x = mid_db[corrected_idx];
    auto &y = mid_db[corrected_idx + 1];
    ext_prod_mux(x, y, selectors[0], mid_db[i / 2 + offset]);
  }

  for (size_t a = 1; a < selectors.size(); a++) {
    const size_t level_sz = (1 << (h - a));
    const size_t half = level_sz >> 1;
    for (size_t i = 0; i < half; i++) {
      auto &x = mid_db[i];
      auto &y = mid_db[i + half];
      ext_prod_mux(x, y, selectors[a], mid_db[i]);
    }
  }
  return mid_db[0];
}


void PirServer::ext_prod_mux(RlweCt &x, RlweCt &y, GSWCt &selection_cipher, RlweCt &result) {
    constexpr size_t N = DBConsts::PolyDegree;
    const auto &qs = pir_params_.get_rns_mods();
    const size_t K = qs.size();

    auto sub_k = [&](RlweCt &a, const RlweCt &b) {
      for (size_t k = 0; k < K; ++k) {
        intel::hexl::EltwiseSubMod(a.c0.data() + k * N, a.c0.data() + k * N,
                                   b.c0.data() + k * N, N, qs[k]);
        intel::hexl::EltwiseSubMod(a.c1.data() + k * N, a.c1.data() + k * N,
                                   b.c1.data() + k * N, N, qs[k]);
      }
    };
    auto add_inplace_k = [&](RlweCt &a, const RlweCt &b) {
      for (size_t k = 0; k < K; ++k) {
        intel::hexl::EltwiseAddMod(a.c0.data() + k * N, a.c0.data() + k * N,
                                   b.c0.data() + k * N, N, qs[k]);
        intel::hexl::EltwiseAddMod(a.c1.data() + k * N, a.c1.data() + k * N,
                                   b.c1.data() + k * N, N, qs[k]);
      }
    };
    auto add_k = [&](const RlweCt &a, const RlweCt &b, RlweCt &c) {
      c.c0.resize(K * N);
      c.c1.resize(K * N);
      c.ntt_form = a.ntt_form;
      for (size_t k = 0; k < K; ++k) {
        intel::hexl::EltwiseAddMod(c.c0.data() + k * N, a.c0.data() + k * N,
                                   b.c0.data() + k * N, N, qs[k]);
        intel::hexl::EltwiseAddMod(c.c1.data() + k * N, a.c1.data() + k * N,
                                   b.c1.data() + k * N, N, qs[k]);
      }
    };
    auto intt_k = [&](RlweCt &ct) {
      for (size_t k = 0; k < K; ++k) {
        utils::ntt_inv(ct.c0.data() + k * N, N, qs[k]);
        utils::ntt_inv(ct.c1.data() + k * N, N, qs[k]);
      }
      ct.ntt_form = false;
    };

    TIME_START(OTHER_DIM_ADD_SUB);
    sub_k(y, x);
    TIME_END(OTHER_DIM_ADD_SUB);

    TIME_START(OTHER_DIM_MUX_EXTERN);
    data_gsw_.external_product(selection_cipher, y, y, LogContext::OTHER_DIM_MUX);
    TIME_END(OTHER_DIM_MUX_EXTERN);

    TIME_START(OTHER_DIM_INTT);
    intt_k(y);
    TIME_END(OTHER_DIM_INTT);

    TIME_START(OTHER_DIM_ADD_SUB);
    if (&result == &x) {
      add_inplace_k(x, y);
    } else {
      add_k(x, y, result);
    }
    TIME_END(OTHER_DIM_ADD_SUB);
}

std::vector<RlweCt>
PirServer::fast_expand_qry(std::size_t client_id, RlweCt &ciphertext) const {
  return fast_expand_qry(client_id, ciphertext, pir_params_.get_expan_height(),
                         pir_params_.get_fst_dim_sz() +
                             pir_params_.get_l() *
                                 (pir_params_.get_num_dims() - 1));
}

std::vector<RlweCt>
PirServer::fast_expand_qry(std::size_t client_id, RlweCt &ciphertext,
                           size_t expan_height, size_t useful_cnt) const {
  const size_t capacity = size_t{1} << expan_height;
  if (useful_cnt > capacity) {
    throw std::invalid_argument("expansion asks for more leaves than 2^h");
  }
  const auto session_it = client_sessions_.find(client_id);
  const bvks::BvGaloisKeys &bv_galois_key =
      session_it != client_sessions_.end()
          ? session_it->second->bv_galois_keys
          : client_bv_galois_keys_.at(client_id);
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = pir_params_.get_rns_mods();
  const size_t K = qs.size();

  auto rlwe_add_k = [&](const RlweCt &a, const RlweCt &b, RlweCt &c) {
    c.c0.resize(K * N);
    c.c1.resize(K * N);
    c.ntt_form = a.ntt_form;
    for (size_t k = 0; k < K; ++k) {
      intel::hexl::EltwiseAddMod(c.c0.data() + k * N, a.c0.data() + k * N,
                                 b.c0.data() + k * N, N, qs[k]);
      intel::hexl::EltwiseAddMod(c.c1.data() + k * N, a.c1.data() + k * N,
                                 b.c1.data() + k * N, N, qs[k]);
    }
  };
  auto rlwe_sub_inplace_k = [&](RlweCt &a, const RlweCt &b) {
    for (size_t k = 0; k < K; ++k) {
      intel::hexl::EltwiseSubMod(a.c0.data() + k * N, a.c0.data() + k * N,
                                 b.c0.data() + k * N, N, qs[k]);
      intel::hexl::EltwiseSubMod(a.c1.data() + k * N, a.c1.data() + k * N,
                                 b.c1.data() + k * N, N, qs[k]);
    }
  };
  auto rlwe_shift_k = [&](const RlweCt &src, RlweCt &dst, size_t index) {
    dst.c0.resize(K * N);
    dst.c1.resize(K * N);
    dst.ntt_form = src.ntt_form;
    for (size_t k = 0; k < K; ++k) {
      utils::negacyclic_shift_poly_coeffmod(src.c0.data() + k * N, N, index,
                                            qs[k], dst.c0.data() + k * N);
      utils::negacyclic_shift_poly_coeffmod(src.c1.data() + k * N, N, index,
                                            qs[k], dst.c1.data() + k * N);
    }
  };

  std::vector<RlweCt> cts(2 * capacity);
  cts[1] = ciphertext;

  for (size_t i = 1; i < capacity; ++i) {
    const int k = int{1} << (std::bit_width(i) - 1);

    const size_t left_leaf = i * capacity / k - capacity;
    if (left_leaf >= useful_cnt)
      continue;

    RlweCt c_prime = cts[i];
    const uint32_t galois_k = DBConsts::PolyDegree / k + 1;
    TIME_START(APPLY_GALOIS);
    bvks::bv_apply_galois_inplace(c_prime, galois_k,
                                  bv_galois_key.get(galois_k),
                                  pir_params_);
    TIME_END(APPLY_GALOIS);
    TIME_START("add_sub");
    rlwe_add_k(cts[i], c_prime, cts[2 * i]);
    rlwe_sub_inplace_k(cts[i], c_prime);
    TIME_END("add_sub");

    TIME_START("shift polynomial");
    rlwe_shift_k(cts[i], cts[2 * i + 1], static_cast<size_t>(-k));
    TIME_END("shift polynomial");
  }

  return std::vector<RlweCt>(
      std::make_move_iterator(cts.begin() + capacity),
      std::make_move_iterator(cts.begin() + capacity + useful_cnt));
}

std::vector<RlweCt> PirServer::expand_query(size_t client_id,
                                            RlweCt &query) const {
  return fast_expand_qry(client_id, query);
}

std::vector<RlweCt> PirServer::expand_query(size_t client_id, RlweCt &query,
                                            size_t expan_height,
                                            size_t useful_cnt) const {
  return fast_expand_qry(client_id, query, expan_height, useful_cnt);
}

std::vector<GSWCt> PirServer::complete_selectors(
    size_t client_id, const std::vector<RlweCt> &expanded) {
  const size_t l_ep = pir_params_.get_l();
  const size_t fst_dim_sz = pir_params_.get_fst_dim_sz();
  const size_t num_selectors = pir_params_.get_num_dims() - 1;
  std::vector<GSWCt> gsw_vec(num_selectors);
  if (num_selectors == 0) return gsw_vec;
  if (expanded.size() < fst_dim_sz + num_selectors * l_ep) {
    throw std::invalid_argument(
        "complete_selectors expects one expanded row group per selector");
  }
  const auto session_it = client_sessions_.find(client_id);
  const GSWCt &completion_key =
      session_it != client_sessions_.end()
          ? session_it->second->gsw_key
          : client_gsw_keys_.at(client_id);
  for (size_t i = 1; i <= num_selectors; i++) {
    std::vector<RlweCt> lwe_vector;
    lwe_vector.reserve(l_ep);
    for (size_t k = 0; k < l_ep; ++k) {
      lwe_vector.push_back(expanded[fst_dim_sz + (i - 1) * l_ep + k]);
    }
    key_gsw_.query_to_gsw(lwe_vector, completion_key, gsw_vec[i - 1]);
  }
  return gsw_vec;
}

RlweCt PirServer::make_query(const size_t client_id, RlweCt &query) {
  return make_query_profiled(client_id, query, nullptr);
}

RlweCt PirServer::make_query_profiled(const size_t client_id, RlweCt &query,
                                      PirPipelineProfile *profile) {
  using ProfileClock = std::chrono::steady_clock;
  ProfileClock::time_point mark;
  if (profile) mark = ProfileClock::now();
  const auto lap = [&](std::chrono::nanoseconds PirPipelineProfile::*slot) {
    if (!profile) return;
    const auto now = ProfileClock::now();
    profile->*slot += now - mark;
    mark = now;
  };

  TIME_START(EXPAND_TIME);
  std::vector<RlweCt> query_vector = fast_expand_qry(client_id, query);
  TIME_END(EXPAND_TIME);
  lap(&PirPipelineProfile::expand);

  TIME_START(CONVERT_TIME);
  std::vector<GSWCt> gsw_vec = complete_selectors(client_id, query_vector);
  TIME_END(CONVERT_TIME);
  lap(&PirPipelineProfile::convert);

  TIME_START(FST_DIM_TIME);
  query_vector.resize(pir_params_.get_fst_dim_sz());
  std::vector<RlweCt> mid_db = evaluate_first_dim(query_vector, profile);
  TIME_END(FST_DIM_TIME);
  if (profile) mark = ProfileClock::now();

  TIME_START(OTHER_DIM_TIME);
  RlweCt result = evaluate_other_dim(mid_db, gsw_vec);
  TIME_END(OTHER_DIM_TIME);
  lap(&PirPipelineProfile::other_dim);

  TIME_START(MOD_SWITCH);
  if (DBConsts::SmallQWidth < DBConsts::RnsMods[0]) {
    DEBUG_PRINT("Modulus switching for a single modulus...");
    const uint64_t small_q = pir_params_.get_small_q();
    mod_switch_inplace(result, small_q);
  }

  TIME_END(MOD_SWITCH);
  lap(&PirPipelineProfile::mod_switch);
  DEBUG_PRINT("Modulus switching done.");
  return result;
}
