#include "tree_multiplane.h"
#include "utils.h"

#include <bit>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>

namespace tree_multiplane {
namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::invalid_argument(std::string("tree_multiplane: ") + message);
  }
}

tree_entry_codec::EncodedEntry encode_entry(const EntryBits &entry,
                                            const Params &params) {
  tree_entry_codec::validate_entry(entry);
  require(entry.bit_length == params.codec.entry_bits,
          "source entry bit length does not match the codec");
  return tree_entry_codec::encode_entry(entry, params.codec);
}

}  // namespace

Params make_params(const TreePirParams &tree, size_t entry_bits) {
  validate_tree_params(tree);
  require(entry_bits > 0, "entry_bits must be positive");
  const size_t payload_bits =
      static_cast<size_t>(std::bit_width(tree.t)) - 1;
  require(payload_bits > 0, "plaintext modulus carries no payload bits");

  Params params;
  params.tree = tree;
  params.codec.entry_bits = entry_bits;
  params.codec.payload_bits_per_coeff = payload_bits;
  params.codec.record_coeffs = tree.g;
  params.codec.plane_count = tree_entry_codec::minimal_plane_count(
      entry_bits, payload_bits, params.codec.record_coeffs);
  params.codec.plaintext_modulus = tree.t;
  tree_entry_codec::validate_codec_spec(params.codec);
  return params;
}

TreeLevelDatabase pack_level(size_t level, size_t plane, const Params &params,
                             const EntrySource &source) {
  require(plane < params.codec.plane_count, "plane index is out of range");
  size_t cached_level = static_cast<size_t>(-1);
  size_t cached_node = static_cast<size_t>(-1);
  tree_entry_codec::EncodedEntry cached;
  const TreeNodeChunkSource chunks =
      [&](size_t l, size_t node, size_t chunk) -> uint64_t {
    require(chunk < params.tree.g, "chunk index is out of range");
    if (cached_level != l || cached_node != node) {
      cached = encode_entry(source(l, node), params);
      cached_level = l;
      cached_node = node;
    }
    return cached[plane][chunk];
  };
  return pack_tree_level(level, params.tree, chunks);
}

Database preprocess(const Params &params, const EntrySource &source,
                    const PirParams &scheme) {
  constexpr size_t N = DBConsts::PolyDegree;
  validate_tree_params(params.tree);
  tree_entry_codec::validate_codec_spec(params.codec);
  const bool m32 = scheme.get_composite_rns().enabled;
  const std::vector<LevelPlan> plans = build_level_plans(params.tree);
  Database db;
  StackedAlphaBetaM32 &stacked = db.stacked;
  if (m32) {
    // AlphaBeta rows stack level by level, plane by plane within a level, in
    // the order answer consumes them.
    stacked.cols = params.tree.N0;
    stacked.first_row.assign(params.codec.plane_count,
                             std::vector<size_t>(params.tree.L + 1, 0));
    for (size_t level = 0; level <= params.tree.L; ++level) {
      if (plans[level].select_case != SelectCase::AlphaBeta) continue;
      for (size_t plane = 0; plane < params.codec.plane_count; ++plane) {
        stacked.first_row[plane][level] = stacked.rows;
        stacked.rows += level_m32_shape(plans[level], params.tree).first;
      }
    }
    const size_t cells = N * stacked.rows * stacked.cols;
    for (std::vector<uint32_t> *limb : {&stacked.lo, &stacked.hi}) {
      limb->reserve(cells);
      utils::advise_huge_pages(limb->data(), cells * sizeof(uint32_t));
      limb->assign(cells, 0);
    }
  }
  db.planes.reserve(params.codec.plane_count);
  for (size_t plane = 0; plane < params.codec.plane_count; ++plane) {
    PreprocessedTree out;
    out.plans = plans;
    if (m32) {
      out.m32.reserve(params.tree.L + 1);
    } else {
      out.ntt.reserve(params.tree.L + 1);
    }
    for (size_t level = 0; level <= params.tree.L; ++level) {
      TreeLevelDatabase level_db = pack_level(level, plane, params, source);
      const LevelPlan &plan = out.plans[level];
      if (m32 && plan.select_case == SelectCase::AlphaBeta) {
        write_level_m32(level_db, plan, params.tree, scheme, stacked.rows,
                        stacked.first_row[plane][level], stacked.lo.data(),
                        stacked.hi.data());
        TreeLevelDatabaseM32 shape;
        shape.level = level;
        shape.R = plan.R;
        std::tie(shape.rows, shape.cols) = level_m32_shape(plan, params.tree);
        out.m32.push_back(std::move(shape));
      } else if (m32) {
        out.m32.push_back(
            build_level_m32_view(level_db, plan, params.tree, scheme));
      } else {
        out.ntt.push_back(build_level_ntt_view(level_db, scheme));
      }
    }
    db.planes.push_back(std::move(out));
  }
  return db;
}

}  // namespace tree_multiplane
