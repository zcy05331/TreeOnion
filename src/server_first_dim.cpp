#include "server.h"
#include "matrix.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

void PirServer::prep_query(std::vector<RlweCt> &fst_dim_query,
                           std::vector<db_coeff_t> &query_data) {
  const size_t fst_dim_sz = pir_params_.get_fst_dim_sz();       // 256
  const size_t coeff_val_cnt = pir_params_.get_coeff_val_cnt(); // 4096
  const size_t slice_sz = fst_dim_sz * 2;
  const auto &rns_mods = pir_params_.get_rns_mods();
  const size_t K = rns_mods.size();
  constexpr size_t N = DBConsts::PolyDegree;

  // prep_query 把 ciphertext-major query 转成 first-dim kernel layout。输入是
  // fst_dim_query[i].c0/c1，每个 i 是 selection vector 的一个 BFV ciphertext；
  // 先对每个 RNS limb 做 NTT，然后按 level-major 写成
  //   query_data[level][i][0/1] = B[i][c0/c1].
  // 因而每个 NTT level 的标准矩阵乘法都是
  //   A[Nrest x N0] * B[N0 x 2] -> C[Nrest x 2].
  // 若 K>1，level = limb*N + coeff；这是 per-limb/per-level 视图。
  for (size_t i = 0; i < fst_dim_query.size(); i++) {
    RlweCt &ct = fst_dim_query[i];
    for (size_t mod_id = 0; mod_id < K; mod_id++) {
      utils::ntt_fwd(ct.c0.data() + mod_id * N, N, rns_mods[mod_id]);
      utils::ntt_fwd(ct.c1.data() + mod_id * N, N, rns_mods[mod_id]);
    }
    ct.ntt_form = true;
  }

  // Pre-fetch the data pointers to avoid repeated indirect access
  std::vector<const uint64_t *> data0_ptrs(fst_dim_sz);
  std::vector<const uint64_t *> data1_ptrs(fst_dim_sz);

  // Prefetch all pointers
  for (size_t i = 0; i < fst_dim_sz; ++i) {
    data0_ptrs[i] = fst_dim_query[i].c0.data();
    data1_ptrs[i] = fst_dim_query[i].c1.data();
  }

  // Process in blocks to improve cache locality
  const size_t BLOCK_SIZE = 8;
  // Fallback to scalar implementation if no SIMD is available
  for (size_t slice_block = 0; slice_block < coeff_val_cnt;
       slice_block += BLOCK_SIZE) {
    const size_t slice_block_end =
        std::min(slice_block + BLOCK_SIZE, coeff_val_cnt);

    for (size_t i = 0; i < fst_dim_sz; ++i) {
      const uint64_t *p0 = data0_ptrs[i];
      const uint64_t *p1 = data1_ptrs[i];

      // Process a block of slices for the same i value (improves temporal
      // locality)
      for (size_t slice_id = slice_block; slice_id < slice_block_end;
           ++slice_id) {
        const size_t idx = slice_id * slice_sz + i * 2;
        query_data[idx] = static_cast<db_coeff_t>(p0[slice_id]);
        query_data[idx + 1] = static_cast<db_coeff_t>(p1[slice_id]);
      }
    }
  }
}

void PirServer::prep_query_composite(const std::vector<RlweCt> &fst_dim_query,
                                     uint32_t *query_lo, uint32_t *query_hi) {
  const size_t fst_dim_sz = pir_params_.get_fst_dim_sz();
  const size_t coeff_val_cnt = pir_params_.get_coeff_val_cnt();
  const size_t slice_sz = fst_dim_sz * 2;
  const auto &crt = pir_params_.get_composite_rns();
  const uint64_t q1 = crt.q1;
  const uint64_t q2 = crt.q2;

  // Composite query 已在 evaluate_first_dim 中按 logical q=q1*q2 进入 NTT。
  // 这里只做首维 kernel 需要的投影：同一 B[N0 x 2] 分别取 mod q1/q2，写入
  // 两组 uint32 level-major buffers。后续两个 32x32->64 kernels 的输出会在
  // inter_to_cts_composite 里 CRT-compose 回 logical mod q。
  std::vector<const uint64_t *> data0_ptrs(fst_dim_sz);
  std::vector<const uint64_t *> data1_ptrs(fst_dim_sz);
  for (size_t i = 0; i < fst_dim_sz; ++i) {
    data0_ptrs[i] = fst_dim_query[i].c0.data();
    data1_ptrs[i] = fst_dim_query[i].c1.data();
  }

  constexpr size_t BLOCK_SIZE = 8;
  for (size_t slice_block = 0; slice_block < coeff_val_cnt;
       slice_block += BLOCK_SIZE) {
    const size_t slice_block_end =
        std::min(slice_block + BLOCK_SIZE, coeff_val_cnt);
    for (size_t i = 0; i < fst_dim_sz; ++i) {
      const uint64_t *p0 = data0_ptrs[i];
      const uint64_t *p1 = data1_ptrs[i];
      for (size_t slice_id = slice_block; slice_id < slice_block_end;
           ++slice_id) {
        const size_t idx = slice_id * slice_sz + i * 2;
        const uint64_t v0 = p0[slice_id];
        const uint64_t v1 = p1[slice_id];
        query_lo[idx]     = static_cast<uint32_t>(v0 % q1);
        query_lo[idx + 1] = static_cast<uint32_t>(v1 % q1);
        query_hi[idx]     = static_cast<uint32_t>(v0 % q2);
        query_hi[idx + 1] = static_cast<uint32_t>(v1 % q2);
      }
    }
  }
}

// Computes Algorithm 4 first dimension as batched standard matrix
// multiplication over NTT levels. For each limb/coefficient level:
//   A = DB values with shape [other_dim_sz x fst_dim_sz],
//   B = BFV selection vector columns [fst_dim_sz x 2] (c0/c1),
//   C = encrypted candidates [other_dim_sz x 2].
// delayed modulus optimization lives inside the matmul kernels: they accumulate
// only as far as the accumulator width allows, then reduce mod q.
std::vector<RlweCt>
PirServer::evaluate_first_dim(std::vector<RlweCt> &fst_dim_query,
                              PirPipelineProfile *profile) {
  const size_t fst_dim_sz = pir_params_.get_fst_dim_sz();  // number of plaintexts in the first dimension
  const size_t other_dim_sz = pir_params_.get_other_dim_sz();  // number of plaintexts in the other dimensions
  const size_t K = pir_params_.K();
  const size_t coeff_val_cnt = pir_params_.get_coeff_val_cnt(); // polydegree * RNS moduli count
  const size_t one_ct_sz = 2 * coeff_val_cnt; // Ciphertext has two polynomials
  const auto &rns_mods = pir_params_.get_rns_mods();
  constexpr size_t N = DBConsts::PolyDegree;

  const auto &crt = pir_params_.get_composite_rns();
  if (crt.enabled) {
    // Composite path: logical ciphertext modulus q=q1*q2 has pipeline K()==1, so
    // queries are NTTed once under q. Only the first-dim kernel splits DB/query
    // NTT values into two uint32 projections (mod q1 and mod q2), then runs
    // the same 32x32->64 kernel under q1 and q2 to compute residue outputs.
    // inter_to_cts_composite CRT-composes those residues back to logical mod q
    // before INTT. Later PIR stages still see K=1 ciphertexts.
    // 可选的子阶段计时：只在既有边界读时钟。
    using ProfileClock = std::chrono::steady_clock;
    ProfileClock::time_point mark;
    if (profile) mark = ProfileClock::now();
    const auto lap = [&](std::chrono::nanoseconds PirPipelineProfile::*slot) {
      if (!profile) return;
      const auto now = ProfileClock::now();
      profile->*slot += now - mark;
      mark = now;
    };

    const uint64_t q = rns_mods[0];
    for (size_t i = 0; i < fst_dim_query.size(); ++i) {
      RlweCt &ct = fst_dim_query[i];
      utils::ntt_fwd(ct.c0.data(), N, q);
      utils::ntt_fwd(ct.c1.data(), N, q);
      ct.ntt_form = true;
    }
    lap(&PirPipelineProfile::first_dim_query_ntt);

    std::fill(inter_res_lo_.begin(), inter_res_lo_.end(), 0);
    std::fill(inter_res_hi_.begin(), inter_res_hi_.end(), 0);

    TIME_START(FST_DIM_PREP);
    std::vector<uint32_t> query_lo(fst_dim_sz * one_ct_sz);
    std::vector<uint32_t> query_hi(fst_dim_sz * one_ct_sz);
    prep_query_composite(fst_dim_query, query_lo.data(), query_hi.data());
    TIME_END(FST_DIM_PREP);
    lap(&PirPipelineProfile::first_dim_query_pack);

    TIME_START(CORE_TIME);
    level_mat_mat_32(db_lo_.get(), query_lo.data(), inter_res_lo_.data(),
                     other_dim_sz, fst_dim_sz, coeff_val_cnt, crt.q1);
    level_mat_mat_32(db_hi_.get(), query_hi.data(), inter_res_hi_.data(),
                     other_dim_sz, fst_dim_sz, coeff_val_cnt, crt.q2);
    TIME_END(CORE_TIME);
    lap(&PirPipelineProfile::first_dim_core);

    TIME_START(FST_INTER_TO_CTS_TIME);
    std::vector<RlweCt> result;
    result.reserve(other_dim_sz);
    inter_to_cts_composite(result, inter_res_lo_.data(), inter_res_hi_.data());
    TIME_END(FST_INTER_TO_CTS_TIME);
    lap(&PirPipelineProfile::first_dim_finalize);
    return result;
  }

  using ProfileClock = std::chrono::steady_clock;
  ProfileClock::time_point mark;
  if (profile) mark = ProfileClock::now();
  const auto lap = [&](std::chrono::nanoseconds PirPipelineProfile::*slot) {
    if (!profile) return;
    const auto now = ProfileClock::now();
    profile->*slot += now - mark;
    mark = now;
  };

  // fill the intermediate result with zeros
  std::fill(inter_res_.begin(), inter_res_.end(), 0);

  // reallocate the query data to a continuous memory
  TIME_START(FST_DIM_PREP);
  std::vector<db_coeff_t> query_data(fst_dim_sz * one_ct_sz);
  prep_query(fst_dim_query, query_data);
  TIME_END(FST_DIM_PREP);
  lap(&PirPipelineProfile::first_dim_query_pack);

  /*
  Standard path for 2025 Algorithm 4: instead of treating each DB matrix entry
  as a vector and doing component-wise vector products, we peel off the vector
  dimension. For every NTT level independently:
      db_mat[level]    = A[other_dim_sz x fst_dim_sz]
      query_mat[level] = B[fst_dim_sz x 2]  (BFV c0/c1 selection vector)
      inter_res[level] = C[other_dim_sz x 2]
  This is why the first PIR dimension is exactly a standard matrix
  multiplication kernel; coefficient-major DB preprocessing makes the inner
  fst_dim_sz scan contiguous.
  */
  // prepare the matrices
  db_matrix_t db_mat { db_aligned_.get(), other_dim_sz, fst_dim_sz, coeff_val_cnt };
  db_matrix_t query_mat { query_data.data(), fst_dim_sz, 2, coeff_val_cnt };
  inter_matrix_t inter_res_mat { inter_res_.data(), other_dim_sz, 2, coeff_val_cnt };

  // Per-level modulus: level lvl spans coefficients of limb (lvl / N).
  std::vector<uint64_t> level_qs(coeff_val_cnt);
  for (size_t k = 0; k < K; ++k) {
    std::fill(level_qs.begin() + k * N, level_qs.begin() + (k + 1) * N, rns_mods[k]);
  }
  TIME_START(CORE_TIME);
  level_mat_mat(&db_mat, &query_mat, &inter_res_mat, level_qs.data());
  TIME_END(CORE_TIME);
  lap(&PirPipelineProfile::first_dim_core);

  // ========== transform the intermediate to coefficient form. Delay the modulus operation ==========
  TIME_START(FST_INTER_TO_CTS_TIME);
  std::vector<RlweCt> result; // output vector
  result.reserve(other_dim_sz);
  inter_to_cts(result, inter_res_.data());
  TIME_END(FST_INTER_TO_CTS_TIME);
  lap(&PirPipelineProfile::first_dim_finalize);

  return result;
}


void PirServer::inter_to_cts(std::vector<RlweCt> &result, const inter_coeff_t *__restrict inter_res) {
  const size_t other_dim_sz = pir_params_.get_other_dim_sz();
  const size_t K = pir_params_.K();
  constexpr size_t coeff_count = DBConsts::PolyDegree;
  const auto &rns_mods = pir_params_.get_rns_mods();
  const size_t coeff_val_cnt = coeff_count * K;
  const size_t inter_padding = other_dim_sz * 2;  // distance between coefficients in inter_res

  // inter_res 的实际 stride 是 C[level][candidate][poly]，也就是
  // level-major 后接 candidate，再接 c0/c1 两列：
  //   inter_res[level * other_dim_sz * 2 + candidate * 2 + poly].
  // inter_to_cts 做的是 transpose/gather，把它变成
  //   RlweCt[candidate].c{0,1}[level].
  // gather 后每个 ciphertext 仍在 NTT form，随后逐 limb INTT 回 coefficient form。
  //
  // We need to unroll the loop to process multiple ciphertexts at once.
  // Otherwise, this function is basically reading the intermediate result
  // with a stride of inter_padding, which causes many cache misses.
  constexpr size_t unroll_factor = 16;

  // Process ciphertexts in blocks of unroll_factor for the main part
  const size_t main_blocks = other_dim_sz / unroll_factor;
  for (size_t block = 0; block < main_blocks; block++) {
    const size_t j = block * unroll_factor;

    // Create an array of ciphertexts.
    std::array<RlweCt, unroll_factor> cts;
    for (size_t idx = 0; idx < unroll_factor; idx++) {
      cts[idx].c0.assign(coeff_val_cnt, 0);
      cts[idx].c1.assign(coeff_val_cnt, 0);
    }

    // Compute the base indices for each ciphertext's two intermediate parts.
    // For ciphertext idx, poly0 uses base index: j*2 + 2*idx and poly1 uses j*2 + 2*idx + 1.
    std::array<size_t, unroll_factor> base0, base1;
    for (size_t idx = 0; idx < unroll_factor; idx++) {
      base0[idx] = j * 2 + 2 * idx;
      base1[idx] = j * 2 + 2 * idx + 1;
    }

    // Initialize intermediate indices and ciphertext write indices.
    std::array<size_t, unroll_factor> inter_idx0 = {0};  // for poly0 of each ciphertext
    std::array<size_t, unroll_factor> inter_idx1 = {0};  // for poly1 of each ciphertext
    std::array<size_t, unroll_factor> ct_idx0    = {0};  // write index for poly0
    std::array<size_t, unroll_factor> ct_idx1    = {0};  // write index for poly1

    // Process each modulus and coefficient. The `% q` is mathematically
    // redundant (mat_mat already reduces per output write)
    for (size_t mod_id = 0; mod_id < K; mod_id++) {
      // const uint64_t q = rns_mods[mod_id];
      for (size_t coeff_id = 0; coeff_id < coeff_count; coeff_id++) {
        #pragma unroll
        for (size_t idx = 0; idx < unroll_factor; idx++) {
          inter_coeff_t x0 = inter_res[ base0[idx] + inter_idx0[idx] * inter_padding ];
          // cts[idx].c0[ ct_idx0[idx]++ ] = static_cast<uint64_t>(x0 % q);
          cts[idx].c0[ ct_idx0[idx]++ ] = static_cast<uint64_t>(x0);

          inter_coeff_t x1 = inter_res[ base1[idx] + inter_idx1[idx] * inter_padding ];
          cts[idx].c1[ ct_idx1[idx]++ ] = static_cast<uint64_t>(x1);

          inter_idx0[idx]++;
          inter_idx1[idx]++;
        }
      }
    }

    // Mark each ciphertext as being in NTT form and then transform back.
    #pragma unroll
    for (size_t idx = 0; idx < unroll_factor; idx++) {
      for (size_t mod_id = 0; mod_id < K; mod_id++) {
        utils::ntt_inv(cts[idx].c0.data() + mod_id * coeff_count, coeff_count, rns_mods[mod_id]);
        utils::ntt_inv(cts[idx].c1.data() + mod_id * coeff_count, coeff_count, rns_mods[mod_id]);
      }
      cts[idx].ntt_form = false;
      result.emplace_back(std::move(cts[idx]));
    }
  }

  // Handle remaining ciphertexts individually for edge cases
  const size_t remaining_start = main_blocks * unroll_factor;
  for (size_t j = remaining_start; j < other_dim_sz; j++) {
    // Create a single ciphertext
    RlweCt ct;
    ct.c0.assign(coeff_val_cnt, 0);
    ct.c1.assign(coeff_val_cnt, 0);

    // Compute the base indices for this ciphertext's two intermediate parts
    const size_t base0 = j * 2;
    const size_t base1 = j * 2 + 1;

    // Initialize intermediate indices and ciphertext write indices
    size_t inter_idx0 = 0;  // for poly0
    size_t inter_idx1 = 0;  // for poly1
    size_t ct_idx0 = 0;     // write index for poly0
    size_t ct_idx1 = 0;     // write index for poly1

    // Edge-case loop (other_dim_sz % unroll_factor != 0). Same gather-and-cast
    // as the unrolled block above; mat_mat already produced values < q.
    for (size_t mod_id = 0; mod_id < K; mod_id++) {
      for (size_t coeff_id = 0; coeff_id < coeff_count; coeff_id++) {
        inter_coeff_t x0 = inter_res[base0 + inter_idx0 * inter_padding];
        ct.c0[ct_idx0++] = static_cast<uint64_t>(x0);

        inter_coeff_t x1 = inter_res[base1 + inter_idx1 * inter_padding];
        ct.c1[ct_idx1++] = static_cast<uint64_t>(x1);

        inter_idx0++;
        inter_idx1++;
      }
    }

    // Mark ciphertext as being in NTT form and then transform back
    for (size_t mod_id = 0; mod_id < K; mod_id++) {
      utils::ntt_inv(ct.c0.data() + mod_id * coeff_count, coeff_count, rns_mods[mod_id]);
      utils::ntt_inv(ct.c1.data() + mod_id * coeff_count, coeff_count, rns_mods[mod_id]);
    }
    ct.ntt_form = false;
    result.emplace_back(std::move(ct));
  }
}

void PirServer::inter_to_cts_composite(std::vector<RlweCt> &result,
                                       const uint64_t *inter_lo,
                                       const uint64_t *inter_hi) {
  const auto &crt = pir_params_.get_composite_rns();
  const uint64_t q1 = crt.q1;
  const uint64_t q2 = crt.q2;
  const uint64_t q1_inv_mod_q2 = crt.q1_inv_mod_q2;
  const uint64_t q = q1 * q2;
  const size_t other_dim_sz = pir_params_.get_other_dim_sz();
  constexpr size_t coeff_count = DBConsts::PolyDegree;
  const size_t inter_padding = other_dim_sz * 2;

  // Composite inter_res_lo/hi 使用同样的 C[level][candidate][poly] stride；
  // logical K=1，所以 level 就是 NTT coefficient。这里先把 lo/hi 两个 limb 做
  // CRT-compose 得到 mod q 的 RlweCt[candidate].c{0,1}[coeff_id]，再 INTT
  // 回 coefficient form。split 和 compose 都只包围首维 matmul。
  //
  // Garner CRT compose: (lo, hi) → lo + q1 * ((hi − lo) · q1_inv mod q2).
  // Since q1, q2 < 2^29, diff·q1_inv stays under 2^58 — no 128-bit mul.
  // compose runs 2 * coeff_count * other_dim_sz times per query, so the two
  // reductions go through Barrett instead of a hardware divide, and the sum
  // in (0, 2 q2) takes one conditional subtraction.
  const utils::BarrettU128 b_q2 = utils::barrett_u128_setup(q2);
  auto compose = [q1, q2, q1_inv_mod_q2, b_q2](uint64_t lo,
                                               uint64_t hi) -> uint64_t {
    const uint64_t sum = hi + q2 - utils::barrett_reduce_u128(lo, b_q2);
    const uint64_t diff = sum >= q2 ? sum - q2 : sum;
    const uint64_t k = utils::barrett_reduce_u128(
        static_cast<uint128_t>(diff) * q1_inv_mod_q2, b_q2);
    return lo + q1 * k;
  };

  constexpr size_t unroll_factor = 16;
  const size_t main_blocks = other_dim_sz / unroll_factor;
  for (size_t block = 0; block < main_blocks; block++) {
    const size_t j = block * unroll_factor;
    std::array<RlweCt, unroll_factor> cts;
    for (size_t idx = 0; idx < unroll_factor; idx++) {
      cts[idx].c0.assign(coeff_count, 0);
      cts[idx].c1.assign(coeff_count, 0);
    }
    for (size_t coeff_id = 0; coeff_id < coeff_count; coeff_id++) {
      const size_t row = coeff_id * inter_padding;
      #pragma unroll
      for (size_t idx = 0; idx < unroll_factor; idx++) {
        const size_t b0 = row + j * 2 + 2 * idx;
        const size_t b1 = b0 + 1;
        cts[idx].c0[coeff_id] = compose(inter_lo[b0], inter_hi[b0]);
        cts[idx].c1[coeff_id] = compose(inter_lo[b1], inter_hi[b1]);
      }
    }
    for (size_t idx = 0; idx < unroll_factor; idx++) {
      utils::ntt_inv(cts[idx].c0.data(), coeff_count, q);
      utils::ntt_inv(cts[idx].c1.data(), coeff_count, q);
      cts[idx].ntt_form = false;
      result.emplace_back(std::move(cts[idx]));
    }
  }

  for (size_t j = main_blocks * unroll_factor; j < other_dim_sz; j++) {
    RlweCt ct;
    ct.c0.assign(coeff_count, 0);
    ct.c1.assign(coeff_count, 0);
    for (size_t coeff_id = 0; coeff_id < coeff_count; coeff_id++) {
      const size_t b0 = coeff_id * inter_padding + j * 2;
      const size_t b1 = b0 + 1;
      ct.c0[coeff_id] = compose(inter_lo[b0], inter_hi[b0]);
      ct.c1[coeff_id] = compose(inter_lo[b1], inter_hi[b1]);
    }
    utils::ntt_inv(ct.c0.data(), coeff_count, q);
    utils::ntt_inv(ct.c1.data(), coeff_count, q);
    ct.ntt_form = false;
    result.emplace_back(std::move(ct));
  }
}
