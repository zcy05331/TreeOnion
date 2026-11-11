#include "tree_multiplane.h"
#include "utils.h"

#include <algorithm>
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

}

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

namespace {

std::vector<TreeLevelDatabase> pack_level_all_planes(
    size_t level, const Params &params, const EntrySource &source) {
  const TreePirParams &tree = params.tree;
  require(level <= tree.L, "level is out of range");
  const size_t planes = params.codec.plane_count;
  const size_t R = level >= tree.r ? size_t{1} << (level - tree.r) : size_t{1};
  const size_t node_count = size_t{1} << level;
  std::vector<TreeLevelDatabase> dbs(planes);
  for (TreeLevelDatabase &db : dbs) {
    db.level = level;
    db.R = R;
    db.plaintexts.assign(R, std::vector<uint64_t>(tree.n, 0));
  }
  for (size_t p = 0; p < R; ++p) {
    for (size_t u = 0; u < tree.rho; ++u) {
      const size_t node = p + u * R;
      if (node >= node_count) break;
      const tree_entry_codec::EncodedEntry encoded =
          encode_entry(source(level, node), params);
      for (size_t plane = 0; plane < planes; ++plane) {
        std::vector<uint64_t> &pt = dbs[plane].plaintexts[p];
        for (size_t j = 0; j < tree.g; ++j) {
          pt[u + j * tree.rho] = encoded[plane][j] % tree.t;
        }
      }
    }
  }
  return dbs;
}

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
    stacked.lo.assign(N * stacked.rows * stacked.cols, 0);
    stacked.hi.assign(N * stacked.rows * stacked.cols, 0);
  }
  const size_t plane_count = params.codec.plane_count;
  std::vector<PreprocessedTree> outs(plane_count);
  for (PreprocessedTree &out : outs) {
    out.plans = plans;
    if (m32) {
      out.m32.reserve(params.tree.L + 1);
    } else {
      out.ntt.reserve(params.tree.L + 1);
    }
  }
  for (size_t level = 0; level <= params.tree.L; ++level) {
    const std::vector<TreeLevelDatabase> level_dbs =
        pack_level_all_planes(level, params, source);
    const LevelPlan &plan = plans[level];
    for (size_t plane = 0; plane < plane_count; ++plane) {
      const TreeLevelDatabase &level_db = level_dbs[plane];
      PreprocessedTree &out = outs[plane];
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
  }
  db.planes.reserve(plane_count);
  for (PreprocessedTree &out : outs) db.planes.push_back(std::move(out));
  return db;
}

void update_entry(Database &db, const Params &params, const PirParams &scheme,
                  size_t level, size_t node, const EntryBits &old_entry,
                  const EntryBits &new_entry) {
  constexpr size_t N = DBConsts::PolyDegree;
  const CompositeRnsTables &crt = scheme.get_composite_rns();
  require(crt.enabled, "updates need the m32 database");
  const TreePirParams &tree = params.tree;
  require(level <= tree.L && node < (size_t{1} << level), "node is out of range");
  const LevelPlan &plan = db.planes.front().plans[level];
  const size_t p = node % plan.R, u = node / plan.R;
  const auto [rows, cols] = level_m32_shape(plan, tree);
  const bool alpha_beta = plan.select_case == SelectCase::AlphaBeta;
  const size_t row = alpha_beta ? p % rows : 0, col = alpha_beta ? p / rows : p;
  const auto old_digits = encode_entry(old_entry, params);
  const auto new_digits = encode_entry(new_entry, params);
  const uint64_t q = scheme.get_rns_mods()[0];
  std::vector<uint64_t> delta(N);
  for (size_t plane = 0; plane < params.codec.plane_count; ++plane) {
    std::fill(delta.begin(), delta.end(), 0);
    for (size_t d = 0; d < tree.g; ++d)
      delta[u + d * tree.rho] = (new_digits[plane][d] + q - old_digits[plane][d]) % q;
    utils::ntt_fwd(delta.data(), N, q);
    uint32_t *lo, *hi;
    size_t total_rows, first_row;
    if (alpha_beta) {
      lo = db.stacked.lo.data();
      hi = db.stacked.hi.data();
      total_rows = db.stacked.rows;
      first_row = db.stacked.first_row[plane][level];
    } else {
      TreeLevelDatabaseM32 &view = db.planes[plane].m32[level];
      lo = view.lo.data();
      hi = view.hi.data();
      total_rows = view.rows;
      first_row = 0;
    }
    for (size_t coeff = 0; coeff < N; ++coeff) {
      const size_t at = (coeff * total_rows + first_row + row) * cols + col;
      lo[at] = static_cast<uint32_t>((lo[at] + delta[coeff] % crt.q1) % crt.q1);
      hi[at] = static_cast<uint32_t>((hi[at] + delta[coeff] % crt.q2) % crt.q2);
    }
  }
}

}
