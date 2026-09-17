/* pipeprobe.c -- POWER9 pipeline probes for the POWERarm code-shaping study.
**
** Every probe is a whole loop written in one inline-asm block (so the layout
** of branches and the 32-byte alignment are exactly what is measured), paired
** with a plain-C reference that computes the same checksum. A probe that does
** not reproduce its reference prints FAIL and is never timed.
**
** Usage:
**   pipeprobe list                 list probe names
**   pipeprobe time  <name> [iters] parity check, then best-of-5 ns/iteration
**   pipeprobe once  <name> [iters] parity check, then exactly one run (for perf stat)
**
** Build: gcc -O2 -mcpu=power9 -fno-pie -no-pie -o pipeprobe pipeprobe.c
** (POWER9-only instructions -- mcrxrx, addpcis -- are used only by the probes
**  whose names say so; everything else is POWER8-safe.)
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define PROG_BITS 20
#define PROG_LEN (1u << PROG_BITS)

/* Shared scratch state. All buffers are 64-byte aligned. */
static uint8_t  *prog_rand2;   /* 0/1 uniform                              */
static uint8_t  *prog_rand4;   /* 0..3 uniform                             */
static uint8_t  *prog_rand16;  /* 0..15 uniform                            */
static uint8_t  *prog_rep8;    /* period-8 pattern over 0..3               */
static uint8_t  *prog_rep64;   /* period-64 pattern over 0..15             */
static uint8_t  *prog_skew;    /* 90% opcode 0, rest 1..3                  */
static uint64_t *ctx;          /* 4 KiB context-like buffer                */
static uint64_t *tab;          /* 16 handler addresses                     */
static uint64_t *stackbuf;     /* manual stack for the recursion probe     */
static uint64_t *chase;        /* 16-entry cyclic pointer chain            */
static uint64_t *tocbuf;       /* constant pool                            */

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rng(void) {
  uint64_t x = rng_state;
  x ^= x << 13; x ^= x >> 7; x ^= x << 17;
  rng_state = x;
  return x;
}

static void *alloc64(size_t bytes) {
  void *p = NULL;
  if (posix_memalign(&p, 64, bytes) != 0) { perror("posix_memalign"); exit(2); }
  memset(p, 0, bytes);
  return p;
}

static void setup(void) {
  prog_rand2  = alloc64(PROG_LEN);
  prog_rand4  = alloc64(PROG_LEN);
  prog_rand16 = alloc64(PROG_LEN);
  prog_rep8   = alloc64(PROG_LEN);
  prog_rep64  = alloc64(PROG_LEN);
  prog_skew   = alloc64(PROG_LEN);
  ctx      = alloc64(4096);
  tab      = alloc64(16 * 8);
  stackbuf = alloc64(16 * 1024);
  chase    = alloc64(64 * 8);
  tocbuf   = alloc64(64 * 8);
  uint8_t p8[8], p64[64];
  for (int i = 0; i < 8; i++)  p8[i]  = rng() & 3;
  for (int i = 0; i < 64; i++) p64[i] = rng() & 15;
  for (uint32_t i = 0; i < PROG_LEN; i++) {
    uint64_t r = rng();
    prog_rand2[i]  = r & 1;
    prog_rand4[i]  = (r >> 8) & 3;
    prog_rand16[i] = (r >> 16) & 15;
    prog_rep8[i]   = p8[i & 7];
    prog_rep64[i]  = p64[i & 63];
    prog_skew[i]   = ((r >> 24) % 10 == 0) ? 1 + ((r >> 32) % 3) : 0;
  }
  /* 16-entry cyclic chain, stride 8 bytes, in one cache line pair */
  for (int i = 0; i < 16; i++) chase[i] = (uint64_t)&chase[(i + 1) & 15];
  for (int i = 0; i < 64; i++) tocbuf[i] = 0x1234567890ABCDEFull + i;
}

static double now_ns(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}

/* ------------------------------------------------------------------------- */
/* Loop scaffolding shared by the probes: 32-byte aligned head, counter in n. */
#define LOOP_HEAD "  .p2align 5\n1:\n"
#define LOOP_TAIL "  addi %[n],%[n],-1\n  cmpdi %[n],0\n  bne 1b\n"
#define X8(s) s s s s s s s s
#define X4(s) s s s s

typedef uint64_t (*probe_fn)(uint64_t n);

/* ===================== A: width and latency ============================== */

static uint64_t A_chain1(uint64_t n) {      /* 8 dependent adds per iteration */
  uint64_t a = 1, b = 3;
  __asm__ volatile(LOOP_HEAD X8("  add %[a],%[a],%[b]\n") LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : [b] "b"(b) : "cr0");
  return a;
}
static uint64_t A_chain1_ref(uint64_t n) { return 1 + 8 * n * 3; }

static uint64_t A_chain2x4(uint64_t n) {    /* 2 chains x 4 dependent adds */
  uint64_t a = 1, c = 5, b = 3;
  __asm__ volatile(LOOP_HEAD
                   X4("  add %[a],%[a],%[b]\n  add %[c],%[c],%[b]\n") LOOP_TAIL
                   : [a] "+b"(a), [c] "+b"(c), [n] "+b"(n) : [b] "b"(b) : "cr0");
  return a + c;
}
static uint64_t A_chain2x4_ref(uint64_t n) { return (1 + 4 * n * 3) + (5 + 4 * n * 3); }

static uint64_t A_indep8(uint64_t n) {      /* 8 independent adds per iteration */
  uint64_t a0 = 11, a1 = 12, a2 = 13, a3 = 14, a4 = 15, a5 = 16, a6 = 17, a7 = 18, b = 3;
  __asm__ volatile(LOOP_HEAD
                   "  add %[a0],%[a0],%[b]\n  add %[a1],%[a1],%[b]\n"
                   "  add %[a2],%[a2],%[b]\n  add %[a3],%[a3],%[b]\n"
                   "  add %[a4],%[a4],%[b]\n  add %[a5],%[a5],%[b]\n"
                   "  add %[a6],%[a6],%[b]\n  add %[a7],%[a7],%[b]\n"
                   LOOP_TAIL
                   : [a0] "+b"(a0), [a1] "+b"(a1), [a2] "+b"(a2), [a3] "+b"(a3),
                     [a4] "+b"(a4), [a5] "+b"(a5), [a6] "+b"(a6), [a7] "+b"(a7), [n] "+b"(n)
                   : [b] "b"(b) : "cr0");
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}
static uint64_t A_indep8_ref(uint64_t n) { return 116 + 8 * n * 3; }

static uint64_t A_indep16(uint64_t n) {     /* 16 independent adds per iteration */
  uint64_t a0 = 11, a1 = 12, a2 = 13, a3 = 14, a4 = 15, a5 = 16, a6 = 17, a7 = 18, b = 3;
  __asm__ volatile(LOOP_HEAD
                   "  add %[a0],%[a0],%[b]\n  add %[a1],%[a1],%[b]\n"
                   "  add %[a2],%[a2],%[b]\n  add %[a3],%[a3],%[b]\n"
                   "  add %[a4],%[a4],%[b]\n  add %[a5],%[a5],%[b]\n"
                   "  add %[a6],%[a6],%[b]\n  add %[a7],%[a7],%[b]\n"
                   "  add %[a0],%[a0],%[b]\n  add %[a1],%[a1],%[b]\n"
                   "  add %[a2],%[a2],%[b]\n  add %[a3],%[a3],%[b]\n"
                   "  add %[a4],%[a4],%[b]\n  add %[a5],%[a5],%[b]\n"
                   "  add %[a6],%[a6],%[b]\n  add %[a7],%[a7],%[b]\n"
                   LOOP_TAIL
                   : [a0] "+b"(a0), [a1] "+b"(a1), [a2] "+b"(a2), [a3] "+b"(a3),
                     [a4] "+b"(a4), [a5] "+b"(a5), [a6] "+b"(a6), [a7] "+b"(a7), [n] "+b"(n)
                   : [b] "b"(b) : "cr0");
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}
static uint64_t A_indep16_ref(uint64_t n) { return 116 + 16 * n * 3; }

static uint64_t A_mulchain(uint64_t n) {    /* 8 dependent mulld */
  uint64_t a = 1, b = 3;
  __asm__ volatile(LOOP_HEAD X8("  mulld %[a],%[a],%[b]\n") LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : [b] "b"(b) : "cr0");
  return a;
}
static uint64_t A_mulchain_ref(uint64_t n) { uint64_t a = 1; for (uint64_t i = 0; i < 8 * n; i++) a *= 3; return a; }

static uint64_t A_ldchain(uint64_t n) {     /* 8 dependent L1-hit loads (pointer chase) */
  uint64_t p = (uint64_t)chase;
  __asm__ volatile(LOOP_HEAD X8("  ld %[p],0(%[p])\n") LOOP_TAIL
                   : [p] "+b"(p), [n] "+b"(n) : : "cr0", "memory");
  return p - (uint64_t)chase;
}
static uint64_t A_ldchain_ref(uint64_t n) { return ((8 * n) & 15) * 8; }

/* ===================== F: flags ========================================== */

static uint64_t F_add_addi(uint64_t n) {    /* control: 16 dependent ops, no XER */
  uint64_t a = 1, b = 0x8000000000000003ull;
  __asm__ volatile(LOOP_HEAD X8("  add %[a],%[a],%[b]\n  addi %[a],%[a],0\n") LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : [b] "b"(b) : "cr0");
  return a;
}
static uint64_t F_add_addi_ref(uint64_t n) { uint64_t a = 1; for (uint64_t i = 0; i < 8 * n; i++) a += 0x8000000000000003ull; return a; }

static uint64_t F_addc_addze(uint64_t n) {  /* 16 dependent ops, carry through XER.CA */
  uint64_t a = 1, b = 0x8000000000000003ull;
  __asm__ volatile(LOOP_HEAD X8("  addc %[a],%[a],%[b]\n  addze %[a],%[a]\n") LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : [b] "b"(b) : "cr0", "xer");
  return a;
}
static uint64_t F_addc_addze_ref(uint64_t n) {
  uint64_t a = 1, b = 0x8000000000000003ull;
  for (uint64_t i = 0; i < 8 * n; i++) { uint64_t s = a + b; uint64_t c = s < a; a = s + c; }
  return a;
}

/* carry materialised as a CR bit by an unsigned compare, consumed by isel */
static uint64_t F_cmpld_isel(uint64_t n) {
  uint64_t a = 1, b = 0x8000000000000003ull, t;
  __asm__ volatile(LOOP_HEAD
                   X8("  add %[t],%[a],%[b]\n  cmpld 7,%[t],%[a]\n  isel %[a],%[b],%[t],28\n")
                   LOOP_TAIL
                   : [a] "+b"(a), [t] "=&b"(t), [n] "+b"(n) : [b] "b"(b) : "cr0", "cr7");
  return a;
}
static uint64_t F_cmpld_isel_ref(uint64_t n) {
  uint64_t a = 1, b = 0x8000000000000003ull;
  for (uint64_t i = 0; i < 8 * n; i++) { uint64_t s = a + b; a = (s < a) ? b : s; }
  return a;
}

/* same chain, plus a CR field round trip through a GPR (mfocrf/mtocrf) */
static uint64_t F_mfocrf_rt(uint64_t n) {
  uint64_t a = 1, b = 0x8000000000000003ull, t, u;
  __asm__ volatile(LOOP_HEAD
                   X8("  add %[t],%[a],%[b]\n  cmpld 7,%[t],%[a]\n  mfocrf %[u],1\n  mtocrf 1,%[u]\n  isel %[a],%[b],%[t],28\n")
                   LOOP_TAIL
                   : [a] "+b"(a), [t] "=&b"(t), [u] "=&b"(u), [n] "+b"(n) : [b] "b"(b) : "cr0", "cr7");
  return a;
}
/* whole-CR round trip (mfcr/mtcrf 0xff) */
static uint64_t F_mfcr_rt(uint64_t n) {
  uint64_t a = 1, b = 0x8000000000000003ull, t, u;
  __asm__ volatile(LOOP_HEAD
                   X8("  add %[t],%[a],%[b]\n  cmpld 7,%[t],%[a]\n  mfcr %[u]\n  mtcrf 0xff,%[u]\n  isel %[a],%[b],%[t],28\n")
                   LOOP_TAIL
                   : [a] "+b"(a), [t] "=&b"(t), [u] "=&b"(u), [n] "+b"(n) : [b] "b"(b)
                   : "cr0", "cr1", "cr2", "cr3", "cr4", "cr5", "cr6", "cr7");
  return a;
}
/* POWER9: carry into CR via mcrxrx, consumed by isel (bit 30 = cr7.CA) */
static uint64_t F_mcrxrx_p9(uint64_t n) {
  uint64_t a = 1, b = 0x8000000000000003ull, t;
  __asm__ volatile(LOOP_HEAD
                   X8("  addc %[t],%[a],%[b]\n  mcrxrx 7\n  isel %[a],%[b],%[t],30\n")
                   LOOP_TAIL
                   : [a] "+b"(a), [t] "=&b"(t), [n] "+b"(n) : [b] "b"(b) : "cr0", "cr7", "xer");
  return a;
}
/* carry read back through mfxer (XER -> GPR), added to the data */
static uint64_t F_mfxer_chain(uint64_t n) {
  uint64_t a = 1, b = 0x8000000000000003ull, u;
  __asm__ volatile(LOOP_HEAD
                   X8("  addc %[a],%[a],%[b]\n  mfxer %[u]\n  rlwinm %[u],%[u],3,31,31\n  add %[a],%[a],%[u]\n")
                   LOOP_TAIL
                   : [a] "+b"(a), [u] "=&b"(u), [n] "+b"(n) : [b] "b"(b) : "cr0", "xer");
  return a;
}
/* XER cleared by mtxer before every addc (the LuaJIT trace-head shape) */
static uint64_t F_mtxer_chain(uint64_t n) {
  uint64_t a = 1, b = 0x8000000000000003ull, u;
  __asm__ volatile(LOOP_HEAD
                   X8("  li %[u],0\n  mtxer %[u]\n  addc %[a],%[a],%[b]\n  addze %[a],%[a]\n")
                   LOOP_TAIL
                   : [a] "+b"(a), [u] "=&b"(u), [n] "+b"(n) : [b] "b"(b) : "cr0", "xer");
  return a;
}
/* 32-bit lazy carry: bit 32 of the 64-bit sum, no flags anywhere */
static uint64_t F_srdi32(uint64_t n) {
  uint64_t a = 1, b = 0x80000003ull, t, c;
  __asm__ volatile(LOOP_HEAD
                   X8("  add %[t],%[a],%[b]\n  srdi %[c],%[t],32\n  rldicl %[a],%[t],0,32\n  add %[a],%[a],%[c]\n")
                   LOOP_TAIL
                   : [a] "+b"(a), [t] "=&b"(t), [c] "=&b"(c), [n] "+b"(n) : [b] "b"(b) : "cr0");
  return a;
}
static uint64_t F_srdi32_ref(uint64_t n) {
  uint64_t a = 1, b = 0x80000003ull;
  for (uint64_t i = 0; i < 8 * n; i++) { uint64_t s = a + b; a = (s & 0xffffffffull) + (s >> 32); }
  return a;
}
/* recording form: CR0 written by add. but never read (does the CR write cost?) */
static uint64_t F_add_dot(uint64_t n) {
  uint64_t a = 1, b = 3;
  __asm__ volatile(LOOP_HEAD X8("  add. %[a],%[a],%[b]\n") LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : [b] "b"(b) : "cr0");
  return a;
}
/* CR0 written by add. and consumed by a dependent isel every op */
static uint64_t F_add_dot_isel(uint64_t n) {
  uint64_t a = 1, b = 3;
  __asm__ volatile(LOOP_HEAD X8("  add. %[a],%[a],%[b]\n  isel %[a],%[b],%[a],2\n") LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : [b] "b"(b) : "cr0");
  return a;
}
static uint64_t F_add_dot_isel_ref(uint64_t n) {
  uint64_t a = 1; for (uint64_t i = 0; i < 8 * n; i++) { a += 3; a = (a == 0) ? 3 : a; } return a;
}

/* ===================== S: store forwarding =============================== */

static uint64_t S_reg_rt1(uint64_t n) {     /* control: loop-carried add in a register */
  uint64_t a = 0;
  __asm__ volatile(LOOP_HEAD "  addi %[a],%[a],1\n" LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : : "cr0");
  return a;
}
static uint64_t S_n_ref(uint64_t n) { return n; }

static uint64_t S_ctx_rt1(uint64_t n) {     /* loop-carried through one 8-byte context slot */
  uint64_t a; ctx[0] = 0;
  __asm__ volatile(LOOP_HEAD "  ld %[a],0(%[p])\n  addi %[a],%[a],1\n  std %[a],0(%[p])\n" LOOP_TAIL
                   : [a] "=&b"(a), [n] "+b"(n) : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0];
}
static uint64_t S_ctx_rt4(uint64_t n) {     /* four independent slots, each round-tripping */
  uint64_t a, b, c, d; ctx[0] = ctx[1] = ctx[2] = ctx[3] = 0;
  __asm__ volatile(LOOP_HEAD
                   "  ld %[a],0(%[p])\n  ld %[b],8(%[p])\n  ld %[c],16(%[p])\n  ld %[d],24(%[p])\n"
                   "  addi %[a],%[a],1\n  addi %[b],%[b],1\n  addi %[c],%[c],1\n  addi %[d],%[d],1\n"
                   "  std %[a],0(%[p])\n  std %[b],8(%[p])\n  std %[c],16(%[p])\n  std %[d],24(%[p])\n"
                   LOOP_TAIL
                   : [a] "=&b"(a), [b] "=&b"(b), [c] "=&b"(c), [d] "=&b"(d), [n] "+b"(n)
                   : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0] + ctx[1] + ctx[2] + ctx[3];
}
static uint64_t S_4n_ref(uint64_t n) { return 4 * n; }
static uint64_t S_reg_rt4(uint64_t n) {
  uint64_t a = 0, b = 0, c = 0, d = 0;
  __asm__ volatile(LOOP_HEAD
                   "  addi %[a],%[a],1\n  addi %[b],%[b],1\n  addi %[c],%[c],1\n  addi %[d],%[d],1\n"
                   LOOP_TAIL
                   : [a] "+b"(a), [b] "+b"(b), [c] "+b"(c), [d] "+b"(d), [n] "+b"(n) : : "cr0");
  return a + b + c + d;
}
/* the crc32 inner-loop shape: 3 loop-carried slots, each ld / 2 ALU ops / std, plus a
   table lookup whose address depends on one of them */
static uint64_t S_ctx_crcshape(uint64_t n) {
  uint64_t a, b, c, t; ctx[0] = ctx[1] = ctx[2] = 0;
  __asm__ volatile(LOOP_HEAD
                   "  ld %[a],0(%[p])\n  ld %[b],8(%[p])\n  ld %[c],16(%[p])\n"
                   "  xor %[t],%[a],%[b]\n  rldicl %[t],%[t],0,60\n  sldi %[t],%[t],3\n"
                   "  ldx %[t],%[q],%[t]\n"
                   "  srdi %[a],%[a],8\n  xor %[a],%[a],%[t]\n"
                   "  addi %[b],%[b],1\n  addi %[c],%[c],1\n"
                   "  std %[a],0(%[p])\n  std %[b],8(%[p])\n  std %[c],16(%[p])\n"
                   LOOP_TAIL
                   : [a] "=&b"(a), [b] "=&b"(b), [c] "=&b"(c), [t] "=&b"(t), [n] "+b"(n)
                   : [p] "b"(ctx), [q] "b"(tocbuf) : "cr0", "memory");
  return ctx[0] + ctx[1] + ctx[2];
}
static uint64_t S_ctx_crcshape_ref(uint64_t n) {
  uint64_t a = 0, b = 0, c = 0;
  for (uint64_t i = 0; i < n; i++) { uint64_t t = tocbuf[(a ^ b) & 15]; a = (a >> 8) ^ t; b++; c++; }
  return a + b + c;
}
static uint64_t S_reg_crcshape(uint64_t n) { /* same computation, registers only */
  uint64_t a = 0, b = 0, c = 0, t;
  __asm__ volatile(LOOP_HEAD
                   "  xor %[t],%[a],%[b]\n  rldicl %[t],%[t],0,60\n  sldi %[t],%[t],3\n"
                   "  ldx %[t],%[q],%[t]\n"
                   "  srdi %[a],%[a],8\n  xor %[a],%[a],%[t]\n"
                   "  addi %[b],%[b],1\n  addi %[c],%[c],1\n"
                   LOOP_TAIL
                   : [a] "+b"(a), [b] "+b"(b), [c] "+b"(c), [t] "=&b"(t), [n] "+b"(n)
                   : [q] "b"(tocbuf) : "cr0", "memory");
  return a + b + c;
}

static uint64_t S_fwd_stw_ld(uint64_t n) {  /* 4-byte store, 8-byte load (store + cache merge) */
  uint64_t a; ctx[0] = 0;
  __asm__ volatile(LOOP_HEAD "  ld %[a],0(%[p])\n  addi %[a],%[a],1\n  stw %[a],0(%[p])\n" LOOP_TAIL
                   : [a] "=&b"(a), [n] "+b"(n) : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0];
}
static uint64_t S_fwd_std_lwz(uint64_t n) { /* 8-byte store, 4-byte load (subset) */
  uint64_t a; ctx[0] = 0;
  __asm__ volatile(LOOP_HEAD "  lwz %[a],0(%[p])\n  addi %[a],%[a],1\n  std %[a],0(%[p])\n" LOOP_TAIL
                   : [a] "=&b"(a), [n] "+b"(n) : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0];
}
static uint64_t S_fwd_2stw_ld(uint64_t n) { /* two 4-byte stores, one 8-byte load */
  uint64_t a, t; ctx[0] = 0;
  __asm__ volatile(LOOP_HEAD
                   "  ld %[a],0(%[p])\n  addi %[a],%[a],1\n  stw %[a],0(%[p])\n  srdi %[t],%[a],32\n  stw %[t],4(%[p])\n"
                   LOOP_TAIL
                   : [a] "=&b"(a), [t] "=&b"(t), [n] "+b"(n) : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0];
}
static uint64_t S_fwd_2stb_lhz(uint64_t n) { /* two 1-byte stores, one 2-byte load */
  uint64_t a, t; ctx[0] = 0;
  __asm__ volatile(LOOP_HEAD
                   "  lhz %[a],0(%[p])\n  addi %[a],%[a],1\n  stb %[a],0(%[p])\n  srdi %[t],%[a],8\n  stb %[t],1(%[p])\n"
                   LOOP_TAIL
                   : [a] "=&b"(a), [t] "=&b"(t), [n] "+b"(n) : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0] & 0xffff;
}
static uint64_t S_fwd_2stb_lhz_ref(uint64_t n) { return n & 0xffff; }
static uint64_t S_fwd_cross8(uint64_t n) {  /* 8-byte slot straddling a doubleword boundary */
  uint64_t a; uint8_t *p = (uint8_t *)ctx + 4; memset(ctx, 0, 64);
  __asm__ volatile(LOOP_HEAD "  ld %[a],0(%[p])\n  addi %[a],%[a],1\n  std %[a],0(%[p])\n" LOOP_TAIL
                   : [a] "=&b"(a), [n] "+b"(n) : [p] "b"(p) : "cr0", "memory");
  uint64_t r; memcpy(&r, p, 8); return r;
}
static uint64_t S_fwd_cross32(uint64_t n) { /* 8-byte slot straddling a 32-byte granule */
  uint64_t a; uint8_t *p = (uint8_t *)ctx + 28; memset(ctx, 0, 64);
  __asm__ volatile(LOOP_HEAD "  ld %[a],0(%[p])\n  addi %[a],%[a],1\n  std %[a],0(%[p])\n" LOOP_TAIL
                   : [a] "=&b"(a), [n] "+b"(n) : [p] "b"(p) : "cr0", "memory");
  uint64_t r; memcpy(&r, p, 8); return r;
}
/* store then reload through a DIFFERENT base register (aliased): tests the
   base-register LHS/SHL predictor described in UM 25.1.7.7 */
static uint64_t S_alias_rt1(uint64_t n) {
  uint64_t a = 0; ctx[0] = 0; uint64_t *q = ctx;
  __asm__ volatile("  mr %[q],%[p]\n" LOOP_HEAD
                   "  addi %[a],%[a],1\n  std %[a],0(%[p])\n  ld %[a],0(%[q])\n" LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n), [q] "+&b"(q) : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0];
}
static uint64_t S_samebase_rt1(uint64_t n) {
  uint64_t a = 0; ctx[0] = 0;
  __asm__ volatile(LOOP_HEAD
                   "  addi %[a],%[a],1\n  std %[a],0(%[p])\n  ld %[a],0(%[p])\n" LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0];
}
/* block-local allocation model: load 4 slots once, 8 iterations of work in
   registers, store 4 slots once (per outer iteration) */
static uint64_t S_blocklocal(uint64_t n) {
  uint64_t a, b, c, d; ctx[0] = ctx[1] = ctx[2] = ctx[3] = 0;
  __asm__ volatile(LOOP_HEAD
                   "  ld %[a],0(%[p])\n  ld %[b],8(%[p])\n  ld %[c],16(%[p])\n  ld %[d],24(%[p])\n"
                   X8("  addi %[a],%[a],1\n  addi %[b],%[b],1\n  addi %[c],%[c],1\n  addi %[d],%[d],1\n")
                   "  std %[a],0(%[p])\n  std %[b],8(%[p])\n  std %[c],16(%[p])\n  std %[d],24(%[p])\n"
                   LOOP_TAIL
                   : [a] "=&b"(a), [b] "=&b"(b), [c] "=&b"(c), [d] "=&b"(d), [n] "+b"(n)
                   : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0] + ctx[1] + ctx[2] + ctx[3];
}
static uint64_t S_32n_ref(uint64_t n) { return 32 * n; }
static uint64_t S_ctx_rt4_x8(uint64_t n) {  /* the same work with a round trip per op */
  uint64_t a, b, c, d; ctx[0] = ctx[1] = ctx[2] = ctx[3] = 0;
  __asm__ volatile(LOOP_HEAD
                   X8("  ld %[a],0(%[p])\n  ld %[b],8(%[p])\n  ld %[c],16(%[p])\n  ld %[d],24(%[p])\n"
                      "  addi %[a],%[a],1\n  addi %[b],%[b],1\n  addi %[c],%[c],1\n  addi %[d],%[d],1\n"
                      "  std %[a],0(%[p])\n  std %[b],8(%[p])\n  std %[c],16(%[p])\n  std %[d],24(%[p])\n")
                   LOOP_TAIL
                   : [a] "=&b"(a), [b] "=&b"(b), [c] "=&b"(c), [d] "=&b"(d), [n] "+b"(n)
                   : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0] + ctx[1] + ctx[2] + ctx[3];
}

/* ===================== B: branches ======================================= */

static uint64_t B_bctr_next(uint64_t n) {   /* one predicted bctr per iteration */
  uint64_t t, a = 0;
  __asm__ volatile("  bcl 20,31,.+4\n8: mflr %[t]\n  addi %[t],%[t],2f-8b\n"
                   LOOP_HEAD
                   "  mtctr %[t]\n  bctr\n"
                   "  .p2align 5\n2:\n  addi %[a],%[a],1\n"
                   LOOP_TAIL
                   : [a] "+b"(a), [t] "=&b"(t), [n] "+b"(n) : : "cr0", "ctr", "lr");
  return a;
}
/* two predicted bctr in the SAME aligned 32-byte block */
static uint64_t B_bctr2_sameblock(uint64_t n) {
  uint64_t t, u, a = 0;
  __asm__ volatile("  bcl 20,31,.+4\n8: mflr %[t]\n  addi %[u],%[t],3f-8b\n  addi %[t],%[t],2f-8b\n"
                   LOOP_HEAD
                   "  mtctr %[t]\n  bctr\n"
                   "2:\n  mtctr %[u]\n  bctr\n"
                   "3:\n  addi %[a],%[a],1\n"
                   LOOP_TAIL
                   : [a] "+b"(a), [t] "=&b"(t), [u] "=&b"(u), [n] "+b"(n) : : "cr0", "ctr", "lr");
  return a;
}
/* two predicted bctr in DIFFERENT 32-byte blocks */
static uint64_t B_bctr2_sepblock(uint64_t n) {
  uint64_t t, u, a = 0;
  __asm__ volatile("  bcl 20,31,.+4\n8: mflr %[t]\n  addi %[u],%[t],3f-8b\n  addi %[t],%[t],2f-8b\n"
                   LOOP_HEAD
                   "  mtctr %[t]\n  bctr\n"
                   "  .p2align 5\n2:\n  mtctr %[u]\n  bctr\n"
                   "  .p2align 5\n3:\n  addi %[a],%[a],1\n"
                   LOOP_TAIL
                   : [a] "+b"(a), [t] "=&b"(t), [u] "=&b"(u), [n] "+b"(n) : : "cr0", "ctr", "lr");
  return a;
}
/* 8 not-taken conditional branches packed in one 32-byte block */
static uint64_t B_bc8_packed(uint64_t n) {
  uint64_t a = 0;
  __asm__ volatile("  cmpdi 7,%[n],0\n" LOOP_HEAD
                   X8("  beq 7,9f\n")
                   "  addi %[a],%[a],1\n" LOOP_TAIL "9:\n"
                   : [a] "+b"(a), [n] "+b"(n) : : "cr0", "cr7");
  return a;
}
/* 8 not-taken conditional branches, each followed by 3 ALU ops */
static uint64_t B_bc8_spread(uint64_t n) {
  uint64_t a = 0, b = 0, c = 0, d = 0;
  __asm__ volatile("  cmpdi 7,%[n],0\n" LOOP_HEAD
                   X8("  beq 7,9f\n  addi %[b],%[b],1\n  addi %[c],%[c],1\n  addi %[d],%[d],1\n")
                   "  addi %[a],%[a],1\n" LOOP_TAIL "9:\n"
                   : [a] "+b"(a), [b] "+b"(b), [c] "+b"(c), [d] "+b"(d), [n] "+b"(n) : : "cr0", "cr7");
  return a;
}
/* 8 TAKEN unconditional branches per iteration (block-to-block chaining) */
static uint64_t B_b8_taken(uint64_t n) {
  uint64_t a = 0;
  __asm__ volatile(LOOP_HEAD
                   "  b 2f\n  .p2align 4\n2: b 3f\n  .p2align 4\n3: b 4f\n  .p2align 4\n4: b 5f\n"
                   "  .p2align 4\n5: b 6f\n  .p2align 4\n6: b 7f\n  .p2align 4\n7: b 8f\n  .p2align 4\n8: b 9f\n"
                   "  .p2align 4\n9:\n  addi %[a],%[a],1\n" LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : : "cr0");
  return a;
}
/* 8 sequential ALU ops, no branches (control for the two above) */
static uint64_t B_none(uint64_t n) {
  uint64_t a = 0;
  __asm__ volatile(LOOP_HEAD "  addi %[a],%[a],1\n" LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : : "cr0");
  return a;
}

/* --- call/return forms. Two call sites chosen by a random bit; callee adds 1. --- */
#define RAND2_HEAD                                            \
  LOOP_HEAD                                                   \
  "  lbzx %[t],%[arr],%[i]\n  addi %[i],%[i],1\n  rldicl %[i],%[i],0,44\n" \
  "  cmpwi %[t],0\n  beq 2f\n"
#define RAND2_MID "  b 3f\n  .p2align 4\n2:\n"
#define RAND2_TAIL "3:\n" LOOP_TAIL "  b 9f\n  .p2align 5\n5:\n"

static uint64_t B_r2_bl_blr(uint64_t n) {
  uint64_t a = 0, t, i = 0;
  __asm__ volatile(RAND2_HEAD "  bl 5f\n" RAND2_MID "  bl 5f\n" RAND2_TAIL
                   "  addi %[a],%[a],1\n  blr\n9:\n"
                   : [a] "+b"(a), [t] "=&b"(t), [i] "+b"(i), [n] "+b"(n)
                   : [arr] "b"(prog_rand2) : "cr0", "lr", "memory");
  return a;
}
static uint64_t B_r2_bl_bctr(uint64_t n) {  /* RET through the count cache */
  uint64_t a = 0, t, i = 0;
  __asm__ volatile(RAND2_HEAD "  bl 5f\n" RAND2_MID "  bl 5f\n" RAND2_TAIL
                   "  mflr %[t]\n  mtctr %[t]\n  addi %[a],%[a],1\n  bctr\n9:\n"
                   : [a] "+b"(a), [t] "=&b"(t), [i] "+b"(i), [n] "+b"(n)
                   : [arr] "b"(prog_rand2) : "cr0", "lr", "ctr", "memory");
  return a;
}
static uint64_t B_r2_bl_mtlr_blr(uint64_t n) { /* LR round-tripped through memory (shadow stack) */
  uint64_t a = 0, t, i = 0;
  __asm__ volatile(RAND2_HEAD "  bl 5f\n" RAND2_MID "  bl 5f\n" RAND2_TAIL
                   "  mflr %[t]\n  std %[t],64(%[p])\n  ld %[t],64(%[p])\n  mtlr %[t]\n  addi %[a],%[a],1\n  blr\n9:\n"
                   : [a] "+b"(a), [t] "=&b"(t), [i] "+b"(i), [n] "+b"(n)
                   : [arr] "b"(prog_rand2), [p] "b"(ctx) : "cr0", "lr", "memory");
  return a;
}
static uint64_t B_r2_bctrl_blr(uint64_t n) { /* indirect call with LK=1, plain blr */
  uint64_t a = 0, t, f, i = 0;
  __asm__ volatile("  bcl 20,31,.+4\n8: mflr %[f]\n  addi %[f],%[f],5f-8b\n"
                   RAND2_HEAD "  mtctr %[f]\n  bctrl\n" RAND2_MID "  mtctr %[f]\n  bctrl\n" RAND2_TAIL
                   "  addi %[a],%[a],1\n  blr\n9:\n"
                   : [a] "+b"(a), [t] "=&b"(t), [f] "=&b"(f), [i] "+b"(i), [n] "+b"(n)
                   : [arr] "b"(prog_rand2) : "cr0", "lr", "ctr", "memory");
  return a;
}
static uint64_t B_r2_b_mtlr_blr(uint64_t n) { /* call with NO link push (LR set by hand), blr return */
  uint64_t a = 0, t, i = 0;
  __asm__ volatile(RAND2_HEAD
                   "  bcl 20,31,.+4\n6: mflr %[t]\n  addi %[t],%[t],7f-6b\n  mtlr %[t]\n  b 5f\n7:\n"
                   RAND2_MID
                   "  bcl 20,31,.+4\n6: mflr %[t]\n  addi %[t],%[t],7f-6b\n  mtlr %[t]\n  b 5f\n7:\n"
                   RAND2_TAIL
                   "  addi %[a],%[a],1\n  blr\n9:\n"
                   : [a] "+b"(a), [t] "=&b"(t), [i] "+b"(i), [n] "+b"(n)
                   : [arr] "b"(prog_rand2) : "cr0", "lr", "memory");
  return a;
}
static uint64_t B_r2_b_mtctr_bctr(uint64_t n) { /* call with b, return via mtctr/bctr (count cache, 2 targets) */
  uint64_t a = 0, t, i = 0;
  __asm__ volatile(RAND2_HEAD
                   "  bcl 20,31,.+4\n6: mflr %[t]\n  addi %[t],%[t],7f-6b\n  mtctr %[t]\n  b 5f\n7:\n"
                   RAND2_MID
                   "  bcl 20,31,.+4\n6: mflr %[t]\n  addi %[t],%[t],7f-6b\n  mtctr %[t]\n  b 5f\n7:\n"
                   RAND2_TAIL
                   "  addi %[a],%[a],1\n  bctr\n9:\n"
                   : [a] "+b"(a), [t] "=&b"(t), [i] "+b"(i), [n] "+b"(n)
                   : [arr] "b"(prog_rand2) : "cr0", "ctr", "lr", "memory");
  return a;
}
/* single call site (control): bl/blr */
static uint64_t B_bl_blr(uint64_t n) {
  uint64_t a = 0;
  __asm__ volatile(LOOP_HEAD "  bl 5f\n" LOOP_TAIL "  b 9f\n  .p2align 5\n5:\n  addi %[a],%[a],1\n  blr\n9:\n"
                   : [a] "+b"(a), [n] "+b"(n) : : "cr0", "lr");
  return a;
}

/* recursion to depth D (bl/blr pairs, LR saved on a private stack) */
static uint64_t B_depth(uint64_t n, uint64_t D) {
  uint64_t a = 0, d, t, sp = (uint64_t)stackbuf + 16 * 1000;
  __asm__ volatile(LOOP_HEAD
                   "  mr %[d],%[D]\n  bl 5f\n" LOOP_TAIL "  b 9f\n"
                   "  .p2align 5\n5:\n"
                   "  addi %[a],%[a],1\n  addi %[d],%[d],-1\n  cmpdi %[d],0\n  beq 6f\n"
                   "  mflr %[t]\n  stdu %[t],-16(%[sp])\n  bl 5b\n  ld %[t],0(%[sp])\n  addi %[sp],%[sp],16\n  mtlr %[t]\n"
                   "6:\n  blr\n9:\n"
                   : [a] "+b"(a), [d] "=&b"(d), [t] "=&b"(t), [sp] "+b"(sp), [n] "+b"(n)
                   : [D] "b"(D) : "cr0", "lr", "memory");
  return a;
}
static uint64_t B_depth8(uint64_t n)   { return B_depth(n, 8); }
static uint64_t B_depth24(uint64_t n)  { return B_depth(n, 24); }
static uint64_t B_depth48(uint64_t n)  { return B_depth(n, 48); }
static uint64_t B_depth96(uint64_t n)  { return B_depth(n, 96); }
static uint64_t B_depth8_ref(uint64_t n)  { return 8 * n; }
static uint64_t B_depth24_ref(uint64_t n) { return 24 * n; }
static uint64_t B_depth48_ref(uint64_t n) { return 48 * n; }
static uint64_t B_depth96_ref(uint64_t n) { return 96 * n; }

/* ===================== D: dispatch (the vm shape) ======================== */
/* 16 handlers h_k: a += k+1, then back to the dispatch head. */
#define HANDLERS                                                        \
  "  .p2align 4\n4:\n  addi %[a],%[a],1\n  b 3f\n"                     \
  "  .p2align 4\n  addi %[a],%[a],2\n  b 3f\n"                          \
  "  .p2align 4\n  addi %[a],%[a],3\n  b 3f\n"                          \
  "  .p2align 4\n  addi %[a],%[a],4\n  b 3f\n"                          \
  "  .p2align 4\n  addi %[a],%[a],5\n  b 3f\n"                          \
  "  .p2align 4\n  addi %[a],%[a],6\n  b 3f\n"                          \
  "  .p2align 4\n  addi %[a],%[a],7\n  b 3f\n"                          \
  "  .p2align 4\n  addi %[a],%[a],8\n  b 3f\n"                          \
  "  .p2align 4\n  addi %[a],%[a],9\n  b 3f\n"                          \
  "  .p2align 4\n  addi %[a],%[a],10\n  b 3f\n"                         \
  "  .p2align 4\n  addi %[a],%[a],11\n  b 3f\n"                         \
  "  .p2align 4\n  addi %[a],%[a],12\n  b 3f\n"                         \
  "  .p2align 4\n  addi %[a],%[a],13\n  b 3f\n"                         \
  "  .p2align 4\n  addi %[a],%[a],14\n  b 3f\n"                         \
  "  .p2align 4\n  addi %[a],%[a],15\n  b 3f\n"                         \
  "  .p2align 4\n  addi %[a],%[a],16\n  b 3f\n"
/* handler k is at 4b + 16*k: fill the table from the label */
#define TAB_SETUP(stride)                                               \
  "  bcl 20,31,.+4\n8: mflr %[t]\n  addi %[t],%[t],4f-8b\n"           \
  "  li %[u],0\n7:\n  stdx %[t],%[tab],%[u]\n  addi %[u],%[u],8\n"  \
  "  addi %[t],%[t]," #stride "\n  cmpdi %[u],128\n  bne 7b\n"
#define FETCH "  lbzx %[op],%[prog],%[i]\n  addi %[i],%[i],1\n  rldicl %[i],%[i],0,44\n"
#define BCTR_TAIL "  sldi %[op],%[op],3\n  ldx %[t],%[tab],%[op]\n  mtctr %[t]\n  bctr\n"
#define DISP_END "3:\n" LOOP_TAIL

static uint64_t D_bctr(uint64_t n, const uint8_t *prog) {
  uint64_t a = 0, t, u, op, i = 0;
  __asm__ volatile(TAB_SETUP(16) LOOP_HEAD FETCH BCTR_TAIL HANDLERS DISP_END
                   : [a] "+b"(a), [t] "=&b"(t), [u] "=&b"(u), [op] "=&b"(op), [i] "+b"(i), [n] "+b"(n)
                   : [prog] "b"(prog), [tab] "b"(tab) : "cr0", "ctr", "lr", "memory");
  return a;
}
/* inline-cache form: compare chain for opcodes 0..3, bctr fallback */
static uint64_t D_cmp(uint64_t n, const uint8_t *prog) {
  uint64_t a = 0, t, u, op, i = 0;
  __asm__ volatile(TAB_SETUP(16) LOOP_HEAD FETCH
                   "  cmpdi %[op],0\n  beq 4f\n"
                   "  cmpdi %[op],1\n  beq 10f\n"
                   "  cmpdi %[op],2\n  beq 11f\n"
                   "  cmpdi %[op],3\n  beq 12f\n"
                   BCTR_TAIL
                   "  .p2align 4\n4:\n  addi %[a],%[a],1\n  b 3f\n"
                   "  .p2align 4\n10:\n  addi %[a],%[a],2\n  b 3f\n"
                   "  .p2align 4\n11:\n  addi %[a],%[a],3\n  b 3f\n"
                   "  .p2align 4\n12:\n  addi %[a],%[a],4\n  b 3f\n"
                   "  .p2align 4\n  addi %[a],%[a],5\n  b 3f\n"
                   "  .p2align 4\n  addi %[a],%[a],6\n  b 3f\n"
                   "  .p2align 4\n  addi %[a],%[a],7\n  b 3f\n"
                   "  .p2align 4\n  addi %[a],%[a],8\n  b 3f\n"
                   "  .p2align 4\n  addi %[a],%[a],9\n  b 3f\n"
                   "  .p2align 4\n  addi %[a],%[a],10\n  b 3f\n"
                   "  .p2align 4\n  addi %[a],%[a],11\n  b 3f\n"
                   "  .p2align 4\n  addi %[a],%[a],12\n  b 3f\n"
                   "  .p2align 4\n  addi %[a],%[a],13\n  b 3f\n"
                   "  .p2align 4\n  addi %[a],%[a],14\n  b 3f\n"
                   "  .p2align 4\n  addi %[a],%[a],15\n  b 3f\n"
                   "  .p2align 4\n  addi %[a],%[a],16\n  b 3f\n"
                   DISP_END
                   : [a] "+b"(a), [t] "=&b"(t), [u] "=&b"(u), [op] "=&b"(op), [i] "+b"(i), [n] "+b"(n)
                   : [prog] "b"(prog), [tab] "b"(tab) : "cr0", "ctr", "lr", "memory");
  return a;
}
/* replicated tails: every handler ends in its own fetch+bctr (LLInt / JIT shape:
   one indirect branch site per guest BR) */
#define RTAIL "  addi %[n],%[n],-1\n  cmpdi %[n],0\n  beq 3f\n" FETCH BCTR_TAIL
static uint64_t D_bctr_repl(uint64_t n, const uint8_t *prog) {
  uint64_t a = 0, t, u, op, i = 0;
  __asm__ volatile(TAB_SETUP(64) LOOP_HEAD FETCH BCTR_TAIL
                   "  .p2align 6\n4:\n  addi %[a],%[a],1\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],2\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],3\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],4\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],5\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],6\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],7\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],8\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],9\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],10\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],11\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],12\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],13\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],14\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],15\n" RTAIL
                   "  .p2align 6\n  addi %[a],%[a],16\n" RTAIL
                   "3:\n"
                   : [a] "+b"(a), [t] "=&b"(t), [u] "=&b"(u), [op] "=&b"(op), [i] "+b"(i), [n] "+b"(n)
                   : [prog] "b"(prog), [tab] "b"(tab) : "cr0", "ctr", "lr", "memory");
  return a;
}
static uint64_t D_ref(uint64_t n, const uint8_t *prog) {
  uint64_t a = 0; for (uint64_t i = 0; i < n; i++) a += prog[i & (PROG_LEN - 1)] + 1; return a;
}
#define DISPATCH_PROBE(kind, progname)                                              \
  static uint64_t D_##kind##_##progname(uint64_t n) { return D_##kind(n, prog_##progname); } \
  static uint64_t D_##kind##_##progname##_ref(uint64_t n) { return D_ref(n, prog_##progname); }
DISPATCH_PROBE(bctr, rand4) DISPATCH_PROBE(bctr, rand16) DISPATCH_PROBE(bctr, rep8)
DISPATCH_PROBE(bctr, rep64) DISPATCH_PROBE(bctr, skew)
DISPATCH_PROBE(cmp, rand4) DISPATCH_PROBE(cmp, rep8) DISPATCH_PROBE(cmp, skew) DISPATCH_PROBE(cmp, rand16)
DISPATCH_PROBE(bctr_repl, rand4) DISPATCH_PROBE(bctr_repl, rep8) DISPATCH_PROBE(bctr_repl, rep64)

/* ===================== L: larx/stcx and barriers ========================= */

static uint64_t L_lwz_stw(uint64_t n) {
  uint64_t a; ctx[0] = 0;
  __asm__ volatile(LOOP_HEAD "  lwz %[a],0(%[p])\n  addi %[a],%[a],1\n  stw %[a],0(%[p])\n" LOOP_TAIL
                   : [a] "=&b"(a), [n] "+b"(n) : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0];
}
static uint64_t L_larx_stcx(uint64_t n) {   /* uncontended lwarx/stwcx. increment */
  uint64_t a; ctx[0] = 0;
  __asm__ volatile(LOOP_HEAD "2:\n  lwarx %[a],0,%[p]\n  addi %[a],%[a],1\n  stwcx. %[a],0,%[p]\n  bne- 2b\n" LOOP_TAIL
                   : [a] "=&b"(a), [n] "+b"(n) : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0];
}
static uint64_t L_larx_stcx_acqrel(uint64_t n) { /* lwsync before, isync after (LDAXR/STLXR-style) */
  uint64_t a; ctx[0] = 0;
  __asm__ volatile(LOOP_HEAD "  lwsync\n2:\n  lwarx %[a],0,%[p]\n  addi %[a],%[a],1\n  stwcx. %[a],0,%[p]\n  bne- 2b\n  isync\n" LOOP_TAIL
                   : [a] "=&b"(a), [n] "+b"(n) : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0];
}
static uint64_t L_lwz_stw_lwsync(uint64_t n) {
  uint64_t a; ctx[0] = 0;
  __asm__ volatile(LOOP_HEAD "  lwz %[a],0(%[p])\n  addi %[a],%[a],1\n  stw %[a],0(%[p])\n  lwsync\n" LOOP_TAIL
                   : [a] "=&b"(a), [n] "+b"(n) : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0];
}
static uint64_t L_lwz_stw_hwsync(uint64_t n) {
  uint64_t a; ctx[0] = 0;
  __asm__ volatile(LOOP_HEAD "  lwz %[a],0(%[p])\n  addi %[a],%[a],1\n  stw %[a],0(%[p])\n  sync\n" LOOP_TAIL
                   : [a] "=&b"(a), [n] "+b"(n) : [p] "b"(ctx) : "cr0", "memory");
  return ctx[0];
}

/* ===================== T: constants ====================================== */
#define K5 0x1234567890ABCDEFull
static uint64_t T_mat5_x4(uint64_t n) {     /* 5-instruction materialisation, 4 independent uses */
  uint64_t a = 0, b = 0, c = 0, d = 0, t;
  __asm__ volatile(LOOP_HEAD
                   "  lis %[t],0x1234\n  ori %[t],%[t],0x5678\n  rldicr %[t],%[t],32,31\n  oris %[t],%[t],0x90ab\n  ori %[t],%[t],0xcdef\n  add %[a],%[a],%[t]\n"
                   "  lis %[t],0x1234\n  ori %[t],%[t],0x5678\n  rldicr %[t],%[t],32,31\n  oris %[t],%[t],0x90ab\n  ori %[t],%[t],0xcdef\n  add %[b],%[b],%[t]\n"
                   "  lis %[t],0x1234\n  ori %[t],%[t],0x5678\n  rldicr %[t],%[t],32,31\n  oris %[t],%[t],0x90ab\n  ori %[t],%[t],0xcdef\n  add %[c],%[c],%[t]\n"
                   "  lis %[t],0x1234\n  ori %[t],%[t],0x5678\n  rldicr %[t],%[t],32,31\n  oris %[t],%[t],0x90ab\n  ori %[t],%[t],0xcdef\n  add %[d],%[d],%[t]\n"
                   LOOP_TAIL
                   : [a] "+b"(a), [b] "+b"(b), [c] "+b"(c), [d] "+b"(d), [t] "=&b"(t), [n] "+b"(n) : : "cr0");
  return a + b + c + d;
}
static uint64_t T_x4_ref(uint64_t n) { return 4 * n * K5; }
static uint64_t T_ld_x4(uint64_t n) {       /* constant-pool load, 4 independent uses */
  uint64_t a = 0, b = 0, c = 0, d = 0, t;
  __asm__ volatile(LOOP_HEAD
                   "  ld %[t],0(%[q])\n  add %[a],%[a],%[t]\n"
                   "  ld %[t],0(%[q])\n  add %[b],%[b],%[t]\n"
                   "  ld %[t],0(%[q])\n  add %[c],%[c],%[t]\n"
                   "  ld %[t],0(%[q])\n  add %[d],%[d],%[t]\n"
                   LOOP_TAIL
                   : [a] "+b"(a), [b] "+b"(b), [c] "+b"(c), [d] "+b"(d), [t] "=&b"(t), [n] "+b"(n)
                   : [q] "b"(tocbuf) : "cr0", "memory");
  return a + b + c + d;
}
static uint64_t T_addpcis_ld_x4_p9(uint64_t n) { /* pc-relative pool: addpcis + ld */
  uint64_t a = 0, b = 0, c = 0, d = 0, t;
  __asm__ volatile("  b 2f\n  .p2align 3\n7:\n  .quad 0x1234567890ABCDEF\n2:\n" LOOP_HEAD
                   "  addpcis %[t],0\n  ld %[t],7b-.(%[t])\n  add %[a],%[a],%[t]\n"
                   "  addpcis %[t],0\n  ld %[t],7b-.(%[t])\n  add %[b],%[b],%[t]\n"
                   "  addpcis %[t],0\n  ld %[t],7b-.(%[t])\n  add %[c],%[c],%[t]\n"
                   "  addpcis %[t],0\n  ld %[t],7b-.(%[t])\n  add %[d],%[d],%[t]\n"
                   LOOP_TAIL
                   : [a] "+b"(a), [b] "+b"(b), [c] "+b"(c), [d] "+b"(d), [t] "=&b"(t), [n] "+b"(n) : : "cr0", "memory");
  return a + b + c + d;
}
/* the materialisation on the critical path: a = a + K where K is rebuilt each time
   and the build is made dependent on a (xor with a value that is always zero) */
static uint64_t T_mat5_dep(uint64_t n) {
  uint64_t a = 0, t, z = 0;
  __asm__ volatile(LOOP_HEAD
                   "  and %[t],%[a],%[z]\n  addis %[t],%[t],0x1234\n  ori %[t],%[t],0x5678\n  rldicr %[t],%[t],32,31\n  oris %[t],%[t],0x90ab\n  ori %[t],%[t],0xcdef\n  add %[a],%[a],%[t]\n"
                   LOOP_TAIL
                   : [a] "+&b"(a), [t] "=&b"(t), [n] "+b"(n) : [z] "b"(z) : "cr0");
  return a;
}
static uint64_t T_mat5_dep_ref(uint64_t n) { return n * K5; }
static uint64_t T_ld_dep(uint64_t n) {
  uint64_t a = 0, t, z = 0;
  __asm__ volatile(LOOP_HEAD
                   "  and %[t],%[a],%[z]\n  ldx %[t],%[q],%[t]\n  add %[a],%[a],%[t]\n"
                   LOOP_TAIL
                   : [a] "+&b"(a), [t] "=&b"(t), [n] "+b"(n) : [z] "b"(z), [q] "b"(tocbuf) : "cr0", "memory");
  return a;
}


/* ===================== Q: loop-shape ceiling ============================= */
static uint64_t Q_loop_bne(uint64_t n) {     /* addi n / cmpdi / bne (the LOOP_TAIL shape) */
  uint64_t a = 0;
  __asm__ volatile(LOOP_HEAD "  addi %[a],%[a],1\n" LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : : "cr0");
  return a;
}
static uint64_t Q_loop_bdnz(uint64_t n) {    /* CTR-counted loop, no CR dependency */
  uint64_t a = 0;
  __asm__ volatile("  mtctr %[n]\n  .p2align 5\n1:\n  addi %[a],%[a],1\n  bdnz 1b\n"
                   : [a] "+b"(a) : [n] "b"(n) : "ctr");
  return a;
}
static uint64_t Q_loop_cmpfar(uint64_t n) {  /* compare issued early, 8 independent adds, then bne */
  uint64_t a0 = 11, a1 = 12, a2 = 13, a3 = 14, a4 = 15, a5 = 16, a6 = 17, a7 = 18, b = 3;
  __asm__ volatile("  .p2align 5\n1:\n  addi %[n],%[n],-1\n  cmpdi %[n],0\n"
                   "  add %[a0],%[a0],%[b]\n  add %[a1],%[a1],%[b]\n"
                   "  add %[a2],%[a2],%[b]\n  add %[a3],%[a3],%[b]\n"
                   "  add %[a4],%[a4],%[b]\n  add %[a5],%[a5],%[b]\n"
                   "  add %[a6],%[a6],%[b]\n  add %[a7],%[a7],%[b]\n"
                   "  bne 1b\n"
                   : [a0] "+b"(a0), [a1] "+b"(a1), [a2] "+b"(a2), [a3] "+b"(a3),
                     [a4] "+b"(a4), [a5] "+b"(a5), [a6] "+b"(a6), [a7] "+b"(a7), [n] "+b"(n)
                   : [b] "b"(b) : "cr0");
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}
static uint64_t Q_loop_bne_x4(uint64_t n) {  /* one iteration = 4 blocks each ending in a taken bne */
  uint64_t a = 0;
  __asm__ volatile("  cmpdi 7,%[n],0\n" LOOP_HEAD
                   "  addi %[a],%[a],1\n  bne 7,2f\n  .p2align 5\n2:\n"
                   "  addi %[a],%[a],1\n  bne 7,3f\n  .p2align 5\n3:\n"
                   "  addi %[a],%[a],1\n  bne 7,4f\n  .p2align 5\n4:\n"
                   "  addi %[a],%[a],1\n" LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : : "cr0", "cr7");
  return a;
}
static uint64_t Q_loop_b_x4(uint64_t n) {    /* same with unconditional b */
  uint64_t a = 0;
  __asm__ volatile(LOOP_HEAD
                   "  addi %[a],%[a],1\n  b 2f\n  .p2align 5\n2:\n"
                   "  addi %[a],%[a],1\n  b 3f\n  .p2align 5\n3:\n"
                   "  addi %[a],%[a],1\n  b 4f\n  .p2align 5\n4:\n"
                   "  addi %[a],%[a],1\n" LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : : "cr0");
  return a;
}
static uint64_t Q_loop_bctr_x4(uint64_t n) { /* same with mtctr/bctr (JIT block-exit shape) */
  uint64_t a = 0, t, u, v;
  __asm__ volatile("  bcl 20,31,.+4\n8: mflr %[t]\n  addi %[u],%[t],3f-8b\n  addi %[v],%[t],4f-8b\n  addi %[t],%[t],2f-8b\n"
                   LOOP_HEAD
                   "  addi %[a],%[a],1\n  mtctr %[t]\n  bctr\n  .p2align 5\n2:\n"
                   "  addi %[a],%[a],1\n  mtctr %[u]\n  bctr\n  .p2align 5\n3:\n"
                   "  addi %[a],%[a],1\n  mtctr %[v]\n  bctr\n  .p2align 5\n4:\n"
                   "  addi %[a],%[a],1\n" LOOP_TAIL
                   : [a] "+b"(a), [t] "=&b"(t), [u] "=&b"(u), [v] "=&b"(v), [n] "+b"(n) : : "cr0", "ctr", "lr");
  return a;
}


static uint64_t Q_loop_bback(uint64_t n) {   /* forward conditional exit, unconditional back-edge */
  uint64_t a = 0;
  __asm__ volatile(LOOP_HEAD "  addi %[a],%[a],1\n  addi %[n],%[n],-1\n  cmpdi %[n],0\n  beq 9f\n  b 1b\n9:\n"
                   : [a] "+b"(a), [n] "+b"(n) : : "cr0");
  return a;
}
static uint64_t Q_fwd_far_x4(uint64_t n) {   /* 4 forward b per iter, each over 256 bytes of nops */
  uint64_t a = 0;
  __asm__ volatile(LOOP_HEAD
                   "  addi %[a],%[a],1\n  b 2f\n  .rept 64\n  nop\n  .endr\n2:\n"
                   "  addi %[a],%[a],1\n  b 3f\n  .rept 64\n  nop\n  .endr\n3:\n"
                   "  addi %[a],%[a],1\n  b 4f\n  .rept 64\n  nop\n  .endr\n4:\n"
                   "  addi %[a],%[a],1\n" LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : : "cr0");
  return a;
}
static uint64_t Q_fwd_near_x4(uint64_t n) {  /* 4 forward b per iter, each over 16 bytes of nops */
  uint64_t a = 0;
  __asm__ volatile(LOOP_HEAD
                   "  addi %[a],%[a],1\n  b 2f\n  .rept 4\n  nop\n  .endr\n2:\n"
                   "  addi %[a],%[a],1\n  b 3f\n  .rept 4\n  nop\n  .endr\n3:\n"
                   "  addi %[a],%[a],1\n  b 4f\n  .rept 4\n  nop\n  .endr\n4:\n"
                   "  addi %[a],%[a],1\n" LOOP_TAIL
                   : [a] "+b"(a), [n] "+b"(n) : : "cr0");
  return a;
}
static uint64_t Q_loop_long(uint64_t n) {    /* 64-instruction body (2 x 8 independent adds x 4), one back-edge */
  uint64_t a0 = 11, a1 = 12, a2 = 13, a3 = 14, a4 = 15, a5 = 16, a6 = 17, a7 = 18, b = 3;
  __asm__ volatile(LOOP_HEAD
                   X8("  add %[a0],%[a0],%[b]\n  add %[a1],%[a1],%[b]\n  add %[a2],%[a2],%[b]\n  add %[a3],%[a3],%[b]\n"
                      "  add %[a4],%[a4],%[b]\n  add %[a5],%[a5],%[b]\n  add %[a6],%[a6],%[b]\n  add %[a7],%[a7],%[b]\n")
                   LOOP_TAIL
                   : [a0] "+b"(a0), [a1] "+b"(a1), [a2] "+b"(a2), [a3] "+b"(a3),
                     [a4] "+b"(a4), [a5] "+b"(a5), [a6] "+b"(a6), [a7] "+b"(a7), [n] "+b"(n)
                   : [b] "b"(b) : "cr0");
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}
static uint64_t Q_loop_long_ref(uint64_t n) { return 116 + 64 * n * 3; }

/* ===================== registry ========================================== */
typedef struct { const char *name; probe_fn fn; probe_fn ref; const char *desc; } probe_t;
#define P(name, ref, desc) { #name, name, ref, desc }
static const probe_t probes[] = {
  P(A_chain1,    A_chain1_ref,   "8 dependent add per iter (ALU latency)"),
  P(A_chain2x4,  A_chain2x4_ref, "2 chains x 4 dependent add"),
  P(A_indep8,    A_indep8_ref,   "8 independent add per iter (ALU width)"),
  P(A_indep16,   A_indep16_ref,  "16 independent add per iter"),
  P(A_mulchain,  A_mulchain_ref, "8 dependent mulld"),
  P(A_ldchain,   A_ldchain_ref,  "8 dependent L1-hit loads"),
  P(F_add_addi,  F_add_addi_ref, "16 dependent ops, no flags (control)"),
  P(F_addc_addze, F_addc_addze_ref, "16 dependent ops, carry via XER.CA"),
  P(F_cmpld_isel, F_cmpld_isel_ref, "carry as CR bit (cmpld) -> isel"),
  P(F_mfocrf_rt,  F_cmpld_isel_ref, "... plus mfocrf/mtocrf round trip"),
  P(F_mfcr_rt,    F_cmpld_isel_ref, "... plus mfcr/mtcrf 0xff round trip"),
  P(F_mcrxrx_p9,  F_cmpld_isel_ref, "carry via mcrxrx -> isel (POWER9)"),
  P(F_mfxer_chain, F_addc_addze_ref, "carry via mfxer/rlwinm/add"),
  P(F_mtxer_chain, F_addc_addze_ref, "li/mtxer before every addc"),
  P(F_srdi32,     F_srdi32_ref,   "32-bit lazy carry via srdi 32"),
  P(F_add_dot,    A_chain1_ref,   "8 dependent add. (CR0 written, unread)"),
  P(F_add_dot_isel, F_add_dot_isel_ref, "add. -> isel on CR0 every op"),
  P(S_reg_rt1,    S_n_ref,        "loop-carried add in a register"),
  P(S_ctx_rt1,    S_n_ref,        "loop-carried through one 8-byte slot (ld/addi/std)"),
  P(S_reg_rt4,    S_4n_ref,       "4 register accumulators"),
  P(S_ctx_rt4,    S_4n_ref,       "4 independent slots, ld/addi/std each"),
  P(S_ctx_crcshape, S_ctx_crcshape_ref, "crc32-like loop, 3 context slots"),
  P(S_reg_crcshape, S_ctx_crcshape_ref, "crc32-like loop, registers"),
  P(S_blocklocal, S_32n_ref,      "4 slots loaded once, 8 ops each, stored once"),
  P(S_ctx_rt4_x8, S_32n_ref,      "same work, round trip per op"),
  P(S_fwd_stw_ld, S_n_ref,        "4-byte store -> 8-byte load"),
  P(S_fwd_std_lwz, S_n_ref,       "8-byte store -> 4-byte load"),
  P(S_fwd_2stw_ld, S_n_ref,       "two 4-byte stores -> 8-byte load"),
  P(S_fwd_2stb_lhz, S_fwd_2stb_lhz_ref, "two 1-byte stores -> 2-byte load"),
  P(S_fwd_cross8, S_n_ref,        "8-byte slot at +4 (doubleword straddle)"),
  P(S_fwd_cross32, S_n_ref,       "8-byte slot at +28 (32-byte straddle)"),
  P(S_samebase_rt1, S_n_ref,      "std then ld, same base register"),
  P(S_alias_rt1,  S_n_ref,        "std then ld, aliased base register"),
  P(B_none,       S_n_ref,        "1 add per iter (branch control)"),
  P(B_bctr_next,  S_n_ref,        "mtctr/bctr to next block, predicted"),
  P(B_bctr2_sameblock, S_n_ref,   "two bctr in one 32-byte block"),
  P(B_bctr2_sepblock, S_n_ref,    "two bctr in separate 32-byte blocks"),
  P(B_bc8_packed, S_n_ref,        "8 not-taken bc in one 32-byte block"),
  P(B_bc8_spread, S_n_ref,        "8 not-taken bc, 3 ALU ops between"),
  P(B_b8_taken,   S_n_ref,        "8 taken b per iter, 16-byte blocks"),
  P(B_bl_blr,     S_n_ref,        "bl/blr, one site"),
  P(B_r2_bl_blr,  S_n_ref,        "2 random sites: bl / blr"),
  P(B_r2_bl_bctr, S_n_ref,        "2 random sites: bl / mtctr,bctr"),
  P(B_r2_bl_mtlr_blr, S_n_ref,    "2 random sites: bl / LR via memory, blr"),
  P(B_r2_bctrl_blr, S_n_ref,      "2 random sites: bctrl / blr"),
  P(B_r2_b_mtlr_blr, S_n_ref,     "2 random sites: b (no push) / mtlr,blr"),
  P(B_r2_b_mtctr_bctr, S_n_ref,   "2 random sites: b / mtctr,bctr"),
  P(B_depth8,     B_depth8_ref,   "recursion depth 8, bl/blr"),
  P(B_depth24,    B_depth24_ref,  "recursion depth 24"),
  P(B_depth48,    B_depth48_ref,  "recursion depth 48"),
  P(B_depth96,    B_depth96_ref,  "recursion depth 96"),
  P(D_bctr_rand4, D_bctr_rand4_ref, "shared bctr dispatch, 4 random opcodes"),
  P(D_bctr_rand16, D_bctr_rand16_ref, "shared bctr dispatch, 16 random opcodes"),
  P(D_bctr_rep8,  D_bctr_rep8_ref, "shared bctr dispatch, period-8 program"),
  P(D_bctr_rep64, D_bctr_rep64_ref, "shared bctr dispatch, period-64 program"),
  P(D_bctr_skew,  D_bctr_skew_ref, "shared bctr dispatch, 90% one opcode"),
  P(D_cmp_rand4,  D_cmp_rand4_ref, "compare chain, 4 random opcodes"),
  P(D_cmp_rand16, D_cmp_rand16_ref, "compare chain (4) + bctr, 16 random"),
  P(D_cmp_rep8,   D_cmp_rep8_ref,  "compare chain, period-8 program"),
  P(D_cmp_skew,   D_cmp_skew_ref,  "compare chain, 90% one opcode"),
  P(D_bctr_repl_rand4, D_bctr_repl_rand4_ref, "replicated tails, 4 random"),
  P(D_bctr_repl_rep8, D_bctr_repl_rep8_ref, "replicated tails, period-8"),
  P(D_bctr_repl_rep64, D_bctr_repl_rep64_ref, "replicated tails, period-64"),
  P(L_lwz_stw,    S_n_ref,        "lwz/addi/stw"),
  P(L_larx_stcx,  S_n_ref,        "lwarx/addi/stwcx. uncontended"),
  P(L_larx_stcx_acqrel, S_n_ref,  "lwsync; lwarx..stwcx.; isync"),
  P(L_lwz_stw_lwsync, S_n_ref,    "lwz/stw + lwsync"),
  P(L_lwz_stw_hwsync, S_n_ref,    "lwz/stw + sync"),
  P(T_mat5_x4,    T_x4_ref,       "5-insn constant x4 (throughput)"),
  P(T_ld_x4,      T_x4_ref,       "pool ld x4 (throughput)"),
  P(T_addpcis_ld_x4_p9, T_x4_ref, "addpcis+ld x4 (POWER9)"),
  P(T_mat5_dep,   T_mat5_dep_ref, "5-insn constant on the critical path"),
  P(T_ld_dep,     T_mat5_dep_ref, "pool ld on the critical path"),
  P(Q_loop_bne,   S_n_ref,        "1 add; addi/cmpdi/bne loop"),
  P(Q_loop_bdnz,  S_n_ref,        "1 add; bdnz loop"),
  P(Q_loop_cmpfar, A_indep8_ref,  "cmpdi early, 8 indep adds, bne"),
  P(Q_loop_bne_x4, S_4n_ref,      "4 blocks/iter, each exits by taken bne"),
  P(Q_loop_b_x4,  S_4n_ref,       "4 blocks/iter, each exits by b"),
  P(Q_loop_bctr_x4, S_4n_ref,     "4 blocks/iter, each exits by mtctr/bctr"),
  P(Q_loop_bback, S_n_ref,        "beq exit + unconditional b back-edge"),
  P(Q_fwd_far_x4, S_4n_ref,       "4 forward b over 256 B each"),
  P(Q_fwd_near_x4, S_4n_ref,      "4 forward b over 16 B each"),
  P(Q_loop_long,  Q_loop_long_ref, "64 independent adds per back-edge"),
};
#define NPROBES (sizeof(probes) / sizeof(probes[0]))

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s list | time <name> [iters] | once <name> [iters]\n", argv[0]); return 2; }
  setup();
  if (!strcmp(argv[1], "list")) {
    for (size_t i = 0; i < NPROBES; i++) printf("%-22s %s\n", probes[i].name, probes[i].desc);
    return 0;
  }
  if (argc < 3) { fprintf(stderr, "need a probe name\n"); return 2; }
  const probe_t *p = NULL;
  for (size_t i = 0; i < NPROBES; i++) if (!strcmp(probes[i].name, argv[2])) p = &probes[i];
  if (!p) { fprintf(stderr, "unknown probe %s\n", argv[2]); return 2; }
  uint64_t iters = argc > 3 ? strtoull(argv[3], NULL, 0) : (1ull << 22);
  /* parity: two sizes, one of them the timed size, before any timing */
  uint64_t small = 4097;
  uint64_t got = p->fn(small), want = p->ref(small);
  if (got != want) { printf("%s parity=FAIL n=%llu got=%llx want=%llx\n", p->name, (unsigned long long)small, (unsigned long long)got, (unsigned long long)want); return 1; }
  got = p->fn(iters); want = p->ref(iters);
  if (got != want) { printf("%s parity=FAIL n=%llu got=%llx want=%llx\n", p->name, (unsigned long long)iters, (unsigned long long)got, (unsigned long long)want); return 1; }
  if (!strcmp(argv[1], "once")) { printf("%s parity=ok iters=%llu\n", p->name, (unsigned long long)iters); return 0; }
  double best = 1e30;
  for (int r = 0; r < 5; r++) {
    double t0 = now_ns(); uint64_t v = p->fn(iters); double t = (now_ns() - t0) / iters;
    if (v != want) { printf("%s parity=FAIL (rep %d)\n", p->name, r); return 1; }
    if (t < best) best = t;
  }
  printf("%-22s parity=ok ns/iter=%.3f  %s\n", p->name, best, p->desc);
  return 0;
}
