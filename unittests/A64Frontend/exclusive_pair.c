// SPDX-License-Identifier: MIT
//
// ARMv8.0 LDXP, STXP, LDAXP, STLXP: exclusive pairs in both 32-bit (W) and
// 64-bit (X) variants, checking successful reservation and atomic update,
// second-store failure, store without load, memory modification between
// load and store, CLREX, size mismatch, arbitrary/non-consecutive registers,
// zero registers (XZR), and NZCV preservation across success and fail legs.
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond, msg) do { \
  if (!(cond)) { \
    printf("FAIL: %s at line %d\n", msg, __LINE__); \
    exit(1); \
  } \
} while (0)

static void test_64bit_basic(void) {
  alignas(16) volatile uint64_t mem[2] = { 0x1122334455667788ULL, 0x99aabbccddeeff00ULL };
  uint64_t r0 = 0, r1 = 0;
  uint32_t status = 99;

  // 1. Success case: LDXP -> STXP
  __asm__ volatile(
    "ldxp %[r0], %[r1], [%[addr]]\n"
    "stxp %w[st], %[v0], %[v1], [%[addr]]\n"
    : [r0] "=&r"(r0), [r1] "=&r"(r1), [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0xaabbccddeeff0011ULL), [v1] "r"(0x2233445566778899ULL)
    : "memory"
  );
  CHECK(r0 == 0x1122334455667788ULL, "LDXP r0 value");
  CHECK(r1 == 0x99aabbccddeeff00ULL, "LDXP r1 value");
  CHECK(status == 0, "STXP status on success");
  CHECK(mem[0] == 0xaabbccddeeff0011ULL, "mem[0] updated");
  CHECK(mem[1] == 0x2233445566778899ULL, "mem[1] updated");

  // 2. Second store after first succeeds should fail
  status = 99;
  __asm__ volatile(
    "stxp %w[st], %[v0], %[v1], [%[addr]]\n"
    : [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0x1ULL), [v1] "r"(0x2ULL)
    : "memory"
  );
  CHECK(status == 1, "Second STXP must fail");
  CHECK(mem[0] == 0xaabbccddeeff0011ULL, "mem[0] unchanged on fail");
  CHECK(mem[1] == 0x2233445566778899ULL, "mem[1] unchanged on fail");

  // 3. Store without load must fail
  status = 99;
  __asm__ volatile(
    "stxp %w[st], %[v0], %[v1], [%[addr]]\n"
    : [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0x3ULL), [v1] "r"(0x4ULL)
    : "memory"
  );
  CHECK(status == 1, "STXP without LDXP must fail");

  // 4. Memory modified between LDXP and STXP: STXP must fail
  status = 99;
  __asm__ volatile(
    "ldxp %[r0], %[r1], [%[addr]]\n"
    : [r0] "=&r"(r0), [r1] "=&r"(r1)
    : [addr] "r"(&mem[0])
    : "memory"
  );
  mem[1] ^= 0x1; // Concurrent writer modified high word
  __asm__ volatile(
    "stxp %w[st], %[v0], %[v1], [%[addr]]\n"
    : [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0x5ULL), [v1] "r"(0x6ULL)
    : "memory"
  );
  CHECK(status == 1, "STXP after memory modified must fail");

  // 5. CLREX between LDXP and STXP: STXP must fail
  status = 99;
  __asm__ volatile(
    "ldxp %[r0], %[r1], [%[addr]]\n"
    "clrex\n"
    "stxp %w[st], %[v0], %[v1], [%[addr]]\n"
    : [r0] "=&r"(r0), [r1] "=&r"(r1), [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0x7ULL), [v1] "r"(0x8ULL)
    : "memory"
  );
  CHECK(status == 1, "STXP after CLREX must fail");

  printf("test_64bit_basic: PASS\n");
}

static void test_64bit_acquire_release(void) {
  alignas(16) volatile uint64_t mem[2] = { 0x1234ULL, 0x5678ULL };
  uint64_t r0 = 0, r1 = 0;
  uint32_t status = 99;

  __asm__ volatile(
    "ldaxp %[r0], %[r1], [%[addr]]\n"
    "stlxp %w[st], %[v0], %[v1], [%[addr]]\n"
    : [r0] "=&r"(r0), [r1] "=&r"(r1), [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0x9999ULL), [v1] "r"(0xaaaaULL)
    : "memory"
  );
  CHECK(r0 == 0x1234ULL, "LDAXP r0");
  CHECK(r1 == 0x5678ULL, "LDAXP r1");
  CHECK(status == 0, "STLXP status");
  CHECK(mem[0] == 0x9999ULL, "mem[0]");
  CHECK(mem[1] == 0xaaaaULL, "mem[1]");

  printf("test_64bit_acquire_release: PASS\n");
}

static void test_32bit_basic(void) {
  alignas(8) volatile uint32_t mem[2] = { 0x12345678U, 0x9abcdef0U };
  uint64_t r0 = 0xdeadbeefdeadbeefULL, r1 = 0xdeadbeefdeadbeefULL;
  uint32_t status = 99;

  // 1. Success case: LDXP -> STXP (32-bit: W registers)
  __asm__ volatile(
    "ldxp %w[r0], %w[r1], [%[addr]]\n"
    "stxp %w[st], %w[v0], %w[v1], [%[addr]]\n"
    : [r0] "=&r"(r0), [r1] "=&r"(r1), [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0x55aa55aaU), [v1] "r"(0x33cc33ccU)
    : "memory"
  );
  // Zero-extension invariant: high 32 bits of X register must be 0!
  CHECK(r0 == 0x12345678ULL, "LDXP 32-bit zero-extension r0");
  CHECK(r1 == 0x9abcdef0ULL, "LDXP 32-bit zero-extension r1");
  CHECK(status == 0, "STXP 32-bit status");
  CHECK(mem[0] == 0x55aa55aaU, "mem[0] 32-bit");
  CHECK(mem[1] == 0x33cc33ccU, "mem[1] 32-bit");

  // 2. Second store fails
  status = 99;
  __asm__ volatile(
    "stxp %w[st], %w[v0], %w[v1], [%[addr]]\n"
    : [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0x1U), [v1] "r"(0x2U)
    : "memory"
  );
  CHECK(status == 1, "Second 32-bit STXP must fail");

  // 3. Store without load fails
  status = 99;
  __asm__ volatile(
    "stxp %w[st], %w[v0], %w[v1], [%[addr]]\n"
    : [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0x3U), [v1] "r"(0x4U)
    : "memory"
  );
  CHECK(status == 1, "32-bit STXP without LDXP must fail");

  // 4. Memory modified in between fails
  status = 99;
  __asm__ volatile(
    "ldxp %w[r0], %w[r1], [%[addr]]\n"
    : [r0] "=&r"(r0), [r1] "=&r"(r1)
    : [addr] "r"(&mem[0])
    : "memory"
  );
  mem[0] = 0xbeefcafeU;
  __asm__ volatile(
    "stxp %w[st], %w[v0], %w[v1], [%[addr]]\n"
    : [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0x5U), [v1] "r"(0x6U)
    : "memory"
  );
  CHECK(status == 1, "32-bit STXP after memory modified must fail");

  // 5. CLREX
  status = 99;
  mem[0] = 0x11111111U;
  mem[1] = 0x22222222U;
  __asm__ volatile(
    "ldxp %w[r0], %w[r1], [%[addr]]\n"
    "clrex\n"
    "stxp %w[st], %w[v0], %w[v1], [%[addr]]\n"
    : [r0] "=&r"(r0), [r1] "=&r"(r1), [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0x7U), [v1] "r"(0x8U)
    : "memory"
  );
  CHECK(status == 1, "32-bit STXP after CLREX must fail");

  printf("test_32bit_basic: PASS\n");
}

static void test_32bit_acquire_release(void) {
  alignas(8) volatile uint32_t mem[2] = { 0x11112222U, 0x33334444U };
  uint64_t r0 = 0, r1 = 0;
  uint32_t status = 99;

  __asm__ volatile(
    "ldaxp %w[r0], %w[r1], [%[addr]]\n"
    "stlxp %w[st], %w[v0], %w[v1], [%[addr]]\n"
    : [r0] "=&r"(r0), [r1] "=&r"(r1), [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0x55556666U), [v1] "r"(0x77778888U)
    : "memory"
  );
  CHECK(r0 == 0x11112222ULL, "LDAXP 32-bit r0");
  CHECK(r1 == 0x33334444ULL, "LDAXP 32-bit r1");
  CHECK(status == 0, "STLXP 32-bit status");
  CHECK(mem[0] == 0x55556666U, "mem[0]");
  CHECK(mem[1] == 0x77778888U, "mem[1]");

  printf("test_32bit_acquire_release: PASS\n");
}

static void test_size_mismatch(void) {
  alignas(16) volatile uint64_t mem[2] = { 0x100ULL, 0x200ULL };
  uint64_t r0 = 0, r1 = 0;
  uint32_t status = 99;

  // 32-bit LDXP followed by 64-bit STXP must fail
  __asm__ volatile(
    "ldxp %w[r0], %w[r1], [%[addr]]\n"
    "stxp %w[st], %[v0], %[v1], [%[addr]]\n"
    : [r0] "=&r"(r0), [r1] "=&r"(r1), [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0x300ULL), [v1] "r"(0x400ULL)
    : "memory"
  );
  CHECK(status == 1, "64-bit STXP after 32-bit LDXP must fail");

  // 64-bit LDXP followed by 32-bit STXP must fail
  __asm__ volatile(
    "ldxp %[r0], %[r1], [%[addr]]\n"
    "stxp %w[st], %w[v0], %w[v1], [%[addr]]\n"
    : [r0] "=&r"(r0), [r1] "=&r"(r1), [st] "=&r"(status), "+m"(mem)
    : [addr] "r"(&mem[0]), [v0] "r"(0x500U), [v1] "r"(0x600U)
    : "memory"
  );
  CHECK(status == 1, "32-bit STXP after 64-bit LDXP must fail");

  printf("test_size_mismatch: PASS\n");
}

static void test_arbitrary_registers(void) {
  alignas(16) volatile uint64_t mem[2] = { 0x10ULL, 0x20ULL };

  // Non-consecutive registers: x7, x2, x4, x8
  register uint64_t r7 __asm__("x7") = 0;
  register uint64_t r2 __asm__("x2") = 0;
  register uint64_t r4 __asm__("x4") = 0x30ULL;
  register uint64_t r8 __asm__("x8") = 0x40ULL;
  register uint32_t w9 __asm__("w9") = 99;

  __asm__ volatile(
    "ldxp x7, x2, [%[addr]]\n"
    "stxp w9, x4, x8, [%[addr]]\n"
    : "+r"(r7), "+r"(r2), "+r"(w9), "+m"(mem)
    : [addr] "r"(&mem[0]), "r"(r4), "r"(r8)
    : "memory"
  );
  CHECK(r7 == 0x10ULL, "Non-consecutive r7");
  CHECK(r2 == 0x20ULL, "Non-consecutive r2");
  CHECK(w9 == 0, "Non-consecutive status");
  CHECK(mem[0] == 0x30ULL, "mem[0]");
  CHECK(mem[1] == 0x40ULL, "mem[1]");

  // Storing zero registers xzr
  __asm__ volatile(
    "ldxp x7, x2, [%[addr]]\n"
    "stxp w9, xzr, xzr, [%[addr]]\n"
    : "+r"(r7), "+r"(r2), "+r"(w9), "+m"(mem)
    : [addr] "r"(&mem[0])
    : "memory"
  );
  CHECK(w9 == 0, "stxp xzr status");
  CHECK(mem[0] == 0ULL, "mem[0] zeroed");
  CHECK(mem[1] == 0ULL, "mem[1] zeroed");

  printf("test_arbitrary_registers: PASS\n");
}

static void test_nzcv_preservation(void) {
  alignas(16) volatile uint64_t mem[2] = { 0xaaaULL, 0xbbbULL };

  for (uint32_t flags = 0; flags < 16; ++flags) {
    uint64_t in_nzcv = ((uint64_t)flags) << 28;
    uint64_t out_nzcv = 0;
    uint64_t r0 = 0, r1 = 0;
    uint32_t status = 99;

    // Success path
    __asm__ volatile(
      "msr nzcv, %[in_nzcv]\n"
      "ldxp %[r0], %[r1], [%[addr]]\n"
      "stxp %w[st], %[r0], %[r1], [%[addr]]\n"
      "mrs %[out_nzcv], nzcv\n"
      : [r0] "=&r"(r0), [r1] "=&r"(r1), [st] "=&r"(status), [out_nzcv] "=&r"(out_nzcv), "+m"(mem)
      : [addr] "r"(&mem[0]), [in_nzcv] "r"(in_nzcv)
      : "memory"
    );
    CHECK(status == 0, "STXP success");
    CHECK((out_nzcv >> 28) == flags, "NZCV preserved on STXP success");

    // Fail path
    __asm__ volatile(
      "msr nzcv, %[in_nzcv]\n"
      "stxp %w[st], %[r0], %[r1], [%[addr]]\n"
      "mrs %[out_nzcv], nzcv\n"
      : [st] "=&r"(status), [out_nzcv] "=&r"(out_nzcv), "+m"(mem)
      : [addr] "r"(&mem[0]), [r0] "r"(r0), [r1] "r"(r1), [in_nzcv] "r"(in_nzcv)
      : "memory"
    );
    CHECK(status == 1, "STXP fail");
    CHECK((out_nzcv >> 28) == flags, "NZCV preserved on STXP fail");
  }

  printf("test_nzcv_preservation: PASS\n");
}

int main(void) {
  test_64bit_basic();
  test_64bit_acquire_release();
  test_32bit_basic();
  test_32bit_acquire_release();
  test_size_mismatch();
  test_arbitrary_registers();
  test_nzcv_preservation();
  printf("ALL EXCLUSIVE PAIR TESTS PASSED\n");
  return 0;
}
