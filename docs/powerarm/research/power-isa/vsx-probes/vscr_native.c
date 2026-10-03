#include <stdio.h>
#include <altivec.h>
int main(void) {
  __vector unsigned int v;
  __asm__ volatile("mfvscr %0" : "=v"(v));
  unsigned int out[4] __attribute__((aligned(16)));
  __asm__ volatile("stvx %1,0,%0" :: "r"(out), "v"(v) : "memory");
  printf("VSCR words: %08x %08x %08x %08x  NJ(bit16 of low word)=%u\n", out[0], out[1], out[2], out[3], (out[0]>>16)&1);
  /* vcmpeqfp on denormal vs 0 natively */
  __vector float den = {1e-40f,1e-40f,1e-40f,1e-40f}, z = {0,0,0,0};
  __vector unsigned int eq; __asm__ volatile("vcmpeqfp %0,%1,%2" : "=v"(eq) : "v"(den), "v"(z));
  __vector unsigned int xeq; __asm__ volatile("xvcmpeqsp %x0,%x1,%x2" : "=wa"(xeq) : "wa"(den), "wa"(z));
  unsigned int o1[4] __attribute__((aligned(16))), o2[4] __attribute__((aligned(16)));
  __asm__ volatile("stvx %1,0,%0" :: "r"(o1), "v"(eq) : "memory");
  __asm__ volatile("stvx %1,0,%0" :: "r"(o2), "v"(xeq) : "memory");
  printf("native vcmpeqfp(den,0) lane0=%08x   xvcmpeqsp(den,0) lane0=%08x\n", o1[0], o2[0]);
  return 0;
}
