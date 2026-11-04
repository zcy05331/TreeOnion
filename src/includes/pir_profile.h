#pragma once

#include <chrono>

struct PirPipelineProfile {
  std::chrono::nanoseconds expand{};
  std::chrono::nanoseconds convert{};
  std::chrono::nanoseconds first_dim_query_ntt{};
  std::chrono::nanoseconds first_dim_query_pack{};
  std::chrono::nanoseconds first_dim_core{};
  std::chrono::nanoseconds first_dim_finalize{};
  std::chrono::nanoseconds other_dim{};
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
