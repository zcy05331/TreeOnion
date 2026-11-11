#include "tree_select.h"

#include "tree_select_internal.h"
#include "utils.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <tuple>
#include <utility>

using tree_select_internal::plan_row_and_cols;
using tree_select_internal::require;

namespace {

uint64_t splitmix64_once(uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}


}

uint64_t synthetic_tree_node_value(size_t level, size_t index, uint64_t t) {
  if (t == 0) {
    throw std::invalid_argument("synthetic node values need a modulus");
  }
  const uint64_t mixed = splitmix64_once(
      0x74726565506972ULL ^
      (static_cast<uint64_t>(level) * 0xd6e8feb86659fd93ULL) ^
      (static_cast<uint64_t>(index) * 0xa5a3564e27f8862fULL));
  return mixed % t;
}

std::array<uint8_t, 32> synthetic_tree_node_bytes(size_t level,
                                                  size_t index) {
  std::array<uint8_t, 32> node{};
  uint64_t state = 0x747265654e6f6465ULL ^
                   (static_cast<uint64_t>(level) * 0xd6e8feb86659fd93ULL) ^
                   (static_cast<uint64_t>(index) * 0xa5a3564e27f8862fULL);
  for (size_t word = 0; word < 4; ++word) {
    state = splitmix64_once(state + word);
    for (size_t byte = 0; byte < 8; ++byte) {
      node[word * 8 + byte] = static_cast<uint8_t>(state >> (byte * 8));
    }
  }
  return node;
}

uint64_t synthetic_tree_node_bytes_chunk(size_t level, size_t index,
                                         size_t chunk, uint64_t t) {
  const size_t width = static_cast<size_t>(std::bit_width(t)) - 1;
  if (width == 0) {
    throw std::invalid_argument(
        "tree_select: node chunks need a plaintext modulus above 1");
  }
  const std::array<uint8_t, 32> node = synthetic_tree_node_bytes(level, index);
  uint64_t value = 0;
  for (size_t bit = 0; bit < width; ++bit) {
    const size_t position = width * chunk + bit;
    if (position >= 256) break;
    const uint8_t byte = node[position / 8];
    value |= static_cast<uint64_t>((byte >> (position % 8)) & 1U) << bit;
  }
  return value;
}

TreeLevelDatabase pack_tree_level(size_t level, const TreePirParams &tree,
                                  const TreeNodeChunkSource &source) {
  validate_tree_params(tree);
  require(level <= tree.L, "level is out of range");

  TreeLevelDatabase db;
  db.level = level;
  db.R = level >= tree.r ? size_t{1} << (level - tree.r) : size_t{1};
  const size_t node_count = size_t{1} << level;
  db.plaintexts.assign(db.R, std::vector<uint64_t>(tree.n, 0));
  for (size_t p = 0; p < db.R; ++p) {
    for (size_t u = 0; u < tree.rho; ++u) {
      const size_t node = p + u * db.R;
      if (node >= node_count) break;
      for (size_t j = 0; j < tree.g; ++j) {
        db.plaintexts[p][u + j * tree.rho] = source(level, node, j) % tree.t;
      }
    }
  }
  return db;
}

TreeLevelDatabase pack_tree_level(size_t level, const TreePirParams &tree,
                                  const TreeNodeSource &source) {
  require(tree.g == 1, "scalar node sources require g = 1");
  return pack_tree_level(
      level, tree,
      TreeNodeChunkSource([&source](size_t l, size_t index, size_t) {
        return source(l, index);
      }));
}

std::vector<TreeLevelDatabase> preprocess_tree_reference(
    const TreePirParams &tree, const TreeNodeChunkSource &source) {
  std::vector<TreeLevelDatabase> levels;
  levels.reserve(tree.L + 1);
  for (size_t level = 0; level <= tree.L; ++level) {
    levels.push_back(pack_tree_level(level, tree, source));
  }
  return levels;
}

std::vector<TreeLevelDatabase> preprocess_tree_reference(
    const TreePirParams &tree, const TreeNodeSource &source) {
  require(tree.g == 1, "scalar node sources require g = 1");
  return preprocess_tree_reference(
      tree, TreeNodeChunkSource([&source](size_t l, size_t index, size_t) {
        return source(l, index);
      }));
}

TreeLevelDatabaseNtt build_level_ntt_view(const TreeLevelDatabase &db,
                                          const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  const auto &qs = scheme.get_rns_mods();
  const size_t K = qs.size();
  TreeLevelDatabaseNtt view;
  view.level = db.level;
  view.R = db.R;
  view.plaintexts.reserve(db.plaintexts.size());
  for (const std::vector<uint64_t> &pt : db.plaintexts) {
    require(pt.size() == N, "canonical plaintext has the wrong degree");
    std::vector<uint64_t> lifted(K * N);
    for (size_t k = 0; k < K; ++k) {
      std::copy(pt.begin(), pt.end(), lifted.begin() + k * N);
      utils::ntt_fwd(lifted.data() + k * N, N, qs[k]);
    }
    view.plaintexts.push_back(std::move(lifted));
  }
  return view;
}

PreprocessedTree preprocess_tree_mvp(const TreePirParams &tree,
                                     const TreeNodeChunkSource &source,
                                     const PirParams &scheme) {
  PreprocessedTree result;
  result.plans = build_level_plans(tree);
  result.canonical = preprocess_tree_reference(tree, source);
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

std::pair<size_t, size_t> level_m32_shape(const LevelPlan &plan,
                                          const TreePirParams &tree) {
  const size_t rows = plan.select_case == SelectCase::AlphaBeta
                          ? size_t{1} << plan.beta_count
                          : size_t{1};
  return {rows, plan_row_and_cols(plan, tree).second};
}

void write_level_m32(const TreeLevelDatabase &db, const LevelPlan &plan,
                     const TreePirParams &tree, const PirParams &scheme,
                     size_t total_rows, size_t first_row, uint32_t *lo,
                     uint32_t *hi) {
  constexpr size_t N = DBConsts::PolyDegree;
  const CompositeRnsTables &crt = scheme.get_composite_rns();
  require(crt.enabled, "the m32 view needs a composite configuration");
  require(db.level == plan.level && db.R == plan.R,
          "level database does not match the plan");
  const auto [rows, cols] = level_m32_shape(plan, tree);
  require(rows * cols == db.R, "m32 view must cover every record");
  require(first_row + rows <= total_rows, "m32 rows exceed the matrix");

  const uint64_t q = scheme.get_rns_mods()[0];
  constexpr size_t TILE = 8;
  const uint64_t q1 = crt.q1, q2 = crt.q2;
  std::vector<uint64_t> tile(TILE * N);
  for (size_t row = 0; row < rows; ++row) {
    for (size_t col0 = 0; col0 < cols; col0 += TILE) {
      const size_t bs = std::min(TILE, cols - col0);
      for (size_t b = 0; b < bs; ++b) {
        const size_t col = col0 + b;
        const size_t p = plan.select_case == SelectCase::AlphaBeta
                             ? col * rows + row
                             : col;
        std::copy(db.plaintexts[p].begin(), db.plaintexts[p].end(),
                  tile.begin() + b * N);
        utils::ntt_fwd(tile.data() + b * N, N, q);
      }
      for (size_t coeff = 0; coeff < N; ++coeff) {
        const size_t base =
            (coeff * total_rows + first_row + row) * cols + col0;
        for (size_t b = 0; b < bs; ++b) {
          const uint64_t c = tile[b * N + coeff];
          lo[base + b] = static_cast<uint32_t>(c % q1);
          hi[base + b] = static_cast<uint32_t>(c % q2);
        }
      }
    }
  }
}

TreeLevelDatabaseM32 build_level_m32_view(const TreeLevelDatabase &db,
                                          const LevelPlan &plan,
                                          const TreePirParams &tree,
                                          const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  TreeLevelDatabaseM32 view;
  view.level = db.level;
  view.R = db.R;
  std::tie(view.rows, view.cols) = level_m32_shape(plan, tree);
  view.lo.assign(N * view.rows * view.cols, 0);
  view.hi.assign(N * view.rows * view.cols, 0);
  write_level_m32(db, plan, tree, scheme, view.rows, 0, view.lo.data(),
                  view.hi.data());
  return view;
}
