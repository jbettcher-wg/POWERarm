/* cvtmix: conversion-heavy double kernel -- fixed-point quantisation and
 * dequantisation (fcvtzs/fcvtzu/scvtf/ucvtf), lrint-style rounding
 * lround (fcvtas), a floor written with conversions (fcvtzs/scvtf/fcmp,
 * because the frontend has no FRINT*: lrint and floor lower to frintx/
 * frintm, which POWERarm reports as unimplemented), and a double<->float
 * precision round trip (fcvt). */
#include "fpbench.h"
#define NV 8192
static double v[NV];
static uint64_t run(long scale) {
  for (int i = 0; i < NV; i++) v[i] = (fp_urand() - 0.5) * 2.0e6;
  int64_t acc = 0; uint64_t uacc = 0; double dacc = 0;
  for (long s = 0; s < scale; s++) {
    for (int i = 0; i < NV; i++) {
      double x = v[i];
      int64_t q = (int64_t)(x * 64.0);            /* fcvtzs (fixed-point shape) */
      uint64_t uq = (uint64_t)(x < 0 ? -x : x);   /* fcvtzu */
      long a = lround(x);                         /* fcvtas */
      double hx = x * 0.5; long fi = (long)hx; double fl = (double)fi; if (hx < fl) fl -= 1.0;   /* floor via fcvtzs/scvtf/fcmp */
      long r = fi;
      float f = (float)x;                         /* fcvt s,d */
      double back = (double)f;                    /* fcvt d,s */
      int32_t w = (int32_t)(x * (1.0 / 1024.0));  /* fcvtzs w,d */
      acc += q + r + a + w;
      uacc += uq;
      dacc += (double)q * (1.0 / 64.0) + (double)uq + back - fl;   /* scvtf, ucvtf */
      v[i] = x * 0.999 + (double)(int32_t)(acc & 0xFFFF) * 1e-3;
    }
  }
  uint64_t h = fp_hash(0, (uint64_t)acc); h = fp_hash(h, uacc); h = hash_double(h, dacc);
  for (int i = 0; i < NV; i += 101) h = hash_double(h, v[i]);
  return h;
}
FPBENCH_MAIN(run, 150)
