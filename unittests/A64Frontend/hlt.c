// SPDX-License-Identifier: MIT
//
// HLT is undefined at EL0, so every immediate raises SIGILL (ILL_ILLOPC, at the
// HLT's own address) on the Pi. POWERarm claims one immediate, HLT #0x0F3F
// followed by a 32-byte SHA-256, as its guest->host thunk marker (see
// A64Frontend/Decoder.h). That must stay invisible to programs that are not
// POWERarm thunk stubs: other immediates, the neighbouring #0x0F3E, the marker
// with a hash that names no thunk, and a marker whose hash runs off the end of
// executable memory all fault here exactly as they do on hardware.
//
// Each case runs from a small RX page built at run time, so the words after
// the HLT are under the test's control; a SIGILL handler records the signal
// details and jumps back.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf JumpBuffer;
static volatile int CaughtSignal;
static volatile int CaughtCode;
static volatile uintptr_t CaughtAddr;

static void OnSignal(int Signal, siginfo_t* Info, void* Context) {
  (void)Context;
  CaughtSignal = Signal;
  CaughtCode = Info->si_code;
  CaughtAddr = (uintptr_t)Info->si_addr;
  siglongjmp(JumpBuffer, 1);
}

static long PageSize;
static uint8_t* Region;

// Writes Words at Offset in the region and runs from there. Returns 1 if the
// first instruction raised SIGILL with ILL_ILLOPC at its own address.
static int RunsToSigill(size_t Offset, const uint32_t* Words, size_t Count, const char* Name) {
  mprotect(Region, PageSize, PROT_READ | PROT_WRITE);
  memset(Region, 0, PageSize);
  memcpy(Region + Offset, Words, Count * sizeof(uint32_t));
  __builtin___clear_cache((char*)Region, (char*)Region + PageSize);
  mprotect(Region, PageSize, PROT_READ | PROT_EXEC);

  CaughtSignal = 0;
  CaughtCode = 0;
  CaughtAddr = 0;
  if (sigsetjmp(JumpBuffer, 1) == 0) {
    ((void (*)(void))(Region + Offset))();
  }
  const uintptr_t Expected = (uintptr_t)(Region + Offset);
  const int Ok = CaughtSignal == SIGILL && CaughtCode == ILL_ILLOPC && CaughtAddr == Expected;
  printf("%s %s\n", Ok ? "PASS" : "FAIL", Name);
  if (!Ok) {
    printf("  signal %d code %d at +%ld\n", CaughtSignal, CaughtCode, (long)(CaughtAddr - Expected));
  }
  return Ok;
}

static uint32_t Hlt(uint32_t Imm) {
  return 0xd4400000u | (Imm << 5);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);

  struct sigaction Action;
  memset(&Action, 0, sizeof Action);
  Action.sa_sigaction = OnSignal;
  Action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGILL, &Action, NULL);
  sigaction(SIGSEGV, &Action, NULL);

  PageSize = sysconf(_SC_PAGESIZE);
  // Two pages: code in the first, the second left inaccessible.
  Region = mmap(NULL, 2 * PageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Region == MAP_FAILED) {
    printf("FAIL mmap\n");
    return 1;
  }
  mprotect(Region + PageSize, PageSize, PROT_NONE);

  // RET after each HLT: if the HLT were ever a no-op the case returns
  // normally and fails instead of running into the zero words.
  const uint32_t Ret = 0xd65f03c0u;

  static const uint32_t Immediates[] = {0x0, 0x1, 0x0f3e, 0x0f40, 0x3f0f, 0xf03f, 0xffff};
  for (size_t i = 0; i < sizeof(Immediates) / sizeof(Immediates[0]); ++i) {
    const uint32_t Words[] = {Hlt(Immediates[i]), Ret};
    char Name[64];
    snprintf(Name, sizeof Name, "hlt_%#x.sigill", Immediates[i]);
    RunsToSigill(64, Words, 2, Name);
  }

  // The thunk marker with hashes that name no thunk. The 32 bytes after the
  // HLT are the hash; a RET follows them.
  {
    uint32_t Words[1 + 8 + 1];
    Words[0] = Hlt(0x0f3f);
    memset(&Words[1], 0, 32);
    Words[9] = Ret;
    RunsToSigill(64, Words, 10, "hlt_0xf3f.zero_hash.sigill");
    for (int i = 0; i < 8; ++i) {
      Words[1 + i] = 0x9e3779b9u * (uint32_t)(i + 1);
    }
    RunsToSigill(64, Words, 10, "hlt_0xf3f.unknown_hash.sigill");
  }

  // The marker in the last word of executable memory: its hash would lie in
  // the inaccessible page. Still SIGILL at the HLT, not SIGSEGV.
  {
    const uint32_t Words[] = {Hlt(0x0f3f)};
    RunsToSigill(PageSize - 4, Words, 1, "hlt_0xf3f.at_page_end.sigill");
  }
  // And with only part of the hash readable.
  {
    const uint32_t Words[] = {Hlt(0x0f3f), 0x11111111u, 0x22222222u};
    RunsToSigill(PageSize - 12, Words, 3, "hlt_0xf3f.hash_crosses_page.sigill");
  }

  // A HLT reached by falling through from ordinary code, not as a block entry.
  {
    const uint32_t Words[] = {0xd2800020u /* mov x0, #1 */, 0x91000400u /* add x0, x0, #1 */, Hlt(0x0f3f), Ret};
    mprotect(Region, PageSize, PROT_READ | PROT_WRITE);
    memset(Region, 0, PageSize);
    memcpy(Region + 64, Words, sizeof Words); // the "hash" is the RET and zeros
    __builtin___clear_cache((char*)Region, (char*)Region + PageSize);
    mprotect(Region, PageSize, PROT_READ | PROT_EXEC);
    CaughtSignal = 0;
    CaughtAddr = 0;
    if (sigsetjmp(JumpBuffer, 1) == 0) {
      ((void (*)(void))(Region + 64))();
    }
    const int Ok = CaughtSignal == SIGILL && CaughtAddr == (uintptr_t)(Region + 64 + 8);
    printf("%s hlt_0xf3f.mid_block.sigill\n", Ok ? "PASS" : "FAIL");
  }

  return 0;
}
