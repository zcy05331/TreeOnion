#include "client.h"
#include "rlwe.h"
#include "utils.h"

#include <stdexcept>

std::vector<size_t> PirClient::get_query_indices(
    const PirParams &query_params, size_t pt_idx) const {
  // Algorithm 4 line 1 把扁平 plaintext index 映射到 QueryPack 坐标。
  // col_idx 是首维 BFV one-hot 坐标；row_idx 覆盖其余维度。论文规则是
  // binary hypercube，这里实现为 complete-but-not-perfect/ragged tree。
  // 返回向量为 {col_idx, selector bits...}；selector bits 按服务端归约
  // 顺序输出，第一个 selector 处理 deepest folded leaves。
  const size_t col_idx = pt_idx % query_params.get_fst_dim_sz();  // the first dimension
  const size_t row_idx = pt_idx / query_params.get_fst_dim_sz();  // the rest of the dimensions
  const size_t other_dim_sz = query_params.get_other_dim_sz();
  const size_t d = query_params.get_num_dims();
  const size_t h = d - 1; // the height of the further dimension complete binary tree.

  std::vector<size_t> query_indices = {col_idx};

  // Handle single dimension case
  if (d == 1) {
    // For single dimension, we only need the column index
    DEBUG_PRINT("Single dimension case - returning col_idx: " << col_idx);
    return query_indices;
  }

  const size_t r = 2 * other_dim_sz - (1 << h);   // the number of elements in the last level of the complete binary tree.
  const size_t sl = other_dim_sz - r;

  // r 是实际落在 deepest level 的 leaves 数；sl 是已经在上一层表示的 rows。
  // row_idx < sl 时没有 folded leaf pair，因此 deepest selector bit 为 0，
  // perfect_idx 保持 row_idx。否则 corrected_idx 选中 r 个 deepest leaves
  // 之一：corrected_idx % 2 是 folded-pair bit，corrected_idx / 2 先把
  // leaf pair 折叠回 parent position，再输出剩余 perfect-tree selector bits。
  size_t perfect_idx;
  if (row_idx < other_dim_sz - r) {
    query_indices.push_back(0);
    perfect_idx = row_idx;
  } else {
    size_t corrected_idx = row_idx - sl;
    query_indices.push_back(corrected_idx % 2);
    perfect_idx = sl + corrected_idx / 2;
  }

  // For the remaining perfect tree levels, emit bits MSB-first
  if (h > 1) {
    // There are (h - 1) bits for the perfect subtree
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

  // QueryPack 等价于先 packing plaintext vector 再执行 BFV encrypt。这里利用
  // BFV message-add 线性：先 encrypt zero 到 coefficient form，再把 scaled
  // plaintext 直接加到 c0 的 BitRev 位置（见 add_query_segment）。
  RlweCt query;
  encrypt_zero_rns(rlwe_sk_, N, qs, sigma, rng_, query, /*ntt_form=*/false);
  add_query_segment(query, query_params.get_expan_height(), 0, query_params,
                    query_indices, /*one_hot=*/true);
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
                    query_indices, /*one_hot=*/false);
}

void PirClient::add_query_segment(RlweCt &query, size_t pack_height,
                                  size_t offset, const PirParams &query_params,
                                  const std::vector<size_t> &query_indices,
                                  bool one_hot) {
  if (!pir_params_.scheme_compatible(query_params)) {
    throw std::invalid_argument(
        "PirClient query layout is not scheme-compatible with its session");
  }
  const size_t capacity = size_t{1} << pack_height;  // 2^h slots after expansion
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
    // 注入量是 Delta*(capacity^-1 mod t)；capacity^-1 抵消 ExpandBFV 每层 add/sub
    // 引入的 scaling。K=1 分支用 round(q0*inverse/t) 表示 direct scaling；
    // K=2 分支先按 composite Q=q0*q1 计算 full-q plaintext lift，再降到各
    // RNS limb。
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
  // no further dimensions
  if (query_indices.size() == 1) return;

  // 1/capacity per limb, cancels the scaling factor introduced by expansion.
  std::vector<uint64_t> inv(K);
  for (size_t k = 0; k < K; k++) {
    uint64_t result;
    utils::try_invert_uint_mod(capacity, rns_mods[k], result);
    inv[k] = result;
  }

  // MP gadget table: gadget[k][p] = B^(l-1-p) mod q_k. MSB-first
  // (p=0 = largest power), matching plain_to_gsw.
  std::vector<std::vector<uint64_t>> gadget =
      utils::gsw_gadget(l, query_params.get_base_log2(), rns_mods,
                        query_params.get_ep_low_bits());

  // QueryPack packed slots layout, relative to `offset`:
  // [0, N0) 是首维 BFV one-hot。代码索引 i=1..query_indices.size()-1 时，
  // 第 i 个高维 selector 占用 [N0 + (i-1)*L_EP, N0 + i*L_EP)，即对应
  // RGSW sample 的 top half rows。selector=1 注入 gadget powers；
  // selector=0 保持这些 slots 为 encryption of zero。每个位置先 BitRev，
  // 再乘 capacity^-1，使 ExpandBFV 恢复预期 row constants。
  auto q_head = query.data(0);
  for (size_t i = 1; i < query_indices.size(); i++) {
    if (query_indices[i] != 1) continue;

    // l slots per dim, gadget value written under every limb.
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

// [共用底座 / Tree 打包查询入口] 加密零的新鲜密文：系数形式、全 Q，每次调用消耗新的
// (a, e) 随机性。两类使用者：(1) 噪声基线类测试；(2) tree PIR 的打包查询入口——
// make_tree_query 以它为底座，把 Δ·(W⁻¹ mod t)、(W⁻¹ mod q_k)·G[k] 等常数直接加进
// c0 的目标系数（Enc(0) + 明文常数 = 该常数的合法加密），从而免于重新实现编码器。
RlweCt PirClient::fresh_zero_ct() {
  // 取全 RNS limb 集与噪声参数，调用 encrypt_zero_rns 产出系数形式（非 NTT）密文，
  // 保持系数形式是为了让调用方能按系数下标做常数注入（BitRev 槽位写入）。
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs_arr = pir_params_.get_rns_mods();
  const std::vector<uint64_t> qs(qs_arr.begin(), qs_arr.end());
  const double sigma = pir_params_.get_noise_std_dev();
  RlweCt ct;
  encrypt_zero_rns(rlwe_sk_, N, qs, sigma, rng_, ct, /*ntt_form=*/false);
  return ct;
}
