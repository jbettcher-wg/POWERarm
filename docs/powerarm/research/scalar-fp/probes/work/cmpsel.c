/* cmpsel: compare/select bound -- ray/AABB slab tests with fmin/fmax
 * (fminnm/fmaxnm from fmin()/fmax(), fcsel from ternaries), fcmp + b.cond
 * on data-dependent doubles, and a branchy insertion sort of doubles. */
#include "fpbench.h"
#define NB 2048
static double bmin[NB][3], bmax[NB][3];
static double keys[256];
static uint64_t run(long scale) {
  for (int i = 0; i < NB; i++) for (int k = 0; k < 3; k++) { double a = fp_urand() * 100, b = fp_urand() * 100; bmin[i][k] = a < b ? a : b; bmax[i][k] = a < b ? b : a; }
  uint64_t hits = 0; double tsum = 0;
  for (long s = 0; s < scale; s++) {
    double o[3] = { fp_urand() * 100, fp_urand() * 100, fp_urand() * 100 };
    double d[3] = { fp_urand() - 0.5, fp_urand() - 0.5, fp_urand() - 0.5 };
    double inv[3] = { 1.0 / d[0], 1.0 / d[1], 1.0 / d[2] };
    for (int i = 0; i < NB; i++) {
      double tmin = 0.0, tmax = 1e30;
      for (int k = 0; k < 3; k++) {
        double t0 = (bmin[i][k] - o[k]) * inv[k], t1 = (bmax[i][k] - o[k]) * inv[k];
        double tn = fmin(t0, t1), tf = fmax(t0, t1);        /* fminnm/fmaxnm */
        tmin = tmin > tn ? tmin : tn;                       /* fcmp + fcsel */
        tmax = tmax < tf ? tmax : tf;
      }
      if (tmax >= tmin) { hits++; tsum += tmin; }           /* fcmp + b.cond, data dependent */
    }
    /* insertion sort of 256 doubles: fcmp + b.cond, mispredict-prone */
    for (int i = 0; i < 256; i++) keys[i] = fp_urand();
    for (int i = 1; i < 256; i++) { double k = keys[i]; int j = i - 1; while (j >= 0 && keys[j] > k) { keys[j + 1] = keys[j]; j--; } keys[j + 1] = k; }
    tsum += keys[0] + keys[255];
  }
  uint64_t h = fp_hash(0, hits); h = hash_double(h, tsum);
  return h;
}
FPBENCH_MAIN(run, 2000)
