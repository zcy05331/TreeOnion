// VIA + PBC on the root-excluded Merkle path workload with the seed-7 leaves:
// one VIA query (Figure 4) per bucket of the OnionPIR cuckoo baseline's PBC
// instance (same records, hashes, bucket positions and leaf schedule; dummy
// buckets ask for position 0). A path query samples one key pair, so its
// buckets share one ring-switching key.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "pbc.hpp"
#include "via_base.hpp"

namespace via::base {
namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
double ms_between(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

struct Args {
  size_t height = 0, warmups = 3, trials = 8;
  uint64_t seed = 7;
  int log_rows = -1;  // log2 I; -1 picks the power of two nearest 0.65 * sqrt(polys)
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
    else if (k == "--log-rows") a.log_rows = std::stoi(val());
    else if (k == "--output") a.output = val();
    else if (k == "--fixture") a.fixture = val();
    else if (k == "--compiler-label") a.compiler_label = val();
    else if (k == "--self-test") a.self_test = true;
    else throw std::invalid_argument("unknown argument " + k);
  }
  return a;
}

size_t bits_of(u128 modulus) {
  size_t b = 0;
  for (u128 v = modulus - 1; v; v >>= 1) ++b;
  return b;
}
uint64_t packed_bytes(size_t count, size_t bits) { return (static_cast<uint64_t>(count) * bits + 7) / 8; }

std::array<uint8_t, 32> record_of(uint32_t ordinal) {
  const auto node = pbc::node_of_ordinal(ordinal);
  return pbc::merkle_node(node.first, node.second);
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

std::string jnum(double v) {
  std::ostringstream o;
  o.precision(6);
  o << std::fixed << v;
  return o.str();
}

template <typename RecordFn>
void build_db(const Ring &R2, const Shape &sh, size_t members, RecordFn record, BucketDb &db) {
  db.allocate(sh.I, sh.J);
  Vec poly(kN1);
  for (size_t alpha = 0; alpha < sh.I; ++alpha)
    for (size_t beta = 0; beta < sh.J; ++beta) {
      encode_poly(sh, alpha, beta, members, record, poly.data());
      R2.ntt[0]->ComputeForward(poly.data(), poly.data(), 1, 1);
      db.store(alpha, beta, poly.data());
    }
}

struct Stages {
  double expand = 0, dmux = 0, modswitch = 0, first_dim = 0, ring_switch = 0, cmux = 0, respond = 0;
  double total() const { return expand + dmux + modswitch + first_dim + ring_switch + cmux + respond; }
};

// The answer path of Figure 4 for one bucket query, with reused buffers.
struct Server {
  const Ring &R1, &R2, &R2s;
  Scratch S1, S2;
  ServerQuery q;
  std::vector<Ct> leaves, cols, small;
  Ct zero;
  Vec q_ab, tmp, A, B;
  Server(const Ring &r1, const Ring &r2, const Ring &r2s, const Shape &sh)
      : R1(r1), R2(r2), R2s(r2s), S1(2 * kCtrl.len, r1.words()), S2(kD * kRsk.len, r2s.words()),
        cols(sh.J, Ct(r2.words())), small(sh.J, Ct(r2s.words())), zero(r2s.words()), A(kN1), B(kN2) {}

  void answer(const Shape &sh, const BucketDb &db, const std::vector<Ct> &rsk, u64 seed, u64 stream, const BucketQuery &bq,
              std::vector<Answer> &out, Stages &st) {
    const auto t0 = Clock::now();
    server_expand(R1, R2s, sh, seed, stream, bq, q);
    const auto t1 = Clock::now();
    dmux(R1, S1, q, sh.I, leaves);
    const auto t2 = Clock::now();
    first_dim_query(R1, R2, leaves, q_ab, tmp);
    const auto t3 = Clock::now();
    first_dimension(db, q_ab.data(), cols);
    for (auto &c : cols) {
      inv(R2, c.a.data());
      inv(R2, c.b.data());
    }
    const auto t4 = Clock::now();
    for (size_t j = 0; j < sh.J; ++j) ring_switch(R2s, S2, rsk, cols[j], A, B, small[j]);
    const auto t5 = Clock::now();
    cmux_levels(R2s, S2, q.sel, small, sh.J);
    const auto t6 = Clock::now();
    out.resize(sh.answers());
    for (size_t k = 0; k < sh.answers(); ++k) {
      const size_t col = k << sh.sel_bits();
      respond(col < sh.J ? small[col] : zero, out[k]);
    }
    const auto t7 = Clock::now();
    st.expand += ms_between(t0, t1);
    st.dmux += ms_between(t1, t2);
    st.modswitch += ms_between(t2, t3);
    st.first_dim += ms_between(t3, t4);
    st.ring_switch += ms_between(t4, t5);
    st.cmux += ms_between(t5, t6);
    st.respond += ms_between(t6, t7);
  }
};

constexpr u64 kRskStreams = kD * kRsk.len;  // streams of the ring-switching key; bucket queries follow

// Every record position of a few toy buckets against the secret key: DMux below
// the client's levels, first dimension, ring switching, CMux with and without
// server levels, answers past J, and blinded extraction.
int scheme_self_test() {
  Ring R1(kN1, kQ11, kQ12), R2(kN1, kQ2), R2s(kN2, kQ2), R3(kN2, kQ3);
  Prg kg(99, 1), noise(5, 6);
  const ClientKeys K = client_keygen(R1, R2s, R3, kg);
  const u64 seed = 1234;
  const auto rsk_b = client_rsk(R2s, K, seed, 0, noise);
  const auto rsk = server_rsk(R2s, seed, 0, rsk_b);
  // (log_i, members): I = 16, J = 12 has one server DMux level and one server
  // CMux level; I = 4, J = 5 has neither and three empty answers.
  const std::pair<size_t, size_t> shapes[] = {{4, 16 * 12 * kRecordsPerPoly - 5}, {2, 4 * 5 * kRecordsPerPoly}};
  i64 worst = 0;
  size_t checked = 0;
  for (const auto &[log_i, members] : shapes) {
    const Shape sh = Shape::make(members, log_i);
    BucketDb db;
    build_db(R2, sh, members, [](size_t pos) { return record_of(static_cast<uint32_t>(pos)); }, db);
    Server server(R1, R2, R2s, sh);
    std::vector<Answer> answers;
    Stages st;
    for (const size_t gamma : {size_t{0}, size_t{1}, size_t{3}, size_t{4}, size_t{62}, size_t{63}})
      for (const size_t alpha : {size_t{0}, sh.I / 2 + 1, sh.I - 1})
        for (const size_t beta : {size_t{0}, sh.J / 2, sh.J - 1}) {
          const size_t pos = gamma * sh.I * sh.J + alpha * sh.J + beta;
          if (pos >= members) continue;
          const BucketQuery bq = client_bucket_query(R1, R2s, K, sh, alpha, beta, gamma, seed, kRskStreams, noise);
          server.answer(sh, db, rsk, seed, kRskStreams, bq, answers, st);
          std::array<uint8_t, kRecordBytes> got{};
          client_decode(R3, K, answers[sh.answer_of(beta)], got, worst);
          if (got != record_of(static_cast<uint32_t>(pos))) {
            std::cerr << "self-test: I=" << sh.I << " J=" << sh.J << " position " << pos << " (alpha " << alpha << ", beta "
                      << beta << ", gamma " << gamma << ") decoded wrongly\n";
            return 1;
          }
          ++checked;
        }
  }
  std::cout << "  " << checked << " toy-bucket records decoded, max error " << worst << " of " << kQ4 / kP / 2
            << " (q4 units)\n";
  return 0;
}

int self_test(const Args &a) {
  const auto expected = read_int_array(a.fixture);
  const auto buckets = pbc::build_buckets(pbc::benchmark_params(pbc::item_count(22), 22));
  std::vector<size_t> counts;
  for (const auto &b : buckets) counts.push_back(b.size());
  if (counts != expected) { std::cerr << "self-test: H=22 bucket counts differ\n"; return 1; }
  const std::vector<size_t> m22{489912, 3430182, 325581, 307198, 4049938, 1889243, 641477, 3537643};
  const std::vector<size_t> m24{13290720, 9622024, 8226918, 14471806, 8955543, 13755917, 1848245, 11926251};
  if (pbc::trial_plan(size_t{1} << 22, 3, 8, 7).second != m22 || pbc::trial_plan(size_t{1} << 24, 3, 8, 7).second != m24) {
    std::cerr << "self-test: leaf schedule differs\n";
    return 1;
  }
  if (scheme_self_test()) return 1;
  std::cout << "self-test ok: H=22 bucket counts, seed-7 leaves, VIA answers\n";
  return 0;
}

int run(const Args &a) {
  if (a.height < 8 || a.height > 27 || a.output.empty()) throw std::invalid_argument("--height 8..27 and --output are required");
  const size_t h = a.height, leaf_count = size_t{1} << h, items = pbc::item_count(h);

  const auto bucket_start = Clock::now();
  const pbc::Params cp = pbc::benchmark_params(items, h);
  const auto buckets = pbc::build_buckets(cp);
  size_t max_members = 0, total_members = 0;
  for (const auto &b : buckets) {
    max_members = std::max(max_members, b.size());
    total_members += b.size();
  }
  const size_t polys = (max_members + kRecordsPerPoly - 1) / kRecordsPerPoly;
  size_t log_i = 0;
  if (a.log_rows >= 0) {
    log_i = static_cast<size_t>(a.log_rows);
  } else {
    // A DMux node over (n1, q1) and its row's switch to q2 cost about 2.5
    // columns' ring switch and CMux node over (n2, q2) (measured), so
    // I ~ 0.65 * sqrt(polys) balances the rows against the columns.
    log_i = static_cast<size_t>(std::lround(std::log2(std::max(1.0, 0.65 * std::sqrt(static_cast<double>(polys))))));
  }
  const Shape sh = Shape::make(max_members, log_i);
  if (sh.I > polys && log_i > 0) throw std::invalid_argument("--log-rows exceeds the bucket's polynomial count");
  const double bucket_ms = ms_since(bucket_start);
  std::cerr << "via_pbc: H=" << h << " buckets=" << cp.num_buckets << " max_members=" << max_members << " I=" << sh.I
            << " J=" << sh.J << " log_j=" << sh.log_j << " answers=" << sh.answers() << "\n";

  Ring R1(kN1, kQ11, kQ12), R2(kN1, kQ2), R2s(kN2, kQ2), R3(kN2, kQ3);
  auto t0 = Clock::now();
  std::vector<BucketDb> dbs(buckets.size());
  for (size_t b = 0; b < buckets.size(); ++b)
    build_db(R2, sh, buckets[b].size(), [&](size_t pos) { return record_of(buckets[b][pos]); }, dbs[b]);
  const double db_setup_ms = ms_since(t0);
  uint64_t db_bytes = 0;
  for (const auto &d : dbs) db_bytes += d.bytes();

  const auto plan = pbc::trial_plan(leaf_count, a.warmups, a.trials, a.seed);
  Server server(R1, R2, R2s, sh);
  std::vector<std::vector<Answer>> answers(cp.num_buckets);
  struct Spec {
    size_t pos, alpha, beta, gamma;
    bool real;
    uint32_t ordinal;
  };
  std::ostringstream samples;
  std::vector<double> server_v, client_v, decode_v;
  std::vector<Stages> stage_v;
  i64 worst_error = 0;
  size_t measured_index = 0;
  for (int phase = 0; phase < 2; ++phase) {
    const auto &trial_leaves = phase == 0 ? plan.first : plan.second;
    for (size_t trial = 0; trial < trial_leaves.size(); ++trial) {
      const size_t leaf = trial_leaves[trial];
      const auto assignment = pbc::cuckoo_place(pbc::sibling_ordinals(leaf, h), cp);
      std::vector<Spec> specs(cp.num_buckets);
      for (size_t b = 0; b < cp.num_buckets; ++b) {
        Spec &s = specs[b];
        s.real = assignment[b].has_value();
        s.ordinal = s.real ? *assignment[b] : 0;
        s.pos = s.real ? pbc::position_in_bucket(buckets[b], s.ordinal) : 0;
        sh.locate(s.pos, s.alpha, s.beta, s.gamma);
      }
      u64 seed_state = a.seed * 0x10001 + leaf;
      const u64 query_seed = Prg::splitmix(seed_state), key_seed = Prg::splitmix(seed_state);

      // Client: fresh keys, the ring-switching key, one VIA query per bucket.
      auto tc = Clock::now();
      Prg kg(key_seed, 0x6b), noise(key_seed, 0xe44);
      const ClientKeys K = client_keygen(R1, R2s, R3, kg);
      const auto rsk_b = client_rsk(R2s, K, query_seed, 0, noise);
      std::vector<BucketQuery> queries(cp.num_buckets);
      for (size_t b = 0; b < cp.num_buckets; ++b)
        queries[b] = client_bucket_query(R1, R2s, K, sh, specs[b].alpha, specs[b].beta, specs[b].gamma, query_seed,
                                         kRskStreams + b * sh.ciphertexts(), noise);
      const double client_ms = ms_since(tc);

      // Server.
      Stages st;
      auto s0 = Clock::now();
      const auto rsk = server_rsk(R2s, query_seed, 0, rsk_b);
      st.expand += ms_since(s0);
      for (size_t b = 0; b < cp.num_buckets; ++b)
        server.answer(sh, dbs[b], rsk, query_seed, kRskStreams + b * sh.ciphertexts(), queries[b], answers[b], st);

      // Client: decode each bucket's answer and check every real record against the oracle.
      tc = Clock::now();
      std::vector<std::array<uint8_t, kRecordBytes>> got(cp.num_buckets);
      for (size_t b = 0; b < cp.num_buckets; ++b)
        client_decode(R3, K, answers[b][sh.answer_of(specs[b].beta)], got[b], worst_error);
      const double decode_ms = ms_since(tc);
      size_t recovered = 0;
      for (size_t b = 0; b < cp.num_buckets; ++b) {
        if (!specs[b].real) continue;
        if (got[b] != record_of(specs[b].ordinal)) {
          std::cerr << "via_pbc: record mismatch at leaf " << leaf << " bucket " << b << " pos " << specs[b].pos << " (alpha "
                    << specs[b].alpha << ", beta " << specs[b].beta << ", gamma " << specs[b].gamma << ")\n";
          return 3;
        }
        ++recovered;
      }
      if (recovered != h) { std::cerr << "via_pbc: recovered " << recovered << " of " << h << "\n"; return 3; }
      const double server_ms = st.total();
      std::cerr << "via_pbc: " << (phase ? "trial " : "warmup ") << trial << " leaf " << leaf << " server " << server_ms
                << " ms (dmux " << st.dmux << ", first_dim " << st.first_dim << ", ring_switch " << st.ring_switch << ", cmux "
                << st.cmux << ")\n";
      if (phase == 1) {
        if (measured_index++) samples << ",\n";
        samples << "    {\"trial\": " << trial << ", \"leaf\": " << leaf << ", \"server_total_ms\": " << jnum(server_ms)
                << ", \"client_query_ms\": " << jnum(client_ms) << ", \"client_decode_ms\": " << jnum(decode_ms)
                << ", \"stages_ms\": {\"expand\": " << jnum(st.expand) << ", \"dmux\": " << jnum(st.dmux)
                << ", \"modswitch\": " << jnum(st.modswitch) << ", \"first_dim\": " << jnum(st.first_dim)
                << ", \"ring_switch\": " << jnum(st.ring_switch) << ", \"cmux\": " << jnum(st.cmux)
                << ", \"respond\": " << jnum(st.respond) << "}, \"correct\": true}";
        server_v.push_back(server_ms);
        client_v.push_back(client_ms);
        decode_v.push_back(decode_ms);
        stage_v.push_back(st);
      }
    }
  }

  // Communication. Query: per bucket the b parts of the sent DMux leaves and of
  // the control and select RGSW rows, per path the ring-switching key's b parts
  // and one 32-byte seed for every mask. Response: per answer the q3 mask and
  // the record's q4 coefficients.
  const size_t q1_bits = bits_of(R1.Q), q2_bits = bits_of(kQ2), q3_bits = bits_of(kQ3);
  const uint64_t rlwe_q1 = packed_bytes(kN1, q1_bits), rlwe_q2 = packed_bytes(kN2, q2_bits);
  const uint64_t bucket_query = sh.sent_leaves() * rlwe_q1 + sh.ctrl_bits() * 2 * kCtrl.len * rlwe_q1 +
                                sh.sel_bits() * (kSelA.len + kSelB.len) * rlwe_q2;
  const uint64_t rsk_bytes = kRskStreams * rlwe_q2;
  const uint64_t query_bytes = cp.num_buckets * bucket_query + rsk_bytes + 32;
  const uint64_t query_bytes_full = 2 * (cp.num_buckets * bucket_query + rsk_bytes);
  const uint64_t answer_bytes = packed_bytes(kN2, q3_bits) + packed_bytes(kRecordCoeffs, kLogQ4);
  const uint64_t response_bytes = cp.num_buckets * sh.answers() * answer_bytes;

  auto mean = [](const std::vector<double> &v) { return std::accumulate(v.begin(), v.end(), 0.0) / v.size(); };
  auto stage_mean = [&](double Stages::*f) {
    double s = 0;
    for (const auto &x : stage_v) s += x.*f;
    return s / stage_v.size();
  };
  const double margin_bits = std::log2(static_cast<double>(kQ4 / kP / 2) / std::max<i64>(worst_error, 1));
  std::ofstream out(a.output);
  out << "{\n  \"schema_version\": \"via_pbc_profile.v1\",\n  \"scheme\": \"via_pbc\",\n"
      << "  \"source\": {\"paper\": \"VIA, ePrint 2025/2074 (S&P 2026), Section 3, Figure 4 and the Section 3.2 optimizations\", "
      << "\"implementation\": \"scripts/path_baselines/via_pbc/via_base.hpp\"},\n"
      << "  \"compiler_label\": \"" << a.compiler_label << "\",\n"
      << "  \"path_semantics\": \"legacy_merkle_paths_omit_root\",\n"
      << "  \"height\": " << h << ", \"leaf_count\": " << leaf_count << ", \"item_count\": " << items
      << ", \"record_bytes\": " << kRecordBytes << ",\n  \"actual_logical_bytes\": " << items * kRecordBytes << ",\n"
      << "  \"warmups\": " << a.warmups << ", \"measured_trials\": " << a.trials << ", \"trial_seed\": " << a.seed << ",\n"
      << "  \"batch\": {\"queries_per_path\": " << cp.num_buckets
      << ", \"framework\": \"same buckets, hashes, positions and leaf schedule as the OnionPIR cuckoo baseline\", "
      << "\"batch_size\": " << cp.batch_size << ", \"num_buckets\": " << cp.num_buckets << ", \"num_hashes\": 3, \"hash_seed\": "
      << cp.seed << ", \"max_bucket_members\": " << max_members << ", \"total_member_entries\": " << total_members
      << ", \"dummy_query_position\": 0, \"keys\": \"one S1, S2 and ring-switching key per path\"},\n"
      << "  \"via_parameters\": {\"n1\": " << kN1 << ", \"n2\": " << kN2 << ", \"q1_bits\": " << q1_bits << ", \"q2_bits\": "
      << q2_bits << ", \"q3_bits\": " << q3_bits << ", \"q4_bits\": " << kLogQ4 << ", \"plaintext_modulus\": " << kP
      << ", \"s1\": \"uniform on [-2, 2]\", \"sigma_e1\": " << kSigmaE1 << ", \"s2\": \"ternary\", \"sigma_e2\": " << kSigmaE2
      << ", \"gadgets\": {\"dmux\": [" << kCtrl.len << ", " << kCtrl.log_base << "], \"cmux_a\": [" << kSelA.len << ", "
      << kSelA.log_base << "], \"cmux_b\": [" << kSelB.len << ", " << kSelB.log_base << "], \"ring_switch\": [" << kRsk.len
      << ", " << kRsk.log_base << "]},"
      << " \"record_coefficients\": " << kRecordCoeffs << ", \"records_per_polynomial\": " << kRecordsPerPoly << ",\n"
      << "    \"rows_I\": " << sh.I << ", \"columns_J\": " << sh.J << ", \"log_rows\": " << sh.log_i << ", \"log_columns\": "
      << sh.log_j << ", \"client_dmux_levels\": " << sh.client_levels << ", \"skipped_cmux_levels\": " << sh.skipped
      << ", \"answers_per_bucket\": " << sh.answers() << ", \"query_ciphertexts_per_bucket\": " << sh.ciphertexts() << "},\n"
      << "  \"setup\": {\"database_build_ms\": " << jnum(bucket_ms) << ", \"db_encode_ms\": " << jnum(db_setup_ms)
      << ", \"preprocessed_db_bytes\": " << db_bytes << ", \"logical_stored_bytes\": " << total_members * kRecordBytes << "},\n"
      << "  \"communication\": {\"public_parameter_bytes\": 0, \"query_bytes_per_bucket\": " << bucket_query
      << ", \"ring_switching_key_bytes\": " << rsk_bytes << ", \"query_bytes_per_path\": " << query_bytes
      << ", \"query_bytes_per_path_uncompressed\": " << query_bytes_full << ", \"response_bytes_per_path\": " << response_bytes
      << ", \"kind\": \"bit-packed; seeded masks count as one 32-byte seed, as the VIA paper reports; each answer is the q3 mask plus the record's q4 coefficients\"},\n"
      << "  \"timing_boundaries\": {\"server_total_ms\": \"from the received b parts and mask seed to the extracted answers: mask expansion, DMux, q1->q2 switch, first dimension, ring switching, CMux and the final switch, for every bucket\", "
      << "\"client_query_ms\": \"key generation, ring-switching key and every bucket query\", \"client_decode_ms\": \"recovery of each bucket's selected answer\"},\n"
      << "  \"samples\": [\n" << samples.str() << "\n  ],\n"
      << "  \"measured_summary\": {\"count\": " << a.trials << ", \"server_ms_mean\": " << jnum(mean(server_v))
      << ", \"client_query_ms_mean\": " << jnum(mean(client_v)) << ", \"client_decode_ms_mean\": " << jnum(mean(decode_v))
      << ", \"stages_ms_mean\": {\"expand\": " << jnum(stage_mean(&Stages::expand)) << ", \"dmux\": "
      << jnum(stage_mean(&Stages::dmux)) << ", \"modswitch\": " << jnum(stage_mean(&Stages::modswitch)) << ", \"first_dim\": "
      << jnum(stage_mean(&Stages::first_dim)) << ", \"ring_switch\": " << jnum(stage_mean(&Stages::ring_switch))
      << ", \"cmux\": " << jnum(stage_mean(&Stages::cmux)) << ", \"respond\": " << jnum(stage_mean(&Stages::respond))
      << "}, \"max_error_q4\": " << worst_error << ", \"decoding_margin_bits\": " << jnum(margin_bits)
      << ", \"all_correct\": true}\n}\n";
  std::cerr << "via_pbc: H=" << h << " mean server " << mean(server_v) << " ms, query " << query_bytes << " B, response "
            << response_bytes << " B, max error " << worst_error << "/" << kQ4 / kP / 2 << "\n";
  return 0;
}

}  // namespace
}  // namespace via::base

int main(int argc, char **argv) {
  try {
    const auto a = via::base::parse(argc, argv);
    return a.self_test ? via::base::self_test(a) : via::base::run(a);
  } catch (const std::exception &e) {
    std::cerr << "via_pbc: " << e.what() << "\n";
    return 2;
  }
}
