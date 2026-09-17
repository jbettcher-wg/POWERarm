// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|common
desc: Handles host -> host and host -> guest signal routing, emulates procmask & co
$end_info$
*/

#include "LinuxSyscalls/SignalDelegator.h"
#include "LinuxSyscalls/Arm64/GeneratedABI.h"
#include "LinuxSyscalls/Syscalls.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include "ArchHelpers/UContext.h"
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/THP.h>
#include <FEXCore/Utils/CompilerDefs.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/Utils/FPState.h>
#include <FEXCore/Utils/Profiler.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#ifdef ARCHITECTURE_ppc64le
#include <FEXCore/Utils/ArchHelpers/PPC64.h>
#endif
#include <FEXHeaderUtils/Syscalls.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <functional>
#include <linux/futex.h>
#include <syscall.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>

// For older build environments
#ifndef SS_AUTODISARM
#define SS_AUTODISARM (1U << 31)
#endif

namespace FEX::HLE {
#ifdef ARCHITECTURE_x86_64
__attribute__((naked)) static void sigrestore() {
  __asm volatile("syscall;" ::"a"(0xF) : "memory");
}
#endif

// FEX_SIGTRACE=1: raw write() tracing of the signal delivery/defer/drain/
// sigreturn flow. Diagnostic-only; snprintf in signal context matches the
// existing FEX_ABORT_TRIPWIRE precedent.
static bool SigTraceEnabled() {
  static const bool on = getenv("FEX_SIGTRACE") != nullptr;
  return on;
}
#define SIGTRACE(fmt, ...) \
  do { \
    if (SigTraceEnabled()) { \
      char _stbuf[256]; \
      int _stn = snprintf(_stbuf, sizeof(_stbuf), "[ST %d] " fmt "\n", FHU::Syscalls::gettid(), ##__VA_ARGS__); \
      [[maybe_unused]] auto _stw = write(2, _stbuf, _stn); \
    } \
  } while (0)

static FEX::HLE::ThreadStateObject* GetThreadFromAltStack(const stack_t& alt_stack) {
  // The thread object lives just before the alt-stack begin. If the alt-stack
  // is disabled or has no base (signal arrived during thread teardown after
  // sigaltstack(SS_DISABLE) in UninstallTLSState), there is no valid thread
  // pointer to read. Return nullptr so the caller can chain the default
  // handler -- the original fault is then preserved in a clean coredump
  // instead of being clobbered by a recovery-path double-fault.
  if ((alt_stack.ss_flags & SS_DISABLE) || alt_stack.ss_sp == nullptr) {
    return nullptr;
  }
  FEX::HLE::ThreadStateObject* ThreadObject {};
  memcpy(&ThreadObject, reinterpret_cast<void*>(reinterpret_cast<uint64_t>(alt_stack.ss_sp) - 8), sizeof(void*));
  return ThreadObject;
}

// 2026-05-14 diagnostic: capture host PC + si_addr for every sync fault
// (SIGSEGV/SIGBUS/SIGILL/SIGFPE) BEFORE FEX hands it to the guest.  Steam
// installs breakpad which re-raises via tgkill, destroying the original
// si_code and clobbering host registers -- so the coredump shows post-
// breakpad state instead of the actual fault site.  Logging via raw
// write() to /tmp/fex_signal_trace.log is async-signal-safe.  Set
// FEX_TRACE_SIGNALS=1 to enable.  No-op otherwise; one atomic-load fast path.
// On the first fatal-signal hit, copy /proc/self/maps to /tmp/fex_maps.log so
// any LR / nip we log can be matched to a loaded library after the fact.
[[gnu::cold]]
static void DumpMapsOnce() {
  static int done = 0;
  if (done) return;
  done = 1;
  int in = ::open("/proc/self/maps", O_RDONLY);
  if (in < 0) return;
  int out = ::open("/tmp/fex_maps.log",
                    O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) { ::close(in); return; }
  char buf[4096];
  ssize_t n;
  while ((n = ::read(in, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < n) {
      ssize_t w = ::write(out, buf + off, n - off);
      if (w <= 0) break;
      off += w;
    }
  }
  ::close(in);
  ::close(out);
}

[[gnu::cold]]
static void TraceSyncSignal(int Signal, siginfo_t* Info, ucontext_t* _context) {
  static int trace_fd = -2;
  if (trace_fd == -2) {
    if (getenv("FEX_TRACE_SIGNALS")) {
      trace_fd = ::open("/tmp/fex_signal_trace.log",
                        O_WRONLY | O_CREAT | O_APPEND, 0644);
      DumpMapsOnce();
    } else {
      trace_fd = -1;
    }
  }
  if (trace_fd < 0) return;
  // Build line manually (no fprintf -- not async-signal-safe).
  // Worst case with every field present is ~300 bytes (11 hex fields at up to
  // 18 chars each plus labels); 512 leaves headroom.
  char buf[512];
  auto write_hex = [](char* dst, uint64_t v) -> int {
    int n = 0;
    char tmp[18];
    if (v == 0) { tmp[n++] = '0'; }
    while (v) { int d = v & 0xf; tmp[n++] = (d < 10 ? '0'+d : 'a'+d-10); v >>= 4; }
    int len = 0;
    dst[len++] = '0'; dst[len++] = 'x';
    while (n > 0) dst[len++] = tmp[--n];
    return len;
  };
  int len = 0;
  const char* prefix = "FEX-SIG tid=";
  for (const char* p = prefix; *p; p++) buf[len++] = *p;
  len += write_hex(buf + len, (uint64_t)::syscall(SYS_gettid));
  const char* sig = " sig="; for (const char* p = sig; *p; p++) buf[len++] = *p;
  len += write_hex(buf + len, (uint64_t)Signal);
  const char* code = " code="; for (const char* p = code; *p; p++) buf[len++] = *p;
  len += write_hex(buf + len, (uint64_t)Info->si_code);
  const char* addr = " addr="; for (const char* p = addr; *p; p++) buf[len++] = *p;
  len += write_hex(buf + len, (uint64_t)Info->si_addr);
  const char* nip = " nip="; for (const char* p = nip; *p; p++) buf[len++] = *p;
#ifdef ARCHITECTURE_ppc64le
  len += write_hex(buf + len, (uint64_t)_context->uc_mcontext.regs->nip);
  const char* r27 = " r27="; for (const char* p = r27; *p; p++) buf[len++] = *p;
  len += write_hex(buf + len, (uint64_t)_context->uc_mcontext.regs->gpr[27]);
  const char* r3 = " r3="; for (const char* p = r3; *p; p++) buf[len++] = *p;
  len += write_hex(buf + len, (uint64_t)_context->uc_mcontext.regs->gpr[3]);
  const char* lr = " lr="; for (const char* p = lr; *p; p++) buf[len++] = *p;
  len += write_hex(buf + len, (uint64_t)_context->uc_mcontext.regs->link);
  // Also r11 (guest RSP per FEX SRA mapping on PPC64LE).
  const char* r11 = " r11="; for (const char* p = r11; *p; p++) buf[len++] = *p;
  len += write_hex(buf + len, (uint64_t)_context->uc_mcontext.regs->gpr[11]);
#endif
  // Reconstructed GUEST RIP.  `nip=` above is the HOST program counter -- on a
  // fault taken inside JIT code it points into a code buffer that was mmap'd
  // after DumpMapsOnce() ran, so it resolves against nothing in
  // /tmp/fex_maps.log and it is NOT the guest instruction that faulted.  That
  // trap is documented in docs/POWER9_PORT_PLAN.md.  RestoreRIPFromHostPC()
  // maps the host PC back to the guest RIP via the block's inline RIP table.
  //
  // IMPORTANT: RestoreRIPFromHostPC() has no failure return.  When the host PC
  // is not inside the current JIT block it silently returns Frame->State.pc
  // (FEXCore/Source/Interface/Core/Core.cpp), which is a stale block-boundary
  // value, not the fault site -- and printing that unqualified would be
  // actively misleading here, since a stale zero would read as a null guest
  // RIP in exactly the null-deref bug being chased.  So the call is gated on
  // IsAddressInCodeBuffer(), the same guard SyscallHandler::
  // DetectMonoBackpatcherBlock (SyscallsSMCTracking.cpp) puts in front of this
  // exact call, and the field is printed as <none> when the guard fails.
  //
  // blk_rip= is the guest RIP of the ENTRY of the block containing the host PC
  // (GetGuestBlockEntry, read straight out of the JITCodeTail).  Coarser than
  // guest_rip=, but it does not depend on the per-instruction vl64pair table,
  // so trust it if the two disagree.  state_rip= is the raw Frame->State.pc and must ALWAYS be
  // read as "possibly stale" -- it is the value guest_rip= would have silently
  // degraded to on the fallback path.
  //
  // InJITCode is hoisted out of the block below because the GUEST GPR dump
  // (second trace line, PPC64LE only) is gated on the exact same condition:
  // the static register allocation only holds guest state while host execution
  // is inside a JIT code buffer.
  [[maybe_unused]] bool InJITCode = false;
#if defined(ARCHITECTURE_arm64) || defined(ARCHITECTURE_ppc64le)
  {
    // Re-entrancy guard.  Everything below dereferences JIT-owned memory
    // (JITCodeHeader/JITCodeTail and the vl64pair RIP table) reached through
    // the per-CodeBuffer block index -- CPUBackend::FindBlockHeader, keyed on
    // the host PC.  Audit P1 removed the InlineJITBlockHeader store this used
    // to go through, so a *stale* pointer is no longer the hazard it was; the
    // index only ever names fully-written blocks in buffers this thread still
    // holds a reference to.  The guard stays anyway: the block bytes
    // themselves are JIT-owned and can be mid-invalidation, and if any deref
    // here faults it does so *inside* this handler, re-enters
    // SignalHandlerThunk, and loops until the alt stack overflows -- which
    // would destroy the very trace we came here for.  A nested entry skips
    // reconstruction and prints <none>.
    static volatile sig_atomic_t InReconstruct = 0;
    uint64_t GuestRIP = 0, BlockRIP = 0, StateRIP = 0;
    bool HaveGuestRIP = false, HaveBlockRIP = false, HaveStateRIP = false;

    auto* ThreadObject = GetThreadFromAltStack(_context->uc_stack);
    if (ThreadObject && !InReconstruct) {
      // Same zombie-ThreadStateObject guard SignalHandlerThunk applies below:
      // after ThreadManager::DestroyThread sets ThreadInfo.IsZombie, the
      // slab is leaked and ->Thread must not be dereferenced.
      const uint32_t TraceHostTid = FHU::Syscalls::gettid();
      const bool ObjIsZombie = ThreadObject->ThreadInfo.IsZombie.load(std::memory_order_acquire);
      const uint32_t ObjTid = ThreadObject->ThreadInfo.TID.load(std::memory_order_relaxed);
      auto* Thread = ThreadObject->Thread;
      if (Thread && !ObjIsZombie && ObjTid == TraceHostTid) {
        InReconstruct = 1;
        const uint64_t HostPC = ArchHelpers::Context::GetPc(_context);
        StateRIP = Thread->CurrentFrame->State.pc;
        HaveStateRIP = true;
        if (Thread->CTX->IsAddressInCodeBuffer(Thread, HostPC)) {
          InJITCode = true;
          GuestRIP = Thread->CTX->RestoreRIPFromHostPC(Thread, HostPC);
          HaveGuestRIP = true;
          BlockRIP = Thread->CTX->GetGuestBlockEntry(Thread, HostPC);
          HaveBlockRIP = BlockRIP != 0;
        }
        InReconstruct = 0;
      }
    }

    auto write_field = [&](const char* Name, bool Have, uint64_t Value) {
      for (const char* p = Name; *p; p++) {
        buf[len++] = *p;
      }
      if (Have) {
        len += write_hex(buf + len, Value);
      } else {
        for (const char* p = "<none>"; *p; p++) {
          buf[len++] = *p;
        }
      }
    };
    write_field(" guest_rip=", HaveGuestRIP, GuestRIP);
    write_field(" blk_rip=", HaveBlockRIP, BlockRIP);
    write_field(" state_rip=", HaveStateRIP, StateRIP);
  }
#endif
  buf[len++] = '\n';
  ::write(trace_fd, buf, len);

#ifdef ARCHITECTURE_ppc64le
  // ---------------------------------------------------------------------
  // Second trace line: the GUEST general-purpose register file.
  //
  // A second line rather than more fields on the first: 16 labelled 64-bit
  // hex fields is ~380 bytes on its own, and buf[512] above already carries
  // ~300 bytes worst case.  Separate buffer, separate write(), so neither
  // line can be truncated by the other.
  //
  // Emitted ONLY when the fault was taken inside a JIT code buffer.  The
  // guest register file lives in host registers under FEX's static register
  // allocation (SRA) while JIT code is executing; anywhere else those host
  // registers hold whatever FEXCore's own C++ left in them, and printing
  // that would look exactly like a plausible guest state.  Same
  // IsAddressInCodeBuffer() gate as guest_rip= above.  When the gate fails
  // an explicit marker is printed so "no register line" is never confused
  // with "this build does not have the patch".
  //
  // MAPPING SOURCE (duplicated by necessity, see below):
  //   FEXCore/Source/Interface/Core/ArchHelpers/PPC64Emitter.h a64::SRA, whose
  //   slot i holds FEXCore::Core::StaticGPRGuestReg[i] (CoreState.h) -- see
  //   PPC64EmitterBase::SpillStaticRegs/FillStaticRegs in PPC64Emitter.cpp.
  //   Host r13 is skipped; it is the ELFv2 thread pointer.
  //
  // WHY DUPLICATED RATHER THAN DERIVED: PPC64Emitter.h pulls in the external
  // emitter headers, which are not on LinuxEmulation's include path, so
  // a64::SRA is unreachable from this TU. If that array changes, this table
  // must change with it.
  //
  // Names are printed with a leading '%' (%x0=, %sp=) to make it unmistakable
  // that these are GUEST AArch64 registers and not the HOST PPC64 registers
  // printed on the first line.
  {
    struct GuestGPRMap {
      const char* Name;
      uint8_t HostGPR;
    };
    // Mirrors FEXCore::Core::StaticGPRGuestReg and a64::SRA (PPC64Emitter.h).
    static constexpr GuestGPRMap Map[] = {
      {" %x0=", 7},   {" %x1=", 8},   {" %x2=", 9},   {" %x3=", 10},  {" %x4=", 11},  {" %x5=", 12},
      {" %x6=", 14},  {" %x7=", 15},  {" %x8=", 16},  {" %x19=", 17}, {" %x20=", 18}, {" %x21=", 19},
      {" %x22=", 20}, {" %x23=", 21}, {" %x24=", 22}, {" %x29=", 23}, {" %x30=", 28}, {" %sp=", 29},
    };

    char gbuf[640];
    int glen = 0;
    auto put = [&](const char* s) {
      for (const char* p = s; *p; p++) {
        gbuf[glen++] = *p;
      }
    };
    put("FEX-SIG-GUEST tid=");
    glen += write_hex(gbuf + glen, (uint64_t)::syscall(SYS_gettid));

    if (!InJITCode) {
      put(" <not-in-jit-code:sra-does-not-hold-guest-state>");
    } else {
      for (const auto& Entry : Map) {
        put(Entry.Name);
        glen += write_hex(gbuf + glen, (uint64_t)_context->uc_mcontext.regs->gpr[Entry.HostGPR]);
      }
    }
    gbuf[glen++] = '\n';
    ::write(trace_fd, gbuf, glen);
  }
#endif
}

static void SignalHandlerThunk(int Signal, siginfo_t* Info, void* UContext) {
  ucontext_t* _context = (ucontext_t*)UContext;
  // Diagnostic: log the raw kernel-delivered context before any FEX/guest
  // handler runs.  Steam's breakpad clobbers this in the post-mortem
  // coredump via SIGSEGV re-raise (tgkill SI_TKILL).
  if (Signal == SIGSEGV || Signal == SIGBUS || Signal == SIGILL || Signal == SIGFPE) {
    TraceSyncSignal(Signal, Info, _context);
  }
  auto ThreadObject = GetThreadFromAltStack(_context->uc_stack);
  if (!ThreadObject) {
    // No valid alt-stack: cannot dispatch this signal through FEX. Restore
    // the default disposition and return -- the kernel will re-deliver on
    // resume, preserving the original fault NIP/siginfo in the coredump.
    struct sigaction sa {};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(Signal, &sa, nullptr);
    return;
  }
  // UAF guard (Steam SteamRT3 teardown race, 2026-05-15): the kernel can
  // deliver an in-flight signal AFTER ThreadManager::DestroyThread has run
  // (which sets ThreadInfo.IsZombie before leaking the slab). If the
  // object is marked zombie or the TID does not match the current kernel
  // TID, the ThreadObject is dead-mail and dereferencing ->Thread /
  // ->SignalInfo crashes. Fall through to default disposition; coredump
  // preserves original siginfo.
  const uint32_t HostTid = FHU::Syscalls::gettid();
  const bool ObjIsZombie = ThreadObject->ThreadInfo.IsZombie.load(std::memory_order_acquire);
  const uint32_t ObjTid = ThreadObject->ThreadInfo.TID.load(std::memory_order_relaxed);
  if (ObjIsZombie || ObjTid != HostTid) {
    struct sigaction sa {};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(Signal, &sa, nullptr);
    return;
  }
  FEXCORE_PROFILE_ACCUMULATION(ThreadObject->Thread, AccumulatedSignalTime);
  ThreadObject->SignalInfo.Delegator->HandleSignal(ThreadObject, Signal, Info, UContext);
}

uint64_t SigIsMember(GuestSAMask* Set, int Signal) {
  // Signal 0 isn't real, so everything is offset by one inside the set
  Signal -= 1;
  return (Set->Val >> Signal) & 1;
}

uint64_t SetSignal(GuestSAMask* Set, int Signal) {
  // Signal 0 isn't real, so everything is offset by one inside the set
  Signal -= 1;
  return Set->Val | (1ULL << Signal);
}

/**
 * @name Signal frame setup
 * @{ */

void SignalDelegator::HandleSignal(FEX::HLE::ThreadStateObject* Thread, int Signal, void* Info, void* UContext) {
  // Let the host take first stab at handling the signal
  if (!Thread) {
    LogMan::Msg::AFmt("Thread {} has received a signal and hasn't registered itself with the delegate! Programming error!",
                      FHU::Syscalls::gettid());
  } else {
    // FEX_SIGFAULTWATCH=1 logs every fault signal on entry, with the guest RIP
    // and faulting address, BEFORE any handler runs.
    //
    // It has to be here rather than further down: FEX's own handlers (SMC
    // tracking) and the frontend handler both return early when they claim a
    // signal, and the frontend is what delivers a fault to the guest — so
    // anything logged after them misses precisely the deliveries of interest.
    //
    // FEX_SIGRIPWATCH cannot answer this either: it lives in SpillSRA, so it
    // only sees signals arriving while the guest is in JIT code with static
    // registers live. A fault taken inside a thunk, or entering/leaving one,
    // never reaches it. Both were silent on a 32-bit Unity title (Dex) that
    // demonstrably takes a SIGSEGV.
    if (Signal == SIGSEGV || Signal == SIGBUS || Signal == SIGILL || Signal == SIGFPE) {
      static const bool FaultWatch = getenv("FEX_SIGFAULTWATCH") != nullptr;
      if (FaultWatch) {
        const auto* SigInfo = static_cast<const siginfo_t*>(Info);
        const auto& St = Thread->Thread->CurrentFrame->State;
        LogMan::Msg::IFmt("SigFaultWatch: tid {} sig {} code {} fault_addr 0x{:x} guest pc 0x{:x} sp 0x{:x} "
                          "x0 0x{:x} x1 0x{:x} x2 0x{:x} x29 0x{:x} x30 0x{:x}",
                          FHU::Syscalls::gettid(), Signal, SigInfo->si_code, reinterpret_cast<uint64_t>(SigInfo->si_addr),
                          St.pc, St.sp, St.x[0], St.x[1], St.x[2], St.x[29], St.x[30]);
      }
    }

    SignalHandler& Handler = HostHandlers[Signal];
    for (auto& HandlerFunc : Handler.Handlers) {
      if (HandlerFunc(Thread->Thread, Signal, Info, UContext)) {
        // If the host handler handled the fault then we can continue now
        return;
      }
    }

    if (Handler.FrontendHandler && Handler.FrontendHandler(Thread->Thread, Signal, Info, UContext)) {
      return;
    }

    // Now let the frontend handle the signal
    // It's clearly a guest signal and this ends up being an OS specific issue
    HandleGuestSignal(Thread, Signal, Info, UContext);
  }
}

void SignalDelegator::RegisterHostSignalHandler(int Signal, HostSignalDelegatorFunction Func, bool Required) {
  SetHostSignalHandler(Signal, std::move(Func), Required);
  FrontendRegisterHostSignalHandler(Signal, Required);
}

void SignalDelegator::SpillSRA(FEXCore::Core::InternalThreadState* Thread, void* ucontext, uint32_t IgnoreMask) {
#if defined(ARCHITECTURE_arm64) || defined(ARCHITECTURE_ppc64le)
  Thread->CurrentFrame->State.pc = CTX->RestoreRIPFromHostPC(Thread, ArchHelpers::Context::GetPc(ucontext));

  for (size_t i = 0; i < Config.SRAGPRCount; i++) {
    const uint8_t SRAIdxMap = Config.SRAGPRMapping[i];
    if (IgnoreMask & (1U << SRAIdxMap)) {
      // Skip this one, it's already spilled
      continue;
    }
    // NOTE: read the ucontext slot DIRECTLY, not via GetArmReg(). GetArmReg()
    // applies a cross-arch ARM-X-name -> PPC-r-reg `+3` shift for callers like
    // SyscallHandler::HandleSegfault that name registers in ARM terms, while
    // SRAGPRMapping[i] is the *host-native* register index.
    const uint32_t GuestReg = FEXCore::Core::StaticGPRGuestReg[i];
    const uint64_t Value = ArchHelpers::Context::GetArmGPRs(ucontext)[SRAIdxMap];
    if (GuestReg == FEXCore::Core::CPUState::SP_INDEX) {
      Thread->CurrentFrame->State.sp = Value;
    } else {
      Thread->CurrentFrame->State.x[GuestReg] = Value;
    }
  }

  // Spill the SRA-mapped host vector registers (guest V0-V15) back into guest State.
  for (size_t i = 0; i < Config.SRAFPRCount; i++) {
    auto FPR = ArchHelpers::Context::GetArmFPR(ucontext, Config.SRAFPRMapping[i]);
    memcpy(&Thread->CurrentFrame->State.v[i][0], &FPR, sizeof(__uint128_t));
  }

  // Guest NZCV lives in CR0 (N, Z) and XER (C, V) while in JIT code.
  Thread->CurrentFrame->State.nzcv = static_cast<uint32_t>(ArchHelpers::Context::GetArmPState(ucontext));

  // Root-cause tripwire for the Ziggurat finalize spin (docs/
  // ZIGGURAT_FINALIZE_SPIN.md): FEX_SIGRIPWATCH=0xBEGIN-0xEND logs every
  // in-JIT signal delivery whose RECONSTRUCTED guest RIP lands in the range,
  // with the host PC and the loop's registers. The suspicion is a resume
  // landing on the wrong instruction boundary so the induction-register init
  // is skipped — this catches the reconstruction in the act, with the
  // register values needed to judge whether they are consistent with the
  // claimed RIP.
  static const auto SigRIPWatch = []() -> std::pair<uint64_t, uint64_t> {
    const char* Env = getenv("FEX_SIGRIPWATCH");
    if (!Env) {
      return {0, 0};
    }
    char* End {};
    const uint64_t Begin = std::strtoull(Env, &End, 0);
    if (*End != '-') {
      return {0, 0};
    }
    return {Begin, std::strtoull(End + 1, nullptr, 0)};
  }();
  if (SigRIPWatch.second) {
    const uint64_t RIP = Thread->CurrentFrame->State.pc;
    if (RIP >= SigRIPWatch.first && RIP < SigRIPWatch.second) {
      const auto& St = Thread->CurrentFrame->State;
      LogMan::Msg::IFmt("SigRIPWatch: tid {} host pc 0x{:x} -> guest pc 0x{:x} x0=0x{:x} x1=0x{:x} sp=0x{:x} x30=0x{:x}",
                        FHU::Syscalls::gettid(), reinterpret_cast<uint64_t>(ArchHelpers::Context::GetPc(ucontext)), RIP,
                        St.x[0], St.x[1], St.sp, St.x[30]);
    }
  }
#endif
}

ArchHelpers::Context::ContextBackup* SignalDelegator::StoreThreadState(FEXCore::Core::InternalThreadState* Thread, int Signal, void* ucontext) {
  // We can end up getting a signal at any point in our host state
  // Jump to a handler that saves all state so we can safely return
  uint64_t OldSP = ArchHelpers::Context::GetSp(ucontext);
  uintptr_t NewSP = OldSP;

  size_t StackOffset = sizeof(ArchHelpers::Context::ContextBackup);

  // We need to back up behind the host's red zone
  // We do this on the guest side as well
  // (does nothing on arm hosts)
  NewSP -= ArchHelpers::Context::ContextBackup::RedZoneSize;

  NewSP -= StackOffset;
  NewSP = FEXCore::AlignDown(NewSP, 16);

  auto Context = reinterpret_cast<ArchHelpers::Context::ContextBackup*>(NewSP);
  ArchHelpers::Context::BackupContext(ucontext, Context);

  // Retain the action pointer so we can see it when we return
  Context->Signal = Signal;

  // Save guest state
  // We can't guarantee if registers are in context or host GPRs
  // So we need to save everything
  memcpy(&Context->GuestState, &Thread->CurrentFrame->State, sizeof(FEXCore::Core::CPUState));

  // Set the new SP
  ArchHelpers::Context::SetSp(ucontext, NewSP);

  Context->Flags = 0;
  Context->FPStateLocation = 0;
  Context->UContextLocation = 0;
  Context->SigInfoLocation = 0;
  Context->InSyscallInfo = 0;

  // Store fault to top status and then reset it
  Context->FaultToTopAndGeneratedException = Thread->CurrentFrame->SynchronousFaultData.FaultToTopAndGeneratedException;
  Thread->CurrentFrame->SynchronousFaultData.FaultToTopAndGeneratedException = false;

  return Context;
}

void SignalDelegator::RestoreThreadState(FEXCore::Core::InternalThreadState* Thread, void* ucontext, RestoreType Type) {
  uint64_t OldSP {};
  if (Type == RestoreType::TYPE_PAUSE) [[unlikely]] {
    OldSP = ArchHelpers::Context::GetSp(ucontext);
  } else {
    // Some fun introspection here.
    // We store a pointer to our host-stack on the guest stack.
    // We need to inspect the guest state coming in, so we can get our host stack back.
    // AArch64: rt_sigreturn is entered with SP at the rt_sigframe that
    // SetupFrame_Arm64 built; the frame record and then the host stack slot
    // sit directly above it.
    uint64_t GuestSP = Thread->CurrentFrame->State.sp;
    GuestSP += sizeof(FEXCore::arm64::rt_sigframe) + sizeof(FEXCore::arm64::frame_record);

    OldSP = *reinterpret_cast<uint64_t*>(GuestSP);
  }

  uintptr_t NewSP = OldSP;
  auto Context = reinterpret_cast<ArchHelpers::Context::ContextBackup*>(NewSP);

#ifdef ARCHITECTURE_ppc64le
  // r10 is included because it is the SRA home of guest RBX: an INJIT resume
  // that reinstates a stale r10 is the current prime suspect for the
  // finalize-spin corruption (docs/ZIGGURAT_FINALIZE_SPIN.md), and this line
  // is the only place that value is visible.
  SIGTRACE("RESTORE type=%d guest_rsp=0x%lx backup=0x%lx nip=0x%lx lr=0x%lx r10=0x%lx flags=0x%x isi=0x%x sig=%d",
           (int)Type, (unsigned long)Thread->CurrentFrame->State.sp, (unsigned long)NewSP,
           (unsigned long)Context->GPRs[32], (unsigned long)Context->GPRs[36], (unsigned long)Context->GPRs[10], Context->Flags,
           (unsigned)Context->InSyscallInfo, Context->Signal);
#endif

  // Restore host state
  ArchHelpers::Context::RestoreContext(ucontext, Context);

  // Reset the guest state
  memcpy(&Thread->CurrentFrame->State, &Context->GuestState, sizeof(FEXCore::Core::CPUState));

  if (Context->UContextLocation) {
    auto Frame = Thread->CurrentFrame;

    if (Context->Flags & ArchHelpers::Context::ContextFlags::CONTEXT_FLAG_INJIT) {
      // 2026-05-15: PPC64LE used to route INJIT signal returns through the
      // dispatcher's FillSRA entry because BackupContext didn't save VMX
      // (V0..V31).  That workaround broke signal-driven inter-thread wakeups
      // (Steam manifest deadlock, Mono/.NET GC busy loops) by forcing every
      // signal return to re-enter the JIT block from its head with state
      // memcpy'd back from memory.  With BackupContext/RestoreContext now
      // saving and restoring VMX state (MContext_ppc64le.h), PPC64LE can
      // resume at the original NIP like ARM64 -- the kernel's RestoreContext
      // path runs, RestoreContext puts V0..V31 back, and execution continues
      // mid-block with the live pre-signal register file.
      Frame->InSyscallInfo = Context->InSyscallInfo;
    } else {
      // Outside-JIT deliveries stash InSyscallInfo too (a thread blocked in a
      // guest syscall like sigsuspend has 0xFFFF set while sitting in host C).
      // Reinstate it with the resumed context so the interrupted syscall op's
      // tail sees the state it left behind; while the handler ran it was 0.
      Frame->InSyscallInfo = Context->InSyscallInfo;
    }

    RestoreFrame_Arm64(Thread, Context, Frame, ucontext);
  }
}

bool SignalDelegator::HandleDispatcherGuestSignal(FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext,
                                                  GuestSigAction* GuestAction, stack_t* GuestStack) {
  auto ContextBackup = StoreThreadState(Thread, Signal, ucontext);

  auto Frame = Thread->CurrentFrame;

  // Ref count our faults
  // We use this to track if it is safe to clear cache
  ++Thread->CurrentFrame->SignalHandlerRefCounter;

  uint64_t OldPC = ArchHelpers::Context::GetPc(ucontext);
  const bool WasInJIT = CTX->IsAddressInCodeBuffer(Thread, OldPC);

  // Spill the SRA regardless of signal handler type
  // We are going to be returning to the top of the dispatcher which will fill again
  // Otherwise we might load garbage
  if (WasInJIT) {
    uint32_t IgnoreMask {};
#if defined(ARCHITECTURE_arm64) || defined(ARCHITECTURE_ppc64le)
    if (Frame->InSyscallInfo != 0) {
      // We are in a syscall, this means we are in a weird register state
      // We need to spill SRA but only some of it, since some values have already been spilled
      // Lower 16 bits tells us which registers are already spilled to the context
      // So we ignore spilling those ones
      IgnoreMask = Frame->InSyscallInfo & 0xFFFF;
    } else {
      // We must spill everything
      IgnoreMask = 0;
    }
#endif

    // We are in jit, SRA must be spilled
    SpillSRA(Thread, ucontext, IgnoreMask);

#if defined(ARCHITECTURE_ppc64le)
    // StoreThreadState captured GuestState BEFORE SpillSRA ran. SpillSRA has
    // now committed the correct x86 state (gregs, rip, xmm) from the actual
    // signal-arrival register file into Thread->CurrentFrame->State. Re-capture
    // so that RestoreThreadState's memcpy(State, GuestState) restores the
    // authoritative pre-signal values rather than a stale one-block-behind copy.
    memcpy(&ContextBackup->GuestState, &Thread->CurrentFrame->State,
           sizeof(FEXCore::Core::CPUState));
#endif

    ContextBackup->Flags |= ArchHelpers::Context::ContextFlags::CONTEXT_FLAG_INJIT;

    // We are leaving the syscall information behind. Make sure to store the previous state.
    ContextBackup->InSyscallInfo = Thread->CurrentFrame->InSyscallInfo;
    Thread->CurrentFrame->InSyscallInfo = 0;
    SIGTRACE("DELIVER sig=%d injit pc=0x%lx rip=0x%lx rsp=0x%lx backup=0x%lx isi=0x%x", Signal, OldPC,
             (unsigned long)Frame->State.pc, (unsigned long)Frame->State.sp,
             (unsigned long)(uintptr_t)ContextBackup, (unsigned)ContextBackup->InSyscallInfo);
  } else {
    // The interrupted context can still be mid-syscall even though the host
    // PC is outside the JIT: DEF_OP(Syscall) sets Frame->InSyscallInfo=0xFFFF
    // (and, since the partial-refill port, DEF_OP(Thunk) and the FABI bridge
    // stubs arm the same field around their host calls — see
    // kInSyscallSentinel in FEXCore ArchHelpers/PPC64Emitter.h)
    // before bctrl'ing into C, so a thread blocked in e.g. sigsuspend carries
    // the in-syscall spill mask while it waits. The guest handler we are about
    // to dispatch runs fresh JIT blocks; if the stale mask is left set, any
    // nested mid-JIT delivery (deferred-signal drain at a poke, another GC
    // suspend) takes SpillSRA's partial-spill path at a boundary that is NOT
    // the syscall window and freezes guest RAX..RDI at stale memory values.
    // That was the Ziggurat "SRA corruption" wedge: Boehm GC's SIGPWR/SIGXCPU
    // storm nests exactly this way (stop handler parked in sigsuspend).
    // Scope it like the InJIT branch does: stash in the backup, clear for the
    // handler, and RestoreThreadState reinstates it with the resumed context.
    ContextBackup->InSyscallInfo = Thread->CurrentFrame->InSyscallInfo;
    Thread->CurrentFrame->InSyscallInfo = 0;
    SIGTRACE("DELIVER sig=%d outside pc=0x%lx indisp=%d rip=0x%lx rsp=0x%lx backup=0x%lx isi=0x%x", Signal, OldPC,
             IsAddressInDispatcher(OldPC) ? 1 : 0, (unsigned long)Frame->State.pc,
             (unsigned long)Frame->State.sp, (unsigned long)(uintptr_t)ContextBackup,
             (unsigned)ContextBackup->InSyscallInfo);
    if (!IsAddressInDispatcher(OldPC)) {
      // This is likely to cause issues but in some cases it isn't fatal
      // This can also happen if we have put a signal on hold, then we just reenabled the signal
      // So we are in the syscall handler
      // Only throw a log message in this case
      if constexpr (false) {
        // XXX: Messages in the signal handler can cause us to crash
        LogMan::Msg::EFmt("Signals in dispatcher have unsynchronized context");
      }
    }
  }

  uint64_t OldGuestSP = Frame->State.sp;

  // Defensive: refuse to lay out a signal frame on a guest SP that is
  // clearly unusable. Threads created via clone() with unsupported flags
  // (FEX has been logging "clone: Unsupported flags w/o CLONE_THREAD"
  // for pressure-vessel's CLONE_PIDFD calls) can end up with a near-NULL
  // RSP. SetupFrame_x64 then decrements NewGuestSP by ucontext_t size and
  // writes to (RSP - sizeof(...))  which wraps to ~0xFFFFFFFFFFFFFE48 and
  // SIGSEGVs. Better to bail out and let the default disposition run.
  // Valid x86_64 user-space addresses are 0x10000..0x7FFFFFFFFFFF; i386
  // is 0x1000..0xFFFFFFFF. The lower bound catches both.
  if (OldGuestSP < 0x10000ULL || OldGuestSP > 0x00007FFFFFFFFFFFULL) {
    // Diagnostic dump: capture as much state as possible to root-cause the
    // bogus-RSP. Print TID, signal info, guest RIP/RBP/segment selectors,
    // and full guest GPR snapshot. Helps distinguish "freshly-cloned thread
    // with uninitialized state" vs "syscall-return-window race" vs other.
    auto& S = Thread->CurrentFrame->State;
    siginfo_t* si = reinterpret_cast<siginfo_t*>(info);
    LogMan::Msg::EFmt("HandleDispatcherGuestSignal: refusing to deliver "
                      "signal {} to guest with bogus SP {:#x}",
                      Signal, OldGuestSP);
    LogMan::Msg::EFmt("  tid={} si_code={} si_addr={:#x} si_pid={}",
                      FHU::Syscalls::gettid(),
                      si->si_code, reinterpret_cast<uint64_t>(si->si_addr), si->si_pid);
    LogMan::Msg::EFmt("  guest PC={:#x} X29={:#x} X30={:#x}", S.pc, S.x[29], S.x[30]);
    LogMan::Msg::EFmt("  X0={:#x} X1={:#x} X2={:#x} X3={:#x}", S.x[0], S.x[1], S.x[2], S.x[3]);
    return false;
  }

  uint64_t NewGuestSP = OldGuestSP;

  // altstack is only used if the signal handler was setup with SA_ONSTACK
  if (GuestAction->sa_flags & SA_ONSTACK) {
    // Additionally the altstack is only used if the enabled (SS_DISABLE flag is not set)
    if (!(GuestStack->ss_flags & SS_DISABLE)) {
      // If our guest is already inside of the alternative stack
      // Then that means we are hitting recursive signals and we need to walk back the stack correctly
      uint64_t AltStackBase = reinterpret_cast<uint64_t>(GuestStack->ss_sp);
      uint64_t AltStackEnd = AltStackBase + GuestStack->ss_size;
      if (OldGuestSP >= AltStackBase && OldGuestSP <= AltStackEnd) {
        // We are already in the alt stack, the rest of the code will handle adjusting this
      } else {
        NewGuestSP = AltStackEnd;
      }
    }
  }

  // siginfo_t
  siginfo_t* HostSigInfo = reinterpret_cast<siginfo_t*>(info);

  ContextBackup->OriginalRIP = Thread->CurrentFrame->State.pc;
  {
    // FEX_LOCKDIAG=1: a guest handler is about to run on top of this host frame.
    // If this thread holds VMATracking's write lock here, that lock is leaked.
    if (FEX::HLE::_SyscallHandler && FEX::HLE::_SyscallHandler->VMATracking.Mutex.WriteHeldBySelfDiag()) [[unlikely]] {
      char Buf[160];
      const int N = ::snprintf(Buf, sizeof(Buf),
                               "FEX: LOCKDIAG guest signal %d (code %d, addr %p, guest rip 0x%llx) delivered while this thread HOLDS the VMA write lock\n",
                               Signal, HostSigInfo->si_code, HostSigInfo->si_addr, (unsigned long long)Frame->State.pc);
      ::write(STDERR_FILENO, Buf, N > 0 ? static_cast<size_t>(N) : 0);
      FEX::HLE::_SyscallHandler->VMATracking.Mutex.ReportAcquirerDiag();
    }
    NewGuestSP = SetupFrame_Arm64(Thread, ContextBackup, Frame, Signal, HostSigInfo, ucontext, GuestAction, GuestStack, NewGuestSP);
  }

  Frame->State.pc = reinterpret_cast<uint64_t>(GuestAction->sigaction_handler.sigaction);
  Frame->State.sp = NewGuestSP;

  // Set the new PC
  ArchHelpers::Context::SetPc(ucontext, Config.AbsoluteLoopTopAddressFillSRA);
  ArchHelpers::Context::SetFillSRASingleInst(ucontext, false);
  // Set our state register to point to our guest thread data
  ArchHelpers::Context::SetState(ucontext, reinterpret_cast<uint64_t>(Frame));

  return true;
}

bool SignalDelegator::HandleSIGILL(FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext) {
  SIGTRACE("SIGILL pc=0x%lx sentinel=%d pause=%d", ArchHelpers::Context::GetPc(ucontext),
           (ArchHelpers::Context::GetPc(ucontext) == Config.SignalHandlerReturnAddress ||
            ArchHelpers::Context::GetPc(ucontext) == Config.SignalHandlerReturnAddressRT) ? 1 : 0,
           ArchHelpers::Context::GetPc(ucontext) == Config.PauseReturnInstruction ? 1 : 0);
  if (ArchHelpers::Context::GetPc(ucontext) == Config.SignalHandlerReturnAddress ||
      ArchHelpers::Context::GetPc(ucontext) == Config.SignalHandlerReturnAddressRT) {
    auto ThreadObject = FEX::HLE::ThreadManager::GetStateObjectFromFEXCoreThread(Thread);
    RestoreThreadState(Thread, ucontext,
                       ArchHelpers::Context::GetPc(ucontext) == Config.SignalHandlerReturnAddressRT ? RestoreType::TYPE_REALTIME :
                                                                                                      RestoreType::TYPE_NONREALTIME);

    // Ref count our faults
    // We use this to track if it is safe to clear cache
    --Thread->CurrentFrame->SignalHandlerRefCounter;

    if (ThreadObject->SignalInfo.DeferredSignalFrames.size() != 0) {
      // If we have more deferred frames to process then mprotect back to PROT_NONE.
      // It will have been RW coming in to this sigreturn and now we need to remove permissions
      // to ensure FEX trampolines back to the SIGSEGV deferred handler.
      Thread->ProtectInterruptFaultPage(true);
    }
    return true;
  }

  if (ArchHelpers::Context::GetPc(ucontext) == Config.PauseReturnInstruction) {
    RestoreThreadState(Thread, ucontext, RestoreType::TYPE_PAUSE);

    // Ref count our faults
    // We use this to track if it is safe to clear cache
    --Thread->CurrentFrame->SignalHandlerRefCounter;
    return true;
  }

  return false;
}

bool SignalDelegator::HandleSignalPause(FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext) {
  auto ThreadObject = FEX::HLE::ThreadManager::GetStateObjectFromFEXCoreThread(Thread);
  SignalEvent SignalReason = ThreadObject->SignalReason.load();
  auto Frame = Thread->CurrentFrame;

  if (SignalReason == SignalEvent::Pause) {
    // Store our thread state so we can come back to this
    StoreThreadState(Thread, Signal, ucontext);

    if (CTX->IsAddressInCodeBuffer(Thread, ArchHelpers::Context::GetPc(ucontext))) {
      // We are in jit, SRA must be spilled
      ArchHelpers::Context::SetPc(ucontext, Config.ThreadPauseHandlerAddressSpillSRA);
    } else {
      // We are in non-jit, SRA is already spilled
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
      LOGMAN_THROW_A_FMT(!IsAddressInDispatcher(ArchHelpers::Context::GetPc(ucontext)), "Signals in dispatcher have unsynchronized "
                                                                                        "context");
#endif
      ArchHelpers::Context::SetPc(ucontext, Config.ThreadPauseHandlerAddress);
    }

    // Set our state register to point to our guest thread data
    ArchHelpers::Context::SetState(ucontext, reinterpret_cast<uint64_t>(Frame));

    // Ref count our faults
    // We use this to track if it is safe to clear cache
    ++Thread->CurrentFrame->SignalHandlerRefCounter;

    ThreadObject->SignalReason.store(SignalEvent::Nothing);
    return true;
  }

  if (SignalReason == SignalEvent::Stop) {
    // Our thread is stopping
    // We don't care about anything at this point
    // Set the stack to our starting location when we entered the core and get out safely
    ArchHelpers::Context::SetSp(ucontext, Frame->ReturningStackLocation);

    // Our ref counting doesn't matter anymore
    Thread->CurrentFrame->SignalHandlerRefCounter = 0;

    // Set the new PC
    if (CTX->IsAddressInCodeBuffer(Thread, ArchHelpers::Context::GetPc(ucontext))) {
      // We are in jit, SRA must be spilled
      ArchHelpers::Context::SetPc(ucontext, Config.ThreadStopHandlerAddressSpillSRA);
    } else {
      // We are in non-jit, SRA is already spilled
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
      LOGMAN_THROW_A_FMT(!IsAddressInDispatcher(ArchHelpers::Context::GetPc(ucontext)), "Signals in dispatcher have unsynchronized "
                                                                                        "context");
#endif
      ArchHelpers::Context::SetPc(ucontext, Config.ThreadStopHandlerAddress);
    }

    // We need to be a little bit careful here
    // If we were already paused (due to GDB) and we are immediately stopping (due to gdb kill)
    // Then we need to ensure we don't double decrement our idle thread counter
    if (ThreadObject->ThreadSleeping) {
      // If the thread was sleeping then its idle counter was decremented
      // Reincrement it here to not break logic
      FEX::HLE::_SyscallHandler->TM.IncrementIdleRefCount();
    }

    ThreadObject->SignalReason.store(SignalEvent::Nothing);
    return true;
  }

  if (SignalReason == SignalEvent::Return || SignalReason == SignalEvent::ReturnRT) {
    RestoreThreadState(Thread, ucontext, SignalReason == SignalEvent::ReturnRT ? RestoreType::TYPE_REALTIME : RestoreType::TYPE_NONREALTIME);

    // Ref count our faults
    // We use this to track if it is safe to clear cache
    --Thread->CurrentFrame->SignalHandlerRefCounter;

    ThreadObject->SignalReason.store(SignalEvent::Nothing);
    return true;
  }
  return false;
}

void SignalDelegator::SignalThread(FEXCore::Core::InternalThreadState* Thread, SignalEvent Event) {
  auto ThreadObject = FEX::HLE::ThreadManager::GetStateObjectFromFEXCoreThread(Thread);
  ThreadObject->SignalReason.store(Event);
  FHU::Syscalls::tgkill(ThreadObject->ThreadInfo.PID, ThreadObject->ThreadInfo.TID, SignalDelegator::SIGNAL_FOR_PAUSE);
}

/**  @} */

static bool IsAsyncSignal(const siginfo_t* Info, int Signal) {
  if (Info->si_code <= SI_USER) {
    // If the signal is not from the kernel then it is always async.
    // This is because synchronous signals can be sent through tgkill,sigqueue and other methods.
    // SI_USER == 0 and all negative si_code values come from the user.
    return true;
  } else {
    // If the signal is from the kernel then it is async only if it isn't an explicit synchronous signal.
    switch (Signal) {
    // These are all synchronous signals.
    case SIGBUS:
    case SIGFPE:
    case SIGILL:
    case SIGSEGV:
    case SIGTRAP: return false;
    default: break;
    }
  }

  // Everything else is async and can be deferred.
  return true;
}

bool SignalDelegator::ThreadHasDeliverableGuestSignal(FEX::HLE::ThreadStateObject* ThreadObject) {
  // Mirrors the drain-time filtering in HandleGuestSignal: a frame that is
  // guest-masked gets parked in PendingSignals instead of delivered, and a
  // SIG_IGN / default-ignore disposition gets dropped. Only what survives
  // those filters can interrupt a sleep on a real kernel.
  //
  // Runs on the owning thread from the syscall path. The thread's own signal
  // handler can append a frame concurrently, but DeferredSignalFrames never
  // reallocates (capacity is pre-reserved, enforced at the emplace site), so
  // indexed iteration over a snapshotted size is safe; a frame appended after
  // the snapshot is caught by the caller's next check.
  const uint64_t GuestMask = ThreadObject->SignalInfo.CurrentSignalMask.Val;
  const auto Deliverable = [&](int Signal) {
    if (Signal < 1 || Signal > MAX_SIGNALS) {
      return false;
    }
    if (GuestMask & (1ULL << (Signal - 1))) {
      return false;
    }
    const SignalHandler& Handler = HostHandlers[Signal];
    const auto GuestHandler = Handler.GuestAction.sigaction_handler.handler;
    if (GuestHandler == SIG_IGN) {
      return false;
    }
    if (GuestHandler == SIG_DFL && Handler.DefaultBehaviour == DEFAULT_IGNORE) {
      return false;
    }
    // Real handler, or default-terminate: both reach the guest on drain.
    return true;
  };

  const auto& Frames = ThreadObject->SignalInfo.DeferredSignalFrames;
  const size_t Count = Frames.size();
  for (size_t i = 0; i < Count; ++i) {
    if (Deliverable(Frames[i].Signal)) {
      return true;
    }
  }

  uint64_t Pending = ThreadObject->SignalInfo.PendingSignals & ~GuestMask;
  while (Pending) {
    const int Signal = __builtin_ctzll(Pending) + 1;
    Pending &= Pending - 1;
    if (Deliverable(Signal)) {
      return true;
    }
  }
  return false;
}

uint64_t SignalDelegator::GetNewSigMask(int Signal) const {
  const SignalHandler& Handler = HostHandlers[Signal];
  // Set up a new mask based on this signals signal mask
  uint64_t NewMask = Handler.GuestAction.sa_mask.Val;

  // If NODEFER then the new signal mask includes this signal
  if (!(Handler.GuestAction.sa_flags & SA_NODEFER)) {
    NewMask |= (1ULL << (Signal - 1));
  }

  // Walk our required signals and stop masking them if requested
  for (size_t i = 0; i < MAX_SIGNALS; ++i) {
    if (HostHandlers[i + 1].Required.load(std::memory_order_relaxed)) {
      // Never mask our required signals
      NewMask &= ~(1ULL << i);
    }
  }

  return NewMask;
}

bool SignalDelegator::HandleFrontendSIGSEGV(FEXCore::Core::InternalThreadState* Thread, int Signal, void* Info, void* UContext) {
  auto SigInfo = *static_cast<siginfo_t*>(Info);

  if (FaultSafeUserMemAccess::TryHandleSafeFault(Signal, SigInfo, UContext)) {
    // The arm64 guest's syscall layer copies guest memory through these
    // helpers to return EFAULT like the kernel, so resume the helper's caller
    // with EFAULT instead of stopping the process.
    return true;
  }

#ifdef ARCHITECTURE_arm64
  if (Signal == SIGSEGV && SigInfo.si_code == SEGV_ACCERR && SigInfo.si_addr >= reinterpret_cast<void*>(Thread->JITGuardPage) &&
      SigInfo.si_addr < reinterpret_cast<void*>(Thread->JITGuardPage + FEXCore::HostPage::Size())) {
    FEXCore::UncheckedLongJump::ManuallyLoadJumpBuf(Thread->RestartJump, Thread->JITGuardOverflowArgument,
                                                    ArchHelpers::Context::GetArmGPRs(UContext), ArchHelpers::Context::GetArmFPRs(UContext),
                                                    ArchHelpers::Context::GetArmPc(UContext));
    return true;
  }
#endif

  return false;
}

void SignalDelegator::HandleGuestSignal(FEX::HLE::ThreadStateObject* ThreadObject, int Signal, void* Info, void* UContext) {
  auto Thread = ThreadObject->Thread;
  ucontext_t* _context = (ucontext_t*)UContext;
  auto SigInfo = *static_cast<siginfo_t*>(Info);

  auto MustDeferSignal = (Thread->CurrentFrame->State.DeferredSignalRefCount.Load() != 0);

#if defined(ARCHITECTURE_ppc64le)
  // PPC64LE: also defer async signals whose host PC lies inside the JIT code
  // buffer. SpillSRA blindly copies the ucontext SRA regs into State.gregs
  // assuming they hold the coherent x86 register file at the snapped x86
  // boundary returned by RestoreRIPFromHostPC. That assumption only holds at
  // IR-op boundaries. The host kernel can interrupt a block mid-IR-op (for
  // example between an `and__` writing a 64-bit AND result into SRA[rax] and
  // the follow-up `rldicl` that zero-extends to 32 bits) — at which point
  // SRA[rax] holds the un-zero-extended scratch value and *not* the post-x86
  // RAX. Eager processing of such a signal corrupts State.gregs and the
  // INJIT-routed FillSRA on return re-dispatches with garbage gregs (typical
  // crash: SEGV_MAPERR at NULL+8 ~2 KB later, when a now-NULL gregs-derived
  // pointer is dereferenced).
  //
  // Treat the entire JIT code buffer as a deferral region for async signals;
  // the EmitSuspendInterruptCheck poke at every block entry / backward branch
  // drains the queued signal at a guaranteed-coherent boundary. Synchronous
  // signals (SIGSEGV/SIGBUS/SIGILL/SIGFPE — IsAsyncSignal is false) still go
  // through their normal handlers, so a real guest fault is not affected.
  // Defer inside the dispatcher too, not just JIT blocks. Blocks exit to the
  // dispatcher loop-top/L1-probe with the SRA registers live and dirty (no
  // spill on the block->dispatcher edge -- that is the SRA design), but the
  // host PC is no longer in the code buffer, so an eager delivery there goes
  // through HandleDispatcherGuestSignal's outside-JIT branch: no SpillSRA,
  // guest frame built from STALE State.gregs, and the handler's sigreturn
  // restores those stale values into the resumed context. Observed live on
  // Ziggurat dungeon generation (compilation storm + Boehm GC signal storm):
  // guest rbx came back holding an old heap pointer and the 0x934d00 scan
  // loop wedged with rbx billions past its r15=4 limit -- the same corrupted
  // register signature as the InSyscallInfo leak, via a different door.
  // Every dispatcher path reaches a block-entry fault-page poke (L1 hit ->
  // block entry; miss -> linker/compile, which is already refcount-deferred,
  // then block entry), so a deferred signal always drains at a boundary
  // where the register state is coherent.
  const uint64_t DeferPc = ArchHelpers::Context::GetPc(UContext);
  // Bit 25 = kInFABISentinel: the thread is inside an F80/vector FABI
  // helper crossing (arming site: the GenerateABICall stubs). The helper
  // never blocks, and the interrupted JIT block holds in-flight x87-pass
  // state in registers that a guest-handler round trip would desync, so
  // these deliveries must defer to the next block boundary regardless of
  // which generated-code region the PC happens to be in (dispatcher-blob
  // stubs, old code-buffer chunks, or the helper's C code).
  const bool InFABICrossing = (Thread->CurrentFrame->InSyscallInfo & 0x0200'0000ull) != 0;
  const bool InJIT_ForDefer = CTX->IsAddressInCodeBuffer(Thread, DeferPc) || IsAddressInDispatcher(DeferPc) ||
                              IsAddressInFABIStubs(DeferPc) || InFABICrossing;
  const bool MustDeferAsync = MustDeferSignal || InJIT_ForDefer;
  SIGTRACE("GUEST sig=%d code=%d pc=0x%lx rip=0x%lx defer=%d injit=%d q=%zu", Signal, SigInfo.si_code,
           ArchHelpers::Context::GetPc(UContext), (unsigned long)Thread->CurrentFrame->State.pc, MustDeferSignal ? 1 : 0,
           InJIT_ForDefer ? 1 : 0, ThreadObject->SignalInfo.DeferredSignalFrames.size());
#else
  const bool MustDeferAsync = MustDeferSignal;
#endif

  // Identification predicate: the fault page is now an mmap'd host page, so compare
  // against the stored pointer rather than an address inside the thread state.
  if (Signal == SIGSEGV && SigInfo.si_code == SEGV_ACCERR && Thread->CurrentFrame->InterruptFaultPagePtr &&
      SigInfo.si_addr == reinterpret_cast<void*>(Thread->CurrentFrame->InterruptFaultPagePtr)) {
    if (!MustDeferSignal) {
      // We just reached the end of the outermost signal-deferring section and faulted to check for pending signals.
      // Pull a signal frame off the stack.

      Thread->ProtectInterruptFaultPage(false);

      // FEX_SMCLAZYLINK: the SMC fault handler arms this page after a lazy
      // deferral, because with block linking live the fault-page poke at block
      // entry is the only trap a linked chain cannot skip. Settle the drain
      // debt BEFORE any resume path below — including the "no signals queued"
      // return — or the thread could re-enter a linked chain and reach a stale
      // translation of code it wrote itself. Unprotect-first ordering above is
      // load-bearing: the drain compiles/relinks nothing, but the thread's
      // next entry poke must not re-fault into a loop. No-op (one relaxed
      // load) unless the lazy-link mode armed it.
      //
      // FEX_SMCLAZYCROSSPOKE arms this same page on threads that did NOT do
      // the writing, so the gate is the whole lazy mode rather than the
      // lazy-link sub-option: with cross-poke on, a thread reaching here may be
      // an innocent reader whose only bound on staleness is this settle. The
      // widened gate costs one relaxed load, and the settle itself is a no-op
      // (one more relaxed load) when this thread owes nothing.
      if (FEX::HLE::_SyscallHandler && FEX::HLE::_SyscallHandler->SMCLazyInvalActive()) {
        Thread->CTX->SettleLazySMCDrainIfPending(Thread);
      }

      if (ThreadObject->SignalInfo.DeferredSignalFrames.empty()) {
        // No signals to defer. Just set the fault page back to RW and continue execution.
        // This occurs as a minor race condition between the refcount decrement and the access to the fault page.
        return;
      }

      const auto& Top = ThreadObject->SignalInfo.DeferredSignalFrames.back();
      Signal = Top.Signal;
      SigInfo = Top.Info;
      // sig mask has been updated at the defer time, recover the original mask
      memcpy(&_context->uc_sigmask, &Top.SigMask, sizeof(uint64_t));
      ThreadObject->SignalInfo.DeferredSignalFrames.pop_back();
      SIGTRACE("DRAIN sig=%d mask=0x%lx qleft=%zu", Signal, Top.SigMask, ThreadObject->SignalInfo.DeferredSignalFrames.size());

      // Until we re-protect the page to PROT_NONE, FEX will now *permanently* defer signals and /not/ check them.
      //
      // In order to return /back/ to a sane state, we wait for the rt_sigreturn to happen.
      // rt_sigreturn will check if there are any more deferred signals to handle
      // - If there are deferred signals
      //   - mprotect back to PROT_NONE
      //   - sigreturn will trampoline out to the previous fault address check, SIGSEGV and restart
      // - If there are *no* deferred signals
      //  - No need to mprotect, it is already RW
    } else {
#if defined(ARCHITECTURE_arm64) || defined(ARCHITECTURE_ppc64le)
      // If RefCount != 0 then that means we hit an access with nested signal-deferring sections.
      // Increment the PC past the unconditional refcount-store (`str zr, [x1]` on Arm64,
      // `std rN, 0(rM)` on PPC64LE — both 4-byte fixed-size stores) so execution continues
      // until we reach the outermost section.
      ArchHelpers::Context::SetPc(UContext, ArchHelpers::Context::GetPc(UContext) + 4);
      return;
#else
      // X86 should always be doing a refcount compare and branch since we can't guarantee instruction size.
      // ARM64 / PPC64LE just always do the access to reduce branching overhead.
      ERROR_AND_DIE_FMT("X86 shouldn't hit this InterruptFaultPage");
#endif
    }
  } else if (IsAsyncSignal(&SigInfo, Signal) && MustDeferAsync) {
    // If the signal is asynchronous (as determined by si_code) and FEX is in a state of needing
    // to defer the signal, then add the signal to the thread's signal queue.
    LOGMAN_THROW_A_FMT(ThreadObject->SignalInfo.DeferredSignalFrames.size() != ThreadObject->SignalInfo.DeferredSignalFrames.capacity(),
                       "Deferred signals vector hit "
                       "capacity size. This will "
                       "likely crash! Asserting now!");

    ThreadObject->SignalInfo.DeferredSignalFrames.emplace_back(ThreadStateObject::DeferredSignalState {
      .Info = SigInfo,
      .Signal = Signal,
      .SigMask = _context->uc_sigmask.__val[0],
    });

    uint64_t NewMask = GetNewSigMask(Signal);

    // Update our host signal mask so we don't hit race conditions with signals
    // This allows us to maintain the expected signal mask through the guest signal handling and then all the way back again
    memcpy(&_context->uc_sigmask, &NewMask, sizeof(uint64_t));

    // Now update the faulting page permissions so it will fault on write.
    Thread->ProtectInterruptFaultPage(true);
    SIGTRACE("DEFER sig=%d pc=0x%lx newmask=0x%lx q=%zu", Signal, ArchHelpers::Context::GetPc(UContext), NewMask,
             ThreadObject->SignalInfo.DeferredSignalFrames.size());

    // Postpone the remainder of signal handling logic until we process the SIGSEGV triggered by writing to InterruptFaultPage.
    return;
  }

#if defined(ARCHITECTURE_ppc64le)
  // Host-fault gate (64K execution plan, open item 1).
  //
  // A synchronous fatal-class signal (kernel si_code) whose host PC is not in
  // any JIT code buffer was not raised by the guest program's instructions. If
  // it was raised inside a deferred-signal section (a syscall body, the block
  // linker/compiler, VMA tracking), at a dispatcher or FABI-stub PC, or is a
  // trap-class signal that guest code cannot produce from host text at all
  // (SIGTRAP from FEX's own `trap` asserts, SIGILL, SIGFPE), then it is FEX's
  // fault, and building a guest signal frame on top of it is never right:
  //
  //   - the guest handler sees a stale block-boundary RIP and a context that
  //     has nothing to do with the fault, so what it does is arbitrary;
  //   - if it returns, the faulting host instruction re-executes and the same
  //     signal fires again (a `trap` loops forever);
  //   - if it does not return (Mono's managed-exception unwind, a longjmp), the
  //     host frame is abandoned with every lock it held. RimWorld Linux on the
  //     64K kernel leaked VMATracking's write lock exactly this way: the code
  //     invalidator's deadline trap, delivered as a guest SIGTRAP on top of
  //     Granule::Madvise (a90c5cd26), and the game ran one frame a minute.
  //
  // Faults the guest IS entitled to see stay on their paths: a fault inside a
  // JIT block (IsAddressInCodeBuffer), a synthesized fault from the Break op's
  // dispatcher stubs (FaultToTopAndGeneratedException, set by the op and
  // cleared by StoreThreadState), the interrupt-fault-page poke and the
  // FaultSafeUserMemAccess probes (both handled before this point), and SMC
  // faults (HandleSegfault runs as a host handler before HandleGuestSignal).
  // A SIGSEGV/SIGBUS in a thunk's host library with no deferred section
  // active is deliberately NOT gated: that is a host library dereferencing a
  // guest-supplied pointer, the outside-JIT delivery below is the historical
  // behaviour for it, and no FEX lock is held there.
  //
  // What happens instead: one loud report on stderr (raw write, so the
  // default-silent log cannot swallow it), the host backtrace, the VMA lock
  // holder if FEX_LOCKDIAG is on, then the signal's default disposition is
  // restored and the handler returns. The kernel re-raises the fault at the
  // original instruction, so the coredump carries the real faulting context,
  // not a re-raise from inside this handler. The unhandled-crash tail further
  // down (FlushAndCloseCodeMap, telemetry, CleanupForExit) is skipped on
  // purpose: by hypothesis this thread may hold FEX locks those paths take.
  //
  // FEX_HOSTFAULTTOGUEST=1 restores the old delivery after the report, as a
  // bisection lever for a title that happened to survive it.
  if ((Signal == SIGSEGV || Signal == SIGBUS || Signal == SIGILL || Signal == SIGFPE || Signal == SIGTRAP) && !IsAsyncSignal(&SigInfo, Signal)) {
    const uint64_t HostPC = ArchHelpers::Context::GetPc(UContext);
    const bool Synthesized = Thread->CurrentFrame->SynchronousFaultData.FaultToTopAndGeneratedException;
    const char* Region = nullptr;
    if (!Synthesized && !CTX->IsAddressInCodeBuffer(Thread, HostPC)) {
      if (MustDeferSignal) {
        Region = "a deferred-signal section (syscall body, block linker/compiler or VMA tracking)";
      } else if (IsAddressInDispatcher(HostPC)) {
        Region = "the dispatcher";
      } else if (IsAddressInFABIStubs(HostPC) || InFABICrossing) {
        Region = "an F80/vector FABI helper crossing";
      } else if (Signal == SIGTRAP || Signal == SIGILL || Signal == SIGFPE) {
        Region = "host code outside every generated-code region";
      }
    }
    if (Region) [[unlikely]] {
      static const bool DeliverAnyway = getenv("FEX_HOSTFAULTTOGUEST") != nullptr;
      char Buf[640];
      const int N = ::snprintf(Buf, sizeof(Buf),
                               "FEX: FATAL host fault: signal %d (si_code %d, addr 0x%lx) at host nip 0x%lx lr 0x%lx, raised in %s; tid %u; "
                               "guest rip 0x%lx (block-boundary value, may be stale); DeferredSignalRefCount %lu; InSyscallInfo 0x%lx. "
                               "%s\n",
                               Signal, SigInfo.si_code, reinterpret_cast<unsigned long>(SigInfo.si_addr), (unsigned long)HostPC,
                               (unsigned long)_context->uc_mcontext.regs->link, Region, FHU::Syscalls::gettid(),
                               (unsigned long)Thread->CurrentFrame->State.pc,
                               (unsigned long)Thread->CurrentFrame->State.DeferredSignalRefCount.Load(),
                               (unsigned long)Thread->CurrentFrame->InSyscallInfo,
                               DeliverAnyway ? "FEX_HOSTFAULTTOGUEST is set: delivering it to the guest anyway." :
                                               "Not delivered to the guest (the host frame and any locks it holds would be abandoned); "
                                               "terminating with the default disposition. FEX_HOSTFAULTTOGUEST=1 delivers it instead.");
      ::write(STDERR_FILENO, Buf, N > 0 ? static_cast<size_t>(N) : 0);
      {
        void* Frames[48];
        const int Count = ::backtrace(Frames, 48);
        static const char Hdr[] = "FEX: host backtrace (this handler first, the faulting frame follows the kernel sigtramp):\n";
        ::write(STDERR_FILENO, Hdr, sizeof(Hdr) - 1);
        ::backtrace_symbols_fd(Frames, Count, STDERR_FILENO);
      }
      if (FEX::HLE::_SyscallHandler && FEX::HLE::_SyscallHandler->VMATracking.Mutex.WriteHeldBySelfDiag()) {
        FEX::HLE::_SyscallHandler->VMATracking.Mutex.ReportAcquirerDiag();
      }
      if (!DeliverAnyway) {
        struct sigaction sa {};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sigaction(Signal, &sa, nullptr);
        return;
      }
    }
  }
#endif

  // Diagnostic (FEX_ABORT_TRIPWIRE=1): log every guest-delivered fatal-class
  // sync signal with its si_addr/si_code and the guest RIP. Paired with the
  // tgkill(SIGABRT) tripwire in Passthrough.cpp -- together they show what
  // fault preceded a guest abort(). Unity redirects guest stderr to
  // Player.log, so that is where these lines land.
  if ((Signal == SIGSEGV || Signal == SIGBUS || Signal == SIGILL || Signal == SIGFPE) && !IsAsyncSignal(&SigInfo, Signal)) {
    static const bool trip = (getenv("FEX_ABORT_TRIPWIRE") != nullptr);
    if (trip) {
      char buf[2048];
      int n = snprintf(buf, sizeof(buf), "[GSIG] tid=%d sig=%d si_code=%d si_addr=0x%lx guest_rip=0x%lx\n",
                       FHU::Syscalls::gettid(), Signal, SigInfo.si_code, reinterpret_cast<unsigned long>(SigInfo.si_addr),
                       (unsigned long)Thread->CurrentFrame->State.pc);
      const auto& St = Thread->CurrentFrame->State;
      n += snprintf(buf + n, sizeof(buf) - n, "[GSIG]  x0=%lx x1=%lx x2=%lx x3=%lx x29=%lx x30=%lx sp=%lx\n",
                    St.x[0], St.x[1], St.x[2], St.x[3], St.x[29], St.x[30], St.sp);

      // FEX_TRIPWIRE_PROBE="<reg>:<off>[,<off>...]" — dump guest memory at
      // fixed offsets from one register, e.g. "rdi:0x0,0x28,0x490". The dumped
      // GPRs are SRA-reconstructed block-entry state, so probing memory they
      // point at is how a suspect object is inspected post-mortem (RimWorld's
      // UnityPlayer+0x1aa3ba0 NULL-field crash is the motivating case).
      // Reads go through process_vm_readv: the register value is untrusted and
      // a raw dereference here would turn a bad reconstruction into a
      // recursive SIGSEGV inside the signal handler, losing the whole dump.
      static const char* ProbeSpec = getenv("FEX_TRIPWIRE_PROBE");
      if (ProbeSpec) {
        static constexpr std::pair<const char*, int> RegNames[] = {
          {"x0", 0}, {"x1", 1}, {"x2", 2}, {"x3", 3}, {"x4", 4}, {"x5", 5}, {"x6", 6}, {"x7", 7},
          {"x8", 8}, {"x19", 19}, {"x20", 20}, {"x21", 21}, {"x29", 29}, {"x30", 30}, {"sp", 31},
        };
        const char* Colon = strchr(ProbeSpec, ':');
        int RegIdx = -1;
        if (Colon) {
          for (auto& [Name, Idx] : RegNames) {
            if (strlen(Name) == static_cast<size_t>(Colon - ProbeSpec) && !memcmp(ProbeSpec, Name, Colon - ProbeSpec)) {
              RegIdx = Idx;
              break;
            }
          }
        }
        if (RegIdx >= 0) {
          const uint64_t Base = RegIdx == 31 ? St.sp : St.x[RegIdx];
          const char* p = Colon + 1;
          for (int i = 0; i < 16 && *p; ++i) {
            char* End = nullptr;
            const uint64_t Off = strtoul(p, &End, 0);
            if (End == p) {
              break;
            }
            uint64_t Val = 0;
            struct iovec Local {&Val, sizeof(Val)};
            struct iovec Remote {reinterpret_cast<void*>(Base + Off), sizeof(Val)};
            if (process_vm_readv(::getpid(), &Local, 1, &Remote, 1, 0) == sizeof(Val)) {
              n += snprintf(buf + n, sizeof(buf) - n, "[GSIG]  probe [%.*s+0x%lx] = 0x%lx\n", static_cast<int>(Colon - ProbeSpec),
                            ProbeSpec, Off, Val);
            } else {
              n += snprintf(buf + n, sizeof(buf) - n, "[GSIG]  probe [%.*s+0x%lx] = <unreadable>\n",
                            static_cast<int>(Colon - ProbeSpec), ProbeSpec, Off);
            }
            p = (*End == ',') ? End + 1 : End;
          }
        }
      }
      // The code bytes at the faulting guest RIP, read fault-free, so a crash
      // in freshly JIT'd guest code (CoreCLR, Mono) names its instruction
      // without a debugger attached at the right moment.
      {
        uint8_t Code[32] = {};
        struct iovec Local {Code, sizeof(Code)};
        struct iovec Remote {reinterpret_cast<void*>(St.pc), sizeof(Code)};
        const ssize_t Got = process_vm_readv(::getpid(), &Local, 1, &Remote, 1, 0);
        n += snprintf(buf + n, sizeof(buf) - n, "[GSIG]  code@rip:");
        for (ssize_t i = 0; i < Got && n < static_cast<int>(sizeof(buf)) - 4; ++i) {
          n += snprintf(buf + n, sizeof(buf) - n, " %02x", Code[i]);
        }
        n += snprintf(buf + n, sizeof(buf) - n, "%s\n", Got <= 0 ? " <unreadable>" : "");
      }
      // The mapping that holds the guest RIP, from /proc/self/maps (open/read
      // only): names the library or JIT heap without a maps snapshot taken
      // at exactly the right moment.
      {
        const int MapsFD = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
        if (MapsFD >= 0) {
          static char Maps[1 << 20];
          ssize_t Total = 0;
          for (;;) {
            const ssize_t R = ::read(MapsFD, Maps + Total, sizeof(Maps) - 1 - Total);
            if (R <= 0 || Total + R >= static_cast<ssize_t>(sizeof(Maps)) - 1) {
              break;
            }
            Total += R;
          }
          ::close(MapsFD);
          Maps[Total] = 0;
          const char* Line = Maps;
          bool Found = false;
          while (*Line && !Found) {
            const char* End = strchr(Line, '\n');
            const size_t Len = End ? static_cast<size_t>(End - Line) : strlen(Line);
            char* Dash = nullptr;
            const uint64_t Lo = strtoull(Line, &Dash, 16);
            const uint64_t Hi = (Dash && *Dash == '-') ? strtoull(Dash + 1, nullptr, 16) : 0;
            if (Lo <= St.pc && St.pc < Hi) {
              n += snprintf(buf + n, sizeof(buf) - n, "[GSIG]  rip in: %.*s\n", static_cast<int>(Len > 200 ? 200 : Len), Line);
              Found = true;
            }
            Line = End ? End + 1 : Line + Len;
          }
          if (!Found) {
            n += snprintf(buf + n, sizeof(buf) - n, "[GSIG]  rip in: <no mapping>\n");
          }
        }
      }
      [[maybe_unused]] auto _ = write(2, buf, n);
    }
  }

  // Check for masked signals
  if (ThreadObject->SignalInfo.CurrentSignalMask.Val & (1ULL << (Signal - 1)) && IsAsyncSignal(&SigInfo, Signal)) {
    // This signal is masked, must defer until the guest updates the signal mask.
    // Add it to the pending signal list
    ThreadObject->SignalInfo.PendingSignals |= 1ULL << (Signal - 1);
    return;
  }

  // Let the host take first stab at handling the signal
  SignalHandler& Handler = HostHandlers[Signal];

  // Remove the pending signal
  ThreadObject->SignalInfo.PendingSignals &= ~(1ULL << (Signal - 1));

  // We have an emulation thread pointer, we can now modify its state
  if (Handler.GuestAction.sigaction_handler.handler == SIG_DFL) {
    if (Handler.DefaultBehaviour == DEFAULT_TERM || Handler.DefaultBehaviour == DEFAULT_COREDUMP) {
      // Let the signal fall through to the unhandled path
      // This way the parent process can know it died correctly
    }
  } else if (Handler.GuestAction.sigaction_handler.handler == SIG_IGN) {
    return;
  } else {
    // FEX_SMCLAZYINVAL drain point (c): guest signal delivery.
    //
    // The guest handler is about to run, on a control-flow edge the guest did
    // not write and cannot have prepared for. It is also a serializing event in
    // x86 terms, so it is a place a guest is entitled to assume its own earlier
    // code writes have become visible. Settle the deferred invalidations before
    // the frame is built and the handler dispatched.
    //
    // Lock protocol: this runs in a host signal handler, so the drain's
    // ReleaseAllPendingSharedLocks is load-bearing rather than decorative --
    // it is the same recovery HandleSegfault performs before it soft-invalidates
    // from signal context. Reaching here means the signal was NOT deferred:
    // async signals with DeferredSignalRefCount != 0 (which covers every host
    // syscall body, hence every place FEX holds VMATracking/ThreadCreation
    // locks) and async signals raised inside the JIT or dispatcher have already
    // returned above, queued. What is left is a synchronous guest fault from
    // JIT code, or an async signal at a drained-to boundary; in neither case is
    // this thread inside a FEX lock scope that the drain's exclusive
    // CodeInvalidationMutex could deadlock against.
    if (_SyscallHandler->SMCLazyInvalActive()) {
      _SyscallHandler->DrainSMCLazyDirtyPages(Thread, FEX::HLE::SMCLazy::DrainPoint::GuestSignal);
    }

    if (Handler.GuestHandler &&
        Handler.GuestHandler(Thread, Signal, &SigInfo, UContext, &Handler.GuestAction, &ThreadObject->SignalInfo.GuestAltStack)) {
      // Guest SA_RESTART bookkeeping. A guest handler is now committed to run on
      // this thread; record whether the guest asked for interrupted syscalls to
      // be restarted around it. HandleSyscall's restart loop reads these once
      // its DeferredSignalRefCountGuard has destructed -- which, for a thread
      // interrupted inside a host syscall, is exactly the point this delivery
      // happens from (fault-page poke -> drain -> handler -> sigreturn -> back
      // into the destructor). See ThreadManager.h for the field comments.
      ++ThreadObject->SignalInfo.DeliveredGuestSignals;
      if (!(Handler.GuestAction.sa_flags & SA_RESTART)) {
        ++ThreadObject->SignalInfo.DeliveredGuestSignalsWithoutRestart;
      }

      uint64_t NewMask = GetNewSigMask(Signal);

      // Update our host signal mask so we don't hit race conditions with signals
      // This allows us to maintain the expected signal mask through the guest signal handling and then all the way back again
      memcpy(&_context->uc_sigmask, &NewMask, sizeof(uint64_t));

      // We handled this signal, continue running
      return;
    }
    // GuestHandler returned false: we tried to deliver the signal but the
    // dispatcher refused (e.g., bogus guest RSP per the HandleDispatcherGuestSignal
    // SP guard, or pressure-vessel sandbox half-init state). Aborting the whole
    // emulator with "Unhandled guest exception" is too aggressive -- the thread
    // is in a bad state but the rest of the process is fine. Fall through to
    // the default-disposition path below so SIGSEGV terminates THAT THREAD
    // (or process, per kernel default for SIGSEGV) instead of crashing FEX.
    LogMan::Msg::EFmt("HandleGuestSignal: GuestHandler refused signal {}; falling to default disposition", Signal);
  }

  // Unhandled crash
  // Call back in to the previous handler
  if (Handler.OldAction.sa_flags & SA_SIGINFO) {
    Handler.OldAction.sigaction(Signal, &SigInfo, UContext);
  } else if (Handler.OldAction.handler == SIG_IGN || (Handler.OldAction.handler == SIG_DFL && Handler.DefaultBehaviour == DEFAULT_IGNORE)) {
    // Do nothing
  } else if (Handler.OldAction.handler == SIG_DFL && (Handler.DefaultBehaviour == DEFAULT_COREDUMP || Handler.DefaultBehaviour == DEFAULT_TERM)) {
    if (Signal == SIGILL && SigInfo.si_code == ILL_ILLOPC) {
      // Until the A64 frontend translates anything, every guest instruction is
      // raised as SIGILL/ILL_ILLOPC at its PC. Name it before the default
      // disposition kills the process. Async-signal-safe: one fault-free read
      // of the instruction word and one write(2).
      // POWERARM-M0-TODO(frontend): keep or narrow once real translators exist; a genuine guest UDF should still die quietly like on arm64 Linux.
      const uint64_t GuestPC = Thread->CurrentFrame->State.pc;
      uint32_t Word = 0;
      struct iovec Local {&Word, sizeof(Word)};
      struct iovec Remote {reinterpret_cast<void*>(GuestPC), sizeof(Word)};
      const bool HaveWord = process_vm_readv(::getpid(), &Local, 1, &Remote, 1, 0) == sizeof(Word);
      char Line[128];
      const int Len = HaveWord ? ::snprintf(Line, sizeof(Line), "POWERarm: unimplemented A64 instruction 0x%08x at pc 0x%llx\n", Word,
                                            static_cast<unsigned long long>(GuestPC)) :
                                 ::snprintf(Line, sizeof(Line), "POWERarm: unimplemented A64 instruction <unreadable> at pc 0x%llx\n",
                                            static_cast<unsigned long long>(GuestPC));
      [[maybe_unused]] auto Written = ::write(STDERR_FILENO, Line, Len > 0 ? static_cast<size_t>(Len) : 0);
    }
    CTX->FlushAndCloseCodeMap();

#ifndef FEX_DISABLE_TELEMETRY
    // In the case of signals that cause coredump or terminate, save telemetry early.
    // FEX is hard crashing at this point and won't hit regular shutdown routines.
    // Add the signal to the crash mask.
    FEXCORE_TELEMETRY_OR(TYPE_CRASH_MASK, (1ULL << Signal));
    if (Signal == SIGSEGV && reinterpret_cast<uint64_t>(SigInfo.si_addr) >= SyscallHandler::TASK_MAX_64BIT) {
      // Tried accessing invalid non-canonical x86-64 address.
      FEXCORE_TELEMETRY_SET(TYPE_UNHANDLED_NONCANONICAL_ADDRESS, 1);
    }
    SaveTelemetry();
#endif

    FEX::HLE::_SyscallHandler->TM.CleanupForExit();
    // FEX_THPLOG: open/read/write only, no allocation, so it is safe here.
    FEXCore::Allocator::THP::Report("signal");

    // Reassign back to DFL and crash
    signal(Signal, SIG_DFL);
    if (SigInfo.si_code != SI_KERNEL) {
      // If the signal wasn't sent by the kernel then we need to reraise it.
      // This is necessary since returning from this signal handler now might just continue executing.
      // eg: If sent from tgkill then the signal gets dropped and returns.
      FHU::Syscalls::tgkill(::getpid(), FHU::Syscalls::gettid(), Signal);
    }
  } else {
    Handler.OldAction.handler(Signal);
  }
}

void SignalDelegator::SaveTelemetry() {
#ifndef FEX_DISABLE_TELEMETRY
  if (!ApplicationName.empty()) {
    FEXCore::Telemetry::Shutdown(ApplicationName);
  }
#endif
}

bool SignalDelegator::InstallHostThunk(int Signal) {
  SignalHandler& SignalHandler = HostHandlers[Signal];
  // If the host thunk is already installed for this, just return
  if (SignalHandler.Installed) {
    return false;
  }

  // Default flags for us
  SignalHandler.HostAction.sa_flags = SA_SIGINFO | SA_ONSTACK;

  bool Result = UpdateHostThunk(Signal);

  SignalHandler.Installed = Result;
  return Result;
}

bool SignalDelegator::UpdateHostThunk(int Signal) {
  SignalHandler& SignalHandler = HostHandlers[Signal];

  // Now install the thunk handler
  SignalHandler.HostAction.sigaction = SignalHandlerThunk;

  auto CheckAndAddFlags = [](uint64_t HostFlags, uint64_t GuestFlags, uint64_t Flags) {
    // If any of the flags don't match then update to the newest set
    if ((HostFlags ^ GuestFlags) & Flags) {
      // Remove all the flags from the host that we are testing for
      HostFlags &= ~Flags;
      // Copy over the guest flags being set
      HostFlags |= GuestFlags & Flags;
    }

    return HostFlags;
  };

  // Don't allow the guest to override flags for
  // SA_SIGINFO : Host always needs SA_SIGINFO
  // SA_ONSTACK : Host always needs the altstack
  // SA_RESETHAND : We don't support one shot handlers
  // SA_RESTORER : We always need our host side restorer on x86-64, Couldn't use guest restorer anyway
  SignalHandler.HostAction.sa_flags = CheckAndAddFlags(SignalHandler.HostAction.sa_flags, SignalHandler.GuestAction.sa_flags,
                                                       SA_NOCLDSTOP | SA_NOCLDWAIT | SA_NODEFER | SA_RESTART);

#if defined(ARCHITECTURE_ppc64le)
  // PPC64LE defers async signals for the whole of HandleSyscall (9560b3c8e), which
  // only works because the interrupted host ::syscall returns -EINTR: that return is
  // what unwinds to the guard's destructor, which is what drains the deferred queue.
  //
  // SA_RESTART on the *host* action defeats that. The kernel runs our thunk, we queue
  // the signal, and then the kernel silently restarts the syscall instead of returning
  // -EINTR -- so HandleSyscall never returns, the guard never destructs, and the queued
  // guest signal is never delivered. A guest thread blocked in futex() when it gets a
  // suspend signal is then unwakeable. Observed on Ziggurat: mono's GC stop-the-world
  // handshake wedges permanently at assembly load, one thread holding a PROT_NONE
  // InterruptFaultPage with nothing left to deliver it.
  //
  // Keep the guest's SA_RESTART recorded in GuestAction (it still drives guest-visible
  // behaviour); just never let the host thunk carry it.
  SignalHandler.HostAction.sa_flags &= ~SA_RESTART;
#endif

#ifdef ARCHITECTURE_x86_64
#define SA_RESTORER 0x04000000
  SignalHandler.HostAction.sa_flags |= SA_RESTORER;
  SignalHandler.HostAction.restorer = sigrestore;
#endif

  // Walk the signals we have that are required and make sure to remove it from the mask
  // This'll likely be SIGILL, SIGBUS, SIG63

  // If the guest has masked some signals then we need to also mask those signals
  for (size_t i = 1; i < HostHandlers.size(); ++i) {
    if (HostHandlers[i].Required.load(std::memory_order_relaxed)) {
      SignalHandler.HostAction.sa_mask &= ~(1ULL << (i - 1));
    } else if (SigIsMember(&SignalHandler.GuestAction.sa_mask, i)) {
      SignalHandler.HostAction.sa_mask |= (1ULL << (i - 1));
    }
  }

  // Check for SIG_IGN
  if (SignalHandler.GuestAction.sigaction_handler.handler == SIG_IGN && HostHandlers[Signal].Required.load(std::memory_order_relaxed) == false) {
    // We are ignoring this signal on the guest
    // Which means we need to ignore it on the host as well
    SignalHandler.HostAction.handler = SIG_IGN;
  }

  // Check for SIG_DFL
  if (SignalHandler.GuestAction.sigaction_handler.handler == SIG_DFL && HostHandlers[Signal].Required.load(std::memory_order_relaxed) == false) {
    // Default handler on guest and default handler on host
    // With coredump and terminate then expect fireworks, but that is what the guest wants
    SignalHandler.HostAction.handler = SIG_DFL;
  }

  // Only update the old action if we haven't ever been installed
  const int Result =
    ::syscall(SYS_rt_sigaction, Signal, &SignalHandler.HostAction, SignalHandler.Installed ? nullptr : &SignalHandler.OldAction, 8);
  if (Result < 0) {
    // Signal 32 and 33 are consumed by glibc. We don't handle this atm
    LogMan::Msg::AFmt("Failed to install host signal thunk for signal {}: {}", Signal, strerror(errno));
    return false;
  }

  return true;
}

void SignalDelegator::UninstallHostHandler(int Signal) {
  SignalHandler& SignalHandler = HostHandlers[Signal];

  ::syscall(SYS_rt_sigaction, Signal, &SignalHandler.OldAction, nullptr, 8);
}

void SignalDelegator::QueueSignal(pid_t tgid, pid_t tid, int Signal, siginfo_t* info, bool IgnoreMask) {
  bool WasIgnored {};
  bool WasMasked {};
  SignalHandler& SignalHandler = HostHandlers[Signal];
  if (SignalHandler.GuestAction.sigaction_handler.handler == SIG_IGN && IgnoreMask) {
    ::syscall(SYS_rt_sigaction, Signal, &SignalHandler.OldAction, nullptr, 8);
    WasIgnored = true;
  }

  // Get the current host signal mask
  uint64_t ThreadSignalMask {};
  const uint64_t SignalMask = 1ULL << (Signal - 1);
  ::syscall(SYS_rt_sigprocmask, 0, nullptr, &ThreadSignalMask, 8);
  if (ThreadSignalMask & SignalMask) {
    WasMasked = true;

    // Signal currently masked, unmask
    ThreadSignalMask &= ~SignalMask;
    ::syscall(SYS_rt_sigprocmask, 0, &ThreadSignalMask, &ThreadSignalMask, 8);
  }

  ::syscall(SYSCALL_DEF(rt_tgsigqueueinfo), tgid, tid, Signal, info);

  if (WasMasked) {
    // Mask again
    ::syscall(SYS_rt_sigprocmask, 0, &ThreadSignalMask, nullptr, 8);
  }

  if (WasIgnored) {
    // Ignore again
    ::syscall(SYS_rt_sigaction, Signal, &SignalHandler.HostAction, nullptr, 8);
  }
}

SignalDelegator::SignalDelegator(FEXCore::Context::Context* _CTX, const std::string_view ApplicationName, bool SupportsAVX)
  : CTX {_CTX}
  , ApplicationName {ApplicationName}
  , SupportsAVX {SupportsAVX} {
  // Signal zero isn't real
  HostHandlers[0].Installed = true;

  // We can't capture SIGKILL or SIGSTOP
  HostHandlers[SIGKILL].Installed = true;
  HostHandlers[SIGSTOP].Installed = true;

  if (HalfBarrierTSOEnabled()) {
    UnalignedHandlerType = FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier;
  } else {
    UnalignedHandlerType = FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::NonAtomic;
  }

  // Most signals default to termination
  // These ones are slightly different
  static constexpr std::array<std::pair<int, SignalDelegator::DefaultBehaviourType>, 14> SignalDefaultBehaviours = {{
    {SIGQUIT, DEFAULT_COREDUMP},
    {SIGILL, DEFAULT_COREDUMP},
    {SIGTRAP, DEFAULT_COREDUMP},
    {SIGABRT, DEFAULT_COREDUMP},
    {SIGBUS, DEFAULT_COREDUMP},
    {SIGFPE, DEFAULT_COREDUMP},
    {SIGSEGV, DEFAULT_COREDUMP},
    {SIGCHLD, DEFAULT_IGNORE},
    {SIGCONT, DEFAULT_IGNORE},
    {SIGURG, DEFAULT_IGNORE},
    {SIGXCPU, DEFAULT_COREDUMP},
    {SIGXFSZ, DEFAULT_COREDUMP},
    {SIGSYS, DEFAULT_COREDUMP},
    {SIGWINCH, DEFAULT_IGNORE},
  }};

  for (const auto& [Signal, Behaviour] : SignalDefaultBehaviours) {
    HostHandlers[Signal].DefaultBehaviour = Behaviour;
  }

  // Register frontend SIGILL handler for forced assertion.
  RegisterFrontendHostSignalHandler(
    SIGILL,
    [](FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext) -> bool {
      ucontext_t* _context = (ucontext_t*)ucontext;
      auto& mcontext = _context->uc_mcontext;
      uint64_t PC {};
#ifdef ARCHITECTURE_arm64
      PC = mcontext.pc;
#elif defined(ARCHITECTURE_ppc64le)
      PC = mcontext.gp_regs[32]; // PPC_PT_NIP
#else
      PC = mcontext.gregs[REG_RIP];
#endif
      if (PC == reinterpret_cast<uint64_t>(&FEXCore::Assert::ForcedAssert)) {
        // This is a host side assert. Don't deliver this to the guest
        // We want to actually break here
        FEX::HLE::ThreadManager::GetStateObjectFromFEXCoreThread(Thread)->SignalInfo.Delegator->UninstallHostHandler(Signal);
        return true;
      }
      return false;
    },
    true);

  const auto PauseHandler = [](FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext) -> bool {
    return FEX::HLE::ThreadManager::GetStateObjectFromFEXCoreThread(Thread)->SignalInfo.Delegator->HandleSignalPause(Thread, Signal, info, ucontext);
  };

  const auto GuestSignalHandler = [](FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext,
                                     GuestSigAction* GuestAction, stack_t* GuestStack) -> bool {
    return FEX::HLE::ThreadManager::GetStateObjectFromFEXCoreThread(Thread)->SignalInfo.Delegator->HandleDispatcherGuestSignal(
      Thread, Signal, info, ucontext, GuestAction, GuestStack);
  };

  const auto SigillHandler = [](FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext) -> bool {
    return FEX::HLE::ThreadManager::GetStateObjectFromFEXCoreThread(Thread)->SignalInfo.Delegator->HandleSIGILL(Thread, Signal, info, ucontext);
  };

  const auto SigsegvHandler = [](FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext) -> bool {
    return FEX::HLE::ThreadManager::GetStateObjectFromFEXCoreThread(Thread)->SignalInfo.Delegator->HandleFrontendSIGSEGV(Thread, Signal,
                                                                                                                         info, ucontext);
  };

  // Register SIGILL signal handler.
  RegisterHostSignalHandler(SIGILL, SigillHandler, true);
  RegisterHostSignalHandler(SIGSEGV, SigsegvHandler, true);

  // SIGTRAP needs a host thunk for the same reason SIGILL does, and until now
  // it never had one. Host thunks were installed only for SIGILL, SIGSEGV,
  // SIGBUS and the pause signal; the all-signals loop below calls
  // RegisterHostSignalHandlerForGuest, which assigns a GuestHandler but never
  // calls InstallHostThunk. So a SIGTRAP-producing instruction reached FEX only
  // if the guest itself had called sigaction(SIGTRAP, ...) — otherwise the
  // process died on the host default disposition with FEX bypassed entirely:
  // no CleanupForExit, no telemetry, and a NIP inside the dispatcher mmap.
  //
  // This is already a live defect independent of any Break-op work. X87Ops.cpp
  // emits `trap` (0x7FE00008) for unsupported fstp conversion paths, with a
  // comment promising "a clear SIGILL". Without a thunk that path core-dumps
  // instead of failing loudly, and it is reachable by any guest doing
  // `fstp dword`/`fstp qword` from a non-80-bit stack value.
  //
  // Required=true matters: it blocks both downgrades in UpdateHostThunk, so a
  // guest setting SIG_DFL or SIG_IGN cannot strip our thunk and leave the
  // sentinel or a Break-generated trap unhandled.
  const auto SigtrapHandler = [](FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext) -> bool {
    return FEX::HLE::ThreadManager::GetStateObjectFromFEXCoreThread(Thread)->SignalInfo.Delegator->HandleSIGILL(Thread, Signal, info, ucontext);
  };
  RegisterHostSignalHandler(SIGTRAP, SigtrapHandler, true);

#ifdef ARCHITECTURE_arm64
  // Register SIGBUS signal handler.
  const auto SigbusHandler = [](FEXCore::Core::InternalThreadState* Thread, int Signal, void* _info, void* ucontext) -> bool {
    const auto PC = ArchHelpers::Context::GetPc(ucontext);
    if (!Thread->CTX->IsAddressInCodeBuffer(Thread, PC)) {
      // Wasn't a sigbus in JIT code
      return false;
    }
    siginfo_t* info = reinterpret_cast<siginfo_t*>(_info);

    if (info->si_code != BUS_ADRALN) {
      // This only handles alignment problems
      return false;
    }

    FEXCORE_PROFILE_INSTANT_INCREMENT(Thread, AccumulatedSIGBUSCount, 1);
    const auto Delegator = FEX::HLE::ThreadManager::GetStateObjectFromFEXCoreThread(Thread)->SignalInfo.Delegator;
    const auto Result = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(Thread, Delegator->GetUnalignedHandlerType(), PC,
                                                                           ArchHelpers::Context::GetArmGPRs(ucontext));
    ArchHelpers::Context::SetPc(ucontext, PC + Result.value_or(0));
    return Result.has_value();
  };

  RegisterHostSignalHandler(SIGBUS, SigbusHandler, true);
#endif

#ifdef ARCHITECTURE_ppc64le
  // PPC64LE SIGBUS handler -- split-lock safety net.
  //
  // POWER8's ldarx/lwarx/lharx/lbarx require natural alignment; an x86
  // LOCK RMW on a misaligned EA would otherwise SIGBUS with si_code=
  // BUS_ADRALN. The PPC64LE JIT emits an inline alignment check on
  // every atomic and routes misaligned EAs through
  // PPC64_SplitLockEmulate (process-wide mutex), so SIGBUS from an
  // atomic LL is not expected in normal codegen. This handler is the
  // safety net for any LL that slips through the inline check
  // (codegen regressions, future opt passes that elide the gate,
  // hardware oddities). On a recognized LL it decodes the LL+body+SC
  // pattern, dispatches to PPC64_SplitLockEmulate, writes the
  // pre-RMW value to the LL's RT register, and advances PC past the
  // bc.NE back-edge so the thread doesn't loop on a faulting
  // reservation.
  const auto SigbusHandlerPPC64 = [](FEXCore::Core::InternalThreadState* Thread, int Signal, void* _info, void* ucontext) -> bool {
    const auto PC = ArchHelpers::Context::GetPc(ucontext);
    if (!Thread->CTX->IsAddressInCodeBuffer(Thread, PC)) {
      // SIGBUS outside JIT code -- let default handling (or a guest
      // handler) take it.
      return false;
    }
    siginfo_t* info = reinterpret_cast<siginfo_t*>(_info);
    if (info->si_code != BUS_ADRALN) {
      // We only own alignment SIGBUS. Other si_codes (BUS_ADRERR,
      // BUS_OBJERR, BUS_MCEERR_*) are real faults.
      return false;
    }

    FEXCORE_PROFILE_INSTANT_INCREMENT(Thread, AccumulatedSIGBUSCount, 1);
    const auto Result = FEXCore::ArchHelpers::PPC64::HandleUnalignedAtomicSIGBUS(
      Thread, PC, ArchHelpers::Context::GetArmGPRs(ucontext));
    if (Result.has_value()) {
      ArchHelpers::Context::SetPc(ucontext, PC + Result.value());
      return true;
    }
    // HandleUnalignedAtomicSIGBUS only returns an advance on a
    // recognized LL/SC pattern. If we don't recognize the LL (CAS,
    // an unfamiliar emit pattern, a SIGBUS from non-atomic JIT
    // code), let the fault escape -- a faulting non-atomic LD/ST
    // should crash the thread rather than silently advance past it.
    return false;
  };

  RegisterHostSignalHandler(SIGBUS, SigbusHandlerPPC64, true);
#endif
  // Register pause signal handler.
  RegisterHostSignalHandler(SignalDelegator::SIGNAL_FOR_PAUSE, PauseHandler, true);

  // Guest signal handlers.
  for (uint32_t Signal = 0; Signal <= SignalDelegator::MAX_SIGNALS; ++Signal) {
    RegisterHostSignalHandlerForGuest(Signal, GuestSignalHandler);
  }
}

SignalDelegator::~SignalDelegator() {
  for (int i = 0; i < MAX_SIGNALS; ++i) {
    if (i == 0 || i == SIGKILL || i == SIGSTOP || !HostHandlers[i].Installed) {
      continue;
    }
    ::syscall(SYS_rt_sigaction, i, &HostHandlers[i].OldAction, nullptr, 8);
    HostHandlers[i].Installed = false;
  }
}

void SignalDelegator::RegisterTLSState(FEX::HLE::ThreadStateObject* Thread) {
  FEXCore::Allocator::RegisterTLSData(Thread->Thread);

  Thread->SignalInfo.Delegator = this;

  // Set up our signal alternative stack
  // This is per thread rather than per signal
  Thread->SignalInfo.AltStackPtr = FEXCore::Allocator::mmap(nullptr, SIGSTKSZ * 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  FEXCore::Allocator::VirtualName("FEXMem_Misc", reinterpret_cast<void*>(Thread->SignalInfo.AltStackPtr), SIGSTKSZ * 16);
  stack_t altstack {};
  altstack.ss_sp = reinterpret_cast<void*>(reinterpret_cast<uint64_t>(Thread->SignalInfo.AltStackPtr) + 8);
  altstack.ss_size = SIGSTKSZ * 16 - 8;
  altstack.ss_flags = 0;
  LOGMAN_THROW_A_FMT(!!altstack.ss_sp, "Couldn't allocate stack pointer");

  // Copy the thread object to the start of the alt-stack
  memcpy(Thread->SignalInfo.AltStackPtr, &Thread, sizeof(void*));

  // Protect the first page of the alt-stack for overflow protection.
  // HOST: alt-stack overflow guard. mprotect rounds the length up to the host page, so
  // saying so explicitly is what keeps the guard from silently eating 60K of alt stack.
  mprotect(Thread->SignalInfo.AltStackPtr, FEXCore::HostPage::Size(), PROT_READ);

  // Register the alt stack
  const int Result = sigaltstack(&altstack, nullptr);
  if (Result == -1) {
    LogMan::Msg::EFmt("Failed to install alternative signal stack {}", strerror(errno));
  }

  // Get the current host signal mask
  ::syscall(SYS_rt_sigprocmask, 0, nullptr, &Thread->SignalInfo.CurrentSignalMask.Val, 8);

  if (Thread->Thread) {
    // Reserve a small amount of deferred signal frames. Usually the stack won't be utilized beyond
    // 1 or 2 signals but add a few more just in case.
    Thread->SignalInfo.DeferredSignalFrames.reserve(8);
  }
}

void SignalDelegator::UninstallTLSState(FEX::HLE::ThreadStateObject* Thread) {
  FEXCore::Allocator::munmap(Thread->SignalInfo.AltStackPtr, SIGSTKSZ * 16);

  Thread->SignalInfo.AltStackPtr = nullptr;

  stack_t altstack {};
  altstack.ss_flags = SS_DISABLE;

  // Uninstall the alt stack
  const int Result = sigaltstack(&altstack, nullptr);
  if (Result == -1) {
    LogMan::Msg::EFmt("Failed to uninstall alternative signal stack {}", strerror(errno));
  }

  FEXCore::Allocator::UninstallTLSData(Thread->Thread);
}

void SignalDelegator::FrontendRegisterHostSignalHandler(int Signal, bool Required) {
  // Linux signal handlers are per-process rather than per thread
  // Multiple threads could be calling in to this
  std::lock_guard lk(HostDelegatorMutex);
  HostHandlers[Signal].Required = Required;
  InstallHostThunk(Signal);
}

void SignalDelegator::FrontendRegisterFrontendHostSignalHandler(int Signal, bool Required) {
  // Linux signal handlers are per-process rather than per thread
  // Multiple threads could be calling in to this
  std::lock_guard lk(HostDelegatorMutex);
  HostHandlers[Signal].Required = Required;
  InstallHostThunk(Signal);
}

void SignalDelegator::RegisterHostSignalHandlerForGuest(int Signal, FEX::HLE::HostSignalDelegatorFunctionForGuest Func) {
  std::lock_guard lk(HostDelegatorMutex);
  HostHandlers[Signal].GuestHandler = std::move(Func);
}

void SignalDelegator::RegisterFrontendHostSignalHandler(int Signal, HostSignalDelegatorFunction Func, bool Required) {
  SetFrontendHostSignalHandler(Signal, std::move(Func), Required);
  FrontendRegisterFrontendHostSignalHandler(Signal, Required);
}

uint64_t SignalDelegator::RegisterGuestSignalHandler(int Signal, const GuestSigAction* Action, GuestSigAction* OldAction) {
  std::lock_guard lk(GuestDelegatorMutex);

  // Invalid signal specified
  if (Signal > MAX_SIGNALS) {
    return -EINVAL;
  }

  // If we have an old signal set then give it back
  if (OldAction) {
    *OldAction = HostHandlers[Signal].GuestAction;
  }

  // Now assign the new action
  if (Action) {
    // These signal dispositions can't be changed on Linux
    if (Signal == SIGKILL || Signal == SIGSTOP) {
      return -EINVAL;
    }

    HostHandlers[Signal].GuestAction = *Action;
    // Only attempt to install a new thunk handler if we were installing a new guest action
    if (!InstallHostThunk(Signal)) {
      UpdateHostThunk(Signal);
    }
  }

  return 0;
}

void SignalDelegator::CheckXIDHandler() {
  std::lock_guard lk(GuestDelegatorMutex);
  std::lock_guard lk2(HostDelegatorMutex);

  constexpr size_t SIGNAL_SETXID = 33;

  kernel_sigaction CurrentAction {};

  // Only update the old action if we haven't ever been installed
  const int Result = ::syscall(SYS_rt_sigaction, SIGNAL_SETXID, nullptr, &CurrentAction, 8);
  if (Result < 0) {
    LogMan::Msg::AFmt("Failed to get status of XID signal");
    return;
  }

  SignalHandler& HostHandler = HostHandlers[SIGNAL_SETXID];
  if (CurrentAction.handler != HostHandler.HostAction.handler) {
    // GLIBC overwrote our XID handler, reinstate our handler
    const int Result = ::syscall(SYS_rt_sigaction, SIGNAL_SETXID, &HostHandler.HostAction, nullptr, 8);
    if (Result < 0) {
      LogMan::Msg::AFmt("Failed to reinstate our XID signal handler {}", strerror(errno));
    }
  }
}

uint64_t SignalDelegator::RegisterGuestSigAltStack(FEX::HLE::ThreadStateObject* Thread, const stack_t* ss, stack_t* old_ss) {
  // Mirrors kernel/signal.c do_sigaltstack for the arm64 guest.
  auto& Current = Thread->SignalInfo.GuestAltStack;
  const uint64_t Base = reinterpret_cast<uint64_t>(Current.ss_sp);
  const uint64_t GuestSP = Thread->Thread->CurrentFrame->State.sp;
  const bool Disarmed = (Current.ss_flags & SS_AUTODISARM) != 0;
  const bool OnStack = !Disarmed && Current.ss_size != 0 && GuestSP > Base && GuestSP - Base <= Current.ss_size;

  if (old_ss) {
    *old_ss = stack_t {};
    old_ss->ss_sp = Current.ss_sp;
    old_ss->ss_size = Current.ss_size;
    old_ss->ss_flags = (Current.ss_size == 0 ? SS_DISABLE : OnStack ? SS_ONSTACK : 0) | (Current.ss_flags & SS_AUTODISARM);
  }

  if (ss) {
    if (OnStack) {
      return -EPERM;
    }
    const int Mode = ss->ss_flags & ~SS_AUTODISARM;
    if (Mode != SS_DISABLE && Mode != SS_ONSTACK && Mode != 0) {
      return -EINVAL;
    }
    if (Mode == SS_DISABLE) {
      Current.ss_sp = nullptr;
      Current.ss_size = 0;
      Current.ss_flags = ss->ss_flags;
      return 0;
    }
    if (ss->ss_size < FEX::HLE::Arm64::ABI::GUEST_MINSIGSTKSZ) {
      return -ENOMEM;
    }
    Current = *ss;
  }

  return 0;
}

static void CheckForPendingSignals(const FEX::HLE::ThreadStateObject* Thread) {
  // Do we have any pending signals that became unmasked?
  uint64_t PendingSignals = ~Thread->SignalInfo.CurrentSignalMask.Val & Thread->SignalInfo.PendingSignals;
  if (PendingSignals != 0) {
    for (int i = 0; i < 64; ++i) {
      if (PendingSignals & (1ULL << i)) {
        FHU::Syscalls::tgkill(Thread->ThreadInfo.PID, Thread->ThreadInfo.TID, i + 1);
        // We might not even return here which is spooky
      }
    }
  }
}

uint64_t SignalDelegator::GuestSigProcMask(FEX::HLE::ThreadStateObject* Thread, int how, const uint64_t* set, uint64_t* oldset) {
  // The order in which we handle signal mask setting is important here
  // old and new can point to the same location in memory.
  // Even if the pointers are to same memory location, we must store the original signal mask
  // coming in to the syscall.
  // 1) Store old mask
  // 2) Set mask to new mask if exists
  // 3) Give old mask back
  auto OldSet = Thread->SignalInfo.CurrentSignalMask.Val;

  if (!!set) {
    uint64_t IgnoredSignalsMask = ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    if (how == SIG_BLOCK) {
      Thread->SignalInfo.CurrentSignalMask.Val |= *set & IgnoredSignalsMask;
    } else if (how == SIG_UNBLOCK) {
      Thread->SignalInfo.CurrentSignalMask.Val &= ~(*set & IgnoredSignalsMask);
    } else if (how == SIG_SETMASK) {
      Thread->SignalInfo.CurrentSignalMask.Val = *set & IgnoredSignalsMask;
    } else {
      return -EINVAL;
    }

    uint64_t HostMask = Thread->SignalInfo.CurrentSignalMask.Val;
    // Now actually set the host mask
    // This will hide from the guest that we are not actually setting all of the masks it wants
    for (size_t i = 0; i < MAX_SIGNALS; ++i) {
      if (HostHandlers[i + 1].Required.load(std::memory_order_relaxed)) {
        // If it is a required host signal then we can't mask it
        HostMask &= ~(1ULL << i);
      }
    }

    ::syscall(SYS_rt_sigprocmask, SIG_SETMASK, &HostMask, nullptr, 8);
  }

  if (!!oldset) {
    *oldset = OldSet;
  }

  CheckForPendingSignals(Thread);

  return 0;
}

uint64_t SignalDelegator::GuestSigPending(FEX::HLE::ThreadStateObject* Thread, uint64_t* set, size_t sigsetsize) {
  if (sigsetsize > sizeof(uint64_t)) {
    return -EINVAL;
  }

  *set = Thread->SignalInfo.PendingSignals;

  sigset_t HostSet {};
  if (sigpending(&HostSet) == 0) {
    uint64_t HostSignals {};
    for (size_t i = 0; i < MAX_SIGNALS; ++i) {
      if (sigismember(&HostSet, i + 1)) {
        HostSignals |= (1ULL << i);
      }
    }

    // Merge the real pending signal mask as well
    *set |= HostSignals;
  }
  return 0;
}

uint64_t SignalDelegator::GuestSigSuspend(FEX::HLE::ThreadStateObject* Thread, uint64_t* set, size_t sigsetsize) {
  if (sigsetsize > sizeof(uint64_t)) {
    return -EINVAL;
  }

  uint64_t IgnoredSignalsMask = ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

  // Backup the mask
  Thread->SignalInfo.PreviousSuspendMask = Thread->SignalInfo.CurrentSignalMask;
  // Set the new mask
  Thread->SignalInfo.CurrentSignalMask.Val = *set & IgnoredSignalsMask;
  sigset_t HostSet {};

  sigemptyset(&HostSet);

  for (int32_t i = 0; i < MAX_SIGNALS; ++i) {
    if (*set & (1ULL << i)) {
      sigaddset(&HostSet, i + 1);
    }
  }

  // Additionally we must always listen to SIGNAL_FOR_PAUSE
  // This technically forces us in to a race but should be fine
  // SIGBUS and SIGILL can't happen so we don't need to listen for them
  // sigaddset(&HostSet, SIGNAL_FOR_PAUSE);

  // Spin this in a loop until we aren't sigsuspended
  // This can happen in the case that the guest has sent signal that we can't block
  uint64_t Result = sigsuspend(&HostSet);
  const int SuspendErrno = errno;

  // The signal that ended the suspend was only queued: the whole syscall body
  // is a deferred-signal section (HandleSyscallImpl), and the queue normally
  // drains when that section ends. By then the mask below has been restored,
  // so a signal the caller blocks outside sigsuspend (the usual pattern, e.g.
  // busybox ash's `wait`) went back to pending and its handler did not run
  // before sigsuspend returned, as Linux guarantees. Drain it here instead,
  // with the suspend mask still in effect. Only the outermost section may
  // drain, which is the case unless FEX itself is nested around this call.
  auto* CurrentFrame = Thread->Thread->CurrentFrame;
  if (!Thread->SignalInfo.DeferredSignalFrames.empty() && CurrentFrame->State.DeferredSignalRefCount.Load() == 1) {
    CurrentFrame->State.DeferredSignalRefCount.Decrement(1);
    // Faults while any deferred frame is queued; each guest handler's
    // rt_sigreturn resumes this store, which faults again for the next frame.
    reinterpret_cast<FEXCore::Core::NonAtomicRefCounter<uint64_t>*>(CurrentFrame->InterruptFaultPagePtr)->Store(0);
    CurrentFrame->State.DeferredSignalRefCount.Increment(1);
  }

  // Restore Previous signal mask we are emulating
  // XXX: Might be unsafe if the signal handler adjusted the thread's signal mask
  // But since we don't support the guest adjusting the mask through the context object
  // then this is safe-ish
  Thread->SignalInfo.CurrentSignalMask = Thread->SignalInfo.PreviousSuspendMask;

  CheckForPendingSignals(Thread);

  return Result == -1 ? -SuspendErrno : Result;
}

uint64_t SignalDelegator::GuestSigTimedWait(uint64_t* set, siginfo_t* info, const struct timespec* timeout, size_t sigsetsize) {
  if (sigsetsize > sizeof(uint64_t)) {
    return -EINVAL;
  }

  uint64_t Result = ::syscall(SYS_rt_sigtimedwait, set, info, timeout);

  return Result == -1 ? -errno : Result;
}

uint64_t SignalDelegator::GuestSignalFD(int fd, const uint64_t* set, size_t sigsetsize, int flags) {
  if (sigsetsize > sizeof(uint64_t)) {
    return -EINVAL;
  }

  sigset_t HostSet {};
  sigemptyset(&HostSet);

  for (size_t i = 0; i < MAX_SIGNALS; ++i) {
    if (HostHandlers[i + 1].Required.load(std::memory_order_relaxed)) {
      // For now skip our internal signals
      continue;
    }

    if (*set & (1ULL << i)) {
      sigaddset(&HostSet, i + 1);
    }
  }

  // XXX: This is a barebones implementation just to get applications that listen for SIGCHLD to work
  // In the future we need our own listern thread that forwards the result
  // Thread is necessary to prevent deadlocks for a thread that has signaled on the same thread listening to the FD and blocking is enabled
  uint64_t Result = signalfd(fd, &HostSet, flags);

  return Result == -1 ? -errno : Result;
}

fextl::unique_ptr<FEX::HLE::SignalDelegator>
CreateSignalDelegator(FEXCore::Context::Context* CTX, const std::string_view ApplicationName, bool SupportsAVX) {
  return fextl::make_unique<FEX::HLE::SignalDelegator>(CTX, ApplicationName, SupportsAVX);
}
} // namespace FEX::HLE
