#include "tests.h"

#include <vector>

void test_approx_gadget();
void test_tree_multiplane();
// TimerLogger accumulation, warm-up skipping and cleanup.
void test_logging();
// Merkle baselines: a profiled plan off the legacy heights, the phase
// clocks, the archived leaf schedule, one-sample statistics, large tiers.
void test_merkle_profiled_override();
void test_merkle_layerwise_merged();
void test_merkle_layerwise_packed();
void test_merkle_server_phases();
void test_merkle_trial_schedule();
void test_merkle_single_sample_stats();
void test_layer_layout_sweep_dominated();
void test_layer_layout_large_tiers();
// The complete response plaintext at the paper's first-dimension width.
void test_tree_noise_envelope();
void test_merkle_sha256_e2e();
// PIR layouts: every row of a ragged and a perfect remaining-dimension
// tree, and a query shape that fills the expansion capacity exactly.
void test_pir_row_sweep();
void test_query_shape_exact_fit();
// Crypto primitives: the vectorized loops against the scalar primitives
// they mirror, and the generated moduli.
void test_bv_keyswitch_reference();
void test_gsw_decomp_shapes();
void test_ntt_friendly_primes();
void test_paper_moduli_pin();

// Correctness suite behind `--test all`: every registered test that asserts
// its result and finishes in seconds to a few minutes. Timed benchmarks,
// the informational cpu_info and the plan_params search are excluded;
// merkle_benchmarks and layer_layout_sweep are main.cpp entry points, not
// PirTest members.
static const std::vector<std::string> kRegressionTests = {
    "barrett", "utils_arith", "noise_sampling", "rlwe_enc", "hexl_ntt",
    "bfv", "decrypt_mod_q", "mod_switch", "bv_ks", "ext_prod",
    "ext_prod_mux", "fst_dim", "db_shape", "runtime_layout", "fast_expand",
    "pir", "pir_profile", "server_loader", "shared_session",
    "tree_index", "tree_query", "tree_select", "tree_rotate", "tree_project",
    "tree_kernel", "tree_multiplane",
    "merkle_baseline", "merkle_benchmark_stats", "layer_layout_planner",
    "merkle_integration", "cuckoo_batch",
    "tree_entry_codec",
    "approx_gadget",
    "tree_packing",
    "logging",
    "merkle_profiled_override", "merkle_layerwise_merged", "merkle_layerwise_packed",
    "merkle_server_phases", "merkle_trial_schedule",
    "merkle_single_sample_stats", "layer_layout_sweep_dominated",
    "layer_layout_large_tiers",
    "tree_noise_envelope", "merkle_sha256_e2e",
    "pir_row_sweep", "query_shape_exact_fit",
    "bv_ks_reference", "gsw_decomp_shapes", "ntt_primes", "paper_moduli_pin",
};

void PirTest::run_test(const std::string &test_name) {
  if (test_name == "all") {
    std::vector<std::string> failures, not_applicable;
    for (const std::string &name : kRegressionTests) {
      try {
        run_test(name);
      } catch (const TestNotApplicable &why) {
        std::cout << "NOT APPLICABLE " << name << ": " << why.what() << '\n';
        not_applicable.push_back(name + ": " + why.what());
      } catch (const std::exception &error) {
        std::cerr << "FAILED " << name << ": " << error.what() << '\n';
        failures.push_back(name + ": " + error.what());
      }
    }
    const size_t applicable = kRegressionTests.size() - not_applicable.size();
    std::cout << "Regression suite: " << applicable - failures.size() << "/"
              << applicable << " applicable tests passed";
    if (!not_applicable.empty()) {
      std::cout << ", " << not_applicable.size()
                << " not applicable to this build:";
      for (const std::string &n : not_applicable) std::cout << "\n  " << n;
    }
    std::cout << '\n';
    if (!failures.empty()) {
      std::string message = "regression failures:";
      for (const std::string &f : failures) message += "\n  " + f;
      throw std::runtime_error(message);
    }
    return;
  }
  std::cout << "Running test: " << test_name << std::endl;

  if (test_name == "pir")                    test_pir();
  else if (test_name == "bfv")               bfv_example();
  else if (test_name == "ext_prod")          test_external_product();
  else if (test_name == "ext_prod_mux")      test_ext_prod_mux();
  else if (test_name == "fst_dim")           test_fst_dim_mult();
  else if (test_name == "fast_expand")       test_fast_expand_query();
  else if (test_name == "tree_index")        test_tree_index();
  else if (test_name == "tree_query")        test_tree_query();
  else if (test_name == "tree_select")       test_tree_select();
  else if (test_name == "tree_rotate")       test_tree_rotate();
  else if (test_name == "tree_project")      test_tree_project();
  else if (test_name == "tree_kernel")       test_tree_kernel();
  else if (test_name == "tree_multiplane")   test_tree_multiplane();
  else if (test_name == "tree_multiplane_bench") test_tree_multiplane_benchmark();
  else if (test_name == "tree_noise_envelope") test_tree_noise_envelope();
  else if (test_name == "merkle_sha256_e2e") test_merkle_sha256_e2e();
  else if (test_name == "cuckoo_batch")      test_cuckoo_batch();
  else if (test_name == "cuckoo_bench")      test_cuckoo_benchmark();
  else if (test_name == "tree_entry_codec")         test_tree_entry_codec();
  else if (test_name == "approx_gadget")            test_approx_gadget();
  else if (test_name == "gsw_decomp_shapes")        test_gsw_decomp_shapes();
  else if (test_name == "tree_packing")             test_tree_packing();
  else if (test_name == "decrypt_mod_q")     test_decrypt_mod_q();
  else if (test_name == "mod_switch")        test_mod_switch();
  else if (test_name == "db_shape")          test_db_shape();
  else if (test_name == "runtime_layout")     test_runtime_layout();
  else if (test_name == "merkle_baseline")    test_merkle_baseline();
  else if (test_name == "server_loader")       test_server_loader();
  else if (test_name == "shared_session")      test_shared_session();
  else if (test_name == "merkle_benchmark_stats") test_merkle_benchmark_stats();
  else if (test_name == "merkle_integration")  test_merkle_integration();
  else if (test_name == "merkle_profiled_override") test_merkle_profiled_override();
  else if (test_name == "merkle_layerwise_merged") test_merkle_layerwise_merged();
  else if (test_name == "merkle_layerwise_packed") test_merkle_layerwise_packed();
  else if (test_name == "merkle_server_phases") test_merkle_server_phases();
  else if (test_name == "merkle_trial_schedule") test_merkle_trial_schedule();
  else if (test_name == "merkle_single_sample_stats") test_merkle_single_sample_stats();
  else if (test_name == "pir_profile")         test_pir_profile();
  else if (test_name == "pir_row_sweep")       test_pir_row_sweep();
  else if (test_name == "query_shape_exact_fit") test_query_shape_exact_fit();
  else if (test_name == "layer_layout_planner") test_layer_layout_planner();
  else if (test_name == "layer_layout_sweep_dominated") test_layer_layout_sweep_dominated();
  else if (test_name == "layer_layout_large_tiers") test_layer_layout_large_tiers();
  else if (test_name == "bv_ks")             test_bv_keyswitch();
  else if (test_name == "bv_ks_reference")   test_bv_keyswitch_reference();
  else if (test_name == "cpu_info")          print_cpu_info();
  else if (test_name == "hexl_ntt")          test_hexl_ntt();
  else if (test_name == "utils_arith")       test_utils_arith();
  else if (test_name == "logging")           test_logging();
  else if (test_name == "ntt_primes")        test_ntt_friendly_primes();
  else if (test_name == "paper_moduli_pin")  test_paper_moduli_pin();
  else if (test_name == "noise_sampling")    test_noise_sampling();
  else if (test_name == "rlwe_enc")          test_rlwe_enc();
  else if (test_name == "barrett")           test_barrett();
  else if (test_name == "plan_params")       plan_params();
  else {
    throw std::invalid_argument("Unknown test: " + test_name);
  }
}
