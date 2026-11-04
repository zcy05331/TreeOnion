#include "tests.h"

#include <vector>

void test_approx_gadget();
void test_tree_multiplane();
// TimerLogger accumulation, warm-up skipping and cleanup.
void test_logging();
// The complete response plaintext at the paper's first-dimension width.
void test_tree_noise_envelope();
void test_merkle_sha256_e2e();
// RFC 6962 logs of non-power-of-two size served as a virtual perfect-tree view.
void test_merkle_rfc6962_view();
// The seeded query wire format.
void test_query_wire();
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
// its result and finishes in seconds to a few minutes. The informational
// cpu_info and the plan_params search are excluded.
static const std::vector<std::string> kRegressionTests = {
    "barrett", "utils_arith", "noise_sampling", "rlwe_enc", "hexl_ntt",
    "bfv", "decrypt_mod_q", "mod_switch", "bv_ks", "ext_prod",
    "ext_prod_mux", "fst_dim", "db_shape", "runtime_layout", "fast_expand",
    "pir", "pir_profile", "server_loader", "shared_session",
    "tree_index", "tree_query", "tree_select", "tree_rotate", "tree_project",
    "tree_kernel", "tree_multiplane",
    "tree_entry_codec",
    "approx_gadget",
    "tree_packing",
    "logging",
    "tree_noise_envelope", "merkle_sha256_e2e", "merkle_rfc6962_view", "query_wire",
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
  else if (test_name == "tree_noise_envelope") test_tree_noise_envelope();
  else if (test_name == "merkle_sha256_e2e") test_merkle_sha256_e2e();
  else if (test_name == "merkle_rfc6962_view") test_merkle_rfc6962_view();
  else if (test_name == "query_wire")        test_query_wire();
  else if (test_name == "tree_entry_codec")         test_tree_entry_codec();
  else if (test_name == "approx_gadget")            test_approx_gadget();
  else if (test_name == "gsw_decomp_shapes")        test_gsw_decomp_shapes();
  else if (test_name == "tree_packing")             test_tree_packing();
  else if (test_name == "decrypt_mod_q")     test_decrypt_mod_q();
  else if (test_name == "mod_switch")        test_mod_switch();
  else if (test_name == "db_shape")          test_db_shape();
  else if (test_name == "runtime_layout")     test_runtime_layout();
  else if (test_name == "server_loader")       test_server_loader();
  else if (test_name == "shared_session")      test_shared_session();
  else if (test_name == "pir_profile")         test_pir_profile();
  else if (test_name == "pir_row_sweep")       test_pir_row_sweep();
  else if (test_name == "query_shape_exact_fit") test_query_shape_exact_fit();
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
