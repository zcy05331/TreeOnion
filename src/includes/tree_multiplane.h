#pragma once

#include "client.h"
#include "matrix.h"
#include "server.h"
#include "tree_query.h"
#include "tree_response.h"
#include "tree_response_merge.h"
#include "tree_select.h"
#include "tree_superrecord_codec.h"

#include <cstddef>
#include <functional>
#include <vector>

#define TREE_MLWE_MERGE_TIME "Tree MLWE merge"

namespace tree_multiplane {

using EntryBits = tree_superrecord::EntryBits;
using EntrySource = std::function<EntryBits(size_t level, size_t node)>;

struct Params {
  TreePirParams tree;
  tree_superrecord::CodecSpec codec;
};

Params make_params(const TreePirParams &tree, size_t entry_bits);

struct Database {
  std::vector<PreprocessedTree> planes;
};

TreeLevelDatabase pack_level(size_t level, size_t plane, const Params &params,
                             const EntrySource &source);
Database preprocess(const Params &params, const EntrySource &source,
                    const PirParams &scheme);

struct Response {
  std::vector<RlweCt> chunks;
  std::vector<size_t> item_offsets;
  bool small_q = false;
};

Response answer(const Database &db, ExpandedTreeQuery &query, PirServer &server,
                size_t client_id, const Params &params,
                const PirParams &scheme,
                tree_superrecord::ResponsePacking packing =
                    tree_superrecord::ResponsePacking::MlweMerge,
                Matrix32QueryLayout query_layout =
                    Matrix32QueryLayout::Interleaved);

std::vector<EntryBits> extract(const Response &response, PirClient &client,
                               const Params &params);

}  // namespace tree_multiplane
