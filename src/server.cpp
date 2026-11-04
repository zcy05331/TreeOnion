#include "server.h"
#include "utils.h"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>

PirServer::PirServer(const PirParams &pir_params)
    : pir_params_(pir_params),
      num_pt_(pir_params.get_num_pt()),
      key_gsw_(pir_params, pir_params.get_l_key(), pir_params.get_base_log2_key()),
      data_gsw_(pir_params, pir_params.get_l(), pir_params.get_base_log2(),
                pir_params.get_ep_low_bits()) {
  const size_t db_elem_cnt = num_pt_ * pir_params_.get_coeff_val_cnt();
  if (pir_params_.get_composite_rns().enabled) {
    db_lo_ = make_unique_aligned<uint32_t, 64>(db_elem_cnt);
    db_hi_ = make_unique_aligned<uint32_t, 64>(db_elem_cnt);
  } else {
    db_aligned_ = make_unique_aligned<db_coeff_t, 64>(db_elem_cnt);
  }
  fill_inter_res();
}

PirServer::~PirServer() {
}

void PirServer::gen_data(const std::vector<size_t>& record_indices) {
  BENCH_PRINT("Generating random data for the server database...");
  std::mt19937_64 rng(std::random_device{}());
  const uint64_t plain_mod = pir_params_.get_plain_mod();
  const size_t coeff_count = pir_params_.get_poly_degree();
  PlaintextSource random_source =
      [&rng, plain_mod, coeff_count](size_t, RlwePt &out) {
        out.data.resize(coeff_count);
        for (uint64_t &coefficient : out.data) {
          coefficient = rng() % plain_mod;
        }
      };
  load_data(num_pt_, random_source, record_indices);
}

void PirServer::load_data(size_t logical_num_pt, const PlaintextSource &source,
                          const std::vector<size_t>& record_indices) {
  if (!source) {
    throw std::invalid_argument("PirServer::load_data requires a source");
  }
  if (logical_num_pt > num_pt_) {
    throw std::invalid_argument(
        "PirServer::load_data logical plaintext count exceeds its layout");
  }
  for (size_t index : record_indices) {
    if (index >= num_pt_) {
      throw std::invalid_argument(
          "PirServer::load_data record index exceeds its layout");
    }
  }

  recorded_pts_.clear();
  recorded_pts_.reserve(record_indices.size());
  std::unordered_set<size_t> record_set(record_indices.begin(),
                                        record_indices.end());

  const size_t coeff_count = DBConsts::PolyDegree;
  const uint64_t plain_mod = pir_params_.get_plain_mod();
  const auto &rns_mods = pir_params_.get_rns_mods();
  const size_t K = rns_mods.size();
  const auto &crt = pir_params_.get_composite_rns();

  TIME_ONCE_START("DB gen+NTT+realign");

  constexpr size_t TILE = 8;
  std::vector<uint64_t> tile_pt(TILE * coeff_count);
  std::vector<uint64_t> stage(K * TILE * coeff_count);

  for (size_t pb = 0; pb < num_pt_; pb += TILE) {
    const size_t bs = std::min(TILE, num_pt_ - pb);

    for (size_t p = 0; p < bs; ++p) {
      uint64_t *dst = tile_pt.data() + p * coeff_count;
      const size_t poly_id = pb + p;
      if (poly_id < logical_num_pt) {
        RlwePt plaintext;
        source(poly_id, plaintext);
        if (plaintext.data.size() != coeff_count) {
          throw std::invalid_argument(
              "Plaintext source returned the wrong coefficient count");
        }
        for (size_t i = 0; i < coeff_count; ++i) {
          if (plaintext.data[i] >= plain_mod) {
            throw std::invalid_argument(
                "Plaintext source coefficient exceeds the plaintext modulus");
          }
          dst[i] = plaintext.data[i];
        }
      } else {
        std::fill(dst, dst + coeff_count, 0);
      }
      if (record_set.count(poly_id)) {
        RlwePt pt;
        pt.data.assign(dst, dst + coeff_count);
        recorded_pts_[poly_id] = std::move(pt);
      }
    }

    if (crt.enabled) {
      const uint64_t Q  = rns_mods[0];
      const uint64_t q1 = crt.q1;
      const uint64_t q2 = crt.q2;
      for (size_t p = 0; p < bs; ++p) {
        utils::ntt_fwd(tile_pt.data() + p * coeff_count, coeff_count, Q);
      }
      for (size_t coeff_idx = 0; coeff_idx < coeff_count; ++coeff_idx) {
        uint32_t *out_lo = db_lo_.get() + coeff_idx * num_pt_ + pb;
        uint32_t *out_hi = db_hi_.get() + coeff_idx * num_pt_ + pb;
        for (size_t p = 0; p < bs; ++p) {
          const uint64_t c = tile_pt[p * coeff_count + coeff_idx];
          out_lo[p] = static_cast<uint32_t>(c % q1);
          out_hi[p] = static_cast<uint32_t>(c % q2);
        }
      }
      continue;
    }

    for (size_t k = 0; k < K; ++k) {
      const uint64_t qk = rns_mods[k];
      uint64_t *limb_base = stage.data() + k * TILE * coeff_count;
      for (size_t p = 0; p < bs; ++p) {
        uint64_t *dst = limb_base + p * coeff_count;
        const uint64_t *src = tile_pt.data() + p * coeff_count;
        for (size_t i = 0; i < coeff_count; ++i) dst[i] = src[i] % qk;
        utils::ntt_fwd(dst, coeff_count, qk);
      }
    }
    for (size_t k = 0; k < K; ++k) {
      uint64_t *limb_base = stage.data() + k * TILE * coeff_count;
      for (size_t coeff_idx = 0; coeff_idx < coeff_count; ++coeff_idx) {
        db_coeff_t *out = db_aligned_.get() +
                          (k * coeff_count + coeff_idx) * num_pt_ + pb;
        for (size_t p = 0; p < bs; ++p) {
          out[p] = static_cast<db_coeff_t>(limb_base[p * coeff_count + coeff_idx]);
        }
      }
    }
  }
  TIME_ONCE_END("DB gen+NTT+realign");
  PRINT_ONCE("DB gen+NTT+realign");
}

void PirServer::set_client_bv_galois_key(const size_t client_id, bvks::BvGaloisKeys bv_keys) {
  client_bv_galois_keys_[client_id] = std::move(bv_keys);
}

void PirServer::set_client_gsw_key(const size_t client_id, GSWCt gsw_key) {
  client_gsw_keys_[client_id] = std::move(gsw_key);
}

void PirServer::set_client_session_keys(size_t client_id,
                                        SharedPirSessionKeys keys) {
  if (!keys) {
    throw std::invalid_argument("PirServer session key bundle cannot be null");
  }
  if (keys->bv_galois_keys.keys.size() < pir_params_.get_expan_height()) {
    throw std::invalid_argument(
        "PirServer session lacks keys for its expansion height");
  }
  client_sessions_[client_id] = std::move(keys);
}

SharedPirSessionKeys PirServer::client_session_keys(size_t client_id) const {
  return client_sessions_.at(client_id);
}


RlwePt PirServer::direct_get_original_plaintext(const size_t plaintext_idx) const {
  auto it = recorded_pts_.find(plaintext_idx);
  if (it == recorded_pts_.end()) {
    throw std::invalid_argument("Plaintext index " + std::to_string(plaintext_idx) +
                                " was not recorded during database loading");
  }
  return it->second;
}

void PirServer::fill_inter_res() {
  const size_t K = pir_params_.K();
  const size_t other_dim_sz = pir_params_.get_other_dim_sz();
  const size_t elem_cnt = other_dim_sz * DBConsts::PolyDegree * K * 2;
  if (pir_params_.get_composite_rns().enabled) {
    inter_res_lo_.resize(elem_cnt);
    inter_res_hi_.resize(elem_cnt);
  } else {
    inter_res_.resize(elem_cnt);
  }
}
