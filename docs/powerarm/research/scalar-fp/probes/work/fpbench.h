/* Shared harness for the scalar-FP workloads. Static glibc programs:
 * usage <prog> [scale] [reps]; prints ns= and sum= per repetition and a
 * parity line at the end, like unittests/A64Bench. */
#ifndef FPBENCH_H
#define FPBENCH_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>

static uint64_t now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t fp_rng = 0x9E3779B97F4A7C15ull;
static uint64_t fp_rnd(void) { fp_rng ^= fp_rng << 13; fp_rng ^= fp_rng >> 7; fp_rng ^= fp_rng << 17; return fp_rng; }
static double fp_urand(void) { return (double)(fp_rnd() >> 11) * (1.0 / 9007199254740992.0); }
static uint64_t fp_hash(uint64_t h, uint64_t v) { h ^= v; h *= 0x100000001B3ull; return h ^ (h >> 29); }
static uint64_t hash_double(uint64_t h, double d) { uint64_t u; memcpy(&u, &d, 8); return fp_hash(h, u); }

#define FPBENCH_MAIN(run_fn, default_scale) \
  int main(int argc, char** argv) { \
    long scale = argc > 1 ? atol(argv[1]) : (default_scale); \
    int reps = argc > 2 ? atoi(argv[2]) : 3; \
    uint64_t first = 0; int ok = 1; \
    for (int r = 0; r < reps; r++) { \
      fp_rng = 0x9E3779B97F4A7C15ull; \
      uint64_t t0 = now_ns(); uint64_t sum = run_fn(scale); uint64_t t1 = now_ns(); \
      printf("rep=%d ns=%llu sum=%016llx\n", r, (unsigned long long)(t1 - t0), (unsigned long long)sum); \
      if (r == 0) first = sum; else if (sum != first) ok = 0; \
    } \
    printf("parity=%s\n", ok ? "ok" : "MISMATCH"); \
    return ok ? 0 : 1; \
  }
#endif
