// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/tensor_rt.c
// Purpose: C runtime support for tensor rt.
// Author:  Mark Oxley
// Created: 2026
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <math.h>
#include <stdint.h>
#ifdef _OPENMP
#include <omp.h>
#endif
#include <stdlib.h>
#include <string.h>
#ifdef __AVX2__
#include <immintrin.h>
#endif

// Skink slice values arrive as slice-header pointers (rc_rt.c); unwrap
// with Skink_slice_data before dereferencing elements.
extern int64_t Skink_slice_len(void *ptr);
extern void *Skink_slice_data(void *ptr);
extern void Skink_panic(char *msg);

// Tensor represents a 2D matrix with row-major double-precision data.
typedef struct {
  int rows;
  int cols;
  double *data;
} Tensor;

// skink_tensor_check_dims validates tensor creation dimensions: both
// must be positive and their product must fit in int, otherwise the
// element-count multiply wraps and a small allocation gets a large
// write loop (VULN-21).
static void skink_tensor_check_dims(int rows, int cols) {
  if (rows <= 0 || cols <= 0 ||
      (int64_t)rows * (int64_t)cols > (int64_t)INT32_MAX) {
    Skink_panic((char *)"tensor: invalid dimensions");
  }
}

// Skink_tensor_ones creates a tensor filled with 1.0 values.
void *Skink_tensor_ones(int rows, int cols) {
  skink_tensor_check_dims(rows, cols);
  Tensor *t = malloc(sizeof(Tensor));
  if (!t)
    return NULL;
  t->rows = rows;
  t->cols = cols;
  t->data = malloc((size_t)rows * cols * sizeof(double));
  if (!t->data) {
    free(t);
    return NULL;
  }
  for (int i = 0; i < rows * cols; i++) {
    t->data[i] = 1.0;
  }
  return t;
}

// Skink_tensor_zeros creates a tensor filled with 0.0 values.
void *Skink_tensor_zeros(int rows, int cols) {
  skink_tensor_check_dims(rows, cols);
  Tensor *t = malloc(sizeof(Tensor));
  if (!t)
    return NULL;
  t->rows = rows;
  t->cols = cols;
  t->data = calloc((size_t)rows * cols, sizeof(double));
  if (!t->data) {
    free(t);
    return NULL;
  }
  return t;
}

// Skink_tensor_matmul_data_row computes one row of the matrix product C = A * B.
__attribute__((always_inline)) static inline void
Skink_tensor_matmul_data_row(const double *a_data, int k, const double *b_data,
                             int n, double *__restrict c_data, int i) {
  int a_row = i * k;
  int c_row = i * n;
  for (int j = 0; j < n; j++) {
    c_data[c_row + j] = 0.0;
  }
  for (int k_idx = 0; k_idx < k; k_idx++) {
    double a_val = a_data[a_row + k_idx];
    int b_row = k_idx * n;
    for (int j = 0; j < n; j++) {
      c_data[c_row + j] += a_val * b_data[b_row + j];
    }
  }
}

// Skink_tensor_matmul_data computes the full matrix product C = A * B with optional OpenMP parallelism.
void Skink_tensor_matmul_data(const double *a_data, int m, int k,
                              const double *b_data, int n,
                              double *__restrict c_data) {
  a_data = (const double *)Skink_slice_data((void *)a_data);
  b_data = (const double *)Skink_slice_data((void *)b_data);
  c_data = (double *)Skink_slice_data((void *)c_data);
  if (m * k * n > 100000000) {
#pragma omp parallel for
    for (int i = 0; i < m; i++) {
      Skink_tensor_matmul_data_row(a_data, k, b_data, n, c_data, i);
    }
  } else {
    for (int i = 0; i < m; i++) {
      Skink_tensor_matmul_data_row(a_data, k, b_data, n, c_data, i);
    }
  }
}

// Skink_tensor_matmul4 computes four matrix products in parallel using OpenMP sections.
void Skink_tensor_matmul4(int m, int k, int n, const double *a_data,
                          const double *b0, double *c0, const double *b1,
                          double *c1, const double *b2, double *c2,
                          const double *b3, double *c3) {
  a_data = (const double *)Skink_slice_data((void *)a_data);
  b0 = (const double *)Skink_slice_data((void *)b0);
  c0 = (double *)Skink_slice_data((void *)c0);
  b1 = (const double *)Skink_slice_data((void *)b1);
  c1 = (double *)Skink_slice_data((void *)c1);
  b2 = (const double *)Skink_slice_data((void *)b2);
  c2 = (double *)Skink_slice_data((void *)c2);
  b3 = (const double *)Skink_slice_data((void *)b3);
  c3 = (double *)Skink_slice_data((void *)c3);
#pragma omp parallel sections
  {
#pragma omp section
    Skink_tensor_matmul_data(a_data, m, k, b0, n, c0);
#pragma omp section
    Skink_tensor_matmul_data(a_data, m, k, b1, n, c1);
#pragma omp section
    Skink_tensor_matmul_data(a_data, m, k, b2, n, c2);
#pragma omp section
    Skink_tensor_matmul_data(a_data, m, k, b3, n, c3);
  }
}

// sigmoid computes the logistic sigmoid function 1 / (1 + exp(-x)).
static inline double sigmoid(double x) { return 1.0 / (1.0 + exp(-x)); }

#define LSTM_IB 64
#define LSTM_KB 50
#define LSTM_NB 40

// Fallback scalar update for the tail of one c row.
static void lstm_matmul_row_scalar(const double *__restrict a, int a_stride,
                                   const double *__restrict b,
                                   double *__restrict c, int i, int k, int n) {
  const double *a_row = a + i * a_stride;
  double *c_row = c + i * n;
  for (int k2 = 0; k2 < k; k2++) {
    double a_val = a_row[k2];
    const double *b_row = b + k2 * n;
    for (int j2 = 0; j2 < n; j2++) {
      c_row[j2] += a_val * b_row[j2];
    }
  }
}

// lstm_matmul_block computes a blocked matrix product with AVX2 or scalar fallback.
__attribute__((always_inline)) static inline void
lstm_matmul_block(const double *__restrict a, int a_stride,
                  const double *__restrict b, double *__restrict c, int i0,
                  int i1, int k, int n) {
  if (n % LSTM_NB != 0 || k % LSTM_KB != 0) {
    for (int i = i0; i < i1; i++) {
      for (int j = 0; j < n; j++)
        c[i * n + j] = 0.0;
      lstm_matmul_row_scalar(a, a_stride, b, c, i, k, n);
    }
    return;
  }

  for (int i = i0; i < i1; i++) {
    for (int j = 0; j < n; j++)
      c[i * n + j] = 0.0;
  }

#ifdef __AVX2__
  double bb[LSTM_KB * LSTM_NB] __attribute__((aligned(32)));

  for (int k0 = 0; k0 < k; k0 += LSTM_KB) {
    for (int j0 = 0; j0 < n; j0 += LSTM_NB) {
      // Pack a b block into contiguous L1 storage.
      for (int k2 = 0; k2 < LSTM_KB; k2++) {
        const double *b_row = b + (k0 + k2) * n + j0;
        double *bb_row = bb + k2 * LSTM_NB;
        _mm256_store_pd(bb_row + 0, _mm256_loadu_pd(b_row + 0));
        _mm256_store_pd(bb_row + 4, _mm256_loadu_pd(b_row + 4));
        _mm256_store_pd(bb_row + 8, _mm256_loadu_pd(b_row + 8));
        _mm256_store_pd(bb_row + 12, _mm256_loadu_pd(b_row + 12));
        _mm256_store_pd(bb_row + 16, _mm256_loadu_pd(b_row + 16));
        _mm256_store_pd(bb_row + 20, _mm256_loadu_pd(b_row + 20));
        _mm256_store_pd(bb_row + 24, _mm256_loadu_pd(b_row + 24));
        _mm256_store_pd(bb_row + 28, _mm256_loadu_pd(b_row + 28));
        _mm256_store_pd(bb_row + 32, _mm256_loadu_pd(b_row + 32));
        _mm256_store_pd(bb_row + 36, _mm256_loadu_pd(b_row + 36));
      }

      // Compute c for this i-block and the packed b block.
      for (int i = i0; i < i1; i++) {
        const double *a_row = a + i * a_stride + k0;
        double *c_row = c + i * n + j0;

        __m256d c0 = _mm256_loadu_pd(c_row + 0);
        __m256d c1 = _mm256_loadu_pd(c_row + 4);
        __m256d c2 = _mm256_loadu_pd(c_row + 8);
        __m256d c3 = _mm256_loadu_pd(c_row + 12);
        __m256d c4 = _mm256_loadu_pd(c_row + 16);
        __m256d c5 = _mm256_loadu_pd(c_row + 20);
        __m256d c6 = _mm256_loadu_pd(c_row + 24);
        __m256d c7 = _mm256_loadu_pd(c_row + 28);
        __m256d c8 = _mm256_loadu_pd(c_row + 32);
        __m256d c9 = _mm256_loadu_pd(c_row + 36);

        for (int k2 = 0; k2 < LSTM_KB; k2++) {
          const double *bb_row = bb + k2 * LSTM_NB;
          __m256d av = _mm256_set1_pd(a_row[k2]);
          c0 = _mm256_fmadd_pd(av, _mm256_load_pd(bb_row + 0), c0);
          c1 = _mm256_fmadd_pd(av, _mm256_load_pd(bb_row + 4), c1);
          c2 = _mm256_fmadd_pd(av, _mm256_load_pd(bb_row + 8), c2);
          c3 = _mm256_fmadd_pd(av, _mm256_load_pd(bb_row + 12), c3);
          c4 = _mm256_fmadd_pd(av, _mm256_load_pd(bb_row + 16), c4);
          c5 = _mm256_fmadd_pd(av, _mm256_load_pd(bb_row + 20), c5);
          c6 = _mm256_fmadd_pd(av, _mm256_load_pd(bb_row + 24), c6);
          c7 = _mm256_fmadd_pd(av, _mm256_load_pd(bb_row + 28), c7);
          c8 = _mm256_fmadd_pd(av, _mm256_load_pd(bb_row + 32), c8);
          c9 = _mm256_fmadd_pd(av, _mm256_load_pd(bb_row + 36), c9);
        }

        _mm256_storeu_pd(c_row + 0, c0);
        _mm256_storeu_pd(c_row + 4, c1);
        _mm256_storeu_pd(c_row + 8, c2);
        _mm256_storeu_pd(c_row + 12, c3);
        _mm256_storeu_pd(c_row + 16, c4);
        _mm256_storeu_pd(c_row + 20, c5);
        _mm256_storeu_pd(c_row + 24, c6);
        _mm256_storeu_pd(c_row + 28, c7);
        _mm256_storeu_pd(c_row + 32, c8);
        _mm256_storeu_pd(c_row + 36, c9);
      }
    }
  }
#else
  for (int i = i0; i < i1; i++) {
    lstm_matmul_row_scalar(a, a_stride, b, c, i, k, n);
  }
#endif
}

// Skink_lstm_forward runs a multi-layer LSTM forward pass over the input batch.
void Skink_lstm_forward(const double *input_data, int m, int input_size,
                        const int *sizes, int n_sizes, const double *weights,
                        const double *bias, double *h_data, int hidden_size) {
  input_data = (const double *)Skink_slice_data((void *)input_data);
  sizes = (const int *)Skink_slice_data((void *)sizes);
  weights = (const double *)Skink_slice_data((void *)weights);
  bias = (const double *)Skink_slice_data((void *)bias);
  h_data = (double *)Skink_slice_data((void *)h_data);
  int layers = n_sizes - 2;
  double *c = calloc((size_t)m * hidden_size, sizeof(double));
  if (!c)
    return;
  double *gin[4];
  double *gh[4];
  for (int g = 0; g < 4; g++) {
    gin[g] = malloc((size_t)m * hidden_size * sizeof(double));
    gh[g] = malloc((size_t)m * hidden_size * sizeof(double));
    if (!gin[g] || !gh[g]) {
      // Clean up what we can and abort
      for (int g2 = 0; g2 <= g; g2++) {
        if (gin[g2])
          free(gin[g2]);
        if (gh[g2])
          free(gh[g2]);
      }
      free(c);
      return;
    }
  }

  const double *b = bias;
  const double *w = weights;
  const double *x = input_data;
  int x_cols = input_size;

#pragma omp parallel
  {
    double *A[8];
    int K[8];
    const double *B[8];
    double *G[8];

    for (int l = 0; l < layers; l++) {
      int hidden = sizes[l + 1];
      int w_size = x_cols * hidden;
      int u_size = hidden * hidden;

      // Prepare per-gate inputs/outputs.
      for (int g = 0; g < 4; g++) {
        A[g] = (double *)x;
        K[g] = x_cols;
        B[g] = w + g * w_size;
        G[g] = gin[g];
      }
      for (int g = 0; g < 4; g++) {
        A[g + 4] = h_data;
        K[g + 4] = hidden;
        B[g + 4] = w + 4 * w_size + g * u_size;
        G[g + 4] = gh[g];
      }

// Zero cell state for this layer.
#pragma omp for
      for (int i = 0; i < m * hidden; i++) {
        c[i] = 0.0;
      }

// Compute all 8 gate projections in parallel across gates and i-blocks.
#pragma omp for collapse(2) schedule(static, 1)
      for (int g = 0; g < 8; g++) {
        for (int i0 = 0; i0 < m; i0 += LSTM_IB) {
          int i1 = i0 + LSTM_IB;
          if (i1 > m)
            i1 = m;
          lstm_matmul_block(A[g], K[g], B[g], G[g], i0, i1, K[g], hidden);
        }
      }

// Combine gates and update cell/hidden state.
#pragma omp for
      for (int i = 0; i < m * hidden; i++) {
        int col = i % hidden;
        double f = sigmoid(gin[0][i] + gh[0][i] + b[0 * hidden + col]);
        double ii = sigmoid(gin[1][i] + gh[1][i] + b[1 * hidden + col]);
        double gg = tanh(gin[2][i] + gh[2][i] + b[2 * hidden + col]);
        double o = sigmoid(gin[3][i] + gh[3][i] + b[3 * hidden + col]);
        c[i] = f * c[i] + ii * gg;
        h_data[i] = o * tanh(c[i]);
      }

#pragma omp single
      {
        w += 4 * w_size + 4 * u_size;
        b += 4 * hidden;
        x = h_data;
        x_cols = hidden;
      }
#pragma omp barrier
    }
  }

  free(c);
  for (int g = 0; g < 4; g++) {
    free(gin[g]);
    free(gh[g]);
  }
}

// Skink_panic triggers a panic with the given message (forward declaration).
void Skink_panic(char *msg);

// Skink_tensor_matmul multiplies two tensors and returns the result tensor.
void *Skink_tensor_matmul(void *a_ptr, void *b_ptr) {
  Tensor *a = (Tensor *)a_ptr;
  Tensor *b = (Tensor *)b_ptr;
  if (!a || !b || !a->data || !b->data) {
    Skink_panic((char *)"matmul: null tensor operand");
    return NULL;
  }
  if (a->cols != b->rows) {
    Skink_panic((char *)"matmul: dimension mismatch (a.cols != b.rows)");
    return NULL;
  }
  int m = a->rows;
  int n = b->cols;
  int p = a->cols;
  Tensor *c = malloc(sizeof(Tensor));
  if (!c)
    return NULL;
  c->rows = m;
  c->cols = n;
  c->data = calloc((size_t)m * n, sizeof(double));
  if (!c->data) {
    free(c);
    return NULL;
  }
  Skink_tensor_matmul_data(a->data, m, p, b->data, n, c->data);
  return c;
}

// Skink_tensor_get returns the element at the given row and column of a tensor.
double Skink_tensor_get(void *t_ptr, int row, int col) {
  Tensor *t = (Tensor *)t_ptr;
  if (!t || !t->data) {
    Skink_panic((char *)"tensor get: null tensor");
    return 0.0;
  }
  if (row < 0 || row >= t->rows || col < 0 || col >= t->cols) {
    Skink_panic((char *)"tensor get: index out of bounds");
    return 0.0;
  }
  return t->data[row * t->cols + col];
}

// Skink_tensor_free frees a tensor and its data buffer.
void Skink_tensor_free(void *t_ptr) {
  Tensor *t = (Tensor *)t_ptr;
  free(t->data);
  free(t);
}

// Skink_tensor_transpose returns a new tensor that is the transpose of the input.
void *Skink_tensor_transpose(void *t_ptr) {
  Tensor *t = (Tensor *)t_ptr;
  Tensor *res = malloc(sizeof(Tensor));
  if (!res)
    return NULL;
  res->rows = t->cols;
  res->cols = t->rows;
  res->data = malloc((size_t)t->rows * t->cols * sizeof(double));
  if (!res->data) {
    free(res);
    return NULL;
  }
  for (int i = 0; i < t->rows; i++) {
    for (int j = 0; j < t->cols; j++) {
      res->data[j * res->cols + i] = t->data[i * t->cols + j];
    }
  }
  return res;
}

// Skink_tensor_det computes the determinant of a square tensor (up to 3x3).
double Skink_tensor_det(void *t_ptr) {
  Tensor *t = (Tensor *)t_ptr;
  if (t->rows != t->cols)
    return 0.0;
  if (t->rows == 1)
    return t->data[0];
  if (t->rows == 2) {
    return t->data[0] * t->data[3] - t->data[1] * t->data[2];
  }
  if (t->rows == 3) {
    double a = t->data[0], b = t->data[1], c = t->data[2];
    double d = t->data[3], e = t->data[4], f = t->data[5];
    double g = t->data[6], h = t->data[7], i = t->data[8];
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  }
  return 0.0;
}

// Skink_tensor_inv computes the inverse of a square tensor (up to 3x3).
void *Skink_tensor_inv(void *t_ptr) {
  Tensor *t = (Tensor *)t_ptr;
  if (t->rows != t->cols)
    return NULL;
  double d = Skink_tensor_det(t);
  if (fabs(d) < 1e-9)
    return NULL;

  Tensor *res = malloc(sizeof(Tensor));
  if (!res)
    return NULL;
  res->rows = t->rows;
  res->cols = t->cols;
  res->data = malloc((size_t)t->rows * t->cols * sizeof(double));
  if (!res->data) {
    free(res);
    return NULL;
  }

  if (t->rows == 2) {
    res->data[0] = t->data[3] / d;
    res->data[1] = -t->data[1] / d;
    res->data[2] = -t->data[2] / d;
    res->data[3] = t->data[0] / d;
  } else if (t->rows == 3) {
    double a = t->data[0], b = t->data[1], c = t->data[2];
    double e = t->data[3], f = t->data[4], g = t->data[5];
    double h = t->data[6], i = t->data[7], j = t->data[8];
    res->data[0] = (f * j - g * i) / d;
    res->data[1] = (c * i - b * j) / d;
    res->data[2] = (b * g - c * f) / d;
    res->data[3] = (g * h - e * j) / d;
    res->data[4] = (a * j - c * h) / d;
    res->data[5] = (c * e - a * g) / d;
    res->data[6] = (e * i - f * h) / d;
    res->data[7] = (b * h - a * i) / d;
    res->data[8] = (a * f - b * e) / d;
  }
  return res;
}

// Skink_math_diff computes the numerical derivative of f at x using central differences.
// f is a Skink closure handle — a block whose first field is the code
// pointer, invoked as code(f, x). Calling the handle itself executes
// heap data as code (VULN-05).
double Skink_math_diff(void *f, double x) {
  double (*code)(void *, double) = *(double (**)(void *, double))f;
  double h = 1e-5;
  return (code(f, x + h) - code(f, x - h)) / (2.0 * h);
}

// Skink_math_integrate computes the definite integral of f from a to b using the trapezoidal rule.
double Skink_math_integrate(void *f, double a, double b) {
  double (*code)(void *, double) = *(double (**)(void *, double))f;
  int n = 1000;
  double h = (b - a) / n;
  double sum = 0.5 * (code(f, a) + code(f, b));
  for (int i = 1; i < n; i++) {
    sum += code(f, a + i * h);
  }
  return sum * h;
}

// Skink_tensor_gradient computes the numerical gradient of f with respect to the tensor x.
void *Skink_tensor_gradient(void *f, void *x_ptr) {
  double (*code)(void *, double *) = *(double (**)(void *, double *))f;
  Tensor *x = (Tensor *)x_ptr;
  Tensor *res = malloc(sizeof(Tensor));
  if (!res)
    return NULL;
  res->rows = x->rows;
  res->cols = x->cols;
  int n = x->rows * x->cols;
  res->data = malloc((size_t)n * sizeof(double));
  if (!res->data) {
    free(res);
    return NULL;
  }

  double *temp = malloc((size_t)n * sizeof(double));
  if (!temp) {
    free(res->data);
    free(res);
    return NULL;
  }
  memcpy(temp, x->data, n * sizeof(double));

  double h = 1e-5;
  for (int i = 0; i < n; i++) {
    double old = temp[i];
    temp[i] = old + h;
    double f1 = code(f, temp);
    temp[i] = old - h;
    double f2 = code(f, temp);
    temp[i] = old;
    res->data[i] = (f1 - f2) / (2.0 * h);
  }
  free(temp);
  return res;
}

// Skink_tensor_dot computes the dot product of two tensors treated as flat vectors.
double Skink_tensor_dot(void *a_ptr, void *b_ptr) {
  Tensor *a = (Tensor *)a_ptr;
  Tensor *b = (Tensor *)b_ptr;
  int n = a->rows * a->cols;
  double sum = 0.0;
  for (int i = 0; i < n; i++) {
    sum += a->data[i] * b->data[i];
  }
  return sum;
}

// Skink_tensor_cross computes the cross product of two 3-element vectors.
void *Skink_tensor_cross(void *a_ptr, void *b_ptr) {
  Tensor *a = (Tensor *)a_ptr;
  Tensor *b = (Tensor *)b_ptr;
  Tensor *res = malloc(sizeof(Tensor));
  if (!res)
    return NULL;
  res->rows = 3;
  res->cols = 1;
  res->data = malloc(3 * sizeof(double));
  if (!res->data) {
    free(res);
    return NULL;
  }
  res->data[0] = a->data[1] * b->data[2] - a->data[2] * b->data[1];
  res->data[1] = a->data[2] * b->data[0] - a->data[0] * b->data[2];
  res->data[2] = a->data[0] * b->data[1] - a->data[1] * b->data[0];
  return res;
}

// Skink_tensor_norm computes the L2 norm of a tensor treated as a flat vector.
double Skink_tensor_norm(void *t_ptr) {
  Tensor *t = (Tensor *)t_ptr;
  int n = t->rows * t->cols;
  double sum = 0.0;
  for (int i = 0; i < n; i++) {
    sum += t->data[i] * t->data[i];
  }
  return sqrt(sum);
}

// Skink_tensor_eigenvalues computes the eigenvalues of a 2x2 tensor.
void *Skink_tensor_eigenvalues(void *t_ptr) {
  Tensor *t = (Tensor *)t_ptr;
  Tensor *res = malloc(sizeof(Tensor));
  if (!res)
    return NULL;
  res->rows = 2;
  res->cols = 1;
  res->data = malloc(2 * sizeof(double));
  if (!res->data) {
    free(res);
    return NULL;
  }
  if (t->rows == 2 && t->cols == 2) {
    double a = t->data[0];
    double b = t->data[1];
    double c = t->data[2];
    double d = t->data[3];
    double trace = a + d;
    double det = a * d - b * c;
    double disc = sqrt(trace * trace - 4.0 * det);
    res->data[0] = (trace + disc) / 2.0;
    res->data[1] = (trace - disc) / 2.0;
  } else {
    res->data[0] = 0.0;
    res->data[1] = 0.0;
  }
  return res;
}

// Element-wise operations with SIMD and OpenMP
void Skink_tensor_add_data(const double *a_data, const double *b_data, int n,
                           double *c_data) {
  a_data = (const double *)Skink_slice_data((void *)a_data);
  b_data = (const double *)Skink_slice_data((void *)b_data);
  c_data = (double *)Skink_slice_data((void *)c_data);
  if (n > 10000) {
#pragma omp parallel for
    for (int i = 0; i < n; i++) {
      c_data[i] = a_data[i] + b_data[i];
    }
  } else {
#ifdef __AVX2__
    int i = 0;
    for (; i <= n - 4; i += 4) {
      __m256d av = _mm256_loadu_pd(a_data + i);
      __m256d bv = _mm256_loadu_pd(b_data + i);
      __m256d cv = _mm256_add_pd(av, bv);
      _mm256_storeu_pd(c_data + i, cv);
    }
    for (; i < n; i++) {
      c_data[i] = a_data[i] + b_data[i];
    }
#else
    for (int i = 0; i < n; i++) {
      c_data[i] = a_data[i] + b_data[i];
    }
#endif
  }
}

// Skink_tensor_sub_data subtracts element-wise b from a, storing the result in c.
void Skink_tensor_sub_data(const double *a_data, const double *b_data, int n,
                           double *c_data) {
  a_data = (const double *)Skink_slice_data((void *)a_data);
  b_data = (const double *)Skink_slice_data((void *)b_data);
  c_data = (double *)Skink_slice_data((void *)c_data);
  if (n > 10000) {
#pragma omp parallel for
    for (int i = 0; i < n; i++) {
      c_data[i] = a_data[i] - b_data[i];
    }
  } else {
#ifdef __AVX2__
    int i = 0;
    for (; i <= n - 4; i += 4) {
      __m256d av = _mm256_loadu_pd(a_data + i);
      __m256d bv = _mm256_loadu_pd(b_data + i);
      __m256d cv = _mm256_sub_pd(av, bv);
      _mm256_storeu_pd(c_data + i, cv);
    }
    for (; i < n; i++) {
      c_data[i] = a_data[i] - b_data[i];
    }
#else
    for (int i = 0; i < n; i++) {
      c_data[i] = a_data[i] - b_data[i];
    }
#endif
  }
}

// Skink_tensor_mul_data multiplies element-wise a by b, storing the result in c.
void Skink_tensor_mul_data(const double *a_data, const double *b_data, int n,
                           double *c_data) {
  a_data = (const double *)Skink_slice_data((void *)a_data);
  b_data = (const double *)Skink_slice_data((void *)b_data);
  c_data = (double *)Skink_slice_data((void *)c_data);
  if (n > 10000) {
#pragma omp parallel for
    for (int i = 0; i < n; i++) {
      c_data[i] = a_data[i] * b_data[i];
    }
  } else {
#ifdef __AVX2__
    int i = 0;
    for (; i <= n - 4; i += 4) {
      __m256d av = _mm256_loadu_pd(a_data + i);
      __m256d bv = _mm256_loadu_pd(b_data + i);
      __m256d cv = _mm256_mul_pd(av, bv);
      _mm256_storeu_pd(c_data + i, cv);
    }
    for (; i < n; i++) {
      c_data[i] = a_data[i] * b_data[i];
    }
#else
    for (int i = 0; i < n; i++) {
      c_data[i] = a_data[i] * b_data[i];
    }
#endif
  }
}

// Skink_tensor_scale_data scales each element of a by scalar s, storing the result in c.
void Skink_tensor_scale_data(const double *a_data, int n, double s,
                             double *c_data) {
  a_data = (const double *)Skink_slice_data((void *)a_data);
  c_data = (double *)Skink_slice_data((void *)c_data);
  if (n > 10000) {
#pragma omp parallel for
    for (int i = 0; i < n; i++) {
      c_data[i] = a_data[i] * s;
    }
  } else {
#ifdef __AVX2__
    __m256d sv = _mm256_set1_pd(s);
    int i = 0;
    for (; i <= n - 4; i += 4) {
      __m256d av = _mm256_loadu_pd(a_data + i);
      __m256d cv = _mm256_mul_pd(av, sv);
      _mm256_storeu_pd(c_data + i, cv);
    }
    for (; i < n; i++) {
      c_data[i] = a_data[i] * s;
    }
#else
    for (int i = 0; i < n; i++) {
      c_data[i] = a_data[i] * s;
    }
#endif
  }
}

// Transpose with OpenMP
void Skink_tensor_transpose_data(const double *a_data, int rows, int cols,
                                 double *c_data) {
  a_data = (const double *)Skink_slice_data((void *)a_data);
  c_data = (double *)Skink_slice_data((void *)c_data);
  if (rows * cols > 10000) {
#pragma omp parallel for
    for (int i = 0; i < rows; i++) {
      for (int j = 0; j < cols; j++) {
        c_data[j * rows + i] = a_data[i * cols + j];
      }
    }
  } else {
    for (int i = 0; i < rows; i++) {
      for (int j = 0; j < cols; j++) {
        c_data[j * rows + i] = a_data[i * cols + j];
      }
    }
  }
}

// Vector helpers for the Skink `dot`/`norm` builtins.
// A Skink []float slice value is a pointer to a slice header
// (rc_rt.c); Skink_slice_data extracts the double payload and
// Skink_slice_len the element count. Both are NULL-safe.
static int64_t skink_slice_len(const void *p) {
  return Skink_slice_len((void *)p);
}

// Skink_vec_dot computes the dot product of two float64 slices.
double Skink_vec_dot(void *a_ptr, void *b_ptr) {
  int64_t na = skink_slice_len(a_ptr);
  int64_t nb = skink_slice_len(b_ptr);
  const double *a = (const double *)Skink_slice_data(a_ptr);
  const double *b = (const double *)Skink_slice_data(b_ptr);
  int64_t n = na < nb ? na : nb;
  double sum = 0.0;
  for (int64_t i = 0; i < n; i++) {
    sum += a[i] * b[i];
  }
  return sum;
}

// Skink_vec_norm computes the L2 norm of a float64 slice.
double Skink_vec_norm(void *v_ptr) {
  int64_t n = skink_slice_len(v_ptr);
  const double *v = (const double *)Skink_slice_data(v_ptr);
  double sum = 0.0;
  for (int64_t i = 0; i < n; i++) {
    sum += v[i] * v[i];
  }
  return sqrt(sum);
}

// Activation functions with SIMD
void Skink_tensor_relu_data(const double *a_data, int n, double *c_data) {
  a_data = (const double *)Skink_slice_data((void *)a_data);
  c_data = (double *)Skink_slice_data((void *)c_data);
#ifdef __AVX2__
  __m256d zero = _mm256_setzero_pd();
  int i = 0;
  for (; i <= n - 4; i += 4) {
    __m256d av = _mm256_loadu_pd(a_data + i);
    __m256d cv = _mm256_max_pd(av, zero);
    _mm256_storeu_pd(c_data + i, cv);
  }
  for (; i < n; i++) {
    c_data[i] = a_data[i] > 0.0 ? a_data[i] : 0.0;
  }
#else
  for (int i = 0; i < n; i++) {
    c_data[i] = a_data[i] > 0.0 ? a_data[i] : 0.0;
  }
#endif
}

// Skink_tensor_sigmoid_data applies the sigmoid activation function element-wise.
void Skink_tensor_sigmoid_data(const double *a_data, int n, double *c_data) {
  a_data = (const double *)Skink_slice_data((void *)a_data);
  c_data = (double *)Skink_slice_data((void *)c_data);
  for (int i = 0; i < n; i++) {
    c_data[i] = 1.0 / (1.0 + exp(-a_data[i]));
  }
}

// Skink_tensor_tanh_data applies the tanh activation function element-wise.
void Skink_tensor_tanh_data(const double *a_data, int n, double *c_data) {
  a_data = (const double *)Skink_slice_data((void *)a_data);
  c_data = (double *)Skink_slice_data((void *)c_data);
  for (int i = 0; i < n; i++) {
    double x = a_data[i];
    c_data[i] = tanh(x);
  }
}

// Softmax along last dimension
void Skink_tensor_softmax_data(const double *a_data, int rows, int cols,
                               double *c_data) {
  a_data = (const double *)Skink_slice_data((void *)a_data);
  c_data = (double *)Skink_slice_data((void *)c_data);
#pragma omp parallel for
  for (int i = 0; i < rows; i++) {
    const double *row = a_data + i * cols;
    double *crow = c_data + i * cols;

    // Find max for numerical stability
    double max_val = row[0];
    for (int j = 1; j < cols; j++) {
      if (row[j] > max_val)
        max_val = row[j];
    }

    // Compute exp and sum
    double sum = 0.0;
    for (int j = 0; j < cols; j++) {
      double exp_val = exp(row[j] - max_val);
      crow[j] = exp_val;
      sum += exp_val;
    }

    // Normalize
    for (int j = 0; j < cols; j++) {
      crow[j] /= sum;
    }
  }
}

// skink_check_conv_dims validates conv/pool parameters: stride==0 used
// to fault with SIGFPE, and kernel>input produced a negative output
// dimension and unchecked negative indexing (VULN-21).
static void skink_check_conv_dims(int in_h, int in_w, int kernel, int stride) {
  if (stride <= 0) {
    Skink_panic((char *)"tensor: stride must be positive");
  }
  if (kernel <= 0 || in_h <= 0 || in_w <= 0 || kernel > in_h || kernel > in_w) {
    Skink_panic((char *)"tensor: kernel size exceeds input dimensions");
  }
}

// 2D Convolution with tiling
void Skink_tensor_conv2d_data(const double *input_data, int batch, int in_ch,
                              int in_h, int in_w, const double *filter_data,
                              int out_ch, int kernel, int stride,
                              double *output_data) {
  input_data = (const double *)Skink_slice_data((void *)input_data);
  filter_data = (const double *)Skink_slice_data((void *)filter_data);
  output_data = (double *)Skink_slice_data((void *)output_data);
  skink_check_conv_dims(in_h, in_w, kernel, stride);
  int out_h = (in_h - kernel) / stride + 1;
  int out_w = (in_w - kernel) / stride + 1;

#pragma omp parallel for collapse(4)
  for (int b = 0; b < batch; b++) {
    for (int oc = 0; oc < out_ch; oc++) {
      for (int oh = 0; oh < out_h; oh++) {
        for (int ow = 0; ow < out_w; ow++) {
          double sum = 0.0;
          for (int ic = 0; ic < in_ch; ic++) {
            for (int kh = 0; kh < kernel; kh++) {
              for (int kw = 0; kw < kernel; kw++) {
                int ih = oh * stride + kh;
                int iw = ow * stride + kw;
                int input_idx = ((b * in_ch + ic) * in_h + ih) * in_w + iw;
                int filter_idx =
                    ((oc * in_ch + ic) * kernel + kh) * kernel + kw;
                sum += input_data[input_idx] * filter_data[filter_idx];
              }
            }
          }
          int output_idx = ((b * out_ch + oc) * out_h + oh) * out_w + ow;
          output_data[output_idx] = sum;
        }
      }
    }
  }
}

// 2D Max Pooling
void Skink_tensor_maxpool2d_data(const double *input_data, int batch,
                                 int channels, int in_h, int in_w, int kernel,
                                 int stride, double *output_data) {
  input_data = (const double *)Skink_slice_data((void *)input_data);
  output_data = (double *)Skink_slice_data((void *)output_data);
  skink_check_conv_dims(in_h, in_w, kernel, stride);
  int out_h = (in_h - kernel) / stride + 1;
  int out_w = (in_w - kernel) / stride + 1;

#pragma omp parallel for collapse(3)
  for (int b = 0; b < batch; b++) {
    for (int c = 0; c < channels; c++) {
      for (int oh = 0; oh < out_h; oh++) {
        for (int ow = 0; ow < out_w; ow++) {
          double max_val =
              input_data[((b * channels + c) * in_h + oh * stride) * in_w +
                         ow * stride];
          for (int kh = 0; kh < kernel; kh++) {
            for (int kw = 0; kw < kernel; kw++) {
              int ih = oh * stride + kh;
              int iw = ow * stride + kw;
              double v =
                  input_data[((b * channels + c) * in_h + ih) * in_w + iw];
              if (v > max_val)
                max_val = v;
            }
          }
          output_data[((b * channels + c) * out_h + oh) * out_w + ow] = max_val;
        }
      }
    }
  }
}

// Sum all elements
double Skink_tensor_sum_data(const double *a_data, int n) {
  a_data = (const double *)Skink_slice_data((void *)a_data);
  double sum = 0.0;
#ifdef __AVX2__
  __m256d sum_vec = _mm256_setzero_pd();
  int i = 0;
  for (; i <= n - 4; i += 4) {
    __m256d av = _mm256_loadu_pd(a_data + i);
    sum_vec = _mm256_add_pd(sum_vec, av);
  }
  // Horizontal sum
  double tmp[4];
  _mm256_storeu_pd(tmp, sum_vec);
  sum = tmp[0] + tmp[1] + tmp[2] + tmp[3];
  for (; i < n; i++) {
    sum += a_data[i];
  }
#else
#pragma omp parallel for reduction(+ : sum)
  for (int i = 0; i < n; i++) {
    sum += a_data[i];
  }
#endif
  return sum;
}
