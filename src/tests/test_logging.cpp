// TimerLogger semantics that every benchmark figure is built on. The Merkle
// ServerPhaseAccumulator (merkle_benchmark.cpp) and the tree benchmarks'
// stages_ms read GET_LAST_TIME after END_EXPERIMENT and rely on repeated
// sections being summed within one experiment; the pir averages rely on
// getAvgTime skipping exactly WARMUP_ITERATIONS records. Nothing else
// asserts these values: the benchmarks only copy them into JSON.
#include "tests.h"

#include <chrono>
#include <thread>

void test_logging();

namespace {

// TimerLogger is process-wide and WARMUP_ITERATIONS is the global main.cpp
// sets from --warmup: hand both back even when a require_test throws, so the
// tests the dispatcher runs next see the state they started with.
struct TimerStateGuard {
  std::size_t saved_warmup = WARMUP_ITERATIONS;
  ~TimerStateGuard() {
    TimerLogger::setWarmup(saved_warmup);
    CLEAN_TIMER();
  }
};

void sleep_ms(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Timing checks are lower bounds only: sleep_for blocks for at least the
// requested span, and this may run on a loaded machine.
void check_timer_semantics() {
  TimerStateGuard guard;
  CLEAN_TIMER();
  TimerLogger::setWarmup(2);

  // Two warm-up experiments time only section W.
  for (int i = 0; i < 2; ++i) {
    TIME_START("W");
    sleep_ms(1);
    TIME_END("W");
    END_EXPERIMENT();
  }
  // One measured experiment times M twice, the way a Merkle trial with H
  // PIR calls brackets each phase H times.
  TIME_START("M");
  sleep_ms(2);
  TIME_END("M");
  TIME_START("M");
  sleep_ms(2);
  TIME_END("M");
  END_EXPERIMENT();

  // Catches `currentExperiment[section] += duration` turning into `=` (a
  // plausible "fix" for apparent double counting): the two 2 ms spans add up.
  require_test(GET_LAST_TIME("M") >= 4.0,
               "repeated TIME_START/TIME_END of one section must be summed "
               "within an experiment");
  // getLastTime reads the last record only, warm-ups notwithstanding; a
  // section absent from that record (the way a tree trial's stages_ms reads
  // phases its plan never ran) is 0.0.
  require_test(GET_LAST_TIME("W") == 0.0,
               "getLastTime must read only the last experiment record");

  // getAvgTime skips exactly WARMUP_ITERATIONS records. An off-by-one shows
  // up either as W leaking into the two-warm-up average or as the second W
  // record vanishing from the one-warm-up average.
  require_test(GET_AVG_TIME("M") >= 4.0,
               "getAvgTime must average the post-warm-up records");
  require_test(GET_AVG_TIME("W") == 0.0,
               "a section present only in warm-up records must average to 0.0");
  TimerLogger::setWarmup(1);
  require_test(GET_AVG_TIME("W") >= 1.0,
               "with one warm-up the second W record must be averaged");
  require_test(GET_AVG_TIME("M") >= 4.0,
               "lowering the warm-up count must keep the measured record");
  // Three records and three warm-ups leave nothing measured.
  TimerLogger::setWarmup(3);
  require_test(GET_AVG_TIME("M") == 0.0,
               "records <= warm-ups must average to 0.0");

  CLEAN_TIMER();
  require_test(GET_LAST_TIME("M") == 0.0,
               "cleanup must drop every experiment record");
  TimerLogger::setWarmup(0);
  require_test(GET_AVG_TIME("M") == 0.0,
               "cleanup must leave nothing for getAvgTime to average");
}

}  // namespace

void test_logging() {
  print_func_name(__FUNCTION__);
  check_timer_semantics();
  std::cout << "PASS: TimerLogger sums repeated sections per experiment, "
               "getLastTime reads the last record, getAvgTime skips exactly "
               "WARMUP_ITERATIONS records\n";
  PRINT_BAR;
}
