/* lu: LU decomposition with partial pivoting of a 96x96 double matrix,
 * repeated. Inner loops are scalar fmsub chains; pivoting uses fabs,
 * fcmp and conditional branches; the solve uses fdiv. */
#include "fpbench.h"
#define M 96
static double A[M][M], B[M];
static int lu(void) {
  int perm = 0;
  for (int k = 0; k < M; k++) {
    int p = k; double best = fabs(A[k][k]);
    for (int i = k + 1; i < M; i++) { double v = fabs(A[i][k]); if (v > best) { best = v; p = i; } }
    if (p != k) { for (int j = 0; j < M; j++) { double t = A[k][j]; A[k][j] = A[p][j]; A[p][j] = t; } double t = B[k]; B[k] = B[p]; B[p] = t; perm++; }
    double inv = 1.0 / A[k][k];
    for (int i = k + 1; i < M; i++) {
      double f = A[i][k] * inv; A[i][k] = f;
      for (int j = k + 1; j < M; j++) A[i][j] -= f * A[k][j];
      B[i] -= f * B[k];
    }
  }
  for (int i = M - 1; i >= 0; i--) { double s = B[i]; for (int j = i + 1; j < M; j++) s -= A[i][j] * B[j]; B[i] = s / A[i][i]; }
  return perm;
}
static uint64_t run(long scale) {
  uint64_t h = 0;
  for (long r = 0; r < scale; r++) {
    for (int i = 0; i < M; i++) { for (int j = 0; j < M; j++) A[i][j] = fp_urand() - 0.5; A[i][i] += 4.0; B[i] = fp_urand(); }
    int perm = lu();
    h = fp_hash(h, (uint64_t)perm);
    for (int i = 0; i < M; i += 7) h = hash_double(h, B[i]);
  }
  return h;
}
FPBENCH_MAIN(run, 40)
