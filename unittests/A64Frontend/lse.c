// SPDX-License-Identifier: MIT
//
// FEAT_LSE atomic memory operations, single threaded: the value left in
// memory and the value returned in Rt, for LDADD/LDCLR/LDEOR/LDSET/SWP and
// CAS at every access width and every acquire/release variant, plus the
// zero-register forms (Rt == 31, the ST<op> alias, and Rs == 31).
//
// Differential: the Pi runs the same binary and its stdout is the golden.
// Written for the Bun-based Claude Code executable, which takes the LSE arm
// of libgcc's outline atomics unconditionally, without consulting AT_HWCAP.
#include <stdint.h>
#include <stdio.h>

// The corpus is built without -march, so state the assembler arch here: LSE is
// ARMv8.1 and the LDAPR this test ends with is ARMv8.3.
__asm__(".arch armv8.3-a");

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
    printf("%-10s mem=" pfmt " rt=%016llx\n", name, (ctype)mem,                     \
           (unsigned long long)rt);                                                 \
  } while (0)

#define RMW_W4(op, w, sfx, ctype, pfmt)                                             \
  RMW(op w, sfx, ctype, pfmt, SEED64, OPND64);                                      \
  RMW(op "a" w, sfx, ctype, pfmt, SEED64, OPND64);                                  \
  RMW(op "l" w, sfx, ctype, pfmt, SEED64, OPND64);                                  \
  RMW(op "al" w, sfx, ctype, pfmt, SEED64, OPND64)

#define RMW_WIDTHS(op)                                                              \
  RMW_W4(op, "b", "%w", uint8_t, "%02x");                                           \
  RMW_W4(op, "h", "%w", uint16_t, "%04x");                                          \
  RMW_W4(op, "", "%w", uint32_t, "%08x");                                           \
  RMW_W4(op, "", "%x", uint64_t, "%016llx")

// CAS writes the old memory value back into Rs, so Rs is a read-write operand.
#define CAS_ONE(name, sfx, ctype, pfmt, seed, expected, desired)                     \
  do {                                                                              \
    volatile ctype mem = (ctype)(seed);                                             \
    uint64_t rs = (uint64_t)(ctype)(expected);                                       \
    uint64_t rt = (uint64_t)(ctype)(desired);                                        \
    __asm__ __volatile__(name " " sfx "0, " sfx "1, [%2]"                            \
                         : "+&r"(rs)                                                 \
                         : "r"(rt), "r"(&mem)                                        \
                         : "memory");                                                \
    printf("%-10s %-4s mem=" pfmt " rs=%016llx\n", name,                             \
           (expected) == (seed) ? "hit" : "miss", (ctype)mem,                        \
           (unsigned long long)rs);                                                  \
  } while (0)

#define CAS_PAIR(name, sfx, ctype, pfmt)                                            \
  CAS_ONE(name, sfx, ctype, pfmt, SEED64, SEED64, OPND64);                          \
  CAS_ONE(name, sfx, ctype, pfmt, SEED64, ~(uint64_t)SEED64, OPND64)

#define CAS_SET(op, w, sfx, ctype, pfmt)                                            \
  CAS_PAIR(op w, sfx, ctype, pfmt);                                                 \
  CAS_PAIR(op "a" w, sfx, ctype, pfmt);                                             \
  CAS_PAIR(op "l" w, sfx, ctype, pfmt);                                             \
  CAS_PAIR(op "al" w, sfx, ctype, pfmt)

int main(void) {
  RMW_WIDTHS("ldadd");
  RMW_WIDTHS("ldclr");
  RMW_WIDTHS("ldeor");
  RMW_WIDTHS("ldset");
  RMW_WIDTHS("swp");

  CAS_SET("cas", "b", "%w", uint8_t, "%02x");
  CAS_SET("cas", "h", "%w", uint16_t, "%04x");
  CAS_SET("cas", "", "%w", uint32_t, "%08x");
  CAS_SET("cas", "", "%x", uint64_t, "%016llx");

  // Zero-register forms: Rt == 31 discards the loaded value (the ST<op>
  // alias) and Rs == 31 makes LDADD a plain load.
  {
    volatile uint64_t mem = SEED64;
    __asm__ __volatile__("stadd %0, [%1]" : : "r"(OPND64), "r"(&mem) : "memory");
    printf("%-10s mem=%016llx\n", "stadd", (unsigned long long)mem);
  }
  {
    volatile uint32_t mem = 0x5a5a1234u;
    uint64_t rt = 0;
    __asm__ __volatile__("ldaddal wzr, %w0, [%1]" : "=&r"(rt) : "r"(&mem) : "memory");
    printf("%-10s mem=%08x rt=%016llx\n", "ldaddal-zr", (uint32_t)mem, (unsigned long long)rt);
  }
  {
    volatile uint8_t mem = 0xa5;
    uint64_t rt = 0;
    __asm__ __volatile__("ldaprb %w0, [%1]" : "=&r"(rt) : "r"(&mem) : "memory");
    printf("%-10s mem=%02x rt=%016llx\n", "ldaprb", (uint8_t)mem, (unsigned long long)rt);
  }
  return 0;
}
