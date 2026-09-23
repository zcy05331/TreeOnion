#include "tests.h"
#include "bv_keyswitch.h"

#include <algorithm>
#include <limits>

void PirTest::test_pir() {
  print_func_name(__FUNCTION__);
  auto success_count = 0;

  // ============== setting parameters for PIR scheme ==============
  PirParams pir_params;
  pir_params.print_params();
  PirServer server(pir_params); // Initialize the server with the parameters

  // Pre-generate all query indices so gen_data() only records what we need.
  // Deterministic schedule: the first and last plaintext rows are always
  // queried, the rest come from a fixed seed so a failure can be replayed.
  ChaChaRng rng(0x6f6e696f6e706972ULL);
  const size_t num_pt = pir_params.get_num_pt();
  std::vector<size_t> query_indices(num_experiments);
  for (size_t i = 0; i < num_experiments; i++) {
    query_indices[i] = rng() % num_pt;
  }
  if (num_experiments >= 1) query_indices[0] = 0;
  if (num_experiments >= 2) query_indices[1] = num_pt - 1;

  BENCH_PRINT("Initializing server...");
  server.gen_data(query_indices);
  BENCH_PRINT("Server initialized");

  // some global results
  size_t galois_key_size = 0;
  size_t query_size = 0;
  size_t resp_size = 0;

  // Response noise margin. At the paper point (CONFIG_N2048_K1_COMP with the
  // default gadgets) the flat pipeline leaves only 1-2 bits of budget, so a
  // regression that inflates the noise (wrong base_log2 rounding, a lost
  // centering in the rescale, an extra expansion level) would pass the decode
  // check until it reaches ~3x. Pin one bit of budget, i.e. max_noise <=
  // small_q / (4t) (254 here), as the early canary. The measured response
  // noise is 104-193 (mean 135, sd 15) over 200 experiments in 20 processes,
  // so the floor sits ~7.7 sd above the mean of a per-experiment maximum over
  // 2048 coefficients; extrapolating that tail leaves a spurious failure
  // below 1e-4 per suite run. Other configs and the gadget-campaign builds
  // (PIR_EP_ROWS, PIR_EP_LOW_BITS, PIR_KS_ROWS, PIR_KS_LOW_BITS) have their
  // own margins and only report the measurement; the decode check above them
  // stays, which is why this is a conditional pin and not require_applicable.
  const bool pin_noise_margin =
      paper_config_active() && DBConsts::L_EP == DBConsts::DefaultL_EP &&
      DBConsts::EP_LOW_BITS == 0 && DBConsts::L_KS == DBConsts::DefaultL_KS &&
      DBConsts::KS_LOW_BITS == 0;
  int min_budget_bits = std::numeric_limits<int>::max();
  uint64_t worst_noise = 0;
  size_t worst_noise_idx = 0;

  // Run the query process many times.
  for (size_t i = 0; i < num_experiments; i++) {
    BENCH_PRINT("======================== Experiment " << i + 1 << " ========================");

    // ============= OFFLINE PHASE: key materials ==============
    // Initialize the client
    PirClient client(pir_params);
    const size_t client_id = client.get_client_id();
    std::stringstream resp_stream;

    // Client create BV galois keys and gsw(sk)
    auto bv_galois_keys = client.create_bv_galois_keys();
    galois_key_size = pir_params.get_bv_galois_key_size();
    //--------------------------------------------------------------------------------
    // Server receives the BV galois keys and gsw keys
    server.set_client_bv_galois_key(client_id, std::move(bv_galois_keys));
    server.set_client_gsw_key(client_id, client.generate_gsw_from_key());

    // ===================== ONLINE PHASE =====================
    size_t query_pt_idx = query_indices[i];

    TIME_START(CLIENT_TOT_TIME);
    RlweCt query = client.fast_generate_query(query_pt_idx);
    TIME_END(CLIENT_TOT_TIME);
    query_size = pir_params.get_BFV_size();

    TIME_START(SERVER_TOT_TIME);
    RlweCt response = server.make_query(client_id, query);
    TIME_END(SERVER_TOT_TIME);

    // ---------- server send the response to the client -----------
    resp_size = server.save_resp_to_stream(response, resp_stream);

    // ============= CLIENT ===============
    // client gets result from the server and decrypts it, measuring the
    // largest centered residue of the response on the way.
    RlweCt reconstructed_result = client.load_resp_from_stream(resp_stream);
    uint64_t max_noise = 0;
    int budget_bits = 0;
    TIME_START(CLIENT_TOT_TIME);
    RlwePt decrypted_result = client.decrypt_mod_q_measured(
        reconstructed_result, max_noise, budget_bits);
    TIME_END(CLIENT_TOT_TIME);
    BENCH_PRINT("Noise budget after decryption: " << budget_bits
                << " (max noise: " << max_noise << ")");
    if (max_noise > worst_noise) {
      worst_noise = max_noise;
      worst_noise_idx = query_pt_idx;
    }
    min_budget_bits = std::min(min_budget_bits, budget_bits);

    // ============= Directly get the plaintext from server. Not part of PIR.
    RlwePt actual_plaintext = server.direct_get_original_plaintext(query_pt_idx);

    END_EXPERIMENT();
    // ============= PRINTING RESULTS ===============
    // DEBUG_PRINT("\t\tquery / resp / actual idx:\t" << query_pt_idx << " / " << resp_plaintext_idx << " / " << actual_plaintext_idx);

    if (utils::plaintext_is_equal(decrypted_result, actual_plaintext)) {
      // print a green success message
      std::cout << color_green() << "Success!" << color_reset() << std::endl;
      success_count++;
    } else {
      // print a red failure message
      std::cout << color_red() << "Failure!" << color_reset() << std::endl;
      std::cout << "Query index:\t" << query_pt_idx << std::endl;
      std::cout << "PIR Result:\t";
      utils::print_plaintext(decrypted_result, 20);
      std::cout << "Actual Plaintext:\t";
      utils::print_plaintext(actual_plaintext, 20);
    }
  }

  double avg_server_time = GET_AVG_TIME(SERVER_TOT_TIME);
  double throughput = pir_params.get_DBSize_MB() / (avg_server_time / 1000);

  require_test(success_count == num_experiments,
               "standard OnionPIR correctness mismatch");
  if (pin_noise_margin) {
    require_test(
        min_budget_bits >= 1,
        "response noise margin fell below one bit at the paper point: max "
        "noise " + std::to_string(worst_noise) + " (query index " +
            std::to_string(worst_noise_idx) + ") exceeds small_q/(4t) = " +
            std::to_string(pir_params.get_small_q() /
                           (4 * pir_params.get_plain_mod())));
  }

  // ============= PRINTING FINAL RESULTS ===============]
  PRINT_BAR;
  BENCH_PRINT("                                FINAL RESULTS")
  PRINT_BAR;
  BENCH_PRINT("Success rate: " << success_count << "/" << num_experiments);
  if (num_experiments > 0) {
    BENCH_PRINT("Response noise: max " << worst_noise << " (decode limit small_q/(2t) = "
                << pir_params.get_small_q() / (2 * pir_params.get_plain_mod())
                << "), min budget " << min_budget_bits << " bits"
                << (pin_noise_margin ? ", one-bit floor enforced" : ""));
  }
  BENCH_PRINT("BV galois key size: " << static_cast<double>(galois_key_size) / 1024 << " KB");
  // BENCH_PRINT("gsw key size: " << gsw_key_size << " bytes");
  BENCH_PRINT("gsw key size: " << pir_params.get_gsw_key_size() << " bytes = " << static_cast<double>(pir_params.get_gsw_key_size()) / 1024 << " KB");
  BENCH_PRINT("total key size: " << static_cast<double>(galois_key_size + pir_params.get_gsw_key_size()) / 1024 << "KB");
  BENCH_PRINT("query size: " << query_size << " bytes = " << static_cast<double>(query_size) / 1024 << " KB");
  BENCH_PRINT("response size: " << resp_size << " bytes = " << static_cast<double>(resp_size) / 1024 << " KB");

  PRETTY_PRINT();
  BENCH_PRINT("Server throughput: " << throughput << " MB/s");
}
