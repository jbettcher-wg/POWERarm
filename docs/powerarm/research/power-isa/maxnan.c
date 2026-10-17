/* Does POWER's xvmax/xvmin really follow A64's FMAXNM/FMINNM rule?
 *
 * POWER-STRENGTHS 2.7 proposes checking the RESULT for NaN instead of both
 * INPUTS, which is sound only if a single-QNaN input gives the same answer on
 * hardware as A64 demands -- that case would no longer reach the cold stub. The
 * doc cites that from ISA 3.0B [SPEC]. This asks the chip. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef double v2d __attribute__((vector_size(16)));
typedef float v4f __attribute__((vector_size(16)));

static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double dbl(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static uint32_t bits32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float flt(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

#define QNAN_A 0x7FF8000000000001ULL   /* quiet, payload 1 */
#define QNAN_B 0x7FF800000000ABCDULL   /* quiet, payload ABCD */
#define SNAN_A 0x7FF0000000000001ULL   /* signalling */
#define NEG_QNAN 0xFFF8000000000042ULL

static double maxdp(double a, double b) {
  v2d x = {a, a}, y = {b, b}, r;
  __asm__ volatile("xvmaxdp %x0,%x1,%x2" : "=wa"(r) : "wa"(x), "wa"(y));
  return r[0];
}
static double mindp(double a, double b) {
  v2d x = {a, a}, y = {b, b}, r;
  __asm__ volatile("xvmindp %x0,%x1,%x2" : "=wa"(r) : "wa"(x), "wa"(y));
  return r[0];
}
static float maxsp(float a, float b) {
  v4f x = {a,a,a,a}, y = {b,b,b,b}, r;
  __asm__ volatile("xvmaxsp %x0,%x1,%x2" : "=wa"(r) : "wa"(x), "wa"(y));
  return r[0];
}

static void row(const char* what, double a, double b) {
  printf("  %-26s a=%016llx b=%016llx  max=%016llx  min=%016llx\n", what,
         (unsigned long long)bits(a), (unsigned long long)bits(b),
         (unsigned long long)bits(maxdp(a, b)), (unsigned long long)bits(mindp(a, b)));
}

int main(void) {
  printf("xvmaxdp / xvmindp, f64 (lane 0):\n");
  row("(QNaN_A, 5.0)", dbl(QNAN_A), 5.0);
  row("(5.0, QNaN_A)", 5.0, dbl(QNAN_A));
  row("(QNaN_A, QNaN_B)", dbl(QNAN_A), dbl(QNAN_B));
  row("(QNaN_B, QNaN_A)", dbl(QNAN_B), dbl(QNAN_A));
  row("(SNaN_A, 5.0)", dbl(SNAN_A), 5.0);
  row("(5.0, SNaN_A)", 5.0, dbl(SNAN_A));
  row("(SNaN_A, QNaN_A)", dbl(SNAN_A), dbl(QNAN_A));
  row("(-QNaN, 5.0)", dbl(NEG_QNAN), 5.0);
  row("(+0, -0)", 0.0, -0.0);
  row("(-0, +0)", -0.0, 0.0);
  row("(+Inf, 5.0)", dbl(0x7FF0000000000000ULL), 5.0);
  row("(3.0, 5.0)", 3.0, 5.0);
  printf("\nxvmaxsp, f32 (lane 0): single-QNaN only, the case that matters\n");
  printf("  (QNaN, 5.0f) -> %08x ; (5.0f, QNaN) -> %08x ; (3,5) -> %08x\n",
         bits32(maxsp(flt(0x7FC00001u), 5.0f)), bits32(maxsp(5.0f, flt(0x7FC00001u))),
         bits32(maxsp(3.0f, 5.0f)));
  printf("\nA64 requires, for FMAXNM/FMINNM: a single QNaN input returns THE NUMBER;\n");
  printf("5.0 is 4014000000000000, 3.0 is 4008000000000000, 5.0f is 40a00000.\n");
  return 0;
}
