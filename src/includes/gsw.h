#pragma once
#include "pir.h"
#include "rlwe.h"
#include <random>
#include <vector>
#include "csprng.h"


typedef std::vector<std::vector<uint64_t>> GSWCt;

class GSWEval {
  private:
    PirParams pir_params_;
    size_t l_;
    size_t base_log2_;
    size_t low_bits_;

    std::vector<std::vector<uint64_t>> ep_decomp_;
    std::vector<uint64_t> ep_tmp_;
    std::vector<int64_t> ep_dwork_;

  public:
    GSWEval(const PirParams &pir_params, const size_t l, const size_t base_log2,
            const size_t low_bits = 0)
        : pir_params_(pir_params), l_(l), base_log2_(base_log2),
          low_bits_(low_bits) {}
    ~GSWEval() = default;
    GSWEval(const GSWEval &gsw_eval) = default;

    void external_product(GSWCt const &gsw_enc, RlweCt const &bfv,
                          RlweCt &res_ct,
                          LogContext context = LogContext::GENERIC);

    void decomp_rlwe_mp(RlweCt const &ct, std::vector<std::vector<uint64_t>> &output,
                        LogContext context = LogContext::GENERIC);

    void decomp_rlwe_single_mod(RlweCt const &ct, std::vector<std::vector<uint64_t>> &output,
                                   LogContext context = LogContext::GENERIC);

    void decomp_to_ntt(std::vector<std::vector<uint64_t>> &decomp_coeffs,
                      LogContext context = LogContext::GENERIC);

    void query_to_gsw(std::vector<RlweCt> query, GSWCt gsw_key,
                      GSWCt &output);

    GSWCt plain_to_gsw(std::vector<uint64_t> const &plaintext,
                               const RlweSk &sk, ChaChaRng &rng);

    void gsw_ntt_forward(GSWCt &gsw);
};
