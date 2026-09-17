/* SPDX-License-Identifier: MIT */
/* libc-heavy: stdio formatting, malloc, qsort, string and math routines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int cmp(const void *a, const void *b)
{
	const char *const *x = a, *const *y = b;
	return strcmp(*x, *y);
}

int main(int argc, char **argv)
{
	const char *words[] = { "pear", "apple", "fig", "banana", "cherry", "date", "elderberry" };
	size_t n = sizeof(words) / sizeof(words[0]);
	const char **copy = malloc(n * sizeof(*copy));
	memcpy(copy, words, n * sizeof(*copy));
	qsort(copy, n, sizeof(*copy), cmp);
	for (size_t i = 0; i < n; i++)
		printf("%zu:%s%s", i, copy[i], i + 1 < n ? " " : "\n");
	free(copy);

	char buf[128];
	snprintf(buf, sizeof(buf), "%08.3f|%-6d|%x|%e|%s", 3.14159, -42, 48879, 12345.678, "end");
	printf("%s len=%zu\n", buf, strlen(buf));

	double v = strtod("2.5e3", NULL);
	printf("strtod=%.1f sqrt=%.6f pow=%.3f floor=%.1f\n", v, sqrt(2.0), pow(1.5, 7.0), floor(-2.5));
	long l = strtol("-0x7fff", NULL, 16);
	printf("strtol=%ld atoi=%d\n", l, atoi("  123abc"));

	char *dup = strdup("The Quick Brown Fox");
	for (char *p = dup; *p; p++)
		if (*p >= 'A' && *p <= 'Z')
			*p += 'a' - 'A';
	printf("lower=%s strstr=%s argc=%d\n", dup, strstr(dup, "brown"), argc);
	free(dup);
	return 3;
}
