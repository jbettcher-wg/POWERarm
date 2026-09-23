// Rewriting a range that has never been executed.
//
// This is the case SMCChecks=icache keeps working without any announcement at
// all: a loader that read(2)s or memcpy()s a plugin into RWX memory, decides it
// wants different bytes, and overwrites them before anything branches there.
// There is no stale instruction-cache line on hardware (nothing ever fetched
// from those addresses) and no stale translation here (nothing was ever
// compiled from them), so the first execution must see the last bytes written
// whether or not the writer flushed.
//
// It is also the case that distinguishes "never executed" from "executed and
// not flushed", which icpartial.c covers: the latter is architecturally
// undefined, this one is not.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef int (*intfn)(void);

// mov w0, #Imm ; ret
static void EmitConst(void* Dst, unsigned Imm) {
  uint32_t Code[2];
  Code[0] = 0x52800000u | ((Imm & 0xFFFFu) << 5);
  Code[1] = 0xD65F03C0u;
  memcpy(Dst, Code, sizeof Code);
}

static void FlushLine(void* P) {
  __asm__ volatile("ic ivau, %0\n\t"
                   "dsb ish\n\t"
                   "isb\n\t" ::"r"(P)
                   : "memory");
}

static unsigned char* FreshPage(long Page) {
  unsigned char* Mem = mmap(NULL, (size_t)Page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return Mem == MAP_FAILED ? NULL : Mem;
}

int main(void) {
  const long Page = sysconf(_SC_PAGESIZE);

  // 1. Write, flush, overwrite, flush -- never called in between.
  unsigned char* M1 = FreshPage(Page);
  if (!M1) {
    puts("mmap failed");
    return 1;
  }
  EmitConst(M1, 41);
  __builtin___clear_cache((char*)M1, (char*)M1 + 8);
  EmitConst(M1, 42);
  __builtin___clear_cache((char*)M1, (char*)M1 + 8);
  printf("twice-flushed %d\n", ((intfn)M1)());

  // 2. Write, flush, overwrite with NO flush -- never called in between.
  //
  //    The reasoning this case was written on ("the flush invalidated the line
  //    and nothing has re-fetched it since, so the first fetch sees the second
  //    write") is not something the architecture promises. The second write is
  //    never announced, so DDI 0487 B2.2.5 leaves both answers conforming, and
  //    the two machines disagree in practice: a Cortex-A76 returns the flushed
  //    value 43 -- the flush itself is enough for it to have the line -- while
  //    POWERarm returns 44 under every SMC mode, having translated the bytes
  //    that were there when it first decoded them. Normalised like icpartial's
  //    unflushed lines: what matters is that neither machine crashes or invents
  //    a third answer.
  unsigned char* M2 = FreshPage(Page);
  if (!M2) {
    puts("mmap failed");
    return 1;
  }
  EmitConst(M2, 43);
  FlushLine(M2);
  EmitConst(M2, 44);
  {
    const int Got = ((intfn)M2)();
    printf("flush-then-rewrite %s\n", (Got == 43 || Got == 44) ? "old-or-new" : "BAD");
  }

  // 3. Write twice with no flush at all into memory nothing has ever executed
  //    from.
  unsigned char* M3 = FreshPage(Page);
  if (!M3) {
    puts("mmap failed");
    return 1;
  }
  EmitConst(M3, 45);
  EmitConst(M3, 46);
  __builtin___clear_cache((char*)M3, (char*)M3 + 8);
  printf("rewrite-then-flush %d\n", ((intfn)M3)());

  // 4. A neighbour in the same 64-byte line as code that HAS run. Writing it
  //    is not a code change to the neighbour, and the flush of the neighbour's
  //    line must not disturb it: under mtrack this is the false-sharing fault,
  //    under icache it is nothing at all.
  unsigned char* M4 = FreshPage(Page);
  if (!M4) {
    puts("mmap failed");
    return 1;
  }
  EmitConst(M4, 47);      // bytes 0..7 of line 0
  EmitConst(M4 + 32, 48); // bytes 32..39 of the SAME line
  __builtin___clear_cache((char*)M4, (char*)M4 + 64);
  printf("shared-line %d %d\n", ((intfn)M4)(), ((intfn)(M4 + 32))());
  EmitConst(M4 + 32, 49);
  __builtin___clear_cache((char*)(M4 + 32), (char*)(M4 + 32) + 8);
  printf("shared-line %d %d\n", ((intfn)M4)(), ((intfn)(M4 + 32))());

  // 5. A page whose bytes arrive through read(2) rather than a store, and which
  //    only becomes executable afterwards. Nothing issues IC IVAU for this;
  //    the kernel's own first-exec-mapping sync is what makes it work on
  //    hardware, and the mprotect is what makes it work here.
  unsigned char* M5 = mmap(NULL, (size_t)Page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (M5 == MAP_FAILED) {
    puts("mmap failed");
    return 1;
  }
  EmitConst(M5, 50);
  if (mprotect(M5, (size_t)Page, PROT_READ | PROT_EXEC) != 0) {
    puts("mprotect failed");
    return 1;
  }
  printf("read-then-exec %d\n", ((intfn)M5)());

  return 0;
}
