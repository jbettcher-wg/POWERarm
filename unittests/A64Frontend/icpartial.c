// A guest that writes two 64-byte lines of code and flushes only one of them.
//
// This is the case SMCChecks=icache is allowed to get "wrong" in the same way
// the hardware does. IC IVAU names ONE line (IminLine = 64 on the Pi, which is
// what CTR_EL0 advertises), so a writer that flushes only the first line has
// announced only the first line. For the second, DDI 0487 B2.2.5 says the PE
// may keep executing the old instruction indefinitely: old OR new is a
// conforming answer, on a Cortex-A76 and here.
//
// So the output is deliberately normalised: the flushed line's value is printed
// (architecturally guaranteed), the unflushed line's is only checked to be one
// of the two legal answers. What must NOT happen -- on either machine -- is a
// crash, a third value, or a failure to pick up the change once the line is
// finally flushed.
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

// Deliberately normalised to one token. Which of the two legal answers a given
// machine gives is a microarchitectural accident on the Pi (is the line still in
// the instruction cache?) and an implementation detail here (POWERarm answers
// "old", because nothing told it the bytes changed; mtrack answers "new",
// because its page protection noticed the store). Printing either would make
// the golden unstable for no gain: what this test is actually asserting is that
// neither machine crashes, invents a third answer, or fails to pick the change
// up once the line IS announced.
static const char* Verdict(int Got, int Old, int New) {
  return (Got == New || Got == Old) ? "old-or-new" : "BAD";
}

int main(void) {
  const long Page = sysconf(_SC_PAGESIZE);
  unsigned char* Mem = mmap(NULL, (size_t)Page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Mem == MAP_FAILED) {
    puts("mmap failed");
    return 1;
  }

  // Two functions, deliberately in different 64-byte lines.
  unsigned char* A = Mem;       // line 0
  unsigned char* B = Mem + 64;  // line 1
  unsigned char* C = Mem + 128; // line 2

  EmitConst(A, 1);
  EmitConst(B, 10);
  EmitConst(C, 100);
  __builtin___clear_cache((char*)Mem, (char*)Mem + 192);

  printf("a %d\n", ((intfn)A)());
  printf("b %d\n", ((intfn)B)());
  printf("c %d\n", ((intfn)C)());

  // Rewrite all three, announce only A's line.
  EmitConst(A, 2);
  EmitConst(B, 20);
  EmitConst(C, 200);
  FlushLine(A);

  // A was announced: the new value is required.
  printf("a %d\n", ((intfn)A)());
  // B and C were not: either answer conforms. Normalised so the golden is
  // stable whichever way the Pi and POWERarm each go.
  printf("b %s\n", Verdict(((intfn)B)(), 10, 20));
  printf("c %s\n", Verdict(((intfn)C)(), 100, 200));

  // Announce the rest. Everything must now be the new value.
  FlushLine(B);
  FlushLine(C);
  printf("a %d\n", ((intfn)A)());
  printf("b %d\n", ((intfn)B)());
  printf("c %d\n", ((intfn)C)());

  // A flush whose range spans several lines: __clear_cache loops IC IVAU per
  // IminLine, so all three are announced by one call.
  EmitConst(A, 3);
  EmitConst(B, 30);
  EmitConst(C, 300);
  __builtin___clear_cache((char*)Mem, (char*)Mem + 192);
  printf("a %d\n", ((intfn)A)());
  printf("b %d\n", ((intfn)B)());
  printf("c %d\n", ((intfn)C)());

  // And a flush of a line that holds no code at all: must be a no-op, not a
  // fault. This is the common case for a JIT flushing fresh memory.
  FlushLine(Mem + 3072);
  puts("empty-line flush survived");

  return 0;
}
