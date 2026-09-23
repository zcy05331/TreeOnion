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
  // 对应 Algorithm 4 lines 7-14。mid_db 起始保存 first dimension 之后的
  // Nrest encrypted candidates，其 layout 是 leaf order。selectors[0] 在 deepest
  // remaining tree level 被消费；后续 selector 逐层向 root 归约，每层通过 MUX
  // 前/后半 candidates 把 active candidate count 减半。
  if (pir_params_.get_num_dims() == 1) {
    // 没有 high-dimensional selectors；Algorithm 4 在 lines 4-6 后结束。
    return mid_db[0];
  }

  size_t h = pir_params_.get_num_dims() - 1;
  const size_t other_dim_sz = pir_params_.get_other_dim_sz();
  // `other_dim_sz` 不一定是 perfect power of two。virtual selector tree 高度是 h；
  // `perfect_size` 是 ragged leaves 上方第一个 complete level 的大小。
  const size_t perfect_size = (1 << (h - 1)); // second to last level size

  // Complete-tree layouts always satisfy other_dim_sz >= 2^(h-1); anything
  // smaller (e.g. an expansion-only with_query_shape view) would underflow
  // last_level_sz below and corrupt mid_db, so fail loudly instead.
  if (other_dim_sz < perfect_size) {
    throw std::invalid_argument(
        "evaluate_other_dim: layout has fewer candidates than its selector "
        "tree; query-shape views cannot answer make_query");
  }

  // 在 deepest/ragged level，`last_level_sz` 是 virtual tree 中真实拥有 sibling 的
  // leaves 数量；`offset` 之前的 leaves 在这一层没有 sibling，会原位保留。
  // 这里的 pairing 从 corrected_idx 开始，并把 parent 写回同一 logical level range。
  const size_t last_level_sz = 2 * other_dim_sz - (1 << h);
  const size_t offset = other_dim_sz - last_level_sz;

  for (size_t i = 0; i < last_level_sz; i += 2) { // i is the index within the last level.
    size_t corrected_idx = i + offset;  // index in the database.
    auto &x = mid_db[corrected_idx];
    auto &y = mid_db[corrected_idx + 1];
    ext_prod_mux(x, y, selectors[0], mid_db[i / 2 + offset]);
  }

  for (size_t a = 1; a < selectors.size(); a++) { // starting from the second to the last level
    const size_t level_sz = (1 << (h - a));
    const size_t half = level_sz >> 1;
    for (size_t i = 0; i < half; i++) {
      // 这里的 perfect levels 是 dense 的：candidate i 与 i+half 配对，对应
      // Algorithm 4 中 selector bit b 的规则：b=0 取前半，b=1 取后半。
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

    // 这里使用 MUX identity：select(b, x_orig, y_orig) = x_orig + b*(y_orig-x_orig)。
    // 本函数会有意改写参数 `y`：这一步之后它不再是 y_orig，而是每个 full-q
    // RNS limb 下 coefficient-form 的差值 y_orig-x_orig。
    TIME_START(OTHER_DIM_ADD_SUB);
    sub_k(y, x);
    TIME_END(OTHER_DIM_ADD_SUB);

    // 复用 `y` 作为 external-product scratch/output。GSW bit b 是 encrypted：
    // b=0 得到 zero 的 encryption，最终加回后保留 x_orig；b=1 得到
    // y_orig-x_orig，最终加回后选择 y_orig。external_product 的 RLWE output
    // 是 NTT form。
    TIME_START(OTHER_DIM_MUX_EXTERN);
    data_gsw_.external_product(selection_cipher, y, y, LogContext::OTHER_DIM_MUX);
    TIME_END(OTHER_DIM_MUX_EXTERN);

    // external-product output 要先转回 coefficient form，才能加到仍保持
    // coefficient-form 的 x 上。
    TIME_START(OTHER_DIM_INTT);
    intt_k(y);
    TIME_END(OTHER_DIM_INTT);

    // result 可能 alias x（常见的 reduction-in-place 路径）。y 是 scratch 且已经
    // 被破坏；x 在这次 final add 前仍是 x_orig。
    TIME_START(OTHER_DIM_ADD_SUB);
    if (&result == &x) {
      add_inplace_k(x, y);
    } else {
      add_k(x, y, result);
    }
    TIME_END(OTHER_DIM_ADD_SUB);
}

// Algorithm 2 ExpandBFV，按单次 level-order heap walk 实现（root index = 1）。
// 输入是一条 coefficient-form、K-limb、full-q packed BFV query。输出前
// u = N0 + L_EP * (d - 1) 个 constant BFV ciphertexts：先是 N0 个 first-dim
// one-hot slots，再是每个高维 selector 对应的 L_EP rows。client-side BitRev
// packing 与这里的 leaf order 配套，因此 leaf i 解密到 BitRev(i) 处写入的
// constant。位于 u 右侧的 internal nodes 在 Subs 前被 pruned，因为它们只会
// 生成未使用的 zero leaves。
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
  // ============== parameters: h and u = the leaves the caller consumes
  const size_t capacity = size_t{1} << expan_height;          // 2^h
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

  // K-aware per-limb helpers. All ciphertexts in this routine are coefficient
  // form, K-limb, with c0/c1 each holding K*N uint64_t.
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

  // ============== storage   – index 0 is *unused*, root is slot 1
  std::vector<RlweCt> cts(2 * capacity); // slots 0 … 2*capacity-1
  cts[1] = ciphertext;

  // ============== level-order walk, skip right-of-u sub-trees
  for (size_t i = 1; i < capacity; ++i) { // internal nodes only
    const int k = int{1} << (std::bit_width(i) - 1); // k = 2^{⌊log i⌋}   (span of this sub-tree)

    // 当前 heap subtree 覆盖的第一个 returned leaf；>= u 时整棵 subtree 可跳过。
    const size_t left_leaf = i * capacity / k - capacity;
    if (left_leaf >= useful_cnt)
      continue;

    RlweCt c_prime = cts[i];
    // σ_{N/k+1}: 当前 split level 对应的 Algorithm 2 automorphism。
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

  // ============== return useful leaf slice: heap slots capacity … capacity+u−1
  return std::vector<RlweCt>(
      std::make_move_iterator(cts.begin() + capacity),
      std::make_move_iterator(cts.begin() + capacity + useful_cnt));
}

// [Tree PIR 复用入口] Algorithm 2 ExpandBFV 的公共包装：把一条系数形式全 q 打包查询
// 按本服务器的参数布局解包成 useful-leaf 常数序列（fst_dim_sz 个首维 BFV 密文，随后
// 每个 selector 一组 L_EP 行；容量 2^expansion_height，w 右侧无用子树在展开中剪枝）。
// 生产路径 make_query 内部直接用 fast_expand_qry；这个公共入口让树路径
// （unpack_tree_query）复用同一份展开实现，再自行调 complete_selectors 做 RGSW 补全。
// 输入密文只读、不被修改。
std::vector<RlweCt> PirServer::expand_query(size_t client_id,
                                            RlweCt &query) const {
  // 纯转发：不复制、不改布局，公共 API 只是给外部组合器开一个访问口。
  return fast_expand_qry(client_id, query);
}

std::vector<RlweCt> PirServer::expand_query(size_t client_id, RlweCt &query,
                                            size_t expan_height,
                                            size_t useful_cnt) const {
  return fast_expand_qry(client_id, query, expan_height, useful_cnt);
}

std::vector<GSWCt> PirServer::complete_selectors(
    size_t client_id, const std::vector<RlweCt> &expanded) {
  // 从参数视图读出行组几何：每组 L_EP 行、首维宽度、selector 个数 = num_dims − 1
  // （维度约定：首维 + 每个高维 selector 各计一维）。
  const size_t l_ep = pir_params_.get_l();
  const size_t fst_dim_sz = pir_params_.get_fst_dim_sz();
  const size_t num_selectors = pir_params_.get_num_dims() - 1;
  std::vector<GSWCt> gsw_vec(num_selectors);
  // 单维布局没有高维 selector，直接返回空集（Algorithm 4 只用首维就结束）。
  if (num_selectors == 0) return gsw_vec;
  // 守卫：展开输出必须至少覆盖首维加上每个 selector 一个完整 L_EP 行组，
  // 防止形状不匹配的展开结果被静默切片出错误的行。
  if (expanded.size() < fst_dim_sz + num_selectors * l_ep) {
    throw std::invalid_argument(
        "complete_selectors expects one expanded row group per selector");
  }
  // 补全密钥查找：优先共享会话束（多布局 API），否则回退旧的按 client 注册表。
  const auto session_it = client_sessions_.find(client_id);
  const GSWCt &completion_key =
      session_it != client_sessions_.end()
          ? session_it->second->gsw_key
          : client_gsw_keys_.at(client_id);
  for (size_t i = 1; i <= num_selectors; i++) {
    // 按上述布局切出第 i 个 selector 的 L_EP 行 top half。
    std::vector<RlweCt> lwe_vector;
    lwe_vector.reserve(l_ep);
    for (size_t k = 0; k < l_ep; ++k) {
      lwe_vector.push_back(expanded[fst_dim_sz + (i - 1) * l_ep + k]);
    }
    // query_to_gsw 用 RGSW(s) 同态派生 bottom half，装配出完整 RGSW selector，
    // 写入 0 基下标 i−1。
    key_gsw_.query_to_gsw(lwe_vector, completion_key, gsw_vec[i - 1]);
  }
  return gsw_vec;
}

RlweCt PirServer::make_query(const size_t client_id, RlweCt &query) {
  return make_query_profiled(client_id, query, nullptr);
}

RlweCt PirServer::make_query_profiled(const size_t client_id, RlweCt &query,
                                      PirPipelineProfile *profile) {
  // 这是 Algorithm 4 executable skeleton。Function boundary：
  //   entry 前外部负责：client key setup 与 packed query transport
  //   本函数内部负责：Algorithm 2 expansion、Algorithm 3 completion、lines 4-15 eval
  //   return 后外部负责：save_resp_to_stream 做 response serialization
  // profile != nullptr 时只在既有阶段边界上读时钟，不改变任何运算顺序。
  using ProfileClock = std::chrono::steady_clock;
  ProfileClock::time_point mark;
  if (profile) mark = ProfileClock::now();
  const auto lap = [&](std::chrono::nanoseconds PirPipelineProfile::*slot) {
    if (!profile) return;
    const auto now = ProfileClock::now();
    profile->*slot += now - mark;
    mark = now;
  };

  // ========================== Expansion & conversion ==========================
  // 阶段 1 / Algorithm 2 ExpandBFV：把一条 full-q packed BFV query unpack 成
  // 得到 N0 个 first-dim BFV constants，加上每个 remaining selector 的 L_EP rows。
  TIME_START(EXPAND_TIME);
  std::vector<RlweCt> query_vector = fast_expand_qry(client_id, query);
  TIME_END(EXPAND_TIME);
  lap(&PirPipelineProfile::expand);

  // 阶段 2 / Algorithm 3 completion：把 expanded BFV selector rows 转成完整
  // RGSW ciphertexts；bottom half 由已注册到 server 的 client RGSW(s) key 补齐。
  // Row layout 与 completion 逻辑统一在 complete_selectors 中（tree unpack
  // 路径也走同一份实现）。
  TIME_START(CONVERT_TIME);
  std::vector<GSWCt> gsw_vec = complete_selectors(client_id, query_vector);
  TIME_END(CONVERT_TIME);
  lap(&PirPipelineProfile::convert);

  // ========================== Evaluations ==========================
  // 阶段 3 / Algorithm 4 lines 4-6：只取前 N0 个 expanded BFV ciphertexts
  // 做 database matrix-vector product。result vector 是 Nrest encrypted
  // 这些 candidates 每个对应一个 remaining-dimensional position。
  // 首维内部的四个子阶段（query NTT / pack / core / finalize）由
  // evaluate_first_dim 自己记入 profile。
  TIME_START(FST_DIM_TIME);
  query_vector.resize(pir_params_.get_fst_dim_sz());
  std::vector<RlweCt> mid_db = evaluate_first_dim(query_vector, profile);
  TIME_END(FST_DIM_TIME);
  if (profile) mark = ProfileClock::now();

  // 阶段 4 / Algorithm 4 lines 7-14：每个 RGSW selector bit 驱动一层 MUX，
  // 在 remaining encrypted candidates 上持续归约，直到只剩一条 ciphertext。
  TIME_START(OTHER_DIM_TIME);
  RlweCt result = evaluate_other_dim(mid_db, gsw_vec);
  TIME_END(OTHER_DIM_TIME);
  lap(&PirPipelineProfile::other_dim);

  // ========================== Post-processing ==========================
  TIME_START(MOD_SWITCH);
  // 阶段 5 / Algorithm 4 line 15：所有 homomorphic operation 完成后，才把
  // full-q ciphertext 转到 single-limb small-q response modulus。提前 switching
  // 会损失 noise headroom，并破坏与 first-dim matmul、RGSW external products、
  // Galois-key material 的 parameter agreement。
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
