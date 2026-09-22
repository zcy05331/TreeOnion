// KsPIR (CCS'24, mmingluo/kspir) behind the PBC cuckoo framework of the OnionPIR
// cuckoo baseline: same root-excluded Merkle records, 3-hash buckets, bucket
// positions and seed-7 leaves; only the per-bucket PIR backend differs. Every
// bucket uses the same packing number r and BSGS split, so one set of BSGS and
// packing keys serves all buckets. The online path calls the upstream
// functions exactly as tests/test-pir.cpp does.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "pir.h"
#include "crt.h"
#include "pbc.hpp"

namespace {

constexpr size_t kRecordBytes = 32;
constexpr size_t kItemBytes = N * 2;                             // 4096 16-bit values = 8 KB
constexpr size_t kRecordsPerItem = kItemBytes / kRecordBytes;    // 256
constexpr size_t kColumns = N / 2;                               // items per packing instance
constexpr size_t kMatrixWords = static_cast<size_t>(N) * N / 2;  // datacrt words per instance
const char *kUpstreamRepo = "https://github.com/mmingluo/kspir";

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Args {
  size_t height = 0, warmups = 3, trials = 8;
  uint64_t seed = 7;
  int32_t n1 = 128;
  size_t min_r = 1;  // test only: force packing with small buckets
  std::string output, fixture, compiler_label;
  bool self_test = false;
};

Args parse(int argc, char **argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument(k + " needs a value");
      return argv[++i];
    };
    if (k == "--height") a.height = std::stoul(val());
    else if (k == "--warmup") a.warmups = std::stoul(val());
    else if (k == "--trials") a.trials = std::stoul(val());
    else if (k == "--trial-seed") a.seed = std::stoull(val());
    else if (k == "--n1") a.n1 = std::stoi(val());
    else if (k == "--min-r") a.min_r = std::stoul(val());
    else if (k == "--output") a.output = val();
    else if (k == "--fixture") a.fixture = val();
    else if (k == "--compiler-label") a.compiler_label = val();
    else if (k == "--self-test") a.self_test = true;
    else throw std::invalid_argument("unknown argument " + k);
  }
  return a;
}

uint64_t bits_of(uint64_t modulus) { return 64 - static_cast<uint64_t>(__builtin_clzll(modulus - 1)); }
uint64_t poly_bytes(uint64_t modulus) { return (static_cast<uint64_t>(N) * bits_of(modulus) + 7) / 8; }

// Wire sizes: "full" sends both polynomials; "seeded" replaces the uniform mask
// with a 32-byte PRG seed, the convention the KsPIR paper reports.
struct Bytes {
  uint64_t full = 0, seeded = 0;
  Bytes &operator+=(const Bytes &o) { full += o.full; seeded += o.seeded; return *this; }
};
Bytes rlwe_bytes(const RlweCiphertext &ct) {
  const uint64_t p = poly_bytes(ct.getModulus());
  return {2 * p, p + 32};
}
Bytes rgsw_bytes(const RGSWCiphertext &ct) {
  const uint64_t p = poly_bytes(ct.getModulus());
  return {(ct.a.size() + ct.b.size()) * p, ct.b.size() * p + 32 * ct.a.size()};
}
template <typename KeyMap> Bytes key_bytes(const KeyMap &key_map) {
  Bytes total;
  for (const auto &entry : key_map)
    for (const auto &ct : entry.second) total += rlwe_bytes(ct);
  return total;
}

// upstream inverse_encode with the NTT object built once instead of per call
struct Encoder {
  intel::hexl::NTT ntt;
  std::vector<int32_t> decode;
  std::vector<uint64_t> tmp;
  Encoder() : ntt(N, bsgsp), decode(N, 0), tmp(N, 0) { compute_query_decode(decode, N); }
  void apply(std::vector<uint64_t> &m) {
    ntt.ComputeForward(m.data(), m.data(), 1, 1);
    std::copy(m.begin(), m.end(), tmp.begin());
    for (size_t j = 0; j < static_cast<size_t>(N); ++j) m[decode[j]] = tmp[j];
  }
};

// Record p of a bucket lives in item x = p / 256 at byte offset (p % 256) * 32;
// item x is KsPIR record (column c = x % 2048, packing t = x / 2048) and its
// 16-bit value m sits in instance k = m % r, row r * (m / r) + t, column c.
void build_bucket(uint64_t *datacrt, const std::vector<uint32_t> &members, size_t r, int32_t n1, Encoder &enc,
                  std::vector<std::vector<uint64_t>> &data, std::vector<std::vector<uint64_t>> &data_ntt,
                  std::vector<uint8_t> &items) {
  items.assign(r * kColumns * kItemBytes, 0);
  for (size_t p = 0; p < members.size(); ++p) {
    const auto node_idx = pbc::node_of_ordinal(members[p]);
    const auto node = pbc::merkle_node(node_idx.first, node_idx.second);
    std::memcpy(&items[(p / kRecordsPerItem) * kItemBytes + (p % kRecordsPerItem) * kRecordBytes], node.data(),
                kRecordBytes);
  }
  std::vector<uint64_t> column(N);
  for (size_t k = 0; k < r; ++k) {
    for (size_t row = 0; row < static_cast<size_t>(N); ++row) {
      const size_t t = row % r, m = r * (row / r) + k;
      for (size_t c = 0; c < kColumns; ++c) {
        const uint8_t *src = &items[(t * kColumns + c) * kItemBytes + 2 * m];
        data[row][c] = static_cast<uint64_t>(src[0]) | (static_cast<uint64_t>(src[1]) << 8);
      }
    }
    for (size_t c = 0; c < kColumns; ++c) {
      for (size_t row = 0; row < static_cast<size_t>(N); ++row) column[row] = data[row][c];
      enc.apply(column);
      for (size_t row = 0; row < static_cast<size_t>(N); ++row) data[row][c] = column[row];
    }
    database_tobsgsntt(data_ntt, data, crtMod, n1);
    database_tocrt(datacrt + kMatrixWords * k, data_ntt, n1);
  }
}

std::vector<size_t> read_int_array(const std::string &path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open fixture " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string s = ss.str();
  for (char &ch : s)
    if (ch == '[' || ch == ']' || ch == ',') ch = ' ';
  std::stringstream nums(s);
  std::vector<size_t> out;
  size_t v;
  while (nums >> v) out.push_back(v);
  return out;
}

int self_test(const Args &a) {
  // 1. PBC bucket member counts at H=22 match the C++ cuckoo benchmark output.
  const auto expected = read_int_array(a.fixture);
  const auto buckets = pbc::build_buckets(pbc::benchmark_params(pbc::item_count(22), 22));
  std::vector<size_t> counts;
  for (const auto &b : buckets) counts.push_back(b.size());
  if (counts != expected) { std::cerr << "self-test: H=22 bucket counts differ\n"; return 1; }
  // 2. Seed-7 leaf schedules match the C++ benchmark.
  const std::vector<size_t> m22{489912, 3430182, 325581, 307198, 4049938, 1889243, 641477, 3537643};
  const std::vector<size_t> m24{13290720, 9622024, 8226918, 14471806, 8955543, 13755917, 1848245, 11926251};
  if (pbc::trial_plan(size_t{1} << 22, 3, 8, 7).second != m22 || pbc::trial_plan(size_t{1} << 24, 3, 8, 7).second != m24) {
    std::cerr << "self-test: leaf schedule differs\n";
    return 1;
  }
  // 3. The reused-NTT encoder equals upstream inverse_encode.
  Encoder enc;
  std::mt19937_64 rng(11);
  for (int trial = 0; trial < 4; ++trial) {
    std::vector<uint64_t> x(N), y(N);
    for (auto &v : x) v = rng() % 65536;
    y = x;
    enc.apply(x);
    inverse_encode(y);
    if (x != y) { std::cerr << "self-test: encoder differs from inverse_encode\n"; return 1; }
  }
  std::cout << "self-test ok: H=22 bucket counts, seed-7 leaves, encoder equivalence\n";
  return 0;
}

std::string jnum(double v) { std::ostringstream o; o.precision(6); o << std::fixed << v; return o.str(); }

}  // namespace

int main(int argc, char **argv) {
  try {
    const Args a = parse(argc, argv);
    if (a.self_test) return self_test(a);
    if (a.height < 8 || a.height > 27 || a.output.empty()) throw std::invalid_argument("--height 8..27 and --output are required");
    if (a.n1 <= 1 || (a.n1 & (a.n1 - 1)) != 0 || kColumns % a.n1 != 0) throw std::invalid_argument("--n1 must be a power of two dividing N/2");
    const size_t h = a.height, leaf_count = size_t{1} << h;
    const pbc::Params cp = pbc::benchmark_params(pbc::item_count(h), h);

    const auto setup_start = Clock::now();
    const auto buckets = pbc::build_buckets(cp);
    size_t max_members = 0, total_members = 0;
    std::vector<size_t> member_counts;
    for (const auto &b : buckets) { member_counts.push_back(b.size()); max_members = std::max(max_members, b.size()); total_members += b.size(); }
    const size_t items_needed = (max_members + kRecordsPerItem - 1) / kRecordsPerItem;
    size_t r = 1;
    while (r * kColumns < items_needed || r < a.min_r) r <<= 1;  // packingRLWEs needs a power of two
    std::cerr << "kspir_pbc: H=" << h << " buckets=" << cp.num_buckets << " max_members=" << max_members << " r=" << r
              << " N1=" << a.n1 << "\n";

    Secret queryKey(crtMod, false);
    const uint64_t modulus = crtMod;
    const uint64_t length = queryKey.getLength();
    AutoKeyBSGSRNS autoKey(N, crtMod, bsMod);
    autoKey.bsgsKeyGen(queryKey, a.n1);
    AutoKey packingKey(length, modulus, 4, 0, 0x01 << 14);
    packingKey.keyGen(queryKey, static_cast<int32_t>(r), true);
    std::vector<std::vector<int32_t>> permutations(a.n1, std::vector<int32_t>(length, 0));
    compute_permutation_matrix(permutations, a.n1, length);
    Bytes offline = key_bytes(autoKey.keyMap);
    offline += key_bytes(packingKey.keyMap);

    Encoder enc;
    std::vector<std::vector<uint64_t>> data(N, std::vector<uint64_t>(N / 2, 0));
    std::vector<std::vector<uint64_t>> data_ntt(N / 2, std::vector<uint64_t>(N, 0));
    std::vector<uint8_t> items;
    std::vector<std::unique_ptr<uint64_t, decltype(&std::free)>> dbs;
    for (const auto &bucket : buckets) {
      auto *mem = static_cast<uint64_t *>(aligned_alloc(64, sizeof(uint64_t) * kMatrixWords * r));
      if (!mem) throw std::bad_alloc();
      dbs.emplace_back(mem, &std::free);
      build_bucket(mem, bucket, r, a.n1, enc, data, data_ntt, items);
    }
    std::vector<uint8_t>().swap(items);
    const double offline_setup_ms = ms_since(setup_start);

    const auto plan = pbc::trial_plan(leaf_count, a.warmups, a.trials, a.seed);
    std::ostringstream samples;
    std::vector<double> server_v, client_v, decode_v, fd_v, ep_v, pk_v;
    Bytes query_per_path, response_per_path;
    bool shape_set = false;
    size_t measured_index = 0;
    for (int phase = 0; phase < 2; ++phase) {
      const auto &leaves = phase == 0 ? plan.first : plan.second;
      for (size_t trial = 0; trial < leaves.size(); ++trial) {
        const size_t leaf = leaves[trial];
        const auto ordinals = pbc::sibling_ordinals(leaf, h);
        const auto assignment = pbc::cuckoo_place(ordinals, cp);
        double client_ms = 0, server_ms = 0, decode_ms = 0, fd_ms = 0, ep_ms = 0, pk_ms = 0;
        Bytes q_bytes, r_bytes;
        size_t recovered = 0;
        for (size_t b = 0; b < cp.num_buckets; ++b) {
          const size_t pos = assignment[b].has_value() ? pbc::position_in_bucket(buckets[b], *assignment[b]) : 0;
          const size_t x = pos / kRecordsPerItem, col = x % kColumns, packing = x / kColumns;

          auto t0 = Clock::now();
          std::vector<RlweCiphertext> query1(1, RlweCiphertext(N, crtMod));
          query1.push_back(RlweCiphertext(N, bsMod));
          RGSWCiphertext queryGsw(N, modulus, 2, 0x01 << 20, 0x01 << 18);
          query_bsgs_rns(query1, queryKey, col);
          queryGsw.keyGen(queryKey, packing, true);
          client_ms += ms_since(t0);
          q_bytes += rlwe_bytes(query1[0]);
          q_bytes += rlwe_bytes(query1[1]);
          q_bytes += rgsw_bytes(queryGsw);

          std::vector<RlweCiphertext> result(r, RlweCiphertext(N, crtMod));
          std::vector<RlweCiphertext> ext_rlwes(r, RlweCiphertext(N, modulus));
          RlweCiphertext result_output(N, modulus);
          t0 = Clock::now();
          matrix_vector_mul_bsgs_rns_crt_large(result, query1, dbs[b].get(), autoKey, a.n1, permutations, static_cast<int32_t>(r));
          auto t1 = Clock::now();
          for (size_t j = 0; j < r; ++j) externalProduct(ext_rlwes[j], result[j], queryGsw);
          auto t2 = Clock::now();
          packingRLWEs(result_output, ext_rlwes, packingKey);
          auto t3 = Clock::now();
          fd_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
          ep_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
          pk_ms += std::chrono::duration<double, std::milli>(t3 - t2).count();
          server_ms += std::chrono::duration<double, std::milli>(t3 - t0).count();
          r_bytes += rlwe_bytes(result_output);

          std::vector<uint64_t> decrypted(length);
          t0 = Clock::now();
          decrypt_bsgs_total(decrypted, result_output, queryKey, static_cast<int32_t>(r));
          // decrypt_bsgs_total maps plaintext 0 with slightly negative noise to p
          // (its negative branch adds p without reducing); the plaintext space is Z_p.
          for (auto &v : decrypted) v %= bsgsp;
          decode_ms += ms_since(t0);
          if (assignment[b].has_value()) {
            const auto node_idx = pbc::node_of_ordinal(*assignment[b]);
            const auto expected = pbc::merkle_node(node_idx.first, node_idx.second);
            const size_t first = (pos % kRecordsPerItem) * (kRecordBytes / 2);
            for (size_t i = 0; i < kRecordBytes / 2; ++i) {
              const uint64_t v = decrypted[first + i];
              if (v > 0xffff || static_cast<uint8_t>(v) != expected[2 * i] || static_cast<uint8_t>(v >> 8) != expected[2 * i + 1]) {
                std::cerr << "kspir_pbc: record mismatch at leaf " << leaf << " bucket " << b << " pos " << pos
                          << " item " << x << " col " << col << " packing " << packing << "\n";
                // Diagnostic: compare the whole decrypted item with the stored item bytes.
                std::vector<uint8_t> item(kItemBytes, 0);
                for (size_t q = x * kRecordsPerItem; q < std::min(buckets[b].size(), (x + 1) * kRecordsPerItem); ++q) {
                  const auto nq = pbc::node_of_ordinal(buckets[b][q]);
                  const auto nd = pbc::merkle_node(nq.first, nq.second);
                  std::memcpy(&item[(q % kRecordsPerItem) * kRecordBytes], nd.data(), kRecordBytes);
                }
                size_t equal = 0, first_bad = kItemBytes;
                for (size_t m = 0; m < static_cast<size_t>(N); ++m) {
                  const uint64_t want = item[2 * m] | (static_cast<uint64_t>(item[2 * m + 1]) << 8);
                  if (decrypted[m] == want) ++equal; else if (first_bad == kItemBytes) first_bad = m;
                }
                std::cerr << "  item values equal: " << equal << "/" << N << ", first bad index " << first_bad << "\n  got:";
                for (size_t m = first_bad; m < std::min<size_t>(first_bad + 8, N); ++m) std::cerr << " " << decrypted[m];
                std::cerr << "\n want:";
                for (size_t m = first_bad; m < std::min<size_t>(first_bad + 8, N); ++m) std::cerr << " " << (item[2 * m] | (static_cast<uint64_t>(item[2 * m + 1]) << 8));
                std::cerr << "\n";
                return 3;
              }
            }
            ++recovered;
          }
        }
        if (recovered != h) { std::cerr << "kspir_pbc: recovered " << recovered << " of " << h << "\n"; return 3; }
        if (!shape_set) { query_per_path = q_bytes; response_per_path = r_bytes; shape_set = true; }
        if (phase == 1) {
          if (measured_index++) samples << ",\n";
          samples << "    {\"trial\": " << trial << ", \"leaf\": " << leaf << ", \"server_total_ms\": " << jnum(server_ms)
                  << ", \"client_query_ms\": " << jnum(client_ms) << ", \"client_decode_ms\": " << jnum(decode_ms)
                  << ", \"stages_ms\": {\"first_dim\": " << jnum(fd_ms) << ", \"external_product\": " << jnum(ep_ms)
                  << ", \"packing\": " << jnum(pk_ms) << "}, \"correct\": true}";
          server_v.push_back(server_ms); client_v.push_back(client_ms); decode_v.push_back(decode_ms);
          fd_v.push_back(fd_ms); ep_v.push_back(ep_ms); pk_v.push_back(pk_ms);
        }
      }
    }
    auto mean = [](const std::vector<double> &v) { return std::accumulate(v.begin(), v.end(), 0.0) / v.size(); };
    std::ofstream out(a.output);
    out << "{\n  \"schema_version\": \"kspir_pbc_profile.v1\",\n  \"scheme\": \"kspir_pbc\",\n"
        << "  \"upstream\": {\"repository\": \"" << kUpstreamRepo << "\", \"commit\": \"54c2f61d0ae3c4be49e7fd6b0406e524f9be2677\","
        << " \"patch\": \"scripts/path_baselines/kspir_pbc/patches/kspir-build.patch (HEXL 1.2.6, no hardcoded compiler)\"},\n"
        << "  \"compiler_label\": \"" << a.compiler_label << "\", \"min_r\": " << a.min_r << ",\n"
        << "  \"path_semantics\": \"legacy_merkle_paths_omit_root\",\n"
        << "  \"height\": " << h << ", \"leaf_count\": " << leaf_count << ", \"item_count\": " << cp.item_count
        << ", \"record_bytes\": " << kRecordBytes << ",\n  \"actual_logical_bytes\": " << cp.item_count * kRecordBytes << ",\n"
        << "  \"warmups\": " << a.warmups << ", \"measured_trials\": " << a.trials << ", \"trial_seed\": " << a.seed << ",\n"
        << "  \"pbc\": {\"framework\": \"same buckets, hashes, positions and leaf schedule as the OnionPIR cuckoo baseline\", "
        << "\"batch_size\": " << cp.batch_size << ", \"num_buckets\": " << cp.num_buckets << ", \"num_hashes\": 3, \"hash_seed\": "
        << cp.seed << ", \"max_bucket_members\": " << max_members << ", \"total_member_entries\": " << total_members
        << ", \"dummy_query_position\": 0},\n"
        << "  \"kspir_parameters\": {\"ring_degree\": " << N << ", \"crt_modulus_bits\": " << bits_of(crtMod)
        << ", \"bs_modulus_bits\": " << bits_of(bsMod) << ", \"plaintext_modulus\": " << bsgsp << ", \"packing_r\": " << r
        << ", \"bsgs_n1\": " << a.n1 << ", \"bsgs_n2\": " << kColumns / a.n1 << ", \"item_bytes\": " << kItemBytes
        << ", \"records_per_item\": " << kRecordsPerItem << ", \"items_per_bucket\": " << r * kColumns << "},\n"
        << "  \"setup\": {\"offline_setup_ms\": " << jnum(offline_setup_ms) << ", \"preprocessed_db_bytes\": "
        << static_cast<uint64_t>(cp.num_buckets) * kMatrixWords * r * 8 << ", \"logical_replicated_bytes\": " << total_members * kRecordBytes << "},\n"
        << "  \"communication\": {\"public_parameter_bytes\": " << offline.seeded << ", \"public_parameter_bytes_uncompressed\": " << offline.full
        << ", \"query_bytes_per_path\": " << query_per_path.seeded << ", \"query_bytes_per_path_uncompressed\": " << query_per_path.full
        << ", \"response_bytes_per_path\": " << response_per_path.full
        << ", \"kind\": \"bit-packed ciphertext sizes; seeded replaces each uniform mask by a 32-byte PRG seed, as the KsPIR paper reports; the response is the full packed ciphertext\"},\n"
        << "  \"timing_boundaries\": {\"server_total_ms\": \"matrix_vector_mul_bsgs_rns_crt_large + r externalProduct + packingRLWEs, summed over buckets\", "
        << "\"client_query_ms\": \"query_bsgs_rns + RGSW keyGen, summed over buckets\", \"client_decode_ms\": \"decrypt_bsgs_total, summed over buckets\"},\n"
        << "  \"samples\": [\n" << samples.str() << "\n  ],\n"
        << "  \"measured_summary\": {\"count\": " << a.trials << ", \"server_ms_mean\": " << jnum(mean(server_v))
        << ", \"client_query_ms_mean\": " << jnum(mean(client_v)) << ", \"client_decode_ms_mean\": " << jnum(mean(decode_v))
        << ", \"stages_ms_mean\": {\"first_dim\": " << jnum(mean(fd_v)) << ", \"external_product\": " << jnum(mean(ep_v))
        << ", \"packing\": " << jnum(mean(pk_v)) << "}, \"all_correct\": true}\n}\n";
    std::cerr << "kspir_pbc: H=" << h << " mean server " << mean(server_v) << " ms, query " << query_per_path.seeded
              << " B seeded, response " << response_per_path.full << " B, keys " << offline.seeded << " B\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "kspir_pbc: " << e.what() << "\n";
    return 2;
  }
}
