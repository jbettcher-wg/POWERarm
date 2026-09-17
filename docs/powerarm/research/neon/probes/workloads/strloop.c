/* strloop: hot-loop string workload for POWERarm profiling. Repeats glibc's
 * NEON string routines over a buffer so the SIMD loop dominates. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int main(int argc, char** argv) {
  size_t len = argc > 1 ? strtoul(argv[1], 0, 10) : 4096; int reps = argc > 2 ? atoi(argv[2]) : 20000;
  char* buf = malloc(len + 64); for (size_t i = 0; i < len; i++) buf[i] = 'a' + (i % 23); buf[len] = 0;
  uint64_t acc = 0;
  for (int r = 0; r < reps; r++) {
    __asm__ volatile("" : : "r"(buf) : "memory");            /* the compiler must not hoist the pure calls */
    acc += strlen(buf);                                   /* __strlen_asimd: cmeq/shrn/fmov/cbz */
    acc += (uintptr_t)memchr(buf, 'z' + 1, len) & 0xff;  /* __memchr_generic: cmeq/umaxp/fmov/cbz */
    acc += (uintptr_t)strchr(buf, 'Z') & 0xff;           /* strchr: cmeq/cmhs/bit/addp */
    acc += memcmp(buf, buf, len) + 1;                     /* memcmp: ldp/eor/... */
    acc += strnlen(buf, len);
    buf[r % len] = 'a' + (r % 23);                          /* touch the buffer so nothing is loop-invariant */
  }
  printf("%llu\n", (unsigned long long)acc); return 0;
}
