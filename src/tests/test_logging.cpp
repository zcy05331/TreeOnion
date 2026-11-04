#include "tests.h"

#include <chrono>
#include <thread>

void test_logging();

namespace {

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

void check_timer_semantics() {
  TimerStateGuard guard;
  CLEAN_TIMER();
  TimerLogger::setWarmup(2);

  for (int i = 0; i < 2; ++i) {
    TIME_START("W");
    sleep_ms(1);
    TIME_END("W");
    END_EXPERIMENT();
  }
  TIME_START("M");
  sleep_ms(2);
  TIME_END("M");
  TIME_START("M");
  sleep_ms(2);
  TIME_END("M");
  END_EXPERIMENT();

  require_test(GET_LAST_TIME("M") >= 4.0,
               "repeated TIME_START/TIME_END of one section must be summed "
               "within an experiment");
  require_test(GET_LAST_TIME("W") == 0.0,
               "getLastTime must read only the last experiment record");

  require_test(GET_AVG_TIME("M") >= 4.0,
               "getAvgTime must average the post-warm-up records");
  require_test(GET_AVG_TIME("W") == 0.0,
               "a section present only in warm-up records must average to 0.0");
  TimerLogger::setWarmup(1);
  require_test(GET_AVG_TIME("W") >= 1.0,
               "with one warm-up the second W record must be averaged");
  require_test(GET_AVG_TIME("M") >= 4.0,
               "lowering the warm-up count must keep the measured record");
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

}

void test_logging() {
  print_func_name(__FUNCTION__);
  check_timer_semantics();
  std::cout << "PASS: TimerLogger sums repeated sections per experiment, "
               "getLastTime reads the last record, getAvgTime skips exactly "
               "WARMUP_ITERATIONS records\n";
  PRINT_BAR;
}
