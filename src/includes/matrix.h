#pragma once
#include "utils.h"
#include <stdint.h>
#include <stddef.h>

#if defined(__AVX512F__)
    #include <immintrin.h>
#elif defined(__AVX2__)
    #include <immintrin.h>
#endif

typedef struct {
    uint64_t *data;
    size_t rows;
    size_t cols;
    size_t levels;
} matrix_t;

typedef struct {
    db_coeff_t *data;
    size_t rows;
    size_t cols;
    size_t levels;
} db_matrix_t;

typedef struct {
    inter_coeff_t *data;
    size_t rows;
    size_t cols;
    size_t levels;
} inter_matrix_t;

typedef struct {
    uint128_t *data;
    size_t rows;
    size_t cols;
    size_t levels;
} matrix128_t;

enum class MatMul32Dispatch {
    Avx512Lazy,
    ScalarU128,
};

enum class Matrix32QueryLayout {
    Interleaved,
    SplitColumns,
};

struct Avx512KernelAvailability {
    bool compiled_with_required_avx512_kernel = false;
    bool runtime_cpu_supports_required_avx512_features = false;

    bool available() const noexcept {
        return compiled_with_required_avx512_kernel &&
               runtime_cpu_supports_required_avx512_features;
    }
};

Avx512KernelAvailability detected_matmul32_availability();

bool avx512_lazy_accumulation_safe(size_t cols, uint64_t modulus);

MatMul32Dispatch select_matmul32_dispatch(
    size_t cols,
    uint64_t modulus,
    Avx512KernelAvailability availability);

void level_mat_mat_32_dispatch(const uint32_t *A_data,
                               const uint32_t *B_data,
                               uint64_t *out_data,
                               size_t m,
                               size_t n,
                               size_t levels,
                               uint64_t q,
                               Avx512KernelAvailability availability,
                               Matrix32QueryLayout query_layout =
                                   Matrix32QueryLayout::Interleaved);



void mat_mat(const db_coeff_t *__restrict A, const db_coeff_t *__restrict B,
    inter_coeff_t *__restrict out, const size_t rows,
    const size_t cols, uint64_t q);

void level_mat_mat(db_matrix_t *A, db_matrix_t *B, inter_matrix_t *out,
                   const uint64_t *level_qs);

void level_mat_mat_nochunk(db_matrix_t *A, db_matrix_t *B, inter_matrix_t *out,
                           const uint64_t *level_qs);

void level_mat_mat_nochunk_u64(const uint32_t *A_data, const uint32_t *B_data,
                               uint64_t *out_data, size_t m, size_t n,
                               size_t levels, const uint64_t *level_qs);

uint32_t level_mat_mat_stream_only(const uint32_t *A_data, size_t m,
                                   size_t n, size_t levels);

void level_mat_mat_32(const uint32_t *A_data, const uint32_t *B_data,
                      uint64_t *out_data, size_t m, size_t n, size_t levels,
                      uint64_t q,
                      Matrix32QueryLayout query_layout =
                          Matrix32QueryLayout::Interleaved);

void component_wise_mult(matrix_t *A, matrix_t *B, matrix_t *out);
void component_wise_mult_128(matrix_t *A, matrix_t *B, matrix128_t *out);
#if defined(__AVX512F__)
void component_wise_mult_direct_mod(matrix_t *A, matrix_t *B, uint64_t *out, const uint64_t mod);
#endif
