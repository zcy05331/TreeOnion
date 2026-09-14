#include "tree_select.h"

#include "tree_select_internal.h"
#include "utils.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <utility>

using tree_select_internal::plan_row_and_cols;
using tree_select_internal::require;

namespace {

// SplitMix64 的单步混合（固定常数的加-异或-乘雪崩）：用作合成节点数据的
// 确定性伪随机源，保证同一 (level, index) 在任何机器上得到相同节点值。
uint64_t splitmix64_once(uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}


}  // namespace

// 合成标量节点值（g = 1 测试/基准 oracle）：把 (level, index) 混合成一个
// 确定性的 64 位值再约减 mod t。不是密码学哈希，只求可复现且分布均匀。
uint64_t synthetic_tree_node_value(size_t level, size_t index, uint64_t t) {
  // 守卫：t = 0 时 "% t" 是未定义行为，直接报参数错误。
  if (t == 0) {
    throw std::invalid_argument("synthetic node values need a modulus");
  }
  // 域分隔常数 "treePir" 异或上 level、index 各乘一个大奇数，再过一次
  // SplitMix64 雪崩——保证不同 (level, index) 映射到互不相关的值。
  const uint64_t mixed = splitmix64_once(
      0x74726565506972ULL ^
      (static_cast<uint64_t>(level) * 0xd6e8feb86659fd93ULL) ^
      (static_cast<uint64_t>(index) * 0xa5a3564e27f8862fULL));
  // 约减到明文域 Z_t，直接可写入打包明文。
  return mixed % t;
}

// 合成 32 字节节点（模拟真实场景中哈希/承诺类的 256 位负载）：
// 由 (level, index) 确定性生成 4 个 SplitMix64 字，按小端展开成 32 字节。
std::array<uint8_t, 32> synthetic_tree_node_bytes(size_t level,
                                                  size_t index) {
  std::array<uint8_t, 32> node{};
  // 种子：域分隔常数 "treeNode"（与标量版的 "treePir" 区分开）异或
  // level、index 各乘一个大奇数——同一节点在两种 oracle 下互不混淆。
  uint64_t state = 0x747265654e6f6465ULL ^
                   (static_cast<uint64_t>(level) * 0xd6e8feb86659fd93ULL) ^
                   (static_cast<uint64_t>(index) * 0xa5a3564e27f8862fULL);
  // 4 个 64 位字 = 256 位；每个字加上序号 word 再混合一次，随后按
  // 小端字节序写入 node[word*8 .. word*8+7]。
  for (size_t word = 0; word < 4; ++word) {
    state = splitmix64_once(state + word);
    for (size_t byte = 0; byte < 8; ++byte) {
      node[word * 8 + byte] = static_cast<uint8_t>(state >> (byte * 8));
    }
  }
  return node;
}

// 32 字节合成节点的第 chunk 个分片：把 256 位值按 chunk 宽度切成小端窗口，
// 每个窗口装进一个 Z_t 系数槽位——这是 TreeNodeChunkSource 的测试实现。
uint64_t synthetic_tree_node_bytes_chunk(size_t level, size_t index,
                                         size_t chunk, uint64_t t) {
  // chunk 宽度取方案的可用系数容量：floor(log2(t)) = bit_width(t)−1 位
  // （13 位 t 给 12 位，40 位 t 给 39 位），保证每个分片值严格小于 t，
  // 装入槽位后无信息丢失。
  const size_t width = static_cast<size_t>(std::bit_width(t)) - 1;
  // 守卫：t <= 1 时宽度为 0，分片无法承载任何信息。
  if (width == 0) {
    throw std::invalid_argument(
        "tree_select: node chunks need a plaintext modulus above 1");
  }
  // 重新生成完整 32 字节节点（确定性，无需缓存）。
  const std::array<uint8_t, 32> node = synthetic_tree_node_bytes(level, index);
  // 取 256 位值的小端窗口 [width·chunk, width·(chunk+1))：逐位从字节数组
  // 抽出并放到 value 的第 bit 位；越过第 256 位的部分读作 0（所以尾部
  // 未占用的 chunk 自然为 0，客户端拼回时无需特判）。
  uint64_t value = 0;
  for (size_t bit = 0; bit < width; ++bit) {
    const size_t position = width * chunk + bit;
    if (position >= 256) break;
    const uint8_t byte = node[position / 8];
    value |= static_cast<uint64_t>((byte >> (position % 8)) & 1U) << bit;
  }
  return value;
}

// Algorithm 2 打包核心（chunk 版）：把第 level 层的全部 2^level 个节点写进
// R_ℓ 个规范系数形式明文。布局是整个 g>1 设计的秘密所在（见头文件
// TreeLevelDatabase 注释）：节点按 "node = p + u·R_ℓ" 分配到明文 p 的记录
// 槽位 u，记录的 g 个 chunk 落在跨步系数 {u + j·ρ}。
TreeLevelDatabase pack_tree_level(size_t level, const TreePirParams &tree,
                                  const TreeNodeChunkSource &source) {
  // 守卫：先做整套 §3.2 参数校验，再确认层号没有越过树高 L。
  validate_tree_params(tree);
  require(level <= tree.L, "level is out of range");

  TreeLevelDatabase db;
  db.level = level;
  // R_ℓ = 2^max(ℓ-r, 0)：浅层（ℓ < r，节点数 < ρ）单个明文就装得下全层。
  db.R = level >= tree.r ? size_t{1} << (level - tree.r) : size_t{1};
  const size_t node_count = size_t{1} << level;
  // 先全零初始化：未占用的槽位保持 0，正确性依赖这一点（选择/投影后
  // 非目标格位必须干净）。
  db.plaintexts.assign(db.R, std::vector<uint64_t>(tree.n, 0));
  // §6.1 推广形式：D_ℓ[p] 的记录槽位 u 存节点 M[ℓ][p + u·R_ℓ]，第 j 个
  // chunk 写到系数 u + j·ρ。这样同一记录的所有 chunk 对 ρ 同余于 u，
  // 一次旋转 X^{-γ} 即可全部对齐。
  for (size_t p = 0; p < db.R; ++p) {
    for (size_t u = 0; u < tree.rho; ++u) {
      // 反算该槽位承载的节点号；本层节点用完即停（浅层明文只占前几个
      // 槽位，其余保持零填充）。
      const size_t node = p + u * db.R;
      if (node >= node_count) break;
      // 逐 chunk 写入：数据源已按 mod t 约减，这里再取一次 % t 兜底，
      // 保证明文系数恒在 Z_t 内。
      for (size_t j = 0; j < tree.g; ++j) {
        db.plaintexts[p][u + j * tree.rho] = source(level, node, j) % tree.t;
      }
    }
  }
  return db;
}

// 标量数据源的打包重载：把 TreeNodeSource 适配成忽略 chunk 参数的
// TreeNodeChunkSource，然后复用 chunk 版实现——单一打包逻辑，避免两份漂移。
TreeLevelDatabase pack_tree_level(size_t level, const TreePirParams &tree,
                                  const TreeNodeSource &source) {
  // 守卫：标量源没有 chunk 维度，只在 g = 1 的配置下才有意义。
  require(tree.g == 1, "scalar node sources require g = 1");
  return pack_tree_level(
      level, tree,
      TreeNodeChunkSource([&source](size_t l, size_t index, size_t) {
        return source(l, index);
      }));
}

// Algorithm 2 PreprocessTreeReference（chunk 版）：逐层调用 pack_tree_level，
// 得到全部 L+1 个层数据库——这是服务端离线预处理的规范形式。
std::vector<TreeLevelDatabase> preprocess_tree_reference(
    const TreePirParams &tree, const TreeNodeChunkSource &source) {
  std::vector<TreeLevelDatabase> levels;
  levels.reserve(tree.L + 1);
  for (size_t level = 0; level <= tree.L; ++level) {
    levels.push_back(pack_tree_level(level, tree, source));
  }
  return levels;
}

// 标量数据源的全树打包重载：同样是 g = 1 适配后转发到 chunk 版。
std::vector<TreeLevelDatabase> preprocess_tree_reference(
    const TreePirParams &tree, const TreeNodeSource &source) {
  require(tree.g == 1, "scalar node sources require g = 1");
  return preprocess_tree_reference(
      tree, TreeNodeChunkSource([&source](size_t l, size_t index, size_t) {
        return source(l, index);
      }));
}

// 构建 NTT-u64 优化视图（Milestone-6，§6.3 第 1 步）：预处理期把每个规范
// 明文提升到每个 RNS limb 并各做一次前向 NTT。此后每次查询只需 NTT 域逐点
// 乘累加——数据库侧的前向 NTT 从"每查询每项一次"变成"预处理期一次"。
TreeLevelDatabaseNtt build_level_ntt_view(const TreeLevelDatabase &db,
                                          const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = scheme.get_rns_mods();
  const size_t K = qs.size();
  // 元数据（层号、R）原样携带，供后续核与 plan 匹配校验。
  TreeLevelDatabaseNtt view;
  view.level = db.level;
  view.R = db.R;
  view.plaintexts.reserve(db.plaintexts.size());
  for (const std::vector<uint64_t> &pt : db.plaintexts) {
    // 守卫：规范明文必须是 n 次多项式。
    require(pt.size() == N, "canonical plaintext has the wrong degree");
    // 每个明文展成 K·N 的 limb-major 缓冲：明文系数 < t < q_k，对每个
    // limb 都是合法代表元，"提升"就是逐 limb 拷贝，随后原位前向 NTT。
    std::vector<uint64_t> lifted(K * N);
    for (size_t k = 0; k < K; ++k) {
      std::copy(pt.begin(), pt.end(), lifted.begin() + k * N);
      utils::ntt_fwd(lifted.data() + k * N, N, qs[k]);
    }
    view.plaintexts.push_back(std::move(lifted));
  }
  return view;
}

// MVP 预处理入口（蓝图 §19 PreprocessedTree）：一次性产出应答路径需要的
// 全部离线材料——公开 plan、规范层数据库、以及按方案配置二选一的优化视图。
PreprocessedTree preprocess_tree_mvp(const TreePirParams &tree,
                                     const TreeNodeChunkSource &source,
                                     const PirParams &scheme) {
  PreprocessedTree result;
  // 公开的逐层选择计划（只依赖参数，不依赖数据），供每层核选择分支。
  result.plans = build_level_plans(tree);
  // 规范系数形式的全树打包（Algorithm 2）。
  result.canonical = preprocess_tree_reference(tree, source);
  // 优化视图二选一：复合模数配置建 m32、留空 ntt；否则建 ntt、留空 m32。
  // 两种视图等大（8 B/系数），同时保留会让数据库内存翻倍。
  if (scheme.get_composite_rns().enabled) {
    result.m32.reserve(result.canonical.size());
    for (size_t level = 0; level < result.canonical.size(); ++level) {
      result.m32.push_back(build_level_m32_view(
          result.canonical[level], result.plans[level], tree, scheme));
    }
  } else {
    result.ntt.reserve(result.canonical.size());
    for (const TreeLevelDatabase &db : result.canonical) {
      result.ntt.push_back(build_level_ntt_view(db, scheme));
    }
  }
  return result;
}

// 标量数据源的 MVP 预处理重载：g = 1 适配后转发到 chunk 版。
PreprocessedTree preprocess_tree_mvp(const TreePirParams &tree,
                                     const TreeNodeSource &source,
                                     const PirParams &scheme) {
  require(tree.g == 1, "scalar node sources require g = 1");
  return preprocess_tree_mvp(
      tree, TreeNodeChunkSource([&source](size_t l, size_t index, size_t) {
        return source(l, index);
      }),
      scheme);
}

// 构建 m32 矩阵核视图（Milestone-6 完整核，预处理期）：对每个明文做一次
// mod q 的前向 NTT，把 NTT 值按复合模数 q = q1·q2 取余拆成两组 u32，并重排
// 成 level_mat_mat_32 所需的 coefficient-major A 操作数布局
// data[(coeff·rows + row)·cols + col]——同一 NTT 系数下所有 (row, col) 连续，
// 矩阵核可对每个系数独立跑一个小矩阵乘。
TreeLevelDatabaseM32 build_level_m32_view(const TreeLevelDatabase &db,
                                          const LevelPlan &plan,
                                          const TreePirParams &tree,
                                          const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const CompositeRnsTables &crt = scheme.get_composite_rns();
  // 守卫：q1/q2 拆分表只在复合模数配置下存在；db 与 plan 必须描述同一层。
  require(crt.enabled, "the m32 view needs a composite configuration");
  require(db.level == plan.level && db.R == plan.R,
          "level database does not match the plan");

  TreeLevelDatabaseM32 view;
  view.level = db.level;
  view.R = db.R;
  // 矩阵形状：AlphaBeta 有 2^d 个候选行、其余情形单行；列宽由
  // plan_row_and_cols 给出（1 / R / N0），且必须 rows·cols = R 覆盖全部记录。
  const size_t stride =
      plan.select_case == SelectCase::AlphaBeta ? size_t{1} << plan.beta_count
                                                : size_t{1};
  view.rows = plan.select_case == SelectCase::AlphaBeta ? stride : size_t{1};
  view.cols = plan_row_and_cols(plan, tree).second;
  require(view.rows * view.cols == db.R, "m32 view must cover every record");

  // 复合配置只有一个 RNS limb（q = q1·q2 装在 rns_mods[0]），NTT 就按它做。
  const uint64_t q = scheme.get_rns_mods()[0];
  view.lo.assign(N * view.rows * view.cols, 0);
  view.hi.assign(N * view.rows * view.cols, 0);
  std::vector<uint64_t> ntt(N);
  for (size_t row = 0; row < view.rows; ++row) {
    for (size_t col = 0; col < view.cols; ++col) {
      // 明文下标映射：AlphaBeta 消费 §6.2 矩阵视图 D[col·2^d + row]
      // （col=k 扫首维、row=δ 是候选）；单行情形按记录顺序 p = col 扫描。
      const size_t p = plan.select_case == SelectCase::AlphaBeta
                           ? col * stride + row
                           : col;
      // 该明文做一次 mod q 前向 NTT（预处理期一次性成本）。
      std::copy(db.plaintexts[p].begin(), db.plaintexts[p].end(), ntt.begin());
      utils::ntt_fwd(ntt.data(), N, q);
      // 逐 NTT 系数按 q1/q2 取余得到两个 32 位 CRT limb，写入
      // coefficient-major 位置：这一步既是 CRT 拆分也是布局转置。
      for (size_t coeff = 0; coeff < N; ++coeff) {
        const size_t at = (coeff * view.rows + row) * view.cols + col;
        view.lo[at] = static_cast<uint32_t>(ntt[coeff] % crt.q1);
        view.hi[at] = static_cast<uint32_t>(ntt[coeff] % crt.q2);
      }
    }
  }
  return view;
}
