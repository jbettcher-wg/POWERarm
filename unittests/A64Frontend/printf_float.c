// SPDX-License-Identifier: MIT
// printf floating-point formatting: %f, %e, %g, %a over an edge corpus.
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const double values[] = {
  0.0, -0.0, 1.0, -1.0, 0.5, 0.1, 0.25, 3.141592653589793, 2.718281828459045, 1e-5, 1e-300,
  1e300, 123456789.125, -987654321.0625, 1.0 / 3.0, 2.0 / 3.0, 9.999999, 99.5, 0.05, 1e21, 1e22,
  4503599627370496.5, 9007199254740993.0, DBL_MAX, DBL_MIN, DBL_TRUE_MIN, DBL_EPSILON,
  HUGE_VAL, -HUGE_VAL, NAN, 6.02214076e23, 1.602176634e-19,
};

int main(void) {
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    double v = values[i];
    printf("%zu f=%f e=%e g=%g a=%a\n", i, v, v, v, v);
    printf("%zu .0f=%.0f .3f=%.3f .17g=%.17g 12.4e=%12.4e -10.2f=%-10.2f| +g=%+g #g=%#g\n", i, v, v, v, v, v, v, v);
    float f = (float)v;
    printf("%zu float f=%f g=%.9g a=%a\n", i, f, f, f);
  }
  long double ld = 1.0L / 3.0L;
  printf("Lf=%Lf Lg=%.30Lg La=%La\n", ld, ld, ld);
  char buf[64];
  for (int i = -8; i <= 8; ++i) {
    snprintf(buf, sizeof buf, "%.*g", i < 0 ? -i : i, 1234.5678 * i);
    double back = strtod(buf, NULL);
    printf("%d %s %a\n", i, buf, back);
  }
  printf("strtod %a %a %a\n", strtod("0.1", NULL), strtod("1e-320", NULL), strtof("3.4028235e38", NULL));
  return 0;
}
