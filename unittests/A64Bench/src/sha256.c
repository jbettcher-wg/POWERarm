/* Software SHA-256 (FIPS 180-4) over a generated buffer, digests chained between passes.
 * Stresses: 32-bit rotates and adds in long dependency chains, little memory. */
static const char BENCH_NAME[] = "sha256";
static const unsigned long BENCH_DEFAULT_SCALE = 48; /* passes over the buffer */
#include "bench.h"

#define BUF_LEN (1u << 20)
static u8 buf[BUF_LEN];

static const u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xdc6e27f8, 0xe4ae2d7e,
    0x06f067aa, 0x0a637dc9, 0x113a1f63, 0x1b41f6e1, 0x2a70f0bb, 0x3cf1ef10, 0x4a5d5a2d, 0x5b9cca4f,
    0x648b3e3e, 0x734b0f02, 0x7e7ab6ff, 0x88342ecd, 0x937f4a19, 0x9a1bbef1, 0xa4c47fc0, 0xae43f8a8};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void compress(u32 *h, const u8 *p) {
  u32 w[64];
  for (int i = 0; i < 16; i++)
    w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    u32 s = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
    u32 ch = (e & f) ^ (~e & g);
    u32 t1 = hh + s + ch + K[i] + w[i];
    u32 s2 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
    u32 maj = (a & b) ^ (a & c) ^ (b & c);
    u32 t2 = s2 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
  h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256(const u8 *p, u64 n, u32 *h) {
  static const u32 IV[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  for (int i = 0; i < 8; i++) h[i] = IV[i];
  u64 i = 0;
  for (; i + 64 <= n; i += 64) compress(h, p + i);
  u8 tail[128];
  u64 r = n - i, j;
  for (j = 0; j < r; j++) tail[j] = p[i + j];
  tail[j++] = 0x80;
  u64 padlen = (r + 1 + 8 <= 64) ? 64 : 128;
  for (; j < padlen - 8; j++) tail[j] = 0;
  u64 bits = n * 8;
  for (int k = 7; k >= 0; k--) tail[j++] = (u8)(bits >> (8 * k));
  compress(h, tail);
  if (padlen == 128) compress(h, tail + 64);
}

static void bench_setup(u64 scale) {
  (void)scale;
  for (u32 i = 0; i < BUF_LEN; i++) buf[i] = (u8)bench_rand();
}

static u64 bench_run(u64 scale) {
  u32 h[8];
  u8 save[32];
  for (int i = 0; i < 32; i++) save[i] = buf[i];
  for (u64 pass = 0; pass < scale; pass++) {
    sha256(buf, BUF_LEN - 3, h); /* odd length exercises padding */
    for (int i = 0; i < 32; i++) buf[i] = (u8)(h[i / 4] >> (24 - 8 * (i % 4)));
  }
  for (int i = 0; i < 32; i++) buf[i] = save[i]; /* each rep hashes identical input */
  return (u64)h[0] << 32 | h[1];
}
