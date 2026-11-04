#include "tests.h"
#include "bv_keyswitch.h"

#include <algorithm>
#include <limits>

void PirTest::test_pir() {
  print_func_name(__FUNCTION__);
  auto success_count = 0;

  PirParams pir_params;
  pir_params.print_params();
  PirServer server(pir_params);

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

  size_t galois_key_size = 0;
  size_t query_size = 0;
  size_t resp_size = 0;

  const bool pin_noise_margin =
      paper_config_active() && DBConsts::L_EP == DBConsts::DefaultL_EP &&
      DBConsts::EP_LOW_BITS == 0 && DBConsts::L_KS == DBConsts::DefaultL_KS &&
      DBConsts::KS_LOW_BITS == 0;
  int min_budget_bits = std::numeric_limits<int>::max();
  uint64_t worst_noise = 0;
  size_t worst_noise_idx = 0;

  for (size_t i = 0; i < num_experiments; i++) {
    BENCH_PRINT("======================== Experiment " << i + 1 << " ========================");

    PirClient client(pir_params);
    const size_t client_id = client.get_client_id();
    std::stringstream resp_stream;

    auto bv_galois_keys = client.create_bv_galois_keys();
    galois_key_size = pir_params.get_bv_galois_key_size();
    server.set_client_bv_galois_key(client_id, std::move(bv_galois_keys));
    server.set_client_gsw_key(client_id, client.generate_gsw_from_key());

    size_t query_pt_idx = query_indices[i];

    TIME_START(CLIENT_TOT_TIME);
    RlweCt query = client.fast_generate_query(query_pt_idx);
    TIME_END(CLIENT_TOT_TIME);
    query_size = pir_params.get_BFV_size();

    TIME_START(SERVER_TOT_TIME);
    RlweCt response = server.make_query(client_id, query);
    TIME_END(SERVER_TOT_TIME);

    resp_size = server.save_resp_to_stream(response, resp_stream);

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

    RlwePt actual_plaintext = server.direct_get_original_plaintext(query_pt_idx);

    END_EXPERIMENT();

    if (utils::plaintext_is_equal(decrypted_result, actual_plaintext)) {
      std::cout << color_green() << "Success!" << color_reset() << std::endl;
      success_count++;
    } else {
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
  BENCH_PRINT("gsw key size: " << pir_params.get_gsw_key_size() << " bytes = " << static_cast<double>(pir_params.get_gsw_key_size()) / 1024 << " KB");
  BENCH_PRINT("total key size: " << static_cast<double>(galois_key_size + pir_params.get_gsw_key_size()) / 1024 << "KB");
  BENCH_PRINT("query size: " << query_size << " bytes = " << static_cast<double>(query_size) / 1024 << " KB");
  BENCH_PRINT("response size: " << resp_size << " bytes = " << static_cast<double>(resp_size) / 1024 << " KB");

  PRETTY_PRINT();
  BENCH_PRINT("Server throughput: " << throughput << " MB/s");
}
