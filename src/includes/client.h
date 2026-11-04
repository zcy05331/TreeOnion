#pragma once

#include "pir.h"
#include "gsw.h"
#include "bv_keyswitch.h"
#include "pir_session.h"
#include "query_wire.h"
#include "rlwe.h"
#include <random>
#include "csprng.h"

class PirClient {
public:
  PirClient(const PirParams &pirparms);
  ~PirClient() = default;

  RlweCt fast_generate_query(const size_t pt_idx);
  RlweCt fast_generate_query(const PirParams &query_params, size_t pt_idx);

  void add_gsw_to_query(RlweCt &query,
                        const std::vector<size_t> &query_indices);
  void add_gsw_to_query(const PirParams &query_params, RlweCt &query,
                        const std::vector<size_t> &query_indices);

  void add_query_segment(RlweCt &query, size_t pack_height, size_t offset,
                         const PirParams &query_params,
                         const std::vector<size_t> &query_indices,
                         bool one_hot);

  std::vector<size_t> get_query_indices(const PirParams &query_params,
                                        size_t pt_idx) const;

  inline bvks::BvGaloisKeys create_bv_galois_keys() {
    return bvks::gen_bv_galois_keys(pir_params_, rlwe_sk_);
  }

  RlwePt decrypt_ct(const RlweCt &ct);
  GSWCt generate_gsw_from_key();
  SharedPirSessionKeys create_session_keys();
  SharedPirSessionKeys create_session_keys(size_t bv_key_height);

  inline size_t get_client_id() const { return client_id_; }

  bool scheme_compatible(const PirParams &other) const;

  int noise_budget(const RlweCt &ct);


  RlweCt fresh_zero_ct();

  RlweCt fresh_zero_ct_seeded(QuerySeed &seed);

  RlweCt load_resp_from_stream(std::stringstream &resp_stream);

  RlwePt decrypt_mod_q(const RlweCt &ciphertext) const;
  RlwePt decrypt_mod_q_measured(const RlweCt &ciphertext, uint64_t &max_noise,
                                int &budget_bits) const;
  std::vector<int64_t> decrypt_mod_q_errors(const RlweCt &ciphertext) const;


  friend class PirTest;

private:
  const size_t client_id_;
  PirParams pir_params_;
  ChaChaRng rng_;
  RlweSk rlwe_sk_;

  std::vector<uint64_t> get_sk_ntt_small_q(uint64_t old_q, uint64_t small_q) const;

};
