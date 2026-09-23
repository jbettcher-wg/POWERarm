// SPDX-License-Identifier: MIT
#pragma once

// ===========================================================================
// SMCChecks=icache: the guest's own cache maintenance IS the invalidation
// protocol
// ===========================================================================
//
// WHAT THIS REPLACES
// ------------------
// mtrack is the x86 coherency model. x86 fetches coherently with stores, so an
// emulator of it has no announcement to listen to and must rediscover every
// code write the hard way: write-protect the page, take a SIGSEGV, take the
// exclusive CodeInvalidationMutex (which stalls every compiling thread in the
// process), erase every block on the faulting granule, scrub every thread's
// lookup cache, mprotect the page back, recompile -- and the first recompile
// re-arms the page, so the JIT's next write faults again.
//
// AArch64 does not work that way. DDI 0487 B2.2.5 ("Concurrent modification and
// execution of instructions") and the caches chapter say instruction fetch is
// NOT coherent with stores: a PE may keep executing the old instruction
// indefinitely, and the new one becomes visible only once the writer performs
// DC CVAU (unless CTR_EL0.IDC), DSB ISH, IC IVAU (unless CTR_EL0.DIC), DSB ISH
// and the executing PE performs ISB. A translation cache is an instruction
// cache with a bigger line; the architecture lets it hold stale translations
// until IC IVAU.
//
// POWERarm already depends on the guest honouring that: SystemRegisters.h
// advertises the Pi 5's CTR_EL0 = 0x9444C004 (IDC=1, DIC=0, 64-byte lines), so
// libgcc's __aarch64_sync_cache_range, V8's FlushInstructionCache, JSC's
// cacheFlush, Mono, .NET, OpenJDK and LuaJIT all skip DC CVAU and issue one
// IC IVAU per 64 bytes they wrote, then DSB ISH; ISB. The guest tells us
// exactly which 64 bytes changed and exactly when they must become visible.
// This mode stops throwing that away.
//
// THE PROTOCOL
// ------------
//   IC IVAU Xt  ->  ICacheInvalidate IR op -> ContextImpl::ICacheInvalidateFromJit
//       1. Round the address down to 64 bytes (the architecture ignores the low
//          bits; IminLine is what we advertise in CTR_EL0, and it is also the
//          SMC Idea 3 granule, which is what makes step 2 exact).
//       2. Ask the granule bitmap whether any translation lives in that line.
//          Lock-free, no false negatives. Clear => return. This is the common
//          case: a JIT flushes code it just wrote into fresh memory.
//       3. Otherwise take the exclusive CodeInvalidationMutex and erase, with
//          BYTE precision, every block whose decoded guest extent overlaps the
//          line -- in every live CodeBuffer, and over every mirror of a shared
//          mapping. Then scrub each thread's lookup cache for the line.
//
//   ISB         ->  ends the block and exits to the dispatcher. After a context
//                   synchronisation event the thread must re-fetch, which in
//                   this model means re-look-up: the continuation PC misses (or
//                   hits a block whose entry was erased) and is recompiled from
//                   the new bytes. Before the ISB the old translation may run,
//                   exactly as the architecture permits.
//
//   MarkGuestExecutableRange is a no-op (SyscallsSMCTracking.cpp returns early
//   for anything but mtrack), so nothing is ever write-protected, no SMC
//   SIGSEGV can be raised, and HandleSegfault's SMC half is unreachable --
//   every SEGV_ACCERR is then the guest's own.
//
//   Syscall-driven invalidations stay exactly as they are: mmap-over, munmap,
//   mremap, shmdt and madvise(DONTNEED|FREE|REMOVE) all change bytes without a
//   guest store, so no IC IVAU announces them. They mirror what the kernel does
//   on hardware.
//
//   mprotect follows the kernel's own rule. arm64 Linux syncs the I-cache for a
//   page only the FIRST time a PTE for it becomes executable (PG_dcache_clean,
//   arch/arm64/mm/flush.c); a page flipped RX -> RW -> RX is not re-synced,
//   which is exactly why SpiderMonkey flushes after every patch. So an mprotect
//   that adds PROT_EXEC to pages this process has not made executable before
//   invalidates (bytes may have arrived by read(2), which issues no IC IVAU),
//   and a W^X flip back to PROT_EXEC on pages that were executable before costs
//   nothing, because the guest's IC IVAU already did the precise work.
//
// WHY THIS IS SOUND, AND SOUNDER THAN mtrack
// ------------------------------------------
// ContextImpl::CompileBlock holds CodeInvalidationMutex SHARED from before
// decode (inside CompileCode) to after publish, and the IC IVAU path takes it
// EXCLUSIVE. So:
//
//   * A guest store that lands MID-DECODE is followed by an IC IVAU that cannot
//     acquire the exclusive lock until the stale unit has been published -- and
//     then erases it. The unit never survives the flush.
//   * A guest store that lands AFTER a flush is, by definition, before the next
//     flush; the guest owes one for the bytes it just wrote.
//
// That closes the window SMCSoftInvalidate.h calls the "fresh-compile half"
// residual: under mtrack the page is armed AFTER the decode read the bytes, so
// a store in between is invisible to both the hash and the protection. Under
// icache the guest's own ordering (store, then IC IVAU) is what serialises
// against the compiler.
//
// Cross-thread: the invalidation is process-global at the writer's IC IVAU. A
// thread already inside an old translation finishes it and re-looks-up at its
// next exit -- the same in-flight semantics legacy invalidation has, and
// STRONGER than the architecture, which lets that thread run old code until its
// own ISB.
//
// WHAT THIS STOPS TOLERATING
// --------------------------
// Chiefly one thing: a guest that rewrites bytes it has ALREADY EXECUTED and
// never flushes them. That guest is broken on the reference hardware too --
// DIC=0, and a PIPT instruction cache will hold the old line, so a Cortex-A76
// runs the stale instructions. Every runtime in the target set flushes. Bytes
// written into memory that was never executed have no stale translation to hit.
//
// The mprotect rule adds a second, narrower one: an mprotect that STRIPS
// PROT_EXEC does not invalidate (it is the W half of a W^X flip, and
// invalidating there would leave the X half nothing to keep), so a guest that
// makes its own code non-executable and then branches into it anyway runs the
// old code instead of taking SIGSEGV. The guest may not legally execute from
// such a range at all -- the same argument FEX_SMCMPROTECTDEFER already makes
// for the same transition -- and any change that retires the MEMORY (munmap,
// mmap over, mremap, madvise DONTNEED) still invalidates unconditionally.
//
// SMCChecks=mtrack remains the fault-based fallback and HostPageMode=degrade
// still forces SMCChecks=full.
//
// WHAT MUST NOT BE DONE TO STAY SOUND
// -----------------------------------
// No translation may bake a code-page READ into an immediate. V8 patches
// constant pools inside code pages during GC WITHOUT a flush, because to it
// they are data. This is safe only because the frontend translates LDR
// (literal) as a real load (TranslateLoadStore.cpp, LDR_lit_gen ->
// LoadStoreSingle -> _LoadMem). The ValidateCode snapshot of the decoded
// instruction word is the only code-page bytes a translation may carry.
//
// POWER8: no codegen is involved beyond DEF_OP(ICacheInvalidate). This is
// runtime C++ plus one helper call out of JIT code.
// ===========================================================================

#include <cstdint>

namespace FEXCore::SMC {

// The line IC IVAU names. Equal to the IminLine advertised in CTR_EL0
// (SystemRegisters.h) and to CodeGranuleBitmap::kGranuleShift's granule, which
// is what lets the bitmap answer this question exactly.
inline constexpr uint64_t ICacheLineSize = 64;
inline constexpr uint64_t ICacheLineMask = ~(ICacheLineSize - 1);

inline constexpr uint64_t ICacheLineBase(uint64_t Address) {
  return Address & ICacheLineMask;
}

} // namespace FEXCore::SMC
