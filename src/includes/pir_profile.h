#pragma once

#include <chrono>

// Exact per-stage wall time of one PirServer::make_query_profiled call. The
// stages are the existing pipeline boundaries (nothing is reordered to take
// the measurements): ExpandBFV, RGSW completion, the first-dimension query
// NTT, the q1/q2 query packing, the first-dimension matrix core, the CRT
// compose + INTT finalize, the remaining-dimension CMux tree, and the final
// modulus switch. It attributes server time to the stages; make_query itself
// never pays for a profile.
struct PirPipelineProfile {
  std::chrono::nanoseconds expand{};
  std::chrono::nanoseconds convert{};
  std::chrono::nanoseconds first_dim_query_ntt{};
  std::chrono::nanoseconds first_dim_query_pack{};
  std::chrono::nanoseconds first_dim_core{};
  std::chrono::nanoseconds first_dim_finalize{};
  std::chrono::nanoseconds other_dim{};
  // Merged-response path only: private rotation of each level result onto
  // the stride grid, and the MLWE residue merge of all levels into one
  // ciphertext. Zero for the single-ciphertext pipelines.
  std::chrono::nanoseconds response_rotate{};
  std::chrono::nanoseconds response_merge{};
  std::chrono::nanoseconds mod_switch{};

  std::chrono::nanoseconds total() const {
    return expand + convert + first_dim_query_ntt + first_dim_query_pack +
           first_dim_core + first_dim_finalize + other_dim + response_rotate +
           response_merge + mod_switch;
  }

  PirPipelineProfile &operator+=(const PirPipelineProfile &other) {
    expand += other.expand;
    convert += other.convert;
    first_dim_query_ntt += other.first_dim_query_ntt;
    first_dim_query_pack += other.first_dim_query_pack;
    first_dim_core += other.first_dim_core;
    first_dim_finalize += other.first_dim_finalize;
    other_dim += other.other_dim;
    response_rotate += other.response_rotate;
    response_merge += other.response_merge;
    mod_switch += other.mod_switch;
    return *this;
  }
};
