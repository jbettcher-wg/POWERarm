#include <stdio.h>
#include <stdint.h>
#include <string.h>
static uint32_t f2u(float f){uint32_t u; memcpy(&u,&f,4); return u;}
static uint64_t d2u(double d){uint64_t u; memcpy(&u,&d,8); return u;}
__attribute__((noinline)) static void test(const char* tag){
  float den = 1e-40f, zf = 0.0f; double dden = 1e-310, zd = 0.0;
  uint32_t eq32, gt32; float mn32, ad32; uint64_t eq64; double ad64;
  __asm__ volatile("dup v1.4s, %w1\n\tdup v2.4s, %w2\n\tfcmeq v0.4s, v1.4s, v2.4s\n\tfmov %w0, s0" : "=r"(eq32) : "r"(f2u(den)), "r"(f2u(zf)) : "v0","v1","v2");
  __asm__ volatile("dup v1.4s, %w1\n\tdup v2.4s, %w2\n\tfcmgt v0.4s, v1.4s, v2.4s\n\tfmov %w0, s0" : "=r"(gt32) : "r"(f2u(den)), "r"(f2u(zf)) : "v0","v1","v2");
  __asm__ volatile("dup v1.4s, %w1\n\tdup v2.4s, %w2\n\tfmin v0.4s, v1.4s, v2.4s\n\tfmov %s0, s0" : "=w"(mn32) : "r"(f2u(den)), "r"(f2u(zf)) : "v0","v1","v2");
  __asm__ volatile("dup v1.4s, %w1\n\tfadd v0.4s, v1.4s, v1.4s\n\tfmov %s0, s0" : "=w"(ad32) : "r"(f2u(den)) : "v0","v1");
  __asm__ volatile("dup v1.2d, %1\n\tdup v2.2d, %2\n\tfcmeq v0.2d, v1.2d, v2.2d\n\tfmov %0, d0" : "=r"(eq64) : "r"(d2u(dden)), "r"(d2u(zd)) : "v0","v1","v2");
  __asm__ volatile("dup v1.2d, %1\n\tfadd v0.2d, v1.2d, v1.2d\n\tfmov %d0, d0" : "=w"(ad64) : "r"(d2u(dden)) : "v0","v1");
  uint64_t fpcr, fpsr; __asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr" : "=r"(fpcr), "=r"(fpsr));
  printf("%-14s fcmeq.4s(den,0)=%08x fcmgt.4s(den,0)=%08x fmin.4s=%08x fadd.4s(den+den)=%08x | fcmeq.2d=%016llx fadd.2d=%016llx | fpcr=%llx fpsr=%llx\n",
    tag, eq32, gt32, f2u(mn32), f2u(ad32), (unsigned long long)eq64, (unsigned long long)d2u(ad64), (unsigned long long)fpcr, (unsigned long long)fpsr);
}
int main(void){
  test("fresh");
  uint64_t z = 0; __asm__ volatile("msr fpsr, %0" :: "r"(z));
  test("after msr fpsr");
  uint64_t fz = 1ull<<24; __asm__ volatile("msr fpcr, %0" :: "r"(fz));
  test("FZ=1");
  return 0;
}
