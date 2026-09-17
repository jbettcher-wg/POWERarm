/* a76probe.c -- the A76 side of a few pipeprobe.c shapes, for the comparison table.
** Same discipline: whole loop in one asm block, C reference for parity, best-of-5.
** Build: clang -O2 -o a76probe a76probe.c   (AArch64)
** Usage: a76probe [iters]
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define PROG_BITS 20
#define PROG_LEN (1u << PROG_BITS)
static uint8_t *prog_rand4, *prog_rep8, *prog_rand2;
static uint64_t *ctx, *tab;
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rng(void) { uint64_t x = rng_state; x ^= x << 13; x ^= x >> 7; x ^= x << 17; rng_state = x; return x; }
static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }

#define LOOP_HEAD "  .p2align 5\n1:\n"
#define LOOP_TAIL "  subs %[n],%[n],#1\n  b.ne 1b\n"
#define X8(s) s s s s s s s s

static uint64_t Q_loop(uint64_t n) { uint64_t a = 0;
  __asm__ volatile(LOOP_HEAD "  add %[a],%[a],#1\n" LOOP_TAIL : [a] "+r"(a), [n] "+r"(n) : : "cc"); return a; }
static uint64_t A_chain1(uint64_t n) { uint64_t a = 1, b = 3;
  __asm__ volatile(LOOP_HEAD X8("  add %[a],%[a],%[b]\n") LOOP_TAIL : [a] "+r"(a), [n] "+r"(n) : [b] "r"(b) : "cc"); return a; }
static uint64_t A_indep8(uint64_t n) { uint64_t a0=11,a1=12,a2=13,a3=14,a4=15,a5=16,a6=17,a7=18,b=3;
  __asm__ volatile(LOOP_HEAD
    "  add %[a0],%[a0],%[b]\n  add %[a1],%[a1],%[b]\n  add %[a2],%[a2],%[b]\n  add %[a3],%[a3],%[b]\n"
    "  add %[a4],%[a4],%[b]\n  add %[a5],%[a5],%[b]\n  add %[a6],%[a6],%[b]\n  add %[a7],%[a7],%[b]\n" LOOP_TAIL
    : [a0]"+r"(a0),[a1]"+r"(a1),[a2]"+r"(a2),[a3]"+r"(a3),[a4]"+r"(a4),[a5]"+r"(a5),[a6]"+r"(a6),[a7]"+r"(a7),[n]"+r"(n) : [b]"r"(b) : "cc");
  return a0+a1+a2+a3+a4+a5+a6+a7; }
static uint64_t F_adcs_chain(uint64_t n) { uint64_t a = 1, b = 0x8000000000000003ull;
  __asm__ volatile("  cmp xzr,xzr\n" LOOP_HEAD X8("  adds %[a],%[a],%[b]\n  adc %[a],%[a],xzr\n") LOOP_TAIL : [a] "+r"(a), [n] "+r"(n) : [b] "r"(b) : "cc"); return a; }
static uint64_t F_adcs_chain_ref(uint64_t n) { uint64_t a = 1, b = 0x8000000000000003ull;
  for (uint64_t i = 0; i < 8 * n; i++) { uint64_t s = a + b; uint64_t c = s < a; a = s + c; } return a; }
static uint64_t S_ctx_rt1(uint64_t n) { uint64_t a; ctx[0] = 0;
  __asm__ volatile(LOOP_HEAD "  ldr %[a],[%[p]]\n  add %[a],%[a],#1\n  str %[a],[%[p]]\n" LOOP_TAIL : [a] "=&r"(a), [n] "+r"(n) : [p] "r"(ctx) : "cc", "memory"); return ctx[0]; }
static uint64_t S_fwd_2stw_ld(uint64_t n) { uint64_t a, t; ctx[0] = 0;
  __asm__ volatile(LOOP_HEAD "  ldr %[a],[%[p]]\n  add %[a],%[a],#1\n  str %w[a],[%[p]]\n  lsr %[t],%[a],#32\n  str %w[t],[%[p],#4]\n" LOOP_TAIL
    : [a] "=&r"(a), [t] "=&r"(t), [n] "+r"(n) : [p] "r"(ctx) : "cc", "memory"); return ctx[0]; }
static uint64_t S_ctx_crcshape(uint64_t n) { uint64_t a, b, c, t; ctx[0] = ctx[1] = ctx[2] = 0;
  __asm__ volatile(LOOP_HEAD
    "  ldr %[a],[%[p]]\n  ldr %[b],[%[p],#8]\n  ldr %[c],[%[p],#16]\n"
    "  eor %[t],%[a],%[b]\n  and %[t],%[t],#15\n  ldr %[t],[%[q],%[t],lsl #3]\n"
    "  eor %[a],%[t],%[a],lsr #8\n  add %[b],%[b],#1\n  add %[c],%[c],#1\n"
    "  str %[a],[%[p]]\n  str %[b],[%[p],#8]\n  str %[c],[%[p],#16]\n" LOOP_TAIL
    : [a] "=&r"(a), [b] "=&r"(b), [c] "=&r"(c), [t] "=&r"(t), [n] "+r"(n) : [p] "r"(ctx), [q] "r"(tab) : "cc", "memory");
  return ctx[0] + ctx[1] + ctx[2]; }
static uint64_t S_ctx_crcshape_ref(uint64_t n) { uint64_t a = 0, b = 0, c = 0;
  for (uint64_t i = 0; i < n; i++) { uint64_t t = tab[(a ^ b) & 15]; a = (a >> 8) ^ t; b++; c++; } return a + b + c; }
static uint64_t S_reg_crcshape(uint64_t n) { uint64_t a = 0, b = 0, c = 0, t;
  __asm__ volatile(LOOP_HEAD
    "  eor %[t],%[a],%[b]\n  and %[t],%[t],#15\n  ldr %[t],[%[q],%[t],lsl #3]\n"
    "  eor %[a],%[t],%[a],lsr #8\n  add %[b],%[b],#1\n  add %[c],%[c],#1\n" LOOP_TAIL
    : [a] "+r"(a), [b] "+r"(b), [c] "+r"(c), [t] "=&r"(t), [n] "+r"(n) : [q] "r"(tab) : "cc", "memory");
  return a + b + c; }
/* 16-handler dispatch through br, table filled from an adr */
#define HANDLERS16 \
  "  .p2align 4\n4:\n  add %[a],%[a],#1\n  b 3f\n" "  .p2align 4\n  add %[a],%[a],#2\n  b 3f\n" \
  "  .p2align 4\n  add %[a],%[a],#3\n  b 3f\n"     "  .p2align 4\n  add %[a],%[a],#4\n  b 3f\n" \
  "  .p2align 4\n  add %[a],%[a],#5\n  b 3f\n"     "  .p2align 4\n  add %[a],%[a],#6\n  b 3f\n" \
  "  .p2align 4\n  add %[a],%[a],#7\n  b 3f\n"     "  .p2align 4\n  add %[a],%[a],#8\n  b 3f\n" \
  "  .p2align 4\n  add %[a],%[a],#9\n  b 3f\n"     "  .p2align 4\n  add %[a],%[a],#10\n  b 3f\n" \
  "  .p2align 4\n  add %[a],%[a],#11\n  b 3f\n"    "  .p2align 4\n  add %[a],%[a],#12\n  b 3f\n" \
  "  .p2align 4\n  add %[a],%[a],#13\n  b 3f\n"    "  .p2align 4\n  add %[a],%[a],#14\n  b 3f\n" \
  "  .p2align 4\n  add %[a],%[a],#15\n  b 3f\n"    "  .p2align 4\n  add %[a],%[a],#16\n  b 3f\n"
static uint64_t D_br(uint64_t n, const uint8_t *prog) { uint64_t a = 0, t, u, op, i = 0;
  __asm__ volatile("  adr %[t],4f\n  mov %[u],#0\n7:\n  str %[t],[%[tab],%[u]]\n  add %[u],%[u],#8\n  add %[t],%[t],#16\n  cmp %[u],#128\n  b.ne 7b\n"
    LOOP_HEAD "  ldrb %w[op],[%[prog],%[i]]\n  add %[i],%[i],#1\n  and %[i],%[i],#0xfffff\n  ldr %[t],[%[tab],%[op],lsl #3]\n  br %[t]\n"
    HANDLERS16 "3:\n" LOOP_TAIL
    : [a]"+r"(a),[t]"=&r"(t),[u]"=&r"(u),[op]"=&r"(op),[i]"+r"(i),[n]"+r"(n) : [prog]"r"(prog),[tab]"r"(tab) : "cc","memory");
  return a; }
static uint64_t D_cmp(uint64_t n, const uint8_t *prog) { uint64_t a = 0, t, u, op, i = 0;
  __asm__ volatile("  adr %[t],4f\n  mov %[u],#0\n7:\n  str %[t],[%[tab],%[u]]\n  add %[u],%[u],#8\n  add %[t],%[t],#16\n  cmp %[u],#128\n  b.ne 7b\n"
    LOOP_HEAD "  ldrb %w[op],[%[prog],%[i]]\n  add %[i],%[i],#1\n  and %[i],%[i],#0xfffff\n"
    "  cmp %[op],#0\n  b.eq 4f\n  cmp %[op],#1\n  b.eq 10f\n  cmp %[op],#2\n  b.eq 11f\n  cmp %[op],#3\n  b.eq 12f\n"
    "  ldr %[t],[%[tab],%[op],lsl #3]\n  br %[t]\n"
    "  .p2align 4\n4:\n  add %[a],%[a],#1\n  b 3f\n" "  .p2align 4\n10:\n  add %[a],%[a],#2\n  b 3f\n"
    "  .p2align 4\n11:\n  add %[a],%[a],#3\n  b 3f\n" "  .p2align 4\n12:\n  add %[a],%[a],#4\n  b 3f\n"
    "  .p2align 4\n  add %[a],%[a],#5\n  b 3f\n" "  .p2align 4\n  add %[a],%[a],#6\n  b 3f\n"
    "  .p2align 4\n  add %[a],%[a],#7\n  b 3f\n" "  .p2align 4\n  add %[a],%[a],#8\n  b 3f\n"
    "  .p2align 4\n  add %[a],%[a],#9\n  b 3f\n" "  .p2align 4\n  add %[a],%[a],#10\n  b 3f\n"
    "  .p2align 4\n  add %[a],%[a],#11\n  b 3f\n" "  .p2align 4\n  add %[a],%[a],#12\n  b 3f\n"
    "  .p2align 4\n  add %[a],%[a],#13\n  b 3f\n" "  .p2align 4\n  add %[a],%[a],#14\n  b 3f\n"
    "  .p2align 4\n  add %[a],%[a],#15\n  b 3f\n" "  .p2align 4\n  add %[a],%[a],#16\n  b 3f\n"
    "3:\n" LOOP_TAIL
    : [a]"+r"(a),[t]"=&r"(t),[u]"=&r"(u),[op]"=&r"(op),[i]"+r"(i),[n]"+r"(n) : [prog]"r"(prog),[tab]"r"(tab) : "cc","memory");
  return a; }
static uint64_t D_ref(uint64_t n, const uint8_t *prog) { uint64_t a = 0; for (uint64_t i = 0; i < n; i++) a += prog[i & (PROG_LEN - 1)] + 1; return a; }
static uint64_t D_br_rand4(uint64_t n) { return D_br(n, prog_rand4); }
static uint64_t D_br_rand4_ref(uint64_t n) { return D_ref(n, prog_rand4); }
static uint64_t D_br_rep8(uint64_t n) { return D_br(n, prog_rep8); }
static uint64_t D_br_rep8_ref(uint64_t n) { return D_ref(n, prog_rep8); }
static uint64_t D_cmp_rep8(uint64_t n) { return D_cmp(n, prog_rep8); }
static uint64_t D_cmp_rand4(uint64_t n) { return D_cmp(n, prog_rand4); }
/* two random call sites, bl / ret */
static uint64_t B_r2_bl_ret(uint64_t n) { uint64_t a = 0, t, i = 0;
  __asm__ volatile(LOOP_HEAD "  ldrb %w[t],[%[arr],%[i]]\n  add %[i],%[i],#1\n  and %[i],%[i],#0xfffff\n  cbz %[t],2f\n  bl 5f\n  b 3f\n  .p2align 4\n2:\n  bl 5f\n3:\n" LOOP_TAIL
    "  b 9f\n  .p2align 5\n5:\n  add %[a],%[a],#1\n  ret\n9:\n"
    : [a]"+r"(a),[t]"=&r"(t),[i]"+r"(i),[n]"+r"(n) : [arr]"r"(prog_rand2) : "cc","memory","x30"); return a; }
static uint64_t B_bl_ret(uint64_t n) { uint64_t a = 0;
  __asm__ volatile(LOOP_HEAD "  bl 5f\n" LOOP_TAIL "  b 9f\n  .p2align 5\n5:\n  add %[a],%[a],#1\n  ret\n9:\n"
    : [a]"+r"(a),[n]"+r"(n) : : "cc","x30"); return a; }
static uint64_t B_br_next(uint64_t n) { uint64_t a = 0, t;
  __asm__ volatile("  adr %[t],2f\n" LOOP_HEAD "  br %[t]\n  .p2align 5\n2:\n  add %[a],%[a],#1\n" LOOP_TAIL
    : [a]"+r"(a),[t]"=&r"(t),[n]"+r"(n) : : "cc"); return a; }
static uint64_t ref_n(uint64_t n) { return n; }
static uint64_t ref_chain1(uint64_t n) { return 1 + 8 * n * 3; }
static uint64_t ref_indep8(uint64_t n) { return 116 + 8 * n * 3; }

typedef uint64_t (*fn)(uint64_t);
static struct { const char *name; fn f, ref; } probes[] = {
  {"Q_loop", Q_loop, ref_n}, {"A_chain1", A_chain1, ref_chain1}, {"A_indep8", A_indep8, ref_indep8},
  {"F_adcs_chain", F_adcs_chain, F_adcs_chain_ref},
  {"S_ctx_rt1", S_ctx_rt1, ref_n}, {"S_fwd_2stw_ld", S_fwd_2stw_ld, ref_n},
  {"S_ctx_crcshape", S_ctx_crcshape, S_ctx_crcshape_ref}, {"S_reg_crcshape", S_reg_crcshape, S_ctx_crcshape_ref},
  {"D_br_rand4", D_br_rand4, D_br_rand4_ref}, {"D_br_rep8", D_br_rep8, D_br_rep8_ref},
  {"D_cmp_rand4", D_cmp_rand4, D_br_rand4_ref}, {"D_cmp_rep8", D_cmp_rep8, D_br_rep8_ref},
  {"B_bl_ret", B_bl_ret, ref_n}, {"B_r2_bl_ret", B_r2_bl_ret, ref_n}, {"B_br_next", B_br_next, ref_n},
};
int main(int argc, char **argv) {
  uint64_t iters = argc > 1 ? strtoull(argv[1], NULL, 0) : (1ull << 22);
  prog_rand4 = aligned_alloc(64, PROG_LEN); prog_rep8 = aligned_alloc(64, PROG_LEN); prog_rand2 = aligned_alloc(64, PROG_LEN);
  ctx = aligned_alloc(64, 4096); tab = aligned_alloc(64, 512); memset(ctx, 0, 4096);
  uint8_t p8[8]; for (int i = 0; i < 8; i++) p8[i] = rng() & 3;
  for (uint32_t i = 0; i < PROG_LEN; i++) { uint64_t r = rng(); prog_rand2[i] = r & 1; prog_rand4[i] = (r >> 8) & 3; prog_rep8[i] = p8[i & 7]; }
  for (int i = 0; i < 64; i++) tab[i] = 0x1234567890ABCDEFull + i;   /* crcshape table; overwritten by dispatch probes */
  for (size_t k = 0; k < sizeof(probes) / sizeof(probes[0]); k++) {
    for (int i = 0; i < 64; i++) tab[i] = 0x1234567890ABCDEFull + i;
    uint64_t want = probes[k].ref(4097), got = probes[k].f(4097);
    if (got != want) { printf("%-16s parity=FAIL\n", probes[k].name); continue; }
    for (int i = 0; i < 64; i++) tab[i] = 0x1234567890ABCDEFull + i;
    want = probes[k].ref(iters);
    double best = 1e30;
    for (int r = 0; r < 5; r++) {
      for (int i = 0; i < 64; i++) tab[i] = 0x1234567890ABCDEFull + i;
      double t0 = now_ns(); uint64_t v = probes[k].f(iters); double t = (now_ns() - t0) / iters;
      if (v != want) { printf("%-16s parity=FAIL (rep)\n", probes[k].name); best = -1; break; }
      if (t < best) best = t;
    }
    if (best > 0) printf("%-16s parity=ok ns/iter=%.3f cycles@2.4GHz=%.1f\n", probes[k].name, best, best * 2.4);
  }
  return 0;
}
