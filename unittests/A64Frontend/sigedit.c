// SPDX-License-Identifier: MIT
//
// Signal handlers that edit the interrupted context. rt_sigreturn restores
// everything the handler leaves in its ucontext -- X0-X30, SP, PC, NZCV, and
// V0-V31, FPSR and FPCR from the fpsimd_context record in __reserved --
// whether or not the PC changed. Runtimes rely on the same-PC case: a handler
// that emulates or fixes up the faulting instruction and resumes at it, or
// that patches registers after a guard-page fault (JVMs, V8/JSC, Wine-style
// emulation, safepoints, libsigsegv).
//
// Each case loads every register from a pattern, runs one instruction or
// loop, and dumps every register as the code after it sees them:
//
//   segv     a load from a PROT_NONE page. First fault: the handler rewrites
//            every register (the load's base still bad) and returns to the
//            same PC; the load faults again and the second frame must hold
//            those values. Second fault: the handler rewrites everything
//            again, SP too, with the base now pointing at readable memory,
//            and again returns to the same PC; the load completes.
//   segvskip the same load; the handler rewrites everything and steps over it.
//   segvflags
//            as segv, between a compare and a CSEL on its flags, followed by
//            a second compare: the CSEL must see the flags the handler left.
//   udf, unalloc, brk
//            UDF, an unallocated encoding (op0 0b0001) and BRK: the first
//            SIGILL/SIGTRAP rewrites everything at the same PC, the
//            instruction traps again with the rewritten state, and the second
//            rewrites everything and steps over it.
//   svc      SIGUSR1 sent to the thread itself with a raw tgkill (what raise()
//            does), delivered on the way out of the syscall; the handler
//            rewrites everything, X0 included, which wins over the syscall's
//            return value.
//   spin     a `cbz` loop on a register nothing in it writes, interrupted by
//            a 1 ms SIGUSR1 timer; the handler sets that register, rewrites
//            the others, and returns to wherever the loop was.
//
// After resuming, the dump also computes 1.0 / 10.0, whose last bit depends
// on the rounding mode the handler put in FPCR (round toward zero).
//
// A handler that finds its edits ignored bails out by moving the PC (a
// handler-set PC is honoured even then), so a broken sigreturn fails the
// comparison instead of looping.
//
// Output: per case, the checks on each frame the handler saw and on the
// state after resuming. Exact whatever the signal timing.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

// struct _aarch64_ctx, struct fpsimd_context and FPSIMD_MAGIC come from the
// kernel's <asm/sigcontext.h>, through <signal.h>.

// The register image the asm below loads from and dumps to (offsets are
// hard-coded there).
struct image {
  uint64_t x[31]; // 0
  uint64_t sp;    // 248
  uint64_t nzcv;  // 256
  uint64_t fpcr;  // 264
  uint64_t fpsr;  // 272
  uint64_t fdiv;  // 280: 1.0 / 10.0, computed after the dump
  __uint128_t v[32]; // 288
};
_Static_assert(sizeof(struct image) == 800, "image layout");

const struct image* run_segv(const struct image* in, uint64_t stack_top);
const struct image* run_segvflags(const struct image* in, uint64_t stack_top);
const struct image* run_udf(const struct image* in, uint64_t stack_top);
const struct image* run_unalloc(const struct image* in, uint64_t stack_top);
const struct image* run_brk(const struct image* in, uint64_t stack_top);
const struct image* run_svc(const struct image* in, uint64_t stack_top);
const struct image* run_spin(const struct image* in, uint64_t stack_top);
void segv_insn(void);
void segvflags_insn(void);
void udf_insn(void);
void unalloc_insn(void);
void brk_insn(void);
void svc_ret(void);
void spin_top(void);
void spin_exit(void);

// ENTER saves the callee-saved state, switches to the test stack (x1) and
// loads every register from the image at x0, X0 last. DUMP stores every
// register as found to the image at SP, then does the division. LEAVE
// returns the dump's address and restores the caller's state.
__asm__(".macro ENTER\n"
        "  adrp x16, saved\n"
        "  add x16, x16, :lo12:saved\n"
        "  stp x19, x20, [x16, #0]\n"
        "  stp x21, x22, [x16, #16]\n"
        "  stp x23, x24, [x16, #32]\n"
        "  stp x25, x26, [x16, #48]\n"
        "  stp x27, x28, [x16, #64]\n"
        "  stp x29, x30, [x16, #80]\n"
        "  stp d8, d9, [x16, #96]\n"
        "  stp d10, d11, [x16, #112]\n"
        "  stp d12, d13, [x16, #128]\n"
        "  stp d14, d15, [x16, #144]\n"
        "  mov x17, sp\n"
        "  str x17, [x16, #160]\n"
        "  mov sp, x1\n"
        "  ldr x17, [x0, #264]\n"
        "  msr fpcr, x17\n"
        "  ldr x17, [x0, #272]\n"
        "  msr fpsr, x17\n"
        "  ldr x17, [x0, #256]\n"
        "  msr nzcv, x17\n"
        "  add x17, x0, #288\n"
        "  ld1 {v0.2d-v3.2d}, [x17], #64\n"
        "  ld1 {v4.2d-v7.2d}, [x17], #64\n"
        "  ld1 {v8.2d-v11.2d}, [x17], #64\n"
        "  ld1 {v12.2d-v15.2d}, [x17], #64\n"
        "  ld1 {v16.2d-v19.2d}, [x17], #64\n"
        "  ld1 {v20.2d-v23.2d}, [x17], #64\n"
        "  ld1 {v24.2d-v27.2d}, [x17], #64\n"
        "  ld1 {v28.2d-v31.2d}, [x17], #64\n"
        "  ldp x2, x3, [x0, #16]\n"
        "  ldp x4, x5, [x0, #32]\n"
        "  ldp x6, x7, [x0, #48]\n"
        "  ldp x8, x9, [x0, #64]\n"
        "  ldp x10, x11, [x0, #80]\n"
        "  ldp x12, x13, [x0, #96]\n"
        "  ldp x14, x15, [x0, #112]\n"
        "  ldp x16, x17, [x0, #128]\n"
        "  ldp x18, x19, [x0, #144]\n"
        "  ldp x20, x21, [x0, #160]\n"
        "  ldp x22, x23, [x0, #176]\n"
        "  ldp x24, x25, [x0, #192]\n"
        "  ldp x26, x27, [x0, #208]\n"
        "  ldp x28, x29, [x0, #224]\n"
        "  ldr x30, [x0, #240]\n"
        "  ldr x1, [x0, #8]\n"
        "  ldr x0, [x0, #0]\n"
        ".endm\n"
        ".macro DUMP\n"
        "  stp x0, x1, [sp, #0]\n"
        "  stp x2, x3, [sp, #16]\n"
        "  stp x4, x5, [sp, #32]\n"
        "  stp x6, x7, [sp, #48]\n"
        "  stp x8, x9, [sp, #64]\n"
        "  stp x10, x11, [sp, #80]\n"
        "  stp x12, x13, [sp, #96]\n"
        "  stp x14, x15, [sp, #112]\n"
        "  stp x16, x17, [sp, #128]\n"
        "  stp x18, x19, [sp, #144]\n"
        "  stp x20, x21, [sp, #160]\n"
        "  stp x22, x23, [sp, #176]\n"
        "  stp x24, x25, [sp, #192]\n"
        "  stp x26, x27, [sp, #208]\n"
        "  stp x28, x29, [sp, #224]\n"
        "  str x30, [sp, #240]\n"
        "  mov x0, sp\n"
        "  str x0, [sp, #248]\n"
        "  mrs x0, nzcv\n"
        "  str x0, [sp, #256]\n"
        "  mrs x0, fpcr\n"
        "  str x0, [sp, #264]\n"
        "  mrs x0, fpsr\n"
        "  str x0, [sp, #272]\n"
        "  add x1, sp, #288\n"
        "  st1 {v0.2d-v3.2d}, [x1], #64\n"
        "  st1 {v4.2d-v7.2d}, [x1], #64\n"
        "  st1 {v8.2d-v11.2d}, [x1], #64\n"
        "  st1 {v12.2d-v15.2d}, [x1], #64\n"
        "  st1 {v16.2d-v19.2d}, [x1], #64\n"
        "  st1 {v20.2d-v23.2d}, [x1], #64\n"
        "  st1 {v24.2d-v27.2d}, [x1], #64\n"
        "  st1 {v28.2d-v31.2d}, [x1], #64\n"
        "  fmov d0, #1.0\n"
        "  fmov d1, #10.0\n"
        "  fdiv d2, d0, d1\n"
        "  str d2, [sp, #280]\n"
        ".endm\n"
        ".macro LEAVE\n"
        "  mov x0, sp\n"
        "  msr fpcr, xzr\n"
        "  msr fpsr, xzr\n"
        "  adrp x16, saved\n"
        "  add x16, x16, :lo12:saved\n"
        "  ldp x19, x20, [x16, #0]\n"
        "  ldp x21, x22, [x16, #16]\n"
        "  ldp x23, x24, [x16, #32]\n"
        "  ldp x25, x26, [x16, #48]\n"
        "  ldp x27, x28, [x16, #64]\n"
        "  ldp x29, x30, [x16, #80]\n"
        "  ldp d8, d9, [x16, #96]\n"
        "  ldp d10, d11, [x16, #112]\n"
        "  ldp d12, d13, [x16, #128]\n"
        "  ldp d14, d15, [x16, #144]\n"
        "  ldr x17, [x16, #160]\n"
        "  mov sp, x17\n"
        "  ret\n"
        ".endm\n"

        ".bss\n"
        ".balign 16\n"
        "saved: .skip 176\n"
        ".text\n"

        ".globl run_segv\n"
        "run_segv:\n"
        "  ENTER\n"
        ".globl segv_insn\n"
        "segv_insn:\n"
        "  ldr x9, [x21]\n"
        "  DUMP\n"
        "  LEAVE\n"

        // The first compare sets Z and C (the image loads the same flags); the
        // second leaves the CSEL's flags dead.
        ".globl run_segvflags\n"
        "run_segvflags:\n"
        "  ENTER\n"
        "  cmp x1, x1\n"
        ".globl segvflags_insn\n"
        "segvflags_insn:\n"
        "  ldr x9, [x21]\n"
        "  csel x3, x4, x5, eq\n"
        "  cmp x6, x6\n"
        "  DUMP\n"
        "  LEAVE\n"

        ".globl run_udf\n"
        "run_udf:\n"
        "  ENTER\n"
        ".globl udf_insn\n"
        "udf_insn:\n"
        "  udf #0x5e\n"
        "  DUMP\n"
        "  LEAVE\n"

        ".globl run_unalloc\n"
        "run_unalloc:\n"
        "  ENTER\n"
        ".globl unalloc_insn\n"
        "unalloc_insn:\n"
        "  .inst 0x02000000\n"
        "  DUMP\n"
        "  LEAVE\n"

        ".globl run_brk\n"
        "run_brk:\n"
        "  ENTER\n"
        ".globl brk_insn\n"
        "brk_insn:\n"
        "  brk #0x77\n"
        "  DUMP\n"
        "  LEAVE\n"

        ".globl run_svc\n"
        "run_svc:\n"
        "  ENTER\n"
        "  svc #0\n"
        ".globl svc_ret\n"
        "svc_ret:\n"
        "  DUMP\n"
        "  LEAVE\n"

        ".globl run_spin\n"
        "run_spin:\n"
        "  ENTER\n"
        ".globl spin_top\n"
        "spin_top:\n"
        "  add x9, x9, #1\n"
        "  cbz x20, spin_top\n"
        ".globl spin_exit\n"
        "spin_exit:\n"
        "  DUMP\n"
        "  LEAVE\n");

// ---------------------------------------------------------------------------
// Patterns and comparison
// ---------------------------------------------------------------------------

enum { F_SP = 31, F_NZCV, F_FPCR, F_FPSR, F_FDIV, F_V0, F_FPSIMD = F_V0 + 32 };

struct check {
  int bad;
  int first;
  __uint128_t got;
  __uint128_t want;
};

static void note(struct check* c, int field, __uint128_t got, __uint128_t want) {
  if (got == want) {
    return;
  }
  if (c->bad++ == 0) {
    c->first = field;
    c->got = got;
    c->want = want;
  }
}

// Every field but those in skip (bit n: xn; the F_* bits above).
static void compare(struct check* c, const struct image* got, const struct image* want, __uint128_t skip) {
  for (int i = 0; i < 31; i++) {
    if (!((skip >> i) & 1)) {
      note(c, i, got->x[i], want->x[i]);
    }
  }
  if (!((skip >> F_SP) & 1)) {
    note(c, F_SP, got->sp, want->sp);
  }
  note(c, F_NZCV, got->nzcv, want->nzcv);
  note(c, F_FPCR, got->fpcr, want->fpcr);
  note(c, F_FPSR, got->fpsr, want->fpsr);
  if (!((skip >> F_FDIV) & 1)) {
    note(c, F_FDIV, got->fdiv, want->fdiv);
  }
  for (int i = 0; i < 32; i++) {
    note(c, F_V0 + i, got->v[i], want->v[i]);
  }
}

static void field_name(int f, char* buf, size_t n) {
  if (f < 31) {
    snprintf(buf, n, "x%d", f);
  } else if (f >= F_V0 && f < F_V0 + 32) {
    snprintf(buf, n, "v%d", f - F_V0);
  } else {
    static const char* const names[] = {"sp", "nzcv", "fpcr", "fpsr", "fdiv"};
    snprintf(buf, n, "%s", f == F_FPSIMD ? "fpsimd_context" : names[f - F_SP]);
  }
}

static void print_check(const char* what, const struct check* c) {
  if (c->bad == 0) {
    printf(" %s ok", what);
    return;
  }
  char name[32];
  field_name(c->first, name, sizeof(name));
  printf(" %s %d bad (first %s: got %016llx%016llx want %016llx%016llx)", what, c->bad, name, (unsigned long long)(c->got >> 64),
         (unsigned long long)c->got, (unsigned long long)(c->want >> 64), (unsigned long long)c->want);
}

static void pattern(struct image* p, uint64_t seed, uint64_t nzcv, uint64_t fpcr, uint64_t fpsr) {
  memset(p, 0, sizeof(*p));
  for (int i = 0; i < 31; i++) {
    p->x[i] = (seed << 56) ^ (0x0123456789abcdefull * (uint64_t)(i + 1)) ^ ((uint64_t)i << 40);
  }
  for (int i = 0; i < 32; i++) {
    const uint64_t hi = (seed * 0x9e3779b97f4a7c15ull) ^ ((uint64_t)i << 48) ^ 0x00ff00ff00ff00ffull;
    const uint64_t lo = 0xfedcba9876543210ull ^ (seed << 32) ^ ((uint64_t)i * 0x0101010101ull);
    p->v[i] = ((__uint128_t)hi << 64) | lo;
  }
  p->nzcv = nzcv;
  p->fpcr = fpcr;
  p->fpsr = fpsr;
}

// ---------------------------------------------------------------------------
// The frame the handler sees
// ---------------------------------------------------------------------------

static struct fpsimd_context* find_fpsimd(ucontext_t* uc) {
  uint8_t* p = (uint8_t*)uc->uc_mcontext.__reserved;
  uint8_t* end = p + sizeof(uc->uc_mcontext.__reserved);
  while (p + sizeof(struct _aarch64_ctx) <= end) {
    struct _aarch64_ctx* h = (struct _aarch64_ctx*)p;
    if (h->magic == 0 || h->size < sizeof(struct _aarch64_ctx) || p + h->size > end) {
      return NULL;
    }
    if (h->magic == FPSIMD_MAGIC) {
      return (struct fpsimd_context*)p;
    }
    p += h->size;
  }
  return NULL;
}

// Compares the frame with want; a missing fpsimd_context counts as one bad field.
static void check_frame(struct check* c, ucontext_t* uc, const struct image* want, __uint128_t skip) {
  struct image got;
  memset(&got, 0, sizeof(got));
  memcpy(got.x, uc->uc_mcontext.regs, sizeof(got.x));
  got.sp = uc->uc_mcontext.sp;
  got.nzcv = uc->uc_mcontext.pstate & 0xf0000000u;
  const struct fpsimd_context* fp = find_fpsimd(uc);
  if (!fp) {
    note(c, F_FPSIMD, 0, FPSIMD_MAGIC);
    got.fpcr = want->fpcr;
    got.fpsr = want->fpsr;
    memcpy(got.v, want->v, sizeof(got.v));
  } else {
    got.fpcr = fp->fpcr;
    got.fpsr = fp->fpsr;
    memcpy(got.v, fp->vregs, sizeof(got.v));
  }
  got.fdiv = want->fdiv;
  compare(c, &got, want, skip);
}

// Writes the image into the frame. SP only when set (nonzero); X9 stays when keep_x9.
static void edit_frame(ucontext_t* uc, const struct image* img, int keep_x9) {
  const uint64_t x9 = uc->uc_mcontext.regs[9];
  memcpy(uc->uc_mcontext.regs, img->x, sizeof(img->x));
  if (keep_x9) {
    uc->uc_mcontext.regs[9] = x9;
  }
  if (img->sp) {
    uc->uc_mcontext.sp = img->sp;
  }
  uc->uc_mcontext.pstate = (uc->uc_mcontext.pstate & ~(uint64_t)0xf0000000u) | img->nzcv;
  struct fpsimd_context* fp = find_fpsimd(uc);
  if (fp) {
    fp->fpcr = (uint32_t)img->fpcr;
    fp->fpsr = (uint32_t)img->fpsr;
    memcpy(fp->vregs, img->v, sizeof(fp->vregs));
  }
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

enum Mode { M_NONE, M_SAMEPC, M_SKIP, M_SVC, M_SPIN };

#define MAX_FRAMES 2
#define BAIL_FAULTS 6
#define BAIL_SPIN 50

static volatile enum Mode mode;
static uint64_t insn_pc;
static const struct image* frame_want[MAX_FRAMES];
static const struct image* frame_edit[MAX_FRAMES];
static __uint128_t frame_skip;
static volatile int deliveries;
static volatile int stray;
static volatile int bailed;
static struct check frame_check[MAX_FRAMES];
static volatile uint64_t guard;

static void on_fault(int sig, siginfo_t* si, void* uc_) {
  ucontext_t* uc = uc_;
  if (mode != M_SAMEPC && mode != M_SKIP) {
    signal(sig, SIG_DFL);
    return;
  }
  if (sig == SIGSEGV && (uint64_t)si->si_addr != guard) {
    signal(sig, SIG_DFL);
    return;
  }
  const int n = deliveries++;
  if (uc->uc_mcontext.pc != insn_pc || n >= BAIL_FAULTS) {
    // Wrong place, or the edits are not taking: step over it and report.
    bailed = 1;
    edit_frame(uc, frame_edit[MAX_FRAMES - 1], 0);
    uc->uc_mcontext.pc = insn_pc + 4;
    return;
  }
  if (n < MAX_FRAMES) {
    check_frame(&frame_check[n], uc, frame_want[n], frame_skip);
  }
  // The second frame (the first under M_SKIP) is the last: it steps over the
  // instruction, except for the load, which it fixes up.
  const int last = mode == M_SKIP || n >= 1;
  edit_frame(uc, frame_edit[last ? MAX_FRAMES - 1 : 0], 0);
  if (last && sig != SIGSEGV) {
    uc->uc_mcontext.pc += 4;
  }
  if (last && sig == SIGSEGV && mode == M_SKIP) {
    uc->uc_mcontext.pc += 4;
  }
}

static void on_usr1(int sig, siginfo_t* si, void* uc_) {
  (void)sig;
  (void)si;
  ucontext_t* uc = uc_;
  if (mode == M_SVC) {
    const int n = deliveries++;
    if (uc->uc_mcontext.pc != (uint64_t)svc_ret) {
      bailed = 1;
      return;
    }
    if (n == 0) {
      check_frame(&frame_check[0], uc, frame_want[0], frame_skip);
      edit_frame(uc, frame_edit[MAX_FRAMES - 1], 0);
    }
    return;
  }
  if (mode == M_SPIN) {
    const uint64_t pc = uc->uc_mcontext.pc;
    if (pc < (uint64_t)spin_top || pc >= (uint64_t)spin_exit) {
      stray++;
      return;
    }
    const int n = deliveries++;
    if (n == 0) {
      check_frame(&frame_check[0], uc, frame_want[0], frame_skip);
    }
    edit_frame(uc, frame_edit[MAX_FRAMES - 1], 1);
    if (n >= BAIL_SPIN) {
      bailed = 1;
      uc->uc_mcontext.pc = (uint64_t)spin_exit;
    }
  }
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

static uint8_t test_stack[64 * 1024] __attribute__((aligned(16)));
static const uint64_t readable_word = 0x1122334455667788ull;

// The dump lands just under the top of the test stack; the signal frames go below.
static uint64_t stack_top(void) {
  return (uint64_t)test_stack + sizeof(test_stack) - 1024;
}

static struct image pat_init, pat_a, pat_b, want_dump;

static void setup_patterns(void) {
  pattern(&pat_init, 0x11, 0x20000000, 0, 0);
  pattern(&pat_a, 0x22, 0x60000000, 0x00480000, 0x08000001);
  pattern(&pat_b, 0x33, 0x90000000, 0x02c00000, 0x00000090);
  pat_a.sp = 0;
  pat_b.sp = stack_top() - 64;
}

// The state after resuming: pat_b, plus the division rounded toward zero.
static void expect_dump(const struct image* b) {
  want_dump = *b;
  const double tenth_rz = 0x1.9999999999999p-4;
  memcpy(&want_dump.fdiv, &tenth_rz, sizeof(tenth_rz));
}

static void report(const char* name, int frames, const struct image* dump, __uint128_t dump_skip) {
  struct check after = {0};
  compare(&after, dump, &want_dump, dump_skip);
  printf("%-9s deliveries %d%s", name, deliveries, bailed ? " BAILED" : "");
  static const char* const labels[MAX_FRAMES] = {"frame1", "frame2"};
  for (int i = 0; i < frames; i++) {
    print_check(labels[i], &frame_check[i]);
  }
  print_check("resumed", &after);
  printf("\n");
}

static void reset(void) {
  deliveries = 0;
  stray = 0;
  bailed = 0;
  memset(frame_check, 0, sizeof(frame_check));
}

// segv/udf/unalloc/brk: one instruction that faults twice (M_SAMEPC) or once (M_SKIP).
static void run_fault(const char* name, const struct image* (*run)(const struct image*, uint64_t), void (*insn)(void), enum Mode m) {
  reset();
  struct image in = pat_init;
  struct image a = pat_a;
  struct image b = pat_b;
  in.x[21] = guard; // the load's base: faults
  if (run == run_segvflags) {
    in.nzcv = 0x60000000; // what the compare before the load sets
  }
  a.x[21] = guard;  // still faults
  b.x[21] = m == M_SAMEPC ? (uint64_t)&readable_word : guard;
  in.sp = stack_top();
  frame_want[0] = &in;
  frame_want[1] = &a;
  frame_edit[0] = &a;
  frame_edit[1] = &b;
  frame_skip = 0;
  insn_pc = (uint64_t)insn;
  expect_dump(&b);
  // The load completed with the fixed-up base.
  if (m == M_SAMEPC && (run == run_segv || run == run_segvflags)) {
    want_dump.x[9] = readable_word;
  }
  // b's flags have Z clear; the compare after the CSEL sets Z and C again.
  if (run == run_segvflags) {
    want_dump.x[3] = b.x[5];
    want_dump.nzcv = 0x60000000;
  }
  // a's SP is unchanged.
  a.sp = in.sp;
  mode = m;
  const struct image* dump = run(&in, stack_top());
  mode = M_NONE;
  report(name, m == M_SAMEPC ? 2 : 1, dump, 0);
}

static void run_svc_case(void) {
  reset();
  struct image in = pat_init;
  in.x[0] = (uint64_t)getpid();
  in.x[1] = (uint64_t)gettid();
  in.x[2] = SIGUSR1;
  in.x[8] = SYS_tgkill;
  in.sp = stack_top();
  struct image b = pat_b;
  frame_want[0] = &in;
  frame_edit[1] = &b;
  // X0 holds the syscall's result when the handler runs.
  frame_skip = 1;
  expect_dump(&b);
  mode = M_SVC;
  const struct image* dump = run_svc(&in, stack_top());
  mode = M_NONE;
  report("svc", 1, dump, 0);
}

static void run_spin_case(void) {
  reset();
  struct image in = pat_init;
  in.x[9] = 0;  // iterations
  in.x[20] = 0; // the loop runs while this is zero
  in.sp = stack_top();
  struct image b = pat_b;
  b.x[20] = 1;
  frame_want[0] = &in;
  frame_edit[1] = &b;
  frame_skip = (__uint128_t)1 << 9;
  expect_dump(&b);

  struct sigevent sev;
  memset(&sev, 0, sizeof(sev));
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGUSR1;
  timer_t t;
  if (timer_create(CLOCK_MONOTONIC, &sev, &t) != 0) {
    printf("%-9s timer_create failed\n", "spin");
    return;
  }
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  its.it_value.tv_nsec = 1000000;
  its.it_interval.tv_nsec = 1000000;
  mode = M_SPIN;
  timer_settime(t, 0, &its, NULL);
  const struct image* dump = run_spin(&in, stack_top());
  mode = M_NONE;
  timer_delete(t);
  const uint64_t iterations = dump->x[9];
  report("spin", 1, dump, (__uint128_t)1 << 9);
  printf("%-9s looped %s\n", "spin", iterations ? "yes" : "no");
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  void* page = mmap(NULL, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) {
    printf("mmap failed\n");
    return 1;
  }
  guard = (uint64_t)page;
  setup_patterns();

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGTRAP, &sa, NULL);
  sa.sa_sigaction = on_usr1;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGUSR1, &sa, NULL);

  run_fault("segv", run_segv, segv_insn, M_SAMEPC);
  run_fault("segvskip", run_segv, segv_insn, M_SKIP);
  run_fault("segvflags", run_segvflags, segvflags_insn, M_SAMEPC);
  run_fault("udf", run_udf, udf_insn, M_SAMEPC);
  run_fault("unalloc", run_unalloc, unalloc_insn, M_SAMEPC);
  run_fault("brk", run_brk, brk_insn, M_SAMEPC);
  run_svc_case();
  run_spin_case();
  return 0;
}
