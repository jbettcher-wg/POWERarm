/* Recursive quicksort of 32-bit integers through a comparator function pointer, as libc
 * qsort does. Stresses: unpredictable data-dependent branches, indirect calls, and deep
 * call/return pairs (guest BL/RET), then a verification pass. */
static const char BENCH_NAME[] = "sort";
static const unsigned long BENCH_DEFAULT_SCALE = 2500000; /* elements */
#include "bench.h"

#define MAX_N (8u << 20)
static u32 src[MAX_N], work[MAX_N];

typedef int (*cmp_fn)(u32, u32);
__attribute__((noinline)) static int cmp_u32(u32 a, u32 b) { return (a > b) - (a < b); }

__attribute__((noinline)) static void qsort_u32(u32 *a, i64 lo, i64 hi, cmp_fn cmp) {
  while (hi - lo > 16) {
    i64 mid = lo + (hi - lo) / 2;
    if (cmp(a[mid], a[lo]) < 0) { u32 t = a[mid]; a[mid] = a[lo]; a[lo] = t; }
    if (cmp(a[hi], a[lo]) < 0) { u32 t = a[hi]; a[hi] = a[lo]; a[lo] = t; }
    if (cmp(a[hi], a[mid]) < 0) { u32 t = a[hi]; a[hi] = a[mid]; a[mid] = t; }
    u32 p = a[mid];
    i64 i = lo, j = hi;
    while (i <= j) {
      while (cmp(a[i], p) < 0) i++;
      while (cmp(a[j], p) > 0) j--;
      if (i <= j) { u32 t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
    }
    if (j - lo < hi - i) { qsort_u32(a, lo, j, cmp); lo = i; }
    else { qsort_u32(a, i, hi, cmp); hi = j; }
  }
  for (i64 i = lo + 1; i <= hi; i++) {
    u32 v = a[i];
    i64 j = i - 1;
    while (j >= lo && cmp(a[j], v) > 0) { a[j + 1] = a[j]; j--; }
    a[j + 1] = v;
  }
}

static void bench_setup(u64 scale) {
  if (scale > MAX_N) scale = MAX_N;
  for (u64 i = 0; i < scale; i++) src[i] = (u32)(bench_rand() >> 32);
}

static u64 bench_run(u64 scale) {
  if (scale > MAX_N) scale = MAX_N;
  for (u64 i = 0; i < scale; i++) work[i] = src[i];
  qsort_u32(work, 0, (i64)scale - 1, cmp_u32);
  u64 h = 0, bad = 0;
  for (u64 i = 0; i < scale; i++) {
    if (i && work[i - 1] > work[i]) bad++;
    h = (h ^ work[i]) * 0x100000001B3ul;
  }
  return h ^ (bad << 56);
}
