// fex_vasize_probe.c - faithful C port of FEXCore::Allocator::DetermineVASize()
// (POWERarm FEXCore/Source/Utils/Allocator.cpp:121-162, host-page stepping) plus the
// derived placements POWERarm computes from it (ELFCodeLoader.h:608-614, :706-712;
// AllocatorHooks.cpp:48-49; Allocator.cpp:293-301). Read-only reproduction; does not
// link against or run POWERarm.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
static int find(uintptr_t size, long pg) {
  for (int i = 0; i < 64; ++i) {
    void *p = mmap((void *)(size - pg * i), pg, PROT_NONE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) { munmap(p, pg); if (p == (void *)(size - pg * i)) return 1; }
  }
  return 0;
}
int main(void) {
  long pg = sysconf(_SC_PAGESIZE);
  const int sizes[] = {57, 52, 48, 47, 42, 39, 36};
  int bits = -1;
  for (unsigned k = 0; k < 7; k++) if (find(1UL << sizes[k], pg)) { bits = sizes[k]; break; }
  printf("FEXPORT pagesize=%ld DetermineVASize=%d\n", pg, bits);
  printf("FEXPORT Setup48BitAllocatorIfExists engages=%s (steals [0x800000000000,0x1000000000000))\n", bits >= 48 ? "YES" : "no");
  uint64_t vas = bits > 47 ? 47 : bits;
  printf("FEXPORT guest stack hint = 2^%lu - 128MiB = 0x%lx\n", vas, (1UL << vas) - (128UL << 20));
  uint64_t hv = 1UL << bits, t = 1UL << 47;
  printf("FEXPORT ELF interp load hint = min(2^%d,2^47)/3*2 = 0x%lx\n", bits, (hv < t ? hv : t) / 3 * 2);
  printf("FEXPORT internal arena hint window = [0x10000000000, 0x200000000000)\n");
  return 0;
}
