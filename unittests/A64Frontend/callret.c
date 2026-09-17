// SPDX-License-Identifier: MIT
//
// Guest call/return shapes that break a naive pairing of guest BL/BLR with
// guest RET. The backend's shadow return stack and the host link stack may
// only decide how fast a RET reaches its target, never where it goes.
//
// Every section prints a checksum; the Pi golden is the reference.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

static uint64_t mix(uint64_t h, uint64_t v) {
  return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

// ---- 1. recursion deeper than the link stack (64) and the shadow stack ----
__attribute__((noinline)) static uint64_t rec(uint64_t n) {
  if (n == 0) {
    return 1;
  }
  uint64_t r = rec(n - 1);
  return mix(r, n);
}

// ---- 2. setjmp/longjmp out of deep recursion ----
static jmp_buf jb;
__attribute__((noinline)) static uint64_t dive(uint64_t n, uint64_t stop) {
  if (n == stop) {
    longjmp(jb, (int)(n & 0x7fff) + 1);
  }
  uint64_t r = dive(n + 1, stop);
  return mix(r, n);
}

// ---- 3. returns that do not go to the caller (assembly) ----
//  call_ret_elsewhere(x): calls ret_elsewhere, which sets x30 to `landing`
//    and RETs there; landing adds x0 to landing_hits and branches to the
//    continuation call_ret_elsewhere left in x19. Result x + 7.
//  call_via_x1(x): calls ret_via_x1, which returns with `ret x1` to a label
//    after the call. Result x + 3.
//  br_x30_return(x): returns with `br x30`. Result x + 11.
//  mid(x): calls skip_frame, which pops mid's frame and returns straight to
//    mid's caller. Result x + 13.
//  ret_to_self_loop(n): a RET whose target is the RET's own block entry
//    region: a loop driven by `ret` with x30 set by `adr`. Result n * 2.
uint64_t landing_hits;
uint64_t call_ret_elsewhere(uint64_t x);
uint64_t call_via_x1(uint64_t x);
uint64_t br_x30_return(uint64_t x);
uint64_t mid(uint64_t x);
uint64_t ret_to_self_loop(uint64_t n);
__asm__(".text\n"
        "ret_elsewhere:\n"
        "  add x0, x0, #7\n"
        "  adrp x30, landing\n"
        "  add x30, x30, :lo12:landing\n"
        "  ret\n"
        "landing:\n"
        "  adrp x9, landing_hits\n"
        "  ldr x10, [x9, :lo12:landing_hits]\n"
        "  add x10, x10, x0\n"
        "  str x10, [x9, :lo12:landing_hits]\n"
        "  br x19\n"
        ".globl call_ret_elsewhere\n"
        "call_ret_elsewhere:\n"
        "  stp x29, x30, [sp, #-32]!\n"
        "  str x19, [sp, #16]\n"
        "  adr x19, 1f\n"
        "  bl ret_elsewhere\n"
        "  mov x0, #999\n"
        "1:\n"
        "  ldr x19, [sp, #16]\n"
        "  ldp x29, x30, [sp], #32\n"
        "  ret\n"
        "ret_via_x1:\n"
        "  add x0, x0, #3\n"
        "  ret x1\n"
        ".globl call_via_x1\n"
        "call_via_x1:\n"
        "  stp x29, x30, [sp, #-16]!\n"
        "  adr x1, 1f\n"
        "  bl ret_via_x1\n"
        "  mov x0, #777\n"
        "1:\n"
        "  ldp x29, x30, [sp], #16\n"
        "  ret\n"
        ".globl br_x30_return\n"
        "br_x30_return:\n"
        "  add x0, x0, #11\n"
        "  br x30\n"
        "skip_frame:\n"
        "  add x0, x0, #13\n"
        "  ldp x29, x30, [sp], #16\n"
        "  ret\n"
        ".globl mid\n"
        "mid:\n"
        "  stp x29, x30, [sp, #-16]!\n"
        "  mov x29, sp\n"
        "  bl skip_frame\n"
        "  mov x0, #555\n"
        "  ldp x29, x30, [sp], #16\n"
        "  ret\n"
        ".globl ret_to_self_loop\n"
        "ret_to_self_loop:\n"
        "  mov x2, x30\n"
        "  mov x1, #0\n"
        "2:\n"
        "  cbz x0, 3f\n"
        "  sub x0, x0, #1\n"
        "  add x1, x1, #2\n"
        "  adr x30, 2b\n"
        "  ret\n"
        "3:\n"
        "  mov x0, x1\n"
        "  ret x2\n");

// ---- 4. tail calls and polymorphic indirect calls ----
typedef uint64_t (*fn_t)(uint64_t);
__attribute__((noinline)) static uint64_t leaf_a(uint64_t x) { return x * 3 + 1; }
__attribute__((noinline)) static uint64_t leaf_b(uint64_t x) { return x ^ 0x5555; }
__attribute__((noinline)) static uint64_t leaf_c(uint64_t x) { return x + (x >> 3); }
static fn_t fns[3] = {leaf_a, leaf_b, leaf_c};
__attribute__((noinline)) static uint64_t tail(uint64_t x) {
  return fns[x % 3](x); // a sibling call at -O2: the leaf returns to our caller
}
__attribute__((noinline)) static uint64_t tail_rec(uint64_t x, uint64_t n) {
  if (n == 0) {
    return tail(x);
  }
  return tail_rec(mix(x, n), n - 1); // tail-recursive: no frames
}

// ---- 5. signals: a handler entered mid-recursion that makes calls ----
// A handler is entered without a guest call and returns through the signal
// trampoline, so its RET never matches the shadow stack. (No siglongjmp out of
// the handler: abandoning a handler frame is a separate, pre-existing
// signal-delegator bug that crashes or hangs POWERarm intermittently.)
static uint64_t handler_sum;
static void handler(int sig) {
  handler_sum = mix(handler_sum, rec(100) + (uint64_t)sig);
}
__attribute__((noinline)) static uint64_t dive_and_signal(uint64_t n) {
  if (n == 0) {
    kill(getpid(), SIGUSR1);
    return 17;
  }
  uint64_t r = dive_and_signal(n - 1);
  return mix(r, n);
}

// ---- 6. stack switching with ucontext ----
static ucontext_t main_ctx, co_ctx;
static uint64_t co_sum;
static char co_stack[1 << 16];
static void coroutine(void) {
  for (uint64_t i = 0;; i++) {
    co_sum = mix(co_sum, rec(i % 90));
    swapcontext(&co_ctx, &main_ctx);
  }
}

// ---- 7. self-modifying caller: the return continuation's code changes ----
__attribute__((noinline)) static uint64_t smc_callee(uint64_t x) {
  return rec(x % 30) ^ x;
}

int main(void) {
  uint64_t h;

  h = 0;
  const uint64_t depths[] = {1, 63, 64, 65, 200, 5000, 70000, 120000};
  for (unsigned i = 0; i < sizeof(depths) / sizeof(depths[0]); i++) {
    for (int k = 0; k < 3; k++) {
      h = mix(h, rec(depths[i]));
    }
  }
  printf("recursion %016llx\n", (unsigned long long)h);
  fflush(stdout);

  h = 0;
  for (uint64_t i = 0; i < 2000; i++) {
    const uint64_t stop = (i * 7919) % 300;
    int v = setjmp(jb);
    if (v == 0) {
      h = mix(h, dive(0, stop));
    } else {
      h = mix(h, (uint64_t)v);
      h = mix(h, rec(i % 80)); // calls and returns right after the jump
    }
  }
  printf("longjmp %016llx\n", (unsigned long long)h);
  fflush(stdout);

  h = 0;
  for (uint64_t i = 0; i < 5000; i++) {
    h = mix(h, call_ret_elsewhere(i));
    h = mix(h, call_via_x1(i));
    h = mix(h, br_x30_return(i));
    h = mix(h, mid(i));
    h = mix(h, ret_to_self_loop(i % 100));
    h = mix(h, rec(i % 70));
  }
  // 300000 unpaired RETs in a row: more pops than the shadow stack holds
  // above its starting point, so the RET fast path walks off its top.
  h = mix(h, ret_to_self_loop(300000));
  printf("mismatch %016llx %llu\n", (unsigned long long)h, (unsigned long long)landing_hits);
  fflush(stdout);

  h = 0;
  for (uint64_t i = 0; i < 100000; i++) {
    h = mix(h, tail(i));
    h = mix(h, fns[(i * 5) % 3](i));
    if (i % 100 == 0) {
      h = mix(h, tail_rec(i, i % 200));
    }
  }
  printf("tailcalls %016llx\n", (unsigned long long)h);
  fflush(stdout);

  struct sigaction sa = {0};
  sa.sa_handler = handler;
  sigaction(SIGUSR1, &sa, NULL);
  h = 0;
  for (uint64_t i = 0; i < 1000; i++) {
    h = mix(h, dive_and_signal(i % 120));
    h = mix(h, rec(i % 40));
  }
  printf("signals %016llx %016llx\n", (unsigned long long)h, (unsigned long long)handler_sum);
  fflush(stdout);

  h = 0;
  getcontext(&co_ctx);
  co_ctx.uc_stack.ss_sp = co_stack;
  co_ctx.uc_stack.ss_size = sizeof(co_stack);
  co_ctx.uc_link = NULL;
  makecontext(&co_ctx, coroutine, 0);
  for (uint64_t i = 0; i < 20000; i++) {
    swapcontext(&main_ctx, &co_ctx);
    h = mix(h, rec(i % 75) ^ co_sum);
  }
  printf("ucontext %016llx\n", (unsigned long long)h);
  fflush(stdout);

  // stp x29,x30,[sp,#-16]! ; blr x1 ; add x0,x0,#K ; ldp x29,x30,[sp],#16 ; ret
  uint32_t* code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  h = 0;
  for (uint64_t i = 0; i < 3000; i++) {
    const uint32_t k = (uint32_t)(i / 7) & 0xfff;
    code[0] = 0xa9bf7bfd;
    code[1] = 0xd63f0020;
    code[2] = 0x91000000 | (k << 10);
    code[3] = 0xa8c17bfd;
    code[4] = 0xd65f03c0;
    __builtin___clear_cache((char*)code, (char*)code + 20);
    uint64_t (*fn)(uint64_t, uint64_t (*)(uint64_t)) = (void*)code;
    h = mix(h, fn(i, smc_callee));
  }
  printf("smc %016llx\n", (unsigned long long)h);
  fflush(stdout);
  return 0;
}
