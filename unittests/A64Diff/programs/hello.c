/* SPDX-License-Identifier: MIT */
/* Program-level corpus: argv, stdout, stderr and the exit code must match the golden run. */
#include <stdio.h>
int main(int argc, char **argv) {
  printf("Hello, world! argc=%d\n", argc);
  for (int i = 1; i < argc; i++) printf("argv[%d]=%s\n", i, argv[i]);
  fprintf(stderr, "to stderr\n");
  return 7;
}
