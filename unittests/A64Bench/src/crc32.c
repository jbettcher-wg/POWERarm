/* Table-driven software CRC-32 (reflected, poly 0xEDB88320) over a generated buffer.
 * Stresses: byte loads, table lookups, a short loop-carried dependency chain. */
static const char BENCH_NAME[] = "crc32";
static const unsigned long BENCH_DEFAULT_SCALE = 64; /* passes over the buffer */
#include "bench.h"

#define BUF_LEN (1u << 20)
static u32 table[256];
static u8 buf[BUF_LEN];

static void bench_setup(u64 scale) {
  (void)scale;
  for (u32 i = 0; i < 256; i++) {
    u32 c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    table[i] = c;
  }
  for (u32 i = 0; i < BUF_LEN; i++) buf[i] = (u8)bench_rand();
}

static u32 crc32(u32 crc, const u8 *p, u64 n) {
  crc = ~crc;
  while (n--) crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  return ~crc;
}

static u64 bench_run(u64 scale) {
  u32 crc = 0;
  u64 acc = 0;
  for (u64 i = 0; i < scale; i++) {
    crc = crc32(crc, buf, BUF_LEN);
    acc = (acc << 7 | acc >> 57) ^ crc;
  }
  return acc;
}
