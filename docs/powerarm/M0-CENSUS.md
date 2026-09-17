# POWERarm M0b census: x86 guest core removed, AArch64 skeleton in place

M0b removes fastppcx86's x86 guest and puts in an AArch64 skeleton that builds and runs to a
clean first-instruction failure:

- an AArch64 `CPUState` with static register maps
- an empty A64 frontend
- an AArch64-only ELF loader
- the asm-generic syscall table

This document records what that cost, what is still coupled to x86, and where the design
decisions are waiting. It covers the tasks in the M0b brief. DESIGN.md §10.1 points here.

- **Fork base:** `9da3ccf9f` (`daedalao-wt`, DESIGN.md §10.3)
- **M0b start:** `ceb23fdc6`
- **Measured at:** `8c221a4ec`, plus the commit that adds this file
- **Build host:** 192.168.2.24, ppc64le, kernel `7.2.6-64k`, page size 65536
- **Guest build machine:** Raspberry Pi 5, Debian, gcc 14.2.0, kernel `6.18.39+rpt-rpi-2712`

## Commits

| Commit | Subject | Builds? |
|---|---|---|
| `096a8d383` | drop IS_32BIT_THUNK paths from thunk sources | **No.** CMake fails at `Source/Tools/CMakeLists.txt:23`. The commit also swept in the staged deletion of `Source/Tools/CodeSizeValidation/`, while its `add_subdirectory` goes away only in the next commit. The thunk change was verified on its own before commit |
| `642716057` | remove TestHarnessRunner, CodeSizeValidation and zydis | Yes |
| `4097b8b97` | remove the EC-transition IR op and bridge-only core code | Yes |
| `617f0fbad` | replace the x86 frontend with an empty A64 frontend | Yes |
| `a17877bd8` | remove x87, CPUID and other x86-only IR ops and lowerings | Yes |
| `9782cb76a` | replace the x86 CPUState with an AArch64 CPUState | **No, by design** (369 errors). LinuxEmulation and the tools still use the x86 state |
| `cc6f278ce` | port the Linux layer skeleton to AArch64 | **No, by design** (4 errors). FEXInterpreter and the offline compiler are ported in the next commit |
| `bec41c77f` | accept only AArch64 ELFs in the loader | Yes |
| `6b11e3683` | force-disable TSO emulation and mark remaining x86 machinery | Yes |
| `8c221a4ec` | wire the generic passthroughs that were x86-64-only | Yes |

**Per-commit verification.** Each commit was checked out in a throwaway worktree and built
incrementally with the same CMake options as `build-powerarm` (submodules symlinked from the
main tree). The Builds? column is that result (`/tmp/pa-m0b/verify.txt` on the build host).
Non-building commits: `096a8d383` (accidental), `9782cb76a` and `cc6f278ce` (planned run).

**Cold build of HEAD.** `rm -rf build-powerarm`, then CMake and ninja with:

- Release, clang, ThinLTO, ccache
- `BUILD_TESTING=OFF`, `BUILD_THUNKS=OFF`

The options match the previous cache except for the removed `ENABLE_ZYDIS`. **Result:**
rc=0, 203 build steps, 18.6 s wall with a warm ccache, 0 errors.

**Warnings:** 204 of the 255 are the pre-existing `fmt` deprecation. The ones M0b introduced
(unused 32-bit robust-list helpers, the unused GDB `CTX`) were fixed.

**Not built:**

- `BUILD_TESTING=ON`: unittests, FEXLinuxTests and the ASM suites are x86 test corpora and
  were not attempted.
- `BUILD_THUNKS=ON`: at `096a8d383` only the drm, EGL, wayland and xshmfence host libs were
  built. asound, GL and vulkan need an x86 sysroot that the host doesn't have.

## Smoke test

### Binary

`hello-a64` was built on the Pi with `gcc -static -O2 -o hello-a64 hello.c` and copied to
`/tmp/pa-m0b/`.

- **sha256:** `4ecd19182ca116a517d5cf8d727441bc4d613cb0aee73f82257fd8cf0f9720ff`
- **`file`:** `ELF 64-bit LSB executable, ARM aarch64, version 1 (GNU/Linux), statically linked, BuildID[sha1]=3e8b7efc8f06d3e522ba1ad46a2250676b72d0ed, for GNU/Linux 3.7.0, not stripped`
- **Native run on the Pi:** `hello from aarch64`
- **`readelf -h`:** `Entry point address: 0x400600`
- **`objdump -d --start-address=0x400600`:** `400600: d503245f  bti c`
- **`readelf -lW` PT_LOAD alignment:** both segments are 64K-aligned:

  ```
  LOAD 0x000000 0x0000000000400000 0x0000000000400000 0x07f6dc 0x07f6dc R E 0x10000
  LOAD 0x08c160 0x000000000048c160 0x000000000048c160 0x0057e4 0x00acb8 RW  0x10000
  ```

  A stock Debian aarch64 static binary would therefore not need the 4K granule emulation (see
  *Granule infrastructure* below).

### `hello-a64` under POWERarm

Command: `POWERARM_HOSTPAGEMODE=force build-powerarm/Bin/POWERarm ./hello-a64`. Output
(stderr, verbatim):

```
FEX: WARNING: host page size is 65536; guest mappings still go through the 4096-byte granule
emulation on top of the larger host page, although AT_PAGESZ reports the host page (docs/PAGE_SIZE_64K_EXECUTION.md):
  * loader, brk, ASLR and the allocators are host-granular (S2/S4a),
  * guest mmap/mprotect/munmap below the host page go through the granule table
    in the permissive tier: a granule is mapped/protected as the union of its
    live guest pages, so a guest cannot rely on a fault inside a shared granule (S4b),
  * SMC tracking (SMCChecks=mtrack) arms whole host pages and re-arms after a
    write to a shared granule (S4c).

This is tested on the gaming lanes but not proven for every guest; a guest that
depends on sub-granule faults (GC write barriers, guard pages) may misbehave.

FEX_HOSTPAGEMODE=abort (default) refuses to start; =force continues with the
configured SMCChecks (recommended: mtrack, the 4K configuration); =degrade
continues and forces SMCChecks=full, which is several times slower.
FEX: FEX_HOSTPAGEMODE=force -- continuing, forcing nothing. This is a bring-up aid
for working on host-page-size support, not a supported configuration.
E FEXServer returned empty rootfs path; keeping configured value
POWERarm: unimplemented A64 instruction 0xd503245f at pc 0x400600
exit=132
```

The shell adds `Illegal instruction (core dumped)`. **The output matches:**

- pc `0x400600` is the ELF entry.
- The word `0xd503245f` is the first instruction at the entry.
- Exit 132 is 128 + SIGILL, the default action.

### Non-AArch64 ELF

Command: `POWERARM_HOSTPAGEMODE=force build-powerarm/Bin/POWERarm /bin/true`. The target is
the host's own ELF64 PowerPC64 binary. The page-size banner is the same as above; after it:

```
E FEXServer returned empty rootfs path; keeping configured value
POWERarm: '/bin/true' is not a supported ELF: only ELFCLASS64, ELFDATA2LSB, EM_AARCH64, ET_EXEC or ET_DYN binaries can run
exit=248
```

Exit 248 is `-ENOEXEC`, as before.

## Static register map (a64)

Definitions:

- `FEXCore/Source/Interface/Core/ArchHelpers/PPC64Emitter.h` `namespace a64`
- `FEXCore/include/FEXCore/Core/CoreState.h` `StaticGPRGuestReg`

| SRA slot | Guest | Host |
|---|---|---|
| 0-5 | X0-X5 | r7-r12 |
| 6-8 | X6-X8 | r14-r16 |
| 9-14 | X19-X24 | r17-r22 |
| 15 | X29 (FP) | r23 |
| 16 | X30 (LR) | r28 |
| 17 | SP | r29 |
| dynamic GPR pool | - | r24, r25, r26, r30, r31 (all callee-saved) |
| static vectors | V0-V15 | VR0-VR15 |
| dynamic vector pool | - | VR16-VR29 (VR16-19 volatile); VR30/31 are VTMP1/VTMP2 |

**Context-backed, not pinned:** X9-X18, X25-X28, V16-V31, FPCR, FPSR, TPIDR_EL0 and the
exclusive monitor. NZCV lives in host CR0 inside JIT code and is spilled to `State.nzcv`.

**Why this shape:**

- **18 GPR slots.** That's what the backend has (16 x86 GPRs plus PF/AF). The spill/fill,
  kInSyscallSentinel partial refill and signal SpillSRA code are all sized to it.
- **Which X registers.** DESIGN.md §4.1 asked for X0-X8, X19-X30 and SP, which is 22. With 18
  slots, X25-X28 were dropped. They're the least-used callee-saved registers in typical AAPCS64
  code, but this is **unmeasured**: §4.1's rootfs `.text` register census is still owed.
- **Vectors.** §4.1 proposed pinning V0-V31 to vs32-vs63, the whole VMX half. That can't be
  done in this backend:
  - The dynamic vector pool (RAFPR) and VTMP1/VTMP2 are `VR`-typed.
  - Most vector lowerings are VMX-form (`vperm*`, `vsel`, `vcmp*`, `lvx`/`stvx`) and cannot
    address vs0-vs31.
  
  So the VMX half is split as the x86-64 map split it: 16 pinned, 14 dynamic, 2 temporaries.
  Pinning more needs the dynamic pool moved to vs0-vs31 with VSX-form lowerings, or a VSX-aware
  allocator class.

**Layout asserts in CoreState.h:**

- Every field the JIT addresses is within DS-form reach (≤ 32764) and on a multiple of 4.
- `v[]` sits on 16-byte offsets, because `lvx`/`stvx` drop the low 4 EA bits.
- `sp` is `GPROffset(31)`.
- `callret_*` fields are paired.

## (a) Diff stats per subsystem

`git diff --numstat <base> 8c221a4ec`, bucketed by path prefix. "Core (other)" is
`FEXCore/Source/Interface/Core/*` outside the named directories. Counts are lines.

| Subsystem | files (9da3ccf9f) | +/- (9da3ccf9f) | files (ceb23fdc6) | +/- (ceb23fdc6) |
|---|---:|---:|---:|---:|
| JIT/PPC64LE backend | 10 | +89 / -3235 | 10 | +89 / -3235 |
| Core/ArchHelpers (PPC64Emitter) | 2 | +103 / -297 | 2 | +103 / -297 |
| A64Frontend (new) | 4 | +415 / -0 | 4 | +415 / -0 |
| x86 frontend (OpcodeDispatcher/X86Tables/Decoder) | 30 | +0 / -26928 | 30 | +0 / -26928 |
| Core/Interpreter (FABI fallbacks) | 6 | +0 / -1366 | 6 | +0 / -1366 |
| Core (other) | 7 | +70 / -3151 | 7 | +70 / -3151 |
| IR + passes | 10 | +21 / -1993 | 10 | +21 / -1993 |
| Context | 2 | +6 / -89 | 2 | +6 / -89 |
| Config | 1 | +9 / -77 | 1 | +2 / -70 |
| FEXCore/include | 8 | +100 / -530 | 7 | +99 / -529 |
| FEXCore (other) | 6 | +47 / -794 | 4 | +3 / -758 |
| LinuxEmulation | 143 | +1257 / -18281 | 143 | +1251 / -18275 |
| FEXInterpreter + loader | 4 | +80 / -1677 | 3 | +59 / -1646 |
| CommonTools (ELF parser) | 4 | +72 / -917 | 4 | +72 / -917 |
| FEXOfflineCompiler | 2 | +8 / -57 | 1 | +6 / -57 |
| TestHarnessRunner | 4 | +0 / -790 | 4 | +0 / -790 |
| CodeSizeValidation | 2 | +0 / -707 | 2 | +0 / -707 |
| Source/Tools (other) | 68 | +22 / -15748 | 3 | +0 / -11 |
| Source/Common | 4 | +26 / -56 | 2 | +5 / -4 |
| Source (other) | 9 | +106 / -368 | 0 | +0 / -0 |
| ThunkLibs | 24 | +18 / -11278 | 15 | +6 / -10422 |
| External (SoftFloat-3e, cephes, zydis) | 122 | +0 / -13406 | 119 | +0 / -13403 |
| unittests | 2500 | +8 / -355194 | 1 | +1 / -7 |
| docs | 13 | +731 / -1410 | 2 | +0 / -7 |
| other (top-level, CMake, Scripts, Data) | 49 | +154 / -2372 | 6 | +1 / -71 |
| **total** | 3034 | +3342 / -460721 | 388 | +2209 / -84733 |

`CodeEmitter/` is unchanged. JIT/PPC64LE lost 3,235 lines, almost all of them deleted ops:

- F64/F80 and x87
- CPUID/XGetBV
- PF/AF load/store
- VAESKeyGenAssist and VPCMPxSTRx
- the FABI bridge

Its 89 added lines are register-map and state-offset substitutions and TODO markers. Nothing
was renamed or reformatted.

## (b) Compile-error classes

**How these were counted.** Errors are unique `file:line + message` pairs across the iterative
build logs of each phase. **They undercount:** a `file not found` is fatal to its translation
unit and hides everything after it. So "needs design decision" work mostly surfaced while fixing
a file, not as a first error. Those items are listed below the table and carry TODO markers.

| Phase (commit) | Mechanical rename | x86-semantic removed | Needs design decision | Knock-on / own intermediate |
|---|---:|---:|---:|---:|
| Frontend swap (`617f0fbad`) | 4 | 0 | 0 | 2 |
| x87/CPUID/PF/AF removal (`a17877bd8`) | 0 | 14 | 0 | 0 |
| CPUState swap, Linux layer, loader (`9782cb76a`..`8c221a4ec`) | 69 | 99 | 19 | 18 |
| **total** | **73** | **113** | **19** | **20** |

### Mechanical rename

Representative errors:

- `FEXCore/Source/Interface/Core/JIT/PPC64LE/PPC64Dispatcher.cpp:273`: `no member named 'rip'`,
  now `pc`
- `FEXCore/Source/Interface/Core/Core.cpp:766`: `gregs`/`X86State`, now `x[]`/`sp`
- `FEXCore/Source/Interface/Core/JIT/PPC64LE/ALUOps.cpp:3506`: `x64::SRA`, now `a64::SRA`
- `Source/Tools/LinuxEmulation/LinuxSyscalls/Syscalls/FS.cpp:9`: `LinuxSyscalls/x64/Syscalls.h`
  not found, now `Arm64/Syscalls.h`
- `Source/Tools/LinuxEmulation/LinuxSyscalls/Syscalls/Passthrough.cpp:1072`:
  `SYSCALL_Arm64_prlimit_64`, aliased to `prlimit64`
- `FEXCore/Source/Interface/Context/Context.cpp:3`: dead `OpcodeDispatcher.h` include

### x86-semantic removed

Representative errors:

- `FEXCore/Source/Interface/Core/CodeCache.cpp:1790`: `segment_arrays`/`gdt`/`cs_idx`
  (segmentation)
- `Source/Tools/LinuxEmulation/LinuxSyscalls/ThreadManager.cpp:416`: GDT/LDT per-thread state
- `FEXCore/Source/Interface/IR/Passes/RedundantFlagCalculationElimination.cpp:354`:
  `OP_LOADPF`/`OP_STOREAF` (parity/aux-carry)
- `Source/Tools/LinuxEmulation/LinuxSyscalls/FileManagement.cpp:74`: `Is64BitMode` (32-bit
  guest)
- `Source/Tools/LinuxEmulation/LinuxSyscalls/Syscalls/FD.cpp:42`: `SYSCALL_Arm64_open`. This
  is one of 39 x86-only syscall names in shared handlers, now mapped to -1 in
  `LegacySyscallsEnum.h`.
- `FEXCore/include/FEXCore/Core/Context.h:9`: `CPUID.h`

### Needs design decision

Representative errors:

- `Source/Tools/LinuxEmulation/LinuxSyscalls/Seccomp/SeccompEmulator.cpp:631`: per-mode filter
  tables and the audit arch. Now a single `AUDIT_ARCH_AARCH64` filter.
- `Source/Tools/LinuxEmulation/LinuxSyscalls/GdbServer.cpp:24`: register layout. Now the gdb
  `aarch64.core`/`.fpu` layout.
- `Source/Tools/LinuxEmulation/Thunks.cpp:15`: callback argument registers. Now X0/X1, but the
  ABI decision is still open.

Decisions found behind fatal errors while fixing files. Each has a TODO:

- **TLS:** `SetThreadArea` becomes `tpidr_el0`. The pc-after-`svc` resume for a clone child is
  still unverified.
- **Signal frames:** the arm64 `rt_sigframe` plus frame record. `AT_MINSIGSTKSZ` comes from
  it.
- **Auxv:** `AT_PAGESZ` is now the host page; `AT_HWCAP` is 0; `AT_PLATFORM` is `aarch64`.
- **VA size:** 47-bit clamp vs 48-bit.
- **x86 trap-flag single-step:** gone.
- **AVX-high VSX bank:** stubbed.
- **Cache line size for CLZERO/DC ZVA.**
- **FEX_GUESTTRACE:** hardcoded SRA indices.
- **Callback return convention:** return address on the guest stack vs X30.
- **SMC semantic-patch site decoders.**
- **ioctl and mlockall flag encodings.**

## (c) POWERARM-M0-TODO markers by area

54 markers after this commit (`grep -rn POWERARM-M0-TODO FEXCore Source ThunkLibs`).

| Area | Count |
|---|---:|
| cpustate | 4 |
| frontend | 3 |
| ir | 2 |
| backend | 8 |
| tso | 1 |
| syscalls | 10 |
| signals | 6 |
| loader | 5 |
| thunks | 8 |
| smc | 4 |
| config | 1 |
| other | 2 |

#### cpustate (4)

- `FEXCore/include/FEXCore/Core/CoreState.h:131`: layout only; whether the hardware larx/stcx. fast path needs extra per-thread state is an M4 decision.
- `Source/Tools/FEXInterpreter/ELFCodeLoader.h:1061`: hwcap profile per DESIGN §4.8; AT_HWCAP/AT_HWCAP2 stay 0 until the A64 feature profile is decided.
- `Source/Tools/LinuxEmulation/LinuxSyscalls/EmulatedFiles/EmulatedFiles.cpp:71`: arm64 /proc/cpuinfo layout with a placeholder profile; the Features line, implementer and part must come from the hwcap/ID-register profile (DESIGN.md §4.5, §4.8).
- `Source/Tools/LinuxEmulation/LinuxSyscalls/GdbServer.cpp:306`: reconstruct NZCV from the host CR0/XER flag cache when the thread is stopped inside JIT code.

#### frontend (3)

- `FEXCore/Source/Interface/Core/A64Frontend/Decoder.h:98`: single-instruction blocks only; multiblock discovery (direct B/B.cond/CBZ/TBZ targets) and a pooled decode buffer come with the M2 translator.
- `FEXCore/Source/Interface/Core/A64Frontend/IRBuilder.cpp:99`: no A64 translators yet; every instruction raises SIGILL. M1 interpreter / M2 translators replace this.
- `Source/Tools/LinuxEmulation/LinuxSyscalls/SignalDelegator.cpp:1476`: keep or narrow once real translators exist; a genuine guest UDF should still die quietly like on arm64 Linux.

#### ir (2)

- `FEXCore/Source/Interface/IR/Passes/RedundantFlagCalculationElimination.cpp:445`: InvalidateFlags now takes PSTATE NZCV bit positions (was x86 RFLAG_*_RAW_LOC); the A64 frontend must emit it that way.
- `FEXCore/include/FEXCore/Core/CoreState.h:123`: IR.json's LoadContext/StoreContext validation used to forbid context access to all x86 GPRs (all were SRA); it must now forbid only the SRA-pinned subset (a64::StaticGPRGuestReg).

#### backend (8)

- `FEXCore/Source/Interface/Core/ArchHelpers/PPC64Emitter.h:327`: pinning more than 16 guest V registers needs the dynamic vector pool moved to vs0-vs31 with VSX-form lowerings, or a VSX-aware allocator class.
- `FEXCore/Source/Interface/Core/Core.cpp:1087`: the x86 trap-flag single-step check is gone; A64 EL0 has no TF equivalent (software step comes via ptrace/gdbserver).
- `FEXCore/Source/Interface/Core/JIT/PPC64LE/JIT.cpp:1345`: diagnostic still labels the values with x86 names; they are SP, X0, X1 and X8 of the A64 guest.
- `FEXCore/Source/Interface/Core/JIT/PPC64LE/JIT.cpp:2831`: the x86 TF (trap flag) check lived here; CheckTF is always false for the A64 guest.
- `FEXCore/Source/Interface/Core/JIT/PPC64LE/JIT.cpp:5386`: FEX_GUESTTRACE hardcodes x86 SRA meanings (index 1 = RCX deref base, index 4 = RSP return-address slot); under the a64 map those are X1 and X4. The compiler cannot see this coupling.
- `FEXCore/Source/Interface/Core/JIT/PPC64LE/MemoryOps.cpp:2266`: was CPUIDEmu::CACHELINE_SIZE (the x86 CLFLUSH/CLZERO line size); A64 DC ZVA zeroes DCZID_EL0-sized blocks, which is a guest-ABI decision.
- `FEXCore/Source/Interface/Core/JIT/PPC64LE/MemoryOps.cpp:25`: the x86 AVX-high VSX bank has no AArch64 state behind it (State.avx_high is gone); the hook stays so these lowerings keep their fastppcx86 shape.
- `FEXCore/Source/Interface/Core/JIT/PPC64LE/MemoryOps.cpp:36`: same as `MemoryOps.cpp:25`.

#### tso (1)

- `FEXCore/Source/Interface/Context/Context.h:565`: TSO emulation is force-disabled. The barrier IR ops, backend lowering, PROT_SAO path and config are x86-TSO machinery wired through the whole JIT, so they stay in-tree but inert until the AArch64 guest memory-ordering story is decided.

#### syscalls (10)

- `Source/Tools/LinuxEmulation/CMakeLists.txt:74`: the x86_32/x86_64 guest struct headers were deleted with LinuxSyscalls/x32 and x64; arm64 guest layout headers go here (verified as aarch64).
- `Source/Tools/LinuxEmulation/LinuxSyscalls/Arm64/LegacySyscallsEnum.h:12`: move the legacy-only registrations out of the shared files (or behind an x86 guard) once fastppcx86 cherry-picks into them stop mattering, and delete this header.
- `Source/Tools/LinuxEmulation/LinuxSyscalls/Arm64/Syscalls.cpp:28`: the shared handlers were written against x86-64 guest values; flag/struct translation (O_* flags, termios, ioctl encoding, epoll_event packing, struct stat) must be re-derived for arm64 -> powerpc64 (DESIGN.md §5).
- `Source/Tools/LinuxEmulation/LinuxSyscalls/Arm64/Syscalls.cpp:44`: the x86-64-specific handlers (mmap family, clone/exit, rt_sigaction/rt_sigreturn, fstat/newfstatat, prctl, arch-specific TLS) were deleted with LinuxSyscalls/x64 and need arm64 versions (M1).
- `Source/Tools/LinuxEmulation/LinuxSyscalls/FaultSafeUserMemAccess.cpp:180`: the arm64 handlers that copy guest structs through possibly-bad pointers should call these.
- `Source/Tools/LinuxEmulation/LinuxSyscalls/Syscalls/Passthrough.cpp:1179`: ioctl is not wired: the x86 path re-encoded _IOC direction/size bits and TTY numbers for powerpc, and the arm64 guest needs its own table (asm-generic _IOC, generic termios).
- `Source/Tools/LinuxEmulation/LinuxSyscalls/Syscalls/Passthrough.cpp:1180`: mlockall is not wired: powerpc MCL_* values (0x2000/0x4000/0x8000) differ from asm-generic (1/2/4) and need translation.
- `Source/Tools/LinuxEmulation/LinuxSyscalls/Syscalls/Thread.cpp:172`: the child resumes at State.pc; confirm the Syscall op has already advanced pc past svc #0 (x86 needed rip += 2 here).
- `Source/Tools/LinuxEmulation/LinuxSyscalls/Syscalls/Thread.cpp:295`: same as `Thread.cpp:172`.
- `Source/Tools/LinuxEmulation/VDSO_Emulation.cpp:648`: the guest vDSO is still the thunked x86 libVDSO-guest.so; arm64 needs its own (__kernel_clock_gettime, __kernel_rt_sigreturn; DESIGN.md §5).

#### signals (6)

- `Source/Tools/FEXInterpreter/ELFCodeLoader.h:1070`: grow this with the fpsimd/esr/SVE records once the signal frame carries them.
- `Source/Tools/LinuxEmulation/ArchHelpers/MContext.h:35`: AArch64 describes the fault through ESR_EL1 (esr_context); map DSISR to an ESR data-abort syndrome instead.
- `Source/Tools/LinuxEmulation/ArchHelpers/UContext.h:7`: layout only. SetupFrame_Arm64/RestoreFrame_Arm64 do not yet build or consume these frames (fpsimd_context in __reserved[], esr_context, the rt_sigreturn trampoline in the guest vDSO).
- `Source/Tools/LinuxEmulation/LinuxSyscalls/SignalDelegator/GuestFramesManagement.cpp:109`: restore FPCR/FPSR/V0-V31 from the fpsimd_context record once SetupFrame_Arm64 emits it.
- `Source/Tools/LinuxEmulation/LinuxSyscalls/SignalDelegator/GuestFramesManagement.cpp:25`: skeleton only. __reserved[] carries no fpsimd_context/esr_context records yet, SA_RESTORER vs the guest vDSO __kernel_rt_sigreturn is not handled, and sigaltstack/redzone rules have not been checked against the arm64 kernel.
- `Source/Tools/LinuxEmulation/VDSO_Emulation.cpp:616`: the fallback trampolines are still the x86 FEX CALLBACKRET instruction bytes; the A64 guest needs an __kernel_rt_sigreturn (mov x8, #139; svc #0) and a callback-return encoding the A64 frontend decodes.

#### loader (5)

- `Source/Common/HostPageGate.h:109`: an AArch64 guest built for 64K pages (PT_LOAD p_align >= host page) needs none of the granule emulation below; skip this gate for it once the loader records p_align.
- `Source/Tools/CommonTools/Linux/Utils/ELFContainer.cpp:179`: ELFContainer is only used for GetELFType now; its MODE_32BIT parsing branches are dead and should go with the class.
- `Source/Tools/FEXInterpreter/ELFCodeLoader.h:599`: the 47-bit clamp is the x86-64 user VA limit; an AArch64 Linux guest expects 48-bit VA (DESIGN.md guest VA size).
- `Source/Tools/FEXInterpreter/ELFCodeLoader.h:766`: fallback to 4K granule emulation for binaries whose PT_LOAD p_align < host page
- `Source/Tools/FEXInterpreter/FEXInterpreter.cpp:527`: an unloadable PT_INTERP lands here too; report the interpreter/RootFS separately once dynamic binaries are supported.

#### thunks (8)

- `FEXCore/Source/Interface/Core/Core.cpp:1590`: the x86-64 trampoline passed its own address in R11; X16 (IP0) is the AAPCS64 veneer register but the guest-thunk ABI is an M5 decision.
- `FEXCore/Source/Interface/Core/JIT/PPC64LE/BranchOps.cpp:100`: x86 callback convention (return address pushed on the guest stack); an AArch64 callback returns through X30 and should not touch SP.
- `FEXCore/Source/Interface/Core/JIT/PPC64LE/JIT.cpp:1923`: x86 callback-sentinel walk over the guest stack; an AArch64 callback returns through X30.
- `FEXCore/Source/Interface/Core/JIT/PPC64LE/PPC64Dispatcher.cpp:1006`: x86-64 callback convention (push ThunkCallbackRet as the return address); an AArch64 callback should load X30 instead.
- `Source/Tools/LinuxEmulation/LinuxSyscalls/FileManagement.cpp:61`: overlay paths are the AArch64 multiarch/lib64 layout; revisit with the Arch Linux ARM rootfs (DESIGN.md §6.4) and the M5 guest thunks.
- `Source/Tools/LinuxEmulation/Thunks.cpp:242`: callback ABI is still FEX's x86-64 one moved onto X0/X1; the guest-side thunk callback unpackers must be rebuilt for AAPCS64.
- `Source/Tools/LinuxEmulation/Thunks.cpp:643`: low-4GB host ranges only mattered for 32-bit x86 guests; drop this export with the host thunk pool once thunks are redesigned.
- `ThunkLibs/Generator/main.cpp:99`: the guest data-layout parse still targets x86_64-linux-gnu; it needs an aarch64 (AAPCS64) guest model (DESIGN.md §6.1).

#### smc (4)

- `FEXCore/Source/Interface/Core/Core.cpp:906`: SMC Idea 4 (FEX_SMCSEMANTICPATCH) site tables stay empty; the x86 rel32/mov-imm site decoders have no A64 counterpart yet (ADRP/MOVZ/B imm26 are the analogues).
- `FEXCore/Source/Interface/Core/SMCSemanticPatch.h:348`: DecodeRel32BranchSite and DecodeMovImmSite below recognise x86 encodings and have no caller since the x86 frontend was removed; the A64 frontend needs B/BL/B.cond/CBZ/TBZ imm-field site recording and a MOVZ/MOVK counterpart.
- `Source/Tools/CommonTools/Linux/Utils/ELFParser.h:292`: code-cache relocation classes were chosen for x86-64 data relocations; re-check which AArch64 relocations can land in cached code ranges.
- `Source/Tools/FEXOfflineCompiler/Main.cpp:154`: the offline compiler only countered 32-bit x86 relocations here; decide whether AArch64 RELATIVE/ABS64 data needs the same treatment.

#### config (1)

- `Source/Common/HostFeatures.cpp:469`: HostFeatures still enumerates x86 guest feature overrides (AVX, AVX2, SSE4a, ...) and Config.json.in keeps other x86-only options (HideHypervisorBit, CPUID-backed CPU count, TSO tuning); prune them when the A64 feature profile lands.

#### other (2)

- `FEXCore/Source/Interface/Context/Context.h:358`: MonoHacks are x86 Unity/Mono JIT workarounds; the A64 frontend has no consumer. Remove together with the Linux-layer Mono detection.
- `Source/Tools/LinuxEmulation/LinuxSyscalls/Syscalls.h:449`: the Is64Bit parameters and the 32-bit allocator only served 32-bit x86 guests and x86-64 MAP_32BIT; AArch64 has neither, so collapse these to the 64-bit path when the arm64 mmap handlers are written (they also need asm-generic -> powerpc MAP_* translation).

## (d) x86 regex counts per subsystem

**Method.** Matching lines per file, from:

```
git grep -c -P 'gregs|\bxmm\b|X86State|RFLAG_|x87|X87|F80|mxcsr|MXCSR|pf_raw|af_raw|\brip\b|CPUID|XGetBV|fs_cached|gs_cached|_idx\b|AVX|SSE' <rev> -- '*.cpp' '*.h' '*.inl'
```

The per-file counts were then summed per subsystem.

**Comparison with DESIGN.md §10.1.** The 9da3ccf9f column reproduces the pre-M0b baselines for
several subsystems: JIT/PPC64LE 537, IR 194, include 176, LinuxEmulation 488, ThunkLibs 176,
CodeEmitter 8. §10.1's own figures are higher because it counted `.json`/`.py` too and used a
slightly different pattern.

| Subsystem | 9da3ccf9f | ceb23fdc6 (M0b start) | HEAD |
|---|---:|---:|---:|
| CodeEmitter | 8 | 8 | 8 |
| JIT/PPC64LE | 537 | 537 | 174 |
| Core/ArchHelpers | 47 | 47 | 17 |
| x86 frontend | 2180 | 2180 | 0 |
| Core/Interpreter | 164 | 164 | 0 |
| CPUID emulation | 302 | 302 | 0 |
| Core (other) | 114 | 114 | 19 |
| IR + passes | 194 | 194 | 13 |
| Context | 18 | 18 | 4 |
| FEXCore/Utils | 9 | 9 | 9 |
| FEXCore/include | 176 | 176 | 39 |
| FEXCore (other) | 33 | 33 | 0 |
| LinuxEmulation | 488 | 488 | 107 |
| FEXInterpreter | 54 | 54 | 5 |
| CommonTools | 112 | 112 | 0 |
| TestHarnessRunner | 45 | 45 | 0 |
| CodeSizeValidation | 4 | 4 | 0 |
| Source/Tools (other) | 319 | 4 | 1 |
| Source/Common | 27 | 27 | 28 |
| Source (other) | 1 | 0 | 0 |
| ThunkLibs | 176 | 176 | 171 |
| External | 102 | 102 | 0 |
| unittests | 140 | 0 | 0 |
| **total** | 5250 | 4794 | 595 |

**What remains at HEAD.** 595 matching lines, by token: SSE 243, AVX 161, rip 91, _idx 90,
x87/X87 57, gregs 23, CPUID 19, xmm 13, MXCSR 9, F80 8, RFLAG_ 7, X86State 2.

- **Mostly comments and names.** Examples: the ppc64le lowering comments that describe which
  SSE/AVX op a sequence came from, and `rip` in diagnostic strings.
- **JIT/PPC64LE (174).** The live code is the AVX-high bank hooks in `MemoryOps.cpp`, which now
  always miss. The rest is comments and log text.
- **LinuxEmulation (107).** `ArchHelpers/MContext_x86_64.h` (host-side x86 context, unbuilt on
  ppc64le) and SignalDelegator comments.
- **ThunkLibs (171).** Dominated by `libGL/glcorearb.h` (113, GL enum names) and the generator
  (x86_64 data-layout model).
- **Source/Common (+1).** The new TODO(config) comment names AVX/SSE4a.

## (e) Surprises

1. **ThinLTO silently dropped the ppc64le fault-safe copy routines.** Their only callers were
   the x32 syscall handlers. Once those were gone, ThinLTO discarded the naked
   `CopyFromUser`/`CopyToUser`, and with them the `*_FaultInst` labels that `IsFaultLocation`
   still references. The result was a link failure that no compile step reports. They're now
   `__attribute__((used))`.
2. **~70 generic passthroughs were registered only for x86-64.** Examples: `readv`, `futex`,
   `clock_gettime`, `ppoll`, `pread64`, `set_robust_list`. They lived in `namespace x64`, not in
   `RegisterCommon`, so deleting `x64/` silently -ENOSYS'd them.
   - Rewired by asm-generic number, except ioctl and mlockall.
   - The ppc64le ioctl remap was written against **x86** `_IOC` encoding.
   - powerpc `MCL_*` values differ from asm-generic.
3. **The GUESTTRACE diagnostic hardcodes x86 SRA meanings** (`JIT.cpp:5386`). Slot 1 is
   "RCX" and slot 4 is "RSP". It compiles unchanged under the a64 map and prints the wrong
   registers.
4. **The thunk callback convention is x86 stack-return, baked into the JIT:**
   - `PPC64Dispatcher.cpp:1006` pushes the return sentinel onto the guest stack.
   - `BranchOps.cpp:100` pops it.
   - `JIT.cpp:1923` walks the guest stack looking for it.

   AAPCS64 wants X30.
5. **Removing the FABI fallback bridge (x87/F80 softfloat) leaves `Op_Unhandled` as a hard
   die.** fastppcx86 routed several ops through it. The A64 frontend must never emit an op
   the backend doesn't lower.
6. **The x86 AVX-high VSX bank is threaded through `MemoryOps.cpp` load/store-context
   lowerings.** It had to be stubbed (slot lookup returns -1) rather than removed, to keep the
   backend churn down.
7. **The vector pinning ceiling is 16, not 32.** The dynamic vector pool and temporaries are
   VMX-typed, so DESIGN.md §4.1's V0-V31 → vs32-vs63 plan conflicts with the allocator. See the
   register map above.
8. **39 x86-only syscall names are referenced from shared handler files.** Examples: `open`,
   `pipe`, `poll`, `iopl`, `arch_prctl`, `alarm`. `LegacySyscallsEnum.h` maps them to -1 so
   the shared files stay cherry-pickable from fastppcx86.
9. **The old host-page banner claimed the guest is told `AT_PAGESZ=4096`.** It was corrected,
   because AT_PAGESZ now reports 65536. `hello-a64` is itself 64K-aligned, so for Debian aarch64
   binaries the granule emulation is usually unnecessary.
10. **Dead code only found by deletion:**
    - `Source/Tools/FEXInterpreter/SignalDelegator.cpp`, an unbuilt copy
    - `Source/Tools/CommonTools/HarnessHelpers.h`, no includers
    - the 32-bit robust-futex walker in ThreadManager
    - most of `ELFContainer` (only `GetELFType` has callers)
11. **`096a8d383` swept in an unrelated staged deletion.** The CodeSizeValidation files went in
    with the thunks commit, so that commit does not configure.
12. **SMC semantic patching (Idea 4) was fed by the x86 decoder.** Its site tables are now
    always empty; the feature is inert, not broken.
13. **`/proc/cpuinfo` and AT_HWCAP were both derived from emulated x86 CPUID leaves.** With
    CPUID gone there is no feature source at all. The TODO refers to **DESIGN.md §4.8, which
    does not exist yet**: §4 ends at §4.7.
14. **The RFCE pass's `InvalidateFlags` used x86 `RFLAG_*_RAW_LOC` bit positions.** It now uses
    PSTATE NZCV bits, a silent contract with the future frontend.
15. **SIGILL at the default action dumps core** ("Illegal instruction (core dumped)"). That
    matches arm64 Linux, but it's noisy for M1 bring-up loops.

## Granule infrastructure: where it becomes a passthrough

**Nothing in the 64K granule and SMC-granule machinery was deleted.** Every piece of it is
keyed off one predicate:

- `FEXCore::HostPage::MatchesGuest()` (`FEXCore/include/FEXCore/Utils/TypeDefines.h:109`)
- which compares the host page size to the compile-time constant
  `FEX_GUEST_PAGE_SIZE = 4096` (`TypeDefines.h:16`)

With a guest whose granule equals the host page, these become identity/no-op paths:

| Site | What turns into a passthrough |
|---|---|
| `Source/Tools/LinuxEmulation/LinuxSyscalls/GranuleTable.h:61` | granule table `Enabled()`: sub-page mmap/mprotect/munmap tracking |
| `Source/Tools/LinuxEmulation/LinuxSyscalls/SMCHostGranule.h:88` | whole-host-page SMC arming and re-arm after shared-granule writes |
| `Source/Tools/LinuxEmulation/LinuxSyscalls/SyscallsSMCTracking.cpp:29` | `Cover()` widening of protection ranges |
| `Source/Common/HostPageMapping.h:47` | anon+pread fallback for non-congruent file offsets |
| `Source/Tools/FEXInterpreter/ELFCodeLoader.h:205`, `:214`, `:301` | host-granular PT_LOAD length, tail zeroing, BSS mapping |
| `Source/Tools/LinuxEmulation/LinuxSyscalls/LinuxAllocator.cpp` | host-granular allocator stepping |
| `Source/Common/HostPageGate.h:109` | the startup gate and banner (`POWERARM_HOSTPAGEMODE`) |

**What's needed.** `MatchesGuest()` has to answer per process instead of against a constant:
guest granule = host page when every PT_LOAD `p_align` ≥ host page. That's the TODO(loader) at
`ELFCodeLoader.h:766` and `HostPageGate.h:109`.

**What stays 4K-denominated either way:**

- the ASLR slide unit (`ELFCodeLoader.h`, `FEX_GUEST_PAGE_SHIFT`)
- the BRK base alignment (`ELFCodeLoader.h:341`)

These are harmless when rounded up to the host page, as they already are.

## Top design issues surfaced

1. **Vector register pinning (backend).** Only V0-V15 can be static. Pinning V16-V31 needs
   VSX-form lowerings for the dynamic pool, or a two-class vector allocator.
2. **Syscall flag and struct translation (Linux layer).** Every shared handler assumed x86-64
   guest values. arm64 → powerpc64 needs a systematic re-derivation: `O_*`, `MAP_*`, `MCL_*`,
   termios, `_IOC`, `struct stat`, sockopt. The x86-specific handlers (mmap, clone, sigaction,
   stat) are simply missing.
3. **Signal delivery.** The frame has no `fpsimd_context`/`esr_context` yet. `rt_sigreturn`
   needs a real arm64 vDSO, and the guest vDSO is still the thunked x86 one. Faults must be
   described through an ESR syndrome, not DSISR.
4. **Thunk and callback ABI.** Callback return is an x86 stack convention inside the JIT and
   dispatcher. The thunk generator models an x86_64 guest data layout.
5. **Page size and VA contract.** AT_PAGESZ is now the host's 65536. Binaries with 4K `p_align`
   need the granule-emulation fallback chosen per process. The loader still clamps to the
   x86-64 47-bit VA, where AArch64 expects 48.
