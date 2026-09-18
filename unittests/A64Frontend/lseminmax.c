// SPDX-License-Identifier: MIT
//
// FEAT_LSE atomic min/max, single threaded: the value left in memory and the
// value returned in Rt for LDSMAX/LDSMIN/LDUMAX/LDUMIN at every access width
// and every acquire/release variant.
//
// These four are the LSE family POWER has no atomic form of, so POWERarm
// spells them out as a load/compare/CAS retry loop (TranslateExclusive.cpp
// AtomicMinMax). That makes two things worth covering beyond the plain result:
//
//  - Signedness at the access width. Every seed/operand pair below is chosen
//    so the signed and the unsigned answer differ, which is what catches a
//    missing or wrong-width sign extension. The 64-bit pair is the only one
//    where the operands are already register-width.
//  - Register aliasing. The loop reloads the address and the operand from Rn
//    and Rs on each iteration, so Rt aliasing either of them is the case that
//    would break if the loaded value were parked in a guest register instead
//    of in CPUState. Rt == Rs is the ordinary `x = max(x, *p)` shape.
//
// NOT covered, and not coverable single threaded: the retry itself. With no
// competing writer the CAS always succeeds first time round, so the back edge
// never executes here. That path wants a threaded stress test rather than a
// golden-compared one, since its output would not be deterministic.
//
// Differential: the Pi runs the same binary and its stdout is the golden.
#include <stdint.h>
#include <stdio.h>

// The corpus is built without -march, so state the assembler arch here.
__asm__(".arch armv8.1-a");

// Chosen so that signed and unsigned disagree at all four widths:
//   8-bit  0xef (-17)      vs 0x10 (16)
//   16-bit 0xcdef (-12817) vs 0x3210 (12816)
//   32-bit 0x89abcdef (<0) vs 0x76543210 (>0)
//   64-bit 0x0123... (>0)  vs 0xfedc... (<0)
#define SEED64 0x0123456789abcdefULL
#define OPND64 0xfedcba9876543210ULL

// One op, one width, one ordering. NAME is the mnemonic as written.
#define RMW(name, sfx, ctype, pfmt, seed, opnd)                                     \
  do {                                                                              \
    volatile ctype mem = (ctype)(seed);                                             \
    uint64_t rt = 0xdeadbeefcafef00dULL;                                            \
    uint64_t rs = (uint64_t)(ctype)(opnd);                                          \
    __asm__ __volatile__(name " " sfx "1, " sfx "0, [%2]"                           \
                         : "=&r"(rt)                                                \
                         : "r"(rs), "r"(&mem)                                       \
                         : "memory");                                               \
    printf("%-12s mem=" pfmt " rt=%016llx\n", name, (ctype)mem,                     \
           (unsigned long long)rt);                                                 \
  } while (0)

#define RMW_W4(op, w, sfx, ctype, pfmt, seed, opnd)                                 \
  RMW(op w, sfx, ctype, pfmt, seed, opnd);                                          \
  RMW(op "a" w, sfx, ctype, pfmt, seed, opnd);                                      \
  RMW(op "l" w, sfx, ctype, pfmt, seed, opnd);                                      \
  RMW(op "al" w, sfx, ctype, pfmt, seed, opnd)

#define RMW_WIDTHS(op, seed, opnd)                                                  \
  RMW_W4(op, "b", "%w", uint8_t, "%02x", seed, opnd);                               \
  RMW_W4(op, "h", "%w", uint16_t, "%04x", seed, opnd);                              \
  RMW_W4(op, "", "%w", uint32_t, "%08x", seed, opnd);                               \
  RMW_W4(op, "", "%x", uint64_t, "%016llx", seed, opnd)

#define ALL_OPS(seed, opnd)                                                         \
  RMW_WIDTHS("ldsmax", seed, opnd);                                                 \
  RMW_WIDTHS("ldsmin", seed, opnd);                                                 \
  RMW_WIDTHS("ldumax", seed, opnd);                                                 \
  RMW_WIDTHS("ldumin", seed, opnd)

int main(void) {
  // The discriminating pair, and the same pair with the roles swapped so that
  // each op is exercised with the winner on both sides of the compare.
  ALL_OPS(SEED64, OPND64);
  ALL_OPS(OPND64, SEED64);

  // Equal operands: neither side wins, and the value must come back unchanged.
  ALL_OPS(SEED64, SEED64);

  // Sign and magnitude boundaries per width. 0x80.. is the signed minimum and
  // the unsigned "large" value, which is where a wrong-width extension shows.
  ALL_OPS(0x8000000000000000ULL, 0x7fffffffffffffffULL);
  ALL_OPS(0x0000000000000000ULL, 0xffffffffffffffffULL);
  ALL_OPS(0xffffffffffffffffULL, 0x0000000000000001ULL);

  // Rt == Rs: the destination is also the operand register.
  {
    volatile int64_t mem = -5;
    uint64_t v = 7;
    __asm__ __volatile__("ldsmax %x0, %x0, [%1]" : "+r"(v) : "r"(&mem) : "memory");
    printf("%-12s mem=%016llx rt=%016llx\n", "ldsmax-ts", (unsigned long long)mem,
           (unsigned long long)v);
  }
  {
    volatile uint32_t mem = 0x80000000u;
    uint64_t v = 1;
    __asm__ __volatile__("ldumin %w0, %w0, [%1]" : "+r"(v) : "r"(&mem) : "memory");
    printf("%-12s mem=%08x rt=%016llx\n", "ldumin-ts", (uint32_t)mem,
           (unsigned long long)v);
  }

  // Rt == Rn: the destination is also the address register.
  {
    volatile int64_t mem = -5;
    uint64_t addr = (uint64_t)&mem;
    uint64_t rs = 7;
    __asm__ __volatile__("ldsmax %x1, %x0, [%x0]" : "+r"(addr) : "r"(rs) : "memory");
    printf("%-12s mem=%016llx rt=%016llx\n", "ldsmax-tn", (unsigned long long)mem,
           (unsigned long long)addr);
  }

  // Rs == Rn: the operand register is also the address register. The operand
  // is then the pointer value itself, so only the comparison's direction is
  // predictable; print what memory ends up holding relative to the address.
  {
    volatile uint64_t mem = 0;
    uint64_t addr = (uint64_t)&mem;
    uint64_t rt = 0;
    __asm__ __volatile__("ldumax %x1, %x0, [%x1]" : "=&r"(rt) : "r"(addr) : "memory");
    printf("%-12s mem==addr=%d rt=%016llx\n", "ldumax-sn",
           (unsigned long long)mem == (unsigned long long)addr, (unsigned long long)rt);
  }

  // Zero-register forms: Rt == 31 discards the loaded value (the ST<op>
  // alias) and Rs == 31 compares against zero.
  {
    volatile int32_t mem = -3;
    __asm__ __volatile__("stsmax %w0, [%1]" : : "r"(4), "r"(&mem) : "memory");
    printf("%-12s mem=%08x\n", "stsmax", (uint32_t)mem);
  }
  {
    volatile int32_t mem = -3;
    __asm__ __volatile__("stsmin %w0, [%1]" : : "r"(4), "r"(&mem) : "memory");
    printf("%-12s mem=%08x\n", "stsmin", (uint32_t)mem);
  }
  {
    volatile int32_t mem = -3;
    uint64_t rt = 0;
    __asm__ __volatile__("ldsmaxal wzr, %w0, [%1]" : "=&r"(rt) : "r"(&mem) : "memory");
    printf("%-12s mem=%08x rt=%016llx\n", "ldsmaxal-zr", (uint32_t)mem,
           (unsigned long long)rt);
  }
  {
    volatile uint32_t mem = 0x80000000u;
    uint64_t rt = 0;
    __asm__ __volatile__("lduminal wzr, %w0, [%1]" : "=&r"(rt) : "r"(&mem) : "memory");
    printf("%-12s mem=%08x rt=%016llx\n", "lduminal-zr", (uint32_t)mem,
           (unsigned long long)rt);
  }
  return 0;
}
