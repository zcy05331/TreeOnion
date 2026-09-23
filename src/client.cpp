#include "client.h"
#include "gsw.h"
#include "utils.h"

#include <memory>
#include <random>
#include <stdexcept>

// Build the K-limb sk lazily because its construction depends on PirParams.
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

  // Recover ternary sk in coefficient form under q_0, then re-canonicalise
  // {-1 ↔ q_0-1} → {-1 ↔ q_k-1} for each limb. We pass the q_0 form to
  // plain_to_gsw, which re-encodes -1 per limb as the matching q_k-1.
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

// [Tree PIR 会话密钥（显式高度版）] 与无参版本相同的密钥束，但 BV Galois 覆盖高度由
// 调用方显式指定。动机（手册 §5.1）：树协议的投影 project_keep_stride 要对每个
// j < min(r, level) 应用 Subs(η_j = n/2^j + 1)，与查询展开高度无关地需要覆盖到
// log₂ n 的密钥；而默认参数只派生到编译期 TREE_HEIGHT，g=1 时 r=11 > 10 就不够用。
// 输入 bv_key_height 是本会话的能力上界：生成的密钥支持自同构 Subs((N >> i) + 1)，
// i < bv_key_height——同一密钥族同时服务查询展开与投影，无需第二套密钥。
// 输出 SharedPirSessionKeys = {BV Galois 密钥, RGSW(s) 补全密钥}，整束交给服务端注册。
SharedPirSessionKeys PirClient::create_session_keys(size_t bv_key_height) {
  // 用 with_query_shape({1, 0, bv_key_height}) 派生一个"仅展开"参数视图：scheme
  // 字段（模数、gadget、密钥参数）原样保留，三个形状字段全部覆写
  //（fst_dim_sz=1、selector 位数=0、expansion_height=指定高度）；
  // gen_bv_galois_keys 只消费其中的 expansion_height，据此逐个生成
  // (N >> i) + 1（i < bv_key_height）的密钥。
  // 该视图不承载数据库，这里只把它当高度的载体。
  auto keys = std::make_shared<PirSessionKeys>();
  keys->bv_galois_keys = bvks::gen_bv_galois_keys(
      pir_params_.with_query_shape({1, 0, bv_key_height}), rlwe_sk_);
  // RGSW(s) 补全密钥与展开高度无关，直接复用生产路径的生成逻辑
  // （Algorithm 3 补全 bottom half 所需的材料）。
  keys->gsw_key = generate_gsw_from_key();
  return keys;
}

bool PirClient::scheme_compatible(const PirParams &other) const {
  return pir_params_.scheme_compatible(other);
}
