// SPDX-License-Identifier: MIT
// Scalar floating-point arithmetic, compares and conversions in libc-shaped code.
#include <math.h>
#include <stdio.h>

volatile double vals[] = {0.0, -0.0, 1.5, -2.25, 1e10, -1e-10, 3e38, 1e300, 9.2233720368547758e18, -9.3e18, 4294967295.5, -2147483648.5};

int main(void) {
  double acc = 0;
  for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; ++i) {
    double a = vals[i], b = vals[(i + 3) % 12];
    long long ll = (long long)a;
    unsigned long long ull = (unsigned long long)a;
    int ii = (int)a;
    unsigned ui = (unsigned)a;
    float f = (float)a;
    acc += a * b - a / (b ? b : 1) + sqrt(fabs(a));
    printf("%u %lld %llu %d %u %a %a %d %d %a %a %lld %ld\n", i, ll, ull, ii, ui, (double)f, acc, a < b, a == b, fmin(a, b), fmax(a, b),
           (long long)(double)ll, lround(a / 1e9));
  }
  return 0;
}
