// VIA-B (the paper's Figure 9) on the root-excluded Merkle path workload with
// the seed-7 leaves: the H sibling queries are independent VIA-C queries over
// the whole database, answers repacked eight per ring-switched response
// ciphertext.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "pbc.hpp"
#include "via.hpp"

namespace {

using namespace via;
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Args {
  size_t height = 0, warmups = 3, trials = 8;
  uint64_t seed = 7;
  int log_rows = -1;  // log2 I; -1 picks the power of two nearest 0.55 * sqrt(polys)
  std::string output, compiler_label;
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

std::string jnum(double v) {
  std::ostringstream o;
  o.precision(6);
  o << std::fixed << v;
  return o.str();
}

// Largest |error| of the message part of a coefficient-form decryption.
double noise_bits(const Ring &R, const Vec &m_plus_e, const std::vector<i128> &expect) {
  i128 worst = 0;
  for (size_t j = 0; j < R.n; ++j) {
    i128 e = R.centered(m_plus_e.data(), j) - expect[j];
    const i128 Qh = i128(R.Q / 2);
    while (e > Qh) e -= i128(R.Q);
    while (e < -Qh) e += i128(R.Q);
    if (e < 0) e = -e;
    worst = std::max(worst, e);
  }
  return worst ? std::log2(static_cast<double>(worst)) : 0.0;
}

// The paper's building blocks on small messages, against the secret key.
int scheme_self_test() {
  Ring R1(kN1, kQ11, kQ12), R2(kN1, kQ2), R3(kN2, kQ3);
  const ClientKeys K = client_keygen(R1, R2, R3, 99);
  const PublicParams pp = client_public_params(R1, R3, K, 1001, 1002);
  const ServerKeys SK = server_prepare(R1, R2, pp);
  Scratch S1(32, R1.words()), S2(16, R2.words()), S3(32, R3.words());
  ConvScratch C;

  // LWE -> RLWE -> RGSW on one gadget digit of a set control bit.
  const Shape sh = Shape::make(4 * 3 * kRecordsPerPoly, 2);  // I = 4, J = 3
  Prg noise(5, 6);
  const size_t alpha = 2, beta = 1, gamma = 7;
  const auto b = client_query(R1, K, sh, alpha, beta, gamma, 77, 0, noise);
  BucketKeys keys;
  server_decompress(R1, R2, S1, C, pp, sh, 77, 0, b.data(), keys);
  {
    // ctrl[0] encrypts alpha's top bit (1): msg digit p decrypts to g_p, neg digit to -S1 * g_p.
    const auto g = gadget_values(R1, kCtrl, false);
    for (size_t p = 0; p < kCtrl.len; ++p) {
      std::vector<i128> want(kN1, 0);
      want[0] = i128(g[p]);
      const double nb = noise_bits(R1, decrypt(R1, K.s1_q1, keys.ctrl[0].msg.c[p]), want);
      for (size_t j = 0; j < kN1; ++j) want[j] = -i128(K.s1[j]) * i128(g[p]);
      const double nn = noise_bits(R1, decrypt(R1, K.s1_q1, keys.ctrl[0].neg.c[p]), want);
      std::cout << "  LWE-to-RLWE noise 2^" << nb << ", RLWE-to-RGSW noise 2^" << nn << " (q1 = 2^75)\n";
      if (nb > 50 || nn > 62) { std::cerr << "self-test: conversion noise too large\n"; return 1; }
    }
  }
  // DMux: only leaf alpha holds floor(q1/p).
  std::vector<Ct> leaves;
  dmux_tree(R1, S1, keys.ctrl, leaves);
  for (size_t i = 0; i < leaves.size(); ++i) {
    Ct c = leaves[i];
    fwd(R1, c.a.data());
    fwd(R1, c.b.data());
    std::vector<i128> want(kN1, 0);
    if (i == alpha) want[0] = i128(R1.Q / kP);
    const double nb = noise_bits(R1, decrypt(R1, K.s1_q1, c), want);
    if (nb > 60) { std::cerr << "self-test: DMux leaf " << i << " noise 2^" << nb << "\n"; return 1; }
  }
  // Repacking and ring switching of eight known answers: answer r encrypts
  // X^0 * (r + 1) + X^32 * r in the record slot 0 (coefficients 32v).
  std::vector<Ct> answers(kAnswersPerPacked, Ct(R2.words()));
  Prg mask(12, 13), enc_noise(14, 15);
  for (size_t r = 0; r < kAnswersPerPacked; ++r) {
    Vec msg(R2.words());
    for (size_t v = 0; v < kRecordCoeffs; ++v) msg[kRecordsPerPoly * v] = static_cast<u64>((r + v) % kP) * (kQ2 / kP);
    msg[5] = static_cast<u64>(9) * (kQ2 / kP);  // outside the record slot: must not leak into the result
    answers[r] = encrypt(R2, K.s1_q2, msg, 3.2, mask, enc_noise);
    inv(R2, answers[r].a.data());
    inv(R2, answers[r].b.data());
  }
  std::vector<const Ct *> ptrs;
  for (const auto &c : answers) ptrs.push_back(&c);
  Ct packed;
  repack(R2, S2, C, SK, ptrs, packed);
  Response resp;
  respond(R3, S3, pp, packed, resp);
  std::vector<u64> nib;
  std::vector<i64> err;
  client_decode(R3, K, resp, nib, err);
  i64 worst = 0;
  for (size_t r = 0; r < kAnswersPerPacked; ++r)
    for (size_t v = 0; v < kRecordCoeffs; ++v) {
      if (nib[packed_coeff(r, v)] != (r + v) % kP) {
        std::cerr << "self-test: repack answer " << r << " coefficient " << v << " decoded " << nib[packed_coeff(r, v)] << "\n";
        return 1;
      }
      worst = std::max<i64>(worst, std::abs(err[packed_coeff(r, v)]));
    }
  std::cout << "  repack + ring switch: 8 answers decoded, max error " << worst << " of " << kQ4 / kP / 2 << " (q4 units)\n";
  return 0;
}

int self_test() {
  const std::vector<size_t> m22{489912, 3430182, 325581, 307198, 4049938, 1889243, 641477, 3537643};
  const std::vector<size_t> m24{13290720, 9622024, 8226918, 14471806, 8955543, 13755917, 1848245, 11926251};
  if (pbc::trial_plan(size_t{1} << 22, 3, 8, 7).second != m22 || pbc::trial_plan(size_t{1} << 24, 3, 8, 7).second != m24) {
    std::cerr << "self-test: leaf schedule differs\n";
    return 1;
  }
  if (scheme_self_test()) return 1;
  std::cout << "self-test ok: seed-7 leaves, conversion, DMux, repacking\n";
  return 0;
}

struct Stages {
  double decompress = 0, dmux = 0, modswitch = 0, first_dim = 0, cmux = 0, crot = 0, repack = 0, respcomp = 0;
  double total() const { return decompress + dmux + modswitch + first_dim + cmux + crot + repack + respcomp; }
};

}  // namespace

int main(int argc, char **argv) {
  try {
    const Args a = parse(argc, argv);
    if (a.self_test) return self_test();
    if (a.height < 8 || a.height > 27 || a.output.empty()) throw std::invalid_argument("--height 8..27 and --output are required");
    const size_t h = a.height, leaf_count = size_t{1} << h, items = pbc::item_count(h);

    // One database of every record; ordinal = position.
    const auto bucket_start = Clock::now();
    const size_t max_records = items, total_records = items, queries_per_path = h;
    const size_t polys = (max_records + kRecordsPerPoly - 1) / kRecordsPerPoly;
    size_t log_i = 0;
    if (a.log_rows >= 0) {
      log_i = static_cast<size_t>(a.log_rows);
    } else {
      // A DMux node over q1 costs about 3x a CMux node over q2 plus a column
      // transform, so I ~ 0.55 * sqrt(polys) balances the two trees.
      const double target = std::log2(std::max(1.0, 0.55 * std::sqrt(static_cast<double>(polys))));
      log_i = static_cast<size_t>(std::lround(target));
    }
    const Shape sh = Shape::make(max_records, log_i);
    if (sh.I > polys && log_i > 0) throw std::invalid_argument("--log-rows exceeds the database's polynomial count");
    const double bucket_ms = ms_since(bucket_start);
    std::cerr << "via_b: H=" << h << " queries=" << queries_per_path << " max_records=" << max_records << " I=" << sh.I
              << " J=" << sh.J << " log_j=" << sh.log_j << "\n";

    Ring R1(kN1, kQ11, kQ12), R2(kN1, kQ2), R3(kN2, kQ3);
    auto t0 = Clock::now();
    const ClientKeys K = client_keygen(R1, R2, R3, 0x5eed0001 ^ a.seed);
    const PublicParams pp = client_public_params(R1, R3, K, 0x5eed0002 ^ a.seed, 0x5eed0003 ^ a.seed);
    const double client_setup_ms = ms_since(t0);
    t0 = Clock::now();
    const ServerKeys SK = server_prepare(R1, R2, pp);
    const double server_key_ms = ms_since(t0);

    t0 = Clock::now();
    BucketDb db;
    Vec poly(kN1);
    db.allocate(sh.I, sh.J);
    for (size_t alpha = 0; alpha < sh.I; ++alpha)
      for (size_t beta = 0; beta < sh.J; ++beta) {
        encode_poly(sh, alpha, beta, items, [](size_t pos) { return record_of(static_cast<uint32_t>(pos)); }, poly.data());
        R2.ntt[0]->ComputeForward(poly.data(), poly.data(), 1, 1);
        db.store(alpha, beta, poly.data());
      }
    const double db_setup_ms = ms_since(t0);
    const uint64_t db_bytes = db.bytes();

    const auto plan = pbc::trial_plan(leaf_count, a.warmups, a.trials, a.seed);
    Scratch S1(32, R1.words()), S2(16, R2.words()), S3(32, R3.words());
    ConvScratch C;
    BucketKeys keys;
    std::vector<Ct> leaves, cols(sh.J, Ct(R2.words()));
    std::vector<Ct> answers(queries_per_path, Ct(R2.words()));
    Vec q_ab, tmp;
    const size_t groups = (queries_per_path + kAnswersPerPacked - 1) / kAnswersPerPacked;
    std::vector<Response> responses(groups);

    std::ostringstream samples;
    std::vector<double> server_v, client_v, decode_v;
    std::vector<Stages> stage_v;
    i64 worst_error = 0;
    size_t measured_index = 0;
    for (int phase = 0; phase < 2; ++phase) {
      const auto &trial_leaves = phase == 0 ? plan.first : plan.second;
      for (size_t trial = 0; trial < trial_leaves.size(); ++trial) {
        const size_t leaf = trial_leaves[trial];
        const auto ordinals = pbc::sibling_ordinals(leaf, h);  // record positions in the database
        u64 seed_state = a.seed * 0x10001 + leaf;
        const u64 query_seed = Prg::splitmix(seed_state);

        // Client: one compressed VIA-C query per sibling.
        auto tc = Clock::now();
        Prg noise(query_seed, 0xe44);
        std::vector<std::vector<u64>> queries(ordinals.size());
        for (size_t q = 0; q < ordinals.size(); ++q) {
          size_t alpha, beta, gamma;
          sh.locate(ordinals[q], alpha, beta, gamma);
          queries[q] = client_query(R1, K, sh, alpha, beta, gamma, query_seed, q * sh.lwes(), noise);
        }
        const double client_ms = ms_since(tc);

        // Server.
        Stages st;
        for (size_t q = 0; q < ordinals.size(); ++q) {
          auto s0 = Clock::now();
          server_decompress(R1, R2, S1, C, pp, sh, query_seed, q * sh.lwes(), queries[q].data(), keys);
          auto s1 = Clock::now();
          dmux_tree(R1, S1, keys.ctrl, leaves);
          auto s2 = Clock::now();
          first_dim_input(R1, R2, leaves, q_ab, tmp);
          auto s3 = Clock::now();
          first_dimension(db, q_ab.data(), cols);
          for (auto &c : cols) {
            inv(R2, c.a.data());
            inv(R2, c.b.data());
          }
          auto s4 = Clock::now();
          cmux_tree(R2, S2, keys.sel, cols, sh.J);
          auto s5 = Clock::now();
          crot(R2, S2, keys.rot, cols[0]);
          answers[q] = cols[0];
          auto s6 = Clock::now();
          st.decompress += std::chrono::duration<double, std::milli>(s1 - s0).count();
          st.dmux += std::chrono::duration<double, std::milli>(s2 - s1).count();
          st.modswitch += std::chrono::duration<double, std::milli>(s3 - s2).count();
          st.first_dim += std::chrono::duration<double, std::milli>(s4 - s3).count();
          st.cmux += std::chrono::duration<double, std::milli>(s5 - s4).count();
          st.crot += std::chrono::duration<double, std::milli>(s6 - s5).count();
        }
        for (size_t g = 0; g < groups; ++g) {
          auto s0 = Clock::now();
          std::vector<const Ct *> ptrs;
          for (size_t q = g * kAnswersPerPacked; q < std::min(ordinals.size(), (g + 1) * kAnswersPerPacked); ++q)
            ptrs.push_back(&answers[q]);
          Ct packed;
          repack(R2, S2, C, SK, ptrs, packed);
          auto s1 = Clock::now();
          respond(R3, S3, pp, packed, responses[g]);
          auto s2 = Clock::now();
          st.repack += std::chrono::duration<double, std::milli>(s1 - s0).count();
          st.respcomp += std::chrono::duration<double, std::milli>(s2 - s1).count();
        }

        // Client: decode and check every record against the oracle.
        tc = Clock::now();
        std::vector<std::vector<u64>> nibs(groups);
        std::vector<std::vector<i64>> errs(groups);
        for (size_t g = 0; g < groups; ++g) client_decode(R3, K, responses[g], nibs[g], errs[g]);
        const double decode_ms = ms_since(tc);
        size_t recovered = 0;
        for (size_t q = 0; q < ordinals.size(); ++q) {
          const size_t g = q / kAnswersPerPacked, r = q % kAnswersPerPacked;
          for (size_t v = 0; v < kRecordCoeffs; ++v) worst_error = std::max<i64>(worst_error, std::abs(errs[g][packed_coeff(r, v)]));
          const auto expected = record_of(ordinals[q]);
          for (size_t byte = 0; byte < kRecordBytes; ++byte) {
            const u64 got = nibs[g][packed_coeff(r, 2 * byte)] | (nibs[g][packed_coeff(r, 2 * byte + 1)] << 4);
            if (got != expected[byte]) {
              size_t alpha, beta, gamma;
              sh.locate(ordinals[q], alpha, beta, gamma);
              std::cerr << "via_b: record mismatch at leaf " << leaf << " query " << q << " pos " << ordinals[q] << " (alpha "
                        << alpha << ", beta " << beta << ", gamma " << gamma << ") byte " << byte << ": got " << got << " want "
                        << static_cast<int>(expected[byte]) << "\n";
              return 3;
            }
          }
          ++recovered;
        }
        if (recovered != h) { std::cerr << "via_b: recovered " << recovered << " of " << h << "\n"; return 3; }
        const double server_ms = st.total();
        std::cerr << "via_b: " << (phase ? "trial " : "warmup ") << trial << " leaf " << leaf << " server " << server_ms
                  << " ms (decompress " << st.decompress << ", dmux " << st.dmux << ", first_dim " << st.first_dim
                  << ", cmux " << st.cmux << ")\n";
        if (phase == 1) {
          if (measured_index++) samples << ",\n";
          samples << "    {\"trial\": " << trial << ", \"leaf\": " << leaf << ", \"server_total_ms\": " << jnum(server_ms)
                  << ", \"client_query_ms\": " << jnum(client_ms) << ", \"client_decode_ms\": " << jnum(decode_ms)
                  << ", \"stages_ms\": {\"decompress\": " << jnum(st.decompress) << ", \"dmux\": " << jnum(st.dmux)
                  << ", \"modswitch\": " << jnum(st.modswitch) << ", \"first_dim\": " << jnum(st.first_dim)
                  << ", \"cmux\": " << jnum(st.cmux) << ", \"crot\": " << jnum(st.crot) << ", \"repack\": " << jnum(st.repack)
                  << ", \"respcomp\": " << jnum(st.respcomp) << "}, \"correct\": true}";
          server_v.push_back(server_ms);
          client_v.push_back(client_ms);
          decode_v.push_back(decode_ms);
          stage_v.push_back(st);
        }
      }
    }

    // Communication. Query: one Z_q1 element per LWE plus one 32-byte seed for
    // every mask. Response: per packed ciphertext the q3 mask and the q4
    // coefficients that carry answers (all 512 when full).
    const size_t q1_bits = bits_of(R1.Q), q3_bits = bits_of(kQ3);
    const uint64_t query_bytes = packed_bytes(queries_per_path * sh.lwes(), q1_bits) + 32;
    const uint64_t query_bytes_full = queries_per_path * sh.lwes() * (packed_bytes(kN1, q1_bits) + packed_bytes(1, q1_bits));
    uint64_t response_bytes = 0;
    for (size_t g = 0; g < groups; ++g) {
      const size_t live = std::min(kAnswersPerPacked, queries_per_path - g * kAnswersPerPacked);
      response_bytes += packed_bytes(kN2, q3_bits) + packed_bytes(live * kRecordCoeffs, kLogQ4);
    }
    const uint64_t rlwe_q1 = packed_bytes(kN1, q1_bits), rlwe_q3 = packed_bytes(kN2, q3_bits);
    const uint64_t conv_cts = 2 * kLogN1 * kConv.len + kConv.len, rsk_cts = (kN1 / kN2) * kRsk.len;
    const uint64_t offline_seeded = conv_cts * rlwe_q1 + rsk_cts * rlwe_q3 + 32;
    const uint64_t offline_full = 2 * (conv_cts * rlwe_q1 + rsk_cts * rlwe_q3);

    auto mean = [](const std::vector<double> &v) { return std::accumulate(v.begin(), v.end(), 0.0) / v.size(); };
    auto stage_mean = [&](double Stages::*f) {
      double s = 0;
      for (const auto &x : stage_v) s += x.*f;
      return s / stage_v.size();
    };
    const double margin_bits = std::log2(static_cast<double>(kQ4 / kP / 2) / std::max<i64>(worst_error, 1));
    std::ofstream out(a.output);
    out << "{\n  \"schema_version\": \"via_b_profile.v1\",\n  \"scheme\": \"via_b\",\n"
        << "  \"source\": {\"paper\": \"VIA, ePrint 2025/2074 (S&P 2026), Sections 4.1-4.7 and Figure 9\", "
        << "\"implementation\": \"scripts/path_baselines/via_b/via.hpp\"},\n"
        << "  \"compiler_label\": \"" << a.compiler_label << "\",\n"
        << "  \"path_semantics\": \"legacy_merkle_paths_omit_root\",\n"
        << "  \"height\": " << h << ", \"leaf_count\": " << leaf_count << ", \"item_count\": " << items
        << ", \"record_bytes\": " << kRecordBytes << ",\n  \"actual_logical_bytes\": " << items * kRecordBytes << ",\n"
        << "  \"warmups\": " << a.warmups << ", \"measured_trials\": " << a.trials << ", \"trial_seed\": " << a.seed << ",\n"
        << "  \"batch\": {\"queries_per_path\": " << queries_per_path
        << ", \"framework\": \"Figure 9: one VIA-C query per sibling over the whole database, answers repacked\"},\n"
        << "  \"via_parameters\": {\"n1\": " << kN1 << ", \"n2\": " << kN2 << ", \"q1_bits\": " << q1_bits
        << ", \"q2_bits\": " << bits_of(kQ2) << ", \"q3_bits\": " << q3_bits << ", \"q4_bits\": " << kLogQ4
        << ", \"plaintext_modulus\": " << kP << ", \"s1\": \"ternary\", \"sigma_e1\": " << kSigmaE1
        << ", \"sigma_s2\": " << kSigmaS2 << ", \"sigma_e2\": " << kSigmaE2
        << ", \"gadgets\": {\"dmux\": [" << kCtrl.len << ", " << kCtrl.log_base << "], \"cmux_crot\": [" << kSel.len << ", "
        << kSel.log_base << "], \"conversion\": [" << kConv.len << ", " << kConv.log_base << "], \"ring_switch\": [" << kRsk.len
        << ", " << kRsk.log_base << "], \"repack_q2_digits\": " << kRepackLen << "},"
        << " \"record_coefficients\": " << kRecordCoeffs << ", \"records_per_polynomial\": " << kRecordsPerPoly
        << ", \"answers_per_response_ciphertext\": " << kAnswersPerPacked << ",\n"
        << "    \"rows_I\": " << sh.I << ", \"columns_J\": " << sh.J << ", \"log_rows\": " << sh.log_i << ", \"log_columns\": "
        << sh.log_j << ", \"lwe_per_query\": " << sh.lwes() << ", \"response_ciphertexts\": " << groups << "},\n"
        << "  \"setup\": {\"database_build_ms\": " << jnum(bucket_ms) << ", \"client_key_setup_ms\": " << jnum(client_setup_ms)
        << ", \"server_key_setup_ms\": " << jnum(server_key_ms) << ", \"db_encode_ms\": " << jnum(db_setup_ms)
        << ", \"preprocessed_db_bytes\": " << db_bytes << ", \"logical_stored_bytes\": " << total_records * kRecordBytes << "},\n"
        << "  \"communication\": {\"public_parameter_bytes\": " << offline_seeded << ", \"public_parameter_bytes_uncompressed\": "
        << offline_full << ", \"query_bytes_per_path\": " << query_bytes << ", \"query_bytes_per_path_uncompressed\": " << query_bytes_full
        << ", \"response_bytes_per_path\": " << response_bytes
        << ", \"kind\": \"bit-packed; seeded masks count as one 32-byte seed, as the VIA paper reports; the response is the q3 mask plus the q4 coefficients that carry answers\"},\n"
        << "  \"timing_boundaries\": {\"server_total_ms\": \"from the received b values and mask seed to the ring-switched responses: mask expansion, query decompression, DMux, q1->q2 switch, first dimension, CMux, CRot per query, then repacking and response compression\", "
        << "\"client_query_ms\": \"LWE query generation for all queries\", \"client_decode_ms\": \"RespCompRecover of every response ciphertext\"},\n"
        << "  \"samples\": [\n" << samples.str() << "\n  ],\n"
        << "  \"measured_summary\": {\"count\": " << a.trials << ", \"server_ms_mean\": " << jnum(mean(server_v))
        << ", \"client_query_ms_mean\": " << jnum(mean(client_v)) << ", \"client_decode_ms_mean\": " << jnum(mean(decode_v))
        << ", \"stages_ms_mean\": {\"decompress\": " << jnum(stage_mean(&Stages::decompress)) << ", \"dmux\": "
        << jnum(stage_mean(&Stages::dmux)) << ", \"modswitch\": " << jnum(stage_mean(&Stages::modswitch)) << ", \"first_dim\": "
        << jnum(stage_mean(&Stages::first_dim)) << ", \"cmux\": " << jnum(stage_mean(&Stages::cmux)) << ", \"crot\": "
        << jnum(stage_mean(&Stages::crot)) << ", \"repack\": " << jnum(stage_mean(&Stages::repack)) << ", \"respcomp\": "
        << jnum(stage_mean(&Stages::respcomp)) << "}, \"max_error_q4\": " << worst_error << ", \"decoding_margin_bits\": "
        << jnum(margin_bits) << ", \"all_correct\": true}\n}\n";
    std::cerr << "via_b: H=" << h << " mean server " << mean(server_v) << " ms, query " << query_bytes
              << " B, response " << response_bytes << " B, keys " << offline_seeded << " B, max error " << worst_error << "/"
              << kQ4 / kP / 2 << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "via_b: " << e.what() << "\n";
    return 2;
  }
}
