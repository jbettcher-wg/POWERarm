/* A64Bench runtime: freestanding, integer-only, no libc.
 *
 * Each workload defines:
 *   static const char BENCH_NAME[];
 *   static const u64 BENCH_DEFAULT_SCALE;
 *   static void bench_setup(u64 scale);     untimed, runs once
 *   static u64  bench_run(u64 scale);       timed, returns a checksum
 * and then includes this header's BENCH_MAIN via BENCH_DEFINE_MAIN().
 *
 * Usage: <binary> [scale] [reps]
 * Output, one line per repetition, then a summary line:
 *   bench=<name> scale=<s> rep=<i> ns=<in-program ns> sum=<hex>
 *   bench=<name> scale=<s> reps=<n> total_ns=<ns> sum=<hex> parity=<ok|MISMATCH>
 * rep=0 includes JIT translation of the hot code under an emulator ("cold");
 * rep>=1 is steady state ("warm"). Syscalls used: write, exit_group, clock_gettime.
 */
#ifndef A64BENCH_H
#define A64BENCH_H

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned short u16;
typedef unsigned char u8;

#if defined(__aarch64__)
#define SYS_write 64
#define SYS_exit_group 94
#define SYS_clock_gettime 113
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "  mov x29, #0\n"
        "  mov x30, #0\n"
        "  mov x0, sp\n"
        "  bl bench_cstart\n"
        "  b .\n"
        ".globl bench_syscall\n"
        "bench_syscall:\n"
        "  mov x8, x0\n"
        "  mov x0, x1\n"
        "  mov x1, x2\n"
        "  mov x2, x3\n"
        "  svc #0\n"
        "  ret\n");
#elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
#define SYS_write 4
#define SYS_exit_group 234
#define SYS_clock_gettime 246
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "  bl 0f\n"
        "0: mflr 12\n"
        "  addis 2, 12, (.TOC.-0b)@ha\n"
        "  addi 2, 2, (.TOC.-0b)@l\n"
        "  mr 3, 1\n"
        "  clrrdi 1, 1, 4\n"
        "  li 0, 0\n"
        "  stdu 0, -128(1)\n"
        "  bl bench_cstart\n"
        "  nop\n"
        "  b .\n"
        ".globl bench_syscall\n"
        "bench_syscall:\n"
        "  mr 0, 3\n"
        "  mr 3, 4\n"
        "  mr 4, 5\n"
        "  mr 5, 6\n"
        "  sc\n"
        "  bnslr\n"
        "  neg 3, 3\n"
        "  blr\n");
#else
#error "A64Bench supports aarch64 and ppc64le only"
#endif

long bench_syscall(long nr, long a, long b, long c);

/* The compiler may emit calls to these for struct copies; byte loops only. */
void *memcpy(void *d, const void *s, u64 n) {
  u8 *dp = d;
  const u8 *sp = s;
  while (n--) *dp++ = *sp++;
  return d;
}
void *memset(void *d, int c, u64 n) {
  u8 *dp = d;
  while (n--) *dp++ = (u8)c;
  return d;
}
void *memmove(void *d, const void *s, u64 n) {
  u8 *dp = d;
  const u8 *sp = s;
  if (dp < sp) {
    while (n--) *dp++ = *sp++;
  } else {
    while (n--) dp[n] = sp[n];
  }
  return d;
}
int memcmp(const void *a, const void *b, u64 n) {
  const u8 *x = a, *y = b;
  for (; n; n--, x++, y++)
    if (*x != *y) return *x - *y;
  return 0;
}

static void bench_write(const char *s, u64 n) {
  while (n) {
    long r = bench_syscall(SYS_write, 1, (long)s, (long)n);
    if (r <= 0) return;
    s += r;
    n -= (u64)r;
  }
}

static u64 bench_strlen(const char *s) {
  u64 n = 0;
  while (s[n]) n++;
  return n;
}

struct bench_buf {
  char b[256];
  u64 n;
};
static void bb_s(struct bench_buf *o, const char *s) {
  while (*s && o->n < sizeof(o->b)) o->b[o->n++] = *s++;
}
static void bb_u(struct bench_buf *o, u64 v) {
  char t[24];
  int i = 0;
  do {
    t[i++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (i && o->n < sizeof(o->b)) o->b[o->n++] = t[--i];
}
static void bb_x(struct bench_buf *o, u64 v) {
  for (int i = 60; i >= 0 && o->n < sizeof(o->b); i -= 4)
    o->b[o->n++] = "0123456789abcdef"[(v >> i) & 15];
}

static u64 bench_parse(const char *s, u64 dflt) {
  if (!s || !*s) return dflt;
  u64 v = 0;
  for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (u64)(*s - '0');
  return v ? v : dflt;
}

struct bench_ts {
  i64 sec, nsec;
};
static u64 bench_now_ns(void) {
  struct bench_ts ts;
  bench_syscall(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, (long)&ts, 0);
  return (u64)ts.sec * 1000000000ul + (u64)ts.nsec;
}

/* xorshift64*: deterministic PRNG shared by the workloads. */
static u64 bench_rng_state = 0x9E3779B97F4A7C15ul;
static inline u64 bench_rand(void) {
  u64 x = bench_rng_state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  bench_rng_state = x;
  return x * 0x2545F4914F6CDD1Dul;
}

static void bench_setup(u64 scale);
static u64 bench_run(u64 scale);

__attribute__((used, noinline)) void bench_cstart(long *sp) {
  long argc = sp[0];
  char **argv = (char **)(sp + 1);
  u64 scale = bench_parse(argc > 1 ? argv[1] : 0, BENCH_DEFAULT_SCALE);
  u64 reps = bench_parse(argc > 2 ? argv[2] : 0, 3);
  bench_setup(scale);
  u64 first = 0, total = 0;
  int ok = 1;
  for (u64 r = 0; r < reps; r++) {
    u64 t0 = bench_now_ns();
    u64 sum = bench_run(scale);
    u64 dt = bench_now_ns() - t0;
    total += dt;
    if (r == 0) first = sum;
    else if (sum != first) ok = 0;
    struct bench_buf o = {.n = 0};
    bb_s(&o, "bench=");
    bb_s(&o, BENCH_NAME);
    bb_s(&o, " scale=");
    bb_u(&o, scale);
    bb_s(&o, " rep=");
    bb_u(&o, r);
    bb_s(&o, " ns=");
    bb_u(&o, dt);
    bb_s(&o, " sum=");
    bb_x(&o, sum);
    bb_s(&o, "\n");
    bench_write(o.b, o.n);
  }
  struct bench_buf o = {.n = 0};
  bb_s(&o, "bench=");
  bb_s(&o, BENCH_NAME);
  bb_s(&o, " scale=");
  bb_u(&o, scale);
  bb_s(&o, " reps=");
  bb_u(&o, reps);
  bb_s(&o, " total_ns=");
  bb_u(&o, total);
  bb_s(&o, " sum=");
  bb_x(&o, first);
  bb_s(&o, ok ? " parity=ok\n" : " parity=MISMATCH\n");
  bench_write(o.b, o.n);
  (void)bench_strlen;
  bench_syscall(SYS_exit_group, ok ? 0 : 1, 0, 0);
  for (;;) {
  }
}

#endif
