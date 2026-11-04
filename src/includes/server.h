#pragma once

#include "gsw.h"
#include "pir.h"
#include "bv_keyswitch.h"
#include "pir_session.h"
#include "aligned_allocator.h"
#include "pir_profile.h"
#include <functional>
#include <map>
#include <sstream>
#include <unordered_map>

using PlaintextSource = std::function<void(size_t logical_index, RlwePt &out)>;

class PirServer {
public:
  PirServer(const PirParams &pir_params);
  ~PirServer();

  void gen_data(const std::vector<size_t>& record_indices = {});

  void load_data(size_t logical_num_pt, const PlaintextSource &source,
                 const std::vector<size_t> &record_indices = {});

  RlweCt make_query(const size_t client_id, RlweCt &query);
  RlweCt make_query_profiled(const size_t client_id, RlweCt &query,
                             PirPipelineProfile *profile);

  std::vector<RlweCt> expand_query(size_t client_id, RlweCt &query) const;
  std::vector<RlweCt> expand_query(size_t client_id, RlweCt &query,
                                   size_t expan_height, size_t useful_cnt) const;

  std::vector<GSWCt> complete_selectors(size_t client_id,
                                        const std::vector<RlweCt> &expanded);

  inline const PirParams &get_params() const { return pir_params_; }

  bool switch_response_to_small_q(RlweCt &response);

  size_t save_resp_to_stream(const RlweCt &response, std::stringstream &resp_stream);
  void set_client_bv_galois_key(const size_t client_id, bvks::BvGaloisKeys bv_keys);
  void set_client_gsw_key(const size_t client_id, GSWCt gsw_key);
  void set_client_session_keys(size_t client_id, SharedPirSessionKeys keys);
  SharedPirSessionKeys client_session_keys(size_t client_id) const;

  RlwePt direct_get_original_plaintext(const size_t index) const;


  std::vector<RlweCt> evaluate_first_dim(std::vector<RlweCt> &selection_vector,
                                         PirPipelineProfile *profile = nullptr);

  RlweCt evaluate_other_dim(std::vector<RlweCt> &intermediate_db, std::vector<GSWCt> &selectors);

  void ext_prod_mux(RlweCt &x, RlweCt &y, GSWCt &selection_cipher, RlweCt &result);


  friend class PirTest;

private:
  size_t num_pt_;
  std::map<size_t, bvks::BvGaloisKeys> client_bv_galois_keys_;
  std::map<size_t, GSWCt> client_gsw_keys_;
  std::map<size_t, SharedPirSessionKeys> client_sessions_;
  std::unordered_map<size_t, RlwePt> recorded_pts_;
  std::unique_ptr<db_coeff_t[], AlignedDeleter<db_coeff_t>> db_aligned_;
  std::vector<inter_coeff_t> inter_res_;

  std::unique_ptr<uint32_t[], AlignedDeleter<uint32_t>> db_lo_;
  std::unique_ptr<uint32_t[], AlignedDeleter<uint32_t>> db_hi_;
  std::vector<uint64_t> inter_res_lo_;
  std::vector<uint64_t> inter_res_hi_;
  PirParams pir_params_;
  GSWEval key_gsw_;
  GSWEval data_gsw_;

  std::vector<RlweCt> fast_expand_qry(size_t client_id, RlweCt &ciphertext) const;
  std::vector<RlweCt> fast_expand_qry(size_t client_id, RlweCt &ciphertext,
                                      size_t expan_height,
                                      size_t useful_cnt) const;

  std::vector<RlweCt> full_expand_qry(size_t client_id, RlweCt &ciphertext) const;

  void inter_to_cts(std::vector<RlweCt> &result, const inter_coeff_t *__restrict inter_res);

  void fill_inter_res();

  void prep_query(std::vector<RlweCt> &fst_dim_query, std::vector<db_coeff_t>& query_data);

  void prep_query_composite(const std::vector<RlweCt> &fst_dim_query,
                            uint32_t *query_lo, uint32_t *query_hi);

  void inter_to_cts_composite(std::vector<RlweCt> &result,
                              const uint64_t *inter_lo,
                              const uint64_t *inter_hi);

  void mod_switch_inplace(RlweCt &ciphertext, const uint64_t small_q);

};
