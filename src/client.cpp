#include "client.h"
#include "gsw.h"
#include "utils.h"

#include <memory>
#include <random>
#include <stdexcept>

static RlweSk make_client_sk(const PirParams &pir_params, ChaChaRng &rng) {
  const auto &qs_arr = pir_params.get_rns_mods();
  const std::vector<uint64_t> qs(qs_arr.begin(), qs_arr.end());
  return gen_secret_key_rns(DBConsts::PolyDegree, qs, rng);
}

PirClient::PirClient(const PirParams &pir_params)
    : client_id_(rand()), pir_params_(pir_params),
      rng_(),
      rlwe_sk_(make_client_sk(pir_params, rng_)) {}

GSWCt PirClient::generate_gsw_from_key() {
  constexpr size_t N = DBConsts::PolyDegree;

  const uint64_t q0 = pir_params_.get_rns_mods()[0];
  std::vector<uint64_t> sk_coef(rlwe_sk_.data.begin(),
                                rlwe_sk_.data.begin() + N);
  utils::ntt_inv(sk_coef.data(), N, q0);

  GSWEval key_gsw(pir_params_, pir_params_.get_l_key(), pir_params_.get_base_log2_key());
  return key_gsw.plain_to_gsw(sk_coef, rlwe_sk_, rng_);
}

SharedPirSessionKeys PirClient::create_session_keys() {
  auto keys = std::make_shared<PirSessionKeys>();
  keys->bv_galois_keys = create_bv_galois_keys();
  keys->gsw_key = generate_gsw_from_key();
  return keys;
}

SharedPirSessionKeys PirClient::create_session_keys(size_t bv_key_height) {
  auto keys = std::make_shared<PirSessionKeys>();
  keys->bv_galois_keys = bvks::gen_bv_galois_keys(
      pir_params_.with_query_shape({1, 0, bv_key_height}), rlwe_sk_);
  keys->gsw_key = generate_gsw_from_key();
  return keys;
}

bool PirClient::scheme_compatible(const PirParams &other) const {
  return pir_params_.scheme_compatible(other);
}
