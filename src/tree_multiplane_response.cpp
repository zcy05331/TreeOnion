#include "tree_multiplane.h"

#include "tree_project.h"
#include "tree_rotate.h"
#include "tree_superrecord_common.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace tree_multiplane {
namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::invalid_argument(std::string("tree_multiplane: ") + message);
  }
}

size_t item_count(const Params &params) {
  return (params.tree.L + 1) * params.codec.plane_count;
}

RlweCt zero_ciphertext(const PirParams &scheme) {
  RlweCt ct;
  const size_t width = scheme.K() * scheme.get_poly_degree();
  ct.c0.assign(width, 0);
  ct.c1.assign(width, 0);
  ct.ntt_form = false;
  return ct;
}

void require_database_shape(const Database &db, const Params &params) {
  require(db.planes.size() == params.codec.plane_count,
          "database plane count does not match params");
  for (const PreprocessedTree &plane : db.planes) {
    const bool use_m32 = !plane.m32.empty();
    require((use_m32 ? plane.m32.size() : plane.ntt.size()) ==
                params.tree.L + 1,
            "database plane is missing level views");
    require(plane.plans.size() == params.tree.L + 1,
            "database plane is missing level plans");
  }
}

void require_mlwe_keys(const bvks::BvGaloisKeys &keys,
                       const Params &params) {
  for (size_t j = 0; j < params.tree.r; ++j) {
    const uint32_t eta = static_cast<uint32_t>((params.tree.n >> j) + 1);
    require(keys.contains(eta),
            "MLWE merge requires the full rho-trace key family");
  }
}

tree_superrecord::EncodedSuperRecord decode_level_planes(
    const Response &response, const std::vector<RlwePt> &plaintexts,
    const Params &params, size_t level) {
  const size_t planes = params.codec.plane_count;
  tree_superrecord::EncodedSuperRecord encoded(
      planes, tree_superrecord::PlaneDigits(params.tree.g, 0));
  for (size_t plane = 0; plane < planes; ++plane) {
    const size_t item = level * planes + plane;
    const size_t chunk = item / params.tree.rho;
    require(chunk < response.chunks.size(), "response chunk index is invalid");
    require(item < response.item_offsets.size(),
            "response item offset map is incomplete");
    const RlwePt &pt = plaintexts[chunk];
    const size_t offset = response.item_offsets[item];
    require(offset < params.tree.rho, "response residue offset is invalid");
    for (size_t digit = 0; digit < params.tree.g; ++digit) {
      const size_t coeff = offset + digit * params.tree.rho;
      require(coeff < pt.data.size(), "decoded digit coefficient is invalid");
      encoded[plane][digit] = pt.data[coeff];
    }
  }
  return encoded;
}

}  // namespace

Response answer(const Database &db, ExpandedTreeQuery &query, PirServer &server,
                size_t client_id, const Params &params,
                const PirParams &scheme,
                tree_superrecord::ResponsePacking packing,
                Matrix32QueryLayout query_layout) {
  validate_tree_params(params.tree);
  tree_superrecord::validate_codec_spec(params.codec);
  require_database_shape(db, params);
  require(query.alpha.size() == params.tree.N0,
          "expanded query alpha width does not match tree params");
  require(query.beta_selectors.size() == params.tree.b &&
              query.gamma_selectors.size() == params.tree.r,
          "expanded query selector count does not match tree params");

  const bvks::BvGaloisKeys &keys =
      server.client_session_keys(client_id)->bv_galois_keys;
  if (packing == tree_superrecord::ResponsePacking::MlweMerge) {
    require_mlwe_keys(keys, params);
  }

  const size_t items = item_count(params);
  const size_t chunks = tree_superrecord::ceil_div(items, params.tree.rho);
  Response response;
  response.chunks.assign(chunks, zero_ciphertext(scheme));
  response.item_offsets.assign(items, 0);

  TIME_START(TREE_PYRAMID_TIME);
  const AlphaPyramid pyramid_ntt =
      build_alpha_pyramid_ntt(query.alpha, params.tree, scheme);
  const bool use_m32 = !db.planes.empty() && !db.planes.front().m32.empty();
  AlphaPyramidM32 pyramid_m32;
  if (use_m32) {
    pyramid_m32 = pyramid_to_m32(pyramid_ntt, scheme, query_layout);
  }
  TIME_END(TREE_PYRAMID_TIME);

  std::vector<RlweCt> merge_inputs;
  size_t merge_chunk = 0;
  const auto flush_merge = [&] {
    if (merge_inputs.empty()) return;
    TIME_START(TREE_MLWE_MERGE_TIME);
    response.chunks[merge_chunk] =
        tree_superrecord::mlwe_merge_residues(
            std::move(merge_inputs), params.tree.rho, keys, scheme);
    TIME_END(TREE_MLWE_MERGE_TIME);
    merge_inputs.clear();
  };

  for (size_t level = 0; level <= params.tree.L; ++level) {
    for (size_t plane = 0; plane < params.codec.plane_count; ++plane) {
      const PreprocessedTree &plane_db = db.planes[plane];
      const LevelPlan &plan = plane_db.plans[level];
      RlweCt selected =
          use_m32 ? select_level_m32(
                        plane_db.m32[level], plan, pyramid_m32,
                        std::span<GSWCt>(query.beta_selectors), server,
                        params.tree, scheme)
                  : select_level_ntt(
                        plane_db.ntt[level], plan, pyramid_ntt,
                        std::span<GSWCt>(query.beta_selectors), server,
                        params.tree, scheme);
      TIME_START(TREE_ROTATE_TIME);
      RlweCt rotated = private_rotate_level(
          std::move(selected), plan, std::span<GSWCt>(query.gamma_selectors),
          server, scheme);
      TIME_END(TREE_ROTATE_TIME);

      const size_t item = level * params.codec.plane_count + plane;
      const size_t chunk = item / params.tree.rho;
      const size_t local = item % params.tree.rho;
      const size_t offset = tree_superrecord::packed_residue_offset(
          local, params.tree.rho, packing);
      response.item_offsets[item] = offset;

      if (packing == tree_superrecord::ResponsePacking::MlweMerge) {
        if (!merge_inputs.empty() && chunk != merge_chunk) flush_merge();
        merge_chunk = chunk;
        merge_inputs.push_back(std::move(rotated));
        if (merge_inputs.size() == params.tree.rho) flush_merge();
        continue;
      }

      TIME_START(TREE_PROJECT_TIME);
      RlweCt projected = project_keep_stride(
          std::move(rotated), plan.projection_depth, keys, scheme);
      TIME_END(TREE_PROJECT_TIME);
      TIME_START(TREE_PACK_TIME);
      if (offset == 0) {
        tree_ct_add_inplace(response.chunks[chunk], projected, scheme);
      } else {
        const RlweCt shifted = mul_x_pow(projected, offset, scheme);
        tree_ct_add_inplace(response.chunks[chunk], shifted, scheme);
      }
      TIME_END(TREE_PACK_TIME);
    }
  }
  flush_merge();

  TIME_START(TREE_SWITCH_TIME);
  for (RlweCt &chunk : response.chunks) {
    response.small_q = server.switch_response_to_small_q(chunk) ||
                       response.small_q;
  }
  TIME_END(TREE_SWITCH_TIME);
  return response;
}

std::vector<EntryBits> extract(const Response &response, PirClient &client,
                               const Params &params) {
  validate_tree_params(params.tree);
  tree_superrecord::validate_codec_spec(params.codec);
  const size_t items = item_count(params);
  require(response.item_offsets.size() == items,
          "response item offset count does not match params");
  require(response.chunks.size() ==
              tree_superrecord::ceil_div(items, params.tree.rho),
          "response chunk count does not match params");

  std::vector<EntryBits> path;
  std::vector<RlwePt> plaintexts;
  plaintexts.reserve(response.chunks.size());
  for (const RlweCt &chunk : response.chunks) {
    plaintexts.push_back(response.small_q ? client.decrypt_mod_q(chunk)
                                          : client.decrypt_ct(chunk));
  }
  path.reserve(params.tree.L + 1);
  for (size_t level = 0; level <= params.tree.L; ++level) {
    const tree_superrecord::EncodedSuperRecord encoded =
        decode_level_planes(response, plaintexts, params, level);
    std::vector<EntryBits> decoded =
        tree_superrecord::decode_super_record(encoded, params.codec);
    require(decoded.size() == 1, "decoded bundle arity is not one");
    path.push_back(std::move(decoded.front()));
  }
  return path;
}

}  // namespace tree_multiplane
