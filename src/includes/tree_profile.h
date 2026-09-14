#pragma once

#include <chrono>

// Full server-side stage timing for one generic tree request (design
// specification, section 12.3). Stages are exclusive; `total` is the wall
// time of the whole request wrapper. A null sink costs nothing.

namespace tree_superrecord {

using TreeStageDuration = std::chrono::nanoseconds;

struct TreeServerTiming {
  TreeStageDuration query_expand{};        // packed query -> useful RLWE constants
  TreeStageDuration selector_completion{}; // RLWE' rows -> RGSW selectors
  TreeStageDuration alpha_prepare{};       // ragged pyramid + NTT + q1/q2 split
  TreeStageDuration alpha_scan{};          // database reads and multiply-accumulate
  TreeStageDuration first_dim_finalize{};  // CRT compose + INTT + materialisation
  TreeStageDuration beta_fold{};           // candidate CMux
  TreeStageDuration private_rotate{};      // encrypted residue shift
  TreeStageDuration projection{};          // BV substitution rounds
  TreeStageDuration path_pack{};           // public shifts and additions
  TreeStageDuration response_merge{};      // MLWE response residue merge
  TreeStageDuration modulus_switch{};      // full-q -> small-q
  TreeStageDuration total{};
  size_t response_merge_keyswitches = 0;
};

inline double stage_ms(TreeStageDuration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

// Lap timer that adds elapsed time to a member of an optional sink.
class StageClock {
public:
  using Clock = std::chrono::steady_clock;
  explicit StageClock(TreeServerTiming *sink) : sink_(sink) {
    if (sink_) mark_ = Clock::now();
  }
  void restart() {
    if (sink_) mark_ = Clock::now();
  }
  void lap(TreeStageDuration TreeServerTiming::*slot) {
    if (!sink_) return;
    const Clock::time_point now = Clock::now();
    sink_->*slot += now - mark_;
    mark_ = now;
  }
  bool active() const { return sink_ != nullptr; }

private:
  TreeServerTiming *sink_;
  Clock::time_point mark_{};
};

}  // namespace tree_superrecord
