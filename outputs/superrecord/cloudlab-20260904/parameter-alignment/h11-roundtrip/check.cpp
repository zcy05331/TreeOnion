#include "client.h"
#include "server.h"
#include "merkle_baseline.h"

#include <bit>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: check output.json");
    constexpr size_t target = 4096;
    const MerkleWorkload workload{size_t{1} << 18, 18, 32};
    const PirParams reference = PirParams().with_layout({349526, 11, true});
    PirClient client(reference);
    const auto keys = client.create_session_keys(11);
    if (keys->bv_galois_keys.keys.size() != 11)
      throw std::runtime_error("expected eleven expansion keys");

    std::ofstream out(argv[1]);
    out << "{\n  \"scope\": \"local encrypted legality probe, not a timing benchmark\","
        << "\n  \"plaintext_count\": " << target
        << ",\n  \"node_entry_bits\": 256,"
        << "\n  \"reference_height\": 11,"
        << "\n  \"expansion_key_count\": " << keys->bv_galois_keys.keys.size()
        << ",\n  \"checks\": [\n";
    bool first = true;
    for (const size_t height : {size_t{10}, size_t{11}}) {
      const PirParams shape = reference.with_layout({target, height, true});
      const size_t expected_n0 = height == 11 ? 1024 : 512;
      if (shape.get_fst_dim_sz() != expected_n0)
        throw std::runtime_error("probe did not construct the intended N0");
      PirServer server(shape);
      server.set_client_session_keys(client.get_client_id(), keys);
      server.load_data(target, [&](size_t index, RlwePt &pt) {
        pt = make_flat_merkle_plaintext(index, workload, shape);
      });
      for (const size_t index : {size_t{0}, size_t{1023}, size_t{1024}, target - 1}) {
        RlweCt query = client.fast_generate_query(shape, index);
        const RlweCt response = server.make_query(client.get_client_id(), query);
        uint64_t max_noise = 0;
        int budget_bits = 0;
        const RlwePt pt = client.decrypt_mod_q_measured(response, max_noise, budget_bits);
        for (size_t offset = 0; offset < 96; ++offset) {
          const size_t heap = index * 96 + offset + 2;
          const size_t level = std::bit_width(heap) - 1;
          const auto expected = synthetic_merkle_node(level, heap - (size_t{1} << level));
          if (decode_merkle_node(pt, offset, shape) != expected)
            throw std::runtime_error("decoded node differs from source coordinates");
        }
        if (!first) out << ",\n";
        first = false;
        out << "    {\"height\":" << height << ",\"N0\":" << shape.get_fst_dim_sz()
            << ",\"remaining_dims\":" << shape.get_num_other_dims()
            << ",\"plaintext_index\":" << index
            << ",\"nodes_checked\":96,\"max_noise\":" << max_noise
            << ",\"budget_bits\":" << budget_bits << ",\"correct\":true}";
        std::cout << "height=" << height << " N0=" << shape.get_fst_dim_sz()
                  << " plaintext=" << index << " correct; noise=" << max_noise
                  << " budget=" << budget_bits << std::endl;
      }
    }
    out << "\n  ],\n  \"all_correct\": true\n}\n";
    if (!out) throw std::runtime_error("failed to write probe results");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
