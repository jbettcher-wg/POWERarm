// Cache maintenance Linux lets EL0 run (SCTLR_EL1.UCI): DC CVAC, DC CIVAC and
// DC CVAU over a live buffer, then the buffer's contents. None may fault or
// change the data. Firefox runs DC CIVAC at startup.
#include <stdint.h>
#include <stdio.h>

static uint8_t Buf[4096] __attribute__((aligned(64)));

int main(void) {
  for (int i = 0; i < 4096; i++) Buf[i] = (uint8_t)(i * 37 + 11);
  for (int i = 0; i < 4096; i += 64) {
    void* P = &Buf[i];
    __asm__ volatile("dc cvac, %0" ::"r"(P) : "memory");
    __asm__ volatile("dc civac, %0" ::"r"(P) : "memory");
    __asm__ volatile("dc cvau, %0" ::"r"(P) : "memory");
  }
  __asm__ volatile("dsb ish" ::: "memory");
  uint32_t Sum = 0;
  for (int i = 0; i < 4096; i++) Sum = Sum * 31 + Buf[i];
  printf("dc cvac/civac/cvau over 64 lines: data %08x\n", Sum);
  return 0;
}
