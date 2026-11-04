#include "tests.h"

void PirTest::test_db_shape() {
  print_func_name(__FUNCTION__);
  auto [fst1, nd1] = utils::calculate_db_shape(1000000, 5, 9);
  BENCH_PRINT("fst_dim_sz: " << fst1 << ", num_dims: " << nd1);
  require_test(fst1 == 256 && nd1 == 13, "h=9 shape");
  auto [fst2, nd2] = utils::calculate_db_shape(1000000, 6, 8);
  BENCH_PRINT("fst_dim_sz: " << fst2 << ", num_dims: " << nd2);
  require_test(fst2 == 128 && nd2 == 14, "h=8 shape");
  auto [fst3, nd3] = utils::calculate_db_shape(1000, 5, 9, false);
  require_test(fst3 == 507 && nd3 == 2, "non-power-of-two first dimension");
  auto [fst4, nd4] = utils::calculate_db_shape(512, 5, 9);
  require_test(fst4 == 512 && nd4 == 1, "single-dimension shape");
  bool rejected = false;
  try {
    (void)utils::calculate_db_shape(0, 5, 9);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require_test(rejected, "accepted a zero target");
}
