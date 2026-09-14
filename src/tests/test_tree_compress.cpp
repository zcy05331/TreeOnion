#include "tests.h"
#include "merkle_benchmark.h"
#include "tree_compress_bench.h"
#include "tree_compress.h"
#include "tree_index.h"
#include "tree_profile_json.h"
#include "tree_query.h"
#include "tree_response.h"
#include "tree_select.h"

#include <array>
#include <bit>
#include <chrono>
#include <map>
#include <numeric>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

template <typename Fn>
bool throws_invalid_argument(Fn &&fn) {
  try {
    fn();
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

std::array<uint8_t, 32> assemble_node(const std::vector<uint64_t> &chunks,
                                      size_t width) {
  std::array<uint8_t, 32> out{};
  for (size_t c = 0; c < chunks.size(); ++c) {
    for (size_t bit = 0; bit < width; ++bit) {
      const size_t position = width * c + bit;
      if (position >= 256) break;
      if ((chunks[c] >> bit) & 1U) {
        out[position / 8] |= static_cast<uint8_t>(1U << (position % 8));
      }
    }
  }
  return out;
}

template <typename T>
void write_binary(std::ostream &out, const T &value) {
  out.write(reinterpret_cast<const char *>(&value), sizeof(T));
}

size_t write_compressed_response_to_stream(
    const CompressedPathResponse &response, std::ostream &out) {
  size_t bytes = 0;
  const auto write_scalar = [&](const auto &value) {
    write_binary(out, value);
    bytes += sizeof(value);
  };
  write_scalar(response.n2);
  write_scalar(response.modulus);
  write_scalar(response.level_count);
  const size_t offset_count = response.level_offsets.size();
  const size_t c0_count = response.c0.size();
  const size_t c1_count = response.c1.size();
  write_scalar(offset_count);
  write_scalar(c0_count);
  write_scalar(c1_count);
  for (size_t value : response.level_offsets) write_scalar(value);
  for (uint64_t value : response.c0) write_scalar(value);
  for (uint64_t value : response.c1) write_scalar(value);
  return bytes;
}

size_t modeled_ring_switch_key_bytes(const TreeRingSwitchKeys &keys) {
  size_t bytes = sizeof(keys.n2) + sizeof(keys.base_log2) + sizeof(keys.l2);
  for (const auto &component : keys.rows) {
    for (const auto &row : component) {
      bytes += (row.first.size() + row.second.size()) * sizeof(uint64_t);
    }
  }
  return bytes;
}

void require_small_ring_product_identities() {
  // ---- Small-ring product identities (negacyclic wrap and linearity) ----
  {
    const uint64_t q = 97;
    std::vector<uint64_t> a(8, 0), b(8, 0);
    a[7] = 3;  // 3 Y^7
    b[1] = 5;  // 5 Y
    const auto product = small_ring_mul(a, b, q);
    require_test(product[0] == q - 15, "Y^8 = -1 wrap with sign");
    for (size_t i = 1; i < 8; ++i) {
      require_test(product[i] == 0, "single-term product stays a monomial");
    }
    std::vector<uint64_t> u{1, 2, 3, 4, 0, 0, 0, 0};
    std::vector<uint64_t> v{7, 0, 0, 0, 0, 0, 0, 9};
    // (u * v)[3] = u3*v0 - u4*v7-style wrap checked against direct sums.
    const auto uv = small_ring_mul(u, v, q);
    uint64_t direct = (4 * 7) % q;  // u3 v0
    direct = (direct + q - (9 * 1) % q) % q;  // u4.. wrap terms: u_i v_j,
    // i + j = 11 -> -u_i v_j at Y^3: (i, j) = (4..) none nonzero except
    // u_{i} with i + 7 = 11 -> i = 4 (zero) — recompute directly instead:
    uint64_t expect = 0;
    for (size_t i = 0; i < 8; ++i) {
      for (size_t j = 0; j < 8; ++j) {
        const uint64_t term = (u[i] * v[j]) % q;
        if (term == 0) continue;
        if (i + j == 3) expect = (expect + term) % q;
        if (i + j == 11) expect = (expect + q - term) % q;
      }
    }
    require_test(uv[3] == expect, "schoolbook matches the direct convolution");
  }
}

void require_tree_compress_input_rejections(size_t N, const TreeRingSwitchBundle &ring, const PirParams &scheme) {
  // Malformed inputs are rejected before any arithmetic: an odd payload
  // offset, and switch keys whose small ring is not n / 2.
  {
    RlweCt dummy;
    dummy.c0.assign(N, 0);
    dummy.c1.assign(N, 0);
    require_test(throws_invalid_argument([&] {
                   (void)compress_path_response(dummy, {1}, 1, ring.keys,
                                                scheme);
                 }),
                 "accepted an odd payload offset");
    TreeRingSwitchKeys wrong_ring = ring.keys;
    wrong_ring.n2 = N / 4;
    require_test(throws_invalid_argument([&] {
                   (void)compress_path_response(dummy, {0}, 1, wrong_ring,
                                                scheme);
                 }),
                 "accepted switch keys for the wrong ring size");
  }
}

void require_compressed_full_pipeline_matches_plain(
    size_t N, PirClient &client, const SharedPirSessionKeys &keys,
    const TreeRingSwitchBundle &ring, const PirParams &scheme, uint64_t t) {
  // ---- Full pipeline over a real-hash shape and the scalar g = 1 shape ----
  const TreeNodeChunkSource hash_source = [&](size_t level, size_t index,
                                              size_t chunk) {
    return synthetic_tree_node_bytes_chunk(level, index, chunk, t);
  };
  const size_t width = std::bit_width(t) - 1;

  {
    const TreePirParams tree =
        make_tree_pir_params_for_scheme(tree_height_for(3, 5, 32), 3, 32, scheme);
    const PirParams qparams = tree_query_expansion_params(tree, scheme);
    PirServer server(qparams);
    server.set_client_session_keys(client.get_client_id(), keys);
    const PreprocessedTree db = preprocess_tree_mvp(tree, hash_source, scheme);

    for (size_t leaf : {size_t{0}, tree.N - 1,
                        static_cast<size_t>(0x2BADBEEFULL % tree.N)}) {
      RlweCt query = make_tree_query(client, scheme, tree, leaf);
      ExpandedTreeQuery unpacked = unpack_tree_query(
          server, scheme, tree, client.get_client_id(), query);
      CompressedPathResponse compressed = answer_path_compressed(
          db, unpacked, server, client.get_client_id(), tree, scheme,
          ring.keys);
      require_test(compressed.n2 == N / 2,
                   "compressed response lives in the half ring");
      if (leaf == 0) {
        // decode_compressed_path refuses a secret from another ring, a
        // response without its offset map, and an odd record stride.
        TreeRingSwitchSecret other_ring = ring.secret;
        other_ring.n2 = N / 4;
        require_test(throws_invalid_argument([&] {
                       (void)decode_compressed_path(compressed, other_ring,
                                                    t, tree.g, tree.rho);
                     }),
                     "decoded with a secret for another ring");
        CompressedPathResponse unmapped = compressed;
        unmapped.level_offsets.pop_back();
        require_test(throws_invalid_argument([&] {
                       (void)decode_compressed_path(unmapped, ring.secret,
                                                    t, tree.g, tree.rho);
                     }),
                     "decoded a response without its offset map");
        require_test(throws_invalid_argument([&] {
                       (void)decode_compressed_path(
                           compressed, ring.secret, t, tree.g, tree.rho + 1);
                     }),
                     "decoded with an odd record stride");
      }
      const std::vector<std::vector<uint64_t>> path = decode_compressed_path(
          compressed, ring.secret, t, tree.g, tree.rho);
      require_test(path.size() == tree.L + 1, "compressed path length");
      for (size_t level = 0; level <= tree.L; ++level) {
        const size_t node = leaf >> (tree.L - level);
        require_test(assemble_node(path[level], width) ==
                         synthetic_tree_node_bytes(level, node),
                     "compressed path must decode the exact 32-byte node");
      }

      // The compressed decode equals the uncompressed extraction.
      ExpandedTreeQuery second = unpack_tree_query(
          server, scheme, tree, client.get_client_id(), query);
      TreePathResponse plain = answer_path_mvp(
          db, second, server, client.get_client_id(), tree, scheme);
      require_test(extract_path_chunks_mvp(plain, client, tree) == path,
                   "compressed and plain responses must decode identically");
    }
    const size_t small_q_bits = std::bit_width(scheme.get_small_q() - 1);
    BENCH_PRINT("response bytes: plain "
                << 2 * N * small_q_bits / 8 << " -> compressed "
                << 2 * (N / 2) * small_q_bits / 8 << " (payload "
                << (tree.L + 1) * 32 << " B)");
  }
}

void require_even_aligned_rejection_keeps_mvp_path(
    size_t N, PirClient &client, const SharedPirSessionKeys &keys,
    const TreeRingSwitchBundle &ring, const PirParams &scheme, uint64_t t) {
  const TreeNodeChunkSource hash_source = [&](size_t level, size_t index,
                                              size_t chunk) {
    return synthetic_tree_node_bytes_chunk(level, index, chunk, t);
  };
  const size_t width = std::bit_width(t) - 1;
  // The even-aligned packer needs 2L < rho: with g = n/16 (rho = 16) and
  // L = 8 the offsets would reach 2L = 16 = rho, so the compressed path must
  // be refused before any homomorphic work (the MVP packer still serves it).
  {
    const TreePirParams tree =
        make_tree_pir_params_for_scheme(8, 1, N / 16, scheme);
    require_test(2 * tree.L >= tree.rho, "fixture must violate 2L < rho");
    const PirParams qparams = tree_query_expansion_params(tree, scheme);
    PirServer server(qparams);
    server.set_client_session_keys(client.get_client_id(), keys);
    const PreprocessedTree db = preprocess_tree_mvp(tree, hash_source, scheme);
    RlweCt query = make_tree_query(client, scheme, tree, tree.N - 1);
    ExpandedTreeQuery unpacked = unpack_tree_query(
        server, scheme, tree, client.get_client_id(), query);
    require_test(throws_invalid_argument([&] {
                   (void)answer_path_compressed(db, unpacked, server,
                                                client.get_client_id(), tree,
                                                scheme, ring.keys);
                 }),
                 "accepted an even-aligned packing with 2L >= rho");
    ExpandedTreeQuery again = unpack_tree_query(
        server, scheme, tree, client.get_client_id(), query);
    const TreePathResponse plain = answer_path_mvp(
        db, again, server, client.get_client_id(), tree, scheme);
    const auto path = extract_path_chunks_mvp(plain, client, tree);
    for (size_t level = 0; level <= tree.L; ++level) {
      const size_t node = (tree.N - 1) >> (tree.L - level);
      require_test(assemble_node(path[level], width) ==
                       synthetic_tree_node_bytes(level, node),
                   "MVP packer must still serve the shape compression rejects");
    }
  }
}

void require_last_even_aligned_boundary(
    size_t N, PirClient &client, const SharedPirSessionKeys &keys,
    const TreeRingSwitchBundle &ring, const PirParams &scheme, uint64_t t) {
  const TreeNodeChunkSource hash_source = [&](size_t level, size_t index,
                                              size_t chunk) {
    return synthetic_tree_node_bytes_chunk(level, index, chunk, t);
  };
  // The last legal even-aligned packing, 2L = rho - 2: with g = n/16
  // (rho = 16) and L = 7 the leaf level lands at big-ring offset 14 (small-
  // ring offset 7 = rho/2 - 1) and its chunks at 7 + 8j must not collide
  // with any other level's. The shape above tests the rejection at 2L = rho;
  // this one catches a guard tightened to 2L >= rho - 2 or a packer that
  // shifts by 2z + 2, which would still pass the 2L = 28 vs rho = 64 fixture.
  {
    const TreePirParams tree =
        make_tree_pir_params_for_scheme(7, 3, N / 16, scheme);
    require_test(2 * tree.L + 2 == tree.rho,
                 "fixture must sit at 2L = rho - 2");
    const PirParams qparams = tree_query_expansion_params(tree, scheme);
    PirServer server(qparams);
    server.set_client_session_keys(client.get_client_id(), keys);
    const PreprocessedTree db = preprocess_tree_mvp(tree, hash_source, scheme);
    for (size_t leaf : {size_t{0}, tree.N - 1,
                        static_cast<size_t>(0x2BADBEEFULL % tree.N)}) {
      RlweCt query = make_tree_query(client, scheme, tree, leaf);
      ExpandedTreeQuery unpacked = unpack_tree_query(
          server, scheme, tree, client.get_client_id(), query);
      CompressedPathResponse compressed;
      try {
        compressed = answer_path_compressed(db, unpacked, server,
                                            client.get_client_id(), tree,
                                            scheme, ring.keys);
      } catch (const std::invalid_argument &error) {
        require_test(false,
                     std::string("refused the last legal even-aligned "
                                 "packing 2L = rho - 2: ") +
                         error.what());
      }
      for (size_t level = 0; level <= tree.L; ++level) {
        require_test(compressed.level_offsets[level] == level,
                     "even-aligned level z must map to small-ring offset z");
      }
      const std::vector<std::vector<uint64_t>> path = decode_compressed_path(
          compressed, ring.secret, t, tree.g, tree.rho);
      require_test(path.size() == tree.L + 1,
                   "boundary compressed path length");
      for (size_t level = 0; level <= tree.L; ++level) {
        const size_t node = leaf >> (tree.L - level);
        for (size_t j = 0; j < tree.g; ++j) {
          require_test(path[level][j] == hash_source(level, node, j),
                       "boundary compressed path chunk mismatch");
        }
      }
      ExpandedTreeQuery again = unpack_tree_query(
          server, scheme, tree, client.get_client_id(), query);
      const TreePathResponse plain = answer_path_mvp(
          db, again, server, client.get_client_id(), tree, scheme);
      require_test(extract_path_chunks_mvp(plain, client, tree) == path,
                   "boundary compressed and plain responses must decode "
                   "identically");
    }
  }
}

void require_scalar_compressed_pipeline(PirClient &client, const SharedPirSessionKeys &keys, const TreeRingSwitchBundle &ring, const PirParams &scheme, uint64_t t) {
  // g = 1 scalar shape keeps working through the same pipeline.
  {
    const TreePirParams tree = make_tree_pir_params_for_scheme(tree_height_for(2, 0), 2, scheme);
    const PirParams qparams = tree_query_expansion_params(tree, scheme);
    PirServer server(qparams);
    server.set_client_session_keys(client.get_client_id(), keys);
    const TreeNodeSource source = [&](size_t level, size_t index) {
      return synthetic_tree_node_value(level, index, t);
    };
    const PreprocessedTree db = preprocess_tree_mvp(tree, source, scheme);
    const size_t leaf = tree.N - 1;
    RlweCt query = make_tree_query(client, scheme, tree, leaf);
    ExpandedTreeQuery unpacked = unpack_tree_query(
        server, scheme, tree, client.get_client_id(), query);
    CompressedPathResponse compressed = answer_path_compressed(
        db, unpacked, server, client.get_client_id(), tree, scheme,
        ring.keys);
    const std::vector<std::vector<uint64_t>> path = decode_compressed_path(
        compressed, ring.secret, t, tree.g, tree.rho);
    for (size_t level = 0; level <= tree.L; ++level) {
      require_test(path[level][0] ==
                       synthetic_tree_node_value(
                           level, leaf >> (tree.L - level), t),
                   "compressed scalar path value");
    }
  }
}


}  // namespace

void run_tree_compress_benchmark(PirTest &test) {
  print_func_name("test_tree_compress_benchmark");
  constexpr size_t N = DBConsts::PolyDegree;
  using Clock = std::chrono::steady_clock;
  const auto ms_since = [](Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
        .count();
  };

  PirParams scheme;
  require_applicable(
      scheme.K() == 1,
      "compressed response benchmark requires a single-limb (K = 1) scheme");
  const uint64_t t = scheme.get_plain_mod();
  const size_t chunk_bits = std::bit_width(t) - 1;
  const size_t g = std::bit_ceil((size_t{255} / chunk_bits) + 1);
  const size_t L = test.bench_tree_height(24);
  const size_t key_height = scheme.get_expan_height();
  size_t chosen_a = 2;
  for (size_t a = 20; a >= 2; --a) {
    try {
      (void)make_tree_pir_params_for_scheme(L, a, g, scheme, key_height);
      chosen_a = a;
      break;
    } catch (const std::invalid_argument &) {
    }
  }
  const TreePirParams tree =
      make_tree_pir_params_for_scheme(L, chosen_a, g, scheme, key_height);
  require_applicable(2 * tree.L < tree.rho,
                     "compressed benchmark requires even-aligned 2L < rho");
  const TreeNodeChunkSource source = [&](size_t level, size_t index,
                                         size_t chunk) {
    return synthetic_tree_node_bytes_chunk(level, index, chunk, t);
  };

  PirClient client(scheme);
  SharedPirSessionKeys keys =
      client.create_session_keys(std::bit_width(N) - 1);
  TreeRingSwitchBundle ring = client.create_ring_switch_bundle(N / 2);
  const PirParams qparams = tree_query_expansion_params(tree, scheme);
  PirServer server(qparams);
  server.set_client_session_keys(client.get_client_id(), keys);

  const auto setup_start = Clock::now();
  const PreprocessedTree db = preprocess_tree_mvp(tree, source, scheme);
  const double setup_ms = ms_since(setup_start);

  const size_t kWarmups = test.bench_warmups.value_or(3);
  const size_t kTrials = test.bench_measured_trials.value_or(100);
  const uint64_t trial_seed =
      test.bench_trial_seed.value_or(0x7472656543376d70ULL);
  const BenchmarkTrialPlan plan =
      make_benchmark_trial_plan(tree.N, kWarmups, kTrials, trial_seed);

  std::vector<double> query_ms, unpack_ms, path_ms, decode_ms;
  size_t actual_response_bytes = 0;
  std::vector<tree_profile_json::Sample> json_samples;
  CLEAN_TIMER();
  const auto run_trial = [&](size_t leaf, bool measured, size_t trial) {
    auto start = Clock::now();
    RlweCt query = make_tree_query(client, scheme, tree, leaf);
    const double q_ms = ms_since(start);

    TreeQueryUnpackTiming unpack_timing;
    start = Clock::now();
    ExpandedTreeQuery unpacked = unpack_tree_query(
        server, scheme, tree, client.get_client_id(), query, &unpack_timing);
    const double u_ms = ms_since(start);

    start = Clock::now();
    CompressedPathResponse response = answer_path_compressed(
        db, unpacked, server, client.get_client_id(), tree, scheme, ring.keys);
    const double p_ms = ms_since(start);

    start = Clock::now();
    std::ostringstream wire;
    const size_t response_bytes =
        write_compressed_response_to_stream(response, wire);
    const double response_serialize_ms = ms_since(start);

    start = Clock::now();
    const std::vector<std::vector<uint64_t>> path = decode_compressed_path(
        response, ring.secret, t, tree.g, tree.rho);
    const double d_ms = ms_since(start);
    END_EXPERIMENT();

    std::map<std::string, double> sample_stages;
    if (measured) {
      sample_stages["query_expand"] =
          tree_profile_json::ns_to_ms(unpack_timing.query_expand);
      sample_stages["selector_completion"] =
          tree_profile_json::ns_to_ms(unpack_timing.selector_completion);
      for (size_t i = 0; i < tree_profile_json::kScanTimerSections.size();
           ++i) {
        sample_stages[tree_profile_json::kScanStageLabels[i]] =
            GET_LAST_TIME(tree_profile_json::kScanTimerSections[i]);
      }
      for (size_t i = 0; i < tree_profile_json::kPathTimerSections.size();
           ++i) {
        sample_stages[tree_profile_json::kPathStageLabels[i]] =
            GET_LAST_TIME(tree_profile_json::kPathTimerSections[i]);
      }
    }

    require_test(path.size() == tree.L + 1,
                 "compressed benchmark path length");
    for (size_t level = 0; level <= tree.L; ++level) {
      const size_t node = leaf >> (tree.L - level);
      for (size_t j = 0; j < tree.g; ++j) {
        require_test(path[level][j] == source(level, node, j),
                     "compressed benchmark path chunk mismatch");
      }
    }
    if (measured) {
      query_ms.push_back(q_ms);
      unpack_ms.push_back(u_ms);
      path_ms.push_back(p_ms);
      decode_ms.push_back(d_ms);
      actual_response_bytes = response_bytes;
      tree_profile_json::Sample sample;
      sample.trial = trial;
      sample.leaf = leaf;
      sample.client_query_ms = q_ms;
      sample.server_total_ms =
          tree_profile_json::query_unpack_ms(unpack_timing) + p_ms;
      sample.stages_ms = std::move(sample_stages);
      sample.diagnostics_ms["scan_combined"] = GET_LAST_TIME(TREE_SCAN_TIME);
      sample.diagnostics_ms["response_ring_switch_key_switch"] =
          GET_LAST_TIME(TREE_COMPRESS_KEY_SWITCH_TIME);
      sample.diagnostics_ms["response_ring_switch_modswitch"] =
          GET_LAST_TIME(TREE_COMPRESS_MOD_SWITCH_TIME);
      sample.response_serialize_ms = response_serialize_ms;
      sample.actual_response_bytes = response_bytes;
      sample.client_decode_ms = d_ms;
      sample.correct = true;
      json_samples.push_back(std::move(sample));
    }
  };

  size_t trial = 0;
  for (size_t leaf : plan.warmup_leaf_indices) run_trial(leaf, false, trial++);
  trial = 0;
  for (size_t leaf : plan.measured_leaf_indices) run_trial(leaf, true, trial++);

  const auto avg = [](const std::vector<double> &v) {
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
  };
  BENCH_PRINT("tree compressed 32-byte-node benchmark: N=2^"
              << tree.L << " leaves, g=" << tree.g << ", N0=" << tree.N0
              << ", b=" << tree.b << ", rho=" << tree.rho
              << ", h_q=" << tree.h_q << ", setup " << setup_ms << " ms");
  BENCH_PRINT("trials: " << kTrials << " measured after " << kWarmups
              << " warmup");
  BENCH_PRINT("client query avg " << avg(query_ms) << " ms");
  BENCH_PRINT("server unpack (expand+convert) avg " << avg(unpack_ms)
              << " ms");
  BENCH_PRINT("server compressed path avg " << avg(path_ms) << " ms");
  BENCH_PRINT("server total avg " << avg(unpack_ms) + avg(path_ms)
              << " ms");
  BENCH_PRINT("client compressed decode avg " << avg(decode_ms) << " ms");
  BENCH_PRINT("communication: compressed response "
              << actual_response_bytes << " B (struct wire model)");
  const size_t query_bytes = scheme.get_BFV_size(true);
  const size_t helper_bytes =
      scheme.with_query_shape({1, 0, std::bit_width(N) - 1})
          .get_bv_galois_key_size(true) +
      scheme.get_gsw_key_size(true);
  const size_t compression_key_bytes =
      modeled_ring_switch_key_bytes(ring.keys);

  tree_profile_json::write_report(
      tree_profile_json::output_path_from_env(), "legacy-tree-g32-compressed",
      tree.N, 256, (2 * tree.N - 1) * 32, kWarmups, kTrials, trial_seed,
      setup_ms, query_bytes, helper_bytes, compression_key_bytes, false,
      "compressed", json_samples,
      {{"hardware_target", "CloudLab Xeon 6142 AVX-512 376GiB RAM"},
       {"payload_note", "compressed path benchmark carries full 32-byte nodes"},
       {"chunk_bits", std::to_string(chunk_bits)},
       {"g", std::to_string(tree.g)},
       {"response_bytes_model", std::to_string(actual_response_bytes)}});
}

// Milestone-7 gate: the d = 2 ring switch compresses the aligned path
// response to one R_{n/2} ciphertext that still decodes the exact path.
void PirTest::test_tree_compress() {
  print_func_name(__FUNCTION__);
  constexpr size_t N = DBConsts::PolyDegree;

  require_small_ring_product_identities();


  PirParams scheme;
  // The d = 2 ring switch is defined for the single-limb scheme only; a
  // multi-limb build cannot verify this gate, so the test is reported as
  // not applicable rather than as a pass.
  require_applicable(
      scheme.K() == 1,
      "ring-switch gate requires a single-limb (K = 1) scheme build");
  PirClient client(scheme);
  const uint64_t t = scheme.get_plain_mod();
  SharedPirSessionKeys keys =
      client.create_session_keys(std::bit_width(N) - 1);
  TreeRingSwitchBundle ring = client.create_ring_switch_bundle(N / 2);

  require_tree_compress_input_rejections(N, ring, scheme);


  require_compressed_full_pipeline_matches_plain(
      N, client, keys, ring, scheme, t);


  require_even_aligned_rejection_keeps_mvp_path(
      N, client, keys, ring, scheme, t);


  require_last_even_aligned_boundary(N, client, keys, ring, scheme, t);


  require_scalar_compressed_pipeline(client, keys, ring, scheme, t);

}
