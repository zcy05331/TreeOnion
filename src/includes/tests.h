#pragma once

#include <string>

// Common includes used by most tests
#include "gsw.h"
#include "pir.h"
#include "server.h"
#include "client.h"
#include "utils.h"
#include "logging.h"
#include "matrix.h"

#include <bit>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <bitset>
#include <optional>
#include <stdexcept>

inline void require_test(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// A test whose precondition this build does not meet: a single-limb-only
// feature, or the paper parameter point its frozen constants are defined for.
// `--test all` counts these separately from failures; a direct run still
// exits non-zero with the reason.
struct TestNotApplicable : std::runtime_error {
  using std::runtime_error::runtime_error;
};
inline void require_applicable(bool condition, const std::string &reason) {
  if (!condition) {
    throw TestNotApplicable(reason);
  }
}
// Frozen sizes, moduli and fixtures are defined for the paper parameter point
// CONFIG_N2048_K1_COMP (2048 x 12-bit plaintexts).
inline bool paper_config_active() {
  return ACTIVE_CONFIG == CONFIG_N2048_K1_COMP;
}
inline std::string paper_config_reason(const char *what) {
  return std::string(what) +
         " is defined for CONFIG_N2048_K1_COMP (frozen constants); this "
         "build uses another parameter point";
}
// Tree-test shapes are (a, b) pairs: the height with `b` beta bits for
// first-dimension bits `a` and `g` slots per node is L = log2(n / g) + a + b,
// which keeps every fixture legal and meaningful under any ring size.
inline size_t tree_height_for(size_t a, size_t b, size_t g = 1) {
  return static_cast<size_t>(std::bit_width(DBConsts::PolyDegree / g) - 1) +
         a + b;
}

inline void print_throughput(const std::string &name, const size_t db_size) {
  double avg_time = GET_AVG_TIME(name);
  double throughput = db_size / (avg_time * 1000);
  BENCH_PRINT(name << ": " << throughput << " MB/s");
}

class PirTest {
  public:
    size_t num_experiments = 10;

    void run_test(const std::string &test_name);

    // ! the main test for PIR
    void test_pir();

    // ======================== BFV & GSW tests ========================
    void bfv_example();
    void test_external_product();
    void test_decrypt_mod_q();
    void test_ext_prod_mux();

    // ======================== Matrix tests ========================
    // simulation of the first dimension multiplication
    void test_fst_dim_mult();

    // ======================== System Information ========================
    void print_cpu_info();

    // ======================== Other tests ========================
    void test_fast_expand_query();
    // ======================== Tree PIR tests ========================
    // (gamma, alpha, beta) index math and public level plans (pure arithmetic).
    void test_tree_index();
    // Packed tree query -> ExpandBFV -> order/scale -> RGSW -> isolated CMux.
    void test_tree_query();
    // Milestone 2: level packing, alpha pyramid, SelectLevel == D_l[p_l].
    void test_tree_select();
    // Milestone 3: MulXPow oracle, RotSelect, PrivateRotateLevel alignment.
    void test_tree_rotate();
    // Milestone 3: projection map for every depth plus the rotate+project gate.
    void test_tree_project();
    void test_tree_packing();
    // Milestone 6: scalar vs NTT first-dimension kernels, bit-for-bit.
    void test_tree_kernel();
    // h=1 entry codec: bitstream packing, zero padding and layout policy.
    void test_tree_entry_codec();
    void test_mod_switch();
    void test_db_shape();
    void test_runtime_layout();
    void test_server_loader();
    void test_shared_session();
    // Stage-profiled make_query is bit-for-bit the plain pipeline.
    void test_pir_profile();
    void test_bv_keyswitch();
    void test_hexl_ntt();
    void test_utils_arith();
    void test_noise_sampling();
    void test_rlwe_enc();
    void test_barrett();

    // Search (tree_height, num_queries, num_other_dims) for minimum total
    // communication in each (QueryMode, GswSource) combination, given the
    // current config's num_pt, L_EP, L_KEY and BFV ciphertext size.
    void plan_params();
};
