#include "tree_select.h"

#include "tree_select_internal.h"

#include "matrix.h"
#include "utils.h"

#include "hexl/hexl.hpp"

#include <algorithm>
#include <memory>
#include <utility>

using tree_select_internal::plan_row_and_cols;
using tree_select_internal::require;

namespace {

// 系数形式全 q 密文的逐 limb 加法：CtAdd 是 RLWE 同态加的最基本形式，
// 首维累加 Σ_k D[k]·A_k 的求和部分全靠它。
void ct_add_inplace(RlweCt &acc, const RlweCt &x, const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = scheme.get_rns_mods();
  // 守卫：两个输入必须都是系数形式、且长度都等于 K·N（防止 NTT/系数形式
  // 混加或不同 limb 数的密文相加得到无意义结果）。
  require(!acc.ntt_form && !x.ntt_form &&
              acc.c0.size() == x.c0.size() &&
              acc.c0.size() == qs.size() * N,
          "ciphertext addition needs matching coefficient-form inputs");
  // 对每个 RNS limb 分别做模 q_k 的逐系数加：c0、c1 两个分量同时累加，
  // 对应密文加法 (c0+c0', c1+c1')。
  for (size_t k = 0; k < qs.size(); ++k) {
    intel::hexl::EltwiseAddMod(acc.c0.data() + k * N, acc.c0.data() + k * N,
                               x.c0.data() + k * N, N, qs[k]);
    intel::hexl::EltwiseAddMod(acc.c1.data() + k * N, acc.c1.data() + k * N,
                               x.c1.data() + k * N, N, qs[k]);
  }
}

void ntt_ct_add_inplace(RlweCt &acc, const RlweCt &x,
                        const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = scheme.get_rns_mods();
  require(acc.ntt_form && x.ntt_form && acc.c0.size() == x.c0.size() &&
              acc.c1.size() == x.c1.size() &&
              acc.c0.size() == acc.c1.size() &&
              acc.c0.size() == qs.size() * N,
          "NTT ciphertext addition needs matching NTT-form inputs");
  for (size_t k = 0; k < qs.size(); ++k) {
    intel::hexl::EltwiseAddMod(acc.c0.data() + k * N, acc.c0.data() + k * N,
                               x.c0.data() + k * N, N, qs[k]);
    intel::hexl::EltwiseAddMod(acc.c1.data() + k * N, acc.c1.data() + k * N,
                               x.c1.data() + k * N, N, qs[k]);
  }
}


}  // namespace

// 参考实现的 BFV PtCtMul：明文多项式 pt ∈ Z_t[X]/(X^n+1) 乘进密文的两个
// 分量。由于 Dec(c) = c0 + c1·s，两个分量同乘 pt 即得 Enc(pt·m)。首维
// Σ D[k]·A_k 的标量路径每一项都靠它；优化核只是改变了这一乘法的求值顺序。
RlweCt tree_pt_ct_mul(const std::vector<uint64_t> &pt, const RlweCt &ct,
                      const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = scheme.get_rns_mods();
  const size_t K = qs.size();
  // 守卫：明文须是 n 次多项式；密文须为系数形式且两分量都覆盖全部 K 个 limb。
  require(pt.size() == N, "plaintext polynomial has the wrong degree");
  require(!ct.ntt_form && ct.c0.size() == K * N && ct.c1.size() == K * N,
          "PtCtMul needs a coefficient-form full-q ciphertext");

  // 每个 limb 做一次参考负循环乘：两操作数都前向 NTT、逐点相乘、再 INTT
  // 回系数形式。明文系数 < t < q_k，同一份系数值对每个 limb 都是合法代表元，
  // 无需按 limb 做中心化提升。
  RlweCt result;
  result.c0.assign(K * N, 0);
  result.c1.assign(K * N, 0);
  result.ntt_form = false;
  // pt_ntt/component 是逐 limb 复用的临时缓冲，避免在循环里反复分配。
  std::vector<uint64_t> pt_ntt(N), component(N);
  for (size_t k = 0; k < K; ++k) {
    const uint64_t q = qs[k];
    // 明文提升到 limb k 并前向 NTT（每 limb 一次，供 c0/c1 两次点乘共用）。
    std::copy(pt.begin(), pt.end(), pt_ntt.begin());
    utils::ntt_fwd(pt_ntt.data(), N, q);
    // 依次处理 c0（second=false）与 c1（second=true）：拷出该 limb 的分量、
    // NTT、与 pt_ntt 逐点乘、INTT 回系数形式、写回结果对应位置。
    for (const bool second : {false, true}) {
      const uint64_t *src = (second ? ct.c1.data() : ct.c0.data()) + k * N;
      uint64_t *dst = (second ? result.c1.data() : result.c0.data()) + k * N;
      std::copy(src, src + N, component.data());
      utils::ntt_fwd(component.data(), N, q);
      intel::hexl::EltwiseMultMod(component.data(), component.data(),
                                  pt_ntt.data(), N, q, 1);
      utils::ntt_inv(component.data(), N, q);
      std::copy(component.begin(), component.end(), dst);
    }
  }
  return result;
}

// 构建 α 部分和金字塔（§10）：由展开得到的 N0 维 one-hot 向量 A^(0)，
// 只用密文加法逐层求相邻对之和，得到全部 a+1 个分辨率的 one-hot 视图。
// 关键洞察：Σ_{i∈子树} [α == i] = [⌊α/2^c⌋ == j]——粗化层直接复用上层部分和，
// 免去第二次查询展开，也免去粗化层各自的密文-明文乘。噪声只按加法线性增长。
AlphaPyramid build_alpha_pyramid(std::span<const RlweCt> alpha_cts,
                                 const TreePirParams &tree,
                                 const PirParams &scheme) {
  // 守卫：输入必须恰是展开出的 N0 条 one-hot 密文（多/少都说明展开配置错）。
  require(alpha_cts.size() == tree.N0,
          "alpha pyramid needs the N0 expanded one-hot ciphertexts");
  AlphaPyramid pyramid;
  pyramid.reserve(tree.a + 1);
  // 第 0 层就是展开结果 A^(0) 本身（拷贝一份，调用方保留原件）。
  pyramid.emplace_back(alpha_cts.begin(), alpha_cts.end());
  // §10 递推：A^(c+1)_j = A^(c)_{2j} + A^(c)_{2j+1}。归纳可证第 c 层
  // one-hot 选中 ⌊α/2^c⌋；塔顶 A^(a)_0 是全体指示值之和，恒加密 1。
  for (size_t c = 0; c < tree.a; ++c) {
    const std::vector<RlweCt> &prev = pyramid.back();
    std::vector<RlweCt> next;
    next.reserve(prev.size() / 2);
    // 相邻配对求和：每层规模减半，总加法次数为 N0 − 1。
    for (size_t j = 0; j < prev.size() / 2; ++j) {
      RlweCt sum = prev[2 * j];
      ct_add_inplace(sum, prev[2 * j + 1], scheme);
      next.push_back(std::move(sum));
    }
    pyramid.push_back(std::move(next));
  }
  return pyramid;
}

// AlphaBeta 情形的首维求值（标量参考路径，§11.2）：把 R = N0·2^d 个明文
// 视作 §6.2 的 N0×2^d 矩阵 D_ℓ[k, δ] = D_ℓ[k·2^d + δ]，对每列 δ 计算
// Y[δ] = Σ_k D_ℓ[k, δ]·A^(0)_k。由于 A^(0) 是 α 的 one-hot，Y[δ] 恰加密
// D_ℓ[α·2^d + δ]——α 维被同态"消掉"，剩下 2^d 个 β 候选。
std::vector<RlweCt> evaluate_alpha_dimension(const TreeLevelDatabase &db,
                                             std::span<const RlweCt> alpha,
                                             const LevelPlan &plan,
                                             const TreePirParams &tree,
                                             const PirParams &scheme) {
  // 守卫组：本函数只服务 AlphaBeta 情形；db 与 plan 必须描述同一层；
  // α 向量必须是完整的 N0 条 one-hot 密文。
  require(plan.select_case == SelectCase::AlphaBeta,
          "alpha dimension evaluation applies to the AlphaBeta case");
  require(db.level == plan.level && db.R == plan.R,
          "level database does not match the plan");
  require(alpha.size() == tree.N0, "alpha vector must have N0 entries");
  // 矩阵列数 = β 候选数 2^{d_ℓ}；AlphaBeta 层的记录数必须恰好铺满矩阵。
  const size_t stride = size_t{1} << plan.beta_count;  // 2^{d_l}
  require(db.R == tree.N0 * stride, "AlphaBeta level has R = N0 * 2^d");

  // §11.2 主循环：候选 δ 累加 D_ℓ[k·2^d + δ] · A_k（k 扫过整个首维）。
  // 候选按 δ 的自然整数序压入——后续 β 折叠的 MSB-first 配对依赖此顺序。
  std::vector<RlweCt> candidates;
  candidates.reserve(stride);
  for (size_t delta = 0; delta < stride; ++delta) {
    // k = 0 项直接作为累加器初值，省一次全零密文的构造与加法。
    RlweCt acc = tree_pt_ct_mul(db.plaintexts[delta], alpha[0], scheme);
    for (size_t k = 1; k < tree.N0; ++k) {
      // 逐项 PtCtMul 后 CtAdd：这是最朴素的内积求值，优化核用 NTT 域
      // 累加/矩阵乘替换的正是这段循环。
      RlweCt term =
          tree_pt_ct_mul(db.plaintexts[k * stride + delta], alpha[k], scheme);
      ct_add_inplace(acc, term, scheme);
    }
    candidates.push_back(std::move(acc));
  }
  return candidates;
}

// β 折叠（§11.3 第 4.5 步）：用 RGSW 位选择器把 2^d 个候选二分收敛到 1 个。
// 每步对位 β_u 做 CMux：位为 0 保留下半，位为 1 保留上半，然后数组截半。
// 候选按 δ 的自然整数序排列，因此必须先消耗剩余 δ 的最高位（MSB-first）：
// 下半数组正是 "β_u = 0" 的那一半、上半是 "β_u = 1" 的那一半；LSB-first
// 会把不相邻的候选配成对，对非对称目标必错（明文模型 + 突变实验已验证）。
RlweCt fold_beta_dimension(std::vector<RlweCt> candidates,
                           const LevelPlan &plan,
                           std::span<GSWCt> beta_selectors, PirServer &mux) {
  // 守卫：候选数必须恰为 2^{beta_count}（否则截半流程会失衡）；
  // 活跃位窗口 [beta_begin, beta_begin+beta_count) 必须整个落在选择器数组内。
  require(candidates.size() == size_t{1} << plan.beta_count,
          "candidate count must be 2^{beta_count}");
  require(plan.beta_begin + plan.beta_count <= beta_selectors.size() ||
              plan.beta_count == 0,
          "fold needs a selector for every active beta bit");

  // 主循环：step 从 0 走到 beta_count−1，对应位号 u 从最高位
  // beta_begin+beta_count−1 递减到 beta_begin（即 MSB-first）。
  for (size_t step = 0; step < plan.beta_count; ++step) {
    const size_t u = plan.beta_begin + plan.beta_count - 1 - step;
    const size_t half = candidates.size() / 2;
    // 对每个下标 j 做 CMux(S^β_u, 下半 candidates[j], 上半 candidates[j+half])，
    // 结果写回 candidates[j]。ext_prod_mux 允许结果与 x 别名、且会破坏 y——
    // 这里都无妨：上半在本步结束后立即被 resize 截掉。
    for (size_t j = 0; j < half; ++j) {
      mux.ext_prod_mux(candidates[j], candidates[j + half], beta_selectors[u],
                       candidates[j]);
    }
    // 截半：本步位已消耗，剩余候选对应剩余 δ 位的所有取值。
    candidates.resize(half);
  }
  // 守卫：d 步截半后必然只剩一个候选——它加密的正是 D_ℓ[p_ℓ]。
  require(candidates.size() == 1, "fold must end with a single candidate");
  return std::move(candidates.front());
}


// 金字塔的查询期 NTT 提升（§6.3 第 2 步）：把整座 α 金字塔的每条密文各做
// 一次前向 NTT。这是每次查询的一次性成本，此后所有层的首维都在 NTT 域
// 逐点相乘，无需再对查询侧密文做任何前向变换。
AlphaPyramid pyramid_to_ntt(const AlphaPyramid &pyramid,
                            const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = scheme.get_rns_mods();
  AlphaPyramid ntt;
  ntt.reserve(pyramid.size());
  // 逐行逐密文变换：行结构（分辨率层次）原样保留，消费方按同样的
  // (c, j) 下标取用。
  for (const std::vector<RlweCt> &row : pyramid) {
    std::vector<RlweCt> out_row;
    out_row.reserve(row.size());
    for (const RlweCt &ct : row) {
      // 守卫：防止对已是 NTT 形式的密文重复变换（那会得到无意义数据）。
      require(!ct.ntt_form, "pyramid must be coefficient form before lifting");
      // 拷贝后对 c0/c1 的每个 limb 原位前向 NTT，并打上 ntt_form 标记，
      // 让下游核能校验输入形式。
      RlweCt lifted = ct;
      for (size_t k = 0; k < qs.size(); ++k) {
        utils::ntt_fwd(lifted.c0.data() + k * N, N, qs[k]);
        utils::ntt_fwd(lifted.c1.data() + k * N, N, qs[k]);
      }
      lifted.ntt_form = true;
      out_row.push_back(std::move(lifted));
    }
    ntt.push_back(std::move(out_row));
  }
  return ntt;
}

AlphaPyramid build_alpha_pyramid_ntt(std::span<const RlweCt> alpha_cts,
                                     const TreePirParams &tree,
                                     const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = scheme.get_rns_mods();
  require(alpha_cts.size() == tree.N0,
          "alpha pyramid needs the N0 expanded one-hot ciphertexts");
  AlphaPyramid pyramid;
  pyramid.reserve(tree.a + 1);
  std::vector<RlweCt> base;
  base.reserve(alpha_cts.size());
  for (const RlweCt &ct : alpha_cts) {
    require(!ct.ntt_form && ct.c0.size() == qs.size() * N &&
                ct.c1.size() == qs.size() * N,
            "alpha pyramid input must be coefficient-form");
    RlweCt lifted = ct;
    for (size_t k = 0; k < qs.size(); ++k) {
      utils::ntt_fwd(lifted.c0.data() + k * N, N, qs[k]);
      utils::ntt_fwd(lifted.c1.data() + k * N, N, qs[k]);
    }
    lifted.ntt_form = true;
    base.push_back(std::move(lifted));
  }
  pyramid.push_back(std::move(base));
  for (size_t c = 0; c < tree.a; ++c) {
    const std::vector<RlweCt> &prev = pyramid.back();
    std::vector<RlweCt> next;
    next.reserve(prev.size() / 2);
    for (size_t j = 0; j < prev.size() / 2; ++j) {
      RlweCt sum = prev[2 * j];
      ntt_ct_add_inplace(sum, prev[2 * j + 1], scheme);
      next.push_back(std::move(sum));
    }
    pyramid.push_back(std::move(next));
  }
  return pyramid;
}

namespace {

// 单个候选的 NTT 域累加器：acc += D_ntt · A_ntt 对 c0/c1 两个分量同时进行，
// 最后每个 limb 各做一次 INTT 收尾。正确性根基是 INTT 的线性：
// INTT(Σ 点乘) = Σ INTT(点乘)，且 mod-q 算术精确，所以整段求和只花一次
// 逆变换，结果与标量路径按位相同。
struct NttAccumulator {
  // c0/c1 是 NTT 域的部分和；tmp 是逐项点乘的临时缓冲（复用避免反复分配）。
  std::vector<uint64_t> c0, c1, tmp;
  // 清零到 K·N 大小，开始一个新候选的累加。
  void reset(size_t size) {
    c0.assign(size, 0);
    c1.assign(size, 0);
    tmp.assign(size, 0);
  }
  // 累加一项 D[k]·A_k：对每个 RNS limb 做 NTT 域逐点乘（明文 × 密文分量），
  // 再逐点加进部分和；c0、c1 各一次——对应 PtCtMul 作用于密文两个分量。
  void add_product(const std::vector<uint64_t> &pt_ntt, const RlweCt &a_ntt,
                   const PirParams &scheme) {
    constexpr size_t N = DBConsts::PolyDegree;
    const auto &qs = scheme.get_rns_mods();
    for (size_t k = 0; k < qs.size(); ++k) {
      const uint64_t q = qs[k];
      intel::hexl::EltwiseMultMod(tmp.data() + k * N,
                                  pt_ntt.data() + k * N,
                                  a_ntt.c0.data() + k * N, N, q, 1);
      intel::hexl::EltwiseAddMod(c0.data() + k * N, c0.data() + k * N,
                                 tmp.data() + k * N, N, q);
      intel::hexl::EltwiseMultMod(tmp.data() + k * N,
                                  pt_ntt.data() + k * N,
                                  a_ntt.c1.data() + k * N, N, q, 1);
      intel::hexl::EltwiseAddMod(c1.data() + k * N, c1.data() + k * N,
                                 tmp.data() + k * N, N, q);
    }
  }
  // 收尾：把部分和移动进结果密文，每个 limb 各做一次 INTT 回到系数形式。
  // 整个候选的求和只在这里付一次逆变换成本。
  RlweCt finish(const PirParams &scheme) {
    constexpr size_t N = DBConsts::PolyDegree;
    const auto &qs = scheme.get_rns_mods();
    RlweCt out;
    out.c0 = std::move(c0);
    out.c1 = std::move(c1);
    out.ntt_form = false;
    for (size_t k = 0; k < qs.size(); ++k) {
      utils::ntt_inv(out.c0.data() + k * N, N, qs[k]);
      utils::ntt_inv(out.c1.data() + k * N, N, qs[k]);
    }
    return out;
  }
};

}  // namespace

// 金字塔的 m32 拆分（查询期一次，所有层共享）：把 NTT 形式金字塔的每条密文
// 按 q1/q2 取余成两组 u32，并排成矩阵核 B 操作数的布局
// data[(coeff·cols + col)·2 + comp]——col 对应金字塔条目 k（矩阵乘的求和维），
// comp∈{0,1} 对应 c0/c1（作为 B 的两个输出通道一起被乘）。
AlphaPyramidM32 pyramid_to_m32(const AlphaPyramid &pyramid_ntt,
                               const PirParams &scheme,
                               Matrix32QueryLayout query_layout) {
  constexpr size_t N = DBConsts::PolyDegree;
  const CompositeRnsTables &crt = scheme.get_composite_rns();
  // 守卫：CRT 拆分表只在复合模数配置下可用。
  require(crt.enabled, "the m32 pyramid needs a composite configuration");
  AlphaPyramidM32 out;
  out.query_layout = query_layout;
  out.lo.reserve(pyramid_ntt.size());
  out.hi.reserve(pyramid_ntt.size());
  // 每个金字塔行独立生成一对 (lo, hi) 缓冲：行 c 的 cols = row.size()，
  // 与该行被消费时的选择宽度一致。
  for (const std::vector<RlweCt> &row : pyramid_ntt) {
    const size_t cols = row.size();
    std::vector<uint32_t> lo(N * cols * 2), hi(N * cols * 2);
    for (size_t k = 0; k < row.size(); ++k) {
      // 守卫：必须消费 pyramid_to_ntt 的输出（系数形式在此无意义）。
      require(row[k].ntt_form, "the m32 pyramid consumes the NTT-form pyramid");
    }
    if (query_layout == Matrix32QueryLayout::SplitColumns) {
      constexpr size_t CoeffTile = 8;
      constexpr size_t ColTile = 16;
      for (size_t coeff_base = 0; coeff_base < N; coeff_base += CoeffTile) {
        const size_t coeff_end = std::min(coeff_base + CoeffTile, N);
        for (size_t col_base = 0; col_base < cols; col_base += ColTile) {
          const size_t col_end = std::min(col_base + ColTile, cols);
          for (size_t k = col_base; k < col_end; ++k) {
            const RlweCt &ct = row[k];
            for (size_t coeff = coeff_base; coeff < coeff_end; ++coeff) {
              const size_t level_base = coeff * cols * 2;
              const size_t c0_at = level_base + k;
              const size_t c1_at = level_base + cols + k;
              lo[c0_at] = static_cast<uint32_t>(ct.c0[coeff] % crt.q1);
              hi[c0_at] = static_cast<uint32_t>(ct.c0[coeff] % crt.q2);
              lo[c1_at] = static_cast<uint32_t>(ct.c1[coeff] % crt.q1);
              hi[c1_at] = static_cast<uint32_t>(ct.c1[coeff] % crt.q2);
            }
          }
        }
      }
    } else {
      for (size_t k = 0; k < row.size(); ++k) {
        // 逐 NTT 系数、逐分量取 mod q1 / mod q2，写入 B 布局位置。
        for (size_t coeff = 0; coeff < N; ++coeff) {
          for (size_t comp = 0; comp < 2; ++comp) {
            const uint64_t value =
                comp == 0 ? row[k].c0[coeff] : row[k].c1[coeff];
            const size_t level_base = coeff * row.size() * 2;
            const size_t at = level_base + 2 * k + comp;
            lo[at] = static_cast<uint32_t>(value % crt.q1);
            hi[at] = static_cast<uint32_t>(value % crt.q2);
          }
        }
      }
    }
    out.lo.push_back(std::move(lo));
    out.hi.push_back(std::move(hi));
  }
  return out;
}

// Milestone-6 完整矩阵核版 SelectLevel：与标量/NTT 路径求同一个环表达式
// Y[row] = Σ_col D[row,col]·A_col，但把 NTT 域点乘累加改写成每个 CRT limb
// 一次 32×32→64 的矩阵乘（延迟规约），随后 CRT 合成回 mod q、每候选一次
// INTT。mod-q 算术精确 + CRT 合成返回规范代表元 ⇒ 输出与另两条路径按位一致。
RlweCt select_level_m32(const TreeLevelDatabaseM32 &db, const LevelPlan &plan,
                        const AlphaPyramidM32 &pyramid_m32,
                        std::span<GSWCt> beta_selectors, PirServer &mux,
                        const TreePirParams &tree, const PirParams &scheme) {
  ScanScratch scratch;
  return select_level_m32(db, plan, pyramid_m32, beta_selectors, mux, tree,
                          scheme, scratch);
}

RlweCt select_level_m32(const TreeLevelDatabaseM32 &db, const LevelPlan &plan,
                        const AlphaPyramidM32 &pyramid_m32,
                        std::span<GSWCt> beta_selectors, PirServer &mux,
                        const TreePirParams &tree, const PirParams &scheme,
                        ScanScratch &scratch) {
  constexpr size_t N = DBConsts::PolyDegree;
  const CompositeRnsTables &crt = scheme.get_composite_rns();
  // 守卫组：复合配置才有 m32 核；视图与 plan 必须同层；金字塔行号、行宽都
  // 要与视图的列宽一致（防止拿错分辨率或形状不匹配的缓冲相乘）。
  require(crt.enabled, "select_level_m32 needs a composite configuration");
  require(db.level == plan.level && db.R == plan.R,
          "level m32 view does not match the plan");
  const auto [pyramid_row, cols] = plan_row_and_cols(plan, tree);
  require(cols == db.cols && pyramid_row < pyramid_m32.lo.size(),
          "pyramid row does not match the m32 view");
  require(pyramid_m32.lo[pyramid_row].size() == N * cols * 2,
          "pyramid row width does not match the m32 view");

  // 每个 CRT limb 跑一次 level_mat_mat_32：对全部 N 个 NTT 系数一次性完成
  // rows×cols 与 cols×2 的 32×32→64 矩阵乘（内部延迟规约到 mod q1 / q2）。
  // 输出布局与金字塔 B 布局同构：out[(coeff·rows + row)·2 + comp]。
  TIME_START(TREE_SCAN_TIME);
  const size_t out_elems = N * db.rows * 2;
  // 输出缓冲只增不减地复用：level_mat_mat_32 会写满每一个元素，所以既不
  // 清零也不缩小。首次到达某个尺寸时扩容一次，之后的层与查询直接复用。
  if (scratch.lo.size() < out_elems) scratch.lo.resize(out_elems);
  if (scratch.hi.size() < out_elems) scratch.hi.resize(out_elems);
  uint64_t *const out_lo = scratch.lo.data();
  uint64_t *const out_hi = scratch.hi.data();
  TIME_START(TREE_SCAN_CORE_TIME);
  level_mat_mat_32(db.lo.data(), pyramid_m32.lo[pyramid_row].data(),
                   out_lo, db.rows, db.cols, N, crt.q1,
                   pyramid_m32.query_layout);
  level_mat_mat_32(db.hi.data(), pyramid_m32.hi[pyramid_row].data(),
                   out_hi, db.rows, db.cols, N, crt.q2,
                   pyramid_m32.query_layout);
  TIME_END(TREE_SCAN_CORE_TIME);

  // 复合配置下唯一的 RNS limb 就是 q = q1·q2，INTT 按它做。
  const uint64_t q = scheme.get_rns_mods()[0];
  std::vector<RlweCt> candidates;
  candidates.reserve(db.rows);
  // 逐候选行做 CRT 合成 + INTT，还原成系数形式的 RLWE 密文。
  TIME_START(TREE_SCAN_FINALIZE_TIME);
  for (size_t row = 0; row < db.rows; ++row) {
    RlweCt ct;
    ct.c0.assign(N, 0);
    ct.c1.assign(N, 0);
    ct.ntt_form = false;
    for (size_t coeff = 0; coeff < N; ++coeff) {
      for (size_t comp = 0; comp < 2; ++comp) {
        // 取出该 (系数, 候选, 分量) 在两个 limb 下的残差 x1 = x mod q1、
        // x2 = x mod q2。
        const size_t at = (coeff * db.rows + row) * 2 + comp;
        const uint64_t x1 = out_lo[at];
        const uint64_t x2 = out_hi[at];
        // Garner 式 CRT 合成：x = x1 + q1·((x2 − x1)·q1^{-1} mod q2)。
        // diff 先把 x1 约减到 mod q2 再做非负减法；乘 q1^{-1} (mod q2) 用
        // u128 防溢出。结果是 [0, q1·q2) 内的唯一规范代表元——正因为返回
        // 的是规范代表元，逐位一致性才能对标量路径成立。
        const uint64_t diff = (x2 + crt.q2 - x1 % crt.q2) % crt.q2;
        const uint64_t lift =
            static_cast<uint64_t>((static_cast<uint128_t>(diff) *
                                   crt.q1_inv_mod_q2) % crt.q2);
        const uint64_t value = x1 + crt.q1 * lift;
        (comp == 0 ? ct.c0 : ct.c1)[coeff] = value;
      }
    }
    // 每候选一次 INTT：与 NTT 路径同一收尾（INTT 线性保证求和可后置）。
    utils::ntt_inv(ct.c0.data(), N, q);
    utils::ntt_inv(ct.c1.data(), N, q);
    candidates.push_back(std::move(ct));
  }
  TIME_END(TREE_SCAN_FINALIZE_TIME);
  TIME_END(TREE_SCAN_TIME);

  // Single / CoarsenedAlpha 情形只有单行输出，直接就是 C_ℓ；
  // AlphaBeta 情形还需把 2^d 个候选做 MSB-first β 折叠。
  if (plan.select_case != SelectCase::AlphaBeta) {
    return std::move(candidates.front());
  }
  TIME_START(TREE_FOLD_TIME);
  RlweCt folded = fold_beta_dimension(std::move(candidates), plan,
                                      beta_selectors, mux);
  TIME_END(TREE_FOLD_TIME);
  return folded;
}

// NTT-u64 优化版 SelectLevel（Milestone-6，§6.3）：与 select_level 逐分支
// 对应、求同一个环表达式，只是每一项 PtCtMul 换成 NTT 域逐点乘累加，
// 每个候选只在收尾付一次 INTT。三种情形消费的金字塔行与标量路径一致。
RlweCt select_level_ntt(const TreeLevelDatabaseNtt &db, const LevelPlan &plan,
                        const AlphaPyramid &pyramid_ntt,
                        std::span<GSWCt> beta_selectors, PirServer &mux,
                        const TreePirParams &tree, const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const size_t K = scheme.K();
  // 守卫：视图与 plan 必须同层；金字塔必须有全部 a+1 行且底行宽 N0。
  require(db.level == plan.level && db.R == plan.R,
          "level NTT view does not match the plan");
  require(pyramid_ntt.size() == tree.a + 1 &&
              pyramid_ntt[0].size() == tree.N0,
          "alpha pyramid does not match the tree parameters");

  NttAccumulator acc;
  TIME_START(TREE_SCAN_TIME);
  switch (plan.select_case) {
    case SelectCase::Single: {
      // R = 1：唯一明文乘塔顶 A^(a)_0（恒加密 1），一次点乘即完成选择。
      acc.reset(K * N);
      acc.add_product(db.plaintexts[0], pyramid_ntt[tree.a][0], scheme);
      RlweCt out = acc.finish(scheme);
      TIME_END(TREE_SCAN_TIME);
      return out;
    }
    case SelectCase::CoarsenedAlpha: {
      // 1 < R < N0：粗化行 c 恰是 R 维 one-hot，Σ_k D[k]·A^(c)_k 选出
      // 下标 ⌊α/2^c⌋ 的明文。
      const size_t c = plan.coarsen_count;
      require(pyramid_ntt[c].size() == db.R,
              "coarsened pyramid level does not match R");
      acc.reset(K * N);
      for (size_t k = 0; k < db.R; ++k) {
        acc.add_product(db.plaintexts[k], pyramid_ntt[c][k], scheme);
      }
      RlweCt out = acc.finish(scheme);
      TIME_END(TREE_SCAN_TIME);
      return out;
    }
    case SelectCase::AlphaBeta: {
      // R = N0·2^d：对每个 δ 用底行 A^(0) 做首维内积（同
      // evaluate_alpha_dimension 的矩阵视图），再 β 折叠收敛到单候选。
      const size_t stride = size_t{1} << plan.beta_count;
      require(db.R == tree.N0 * stride, "AlphaBeta level has R = N0 * 2^d");
      std::vector<RlweCt> candidates;
      candidates.reserve(stride);
      for (size_t delta = 0; delta < stride; ++delta) {
        acc.reset(K * N);
        for (size_t k = 0; k < tree.N0; ++k) {
          acc.add_product(db.plaintexts[k * stride + delta],
                          pyramid_ntt[0][k], scheme);
        }
        candidates.push_back(acc.finish(scheme));
      }
      TIME_END(TREE_SCAN_TIME);
      TIME_START(TREE_FOLD_TIME);
      RlweCt folded = fold_beta_dimension(std::move(candidates), plan,
                                          beta_selectors, mux);
      TIME_END(TREE_FOLD_TIME);
      return folded;
    }
  }
  TIME_END(TREE_SCAN_TIME);
  // 枚举值损坏时的兜底。
  throw std::invalid_argument("tree_select: unknown level plan case");
}

// Algorithm 4 SelectLevel（标量参考路径）：按公开 plan 的三个互斥情形返回
// 加密 D_ℓ[p_ℓ] 的密文 C_ℓ。它是 test_tree_kernel 逐位对拍的基准实现；
// 分支与循环次数只依赖公开量，私密坐标只经由金字塔/selector 密文进入。
RlweCt select_level(const TreeLevelDatabase &db, const LevelPlan &plan,
                    const AlphaPyramid &pyramid,
                    std::span<GSWCt> beta_selectors, PirServer &mux,
                    const TreePirParams &tree, const PirParams &scheme) {
  // 守卫：数据库与 plan 必须描述同一层；金字塔须有全部 a+1 行、底行宽 N0。
  require(db.level == plan.level && db.R == plan.R,
          "level database does not match the plan");
  require(pyramid.size() == tree.a + 1 && pyramid[0].size() == tree.N0,
          "alpha pyramid does not match the tree parameters");

  switch (plan.select_case) {
    case SelectCase::Single: {
      // §11.3 情形 R = 1：全层只有一个明文，无需任何选择。乘上塔顶
      // A^(a)_0（恒加密常数 1）即把明文"抬"成密文，且保持与其余分支
      // 一致的输出噪声形态。
      return tree_pt_ct_mul(db.plaintexts[0], pyramid[tree.a][0], scheme);
    }
    case SelectCase::CoarsenedAlpha: {
      // §11.3 情形 1 < R < N0：粗化行 c = a − log2(R) 恰是 R 维 one-hot，
      // 对 ⌊α/2^c⌋ 选一——内积 Σ_k D[k]·A^(c)_k 只留目标明文。
      const size_t c = plan.coarsen_count;
      require(pyramid[c].size() == db.R,
              "coarsened pyramid level does not match R");
      // k = 0 项作累加器初值，其余项逐个 PtCtMul + CtAdd。
      RlweCt acc = tree_pt_ct_mul(db.plaintexts[0], pyramid[c][0], scheme);
      for (size_t k = 1; k < db.R; ++k) {
        RlweCt term = tree_pt_ct_mul(db.plaintexts[k], pyramid[c][k], scheme);
        ct_add_inplace(acc, term, scheme);
      }
      return acc;
    }
    case SelectCase::AlphaBeta: {
      // §11.3 情形 R >= N0：先用完整 one-hot A^(0) 消掉 α 维得到 2^d 个
      // β 候选，再用 RGSW 选择器 MSB-first 折叠出唯一目标。
      std::vector<RlweCt> candidates =
          evaluate_alpha_dimension(db, pyramid[0], plan, tree, scheme);
      return fold_beta_dimension(std::move(candidates), plan, beta_selectors,
                                 mux);
    }
  }
  // 枚举值损坏时的兜底。
  throw std::invalid_argument("tree_select: unknown level plan case");
}
