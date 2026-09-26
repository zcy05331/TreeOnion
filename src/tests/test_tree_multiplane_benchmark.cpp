#include "tests.h"
#include "merkle_benchmark.h"
#include "tree_multiplane.h"
#include "tree_profile_json.h"
#include "tree_packing.h"
#include "tree_entry_test_util.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <numeric>
#include <string_view>

namespace {

// Per-coefficient decryption-error statistics in small-q units.
struct NoiseStats {
  uint64_t count = 0;
  double sum = 0.0, sumsq = 0.0;
  int64_t max_abs = 0;
  std::vector<uint64_t> abs_hist = std::vector<uint64_t>(1024, 0);
  uint64_t overflow = 0;
  void add(int64_t e) {
    ++count;
    sum += static_cast<double>(e);
    sumsq += static_cast<double>(e) * static_cast<double>(e);
    const int64_t a = e < 0 ? -e : e;
    max_abs = std::max(max_abs, a);
    if (a < static_cast<int64_t>(abs_hist.size())) ++abs_hist[static_cast<size_t>(a)];
    else ++overflow;
  }
  void write(std::ostream &out) const {
    out << "{\"count\": " << count << ", \"sum\": " << sum
        << ", \"sumsq\": " << sumsq << ", \"max_abs\": " << max_abs
        << ", \"overflow\": " << overflow << ", \"abs_hist\": [";
    for (size_t i = 0; i < abs_hist.size(); ++i) out << (i ? "," : "") << abs_hist[i];
    out << "]}";
  }
};

size_t positive_env(const char *name, size_t fallback) {
  const char *raw = std::getenv(name);
  if (!raw || !*raw) return fallback;
  const std::string_view text(raw);
  size_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value == 0) {
    throw std::invalid_argument(std::string(name) + " must be a positive integer");
  }
  return value;
}

std::string string_env(const char *name, const char *fallback) {
  const char *value = std::getenv(name);
  return value && *value ? value : fallback;
}

using Clock = std::chrono::steady_clock;
double elapsed_ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}  // namespace

// A separately named benchmark leaves the original g32 baseline unchanged.
// One expanded old-layout query serves every level and every plane.
void PirTest::test_tree_multiplane_benchmark() {
  using namespace tree_multiplane;
  using tree_packing::ResponsePacking;
  print_func_name(__FUNCTION__);
  PirParams scheme;
  require_applicable(scheme.get_composite_rns().enabled,
                     "multiplane benchmark needs CONFIG_N2048_K1_COMP");
  const size_t n = scheme.get_poly_degree();
  const size_t key_height = std::bit_width(n) - 1;
  const size_t L = bench_tree_height(22);
  const size_t entry_bits = positive_env("TREE_MULTIPLANE_ENTRY_BITS", 256);
  const size_t payload_bits = tree_entry_test::payload_bits(scheme);
  const auto layout = tree_entry_codec::choose_layout(entry_bits, payload_bits, n);
  // TREE_MULTIPLANE_G may leave the layout policy for the plane-geometry
  // ablation. The JSON records the policy shape beside the measured one, and
  // the formal runners reject rows whose shape is not the policy's.
  const char *env_g = std::getenv("TREE_MULTIPLANE_G");
  const size_t g = env_g && *env_g ? positive_env("TREE_MULTIPLANE_G", layout.g)
                                   : layout.g;
  require_test(std::has_single_bit(g) && n / g >= 2,
               "TREE_MULTIPLANE_G must be a power of two with rho = n/g >= 2");
  const size_t leaf_count = size_t{1} << L;
  const size_t N0 = positive_env("TREE_MULTIPLANE_N0",
                                 std::min(size_t{512}, leaf_count / (n / g)));
  require_test(std::has_single_bit(N0) && N0 >= 2,
               "TREE_MULTIPLANE_N0 must be a power of two >= 2");
  const auto params = make_params(make_tree_pir_params_for_scheme(
      L, std::bit_width(N0) - 1, g, scheme, key_height), entry_bits);
  const auto &tree = params.tree;
  const size_t planes = params.codec.plane_count;
  const size_t items = (L + 1) * planes;
  const auto bounds = tree_packing::path_chunk_bounds(items, tree.rho);
  const std::string packing_name = string_env("TREE_MULTIPLANE_PACKING", "mlwe");
  require_test(packing_name == "mlwe" || packing_name == "projection",
               "TREE_MULTIPLANE_PACKING must be mlwe or projection");
  const auto packing = packing_name == "mlwe" ? ResponsePacking::MlweMerge
                                               : ResponsePacking::Projection;
  const std::string layout_name =
      string_env("TREE_MULTIPLANE_QUERY_LAYOUT", "interleaved");
  require_test(layout_name == "interleaved" || layout_name == "split-columns",
               "TREE_MULTIPLANE_QUERY_LAYOUT must be interleaved or split-columns");
  const auto query_layout = layout_name == "interleaved"
                                ? Matrix32QueryLayout::Interleaved
                                : Matrix32QueryLayout::SplitColumns;
  const size_t warmups = bench_warmups.value_or(3);
  const size_t trials = bench_measured_trials.value_or(16);
  const uint64_t seed = bench_trial_seed.value_or(7);
  require_test(trials > 0, "multiplane benchmark needs measured trials");
  const auto schedule = make_benchmark_trial_plan(leaf_count, warmups, trials, seed);
  // Noise diagnostics may replace the synthetic nodes by all-one entries,
  // which give every digit its largest value 2^c - 1.
  const std::string entry_fill = string_env("TREE_MULTIPLANE_ENTRY_FILL", "synthetic");
  require_test(entry_fill == "synthetic" || entry_fill == "ones",
               "TREE_MULTIPLANE_ENTRY_FILL must be synthetic or ones");
  const std::vector<uint8_t> ones((entry_bits + 7) / 8, 0xFF);
  const EntrySource source = [&](size_t level, size_t node) {
    if (entry_fill == "ones") return tree_entry_codec::entry_from_bytes(ones, entry_bits);
    if (entry_bits == 256) {
      return tree_entry_codec::entry_from_bytes(
          synthetic_tree_node_bytes(level, node), entry_bits);
    }
    return tree_entry_test::synthetic_entry(entry_bits, level, node);
  };

  auto start = Clock::now();
  PirClient client(scheme);
  auto keys = client.create_session_keys(key_height);
  PirServer server(tree_query_expansion_params(tree, scheme));
  server.set_client_session_keys(client.get_client_id(), keys);
  const double session_setup_ms = elapsed_ms(start);
  start = Clock::now();
  const auto db = preprocess(params, source, scheme);
  const double setup_ms = elapsed_ms(start);

  size_t plaintexts = 0, fold_cmux = 0, rotate_cmux = 0, packing_keyswitches = 0;
  for (const auto &plan : build_level_plans(tree)) {
    plaintexts += planes * plan.R;
    fold_cmux += planes * ((size_t{1} << plan.beta_count) - 1);
    rotate_cmux += planes * plan.gamma_count;
    if (packing == ResponsePacking::Projection)
      packing_keyswitches += planes * plan.projection_depth;
  }
  if (packing == ResponsePacking::MlweMerge) {
    for (const auto &[first, count] : bounds)
      packing_keyswitches +=
          tree_packing::mlwe_merge_keyswitch_count(count, tree.rho);
  }

  uint64_t max_noise = 0;
  int min_budget = std::numeric_limits<int>::max();
  const char *noise_stats_path = std::getenv("TREE_MULTIPLANE_NOISE_STATS");
  const bool collect_noise = noise_stats_path && *noise_stats_path;
  NoiseStats payload_noise, other_noise;
  // Per level-plane item moments: count, sum, sum of squares, max |error|.
  std::vector<std::array<double, 4>> item_noise(items, {0.0, 0.0, 0.0, 0.0});
  // The same moments per (item, digit) coordinate of the response.
  std::vector<std::array<double, 4>> coord_noise(items * g, {0.0, 0.0, 0.0, 0.0});
  std::vector<tree_profile_json::Sample> samples;
  samples.reserve(trials);
  CLEAN_TIMER();
  const auto run_trial = [&](size_t leaf, bool measured, size_t trial) {
    tree_profile_json::Sample sample;
    sample.trial = trial;
    sample.leaf = leaf;
    auto tick = Clock::now();
    auto query = make_tree_query(client, scheme, tree, leaf);
    sample.client_query_ms = elapsed_ms(tick);
    TreeQueryUnpackTiming unpack_timing;
    tick = Clock::now();
    auto expanded = unpack_tree_query(server, scheme, tree, client.get_client_id(),
                                      query, &unpack_timing);
    auto response = answer(db, expanded, server, client.get_client_id(), params,
                           scheme, packing, query_layout);
    // Includes all unpacking/preparation/glue, rather than only named timers.
    sample.server_total_ms = elapsed_ms(tick);
    tick = Clock::now();
    const auto path = extract(response, client, params);
    sample.client_decode_ms = elapsed_ms(tick);
    tick = Clock::now();
    std::stringstream wire;
    for (const auto &chunk : response.chunks)
      sample.actual_response_bytes += server.save_resp_to_stream(chunk, wire);
    sample.response_serialize_ms = elapsed_ms(tick);
    END_EXPERIMENT();

    // Verify every response coefficient, including zero padding and unused
    // residues. Byte-only checks would miss noise failures in unused slots.
    require_test(response.small_q, "multiplane benchmark requires small-q responses");
    require_test(response.chunks.size() == bounds.size() &&
                     response.item_offsets.size() == items,
                 "multiplane response shape mismatch");
    for (size_t chunk = 0; chunk < bounds.size(); ++chunk) {
      uint64_t noise = 0;
      int budget = 0;
      const auto decoded = client.decrypt_mod_q_measured(response.chunks[chunk], noise, budget);
      std::vector<uint64_t> expected(n, 0);
      std::vector<char> payload_slot(n, 0);
      std::vector<int> slot_item(n, -1), slot_digit(n, -1);
      const auto [first, count] = bounds[chunk];
      for (size_t local = 0; local < count; ++local) {
        const size_t item = first + local;
        const size_t level = item / planes;
        const size_t plane = item % planes;
        const auto entry = source(level, leaf >> (L - level));
        const auto digits = tree_entry_codec::encode_entry(entry, params.codec);
        const size_t offset = tree_packing::packed_residue_offset(
            local, tree.rho, packing);
        require_test(response.item_offsets[item] == offset,
                     "multiplane response placement mismatch");
        for (size_t digit = 0; digit < g; ++digit) {
          expected[offset + digit * tree.rho] = digits[plane][digit];
          payload_slot[offset + digit * tree.rho] = 1;
          slot_item[offset + digit * tree.rho] = static_cast<int>(item);
          slot_digit[offset + digit * tree.rho] = static_cast<int>(digit);
        }
      }
      require_test(decoded.data == expected, "multiplane full response plaintext mismatch");
      if (collect_noise) {
        const std::vector<int64_t> errors = client.decrypt_mod_q_errors(response.chunks[chunk]);
        for (size_t i = 0; i < n; ++i) {
          (payload_slot[i] ? payload_noise : other_noise).add(errors[i]);
          if (slot_item[i] >= 0) {
            auto &m = item_noise[static_cast<size_t>(slot_item[i])];
            const double e = static_cast<double>(errors[i]);
            m[0] += 1.0; m[1] += e; m[2] += e * e; m[3] = std::max(m[3], std::abs(e));
            auto &c = coord_noise[static_cast<size_t>(slot_item[i]) * g +
                                  static_cast<size_t>(slot_digit[i])];
            c[0] += 1.0; c[1] += e; c[2] += e * e; c[3] = std::max(c[3], std::abs(e));
          }
        }
      }
      max_noise = std::max(max_noise, noise);
      min_budget = std::min(min_budget, budget);
    }
    require_test(path.size() == L + 1, "multiplane path length mismatch");
    for (size_t level = 0; level <= L; ++level)
      require_test(path[level] == source(level, leaf >> (L - level)),
                   "multiplane entry byte mismatch");
    sample.correct = true;
    if (!measured) return;
    sample.stages_ms["query_expand"] = tree_profile_json::ns_to_ms(unpack_timing.query_expand);
    sample.stages_ms["selector_completion"] = tree_profile_json::ns_to_ms(unpack_timing.selector_completion);
    for (size_t k = 0; k < tree_profile_json::kScanTimerSections.size(); ++k)
      sample.stages_ms[tree_profile_json::kScanStageLabels[k]] =
          GET_LAST_TIME(tree_profile_json::kScanTimerSections[k]);
    for (size_t k = 0; k < tree_profile_json::kPathTimerSections.size(); ++k)
      sample.stages_ms[tree_profile_json::kPathStageLabels[k]] =
          GET_LAST_TIME(tree_profile_json::kPathTimerSections[k]);
    sample.stages_ms["merge"] = GET_LAST_TIME(TREE_MLWE_MERGE_TIME);
    sample.diagnostics_ms["scan_combined"] = GET_LAST_TIME(TREE_SCAN_TIME);
    samples.push_back(std::move(sample));
  };
  for (size_t i = 0; i < warmups; ++i) run_trial(schedule.warmup_leaf_indices[i], false, i);
  for (size_t i = 0; i < trials; ++i) run_trial(schedule.measured_leaf_indices[i], true, i);

  const size_t helper_bytes = scheme.with_query_shape({1, 0, key_height})
                                 .get_bv_galois_key_size(true) + scheme.get_gsw_key_size(true);
  const size_t logical_bytes = (2 * tree.N - 1) * ((entry_bits + 7) / 8);
  const double mean_ms = std::accumulate(samples.begin(), samples.end(), 0.0,
      [](double sum, const auto &s) { return sum + s.server_total_ms; }) / trials;
  tree_profile_json::write_report(
      bench_output_json.value_or(tree_profile_json::output_path_from_env()),
      "tree-multiplane", tree.N,
      entry_bits, logical_bytes, warmups, trials, seed, setup_ms,
      scheme.get_BFV_size(true), helper_bytes, 0, entry_bits != 256, packing_name, samples,
      {{"index_layout", "gamma-alpha-beta"}, {"h", "1"},
      {"g", std::to_string(g)}, {"plane_count", std::to_string(planes)},
       {"layout_policy", "P_le_6_max_util_tie_minP"},
       {"policy_g", std::to_string(layout.g)},
       {"policy_plane_count", std::to_string(layout.plane_count)},
       {"N0", std::to_string(N0)}, {"b", std::to_string(tree.b)},
       {"rho", std::to_string(tree.rho)}, {"ring_degree", std::to_string(n)},
       {"chunk_bits", std::to_string(params.codec.payload_bits_per_coeff)},
       {"plaintext_modulus", std::to_string(scheme.get_plain_mod())},
       {"ciphertext_modulus", std::to_string(scheme.get_rns_mods()[0])},
       {"response_modulus", std::to_string(scheme.get_small_q())},
       {"key_height", std::to_string(key_height)}, {"query_height", std::to_string(tree.h_q)},
       {"ep_rows", std::to_string(scheme.get_l())},
       {"ep_low_bits", std::to_string(scheme.get_ep_low_bits())},
       {"ks_rows", std::to_string(bvks::gadget_rows())},
       {"ks_low_bits", std::to_string(bvks::low_bits_for_scheme(scheme))},
       {"query_layout", layout_name}, {"response_packing", packing_name},
       {"merge_stage_definition", "MLWE residue merge, including normalization and BV key switches"},
       {"fold_cmux", std::to_string(fold_cmux)},
       {"rotate_cmux", std::to_string(rotate_cmux)},
       {"response_keyswitches", std::to_string(packing_keyswitches)},
       {"packed_plaintexts", std::to_string(plaintexts)},
       {"path_entries", std::to_string(L + 1)}, {"response_items", std::to_string(items)},
       {"session_setup_ms", std::to_string(session_setup_ms)},
       {"min_noise_budget_bits", std::to_string(min_budget)},
       {"max_noise", std::to_string(max_noise)},
       {"acceptance_policy", "decryption_correctness_only; noise margins are diagnostic"}});
  if (collect_noise) {
    std::ofstream out(noise_stats_path);
    require_test(static_cast<bool>(out), "cannot open TREE_MULTIPLANE_NOISE_STATS output");
    out << "{\"leaf_count\": " << tree.N << ", \"N0\": " << N0 << ", \"g\": " << g
        << ", \"planes\": " << planes << ", \"warmups\": " << warmups
        << ", \"trials\": " << trials << ", \"trial_seed\": " << seed
        << ", \"small_q\": " << scheme.get_small_q()
        << ", \"plain_mod\": " << scheme.get_plain_mod()
        << ", \"ep_rows\": " << scheme.get_l()
        << ", \"ep_low_bits\": " << scheme.get_ep_low_bits()
        << ", \"entry_fill\": \"" << entry_fill << "\""
        << ", \"max_noise\": " << max_noise << ", \"payload\": ";
    payload_noise.write(out);
    out << ", \"other\": ";
    other_noise.write(out);
    out << ", \"items\": [";
    for (size_t item = 0; item < items; ++item) {
      const auto &m = item_noise[item];
      out << (item ? "," : "") << "{\"level\": " << item / planes << ", \"plane\": "
          << item % planes << ", \"count\": " << m[0] << ", \"sum\": " << m[1]
          << ", \"sumsq\": " << m[2] << ", \"max_abs\": " << m[3] << "}";
    }
    out << "], \"coords\": [";
    for (size_t idx = 0; idx < items * g; ++idx) {
      const auto &c = coord_noise[idx];
      const size_t item = idx / g;
      out << (idx ? "," : "") << "{\"level\": " << item / planes << ", \"plane\": "
          << item % planes << ", \"digit\": " << idx % g << ", \"count\": " << c[0]
          << ", \"sum\": " << c[1] << ", \"sumsq\": " << c[2]
          << ", \"max_abs\": " << c[3] << "}";
    }
    out << "]}\n";
  }
  BENCH_PRINT("old gamma/alpha/beta h1: g=" << g << " P=" << planes << " N0=" << N0
              << " b=" << tree.b << " server=" << mean_ms << " ms; "
              << (static_cast<double>(logical_bytes) / (1 << 20)) * 1000 / mean_ms
              << " MiB/s; response=" << samples.back().actual_response_bytes << " B");
}
