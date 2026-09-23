// The AArch64 code-modification sequence, done correctly, in every shape a real
// runtime uses it. This is the guest contract SMCChecks=icache rests on: the
// architecture (DDI 0487 B2.2.5) requires the writer of an instruction to
// announce it, and POWERarm advertises CTR_EL0.DIC=0 (SystemRegisters.h), so
// every flush below ends in an IC IVAU per 64 bytes plus DSB ISH; ISB.
//
// Everything printed here is architecturally guaranteed, so the Pi and POWERarm
// must agree byte for byte.
//
// Cases:
//   1. fresh code in anonymous RWX memory, __builtin___clear_cache, call
//   2. patch it repeatedly, flushing each time
//   3. the same patch with a hand-written IC IVAU / DSB ISH / ISB instead of
//      libgcc's __aarch64_sync_cache_range, so the instruction itself is
//      exercised rather than libgcc's CTR_EL0 reading
//   4. a stub that rewrites an instruction of its OWN 64-byte line, flushes it,
//      ISBs and then executes it -- "modify my own block, ISB, continue". Run
//      twice, so the second run invalidates a translation that is live.
//   5. a W^X flip: mprotect RW, patch, flush, mprotect RX, call. The flush is
//      what makes this correct; the kernel does not re-sync a page that has
//      already been mapped executable once (PG_dcache_clean, arch/arm64/mm/flush.c).
//   6. patching a function in the program's own .text after it has run.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef int (*intfn)(void);
typedef int (*argfn)(void*);

// mov w0, #Imm ; ret
static void EmitConst(void* Dst, unsigned Imm) {
  uint32_t Code[2];
  Code[0] = 0x52800000u | ((Imm & 0xFFFFu) << 5); // movz w0, #Imm
  Code[1] = 0xD65F03C0u;                          // ret
  memcpy(Dst, Code, sizeof Code);
}

// One IC IVAU over the line holding P, then the two barriers the architecture
// asks for. Deliberately NOT __builtin___clear_cache: this is the instruction
// SMCChecks=icache listens to.
static void FlushLine(void* P) {
  __asm__ volatile("ic ivau, %0\n\t"
                   "dsb ish\n\t"
                   "isb\n\t" ::"r"(P)
                   : "memory");
}

// Case 4's stub, assembled by the toolchain rather than hand-encoded.
// Entered with x0 = its own base, which must be 64-byte aligned so that the
// instruction it patches (base+24) lies in the line IC IVAU is given.
// The extent is a fixed array bound rather than a second asm symbol. Both of
// the obvious alternatives misbehave here: differencing two extern C arrays is
// undefined and gcc folds it to zero, and an assembler-computed `.quad end -
// start` next to it got the SAME GOT slot as the stub itself in a static build,
// so the "size" read back as the stub's first two instructions. Eight words,
// counted in the asm below.
#define SELFMOD_STUB_WORDS 8
extern const uint32_t SelfModStub[SELFMOD_STUB_WORDS];
__asm__(".pushsection .rodata\n"
        ".balign 64\n"
        ".globl SelfModStub\n"
        ".hidden SelfModStub\n"
        "SelfModStub:\n"
        "  movz w1, #0x00e0\n"          // w1 = 0x528000E0
        "  movk w1, #0x5280, lsl #16\n" //    = movz w0, #7
        "  str  w1, [x0, #24]\n"        // patch the seventh word of this stub
        "  ic   ivau, x0\n"
        "  dsb  ish\n"
        "  isb\n"
        "  movz w0, #0\n" // <- becomes movz w0, #7
        "  ret\n"
        ".popsection\n");

// Case 6's target lives in the program's own .text -- a private file-backed
// mapping, which is a different VMA class from the anonymous memory every other
// case uses. Written in asm so that the word being patched is exactly known:
// a compiler that emits a BTI landing pad or a PAC prologue (either is a
// plausible distribution default) would put something else at offset 0 and the
// patch would land on it.
extern int TextTarget(void);
__asm__(".pushsection .text\n"
        ".globl TextTarget\n"
        ".hidden TextTarget\n"
        ".type TextTarget, %function\n"
        "TextTarget:\n"
        "  movz w0, #1\n"
        "  ret\n"
        ".size TextTarget, . - TextTarget\n"
        ".popsection\n");

int main(void) {
  const long Page = sysconf(_SC_PAGESIZE);
  unsigned char* Mem = mmap(NULL, (size_t)Page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Mem == MAP_FAILED) {
    puts("mmap failed");
    return 1;
  }

  // 1. Fresh code, flushed with the compiler builtin.
  EmitConst(Mem, 11);
  __builtin___clear_cache((char*)Mem, (char*)Mem + 8);
  printf("fresh %d\n", ((intfn)Mem)());

  // 2. Repeated patch + flush of code that is already translated.
  for (unsigned i = 12; i <= 15; ++i) {
    EmitConst(Mem, i);
    __builtin___clear_cache((char*)Mem, (char*)Mem + 8);
    printf("patch %d\n", ((intfn)Mem)());
  }

  // 3. The same, flushed with the instruction itself.
  EmitConst(Mem, 21);
  FlushLine(Mem);
  printf("ivau %d\n", ((intfn)Mem)());

  // 4. A stub that rewrites and flushes its own line, then runs the result.
  //    Run twice: the first call compiles it, the second has to invalidate a
  //    translation that is live at the moment IC IVAU executes.
  unsigned char* Stub = Mem + 128; // 64-byte aligned inside a page-aligned map
  const size_t StubBytes = sizeof SelfModStub;
  memcpy(Stub, SelfModStub, StubBytes);
  __builtin___clear_cache((char*)Stub, (char*)Stub + StubBytes);
  printf("selfmod %d\n", ((argfn)Stub)(Stub));
  printf("selfmod %d\n", ((argfn)Stub)(Stub));

  // 5. W^X: no PROT_WRITE while executable, which is how SpiderMonkey, V8 and
  //    JSC run. The flip itself synchronises nothing -- the flush does.
  unsigned char* WX = mmap(NULL, (size_t)Page, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (WX == MAP_FAILED) {
    puts("mmap failed");
    return 1;
  }
  for (unsigned i = 31; i <= 33; ++i) {
    if (mprotect(WX, (size_t)Page, PROT_READ | PROT_WRITE) != 0) {
      puts("mprotect failed");
      return 1;
    }
    EmitConst(WX, i);
    __builtin___clear_cache((char*)WX, (char*)WX + 8);
    if (mprotect(WX, (size_t)Page, PROT_READ | PROT_EXEC) != 0) {
      puts("mprotect failed");
      return 1;
    }
    printf("wx %d\n", ((intfn)WX)());
  }

  // 6. A function in the program's own .text, patched after it has run.
  printf("text %d\n", TextTarget());
  uintptr_t TextBase = (uintptr_t)(void*)TextTarget & ~(uintptr_t)(Page - 1);
  if (mprotect((void*)TextBase, 2 * (size_t)Page, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    puts("mprotect failed");
    return 1;
  }
  uint32_t NewRet = 0x52800080u; // movz w0, #4
  memcpy((void*)TextTarget, &NewRet, sizeof NewRet);
  __builtin___clear_cache((char*)(void*)TextTarget, (char*)(void*)TextTarget + 4);
  if (mprotect((void*)TextBase, 2 * (size_t)Page, PROT_READ | PROT_EXEC) != 0) {
    puts("mprotect failed");
    return 1;
  }
  printf("text %d\n", TextTarget());

  return 0;
}
