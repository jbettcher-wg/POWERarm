// SPDX-License-Identifier: MIT
//
// A load with the REV of its result next to it, and a REV with the store of
// its result next to it: the pairs TryFuseMemRev routes onto POWER's
// byte-reversed memory forms (lhbrx/lwbrx/ldbrx, sthbrx/stwbrx/stdbrx).
//
// Self-checking, so it needs no reference machine: the architectural answer
// is "the bytes of the field in the opposite order", which this file
// assembles byte by byte itself. The comparator is checked too -- see
// `control-detects-corruption` at the end.
//
// What each group pins down:
//
//   * Every width (16/32/64) at every alignment 0-15. lhbrx/lwbrx/ldbrx are
//     X-form loads with no alignment requirement, and the unfused lowerings
//     they replace are a mask chain (rev16: 5 instructions), an rlwinm pair
//     (rev w) and a red-zone stdbrx+ld (rev x) -- three different shapes, so
//     all three widths run.
//   * The 64-bit answer is the whole X register: a 16-bit fused load must
//     leave bits 63:16 ZERO, because the guest's `rev16 w` writes a W
//     register. lhbrx zero-extends, so the fused form gets this for free and
//     the test would catch it if it did not.
//   * Every addressing form the fusion accepts, because the byte-reversed
//     instructions are X-form only (base + index, no displacement): the
//     unsigned offset, the unscaled negative offset, pre- and post-index
//     (whose writeback is checked), and the 64-bit register index. Rt == Rn
//     is here because the address must be read before the result is written.
//   * The forms the fusion REFUSES, which must still be right: a scaled or
//     UXTW register index, a sign-extending load, a 64-bit REV over a word
//     load, a REV whose destination is not the loaded register, and a loaded
//     value that is read again afterwards. Each is arranged so that fusing it
//     anyway would give a different answer, not the same one by luck.
//   * The store direction keeps the REV: the reversed value is architectural
//     state in Rd, so every store case checks the register as well as the
//     bytes. `rev x0,x1 ; str x0,[x0]` stores through the reversed value,
//     which pins the order -- the address must be read AFTER the reverse.
//   * The fault boundary: a doubleword flush against an unmapped page must
//     load (ldbrx reads exactly 8 bytes, like ld), and one that straddles the
//     boundary must raise SIGSEGV with the faulting PC at the load and Rt
//     untouched -- the state a resumed fault has to see, fused or not.

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

static int Failures = 0;
static int Checks = 0;

static void expect_u64(const char* what, uint64_t got, uint64_t want) {
  ++Checks;
  if (got == want) {
    return;
  }
  ++Failures;
  printf("FAIL %s: got %016llx want %016llx\n", what, (unsigned long long)got, (unsigned long long)want);
}

static void expect_bytes(const char* what, const uint8_t* got, const uint8_t* want, int n) {
  ++Checks;
  if (memcmp(got, want, (size_t)n) == 0) {
    return;
  }
  ++Failures;
  printf("FAIL %s\n  got ", what);
  for (int i = 0; i < n; ++i) {
    printf("%02x", got[i]);
  }
  printf("\n  want ");
  for (int i = 0; i < n; ++i) {
    printf("%02x", want[i]);
  }
  printf("\n");
}

// The architecturally forced answer, built from the bytes in memory: a load
// of Width bytes followed by a byte reverse is the big-endian reading of
// those bytes, zero-extended to 64 bits.
static uint64_t be_value(const uint8_t* p, int Width) {
  uint64_t v = 0;
  for (int i = 0; i < Width; ++i) {
    v = (v << 8) | p[i];
  }
  return v;
}

// ---------------------------------------------------------------------------
// The pairs. Each asm block holds both instructions, so they are adjacent in
// one decoded block, which is what the fusion requires.
// ---------------------------------------------------------------------------

static uint64_t ld16_rev16(const void* p) {
  uint64_t v;
  asm volatile("ldrh %w0, [%1]\n\trev16 %w0, %w0" : "=&r"(v) : "r"(p) : "memory");
  return v;
}

static uint64_t ld16_rev16_x(const void* p) {
  // The same halfword through the 64-bit `rev16`: over a zero-extended
  // halfword only halfword 0 has content, so this is bswap16 as well.
  uint64_t v;
  asm volatile("ldrh %w0, [%1]\n\trev16 %0, %0" : "=&r"(v) : "r"(p) : "memory");
  return v;
}

static uint64_t ld32_rev(const void* p) {
  uint64_t v;
  asm volatile("ldr %w0, [%1]\n\trev %w0, %w0" : "=&r"(v) : "r"(p) : "memory");
  return v;
}

static uint64_t ld64_rev(const void* p) {
  uint64_t v;
  asm volatile("ldr %0, [%1]\n\trev %0, %0" : "=&r"(v) : "r"(p) : "memory");
  return v;
}

static uint64_t ld64_uimm_rev(const void* p) {
  // Unsigned scaled offset: the displacement has to be folded into the
  // address register, since ldbrx has no displacement field.
  uint64_t v;
  asm volatile("ldr %0, [%1, #16]\n\trev %0, %0" : "=&r"(v) : "r"(p) : "memory");
  return v;
}

static uint64_t ld32_unscaled_rev(const void* p) {
  // LDUR with a negative unscaled offset.
  uint64_t v;
  asm volatile("ldur %w0, [%1, #-3]\n\trev %w0, %w0" : "=&r"(v) : "r"(p) : "memory");
  return v;
}

static uint64_t ld16_uimm_rev16(const void* p) {
  uint64_t v;
  asm volatile("ldrh %w0, [%1, #6]\n\trev16 %w0, %w0" : "=&r"(v) : "r"(p) : "memory");
  return v;
}

static uint64_t ld64_post_rev(const void* p, uint64_t* NewBase) {
  uint64_t v;
  uint64_t b = (uint64_t)p;
  asm volatile("ldr %0, [%1], #24\n\trev %0, %0" : "=&r"(v), "+r"(b) : : "memory");
  *NewBase = b;
  return v;
}

static uint64_t ld64_pre_rev(const void* p, uint64_t* NewBase) {
  uint64_t v;
  uint64_t b = (uint64_t)p;
  asm volatile("ldr %0, [%1, #8]!\n\trev %0, %0" : "=&r"(v), "+r"(b) : : "memory");
  *NewBase = b;
  return v;
}

static uint64_t ld64_regoff_rev(const void* p, uint64_t Index) {
  uint64_t v;
  asm volatile("ldr %0, [%1, %2]\n\trev %0, %0" : "=&r"(v) : "r"(p), "r"(Index) : "memory");
  return v;
}

static uint64_t ld64_rtrn_rev(const void* p) {
  // Rt == Rn: the address must be consumed before the result is written.
  uint64_t v = (uint64_t)p;
  asm volatile("ldr %0, [%0]\n\trev %0, %0" : "+r"(v) : : "memory");
  return v;
}

// --- forms the fusion must refuse, and still answer correctly --------------

static uint64_t ld64_regoff_scaled_rev(const void* p, uint64_t Index) {
  uint64_t v;
  asm volatile("ldr %0, [%1, %2, lsl #3]\n\trev %0, %0" : "=&r"(v) : "r"(p), "r"(Index) : "memory");
  return v;
}

static uint64_t ld64_regoff_uxtw_rev(const void* p, uint32_t Index) {
  uint64_t v;
  asm volatile("ldr %0, [%1, %w2, uxtw]\n\trev %0, %0" : "=&r"(v) : "r"(p), "r"(Index) : "memory");
  return v;
}

static uint64_t ld32_sext_rev(const void* p) {
  // LDRSW then a 64-bit REV: the sign extension happens first, so the answer
  // is the byte reverse of the SIGN-EXTENDED word, not lwbrx.
  uint64_t v;
  asm volatile("ldrsw %0, [%1]\n\trev %0, %0" : "=&r"(v) : "r"(p) : "memory");
  return v;
}

static uint64_t ld32_rev64(const void* p) {
  // A 64-bit REV over a zero-extended word: the swapped word lands in the
  // UPPER half. Fusing this to lwbrx would be wrong.
  uint64_t v;
  asm volatile("ldr %w0, [%1]\n\trev %0, %0" : "=&r"(v) : "r"(p) : "memory");
  return v;
}

static uint64_t ld64_rev_distinct(const void* p, uint64_t* Loaded) {
  // The REV writes a different register, so the loaded value stays live.
  uint64_t v, l;
  asm volatile("ldr %1, [%2]\n\trev %0, %1" : "=&r"(v), "=&r"(l) : "r"(p) : "memory");
  *Loaded = l;
  return v;
}

static uint64_t ld64_rev_reused(const void* p, uint64_t* Sum) {
  // The loaded register is read again after the REV, so its unreversed value
  // is not dead. (The REV here writes a third register.)
  uint64_t v, l;
  asm volatile("ldr %1, [%2]\n\trev %0, %1\n\tadd %1, %1, %0" : "=&r"(v), "=&r"(l) : "r"(p) : "memory");
  *Sum = l;
  return v;
}

// --- store direction -------------------------------------------------------

static uint64_t rev_st64(void* p, uint64_t v) {
  asm volatile("rev %0, %0\n\tstr %0, [%1]" : "+r"(v) : "r"(p) : "memory");
  return v;
}

static uint64_t rev_st32(void* p, uint64_t v) {
  asm volatile("rev %w0, %w0\n\tstr %w0, [%1]" : "+r"(v) : "r"(p) : "memory");
  return v;
}

static uint64_t rev16_st16(void* p, uint64_t v) {
  asm volatile("rev16 %w0, %w0\n\tstrh %w0, [%1]" : "+r"(v) : "r"(p) : "memory");
  return v;
}

static uint64_t rev_st64_uimm(void* p, uint64_t v) {
  asm volatile("rev %0, %0\n\tstr %0, [%1, #16]" : "+r"(v) : "r"(p) : "memory");
  return v;
}

static uint64_t rev_st64_post(void* p, uint64_t v, uint64_t* NewBase) {
  uint64_t b = (uint64_t)p;
  asm volatile("rev %0, %0\n\tstr %0, [%1], #24" : "+r"(v), "+r"(b) : : "memory");
  *NewBase = b;
  return v;
}

static uint64_t rev_st64_distinct(void* p, uint64_t v) {
  uint64_t d;
  asm volatile("rev %0, %1\n\tstr %0, [%2]" : "=&r"(d) : "r"(v), "r"(p) : "memory");
  return d;
}

static uint64_t rev_st64_selfbase(uint64_t SwappedAddr) {
  // `rev x0,x1 ; str x0,[x0]`: the store's base is the REVERSED value, so the
  // address must be read after the reverse. Called with the byte-reversed
  // address of the slot, so the reverse produces the slot's address and the
  // store writes that address into the slot.
  uint64_t d;
  asm volatile("rev %0, %1\n\tstr %0, [%0]" : "=&r"(d) : "r"(SwappedAddr) : "memory");
  return d;
}

// ---------------------------------------------------------------------------
// The fault boundary
// ---------------------------------------------------------------------------

static sigjmp_buf FaultJump;
static volatile sig_atomic_t SawFault;
static volatile uint64_t FaultPC;
static volatile uint64_t FaultRt;

#define FAULT_SENTINEL 0x5A5A1234DEADBEEFULL

static void segv_handler(int Sig, siginfo_t* Info, void* UC) {
  (void)Sig;
  (void)Info;
  ucontext_t* U = (ucontext_t*)UC;
  FaultPC = U->uc_mcontext.pc;
  // x9 is Rt in the faulting pair below.
  FaultRt = U->uc_mcontext.regs[9];
  SawFault = 1;
  siglongjmp(FaultJump, 1);
}

// Loads the doubleword at p into x9 and reverses it. The address of the load
// itself is STORED THROUGH LoadPC before the load runs: when the load faults
// the handler leaves through siglongjmp, so anything written after the asm
// block would never land. x9 is the pair's Rt, preloaded with a sentinel the
// handler reads back out of the signal frame.
static uint64_t ld64_rev_at(const void* p, volatile uint64_t* LoadPC) {
  uint64_t pc;
  uint64_t v;
  asm volatile("adr %0, 1f\n\t"
               "str %0, [%3]\n\t"
               "mov x9, %4\n\t"
               "1: ldr x9, [%2]\n\t"
               "rev x9, x9\n\t"
               "mov %1, x9"
               : "=&r"(pc), "=r"(v)
               : "r"(p), "r"(LoadPC), "r"((uint64_t)FAULT_SENTINEL)
               : "x9", "memory");
  return v;
}

int main(void) {
  // A pattern with no symmetry: byte i is distinguishable from byte j, so a
  // reversal that is off by one, or a load of the wrong width, shows up.
  uint8_t Buf[96];
  for (unsigned i = 0; i < sizeof(Buf); ++i) {
    Buf[i] = (uint8_t)(0x11 * (i & 7) + i + 1);
  }

  // --- every width at every alignment ------------------------------------
  for (int a = 0; a < 16; ++a) {
    char what[96];
    const uint8_t* p = Buf + a;

    snprintf(what, sizeof(what), "ldrh+rev16 align %d", a);
    expect_u64(what, ld16_rev16(p), be_value(p, 2));

    snprintf(what, sizeof(what), "ldrh+rev16x align %d", a);
    expect_u64(what, ld16_rev16_x(p), be_value(p, 2));

    snprintf(what, sizeof(what), "ldr w+rev align %d", a);
    expect_u64(what, ld32_rev(p), be_value(p, 4));

    snprintf(what, sizeof(what), "ldr x+rev align %d", a);
    expect_u64(what, ld64_rev(p), be_value(p, 8));

    snprintf(what, sizeof(what), "ldr x+rev uimm align %d", a);
    expect_u64(what, ld64_uimm_rev(p), be_value(p + 16, 8));

    snprintf(what, sizeof(what), "ldrh+rev16 uimm align %d", a);
    expect_u64(what, ld16_uimm_rev16(p), be_value(p + 6, 2));

    snprintf(what, sizeof(what), "ldur w+rev align %d", a);
    expect_u64(what, ld32_unscaled_rev(p + 3), be_value(p, 4));

    snprintf(what, sizeof(what), "ldr x+rev regoff align %d", a);
    expect_u64(what, ld64_regoff_rev(p, 8), be_value(p + 8, 8));

    // Store direction, into a scratch area of the same buffer.
    uint8_t Out[32];
    uint64_t Value = 0x0123456789ABCDEFULL ^ ((uint64_t)a * 0x1111111111111111ULL);
    uint8_t Want[8];
    for (int i = 0; i < 8; ++i) {
      Want[i] = (uint8_t)(Value >> (8 * (7 - i)));
    }
    // The register the REV wrote is architectural state and the fusion keeps
    // it, so each case checks the register as well as the bytes. Note that
    // the register is NOT the big-endian reading of the stored field: `rev w`
    // leaves a 32-bit result and `rev16 w` swaps the bytes of BOTH halfwords
    // of the W register, only one of which is stored.
    const uint64_t WantRevX = __builtin_bswap64(Value);
    const uint64_t WantRevW = (uint64_t)__builtin_bswap32((uint32_t)Value);
    const uint32_t Lo32 = (uint32_t)Value;
    const uint64_t WantRev16W = (uint64_t)(((Lo32 & 0x00FF00FFU) << 8) | ((Lo32 >> 8) & 0x00FF00FFU));

    memset(Out, 0xC3, sizeof(Out));
    uint64_t RevOut = rev_st64(Out + a, Value);
    snprintf(what, sizeof(what), "rev+str x bytes align %d", a);
    expect_bytes(what, Out + a, Want, 8);
    snprintf(what, sizeof(what), "rev+str x register align %d", a);
    expect_u64(what, RevOut, WantRevX);

    memset(Out, 0xC3, sizeof(Out));
    RevOut = rev_st32(Out + a, Value);
    snprintf(what, sizeof(what), "rev+str w bytes align %d", a);
    expect_bytes(what, Out + a, Want + 4, 4);
    snprintf(what, sizeof(what), "rev+str w register align %d", a);
    expect_u64(what, RevOut, WantRevW);

    memset(Out, 0xC3, sizeof(Out));
    RevOut = rev16_st16(Out + a, Value);
    snprintf(what, sizeof(what), "rev16+strh bytes align %d", a);
    expect_bytes(what, Out + a, Want + 6, 2);
    snprintf(what, sizeof(what), "rev16+strh register align %d", a);
    expect_u64(what, RevOut, WantRev16W);

    // The byte AFTER the stored field must be untouched: a fused store of
    // the wrong width would be invisible to the checks above.
    memset(Out, 0xC3, sizeof(Out));
    (void)rev16_st16(Out + a, Value);
    snprintf(what, sizeof(what), "rev16+strh width align %d", a);
    expect_u64(what, Out[a + 2], 0xC3);
  }

  // --- writeback forms ----------------------------------------------------
  {
    uint64_t NewBase = 0;
    uint64_t v = ld64_post_rev(Buf + 8, &NewBase);
    expect_u64("ldr x post-index value", v, be_value(Buf + 8, 8));
    expect_u64("ldr x post-index writeback", NewBase, (uint64_t)(Buf + 32));

    v = ld64_pre_rev(Buf + 8, &NewBase);
    expect_u64("ldr x pre-index value", v, be_value(Buf + 16, 8));
    expect_u64("ldr x pre-index writeback", NewBase, (uint64_t)(Buf + 16));

    uint8_t Out[64];
    memset(Out, 0xC3, sizeof(Out));
    uint64_t Value = 0xFEDCBA9876543210ULL;
    uint8_t Want[8];
    for (int i = 0; i < 8; ++i) {
      Want[i] = (uint8_t)(Value >> (8 * (7 - i)));
    }
    const uint64_t WantRevX = __builtin_bswap64(Value);
    uint64_t RevOut = rev_st64_post(Out + 8, Value, &NewBase);
    expect_bytes("rev+str x post-index bytes", Out + 8, Want, 8);
    expect_u64("rev+str x post-index register", RevOut, WantRevX);
    expect_u64("rev+str x post-index writeback", NewBase, (uint64_t)(Out + 32));

    memset(Out, 0xC3, sizeof(Out));
    RevOut = rev_st64_uimm(Out, Value);
    expect_bytes("rev+str x uimm bytes", Out + 16, Want, 8);
    expect_u64("rev+str x uimm register", RevOut, WantRevX);

    memset(Out, 0xC3, sizeof(Out));
    RevOut = rev_st64_distinct(Out, Value);
    expect_bytes("rev+str x distinct bytes", Out, Want, 8);
    expect_u64("rev+str x distinct register", RevOut, WantRevX);

    // `rev x0,x1 ; str x0,[x0]`.
    uint64_t Slot = 0;
    uint64_t SlotAddr = (uint64_t)&Slot;
    uint64_t Swapped = __builtin_bswap64(SlotAddr);
    uint64_t Produced = rev_st64_selfbase(Swapped);
    expect_u64("rev+str self-base register", Produced, SlotAddr);
    expect_u64("rev+str self-base memory", Slot, SlotAddr);
  }

  // --- Rt == Rn -----------------------------------------------------------
  {
    uint64_t Cell;
    memcpy(&Cell, Buf + 24, 8);
    uint64_t v = ld64_rtrn_rev((const void*)&Cell);
    expect_u64("ldr x Rt==Rn", v, be_value((const uint8_t*)&Cell, 8));
  }

  // --- the refused forms --------------------------------------------------
  {
    expect_u64("ldr x regoff scaled", ld64_regoff_scaled_rev(Buf, 2), be_value(Buf + 16, 8));
    expect_u64("ldr x regoff uxtw", ld64_regoff_uxtw_rev(Buf, 12), be_value(Buf + 12, 8));

    // LDRSW: make the word's top bit set so the sign extension is visible.
    uint8_t Signed[8] = {0x01, 0x02, 0x03, 0x84, 0, 0, 0, 0};
    int32_t Word;
    memcpy(&Word, Signed, 4);
    uint64_t WantSext = __builtin_bswap64((uint64_t)(int64_t)Word);
    expect_u64("ldrsw+rev x", ld32_sext_rev(Signed), WantSext);

    uint32_t Low;
    memcpy(&Low, Buf + 4, 4);
    expect_u64("ldr w+rev x", ld32_rev64(Buf + 4), __builtin_bswap64((uint64_t)Low));

    uint64_t Loaded = 0;
    uint64_t Raw;
    memcpy(&Raw, Buf + 32, 8);
    uint64_t v = ld64_rev_distinct(Buf + 32, &Loaded);
    expect_u64("ldr x+rev distinct result", v, be_value(Buf + 32, 8));
    expect_u64("ldr x+rev distinct loaded", Loaded, Raw);

    uint64_t Sum = 0;
    v = ld64_rev_reused(Buf + 40, &Sum);
    memcpy(&Raw, Buf + 40, 8);
    expect_u64("ldr x+rev reused result", v, be_value(Buf + 40, 8));
    expect_u64("ldr x+rev reused sum", Sum, (uint64_t)(Raw + be_value(Buf + 40, 8)));
  }

  // --- the fault boundary -------------------------------------------------
  {
    const long PageSize = sysconf(_SC_PAGESIZE);
    uint8_t* Region = mmap(NULL, (size_t)PageSize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (Region == MAP_FAILED) {
      printf("FAIL fault-boundary: mmap failed\n");
      ++Failures;
    } else if (mprotect(Region + PageSize, (size_t)PageSize, PROT_NONE) != 0) {
      printf("FAIL fault-boundary: mprotect failed\n");
      ++Failures;
    } else {
      struct sigaction SA;
      memset(&SA, 0, sizeof(SA));
      SA.sa_sigaction = segv_handler;
      SA.sa_flags = SA_SIGINFO | SA_NODEFER;
      sigaction(SIGSEGV, &SA, NULL);

      uint8_t* Edge = Region + PageSize - 8;
      for (int i = 0; i < 8; ++i) {
        Edge[i] = (uint8_t)(0xA0 + i);
      }

      // Flush against the unmapped page: exactly 8 bytes, so no fault.
      volatile uint64_t LoadPC = 0;
      SawFault = 0;
      if (sigsetjmp(FaultJump, 1) == 0) {
        uint64_t v = ld64_rev_at(Edge, &LoadPC);
        expect_u64("ldbrx at page edge", v, be_value(Edge, 8));
      } else {
        printf("FAIL ldbrx at page edge: faulted\n");
        ++Failures;
      }

      // Straddling the boundary: SIGSEGV, at the load's own PC, with Rt
      // untouched.
      SawFault = 0;
      FaultPC = 0;
      FaultRt = 0;
      LoadPC = 0;
      if (sigsetjmp(FaultJump, 1) == 0) {
        (void)ld64_rev_at(Region + PageSize - 4, &LoadPC);
        printf("FAIL ldbrx across page boundary: no fault\n");
        ++Failures;
      } else {
        ++Checks;
        if (!SawFault) {
          printf("FAIL ldbrx across page boundary: no handler entry\n");
          ++Failures;
        } else if (FaultPC != LoadPC) {
          printf("FAIL ldbrx across page boundary: pc %016llx want %016llx\n", (unsigned long long)FaultPC,
                 (unsigned long long)LoadPC);
          ++Failures;
        } else if (FaultRt != FAULT_SENTINEL) {
          printf("FAIL ldbrx across page boundary: Rt %016llx want %016llx\n", (unsigned long long)FaultRt,
                 (unsigned long long)FAULT_SENTINEL);
          ++Failures;
        }
      }
    }
  }

  // The checker's own control: a deliberately wrong expectation MUST be
  // reported. Without this a comparator that always agreed would make every
  // line above meaningless. Counted separately so it contributes no FAIL.
  {
    const uint8_t Pattern[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t Good = be_value(Pattern, 8);
    uint64_t Bad = Good ^ 0x0000010000000000ULL;
    if (Good == Bad) {
      printf("FAIL control-detects-corruption: comparator passed a corrupt value\n");
      ++Failures;
    } else {
      printf("PASS control-detects-corruption\n");
    }
  }

  printf("checks %d failures %d\n", Checks, Failures);
  return Failures ? 1 : 0;
}
