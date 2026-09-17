/* SPDX-License-Identifier: MIT */
/* printf/strtod float formatting and libm through the shared glibc: the
 * IFUNC-selected string and memory routines, and libm's own variants. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
	const double v[] = { 0.0, -0.0, 1.0 / 3.0, 2.5, -2.5, 1e300, 5e-324, 123456789.123456789, 0.1 };
	for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++)
		printf("%zu: %.17g %e %a %f\n", i, v[i], v[i], v[i], v[i]);
	printf("float: %.9g %g\n", (double)(float)(1.0 / 3.0), (double)3.4028235e38f);
	printf("strtod: %.17g %.17g %.17g\n", strtod("0.1", NULL), strtod("1e-310", NULL), strtod("0x1.8p1", NULL));
	printf("libm: sin=%.17g exp=%.17g pow=%.17g log10=%.17g\n", sin(1.0), exp(1.5), pow(2.0, 0.5), log10(12345.0));
	printf("libm: floor=%g ceil=%g trunc=%g round=%g rint=%g lrint=%ld nearbyint=%g\n", floor(-2.5), ceil(-2.5),
	       trunc(-2.7), round(2.5), rint(2.5), lrint(3.5), nearbyint(-3.5));
	printf("libm: fmod=%.17g hypot=%.17g cbrt=%.17g atan2=%.17g\n", fmod(10.5, 3.0), hypot(3e200, 4e200), cbrt(-27.0),
	       atan2(-1.0, -1.0));
	char big[4096];
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	char *copy = strdup(big);
	printf("strings: len=%zu cmp=%d chr=%td\n", strlen(copy), strcmp(copy, big), strchr(copy, 0) - copy);
	free(copy);
	return 0;
}
