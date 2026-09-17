/* fmix32: single-precision mix -- a particle update with float arithmetic,
 * fcmp + fcsel clamps (min/max via ternaries), fsqrt, and a float -> int ->
 * float quantisation step (fcvtzs, scvtf). Data-dependent selects. */
#include "fpbench.h"
#define NP 4096
static float px[NP], py[NP], vx[NP], vy[NP];
static uint64_t run(long scale) {
  for (int i = 0; i < NP; i++) { px[i] = (float)fp_urand() * 100.f; py[i] = (float)fp_urand() * 100.f; vx[i] = (float)fp_urand() - 0.5f; vy[i] = (float)fp_urand() - 0.5f; }
  int32_t acc = 0; float fsum = 0;
  for (long s = 0; s < scale; s++) {
    for (int i = 0; i < NP; i++) {
      float x = px[i] + vx[i], y = py[i] + vy[i];
      float sp = sqrtf(vx[i] * vx[i] + vy[i] * vy[i]);
      float lim = 0.75f;
      float k = sp > lim ? lim / sp : 1.0f;            /* fcmp + fcsel + fdiv */
      vx[i] = (x < 0.f || x > 100.f) ? -vx[i] * k : vx[i] * k;
      vy[i] = (y < 0.f || y > 100.f) ? -vy[i] * k : vy[i] * k;
      x = x < 0.f ? 0.f : (x > 100.f ? 100.f : x);     /* fminnm/fmaxnm-style clamps */
      y = y < 0.f ? 0.f : (y > 100.f ? 100.f : y);
      int32_t qx = (int32_t)(x * 655.36f), qy = (int32_t)(y * 655.36f);  /* fcvtzs */
      acc += qx ^ (qy << 3);
      px[i] = (float)qx * (1.0f / 655.36f);           /* scvtf */
      py[i] = (float)qy * (1.0f / 655.36f);
      fsum += vx[i] - vy[i];
    }
  }
  uint64_t h = fp_hash(0, (uint32_t)acc);
  h = hash_double(h, (double)fsum);
  for (int i = 0; i < NP; i += 97) h = hash_double(h, (double)px[i] + py[i]);
  return h;
}
FPBENCH_MAIN(run, 300)
