// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/IR/IR.h>
#include <FEXCore/Utils/CompilerDefs.h>
#include <FEXCore/Utils/Telemetry.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <stdint.h>
#include <string_view>
#include <type_traits>

namespace FEXCore::Core {
// Wrapper around std::atomic using std::memory_order_relaxed.
// This allows compilers to emit more performant code at the expense of visibly tearing.
// In particular, increments/decrements may visibly tear if a signal is received half-way through.
//
// Prefer std::atomic with default memory ordering unless you really know what you're doing.
// Primarily this ensure program ordering when signals are concerned.
template<typename T>
class NonAtomicRefCounter {
public:
  void Increment(T Value) {
    // Specifically avoiding fetch_add here because that will turn in to ldxr+stxr or lock xadd.
    // FEX very specifically wants to use simple loadstore instructions for this
    //
    // ARM64 ex:
    // ldr x0, [x1];
    // add x0, x0, #1;
    // str x0, [x1];
    //
    // x86-64 ex:
    // inc qword [rax];
    auto Current = AtomicVariable.load(std::memory_order_relaxed);
    AtomicVariable.store(Current + Value, std::memory_order_relaxed);
  }

  // Returns original value.
  // x86-64 needs to know the result on decrement.
  T Decrement(T Value) {
    // Specifically avoiding fetch_sub here because that will turn into ldxr+stxr or lock xadd.
    // FEX very specifically wants to use simple loadstore instructions for this
    //
    // ARM64 ex:
    // ldr x0, [x1];
    // sub x0, x0, #1;
    // str x0, [x1];
    //
    // x86-64 ex:
    // dec qword [rax];
    auto Current = AtomicVariable.load(std::memory_order_relaxed);
    AtomicVariable.store(Current - Value, std::memory_order_relaxed);
    return Current;
  }

  T Load() const {
    return AtomicVariable.load(std::memory_order_relaxed);
  }

  void Store(T Value) {
    AtomicVariable.store(Value, std::memory_order_relaxed);
  }

private:
  std::atomic<T> AtomicVariable;
};
static_assert(std::is_standard_layout_v<NonAtomicRefCounter<uint64_t>>, "Needs to be standard layout");
static_assert(std::is_trivially_copyable_v<NonAtomicRefCounter<uint64_t>>, "needs to be trivially copyable");
static_assert(sizeof(NonAtomicRefCounter<uint64_t>) == sizeof(uint64_t), "Needs to be correct size");

// AArch64 guest register state.
//
// Layout rules the ppc64le backend actually depends on (these replace the
// arm64-host ldp/stp reach asserts the x86 layout carried):
//
//  * Every field the JIT touches is addressed as a D-form (lwz/stw) or DS-form
//    (ld/std) displacement off STATE, a signed 16-bit field; DS-form also
//    requires the displacement to be a multiple of 4. SpillStaticRegs and
//    FillStaticRegs fall back to an indexed load when an offset does not fit,
//    but the hot fields must not need that fallback.
//  * The vector bank is loaded and stored with lvx/stvx at STATE + offset, and
//    lvx/stvx silently drop the low 4 bits of the effective address, so every
//    v[i] must sit at a 16-byte-aligned offset.
//  * The packed NZCV word keeps the layout the backend's StoreNZCV/LoadNZCV
//    lowerings and the CR0/XER spill/fill already use: N at bit 31, Z at 30,
//    C at 29, V at 28, stored and loaded as one 32-bit word.
struct alignas(64) CPUState {
  // Cacheline: 0
  // LEGACY (audit P1). Used to hold the address of the running JIT block's
  // JITCodeHeader, stored by every EntryPoint prologue. Block lookup now goes
  // through the per-CodeBuffer host-PC -> block index instead
  // (Interface/Core/CPUBackend.h, CodeBuffer::FindBlockHeader), so the JIT
  // publishes nothing here by default. The store is still emitted under
  // FEX_NOBLOCKHEADER=0, and the only reader left is the FEX_RIPRECONLOG
  // cross-check in Interface/Core/Core.cpp.
  uint64_t InlineJITBlockHeader {};
  // Reference counter for FEX's per-thread deferred signals.
  // Counts the nesting depth of program sections that cause signals to be deferred.
  NonAtomicRefCounter<uint64_t> DeferredSignalRefCount;

  uint64_t pc {}; ///< Guest PC. May not be entirely accurate while the JIT is active.

  // Packed N/Z/C/V at bits 31..28 (PSTATE layout). See the layout rules above.
  uint32_t nzcv {};
  uint32_t fpcr {};
  uint32_t fpsr {};
  uint8_t excl_size {};
  uint8_t excl_valid {};
  uint16_t _pad0 {};

  uint64_t L1Pointer {};
  uint64_t L1Mask {};
  uint64_t callret_sp {};
  // Shadow call-ret stack bound mirrors; see callret_base at the struct tail.
  // callret_end = base + CALLRET_STACK_SIZE (the empty/top bound, hot on the
  // RET pop) shares callret_sp's cache line.
  uint64_t callret_end {};

  // Cacheline: 1-4
  // X0-X30 followed by SP, so that r[n] for n in [0, 31] is a single stride
  // (GPROffset). Register 31 is SP here; XZR never has storage.
  // POWERARM-M0-TODO(ir): IR.json's LoadContext/StoreContext validation used to forbid context access to all x86 GPRs (all were SRA); it must now forbid only the SRA-pinned subset (a64::StaticGPRGuestReg).
  uint64_t x[31] {};
  uint64_t sp {};

  uint64_t tpidr_el0 {};
  uint64_t tpidrro_el0 {};

  // Exclusive monitor (LDXR/STXR, LDXP/STXP). Software form; see DESIGN.md §4.3.
  // POWERARM-M0-TODO(cpustate): layout only; whether the hardware larx/stcx. fast path needs extra per-thread state is an M4 decision.
  uint64_t excl_addr {};
  uint64_t excl_value {};
  uint64_t excl_value_hi {};

  // Cacheline: 5-12
  // V0-V31, 128 bits each, stored as the little-endian image an stvx writes.
  alignas(16) uint64_t v[32][2] {};

  // callret_end's partner: the low bound the CALL push checks against.
  uint64_t callret_base {};

  // Scratch for an atomic RMW that the frontend has to spell out as a CAS
  // retry loop (LDSMAX/LDSMIN/LDUMAX/LDUMIN; TranslateExclusive.cpp
  // AtomicMinMax). The loop needs the loaded value in the block that follows
  // it, and no non-fixed SSA value may be live across a block boundary --
  // ConstrainedRAPass::Run makes every register available again at the top of
  // each block, so only the pinned GPRFixed/SRA slots cross an edge. A guest
  // register cannot carry it either: Rt is free to alias Rn or Rs (Rt == Rs is
  // the ordinary `x = __atomic_fetch_max(p, x)` shape), so writing Rt before
  // the back edge would corrupt the address or the operand for the retry.
  // Nothing guest-visible is written until the loop has committed, which also
  // leaves the instruction restartable if a guest SIGSEGV handler resumes.
  //
  // It goes last, in the tail padding that alignof(CPUState) == 64 already
  // required, so it costs no space and moves no other context offset. There is
  // no room earlier: v[] is 16-byte aligned and _pad1 runs up to it exactly.
  uint64_t atomic_scratch {};

  static constexpr size_t GPR_REG_SIZE = sizeof(x[0]);
  static constexpr size_t VECTOR_REG_SIZE = sizeof(v[0]);
  static constexpr size_t NUM_XREGS = sizeof(x) / GPR_REG_SIZE;
  static constexpr size_t NUM_VREGS = sizeof(v) / VECTOR_REG_SIZE;
  static constexpr uint32_t SP_INDEX = 31;

  // Context offset of X0-X30 (Index 0-30) or SP (Index 31).
  static constexpr size_t GPROffset(uint32_t Index) {
    return offsetof(CPUState, x) + Index * GPR_REG_SIZE;
  }
  static constexpr size_t VectorOffset(uint32_t Index) {
    return offsetof(CPUState, v) + Index * VECTOR_REG_SIZE;
  }

  // PSTATE.NZCV bit positions inside nzcv.
  static constexpr uint32_t NZCV_N_BIT = 31;
  static constexpr uint32_t NZCV_Z_BIT = 30;
  static constexpr uint32_t NZCV_C_BIT = 29;
  static constexpr uint32_t NZCV_V_BIT = 28;

  CPUState() {
#ifndef NDEBUG
    pc = ~0ULL;
    for (auto& Reg : v) {
      Reg[0] = 0xDEADBEEFULL;
      Reg[1] = 0xBAD0DAD1ULL;
    }
#endif
  }
};
static_assert(std::is_trivially_copyable_v<CPUState>, "Needs to be trivial");
static_assert(std::is_standard_layout_v<CPUState>, "This needs to be standard layout");
static_assert(alignof(CPUState) == 64, "CPUState needs to be 64-byte aligned!");
static_assert(offsetof(CPUState, DeferredSignalRefCount) % 8 == 0, "Needs to be 8-byte aligned");
static_assert(offsetof(CPUState, L1Mask) == (offsetof(CPUState, L1Pointer) + 8), "These two variables are paired");
static_assert(offsetof(CPUState, callret_end) == offsetof(CPUState, callret_sp) + 8,
              "callret_sp/callret_end must share a cache line for the RET pop's two loads");
static_assert(offsetof(CPUState, sp) == CPUState::GPROffset(CPUState::SP_INDEX), "SP must follow X30 in the GPR stride");
// DS-form (ld/std) reach and 4-multiple for every 64-bit GPR slot and the callret/L1 mirrors.
static_assert(CPUState::GPROffset(CPUState::SP_INDEX) + 8 <= 32764 && CPUState::GPROffset(0) % 4 == 0,
              "GPR slots must be DS-form reachable off STATE");
static_assert(offsetof(CPUState, callret_base) + 8 <= 32760, "callret mirrors must stay int16-reachable for D-form ld off STATE");
static_assert(offsetof(CPUState, L1Mask) + 8 <= 32760, "L1 lookup mirrors must stay int16-reachable for D-form ld off STATE");
// D-form (lwz/stw) reach for the packed flag and FP control words.
static_assert(offsetof(CPUState, fpsr) + 4 <= 32767, "NZCV/FPCR/FPSR must be D-form reachable off STATE");
// lvx/stvx drop the low 4 EA bits.
static_assert(offsetof(CPUState, v) % 16 == 0, "v[] must be 16-byte aligned for lvx/stvx");
// atomic_scratch is deliberately the last member: it fills tail padding that
// alignof(CPUState) == 64 already required, so adding it moved no other context
// offset and did not grow the struct. 896 is the size both before and after it
// was added. If this fires, the field has started costing a whole cacheline per
// thread and wants a real home or a rethink -- do not just bump the number.
static_assert(sizeof(CPUState) == 896, "CPUState grew; see the note on atomic_scratch");
static_assert(offsetof(CPUState, atomic_scratch) + sizeof(uint64_t) <= sizeof(CPUState),
              "atomic_scratch must lie inside CPUState's existing tail padding");
static_assert(CPUState::VectorOffset(CPUState::NUM_VREGS) <= 32767, "v[] must stay within a signed 16-bit displacement");

// Guest registers held in the backend's static (pinned) GPR slots, in slot
// order: slot i holds guest register StaticGPRGuestReg[i] (0-30 = Xn, 31 = SP).
// The ppc64le host register for each slot is a64::SRA in
// Interface/Core/ArchHelpers/PPC64Emitter.h; the choice is documented there.
// Everything else is reached through LoadContext/StoreContext.
inline constexpr std::array<uint8_t, 17> StaticGPRGuestReg = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 19, 20, 21, 22, 23, 29, 30, CPUState::SP_INDEX,
};
// Guest V registers held in the static vector slots: V0-V31 (V0-V15 in host v0-v15, V16-V31 in host vs16-vs31).
inline constexpr size_t NumStaticVectorRegs = 32;

struct InternalThreadState;

struct JITPointers {

  // Process specific
  uint64_t PrintValue {};
  uint64_t PrintVectorValue {};
  uint64_t PrintMsgValue {};
  uint64_t ThreadRemoveCodeEntryFromJIT {};
  // SMCChecks=icache: ContextImpl::ICacheInvalidateFromJit, called by
  // DEF_OP(ICacheInvalidate) when the guest runs IC IVAU.
  uint64_t ICacheInvalidateFromJIT {};
  uint64_t SyscallHandlerObj {};
  uint64_t SyscallHandlerFunc {};
  uint64_t ExitFunctionLink {};
  uint64_t MonoBackpatcherWrite {};
  uint64_t LUDIV {};
  uint64_t LDIV {};
  uint64_t ThunkCallbackRet {};
  // PPC64LE: helper called by Yield (PAUSE) when PauseCount hits threshold.
  // Sig: void (*)(); just calls sched_yield(). C-ABI.
  uint64_t PPC64_PauseSchedYield {};


  uint64_t NamedVectorConstantPointers[FEXCore::IR::NamedVectorConstant::NAMED_VECTOR_CONST_POOL_MAX];
  uint64_t IndexedNamedVectorConstantPointers[FEXCore::IR::IndexNamedVectorConstant::INDEXED_NAMED_VECTOR_MAX];
  // JIT-emitted code loads counter addresses out of this table; sized to the
  // subset of TelemetryType entries the JIT actually needs (see the marker
  // in Telemetry.h). C-helper-only counters live above the marker and are
  // reached through the global TelemetryValues array directly.
  uint64_t TelemetryValueAddresses[FEXCore::Telemetry::TYPE_JIT_ADDRESSABLE_LAST];

  /**
   * @name Dispatcher pointers
   * @{ */
  uint64_t DispatcherLoopTop {};
  uint64_t DispatcherLoopTopFillSRA {};
  uint64_t ExitFunctionLinker {};
  uint64_t ThreadStopHandlerSpillSRA {};
  uint64_t ThreadPauseHandlerSpillSRA {};
  uint64_t GuestSignal_SIGILL {};
  uint64_t GuestSignal_SIGTRAP {};
  uint64_t GuestSignal_SIGSEGV {};
  uint64_t SignalReturnHandler {};
  uint64_t SignalReturnHandlerRT {};
  uint64_t L2Pointer {};
  uint64_t LUDIVHandler {};
  uint64_t LDIVHandler {};
  // G1(a): the shared spill island's two stub entry points (see
  // PPC64Dispatcher::EmitSpillIsland). Every miss leg reaches its stub with
  // `ld TMP1, <slot>(STATE); mtctr; bctr` instead of a PC-relative `b` to a
  // per-unit copy — the frame-slot load is what makes the branch survive the
  // code cache relocating the block to a new buffer offset. Same value in
  // every thread (the island is context-lifetime), written in
  // InitThreadPointers. The 2-page budget note below predates these: it still
  // holds because InternalThreadState's size constraint no longer applies
  // (the interrupt fault page is mmap'd); the binding limit is the 32760
  // Pointers-reachability assert below, which this pair stays under.
  uint64_t SpillIslandExit {};
  uint64_t SpillIslandLink {};
  // PPC64LE block linking (constant-target jump exits only) intentionally
  // does NOT add per-thread frame slots.  The dispatcher stub materialises
  // its C++ callee address as an inline constant (PPC64Dispatcher.cpp), and
  // the thunk record itself caches the stub's address in its StubAddr field
  // (JITClass.h).  Keeping the two addresses out of CpuStateFrame is what
  // preserves the InternalThreadState ≤ 2·PAGE_SIZE budget after C4.5/C6/C7.
  /**  @} */

  // Copy of process-wide named vector constants data.
  alignas(16) uint64_t NamedVectorConstants[FEXCore::IR::NamedVectorConstant::NAMED_VECTOR_CONST_POOL_MAX][2];
};

// Each guest JIT frame has one of these.
//
// Cross-arch alignment note: POWER8 has 128-byte L2/L3 cache lines (vs
// ARM64/x86 64-byte). When two per-thread CpuStateFrame allocations land
// adjacent in memory and the structure is only 64-byte aligned, the END
// of one frame and the START of the next share a 128-byte line. Cross-
// thread atomic operations (lwarx/stwcx_.) on either frame then cancel
// the other's reservation -> live-lock or starvation patterns that don't
// reproduce on ARM64. Force 128-byte alignment + trailing pad on PPC64LE
// so adjacent frames are on disjoint cache lines.
#ifdef ARCHITECTURE_ppc64le
struct alignas(128) CpuStateFrame {
#else
struct CpuStateFrame {
#endif
  CPUState State;

  /**
   * @brief Stack location for the CPU backends to return the stack pointer to
   *
   * Allows the CPU cores to do a long jump out of their execution and safely shut down
   */
  uint64_t ReturningStackLocation {};

  /**
   * @brief If we are in an inline syscall we need to store a bit of additional information about this
   *
   * ARM64:
   *  - Bit 15: In syscall
   *  - Bit 14-0: Number of static registers spilled
   */
  uint64_t InSyscallInfo {};


  uint32_t SignalHandlerRefCounter {};

  struct alignas(8) SynchronousFaultDataStruct {
    bool FaultToTopAndGeneratedException {};
    uint8_t Signal;
    uint8_t TrapNo;
    uint8_t si_code;
    uint16_t err_code;
    uint16_t _pad : 16;
  } SynchronousFaultData;

  InternalThreadState* Thread;

  /**
   * @brief One host page, PROT_READ|PROT_WRITE, mmap'd per thread in
   * ContextImpl::CreateThread and unmapped in DestroyThread.
   *
   * The page carries no data. It exists to be mprotect'd PROT_NONE by the
   * signal delegator so that the JIT's deferred-signal poke (one `stb`) faults
   * at the next guest boundary. It used to be an array embedded in
   * InternalThreadState, which forced the whole thread state to be
   * alignas(page) and exactly two pages; on a host whose page is larger than
   * 4K that layout cannot be produced at all and every arming mprotect returns
   * EINVAL. Holding a pointer instead makes the host page size a runtime
   * quantity at the cost of one L1-resident dependent load on the poke.
   */
  uint8_t* InterruptFaultPagePtr {};


#ifdef ARCHITECTURE_ppc64le
  // 16-byte aligned scratch slot used by JIT-emitted helpers that need a
  // bounce buffer for unaligned vector loads/stores or to spill a GPR around
  // mfcr/mfxer. Previously the emitter used r1-relative negative offsets in
  // the ELFv2 288B red zone -- but that fell inside the [stack] mapping only
  // when the mapping was large enough. On Steam's bash subshells with tight
  // clone()-allocated stacks the mapping ended before the ABI red zone did
  // and those stores SEGV'd. Address as
  // `[STATE + offsetof(CpuStateFrame, JITScratch)]`.
  alignas(16) uint64_t JITScratch[2] {};

  // PAUSE-instruction spin-yield counter. Incremented by every guest PAUSE
  // (x86 `0xF3 0x90`) translation; when it reaches FEX_PAUSE_YIELD_THRESHOLD
  // the JIT calls sched_yield() on the host kernel and resets to 0. Pure
  // SMT-yield (`or 27,27,27`) is a CPU-priority hint only — it does NOT
  // surrender the CPU to the OS scheduler, so on workloads with more guest
  // threads than host SMT contexts (or where the lock-holder is parked by
  // the OS), a tight x86 spinlock loop will starve out the lock-holder
  // indefinitely. The counter gates how often we pay the syscall cost
  // (~100ns) vs the SMT-hint cost (1 cycle).
  uint32_t PauseCount {};

  // Pointer to the ppc64le helper-address table. JIT-emitted call sites
  // load `ld TMP1, PPC64_HelperTable_off(STATE); ld TMP1, HELPER*8(TMP1);
  // mtctr TMP1; bctrl` to reach a host C helper (SplitLockEmulate, CRC32,
  // RDRAND, VAES/VSha/PCLMUL, VPCMPESTRX/ISTRX, F64 impls, F16 converters)
  // without baking that helper's absolute address into the block. Placed in
  // the existing pad between PauseCount (uint32) and the 8-byte-aligned
  // Pointers member so CpuStateFrame stays a multiple of 128 bytes and the
  // ARM64 struct layout is unchanged. See P2.1 in docs/TASK_QUEUE.md.
  uint64_t* PPC64_HelperTable {};
#endif

  // Pointers that the JIT needs to load to remove relocations
  JITPointers Pointers;
};
static_assert(offsetof(CpuStateFrame, State) == 0, "CPUState must be first member in CpuStateFrame");
static_assert(offsetof(CpuStateFrame, Pointers) % 8 == 0, "JITPointers need to be aligned to 8 bytes");
static_assert(offsetof(CpuStateFrame, Pointers) + sizeof(CpuStateFrame::Pointers) <= 32760, "JITPointers maximum pointer needs to be less "
                                                                                            "than architecture maximum 32768");
#ifdef ARCHITECTURE_ppc64le
// Ensure adjacent CpuStateFrame allocations land on disjoint 128-byte cache
// lines (POWER8 line size). Without size-multiple-of-128, the trailing bytes
// of one frame share a line with the leading bytes of the next, which
// causes cross-thread atomic reservations on adjacent frames to cancel each
// other and produces livelock patterns that don't reproduce on ARM64.
static_assert((sizeof(CpuStateFrame) % 128) == 0,
              "CpuStateFrame size must be a multiple of 128 bytes on POWER8 to avoid "
              "false sharing across adjacent per-thread frame allocations. Add a "
              "trailing pad inside the struct to absorb the slop.");
#endif

static_assert(std::is_standard_layout<CpuStateFrame>::value, "This needs to be standard layout");
static_assert(sizeof(CpuStateFrame::SynchronousFaultData) == 8, "This needs to be 8 bytes");
static_assert(alignof(CpuStateFrame::SynchronousFaultDataStruct) == 8, "This needs to be 8 bytes");
static_assert(offsetof(CpuStateFrame, SynchronousFaultData) % 8 == 0, "This needs to be aligned");
// The JIT pokes the fault page with `ld TMP, off(STATE); stb r0, 0(TMP)`, and the ld is
// a D-form load whose displacement field is a signed 16-bit immediate. The old embedded
// array had to be within one page of BaseFrameState for the same reason; the pointer
// has to be within signed-16-bit reach of it.
static_assert(offsetof(CpuStateFrame, InterruptFaultPagePtr) + sizeof(void*) <= 32768,
              "InterruptFaultPagePtr must be reachable from STATE with a signed 16-bit D-form displacement");
} // namespace FEXCore::Core
