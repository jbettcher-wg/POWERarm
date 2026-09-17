// SPDX-License-Identifier: MIT
// Exercises the string and memory routines over lengths, alignments and
// match positions, and prints a hash of every result.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t h = 1469598103934665603ULL;
static void mix(uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (i * 8)) & 0xFF;
    h *= 1099511628211ULL;
  }
}

static char src[512], dst[512];

int main(void) {
  for (int round = 0; round < 3; ++round) {
    for (size_t off = 0; off < 16; ++off) {
      for (size_t len = 0; len < 130; ++len) {
        for (size_t i = 0; i < sizeof src; ++i) {
          src[i] = (char)('a' + (i * 7 + round) % 26);
        }
        src[off + len] = 0;
        mix(strlen(src + off));
        mix(strnlen(src + off, len / 2));
        char c = (char)('a' + (len * 3 + round) % 26);
        char* p = strchr(src + off, c);
        mix(p ? (uint64_t)(p - src) : ~0ULL);
        p = strrchr(src + off, c);
        mix(p ? (uint64_t)(p - src) : ~0ULL);
        p = memchr(src + off, c, len);
        mix(p ? (uint64_t)(p - src) : ~0ULL);
        p = memrchr(src + off, c, len);
        mix(p ? (uint64_t)(p - src) : ~0ULL);
        memset(dst, 0x55, sizeof dst);
        memcpy(dst + (off ^ 5), src + off, len);
        mix(dst[(off ^ 5) + len] | (dst[off ^ 5] << 8));
        memmove(dst + 3, dst + (off ^ 5), len);
        memmove(dst + (off ^ 5) + 1, dst + 3, len);
        memset(dst + off, round, len);
        strcpy(dst + 200, src + off);
        mix(memcmp(dst, src, 256) > 0);
        mix(strcmp(dst + 200, src + off));
        if (len) dst[200 + len - 1] ^= 1;
        mix(strcmp(dst + 200, src + off) > 0 ? 1 : strcmp(dst + 200, src + off) < 0 ? 2 : 0);
        mix(strncmp(dst + 200, src + off, len / 2 + 1) != 0);
        mix(memcmp(dst + 200, src + off, len) < 0);
        char* e = stpcpy(dst + 300, src + off);
        mix((uint64_t)(e - dst));
        for (size_t i = 0; i < sizeof dst; ++i) mix((uint8_t)dst[i]);
      }
    }
    printf("round %d hash %016llx\n", round, (unsigned long long)h);
  }
  return 0;
}
