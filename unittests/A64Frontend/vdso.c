// SPDX-License-Identifier: MIT
//
// The vDSO an arm64 guest is handed, and the clocks behind it.
//
// POWERarm maps its own guest vDSO (ThunkLibs/libVDSO) and passes it as
// AT_SYSINFO_EHDR; its clock entry points are guest->host thunks. Against the
// Pi's kernel vDSO this checks that the guest sees the same shape -- the ELF,
// the four __kernel_* symbols under LINUX_2.6.39, a signal handler returning
// into __kernel_rt_sigreturn -- and that the clocks read through it behave:
// every CLOCK_* id glibc can serve from a vDSO, clock_getres, gettimeofday and
// time, monotonic, and in agreement with the same clock read by raw syscall.
//
// Self-checking and machine-independent, like cntvct.c: it prints PASS/FAIL
// lines only, never a time, a resolution or an address.
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void Check(const char* Name, int Ok) {
  printf("%s %s\n", Ok ? "PASS" : "FAIL", Name);
}

static void Check2(const char* Prefix, const char* Name, int Ok) {
  printf("%s %s.%s\n", Ok ? "PASS" : "FAIL", Prefix, Name);
}

// --- The vDSO image ----------------------------------------------------------

struct VDSOSymbol {
  const char* Name;
  uintptr_t Address;
  int IsFunc;
  const char* Version;
};

static const Elf64_Ehdr* Header;
static const Elf64_Sym* SymTab;
static const char* StrTab;
static const Elf64_Half* VerSym;
static const Elf64_Verdef* VerDef;
static uint32_t SymCount;

static int ParseDynamic(uintptr_t Base) {
  Header = (const Elf64_Ehdr*)Base;
  const Elf64_Phdr* Phdrs = (const Elf64_Phdr*)(Base + Header->e_phoff);
  const Elf64_Dyn* Dynamic = NULL;
  uintptr_t LoadBias = Base;
  for (int i = 0; i < Header->e_phnum; ++i) {
    if (Phdrs[i].p_type == PT_DYNAMIC) {
      Dynamic = (const Elf64_Dyn*)(Base + Phdrs[i].p_offset);
    }
    if (Phdrs[i].p_type == PT_LOAD && Phdrs[i].p_offset == 0) {
      LoadBias = Base - Phdrs[i].p_vaddr;
    }
  }
  if (!Dynamic) {
    return 0;
  }
  const uint32_t* Hash = NULL;
  const uint32_t* GnuHash = NULL;
  for (const Elf64_Dyn* D = Dynamic; D->d_tag != DT_NULL; ++D) {
    switch (D->d_tag) {
    case DT_SYMTAB: SymTab = (const Elf64_Sym*)(LoadBias + D->d_un.d_ptr); break;
    case DT_STRTAB: StrTab = (const char*)(LoadBias + D->d_un.d_ptr); break;
    case DT_HASH: Hash = (const uint32_t*)(LoadBias + D->d_un.d_ptr); break;
    case DT_GNU_HASH: GnuHash = (const uint32_t*)(LoadBias + D->d_un.d_ptr); break;
    case DT_VERSYM: VerSym = (const Elf64_Half*)(LoadBias + D->d_un.d_ptr); break;
    case DT_VERDEF: VerDef = (const Elf64_Verdef*)(LoadBias + D->d_un.d_ptr); break;
    }
  }
  if (!SymTab || !StrTab || (!Hash && !GnuHash)) {
    return 0;
  }
  if (Hash) {
    SymCount = Hash[1]; // nchain: one entry per symbol.
    return 1;
  }
  // DT_GNU_HASH only (the arm64 kernel's vDSO): the symbol count is one past
  // the highest index any bucket chain reaches.
  const uint32_t NBuckets = GnuHash[0], SymOffset = GnuHash[1], BloomSize = GnuHash[2];
  const uint32_t* Buckets = GnuHash + 4 + BloomSize * 2; // 64-bit bloom words
  const uint32_t* Chains = Buckets + NBuckets;
  uint32_t Last = 0;
  for (uint32_t i = 0; i < NBuckets; ++i) {
    if (Buckets[i] > Last) {
      Last = Buckets[i];
    }
  }
  if (Last < SymOffset) {
    SymCount = SymOffset;
    return 1;
  }
  while (!(Chains[Last - SymOffset] & 1)) {
    ++Last;
  }
  SymCount = Last + 1;
  return 1;
}

static const char* VersionName(Elf64_Half Index) {
  Index &= 0x7fff;
  for (const Elf64_Verdef* D = VerDef; D; D = D->vd_next ? (const Elf64_Verdef*)((const char*)D + D->vd_next) : NULL) {
    if (D->vd_ndx == Index) {
      const Elf64_Verdaux* Aux = (const Elf64_Verdaux*)((const char*)D + D->vd_aux);
      return StrTab + Aux->vda_name;
    }
  }
  return "";
}

static int FindSymbol(uintptr_t Base, struct VDSOSymbol* Sym) {
  uintptr_t LoadBias = Base;
  const Elf64_Phdr* Phdrs = (const Elf64_Phdr*)(Base + Header->e_phoff);
  for (int i = 0; i < Header->e_phnum; ++i) {
    if (Phdrs[i].p_type == PT_LOAD && Phdrs[i].p_offset == 0) {
      LoadBias = Base - Phdrs[i].p_vaddr;
    }
  }
  for (uint32_t i = 0; i < SymCount; ++i) {
    const Elf64_Sym* S = &SymTab[i];
    if (S->st_shndx == SHN_UNDEF || strcmp(StrTab + S->st_name, Sym->Name) != 0) {
      continue;
    }
    Sym->Address = LoadBias + S->st_value;
    Sym->IsFunc = ELF64_ST_TYPE(S->st_info) == STT_FUNC;
    Sym->Version = VerSym && VerDef ? VersionName(VerSym[i]) : "";
    return 1;
  }
  return 0;
}

// --- Signal return -----------------------------------------------------------

static volatile uintptr_t HandlerReturnAddress;
static void __attribute__((noinline)) OnSigusr1(int Signal) {
  (void)Signal;
  HandlerReturnAddress = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
}

// --- Clocks ------------------------------------------------------------------

static int64_t Nanoseconds(const struct timespec* T) {
  return (int64_t)T->tv_sec * 1000000000LL + T->tv_nsec;
}

static int RawClockGetTime(clockid_t Clock, struct timespec* T) {
  return (int)syscall(SYS_clock_gettime, Clock, T);
}

struct ClockCase {
  const char* Name;
  clockid_t Id;
};

static const struct ClockCase Clocks[] = {
  {"realtime", CLOCK_REALTIME},
  {"monotonic", CLOCK_MONOTONIC},
  {"process_cputime", CLOCK_PROCESS_CPUTIME_ID},
  {"thread_cputime", CLOCK_THREAD_CPUTIME_ID},
  {"monotonic_raw", CLOCK_MONOTONIC_RAW},
  {"realtime_coarse", CLOCK_REALTIME_COARSE},
  {"monotonic_coarse", CLOCK_MONOTONIC_COARSE},
  {"boottime", CLOCK_BOOTTIME},
  {"tai", CLOCK_TAI},
};

static void CheckClock(const struct ClockCase* C) {
  struct timespec T = {-1, -1};
  Check2(C->Name, "gettime", clock_gettime(C->Id, &T) == 0);
  Check2(C->Name, "nsec_in_range", T.tv_nsec >= 0 && T.tv_nsec < 1000000000L);

  struct timespec Res = {-1, -1};
  Check2(C->Name, "getres", clock_getres(C->Id, &Res) == 0);
  Check2(C->Name, "res_sane", Res.tv_sec == 0 && Res.tv_nsec > 0);
  Check2(C->Name, "getres_null", clock_getres(C->Id, NULL) == 0);

  // Monotonic over repeated reads, interleaved with the raw syscall: every
  // read, whichever path served it, is at or after the one before.
  int Ordered = 1;
  int RawOk = 1;
  struct timespec Previous;
  clock_gettime(C->Id, &Previous);
  for (int i = 0; i < 2000; ++i) {
    struct timespec Now;
    int Result = (i & 1) ? RawClockGetTime(C->Id, &Now) : clock_gettime(C->Id, &Now);
    if (Result != 0) {
      RawOk = 0;
      break;
    }
    if (Nanoseconds(&Now) < Nanoseconds(&Previous)) {
      Ordered = 0;
    }
    Previous = Now;
  }
  Check2(C->Name, "raw_syscall", RawOk);
  Check2(C->Name, "monotonic_with_raw", Ordered);

  struct timespec RawRes = {-1, -1};
  Check2(C->Name, "getres_matches_raw",
         syscall(SYS_clock_getres, C->Id, &RawRes) == 0 && RawRes.tv_sec == Res.tv_sec && RawRes.tv_nsec == Res.tv_nsec);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);

  const uintptr_t Base = getauxval(AT_SYSINFO_EHDR);
  Check("auxv.sysinfo_ehdr", Base != 0);
  if (!Base) {
    return 1;
  }
  Check("auxv.sysinfo_ehdr_page_aligned", (Base & 4095) == 0);

  const Elf64_Ehdr* H = (const Elf64_Ehdr*)Base;
  Check("elf.magic", memcmp(H->e_ident, ELFMAG, SELFMAG) == 0);
  Check("elf.class64", H->e_ident[EI_CLASS] == ELFCLASS64);
  Check("elf.little_endian", H->e_ident[EI_DATA] == ELFDATA2LSB);
  Check("elf.dyn", H->e_type == ET_DYN);
  Check("elf.aarch64", H->e_machine == EM_AARCH64);
  const int Parsed = ParseDynamic(Base);
  Check("elf.dynamic_symbols", Parsed);
  if (!Parsed) {
    return 1;
  }

  static const char* const Names[] = {"__kernel_rt_sigreturn", "__kernel_gettimeofday", "__kernel_clock_gettime", "__kernel_clock_getres"};
  struct VDSOSymbol Sigreturn = {0};
  for (size_t i = 0; i < sizeof(Names) / sizeof(Names[0]); ++i) {
    struct VDSOSymbol Sym = {.Name = Names[i]};
    const int Found = FindSymbol(Base, &Sym);
    Check2(Names[i], "present", Found);
    if (i != 0) {
      // The kernel's __kernel_rt_sigreturn is a code label (STT_NOTYPE).
      Check2(Names[i], "func", Found && Sym.IsFunc);
    }
    Check2(Names[i], "version_LINUX_2.6.39", Found && strcmp(Sym.Version, "LINUX_2.6.39") == 0);
    if (i == 0 && Found) {
      Sigreturn = Sym;
    }
  }

  // Signal return. arm64 glibc leaves sa_restorer unset, so the handler
  // returns to wherever the kernel (here: POWERarm) pointed X30, which must be
  // the vDSO's __kernel_rt_sigreturn. Unwinders recognise a signal frame by
  // the two instructions there.
  struct sigaction Action;
  memset(&Action, 0, sizeof Action);
  Action.sa_handler = OnSigusr1;
  sigaction(SIGUSR1, &Action, NULL);
  raise(SIGUSR1);
  Check("sigreturn.handler_returned", HandlerReturnAddress != 0);
  Check("sigreturn.into_vdso", Sigreturn.Address != 0 && HandlerReturnAddress == Sigreturn.Address);
  if (Sigreturn.Address) {
    const uint32_t* Code = (const uint32_t*)Sigreturn.Address;
    Check("sigreturn.mov_x8_139", Code[0] == 0xd2801168u);
    Check("sigreturn.svc_0", Code[1] == 0xd4000001u);
    Check("sigreturn.nop_before", Code[-1] == 0xd503201fu);
  }

  for (size_t i = 0; i < sizeof(Clocks) / sizeof(Clocks[0]); ++i) {
    CheckClock(&Clocks[i]);
  }

  // Invalid clock ids fail the same way through the vDSO and the syscall.
  static const clockid_t BadClocks[] = {12 /* CLOCK_SGI_CYCLE, removed */, 100};
  for (size_t i = 0; i < sizeof(BadClocks) / sizeof(BadClocks[0]); ++i) {
    struct timespec T;
    errno = 0;
    const int Result = clock_gettime(BadClocks[i], &T);
    const int Errno = errno;
    errno = 0;
    const int RawResult = RawClockGetTime(BadClocks[i], &T);
    const int RawErrno = errno;
    char Name[64];
    snprintf(Name, sizeof Name, "badclock_%d.gettime_einval", (int)BadClocks[i]);
    Check(Name, Result == -1 && Errno == EINVAL);
    snprintf(Name, sizeof Name, "badclock_%d.raw_einval", (int)BadClocks[i]);
    Check(Name, RawResult == -1 && RawErrno == EINVAL);
    errno = 0;
    snprintf(Name, sizeof Name, "badclock_%d.getres_einval", (int)BadClocks[i]);
    Check(Name, clock_getres(BadClocks[i], &T) == -1 && errno == EINVAL);
  }

  // gettimeofday sits between two CLOCK_REALTIME reads (truncated to
  // microseconds), through the vDSO and through the raw syscall.
  {
    int Ok = 1, RawOk = 1, UsecOk = 1;
    for (int i = 0; i < 1000; ++i) {
      struct timespec Before, After;
      struct timeval TV, RawTV;
      clock_gettime(CLOCK_REALTIME, &Before);
      const int Result = gettimeofday(&TV, NULL);
      const int RawResult = (int)syscall(SYS_gettimeofday, &RawTV, NULL);
      clock_gettime(CLOCK_REALTIME, &After);
      const int64_t Lo = Nanoseconds(&Before) / 1000, Hi = Nanoseconds(&After) / 1000;
      const int64_t Us = (int64_t)TV.tv_sec * 1000000 + TV.tv_usec;
      const int64_t RawUs = (int64_t)RawTV.tv_sec * 1000000 + RawTV.tv_usec;
      if (Result != 0 || Us < Lo || Us > Hi) {
        Ok = 0;
      }
      if (RawResult != 0 || RawUs < Us || RawUs > Hi) {
        RawOk = 0;
      }
      if (TV.tv_usec < 0 || TV.tv_usec >= 1000000) {
        UsecOk = 0;
      }
    }
    Check("gettimeofday.between_realtime_reads", Ok);
    Check("gettimeofday.raw_agrees", RawOk);
    Check("gettimeofday.usec_in_range", UsecOk);

    struct timezone TZ = {-12345, -12345};
    struct timeval TV;
    Check("gettimeofday.with_tz", gettimeofday(&TV, &TZ) == 0 && TZ.tz_minuteswest != -12345);
    Check("gettimeofday.null_tv", gettimeofday(NULL, NULL) == 0);
  }

  // time() is served from the coarse realtime clock: at or after the coarse
  // clock read before it, and no later than the precise one read after it.
  {
    struct timespec Before, After;
    clock_gettime(CLOCK_REALTIME_COARSE, &Before);
    time_t Out = 0;
    const time_t Now = time(&Out);
    clock_gettime(CLOCK_REALTIME, &After);
    Check("time.between_reads", Now >= Before.tv_sec && Now <= After.tv_sec);
    Check("time.stores_result", Out == Now);
  }

  return 0;
}
