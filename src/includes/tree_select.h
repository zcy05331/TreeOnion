#pragma once

#include "aligned_allocator.h"
#include "logging.h"
#include "matrix.h"
#include "server.h"
#include "tree_index.h"
#include "tree_query.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#define TREE_PYRAMID_TIME "Tree alpha pyramid"
#define TREE_SCAN_TIME "Tree first-dim scan"
#define TREE_SCAN_CORE_TIME "Tree first-dim scan core"
#define TREE_SCAN_FINALIZE_TIME "Tree first-dim scan finalize"
#define TREE_FOLD_TIME "Tree beta fold"
#define TREE_ROTATE_TIME "Tree private rotate"
#define TREE_PROJECT_TIME "Tree project"
#define TREE_PACK_TIME "Tree pack shift+add"
#define TREE_SWITCH_TIME "Tree response switch"

struct TreeLevelDatabase {
  size_t level = 0;
  size_t R = 0;
  std::vector<std::vector<uint64_t>> plaintexts;
};

using TreeNodeSource = std::function<uint64_t(size_t level, size_t index)>;
using TreeNodeChunkSource =
    std::function<uint64_t(size_t level, size_t index, size_t chunk)>;

uint64_t synthetic_tree_node_value(size_t level, size_t index, uint64_t t);

std::array<uint8_t, 32> synthetic_tree_node_bytes(size_t level, size_t index);
uint64_t synthetic_tree_node_bytes_chunk(size_t level, size_t index,
                                         size_t chunk, uint64_t t);

TreeLevelDatabase pack_tree_level(size_t level, const TreePirParams &tree,
                                  const TreeNodeSource &source);
TreeLevelDatabase pack_tree_level(size_t level, const TreePirParams &tree,
                                  const TreeNodeChunkSource &source);
std::vector<TreeLevelDatabase> preprocess_tree_reference(
    const TreePirParams &tree, const TreeNodeSource &source);
std::vector<TreeLevelDatabase> preprocess_tree_reference(
    const TreePirParams &tree, const TreeNodeChunkSource &source);

RlweCt tree_pt_ct_mul(const std::vector<uint64_t> &pt, const RlweCt &ct,
                      const PirParams &scheme);

using AlphaPyramid = std::vector<std::vector<RlweCt>>;
AlphaPyramid build_alpha_pyramid(std::span<const RlweCt> alpha_cts,
                                 const TreePirParams &tree,
                                 const PirParams &scheme);

std::vector<RlweCt> evaluate_alpha_dimension(const TreeLevelDatabase &db,
                                             std::span<const RlweCt> alpha,
                                             const LevelPlan &plan,
                                             const TreePirParams &tree,
                                             const PirParams &scheme);

RlweCt fold_beta_dimension(std::vector<RlweCt> candidates,
                           const LevelPlan &plan,
                           std::span<GSWCt> beta_selectors, PirServer &mux);

RlweCt select_level(const TreeLevelDatabase &db, const LevelPlan &plan,
                    const AlphaPyramid &pyramid,
                    std::span<GSWCt> beta_selectors, PirServer &mux,
                    const TreePirParams &tree, const PirParams &scheme);

struct TreeLevelDatabaseNtt {
  size_t level = 0;
  size_t R = 0;
  std::vector<std::vector<uint64_t>> plaintexts;
};
TreeLevelDatabaseNtt build_level_ntt_view(const TreeLevelDatabase &db,
                                          const PirParams &scheme);

struct TreeLevelDatabaseM32 {
  size_t level = 0;
  size_t R = 0;
  size_t rows = 0;
  size_t cols = 0;
  CacheAlignedVector<uint32_t> lo, hi;
};
TreeLevelDatabaseM32 build_level_m32_view(const TreeLevelDatabase &db,
                                          const LevelPlan &plan,
                                          const TreePirParams &tree,
                                          const PirParams &scheme);
std::pair<size_t, size_t> level_m32_shape(const LevelPlan &plan,
                                          const TreePirParams &tree);
void write_level_m32(const TreeLevelDatabase &db, const LevelPlan &plan,
                     const TreePirParams &tree, const PirParams &scheme,
                     size_t total_rows, size_t first_row, uint32_t *lo,
                     uint32_t *hi);

struct StackedAlphaBetaM32 {
  size_t rows = 0, cols = 0;
  CacheAlignedVector<uint32_t> lo, hi;
  std::vector<std::vector<size_t>> first_row;
};

struct PreprocessedTree {
  std::vector<TreeLevelDatabase> canonical;
  std::vector<TreeLevelDatabaseNtt> ntt;
  std::vector<TreeLevelDatabaseM32> m32;
  std::vector<LevelPlan> plans;
};
PreprocessedTree preprocess_tree_mvp(const TreePirParams &tree,
                                     const TreeNodeSource &source,
                                     const PirParams &scheme);
PreprocessedTree preprocess_tree_mvp(const TreePirParams &tree,
                                     const TreeNodeChunkSource &source,
                                     const PirParams &scheme);

AlphaPyramid pyramid_to_ntt(const AlphaPyramid &pyramid,
                            const PirParams &scheme);

AlphaPyramid build_alpha_pyramid_ntt(std::span<const RlweCt> alpha_cts,
                                     const TreePirParams &tree,
                                     const PirParams &scheme);

RlweCt select_level_ntt(const TreeLevelDatabaseNtt &db, const LevelPlan &plan,
                        const AlphaPyramid &pyramid_ntt,
                        std::span<GSWCt> beta_selectors, PirServer &mux,
                        const TreePirParams &tree, const PirParams &scheme);

struct AlphaPyramidM32 {
  std::vector<CacheAlignedVector<uint32_t>> lo, hi;
  Matrix32QueryLayout query_layout = Matrix32QueryLayout::Interleaved;
};
AlphaPyramidM32 pyramid_to_m32(const AlphaPyramid &pyramid_ntt,
                               const PirParams &scheme,
                               Matrix32QueryLayout query_layout =
                                   Matrix32QueryLayout::Interleaved);

AlphaPyramidM32 build_alpha_pyramid_m32(std::span<const RlweCt> alpha_cts,
                                        const TreePirParams &tree,
                                        const PirParams &scheme,
                                        Matrix32QueryLayout query_layout =
                                            Matrix32QueryLayout::Interleaved);

RlweCt select_level_m32(const TreeLevelDatabaseM32 &db, const LevelPlan &plan,
                        const AlphaPyramidM32 &pyramid_m32,
                        std::span<GSWCt> beta_selectors, PirServer &mux,
                        const TreePirParams &tree, const PirParams &scheme);

struct ScanScratch {
  CacheAlignedVector<uint64_t> lo, hi;
};
RlweCt select_level_m32(const TreeLevelDatabaseM32 &db, const LevelPlan &plan,
                        const AlphaPyramidM32 &pyramid_m32,
                        std::span<GSWCt> beta_selectors, PirServer &mux,
                        const TreePirParams &tree, const PirParams &scheme,
                        ScanScratch &scratch);

void scan_stacked_alpha_beta(const StackedAlphaBetaM32 &db,
                             const AlphaPyramidM32 &pyramid_m32,
                             const PirParams &scheme, ScanScratch &scratch);

RlweCt finish_level_m32(const ScanScratch &scan, size_t total_rows,
                        size_t first_row, const LevelPlan &plan,
                        std::span<GSWCt> beta_selectors, PirServer &mux,
                        const TreePirParams &tree, const PirParams &scheme);
