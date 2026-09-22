// SPDX-License-Identifier: MIT
// PPC64LE JIT dispatcher implementation.
// Generates the native code glue that:
//   - Enters JIT execution from C++ (DispatchPtr)
//   - Implements the block-dispatch loop (read RIP → L1 lookup → branch/link)
//   - Exits JIT back to C++ (ThreadStop, signal stubs)
//   - Re-enters JIT for callbacks (CallbackPtr)
//
// Layout (all generated into a single code buffer):
//   [DispatchPtr]                   entry from C++
//   [DispatcherLoopTop]             dispatch loop
//   [ExitFunctionLinker]            slow-path exit / block linker call
//   [ThreadStopHandler]             unwind to C++
//   [ThreadPauseHandlerSpillSRA]    pause with SRA spill
//   [ThreadPauseHandler]            pause sleep call + illegal instruction trap
//   [GuestSignal_SIGILL]            signal stubs
//   [GuestSignal_SIGTRAP]
//   [GuestSignal_SIGSEGV]
//   [SignalReturnHandler]           stub
//   [CallbackPtr]                   JIT callback re-entry

#include "Interface/Core/JIT/PPC64LE/PPC64Dispatcher.h"
#include "Interface/Core/JIT/PPC64LE/JITClass.h" // for PPC64JITCore::ExitFunctionLinkWithRecord
#include "Interface/Context/Context.h"
#include "Interface/Core/LookupCache.h"
#include "Utils/MemberFunctionToPointer.h"

#include <bit>

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/ArchHelpers/PPC64CacheFlush.h>
#include <FEXCore/Utils/EnumUtils.h>
#include <FEXCore/Utils/LogManager.h>

#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

#include <sched.h>

namespace {
// Called from the pause handler: park the current thread until it is woken.
static void SleepThread(FEXCore::Context::ContextImpl* CTX, FEXCore::Core::CpuStateFrame* Frame) {
  CTX->SyscallHandler->SleepThread(CTX, Frame);
}
// Called from JIT-emitted Yield when guest PAUSE has fired enough times in a
// row that we want to surrender the CPU to another thread on the host. The
// SMT priority hint (or 27,27,27) emitted inline is a per-core nudge only;
// sched_yield() is the only way to give the kernel a chance to schedule a
// different thread (e.g. the lock-holder).
extern "C" void PPC64_PauseSchedYield() {
  sched_yield();
}
} // namespace

// DEBUG: visible to gdb post-mortem. Updated at every dispatcher fast-path
// entry with the guest RIP being dispatched.
//
// This is port-local scaffolding with no ARM64 equivalent, and it is not free:
// the emit in DispatcherLoopTop below costs 23 instructions (3 stores + 1 load).
// These are alignas(64) process-globals written by every guest thread, so on a
// multithreaded guest the cost is cross-core cacheline coherence traffic, not
// just wasted instructions.
//
// Coverage note: since DEF_OP(ExitFunction) inlined the L1 probe, a JIT block
// exit that HITS in L1 never reaches DispatcherLoopTop, so it is not recorded
// here. What still passes through is L1 misses (via the linker's re-dispatch),
// signal/pause resumes, thread starts and callback entries. The trail is a
// dispatch-slow-path log now, not a block-transition log.
//
// Note for anyone tempted to re-enable this by default: the ring buffer was
// ALREADY unreliable on multithreaded guests. g_dispatch_count is a racing
// non-atomic read-modify-write shared by all guest threads, so threads read the
// same index and store into the same slot; the recorded history is an
// interleaved mix with silent collisions.
//
// Build with -DENABLE_ASSERTIONS=TRUE to get the trace back.
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
extern "C" {
  alignas(64) uint64_t g_last_dispatched_rip = 0;
  alignas(64) uint64_t g_dispatch_count = 0;
  alignas(64) uint64_t g_recent_rips[16] = {};   // ring buffer (count & 15)
}
#endif

namespace FEXCore::CPU {

static constexpr size_t DISPATCHER_CODE_SIZE = 65536;

fextl::unique_ptr<PPC64Dispatcher>
PPC64Dispatcher::Create(FEXCore::Context::ContextImpl* CTX) {
  return fextl::make_unique<PPC64Dispatcher>(CTX);
}

PPC64Dispatcher::PPC64Dispatcher(FEXCore::Context::ContextImpl* CTX)
  : PPC64EmitterBase(CTX, nullptr, 0), CTX(CTX) {
  // Allocate an executable code buffer for the dispatcher
  void* Mem = mmap(nullptr, DISPATCHER_CODE_SIZE,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  LOGMAN_THROW_A_FMT(Mem != MAP_FAILED, "Failed to allocate dispatcher code buffer");

  SetBuffer(static_cast<uint8_t*>(Mem), DISPATCHER_CODE_SIZE);
  EmitDispatcher();

  // Flush D-cache to memory and invalidate I-cache for the freshly emitted
  // dispatcher. POWER8 has split, non-coherent I/D caches; without this the
  // CPU may execute stale bytes the I-cache happened to fetch on the same
  // physical line. mprotect(PROT_EXEC) typically (but not contractually) does
  // this, so we still issue it explicitly to match ARM64 behaviour.
  // (EmitDispatcher already flushes the range it actually emitted, so this is
  // belt-and-braces over the whole 64KiB buffer — 512 blocks, once per process.)
  FEXCore::ArchHelpers::PPC64::FlushICacheRange(Mem, DISPATCHER_CODE_SIZE);

  // Make the buffer read-only+executable after generation
  mprotect(Mem, DISPATCHER_CODE_SIZE, PROT_READ | PROT_EXEC);

  // G1(a): the shared spill island. Emitted here (per-context, single-threaded,
  // before any guest thread's InitThreadPointers) so the per-thread
  // Pointers.SpillIsland{Exit,Link} slots written in InitThreadPointers below
  // always see a valid, flushed island.
  EmitSpillIsland();
}

void PPC64Dispatcher::EmitSpillIsland() {
  // One small executable region for the two shared miss-leg spill stubs. Kept
  // separate from the dispatcher's own mmap so the dispatcher's
  // [DispatcherBegin, DispatcherEnd) range (used by IsAddressInDispatcher and
  // the signal delegator's "not JIT, SRA already spilled" branch) does NOT
  // include it — the island is the SRA spill itself and must be treated as
  // "SRA may be live" (see CPUBackend::IsAddressInCodeBuffer).
  constexpr size_t IslandSize = 4096;
  void* Mem = mmap(nullptr, IslandSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  LOGMAN_THROW_A_FMT(Mem != MAP_FAILED, "Failed to allocate the spill island");

  // A second emitter: this dispatcher's own emitter is bound to its buffer.
  PPC64EmitterBase IslandEmitter(CTX, Mem, IslandSize);
  auto Stubs = IslandEmitter.EmitSpillStubs(offsetof(PPC64BlockLinkRecord, StubAddr));
  const size_t Emitted = IslandEmitter.GetOffset();

  FEXCore::ArchHelpers::PPC64::FlushICacheRange(Mem, Emitted);
  mprotect(Mem, IslandSize, PROT_READ | PROT_EXEC);
  FEXCore::Allocator::VirtualName("POWERarmSpillIsland", Mem, IslandSize);

  SpillIslandBase    = reinterpret_cast<uint64_t>(Mem);
  SpillIslandSize    = Emitted;
  SpillIslandExitAddr = SpillIslandBase + Stubs.Exit;
  SpillIslandLinkAddr = SpillIslandBase + Stubs.Link;
}

void PPC64Dispatcher::InitThreadPointers(FEXCore::Core::InternalThreadState* Thread) {
  auto& Ptrs = Thread->CurrentFrame->Pointers;

  Ptrs.DispatcherLoopTop         = DispatcherLoopTopAddress;
  Ptrs.DispatcherLoopTopFillSRA  = DispatcherLoopTopFillSRAAddress;
  Ptrs.ExitFunctionLinker        = ExitFunctionLinkerAddress;
  // ExitFunctionLinkerWithRecordAddress is NOT copied to a Pointers slot —
  // its address is read via the dispatcher getter at CompileCode time and
  // written into each PPC64BlockLinkRecord::StubAddr, so the thunk's
  // LinkPath ld reads it off the record.  Retiring the frame slot keeps
  // InternalThreadState within its 2-page budget.
  // G1(a): the shared spill island's stub entry points. Identical in every
  // thread (the island is context-lifetime); blocks reach them with
  // `ld TMP1, <slot>(STATE); mtctr; bctr`, which survives the code cache
  // relocating the block. Written after EmitSpillIsland (constructor), so
  // both addresses are valid here.
  Ptrs.SpillIslandExit = SpillIslandExitAddr;
  Ptrs.SpillIslandLink = SpillIslandLinkAddr;
  Ptrs.ThreadStopHandlerSpillSRA = ThreadStopHandlerAddressSpillSRA;
  Ptrs.ThreadPauseHandlerSpillSRA = ThreadPauseHandlerAddressSpillSRA;
  Ptrs.GuestSignal_SIGILL        = GuestSignal_SIGILL_Address;
  Ptrs.GuestSignal_SIGTRAP       = GuestSignal_SIGTRAP_Address;
  Ptrs.GuestSignal_SIGSEGV       = GuestSignal_SIGSEGV_Address;
  Ptrs.SignalReturnHandler       = SignalHandlerReturnAddress;
  Ptrs.SignalReturnHandlerRT     = SignalHandlerReturnAddressRT;
  Ptrs.PPC64_PauseSchedYield     = reinterpret_cast<uint64_t>(&PPC64_PauseSchedYield);

}

// ============================================================
// The generated dispatcher code
// ============================================================
void PPC64Dispatcher::EmitDispatcher() {
  using namespace FEXCore::Core;
  DispatcherBegin = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());

  // ==============================================================
  // DispatchPtr  (called from C++ with C-ABI: r3=Frame, r4=SingleInst)
  // ==============================================================
  DispatchPtr = reinterpret_cast<AsmDispatch>(GetCursorAddress<uint8_t*>());

  // Establish a standard ELFv2 stack frame for the C ABI.
  PushCalleeSavedRegisters();

  // Establish the VZERO_VSX invariant for the life of this dispatcher frame:
  // vs14 == f14 is callee-saved (Push/PopCalleeSavedRegisters handle the
  // outer caller's value), non-volatile across every host call the JIT makes,
  // and written nowhere else. See the VZERO_VSX comment in PPC64Emitter.h.
  //
  // CAVEAT (learned from the AES pinned-mask incident): the ELFv2 guarantee
  // covers only the FPR half (dw0). The vector half (dw1) is volatile, and a
  // host callee's scalar lfd f14 restore leaves it UNDEFINED per the ISA.
  // Consumers may therefore only rely on VZERO's dw0 being zero after any
  // host call; nothing may read its full 128 bits across one. A pinned
  // full-width vector constant in vs14-vs31 is not implementable this way -
  // materialize such constants per-use instead (see EmitAESLoadMask).
  xxlxor(VZERO_VSX, VZERO_VSX, VZERO_VSX);

  // STATE (r27) = Frame* (r3)
  mr(STATE, r3);

  // Record the host stack pointer as the "returning stack location" so that
  // when a Stop/Pause signal fires outside the JIT (e.g. while the thread is
  // inside a glibc syscall wrapper called from the JIT Syscall op), the signal
  // handler can reset r1 to a valid dispatcher frame and PopCalleeSavedRegisters
  // will correctly restore callee-saved regs and blr back to the C++ caller.
  // Without this, Frame->ReturningStackLocation stays 0 (its default) and
  // SetSp(ucontext, 0) corrupts the stack pointer → cascading crash.
  {
    int32_t rsl_off = static_cast<int32_t>(
      offsetof(CpuStateFrame, ReturningStackLocation));
    std(r1, static_cast<int16_t>(rsl_off), STATE);
  }

  // Fill-SRA entry point: signal-return / pause-resume jump here so SRA is
  // reloaded from Frame->State before falling into the L1 lookup loop.
  //
  // SMC single-instruction recovery: the SMC SIGSEGV handler (SyscallsSMCTracking.cpp
  // HandleSegfault) sets r4=1 in the kernel ucontext when it needs the next JIT
  // entry to recompile as a single-instruction block (so a racing SMC write to
  // the same page is observed before the rest of the original block runs).
  // Mirrors ARM64's ENTRY_FILL_SRA_SINGLE_INST_REG / cbnz pattern
  // (Dispatcher.cpp:89-94). Check BEFORE FillStaticRegs because PPC's
  // FillStaticRegs uses TMP2 (=r4) as scratch in its NZCV unpack stage.
  //
  // The C++ fall-through path (main dispatcher entry from `ExecuteDispatch`)
  // must arrive here with r4 == 0, or the compare-and-branch below will
  // always take the CompileSingleStep exit. `PushCalleeSavedRegisters` at :129
  // clobbers TMP2 (== r4) twice: once via `mfcr(TMP2)` (PPC64Emitter.cpp:279),
  // then finally via `LoadImm32(TMP2, 192)` in the VMX save loop
  // (PPC64Emitter.cpp:298). So on fall-through, r4 is DETERMINISTICALLY 192,
  // and the branch fires on every dispatcher entry — one wasted compile per
  // call, self-corrected by the SingleStep tail's fallback to normal
  // compilation but pure waste. The `li(r4, 0)` below is emitted BEFORE the
  // label, so the fall-through path clears r4 while signal/SMC arrivals jump
  // TO the label and keep whatever the kernel set in the ucontext r4.
  li(r4, 0);
  PPC64Emitter::Label CompileSingleStepLabel{};
  // Dispatcher-internal second entry to ExitFunctionLinker; see the slow-path
  // branch in DispatcherLoopTop below and the linker's prologue.
  PPC64Emitter::Label exit_linker_spill_sra{};
  // Failure-exit forward-branch target for the CompileSingleStep body below.
  // Bound at ThreadStopHandlerAddress (post-SpillStaticRegs) because SRA was
  // already spilled by the SMC SIGSEGV handler and the current host r7-r12
  // hold ELFv2-volatile garbage after the C++ `bctrl` — SpillStaticRegs here
  // would clobber the correct spilled state. Only `Pointers.ThreadStopHandlerSpillSRA`
  // is exposed via CoreState.h; the no-spill entry is reachable only in-emitter,
  // hence this label.
  PPC64Emitter::Label ThreadStopNoSpillLabel{};
  DispatcherLoopTopFillSRAAddress = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  // Signal-return / pause-resume can land here from a redirected host context
  // whose f14 was a C++ callee's live value, not this frame's zero — the
  // DispatchPtr prologue's xxlxor didn't run on that path. Re-establish the
  // VZERO_VSX invariant on this cold entry before refilling SRA.
  xxlxor(VZERO_VSX, VZERO_VSX, VZERO_VSX);
  cmpdi(r4, 0);
  bc(CC_NE, &CompileSingleStepLabel);
  FillStaticRegs();

  // Fall through into DispatcherLoopTop

  // ==============================================================
  // DispatcherLoopTop: entered with SRA registers filled, STATE valid.
  // Load RIP, do L1 lookup, branch to JIT block or slow path.
  // ==============================================================
  DispatcherLoopTopAddress = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());

  {
    // Re-establish the r0=0 zero-index invariant before dispatching into any
    // JIT block.
    //
    // Every guest load/store the backend emits is an X-form indexed access with
    // r0 in the rB slot -- ldx(GDst, EA, r0), stdx(GSrc, EA, r0), and ~50 more
    // in MemoryOps.cpp -- so the effective address is EA + r0. Note that PPC's
    // "r0 reads as zero" rule applies only to the rA operand, NOT to rB: in
    // these instructions r0 is an ordinary register whose contents are added to
    // the address. A nonzero r0 therefore silently offsets EVERY memory access
    // in the block, producing garbage loads whose values are stored into guest
    // state and only fault much later somewhere unrelated.
    //
    // FillStaticRegs deliberately leaves r0 alone (ExitFunctionLinker smuggles
    // the resolved host code pointer through it), and only some entry paths
    // restore it: FillForABICall, the ExitFunctionLinker slow path, and the
    // syscall return. The DispatcherLoopTopFillSRA entry above -- which is where
    // HandleSegfault redirects an SMC fault, and where signal and thread-start
    // paths re-enter -- did not, leaving the invariant dependent on whatever r0
    // happened to hold. Do it here so it holds for every dispatcher-mediated
    // block entry regardless of how we got here. One instruction on a path that
    // already performs several loads.
    li(r(0), 0);

    // Spin-loop SMT priority safety net: a thread can leave a hint-marked
    // spin region through a signal/suspend detour instead of a marked exit
    // edge, arriving here still at very-low priority. Restore medium (the
    // default) on every dispatcher-mediated entry. Architectural nop
    // otherwise; see AnalyzeSpinLoops in JIT.cpp.
    smt_medium_priority();

    // Load current RIP from Frame->State.rip
    int32_t rip_off = static_cast<int32_t>(offsetof(CpuStateFrame, State.pc));
    ld(TMP1, rip_off, STATE);  // TMP1 = guest RIP
    // 32-bit guest: mask RIP to 32 bits so the L1 hash and the GuestCode
    // compare both use a canonical 32-bit value. ARM64 dispatcher does the
    // analogous `and_(VirtualMemorySize-1)` (Dispatcher.cpp:188-191).

#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
    // DEBUG: log RIP to globals so gdb post-mortem can see the dispatch trail.
    // Assertions-only: see the note on the globals above for why this must not
    // run in Release. TMP2/TMP3 are clobbered here but both are unconditionally
    // reloaded from STATE immediately below, so removal needs no fixups.
    // Each LoadConstant of a global is 5 instructions (PIE, load base > 32 bits).
    LoadConstant(TMP2, reinterpret_cast<uint64_t>(&g_last_dispatched_rip));
    std(TMP1, 0, TMP2);
    LoadConstant(TMP2, reinterpret_cast<uint64_t>(&g_dispatch_count));
    ld(TMP3, 0, TMP2);
    addi(TMP3, TMP3, 1);
    std(TMP3, 0, TMP2);
    // Ring buffer: g_recent_rips[count & 15] = rip
    LoadConstant(TMP2, reinterpret_cast<uint64_t>(&g_recent_rips[0]));
    rldicl(TMP3, TMP3, 0, 60);   // count & 15
    sldi(TMP3, TMP3, 3);          // *8 for uint64 stride
    add(TMP2, TMP2, TMP3);
    std(TMP1, 0, TMP2);
#endif

    // Load L1Pointer and L1Mask from Frame->State.L1Pointer / L1Mask
    // These are adjacent: L1Pointer at offset X, L1Mask at X+8.
    int32_t l1_off = static_cast<int32_t>(offsetof(CpuStateFrame, State.L1Pointer));
    int32_t l1mask_off = static_cast<int32_t>(offsetof(CpuStateFrame, State.L1Mask));

    ld(TMP2, l1_off, STATE);       // TMP2 = L1Pointer
    if (!FEXCore::Config::Get_DYNAMICL1CACHE()) {
      // Static L1 (the port default): the mask is an emit-time constant, so
      // the whole (RIP << 4) & L1Mask collapses into one rldic — rotate left
      // by 4 and keep bits [4, log2(entries)+4), which is exactly
      // (RIP & (entries-1)) * 16 with the low 4 bits clear. Replaces the
      // L1Mask load + sldi + and_ (3 insns and a dependent load) on the
      // hottest dispatcher leg. MB tracks LookupCache::MAX_L1_ENTRIES.
      static_assert((FEXCore::LookupCache::MAX_L1_ENTRIES & (FEXCore::LookupCache::MAX_L1_ENTRIES - 1)) == 0,
                    "rldic probe requires a power-of-two L1");
      constexpr uint32_t L1MB = 64 - (std::countr_zero(FEXCore::LookupCache::MAX_L1_ENTRIES) + 4);
      rldic(TMP4, TMP1, 4, L1MB);
    } else {
      ld(TMP3, l1mask_off, STATE);   // TMP3 = L1Mask (pre-scaled by sizeof(LookupCacheEntry)=16)

      // Compute byte offset: (RIP << 4) & L1Mask = (RIP & L1PointerMask) * 16.
      // L1Mask is pre-scaled (= L1PointerMask << 4), so shifting RIP left before
      // ANDing gives the correct entry offset. Matches ARM64 dispatcher behavior.
      sldi(TMP4, TMP1, 4);  // LookupCacheEntry = 16 bytes, log2(16) = 4
      and_(TMP4, TMP4, TMP3);
    }

    // Address of L1 entry: L1Pointer + byte_offset
    add(TMP2, TMP2, TMP4);

    // Load L1Entry: {HostCode (8 bytes), GuestCode (8 bytes)}.
    //
    // ORDERING NOTE (paired with LookupCacheEntry::Publish on the writer
    // side): we MUST load GuestCode FIRST and only then load HostCode, or a
    // concurrent writer mid-Publish leaves us with {stale_HostCode,
    // new_GuestCode}, we pass the cmpd, and we jump into a half-installed /
    // invalidated block -- the recurring zero-IR-block spin observed at
    // Steam's PLT-to-malloc stub.  ARM64 sidesteps this entirely with `stp`
    // (atomic store-pair); PPC64LE has no equivalent, hence the writer-side
    // release fence and the reader-side ordering below.
    //
    // CORRECTION (this used to claim the conditional branch was enough):
    // a CONTROL dependency does NOT order load->load on Power.  The branch
    // only orders load->store.  A load after a conditional branch may be
    // executed speculatively before the branch resolves, so `ld GuestCode;
    // cmpd; bc; ld HostCode` permits exactly the torn read this code exists
    // to prevent.  Power orders load->load only on an ADDRESS or DATA
    // dependency, and TMP2 (the entry address) is fully computed before both
    // loads, so there was no dependency to fall back on.
    //
    // Fix: manufacture a real address dependency from the GuestCode value
    // into the HostCode load's address (see below).  Two ALU ops, no barrier.
    // Power ISA Book II honors a register dependency even when the computed
    // value cannot actually vary, so `xor rX, rY, rY` (always 0) is a valid
    // ordering primitive here; `lwsync` between the two loads is the
    // conservative alternative.
    ld(TMP4, 8, TMP2);   // TMP4 = GuestCode (the "key")

    // Compare GuestCode with current RIP — into CR7, NOT CR0.
    //
    // Why CR7: SpillStaticRegs (run on the L1-miss leg below, at
    // ExitFunctionLinker's spilling entry) packs CR0 + XER into
    // flags[RFLAG_NZCV_LOC] as the canonical
    // NZCV storage for cross-block transfer. If we used the default CR0
    // here, any L1 miss would overwrite the correct (just-spilled-by-the-
    // JIT-block's-ExitFunction) NZCV with garbage from this lookup compare.
    // Symptom was a phantom SF=1 (and sometimes ZF=1) appearing in PUSHF /
    // LAHF after popfq=0 across ~25 ASM tests (Primary_9C/9D/84/85,
    // ShiftZeroFlagsUpdate, InitialPFFlag, BLSI_flags, etc.). Running cmpd
    // into CR7 leaves CR0 as set by FillStaticRegs.
    // Use cmpw in 32-bit mode: x86 RIP is 32-bit, and FEX zero-extends RIP
    // to 64 bits via MaybeClrUpper32 before the L1 lookup.  The cached
    // GuestCode is published as zero-extended-from-32 too, so cmpd is
    // correct.  But if any publisher were to leave non-zero upper bits in
    // either operand (32-bit IR ops in the future, signal-frame paths,
    // etc.), cmpd would falsely miss and take the slow path.  cmpw is
    // explicitly only-low-32 — it matches what x86 32-bit branch targets
    // logically are.
    cmpd(cr(7), TMP4, TMP1);

    // If mismatch, take slow path through ExitFunctionLinker.
    // BO=12 (branch if true), BI=30 (CR7.EQ at PPC bit 4*7+2 = 30).
    auto match_label = PPC64Emitter::Label{};
    bc({12, 30}, &match_label);

    // Slow path: jump to ExitFunctionLinker's SRA-SPILLING entry.
    //
    // Every path that reaches DispatcherLoopTop arrives with guest state live
    // in the SRA host registers and NOT yet written back to CpuStateFrame:
    // DispatcherLoopTopFillSRA falls through after FillStaticRegs, CallbackPtr
    // jumps here after FillStaticRegs, and ExitFunctionLinker's own
    // re-dispatch runs FillStaticRegs before branching. So the linker must
    // spill on this path. JIT blocks reach the linker by a different route --
    // they spill inline (BranchOps.cpp DEF_OP(ExitFunction)) and enter at
    // ExitFunctionLinkerAddress, which does not spill.
    b(&exit_linker_spill_sra);

    Bind(&match_label);
    // Fast path: JIT block found.  Feed the GuestCode VALUE into the address
    // of the HostCode load so the hardware cannot hoist the HostCode load
    // above the GuestCode load.  TMP3 is 0 by construction (x ^ x), so TMP2
    // is unchanged, but the load's address now carries a data dependency on
    // TMP4 -- which is what Power's memory model actually orders on.  The
    // conditional branch above is NOT sufficient (see the ORDERING NOTE).
    // The dependency rides the X-form index operand directly: ldx's EA is
    // TMP2 + TMP3 with TMP3 == 0, same ordering guarantee as the old
    // xor_/add/ld sequence, one instruction shorter on this hottest-path leg.
    xor_(TMP3, TMP4, TMP4);   // TMP3 = 0, data-dependent on the GuestCode load
    ldx(TMP3, TMP2, TMP3);    // TMP3 = HostCode (loaded under address-dep)
    mtctr(TMP3);
    li(r(0), 0);  // JIT blocks use r0=0 as zero index for ldx/stdx
    bctr();
  }

  // ==============================================================
  // ExitFunctionLinker — slow path when L1 cache misses.
  // Calls the C++ FindBlock / compile path, then re-dispatches.
  //
  // Wrapped in a DeferredSignalRefCount guard (mirror of ARM64's
  // EmitSignalGuardedRegion at Dispatcher.cpp:254-308 and the contract in
  // docs/DeferredSignals.md). Increment marks the dispatcher-resident C++
  // call as uninterruptible so async signals are deferred; decrement +
  // InterruptFaultPage byte-store drains any signal queued during the
  // uninterruptible region.
  //
  // Scratch register choice: TMP1 (r3) — NOT in SRA (PPC64Emitter.h:53 maps
  // r7-r12, r14-r23 to guest GPRs; r3-r6 are non-SRA caller-clobbered
  // scratch). r0 is NOT usable because addi/std with RA=r0 treat RA as
  // literal zero. r12 is NOT usable because it holds guest RBP after
  // FillStaticRegs reloads SRA (this bug caused a SIGSEGV on Steam launch
  // in commit-before-this; symptom was `ldx rX, r5, r0` after `addi r5,
  // r12, -120` crashing — guest RBP was the refcount value).
  //
  // TWO ENTRIES, distinguished by who owns the SRA spill:
  //
  //   exit_linker_spill_sra  (dispatcher-internal, not in Pointers)
  //       Reached only from DispatcherLoopTop's L1 miss, where guest state is
  //       live in host registers. Enters the deferred-signal guard, spills,
  //       falls into the body.
  //
  //   ExitFunctionLinkerAddress  (Pointers.ExitFunctionLinker)
  //       Reached from a JIT block's inlined L1 miss leg, which has ALREADY
  //       run SpillStaticRegs in JIT code. Enters the guard only.
  //
  // Why the JIT spills before branching rather than letting the linker do it:
  // SignalDelegator's SIGNAL_FOR_PAUSE / Stop handling picks the non-spilling
  // handler entry whenever IsAddressInCodeBuffer(PC) is false, and the
  // dispatcher lives in its own mmap, so a dispatcher PC always takes that
  // branch and reads guest state that only a completed spill makes valid.
  // (The LOGMAN_THROW_A_FMT that would catch this is inside
  // #if ASSERTIONS_ENABLED and is inert in Release.) Spilling in JIT code
  // keeps IsAddressInCodeBuffer a valid proxy for "SRA still in registers".
  //
  // The spill stays INSIDE the guard on the dispatcher entry: a guest signal
  // taken mid-spill at a dispatcher PC would otherwise build its frame from
  // half-written state.
  // ==============================================================
  const int32_t deferred_refcount_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, State.DeferredSignalRefCount));
  // 64K port: the interrupt fault page is an mmap'd host page whose address lives in
  // the frame, not an array embedded in InternalThreadState -- a page-sized D-form
  // displacement is unencodable once the host page is 64K. The poke below loads the
  // pointer and stores through it.
  const int32_t deferred_fault_ptr_off = static_cast<int32_t>(offsetof(FEXCore::Core::CpuStateFrame, InterruptFaultPagePtr));
  static_assert(offsetof(FEXCore::Core::CpuStateFrame, InterruptFaultPagePtr) + sizeof(void*) <= 32768,
                "InterruptFaultPagePtr must be reachable from STATE with a signed 16-bit D-form displacement");

  auto EmitDeferredSignalEnter = [&]() {
    // 3-instr non-atomic thread-local increment per docs/DeferredSignals.md.
    // TMP1 is scratch — the spill and the C-call setup below clobber it
    // again, so we can freely use it here.
    ld(TMP1, deferred_refcount_off, STATE);
    addi(TMP1, TMP1, 1);
    std(TMP1, deferred_refcount_off, STATE);
  };
  auto EmitDeferredSignalExit = [&]() {
    // 5-instr decrement + fault-page byte-store (the pointer load is the 5th;
    // see the note above deferred_fault_ptr_off). If a signal was queued
    // while refcount > 0, the page is mprotect'd PROT_NONE and the stb
    // traps SIGSEGV; the host handler picks the queued signal off the
    // per-thread stack and dispatches the guest handler.
    ld(TMP1, deferred_refcount_off, STATE);
    addi(TMP1, TMP1, -1);
    std(TMP1, deferred_refcount_off, STATE);
    // The refcount store has retired; TMP1 is free again. The value stored by the
    // stb is irrelevant -- only the fault matters.
    ld(TMP1, deferred_fault_ptr_off, STATE);
    stb(TMP1, 0, TMP1);
  };

  // Entry 1: SRA still live in host registers (dispatcher-internal).
  PPC64Emitter::Label exit_linker_body{};
  Bind(&exit_linker_spill_sra);
  {
    // Enter uninterruptible region BEFORE SpillStaticRegs so the spill
    // itself runs under the guard. SRA is live; r12 is non-SRA scratch.
    EmitDeferredSignalEnter();
    SpillStaticRegs(TMP1);
    b(&exit_linker_body);
  }

  // Entry 2: caller (a JIT block) has already spilled.
  ExitFunctionLinkerAddress = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  EmitDeferredSignalEnter();

  Bind(&exit_linker_body);
  {
    // Call PPC64JITCore::ExitFunctionLink(Frame, GuestRIP):
    //   r3 = Frame (CpuStateFrame*)
    //   r4 = current guest RIP
    // Returns r3 = host code address (0 if block can't be compiled).
    // TMP1=r3, TMP2=r4 — must NOT use TMP1/TMP2 to load the function ptr since they
    // hold r3/r4 (the arguments). Load into r12 per ELFv2 indirect-call convention.
    mr(r3, STATE);
    int32_t rip_off = static_cast<int32_t>(offsetof(CpuStateFrame, State.pc));
    ld(r4, rip_off, STATE);
    // 32-bit guest: pass a canonical 32-bit RIP to ExitFunctionLink so the
    // C++ side hashes / lookups match what the JIT block was compiled at.

    // Load function pointer into r12 (ELFv2 requires r12 == callee address for indirect calls)
    int32_t link_off = static_cast<int32_t>(
      offsetof(CpuStateFrame, Pointers.ExitFunctionLink));
    ld(r(12), link_off, STATE);
    mtctr(r(12));
    // Save/restore r2 (TOC) around indirect call as required by ELFv2
    std(r2, 24, r1);
    bctrl();
    ld(r2, 24, r1);

    // After returning, r3 contains the host code pointer (or 0 if not found).
    // If 0, stop execution (block not compilable).
    cmpdi(r3, 0);
    auto found_label = PPC64Emitter::Label{};
    bc(CC_NE, &found_label);

    // Block not found/compilable: reach the *non-spilling* ThreadStopHandler
    // entry (P5.0.3). Symmetric with the SMC single-step failure path a few
    // lines below: SRA was spilled before the C++ bctrl into ExitFunctionLink,
    // and host r7-r12/v0-v19 now hold ELFv2-volatile garbage. The
    // SpillSRA-entry variant would write that garbage over the already-correct
    // spilled state. Balance the deferred-signal counter first, then branch
    // to the label bound at ThreadStopHandlerAddress (post-SpillStaticRegs).
    // Low severity in practice — the thread is terminating — but the pre-P5.0
    // asymmetry with the SMC path was a correctness smell.
    {
      EmitDeferredSignalExit();
      b(&ThreadStopNoSpillLabel);
    }

    Bind(&found_label);
    // FillStaticRegs uses TMP1=r3 as scratch (unconditionally for XMM fills),
    // which would clobber the host code pointer in r3. Save it in r0 first.
    // r0 is safe: not in SRA, not touched by FillStaticRegs.
    mr(r(0), r3);
    FillStaticRegs();
    mtctr(r(0));
    // CTR now holds the host code pointer; exit the guard region. TMP1
    // (r3) is non-SRA scratch — FillStaticRegs already clobbered it and
    // the next JIT block expects it to be scratch, so reusing it for the
    // refcount decrement / fault store is safe. r0 still holds the host
    // code ptr (only used here to be reset to zero before bctr).
    EmitDeferredSignalExit();
    li(r(0), 0);  // JIT blocks use r0=0 as zero index for ldx/stdx
    bctr();
  }

  // ==============================================================
  // ExitFunctionLinkerWithRecord — block-linking variant.
  //
  // Entered ONLY from a link thunk's LinkPath leg (see CompileCode's thunk
  // emission in PPC64LE/JIT.cpp): the JIT block's miss leg has ALREADY run
  // SpillStaticRegs and the hoisted State.rip store, and the thunk left
  //   r4 (TMP2) = &PPC64BlockLinkRecord.
  // Mirror of "Entry 2" above with two differences: the second argument is
  // the record pointer instead of State.rip, and the C++ callee is
  // Pointers.ExitFunctionLinkWithRecord, which backpatches the exit before
  // returning the host code address. The dispatch tail (0 => thread stop,
  // else FillStaticRegs and bctr) is identical in behaviour to the classic
  // linker path; it is duplicated rather than shared so the existing body's
  // label structure stays untouched.
  //
  // r4 must survive from thunk to bctrl: EmitDeferredSignalEnter uses only
  // TMP1 (r3), and nothing below writes r4 before the call.
  // ==============================================================
  ExitFunctionLinkerWithRecordAddress = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  {
    EmitDeferredSignalEnter();

    // Call PPC64JITCore::ExitFunctionLinkWithRecord(Frame, Record):
    //   r3 = Frame (CpuStateFrame*)
    //   r4 = ExitFunctionLinkData* (set by the thunk, preserved here)
    // Returns r3 = host code address (0 if the block can't be compiled).
    //
    // Materialise the C++ function address as an inline constant rather than
    // loading it from a per-thread CpuStateFrame::Pointers slot — the target
    // is `static PPC64JITCore::ExitFunctionLinkWithRecord`, a plain function
    // pointer known at dispatcher-generation time, and eliminating the frame
    // slot keeps InternalThreadState within its 2-page budget after
    // C4.5/C6/C7 consumed the remaining headroom.  Cost is 5 instructions
    // (LoadConstant64) vs 1 ld, once per dispatcher generation; the stub is
    // reached once per unlinked-thunk hit, so aggregate is trivial.
    mr(r3, STATE);
    LoadConstant(r(12), reinterpret_cast<uint64_t>(&PPC64JITCore::ExitFunctionLinkWithRecord));
    mtctr(r(12));
    std(r2, 24, r1);
    bctrl();
    ld(r2, 24, r1);

    cmpdi(r3, 0);
    auto found_wr_label = PPC64Emitter::Label{};
    bc(CC_NE, &found_wr_label);

    // Not compilable: balance the deferred-signal counter, then stop via the
    // non-spilling entry — SRA was spilled before the bctrl and host
    // volatiles hold garbage now (same rationale as the classic path above).
    EmitDeferredSignalExit();
    b(&ThreadStopNoSpillLabel);

    Bind(&found_wr_label);
    // Same tail as the classic linker: park the host pointer in r0 across
    // FillStaticRegs (which clobbers TMP1=r3), drain the guard, restore the
    // r0=0 zero-index invariant, dispatch.
    mr(r(0), r3);
    FillStaticRegs();
    mtctr(r(0));
    EmitDeferredSignalExit();
    li(r(0), 0);  // JIT blocks use r0=0 as zero index for ldx/stdx
    bctr();
  }

  // ==============================================================
  // CompileSingleStep — SMC recovery path.
  // Entered from DispatcherLoopTopFillSRA when r4 (= TMP2 =
  // ENTRY_FILL_SRA_SINGLE_INST_REG) was non-zero, set by the SMC fault
  // handler via SetFillSRASingleInst.  SRA is NOT in host regs yet (the
  // FillStaticRegs at the entry was skipped via the branch). r1 still
  // points at the dispatcher's frame so a host C call is safe.
  //
  // Calls uintptr_t ContextImpl::CompileSingleStep(CpuStateFrame*, uint64_t)
  // which compiles JUST the next x86 instruction at State.rip as its own
  // JIT block and returns the host code pointer.  We then FillStaticRegs
  // and jump straight to that block, bypassing the L1 lookup (so a racing
  // stale cache entry can't be hit).
  // ==============================================================
  Bind(&CompileSingleStepLabel);
  {
    // Deferred-signal guard around the C++ CompileSingleStep call. Mirror of
    // the ExitFunctionLinker pattern above (see comments there) and the
    // contract in docs/DeferredSignals.md — the dispatcher-resident C++ call
    // must run uninterruptibly so async signals stay queued until decrement +
    // fault-page store drains them.
    EmitDeferredSignalEnter();

    // ContextImpl::CompileSingleStep is a non-static member.
    // ELFv2 calling convention: r3 = this (CTX), r4 = Frame, r5 = RIP.
    LoadConstant(r3, reinterpret_cast<uint64_t>(CTX));
    mr(r4, STATE);
    int32_t rip_off = static_cast<int32_t>(offsetof(CpuStateFrame, State.pc));
    ld(r5, rip_off, STATE);

    // MemberFunctionToPointerCast handles the data-vs-text-vs-thunk wrapping
    // that PPC64LE needs for non-static member function pointers.
    FEXCore::Utils::MemberFunctionToPointerCast PMFCompileSingleStep(
      &FEXCore::Context::ContextImpl::CompileSingleStep);
    LoadConstant(r(12), PMFCompileSingleStep.GetConvertedPointer());
    mtctr(r(12));

    // Save/restore TOC around the indirect C call per ELFv2.
    std(r2, 24, r1);
    bctrl();
    ld(r2, 24, r1);

    // Failure guard: CompileSingleStep returns 0 (uintptr_t) if the block
    // cannot be compiled (`Core.cpp:999-1001`). Without this check we would
    // `bctr` to address 0 and take an unrelated crash. Fall through on
    // success, branch to the non-spilling thread-stop on failure.
    cmpdi(r3, 0);
    auto ok_label = PPC64Emitter::Label{};
    bc(CC_NE, &ok_label);

    // Failure path: balance the deferred-signal counter (else refcount stays
    // elevated and every future signal in this thread silently defers), then
    // reach the *non-spilling* ThreadStopHandler entry. SRA was already
    // spilled by SyscallsSMCTracking.cpp:151 before it redirected here; host
    // r7-r12 currently hold ELFv2-volatile garbage from the CompileSingleStep
    // call, so SpillStaticRegs would clobber the correct spilled state.
    EmitDeferredSignalExit();
    b(&ThreadStopNoSpillLabel);

    Bind(&ok_label);
    // Success path: save r3 (host code pointer) into r0 BEFORE
    // EmitDeferredSignalExit clobbers TMP1=r3. r0 is not touched by Exit and
    // is also safe across FillStaticRegs (which uses TMP1=r3 as XMM scratch).
    mr(r(0), r3);
    EmitDeferredSignalExit();
  }
  FillStaticRegs();
  mtctr(r(0));
  li(r(0), 0);  // restore r0=0 zero-index invariant
  bctr();

  // ==============================================================
  // ThreadStopHandler — unwind JIT and return to C++ caller
  // ThreadStopHandlerAddressSpillSRA: entered when in JIT (spill first)
  // ThreadStopHandlerAddress:         entered when already spilled
  //
  // SignalDelegator::HandleSignalDeferred picks between the two based on
  // whether the interrupted PC was inside the JIT code buffer
  // (SignalDelegator.cpp:717-728). On the outside-JIT branch — thread
  // interrupted in host C++ — r27 is an ELFv2 non-volatile register
  // OWNED BY THE C++ FRAME and is NOT `STATE`. Running SpillStaticRegs
  // there emits ~34 stores at [r27 + offset], which land at wild
  // addresses (whatever the C++ code had put in r27) and can silently
  // corrupt another thread's CpuStateFrame if the alias falls that way.
  //
  // ThreadPauseHandler above has always split its two entry points
  // correctly (:490 / :493); ThreadStopHandler used to alias both
  // pointers to the same body (see ContextConfig at :1163). Adding
  // the second entry point makes the outside-JIT path skip the spill.
  // ==============================================================
  ThreadStopHandlerAddressSpillSRA = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  SpillStaticRegs(TMP1);

  ThreadStopHandlerAddress = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  // Bound here (immediately after SpillStaticRegs at ThreadStopHandlerAddressSpillSRA)
  // so the SMC single-step failure path can reach the no-spill entry via a
  // forward branch — see the label declaration near CompileSingleStepLabel.
  Bind(&ThreadStopNoSpillLabel);
  {
    int32_t rsl_off = static_cast<int32_t>(
      offsetof(CpuStateFrame, ReturningStackLocation));
    li(TMP1, 0);
    std(TMP1, static_cast<int16_t>(rsl_off), STATE);
  }
  PopCalleeSavedRegisters();
  blr();

  // ==============================================================
  // ThreadPauseHandler — park this thread until woken (GDB, explicit pause)
  // ThreadPauseHandlerAddressSpillSRA: entered when in JIT (spill first)
  // ThreadPauseHandlerAddress:         entered when already spilled
  // ==============================================================
  ThreadPauseHandlerAddressSpillSRA = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  SpillStaticRegs(TMP1);

  ThreadPauseHandlerAddress = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  // Call SleepThread(CTX, Frame): r3=CTX, r4=STATE
  //
  // ARGUMENT REGISTER: the function pointer goes in r12, NOT TMP1.
  // TMP1 *is* r3 (PPC64Emitter.h:37), so the previous
  // `LoadConstant(TMP1, SleepThread)` overwrote the CTX argument that the
  // line above had just materialised, and SleepThread ran with
  // CTX == &SleepThread. `CTX->SyscallHandler->SleepThread(...)` then read
  // a pointer out of its own machine code and dereferenced the result —
  // a fault *inside* SleepThread on every pause. r12 is the register ELFv2
  // wants here anyway (global entry point → callee's `addis r2,r12,...`
  // TOC prologue), so this is one load instead of load+mr. r12 is SRA
  // (guest RBP) but both entries to this handler arrive with SRA already
  // spilled, so clobbering it is safe — same reasoning as the `ld r12` in
  // ExitFunctionLinker above.
  //
  // STACK FRAME: allocate one before the bctrl. ELFv2 makes the CALLER own
  // the 32-byte linkage area + 64-byte parameter save area at the bottom of
  // its frame (see the layout comment at ArchHelpers/PPC64Emitter.h:82-98):
  // the callee stores its return address to [caller_r1+16], may store CR to
  // [caller_r1+8], and may spill incoming argument registers anywhere in
  // [caller_r1+32, caller_r1+96). Nothing here had reserved that.
  //
  // What r1 actually points at on entry is the reason this was survivable
  // until now, and the reason it must still be fixed: both entry points are
  // only ever reached by SignalDelegator::HandleSignalPause rewriting the
  // interrupted context's PC, and StoreThreadState has already moved SP to
  // point *at* the PPC64ContextBackup it stamped
  // (LinuxSyscalls/SignalDelegator.cpp: StoreThreadState → SetSp(NewSP)).
  // So r1 == &Backup, and PPC64ContextBackup's head is a deliberate
  // 128-byte LinkageArea pad (ArchHelpers/MContext_ppc64le.h:21-48) that
  // absorbs exactly these writes. That pad was added for the guest-signal
  // path, not this one; depending on it here means the correctness of the
  // pause path is coupled to the layout of an unrelated struct, and any
  // future entry with a different r1 corrupts the frame below it. Own the
  // reservation instead.
  //
  // Size: kDynLinkArea (96) — the ABI minimum, already 16-byte aligned, and
  // a multiple of 4 so `stdu`'s DS-form displacement is encodable. Nothing
  // is stored in the frame beyond what the ABI hands to the callee, so
  // there is no reason to make it larger.
  //
  // Placement: after ThreadPauseHandlerAddress, so the SpillSRA entry gets
  // it too by fall-through and the "already spilled" entry does not skip
  // it. SpillStaticRegs does not touch r1 (PPC64Emitter.cpp), so ordering
  // the stdu after it changes nothing about the spill.
  //
  // r1 MUST be restored before the trap word below. The wake path is
  // HandleSIGILL → RestoreThreadState(TYPE_PAUSE), which recovers the
  // backup with `Context = (ContextBackup*)GetSp(ucontext)` — it finds the
  // saved state by taking the trapping SP *as* the backup pointer. Leaving
  // r1 96 bytes low would make it reconstruct the whole host context from
  // the wrong address. (No TOC save/restore around the call, unlike
  // ExitFunctionLinker: nothing between the bctrl and the trap reads r2,
  // and RestoreContext reinstates all 48 gp_regs, r2 included.)
  constexpr int16_t PAUSE_FRAME_SIZE = static_cast<int16_t>(a64::kDynLinkArea);  // 96
  static_assert(PAUSE_FRAME_SIZE >= 96 && (PAUSE_FRAME_SIZE % 16) == 0,
                "ELFv2 caller frame must cover linkage+param save area and stay 16-byte aligned");

  stdu(r1, static_cast<int16_t>(-PAUSE_FRAME_SIZE), r1);
  LoadConstant(r3, reinterpret_cast<uint64_t>(CTX));
  mr(r4, STATE);
  LoadConstant(r(12), reinterpret_cast<uint64_t>(SleepThread));
  mtctr(r(12));
  bctrl();
  addi(r1, r1, PAUSE_FRAME_SIZE);

  // PauseReturnInstruction: illegal opcode 0x00000000 → SIGILL.
  // FEX sends SIGILL to this address to wake the paused thread.
  PauseReturnInstruction = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  Emit32(0x00000000u);  // primary opcode 0 — invalid on PPC64LE, generates SIGILL

  // ==============================================================
  // Guest signal stubs — force matching host fault.
  //
  // The Break IR op populates SynchronousFaultData (FaultToTopAndGenerated
  // Exception=1 plus Signal/TrapNo/si_code/err_code) and jumps here.  Each
  // stub then triggers the corresponding host fault so the host signal
  // handler runs through the SignalDelegator → GuestFramesManagement path
  // and delivers the synthesized signal to the guest with proper si_addr
  // and cr2 (now populated from OriginalRIP for synthesized faults — see
  // GuestFramesManagement.cpp:474 (x64) and :742+:819 (i32 RT)).  Without
  // these stubs guest signal handlers never run for JIT-detected faults
  // (NoExec, alignment, etc.) and threads die silently with pthread_join
  // never returning.
  //
  // ExitOnHLTEnabled() keeps the old clean-exit behavior for unit tests
  // that intentionally trigger faults.
  // ==============================================================
  // SIGILL/SIGTRAP stubs — raise the matching host fault.
  //
  // These used to be `SpillStaticRegs; PopCalleeSavedRegisters; blr`, on the
  // stated grounds that FEX "has been silently NOPing" guest UD2/INT3 and that
  // firing a real signal would surprise callers. **That premise was measured
  // and is false.** `blr` here is the dispatcher's *epilogue*: it returns out
  // of ExecuteThread, after which Syscalls/Thread.cpp runs
  // ReleaseAllPendingSharedLocks, UninstallTLSState and DestroyThread. So the
  // old behaviour was not absorption — it silently **destroyed the guest
  // thread**, with any guest-side locks it held left held forever, no handler
  // run and nothing logged. A probe confirmed this at runtime for both INT3
  // and UD2: the thread executed the instruction and never returned, and its
  // peers then blocked indefinitely.
  //
  // Delivering the signal is therefore strictly better than the status quo for
  // every caller the old comment worried about. A guest whose handler catches
  // SIGILL/SIGTRAP now gets to run it; a guest without one dies loudly instead
  // of losing one thread invisibly.
  //
  // Mechanism: spill guest state, then execute a genuinely faulting word so the
  // host kernel raises the signal and SignalDelegator → GuestFramesManagement
  // synthesises delivery with correct si_addr. State reconstruction already
  // works on this path — IsAddressInCodeBuffer is false for the dispatcher
  // mmap, so SpillSRA is skipped and State.rip survives.
  //
  // SIGTRAP additionally required a host thunk, which FEX never installed; see
  // the registration added in LinuxSyscalls/SignalDelegator.cpp.
  //
  // KNOWN LIMITATION: UD2 and UnimplementedOp emit byte-identical
  // BreakDefinitions (OpcodeDispatcher.cpp), so the backend cannot tell a
  // deliberate guest trap from a FEX codegen gap. After this change an
  // unimplemented op surfaces as a guest SIGILL rather than something
  // diagnosable. Separating them needs a new IR field; recorded, not fixed.
  GuestSignal_SIGILL_Address = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  SpillStaticRegs(TMP1);
  // All-zero word: not a valid PowerPC encoding, so the kernel raises SIGILL
  // with si_code ILL_ILLOPC. Same construct SignalHandlerReturnAddress uses
  // below; the two are distinguished by PC, which HandleSIGILL compares.
  Emit32(0x00000000u);

  GuestSignal_SIGTRAP_Address = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  SpillStaticRegs(TMP1);
  // `trap` = tw 31,r0,r0 — unconditional program check, raises SIGTRAP.
  // Same encoding X87Ops.cpp already emits for unsupported fstp conversions.
  tw(31, r0, r0);

  // SIGSEGV stub DELIBERATELY stays silent — do not "fix" it to match the two
  // above without reading this.
  //
  // Guest HLT lowers to BreakOp(SIGSEGV), and HLT is how the ~2200 ASM tests
  // terminate: silent stub → blr out of ExecuteThread → FEXInterpreter runs its
  // normal teardown and returns StatusCode. Making this fault would change how
  // roughly 6672 of the 7011 ctest cases end, which is why the 6/7011 gate has
  // no bearing on the SIGILL/SIGTRAP change above — no ASM test uses UD2 or
  // INT3, verified by search.
  //
  // The same stub also serves NoExec entry-block faults, which guests *should*
  // see, so this is genuinely two behaviours sharing one stub. Separating them
  // needs the Break op to distinguish its callers — the same missing IR field
  // noted above. Until then silence is the safer default here and delivery is
  // the safer default there.
  //
  // (The prior comment claimed a real fault "breaks the worker pool init" for
  // Steam. That claim traces to f78e0613d, which is a pure comment diff whose
  // own message calls it "exploration that got reverted to a clean blr" — no
  // code ever implemented or tested it. Treated as an untested hypothesis, not
  // a finding.)
  GuestSignal_SIGSEGV_Address = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  SpillStaticRegs(TMP1);
  {
    int32_t rsl_off = static_cast<int32_t>(
      offsetof(CpuStateFrame, ReturningStackLocation));
    li(TMP1, 0);
    std(TMP1, static_cast<int16_t>(rsl_off), STATE);
  }
  PopCalleeSavedRegisters();
  blr();

  // ==============================================================
  // SignalReturnHandler — sentinel for signal-return state restore.
  // Called from C++ as a function pointer (rt_sigreturn syscall handler);
  // must NOT actually return. Faulting here makes the host kernel raise
  // SIGILL, and SignalDelegator::HandleSIGILL detects PC ==
  // SignalHandlerReturnAddress{,RT} and runs RestoreThreadState. Same
  // trick PauseReturnInstruction uses above. arm64 emits hlt(0) for
  // this.
  //
  // TWO distinct sentinels: HandleSignalHandlerReturn(RT) picks which
  // to call, and HandleSIGILL compares the trapped PC against both to
  // decide RestoreType (REALTIME vs NONREALTIME). If they share an
  // address the ternary at SignalDelegator.cpp:704 always picks RT and
  // 64-bit sigreturn tears the wrong frame -- the Factorio SIGILL on
  // pthread's SIGRTMIN handler was this defect (2026-07-31).
  // ==============================================================
  SignalHandlerReturnAddress = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  Emit32(0x00000000u);

  SignalHandlerReturnAddressRT = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  Emit32(0x00000000u);

  // ==============================================================
  // CallbackPtr — re-entry from JIT callback
  // Called with C-ABI: r3=Frame, r4=target RIP
  // ==============================================================
  CallbackPtr = reinterpret_cast<JITCallback>(GetCursorAddress<uint8_t*>());

  // CRITICAL ORDERING: stash incoming RIP (r4) BEFORE PushCalleeSavedRegisters.
  // PushCalleeSavedRegisters() clobbers TMP2 (== r4) twice in its prologue:
  // first `mfcr(TMP2)` at PPC64Emitter.cpp:279 (which the earlier version of
  // this comment blamed), and then FINALLY `LoadImm32(TMP2, 192)` inside the
  // VMX save loop at :298. That last write is the one that survives — r4
  // holds 192 (= 0xC0, the byte offset of v31's stvx slot in the frame) at
  // every return from PushCalleeSavedRegisters, on every entry. Symptom:
  // State.rip ends up = 0xC0, the dispatcher loads garbage as the next
  // guest PC, and the suspect-RIP bypass fires on every single host->guest
  // callback (libGL XSync, libvulkan X11Manager.*, etc.). The mfcr framing
  // in the earlier comment was arithmetically wrong; the fix was right for
  // the wrong reason.
  //
  // r7 is volatile per ELFv2 ABI -- the C++ caller has either spilled it
  // or doesn't care -- and PushCalleeSavedRegisters only touches r0, r1,
  // r4 (TMP2), and r14-r31. Stashing in r7 is safe across the prologue.
  mr(r(7), r4);

  PushCalleeSavedRegisters();
  mr(STATE, r3);

  // Restore RIP from the stash and persist into State.rip. The original
  // 32-bit clamp is preserved for 32-bit guests.
  {
    int32_t rip_off = static_cast<int32_t>(offsetof(CpuStateFrame, State.pc));
    std(r(7), rip_off, STATE);
  }

  // Bump the signal handler ref counter (marks that we're in a JIT callback)
  {
    int32_t ref_off = static_cast<int32_t>(
      offsetof(CpuStateFrame, SignalHandlerRefCounter));
    lwz(TMP1, ref_off, STATE);
    addi(TMP1, TMP1, 1);
    stw(TMP1, ref_off, STATE);
  }

  // A host thunk callee is re-entering guest execution. If the JIT crossing
  // that made the host call armed the InSyscallInfo sentinel (DEF_OP(Thunk)/
  // the FABI stubs — see kInSyscallSentinel in ArchHelpers/PPC64Emitter.h),
  // it MUST die here, for two independent reasons:
  //  1. The callback's guest blocks run with live SRA in host registers; a
  //     signal during them must spill everything. A surviving mask would
  //     make SpillSRA skip the r7-r15 spills over LIVE state — stale frame
  //     RSP/RIP, the 2026-07-31 Factorio crash class recorded at
  //     SpillForABICall's NB comment.
  //  2. The callback republishes guest state in the frame (its blocks spill
  //     to it continuously), so the interrupted crossing's conditional
  //     refill must take the full path when it eventually resumes — which is
  //     exactly what a dead sentinel tells it.
  // Unconditional store: when nothing armed it, the field is already 0 and
  // this is an idempotent 2-instruction cost on an already-expensive rare
  // path. DEF_OP(Syscall) never crosses a callback, so its inline copy of
  // the scheme is unaffected.
  {
    int32_t isi_off = static_cast<int32_t>(
      offsetof(CpuStateFrame, InSyscallInfo));
    li(TMP1, 0);
    std(TMP1, static_cast<int16_t>(isi_off), STATE);
  }

  // AArch64 callback return: the guest callee returns to X30, so X30 is set
  // to ThunkCallbackRet, the HLT #0x0F3E word whose translation is
  // CallbackReturn.
  //
  // X30 in the frame is live guest state here: DEF_OP(Thunk) spilled every
  // static register before its host call, and the interrupted crossing exits
  // to X30 once the host function returns (IRBuilder::HLT). The callee
  // overwrites it, so it is saved in 16 bytes below the guest SP (SP stays
  // 16-byte aligned, as AAPCS64 requires at a call) and DEF_OP(CallbackReturn)
  // restores both X30 and SP. The guest stack rather than the host's: nested
  // callbacks pair up by stack discipline, and the host frame layout that
  // CallbackReturn's ResetStack/PopCalleeSavedRegisters depend on is left
  // alone.
  //
  // The x86-64 convention this replaces pushed ThunkCallbackRet itself as the
  // return address; the guest `ret` popped it and CallbackReturn added 8.
  {
    int32_t ret_off = static_cast<int32_t>(
      offsetof(CpuStateFrame, Pointers.ThunkCallbackRet));
    int32_t sp_off = static_cast<int32_t>(
      offsetof(CpuStateFrame, State.sp));
    int32_t lr_off = static_cast<int32_t>(
      offsetof(CpuStateFrame, State.x) + 30 * sizeof(uint64_t));
    ld(TMP1, ret_off, STATE);     // TMP1 = ThunkCallbackRet (guest VA)
    ld(TMP2, sp_off, STATE);      // TMP2 = guest SP
    ld(TMP3, lr_off, STATE);      // TMP3 = the crossing's X30
    addi(TMP2, TMP2, -16);        // SP -= 16
    std(TMP3, 0, TMP2);           // [SP] = saved X30
    std(TMP2, sp_off, STATE);     // write back the new SP
    std(TMP1, lr_off, STATE);     // X30 = ThunkCallbackRet
  }

  FillStaticRegs();

  // Jump to dispatcher loop top to run the callback target
  {
    LoadConstant(TMP1, DispatcherLoopTopAddress);
    mtctr(TMP1);
    bctr();
  }

  DispatcherEnd = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());

  // The FABI bridge stubs (x87/F80 softfloat, F64 x87 math and SSE4.2 string
  // helpers) were removed with the x86 guest. The exported range stays, empty,
  // for the signal delegator's async-defer gate.
  FABIStubsBegin = reinterpret_cast<uint64_t>(GetCursorAddress<uint8_t*>());
  FABIStubsEnd = FABIStubsBegin;

  // Flush instruction cache for entire generated region. This was one of three
  // copy-pasted inline-asm blocks; it is now the shared helper, which also adds
  // the "memory" clobber the copy here was missing and strides by the auxv
  // cache-block size instead of a hardcoded 32.
  uint8_t* Start = reinterpret_cast<uint8_t*>(DispatcherBegin);
  uint8_t* End = GetCursorAddress<uint8_t*>();
  FEXCore::ArchHelpers::PPC64::FlushICacheRange(Start, static_cast<size_t>(End - Start));
}

FEXCore::SignalDelegatorConfig PPC64Dispatcher::MakeSignalDelegatorConfig() const {
  const std::span<const GPR> SRA    = std::span<const GPR>(a64::SRA);
  const std::span<const VR>  SRAFPR = std::span<const VR>(a64::SRAFPR);

  const auto GPRCount = static_cast<uint16_t>(SRA.size());
  const auto FPRCount = static_cast<uint16_t>(SRAFPR.size());

  SignalDelegatorConfig::SRAIndexMapping GPRMapping {};
  for (size_t i = 0; i < GPRCount; i++) {
    GPRMapping[i] = SRA[i].idx;
  }

  SignalDelegatorConfig::SRAIndexMapping FPRMapping {};
  for (size_t i = 0; i < FPRCount; i++) {
    FPRMapping[i] = SRAFPR[i].idx;
  }

  return FEXCore::SignalDelegatorConfig {
    .DispatcherBegin = DispatcherBegin,
    .DispatcherEnd   = DispatcherEnd,
    .FABIStubsBegin  = FABIStubsBegin,
    .FABIStubsEnd    = FABIStubsEnd,

    .AbsoluteLoopTopAddress        = DispatcherLoopTopAddress,
    .AbsoluteLoopTopAddressFillSRA = DispatcherLoopTopFillSRAAddress,
    .SignalHandlerReturnAddress     = SignalHandlerReturnAddress,
    .SignalHandlerReturnAddressRT   = SignalHandlerReturnAddressRT,

    .PauseReturnInstruction            = PauseReturnInstruction,
    .ThreadPauseHandlerAddressSpillSRA = ThreadPauseHandlerAddressSpillSRA,
    .ThreadPauseHandlerAddress         = ThreadPauseHandlerAddress,

    .ThreadStopHandlerAddressSpillSRA = ThreadStopHandlerAddressSpillSRA,
    .ThreadStopHandlerAddress         = ThreadStopHandlerAddress,

    .SRAGPRCount = GPRCount,
    .SRAFPRCount = FPRCount,

    .SRAGPRMapping = GPRMapping,
    .SRAFPRMapping = FPRMapping,

    // AVX-high bank: one entry per SRA vector when AVX is advertised (never, for the A64 guest).
    .SRAAVXHighBankFirst = static_cast<uint16_t>(AVXHIGH_BANK_FIRST),
    .SRAAVXHighBankCount = static_cast<uint16_t>(CTX->HostFeatures.SupportsAVX ? FPRCount : 0),
  };
}

void PPC64Dispatcher::ExecuteDispatch(FEXCore::Core::CpuStateFrame* Frame, bool SingleInst) {
  DispatchPtr(Frame, SingleInst);
  Frame->ReturningStackLocation = 0;
}

void PPC64Dispatcher::ExecuteJITCallback(FEXCore::Core::CpuStateFrame* Frame, uint64_t RIP) {
  CallbackPtr(Frame, RIP);
  Frame->ReturningStackLocation = 0;
}

} // namespace FEXCore::CPU
