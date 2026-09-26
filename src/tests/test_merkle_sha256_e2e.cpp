#include "tests.h"
#include "tree_multiplane.h"
#include "tree_packing.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

void test_merkle_sha256_e2e();
void test_merkle_rfc6962_view();

namespace {

using Hash = std::array<uint8_t, 32>;

// FIPS 180-4 SHA-256, enough to build a real Merkle tree inside the test.
Hash sha256(const uint8_t *data, size_t len) {
  static constexpr uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::vector<uint8_t> msg(data, data + len);
  msg.push_back(0x80);
  while (msg.size() % 64 != 56) msg.push_back(0);
  const uint64_t bit_len = static_cast<uint64_t>(len) * 8;
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bit_len >> (8 * i)));
  const auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  for (size_t block = 0; block < msg.size(); block += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t{msg[block + 4 * i]} << 24) | (uint32_t{msg[block + 4 * i + 1]} << 16) |
             (uint32_t{msg[block + 4 * i + 2]} << 8) | uint32_t{msg[block + 4 * i + 3]};
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
      const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  Hash out{};
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
  return out;
}

Hash hash_pair(const Hash &left, const Hash &right) {
  uint8_t buf[64];
  std::memcpy(buf, left.data(), 32);
  std::memcpy(buf + 32, right.data(), 32);
  return sha256(buf, sizeof(buf));
}

std::string hex(const Hash &h) {
  static const char *digits = "0123456789abcdef";
  std::string s;
  for (uint8_t byte : h) { s.push_back(digits[byte >> 4]); s.push_back(digits[byte & 15]); }
  return s;
}

// The client's check: fold the leaf with the decoded siblings up to the root,
// ordering each pair by the bit of the current node index.
Hash root_from_proof(const Hash &leaf_hash, size_t leaf, size_t L,
                     const std::vector<Hash> &decoded_path) {
  Hash h = leaf_hash;
  for (size_t level = L; level >= 1; --level) {
    const size_t node = leaf >> (L - level);
    h = (node & 1) ? hash_pair(decoded_path[level], h) : hash_pair(h, decoded_path[level]);
  }
  return h;
}

// RFC 6962 hashing: leaves hash 0x00 || data, inner nodes 0x01 || left || right.
Hash rfc6962_leaf(size_t index) {
  uint8_t buf[13] = {0x00, 'l', 'e', 'a', 'f'};
  for (int b = 0; b < 8; ++b) buf[5 + b] = static_cast<uint8_t>(index >> (8 * b));
  return sha256(buf, sizeof(buf));
}

Hash rfc6962_node(const Hash &left, const Hash &right) {
  uint8_t buf[65] = {0x01};
  std::memcpy(buf + 1, left.data(), 32);
  std::memcpy(buf + 33, right.data(), 32);
  return sha256(buf, sizeof(buf));
}

// RFC 6962 tree hash of leaves [lo, hi), split at the largest power of two
// below the size, computed directly from the definition.
Hash rfc6962_root(size_t lo, size_t hi) {
  if (hi - lo == 1) return rfc6962_leaf(lo);
  size_t k = 1;
  while (2 * k < hi - lo) k *= 2;
  return rfc6962_node(rfc6962_root(lo, lo + k), rfc6962_root(lo + k, hi));
}

// Inclusion-proof verification of RFC 9162, Section 2.1.3.2 (the same tree
// hash as RFC 6962), independent of how the view builds the proof.
bool verify_inclusion(size_t leaf, size_t tree_size, Hash r, const std::vector<Hash> &proof,
                      const Hash &root) {
  if (leaf >= tree_size) return false;
  size_t fn = leaf, sn = tree_size - 1;
  for (const Hash &p : proof) {
    if (sn == 0) return false;
    if ((fn & 1) || fn == sn) {
      r = rfc6962_node(p, r);
      while (!(fn & 1) && fn != 0) { fn >>= 1; sn >>= 1; }
    } else {
      r = rfc6962_node(r, p);
    }
    fn >>= 1;
    sn >>= 1;
  }
  return sn == 0 && r == root;
}

}  // namespace

// Encrypted retrieval of real SHA-256 Merkle proofs through the evaluated
// pipeline: an ordinary tree, its sibling-swapped view as the entry source, the
// g = 8, P = 3 layout, one packed query, the MLWE merge, the small-q response,
// and the client's recomputation of the trusted root. The query and the
// response cross the wire as bytes: the server answers the query it loads
// from (seed, c0), and the client decodes the response it loads from the
// response codec. L = 12 and N0 = 4 give shallow (R = 1), coarsened-alpha
// (1 < R < N0), and beta-fold (R >= N0) levels. Several registrations use
// fresh keys.
void test_merkle_sha256_e2e() {
  print_func_name(__FUNCTION__);
  const uint8_t abc[3] = {'a', 'b', 'c'};
  require_test(hex(sha256(abc, 3)) ==
                   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "SHA-256 known-answer test");
  require_applicable(paper_config_active(),
                     "the SHA-256 Merkle fixture is pinned to CONFIG_N2048_K1_COMP "
                     "(n = 2048, g = 8 gives rho = 256 and b = 2 at L = 12)");

  PirParams scheme;
  const size_t key_height = std::bit_width(DBConsts::PolyDegree) - 1;
  constexpr size_t L = 12, a = 2, g = 8, entry_bits = 256;
  const TreePirParams tree = make_tree_pir_params_for_scheme(L, a, g, scheme, key_height);
  const tree_multiplane::Params params = tree_multiplane::make_params(tree, entry_bits);
  require_test(tree.N0 == 4 && tree.b == 2 && params.codec.plane_count == 3,
               "fixture must use N0 = 4, b = 2, and three planes");

  // Ordinary Merkle tree: leaves hash a label and the leaf index; inner nodes
  // hash the concatenation of their children.
  std::vector<std::vector<Hash>> M(L + 1);
  M[L].resize(tree.N);
  for (size_t i = 0; i < tree.N; ++i) {
    uint8_t buf[12] = {'l', 'e', 'a', 'f'};
    for (int b = 0; b < 8; ++b) buf[4 + b] = static_cast<uint8_t>(i >> (8 * b));
    M[L][i] = sha256(buf, sizeof(buf));
  }
  for (size_t level = L; level-- > 0;) {
    M[level].resize(size_t{1} << level);
    for (size_t j = 0; j < M[level].size(); ++j)
      M[level][j] = hash_pair(M[level + 1][2 * j], M[level + 1][2 * j + 1]);
  }
  const Hash trusted_root = M[0][0];

  // Public sibling-swapped view: V[0][0] is the root, V[l][j] = M[l][j ^ 1].
  const tree_multiplane::EntrySource source = [&](size_t level, size_t node) {
    const Hash &h = level == 0 ? M[0][0] : M[level][node ^ 1];
    return tree_entry_codec::entry_from_bytes(h, entry_bits);
  };
  const tree_multiplane::Database db = tree_multiplane::preprocess(params, source, scheme);

  const std::vector<size_t> leaves = {0, tree.N - 1, 0x555, 0xAAA, 0x0F0F % tree.N,
                                      0x2BADBEEFULL % tree.N, 0x51A7E5EEDULL % tree.N,
                                      tree.N / 2};
  size_t verified = 0, query_bytes = 0, response_bytes = 0;
  for (size_t registration = 0; registration < 3; ++registration) {
    PirClient client(scheme);
    auto keys = client.create_session_keys(key_height);
    PirServer server(tree_query_expansion_params(tree, scheme));
    server.set_client_session_keys(client.get_client_id(), keys);
    for (size_t leaf : leaves) {
      QuerySeed seed{};
      const RlweCt query = make_tree_query(client, scheme, tree, leaf, &seed);
      std::stringstream query_wire;
      query_bytes = save_seeded_query(query, seed, scheme, query_wire);
      RlweCt received = load_seeded_query(query_wire, scheme);
      ExpandedTreeQuery expanded =
          unpack_tree_query(server, scheme, tree, client.get_client_id(), received);
      tree_multiplane::Response response = tree_multiplane::answer(
          db, expanded, server, client.get_client_id(), params, scheme,
          tree_packing::ResponsePacking::MlweMerge);
      require_test(response.small_q, "response must be switched to the small modulus");
      require_test(response.chunks.size() == 1, "the whole path fits one response ciphertext");
      std::stringstream response_wire;
      response_bytes = server.save_resp_to_stream(response.chunks[0], response_wire);
      response.chunks[0] = client.load_resp_from_stream(response_wire);
      const auto path = tree_multiplane::extract(response, client, params);
      require_test(path.size() == L + 1, "decoded path length");
      std::vector<Hash> decoded(L + 1);
      for (size_t level = 0; level <= L; ++level) {
        require_test(path[level].bytes.size() == 32, "decoded node width");
        std::memcpy(decoded[level].data(), path[level].bytes.data(), 32);
      }
      require_test(decoded[0] == trusted_root, "returned root equals the trusted root");
      require_test(root_from_proof(M[L][leaf], leaf, L, decoded) == trusted_root,
                   "decoded siblings recompute the trusted root");
      auto tampered = decoded;
      tampered[L / 2][0] ^= 1;
      require_test(root_from_proof(M[L][leaf], leaf, L, tampered) != trusted_root,
                   "a flipped sibling bit must fail verification");
      ++verified;
    }
  }
  BENCH_PRINT("SHA-256 Merkle e2e: L=" << L << " N0=" << tree.N0 << " g=" << g
              << " P=" << params.codec.plane_count << ": " << verified
              << " proofs over 3 registrations verified against root "
              << hex(trusted_root).substr(0, 16) << "..., query " << query_bytes
              << " B and response " << response_bytes << " B on the wire");
}

// A log whose size is not a power of two, served through the evaluated
// pipeline as a virtual view of the perfect tree over 2^L leaves: the node of
// an aligned range holds the RFC 6962 hash of the range's leaves below the log
// size (its left child's value when the right half is empty) and zero when the
// range is empty. The client drops the siblings with empty ranges, and RFC
// 9162's verification algorithm must accept the rest against the log's root.
void test_merkle_rfc6962_view() {
  print_func_name(__FUNCTION__);
  require_applicable(paper_config_active(),
                     "the RFC 6962 view fixture is pinned to CONFIG_N2048_K1_COMP "
                     "(n = 2048, g = 8 gives rho = 256 and b = 2 at L = 12)");
  PirParams scheme;
  const size_t key_height = std::bit_width(DBConsts::PolyDegree) - 1;
  constexpr size_t L = 12, a = 2, g = 8, entry_bits = 256;
  const TreePirParams tree = make_tree_pir_params_for_scheme(L, a, g, scheme, key_height);
  const tree_multiplane::Params params = tree_multiplane::make_params(tree, entry_bits);
  const Hash zero{};

  size_t verified = 0;
  for (const size_t log_size : {tree.N - 1, tree.N / 2 + 1}) {
    std::vector<std::vector<Hash>> V(L + 1);
    std::vector<std::vector<bool>> present(L + 1);
    V[L].assign(tree.N, zero);
    present[L].assign(tree.N, false);
    for (size_t j = 0; j < log_size; ++j) {
      V[L][j] = rfc6962_leaf(j);
      present[L][j] = true;
    }
    for (size_t level = L; level-- > 0;) {
      V[level].assign(size_t{1} << level, zero);
      present[level].assign(size_t{1} << level, false);
      for (size_t j = 0; j < V[level].size(); ++j) {
        if (present[level + 1][2 * j + 1]) {
          V[level][j] = rfc6962_node(V[level + 1][2 * j], V[level + 1][2 * j + 1]);
          present[level][j] = true;
        } else if (present[level + 1][2 * j]) {
          V[level][j] = V[level + 1][2 * j];
          present[level][j] = true;
        }
      }
    }
    const Hash root = rfc6962_root(0, log_size);
    require_test(V[0][0] == root, "the virtual root equals the RFC 6962 tree hash");

    const tree_multiplane::EntrySource source = [&](size_t level, size_t node) {
      const Hash &h = level == 0 ? V[0][0] : V[level][node ^ 1];
      return tree_entry_codec::entry_from_bytes(h, entry_bits);
    };
    const tree_multiplane::Database db = tree_multiplane::preprocess(params, source, scheme);
    PirClient client(scheme);
    auto keys = client.create_session_keys(key_height);
    PirServer server(tree_query_expansion_params(tree, scheme));
    server.set_client_session_keys(client.get_client_id(), keys);
    for (const size_t leaf : {size_t{0}, log_size - 1, log_size / 2, size_t{0x2BADBEEF} % log_size}) {
      RlweCt query = make_tree_query(client, scheme, tree, leaf);
      ExpandedTreeQuery expanded =
          unpack_tree_query(server, scheme, tree, client.get_client_id(), query);
      const auto path = tree_multiplane::extract(
          tree_multiplane::answer(db, expanded, server, client.get_client_id(), params, scheme,
                                  tree_packing::ResponsePacking::MlweMerge),
          client, params);
      std::vector<Hash> proof;
      Hash decoded_root{};
      std::memcpy(decoded_root.data(), path[0].bytes.data(), 32);
      require_test(decoded_root == root, "returned root equals the log's root");
      for (size_t level = L; level >= 1; --level) {
        const size_t sibling = (leaf >> (L - level)) ^ 1;
        Hash h{};
        std::memcpy(h.data(), path[level].bytes.data(), 32);
        if ((sibling << (L - level)) < log_size) {
          proof.push_back(h);
        } else {
          require_test(h == zero, "an empty sibling range decodes to zero");
        }
      }
      require_test(verify_inclusion(leaf, log_size, rfc6962_leaf(leaf), proof, root),
                   "RFC 9162 verification accepts the proof without empty siblings");
      proof.front()[0] ^= 1;
      require_test(!verify_inclusion(leaf, log_size, rfc6962_leaf(leaf), proof, root),
                   "a flipped proof bit must fail verification");
      ++verified;
    }
  }
  BENCH_PRINT("RFC 6962 virtual view: " << verified << " proofs from logs of "
              << tree.N - 1 << " and " << tree.N / 2 + 1 << " leaves verified");
}
