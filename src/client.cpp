#include "client.h"
#include "gsw.h"
#include "utils.h"

#include <memory>
#include <random>
#include <stdexcept>

// Build the K-limb sk lazily because its construction depends on PirParams.
static RlweSk make_client_sk(const PirParams &pir_params, std::mt19937_64 &rng) {
  const auto &qs_arr = pir_params.get_rns_mods();
  const std::vector<uint64_t> qs(qs_arr.begin(), qs_arr.end());
  return gen_secret_key_rns(DBConsts::PolyDegree, qs, rng);
}

PirClient::PirClient(const PirParams &pir_params)
    : client_id_(rand()), pir_params_(pir_params),
      rng_(std::random_device{}()),
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


// [Tree PIR M7 环切换密钥] 生成 d=2 环切换（n → n₂ = n/2）所需的全部材料（手册 §4.3）。
// 数学背景：把大环多项式按奇偶分解 f(X) = f_e(X²) + X·f_o(X²)，取 Y = X² 后小环仍是
// 负循环（Y^{n₂} = X^n = −1）。大密文相位的偶部 = c0_e + a_e·s_e + Y·a_o·s_o，即一条
// 以主密钥的偶/奇小环分量 {s_e, s_o} 为密钥的 2-秩 MLWE 密文。服务端要把它切到独立
// 小环密钥 s₂ 下，就需要每个分量 s_c（c ∈ {e, o}）的 gadget 密钥切换行
//   KSK_c[t] = RLWE_{n₂,q}(B^t · s_c) under s₂，t < l₂。
// 输出 bundle 分两半：keys（两组 KSK 行，交给服务端做切换）与 secret（s₂ 及 n₂，
// 客户端留作解码）。所有行都在全 q 下构造——切换必须先于降模（§4.3 顺序纪律）。
TreeRingSwitchBundle PirClient::create_ring_switch_bundle(size_t n2) {
  constexpr size_t N = DBConsts::PolyDegree;
  // 守卫：当前实现只支持一步 d=2 切换（n₂ = n/2）；其他比例的相位分解未实现。
  if (n2 * 2 != N) {
    throw std::invalid_argument(
        "ring switch bundle currently supports n2 = n / 2");
  }
  // 守卫：只支持单 limb（K=1）方案；多 limb 下偶部相位与 KSK 语义都需另行推导。
  if (pir_params_.K() != 1) {
    throw std::invalid_argument(
        "ring switch bundle supports single-limb schemes");
  }
  // 单 limb 模数 q 与加密噪声标准差；gadget 取 l₂ = 2 级、基宽
  // base_log2 = ⌈log₂ q / l₂⌉（q≈2^58 时 B = 2^29），向上取整保证 B^{l₂} 覆盖整个 q，
  // 最高 digit 不会溢出。
  const uint64_t q = pir_params_.get_rns_mods()[0];
  const double sigma = pir_params_.get_noise_std_dev();
  const size_t l2 = 2;
  const size_t base_log2 = (pir_params_.get_ct_mod_width() + l2 - 1) / l2;

  // 主密钥以 NTT 域存储；先把 limb0 INTT 回系数形式，才能按系数下标做偶奇拆分：
  // component[0][k] = s_{2k}（即 s_e），component[1][k] = s_{2k+1}（即 s_o），
  // 正对应 f(X) = f_e(X²) + X·f_o(X²) 中的两个小环分量。
  std::vector<uint64_t> sk_coef(rlwe_sk_.data.begin(),
                                rlwe_sk_.data.begin() + N);
  utils::ntt_inv(sk_coef.data(), N, q);
  std::vector<std::vector<uint64_t>> component(
      2, std::vector<uint64_t>(n2, 0));
  for (size_t k = 0; k < n2; ++k) {
    component[0][k] = sk_coef[2 * k];
    component[1][k] = sk_coef[2 * k + 1];
  }

  // secret 半：独立采样三值小环目标密钥 s₂（不是 s 的派生物，安全性独立），
  // 系数以 mod q 的规范代表元存放（-1 ↔ q-1）。
  TreeRingSwitchBundle bundle;
  bundle.secret.n2 = n2;
  bundle.secret.s2.assign(n2, 0);
  utils::sample_ternary(bundle.secret.s2.data(), n2, q, rng_);

  // keys 半的公共几何：小环度数、gadget 级数与基宽，服务端分解 a_e / Y·a_o 时
  // 必须使用与此一致的 (l₂, B)。rows[c] 是分量 s_c 的 l₂ 条 KSK 行。
  bundle.keys.n2 = n2;
  bundle.keys.l2 = l2;
  bundle.keys.base_log2 = base_log2;
  bundle.keys.rows.assign(2, {});
  std::vector<uint64_t> noise(n2), c1(n2);
  for (size_t c = 0; c < 2; ++c) {
    bundle.keys.rows[c].reserve(l2);
    for (size_t t = 0; t < l2; ++t) {
      // 每条 gadget 行都是 s₂ 下的"裸消息"RLWE 加密：
      //   c0 = −c1·s₂ + e + B^t·s_c，  c1 均匀随机，e 高斯。
      // 消息 B^t·s_c 不乘 Δ——密钥切换消费的是 gadget 尺度，不是 BFV 明文尺度。
      utils::sample_uniform_poly(c1.data(), n2, q, rng_);
      utils::sample_gaussian(noise.data(), n2, q, sigma, rng_);
      // 先算 c1·s₂：small_ring_mul 是 R_{n₂} 上的负循环 schoolbook 乘法
      // （u128 单次规约参考核，无需为合成模数注册 2n₂ 次根）。
      std::vector<uint64_t> c0 = small_ring_mul(c1, bundle.secret.s2, q);
      // B^t 本身放得进 u64（l₂=2 时移位量 ≤ 30）；取 uint128 是为了让下面
      // component·power 的乘积直接落在 128 位算术里，与取模配套。
      const uint128_t power = static_cast<uint128_t>(1)
                              << (t * base_log2);
      for (size_t i = 0; i < n2; ++i) {
        // message = B^t · s_c[i] mod q，乘法走 uint128 中间量避免回绕。
        const uint64_t message = static_cast<uint64_t>(
            (static_cast<uint128_t>(component[c][i]) * power) % q);
        // 组装 c0[i] = −(c1·s₂)[i] + e[i] + message（mod q，逐项完成上面的公式）。
        c0[i] = (q - c0[i] + noise[i]) % q;
        c0[i] = (c0[i] + message) % q;
      }
      // 行按 (c0, c1) 存入分量 c 的第 t 级；c1 被拷贝，缓冲区留给下一轮复用。
      bundle.keys.rows[c].emplace_back(std::move(c0), c1);
    }
  }
  return bundle;
}

bool PirClient::scheme_compatible(const PirParams &other) const {
  return pir_params_.scheme_compatible(other);
}
