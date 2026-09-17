/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	printf("hello, dynamic world: argc=%d len=%zu\n", argc, strlen(argv[0]) > 0 ? (size_t)1 : (size_t)0);
	for (int i = 1; i < argc; i++)
		printf("arg%d=%s\n", i, argv[i]);
	return 0;
}
