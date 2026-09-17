# POWERarm design notes: scope, reuse and open work

Written 2026-09-16. This is not a plan of record yet. It records what has to be designed and
what can be reused, so the first decisions are made on evidence.

This follows the handbook's tagging convention:

| Tag | Meaning |
|---|---|
| `[CODE]` | Read from source, with a path. |
| `[DOC]` | From a handbook document or an upstream README. |
| `[WEB]` | From an upstream project page, checked 2026-09-16. Recheck before relying on it. |
| `[SPEC]` | Inference. Each one names the probe that would confirm or refute it. |

---

## 0. Summary

1. **Fork fastppcx86 and add an A64 frontend. Don't start from dynarmic, and don't start from
   scratch.** fastppcx86 is MIT (FEX-derived). It already has everything that isn't the guest
   decoder: the PPC64LE JIT backend (~26k lines under `FEXCore/Source/Interface/Core/JIT/PPC64LE`
   `[CODE]`), the syscall translation layer, a clang-based thunk generator, rootfs overlay,
   binfmt registration, SMC tracking and 4K-on-64K page emulation. Those pieces are most of the
   work.
2. **FEX's IR already has AArch64-shaped flag ops,** because it was designed for an ARM64 host:
   `AddNZCV`, `SubNZCV`, `AdcNZCV`, `SbbNZCV`, `CondAddNZCV`, `CondSubNZCV`, `RmifNZCV`,
   `StoreNZCV` (`FEXCore/Source/Interface/IR/IR.json` `[CODE]`). `CondSubNZCV` is `CCMP` and
   `RmifNZCV` is `RMIF`. The ppc64le backend already lowers all of them, so an A64 frontend
   emits IR the backend already handles.
3. **An AArch64 guest is easier than x86 in three ways that matter:**
   - **Memory ordering.** ARM is weakly ordered like POWER, so the TSO barrier tax is gone. The
     handbook measured that tax at ~5.6× per emulated memory operation
     (`qemu-to-fastppcx86.md` §4.3 `[DOC]`).
   - **Byte order and char signedness.** Both are little-endian, and plain `char` is unsigned on
     both.
   - **Instruction decode.** A64 is fixed-width, so there's no prefix or length decode.
4. **The hard parts that are new:**
   - exclusive monitors (`LDXR`/`STXR`) and LSE atomics
   - NEON→VMX/VSX gaps (`TBL`/`TBX`, some saturating and widening ops). SHA and LSE already
     have IR ops and ppc64le lowerings (§4.3, §4.4)
   - FPCR/FPSR and NaN semantics
   - an AAPCS64 guest side for the thunk generator, and ioctl/termios/`stat` translation
     toward powerpc's own definitions rather than x86's
5. **Thunks mean library thunks** (owner decision, 2026-09-16). Exec redirection to native
   cross compilers is out of scope. How much a thunk helps depends on the program.
6. **Repository shape: a history-sharing fork of fastppcx86, renamed at the product level**
   (§10). Keep internal paths so backend fixes can be cherry-picked both ways. Don't add a
   second guest inside the fastppcx86 tree.

---

## 1. Goals and non-goals

**Goals**

- Run AArch64 Linux ELF programs, static and dynamic, on ppc64le hosts with a 4K or 64K page
  size. Triggered through `binfmt_misc` (magic `EM_AARCH64` = `0xb7` at `e_machine`).
- POWER8 floor, with POWER9 fast paths gated at run time.
- Guest libraries come from an AArch64 rootfs overlay. Native ppc64le libraries can stand in
  through thunks.
- MIT-licensed output.

**Non-goals, at least initially**

- SVE/SVE2/SME. Don't advertise them in `HWCAP`/`HWCAP2`. Code that detects features at run
  time will fall back to NEON.
- AArch32 (A32/T32) guests. Most arm64 distros have dropped compat userspace.
- MTE, pointer authentication enforcement and BTI enforcement. PAC and BTI instructions sit in
  the HINT space and run as NOPs.
- System-mode emulation. That's QEMU's job.

---

## 2. Reuse analysis

### 2.1 Candidates

| Project | License | Guest → host | What it brings | Fit |
|---|---|---|---|---|
| **fastppcx86** (your FEX fork) | MIT | x86/x86-64 → **ppc64le** | PPC64LE backend, dispatcher, SMC, 64K pages, syscalls, thunks, rootfs, binfmt | **Base.** Everything except the frontend `[CODE]` |
| **FEX-Emu** upstream | MIT | x86 → arm64 | Same architecture as fastppcx86, upstream fixes | Source of cherry-picks only |
| **dynarmic** (lioncash / yuzu-mirror) | **0BSD** | A32/A64 → x64, arm64 | A mature A64 decoder, IR translators and FP/NEON semantics that are tested against unicorn | **Decoder and semantics reference. 0BSD code can be copied into MIT freely** `[WEB]` |
| dynarmic, **Eden fork** | 0BSD base; **Eden-added files may carry GPL-3.0 headers, unverified** | A64 → **ppc64** (`backend/ppc64`, `externals/powah`) | Existing A64→POWER lowering | Check each file's SPDX header before reading it for design. Treat as GPL-3 until proven otherwise `[WEB]` |
| **box64** | MIT | x86-64 → arm64/rv64/la64/**ppc64le** (added in v0.4.2) | Wrapped-library (thunk) model, a ppc64le emitter | x86 guest only. A second reference for ppc64le emitter idioms `[WEB]` |
| **arm64emu-user** (sylirre) | Apache-2.0 | A64 → arm64/x86-64/i686/arm32 | Clean-room A64 user emulator in C11, differential test suite | Apache-2.0 is compatible with MIT distribution (keep the NOTICE file). Useful for syscall and test ideas; no POWER backend `[WEB]` |
| **Maarch64** | MIT/Apache-2.0 | A64 → x86-64 (Cranelift) | Rust, thunks | Young (89 commits, 0 stars). Cranelift has no ppc64 backend `[WEB]` |
| **QEMU user** (`qemu-aarch64`) | **GPL-2.0** | A64 → ppc64 (TCG) | Works today | **Oracle only.** Never copy code from it. Also the performance baseline to beat |

### 2.2 Recommendation

Fork **fastppcx86**, keeping MIT and the upstream `LICENSE`, and write a new
`FEXCore/Source/Interface/Core/A64Frontend/`. Use dynarmic's A64 decoder table
(`frontend/A64/decoder/a64.inc`) and its translator semantics as the reference. Copy code only
from 0BSD upstream dynarmic, never from Eden-modified files. Record every copied file in a
`THIRD_PARTY.md`.

Why not dynarmic plus a new backend?

- dynarmic is only a CPU library. It has no ELF loader, syscall layer, signals, thunks,
  rootfs or SMC handling, and fastppcx86 already has all of those for ppc64le.
- A new backend would repeat work already done and measured, such as branch reach, TOC
  discipline, count-cache behaviour and ISA 3.0 gating.

Why not from scratch? Same reason, but more so. The main cost of a from-scratch start is
rediscovering what the handbook already records.

**The cost of the fork:** FEX's frontend, IR passes and syscall layer assume an x86 guest in
many places. They name x86 register slots, use x87/AVX state and hold the x86-64 syscall table.
The first design task (§3) is to find each of those assumptions and pull it behind a guest
interface. Measure how big that job is before committing (§8, M0).

---

## 3. Forking fastppcx86: what gets split

| Area | x86 assumption today | What POWERarm needs |
|---|---|---|
| `Core/CPUState` | 16 GPRs, XMM/YMM, x87, segment state, `rflags` bits | 31 X plus SP, PC, NZCV, FPCR/FPSR, 32 128-bit V registers, `TPIDR_EL0`, exclusive-monitor state |
| `OpcodeDispatcher/*` + `X86Tables` | x86 decode | A64 decoder and translators (new) |
| IR passes | Flag elimination tuned for x86 `PF`/`AF`, deferred flags | Keep the NZCV-only paths and drop PF/AF work. Dead-flag elimination is simpler |
| `LinuxSyscalls/x64`, `x32` | x86-64 syscall numbers and structs | `LinuxSyscalls/Arm64` using asm-generic numbers (§5) |
| Signal delivery | x86 `ucontext`/`sigcontext`, red zone | arm64 `sigcontext` + `fpsimd_context` in `__reserved[]`, no red zone, `x30` and the `rt_sigreturn` trampoline in the guest vDSO |
| CPUID emulation | `CPUID` instruction | `MRS` traps on ID registers (Linux exposes a sanitized `ID_AA64*` view to EL0), `/proc/cpuinfo` features, `HWCAP`/`HWCAP2`, `CTR_EL0`, `DCZID_EL0` |
| Thunk generator | x86-64 SysV guest ABI | AAPCS64 guest ABI (§6) |
| binfmt/rootfs | x86 magic, x86 rootfs images | `EM_AARCH64` magic, arm64 rootfs |
| TSO emulation | Needed | **Delete it from the guest path** |

Recover the removed ARM64 *host* code (`git archive 99284a16c`, see `fexcore-embeddability.md`
`[DOC]`) as a reference only. It shows how FEX lowered NZCV ops onto real NZCV hardware, which
is the exact inverse of what the ppc64le backend does.

---

## 4. The CPU core

### 4.1 Register mapping

**GPRs.** The guest needs 31 X registers plus SP. POWER has 32 GPRs, but these are spoken for:

- r0 (reads as literal zero in `RA` position, handbook §"RA=0")
- r1 (host stack)
- r2 (TOC)
- r12 (entry and indirect-call register)
- r13 (host thread pointer)

That leaves about 25 after the JIT reserves its own statics, so **not every guest register can
be pinned.**

- Pin the hot ones statically: X0–X8 for arguments, X19–X30 for callee-saved registers and
  LR, and SP.
- Spill the rest (X9–X18) to the context block.
- Choose the pinned set with profile data from real arm64 compiler output, not by assumption.
  `[SPEC]` Confirm with a register-use census over the rootfs's `.text`.

**Vector registers.** POWER has 64 128-bit VSX registers. `vs32`–`vs63` are the VMX registers
`v0`–`v31`, which is exactly 32, and A64 has exactly 32 V registers.

- Pin V0–V31 to `vs32`–`vs63`, where every VMX and VSX integer op can reach them.
- Keep `vs0`–`vs31` (the FPR half) as scratch.
- Watch the ELFv2 callee-saved set (`v20`–`v31`, `f14`–`f31`): thunk transitions have to save
  or restore it.

`[SPEC]` Check this against the backend's current VSX register allocator before committing.

### 4.2 Flags

A64 has only NZCV, with no parity or auxiliary carry, so flag elimination is simpler than on x86.

**Carry on subtraction.** Both ARM `C` and POWER `CA` use carry-out of `a + ~b + 1`, so carry
means *no borrow* on both. x86 inverts this. The backend's `SubNZCV`/`SbbNZCV` lowering was
written for an ARM64-shaped IR, so it may already use the ARM convention. `[SPEC]` Write a probe
with the handbook's parity-before-timing structure: exhaustive 8-bit plus random 64-bit
`SUBS`/`SBCS` against the Pi 5.

**Traps to design around** (see the handbook's condition-code section):

- Guest NZCV already lives in **CR0** in the backend: the dispatcher saves it with `mfcr` and
  restores it with `mtcr` (`JIT.cpp:6076` `[CODE]`).
- Sticky `XER.SO`: never use `addo.` + `bso` for V.
- Unordered FP compares.
- 32-bit ops (`W` registers) must zero-extend. This is the handbook's zero-extension invariant.

### 4.3 Exclusive monitors and LSE

`LDXR`/`LDXP` → `STXR`/`STXP` is load-link/store-conditional. POWER has native
`lbarx`/`lharx`/`lwarx`/`ldarx` and matching `stcx.` forms, all ISA 2.07. Mapping one to the
other is attractive but has three problems:

1. **A guest-only access window.** The instructions between `LDXR` and `STXR` are guest code,
   and the JIT may insert dispatcher exits, block links or helper calls between them. Any
   `larx`/`stcx.` the emulator runs in that gap kills the reservation
   (`jsc-ppc64le-atomics-cas.md` §3 `[DOC]`). Two options:
   - translate the whole LL…SC span as one host region with no exits
   - fall back to software exclusive state (address + value + generation), the way QEMU does
     with compare-and-swap. That version has the ABA problem.
2. **Alignment.** `larx`/`stcx.` require natural alignment and raise SIGBUS otherwise
   (`memory-ordering-and-atomics.md` `[MEASURED]`). ARM permits `LDXR` on any address, so an
   unaligned guest access needs the software path.
3. **`LDXP`/`STXP` (pairs)** have no POWER equivalent. Use the software path.

**LSE already has IR ops:** `CAS`, `CASPair`, `AtomicFetchAdd/Sub/And/CLR/Or/Xor/Neg` and
`AtomicSwap`, with ppc64le lowerings in `JIT/PPC64LE/AtomicOps.cpp` `[CODE]`. `AtomicFetchCLR`
is `LDCLR` and `CASPair` is `CASP`, because FEX mirrored the arm64 host's LSE. The lowering is
a short `larx`/`stcx.` retry loop with barriers
(`sync` before, `lwsync` after), like the JSC CAS stub. Advertise `HWCAP_ATOMICS`: glibc,
libstdc++ and outline-atomics code then avoid the LL/SC path entirely.

**Design decision:** use the hardware LL/SC fast path only when the span is one block, aligned
and has no exits. Use a software monitor otherwise. Test for livelock under contention.

### 4.4 Floating point and NEON

- **FPCR.** Rounding mode maps to `FPSCR.RN`. `FZ` has no direct POWER equivalent. Default-NaN
  mode (`DN`) needs explicit canonicalization (handbook §"Canonicalizing a NaN").
- **FPSR.** Cumulative flags map onto `FPSCR` sticky bits. `QC` (saturation) has no host bit
  and must be computed wherever it's observable.
- **NaN propagation.** ARM returns the first operand's NaN, quieted. POWER's `xsmaxdp`,
  `xvminsp` and similar pick the non-NaN operand (handbook `vsx_builtin_map` probe `[DOC]`). So
  `FMIN`/`FMAX`/`FMINNM`/`FMAXNM` each need their own lowering, and parity probes against the
  Pi 5.
- **Float→int conversion.** `FCVTZS` saturates, and **NaN converts to 0**. POWER `fctidz`
  returns `0x8000…` for NaN, so the NaN case needs a fix-up.
- **Direct NEON→VMX/VSX mappings.** add/sub/and/orr/xor/cmp, `CNT`→`vpopcntb`, `CLZ`→`vclz*`,
  `PMULL`→`vpmsumd`, and `ABS`/`UQADD`/`SQADD` for 8/16/32-bit (VMX has saturating forms for
  those widths).
- **Gaps that need multi-instruction or helper lowering:**
  - `TBL`/`TBX`: no `pshufb`-like op. `vpermxor` isn't a lookup. Use a scalar or 16-entry
    `vperm` construction.
  - 64-bit saturating ops.
  - `SQDMULH` and family.
  - Widening and narrowing (`*MULL`, `*SHRN`, `XTN`).
  - `FRECPE`/`FRSQRTE`, which have fixed-precision ARM tables.
  - `SHA1*`/`SHA256*`: no POWER hardware. **The IR already has ARM-shaped ops**
    (`VSha1C/M/P/H/SU1`, `VSha256H/H2/U0/U1`) that the ppc64le backend already lowers
    (`JIT/PPC64LE/VectorOps.cpp` `[CODE]`). FEX lowered x86 SHA onto the ARM instruction set,
    so the A64 frontend can emit these ops directly.
  - `SHA3`/`SHA512` extensions: don't advertise.
- **AES.** The IR's `VAESEnc`/`VAESDec` have **x86** `AESENC` semantics (state, key, zero
  register) `[CODE]`. A64 needs either new `VAESE`/`VAESMC` ops or compositions of the
  existing ones. Map `AESE`/`AESD`/`AESMC`/`AESIMC` onto `vcipher`/`vcipherlast`/`vncipher*`. The
  step order differs: ARM `AESE` is AddRoundKey, SubBytes, ShiftRows, and POWER `vcipher` is
  ShiftRows, SubBytes, MixColumns, AddRoundKey. Rearrange with an explicit XOR. `[SPEC]` Probe
  against NIST vectors on both machines.
- **`CRC32`/`CRC32C`.** Advertise the hardware capability and lower to a `vpmsumd` fold or a
  helper.

### 4.5 Instructions that touch "system" state from EL0

| Instruction or register | Handling |
|---|---|
| `TPIDR_EL0` (read/write), `TPIDRRO_EL0` | Context slot. TLS is the guest's, not r13 |
| `MRS` of `CTR_EL0` | Advertise the cache line size used by `__clear_cache` and `dc zva` loops |
| `MRS` of `DCZID_EL0` | Advertise a ZVA block size (64 bytes is typical) and emulate `DC ZVA` as a memset |
| `MRS` of `ID_AA64*` | Emulate Linux's sanitized cpufeature view |
| `MRS`/`MSR` `FPCR`/`FPSR` | §4.4 |
| `CNTVCT_EL0`, `CNTFRQ_EL0` | Derive from the host timebase. glibc and JITs read these directly |
| `SVC #0` | Syscall (§5) |
| `BRK`, `UDF`, `HLT` | SIGTRAP/SIGILL with the correct `si_code`. Debuggers and `abort()` paths depend on it |
| HINT space (`PACIASP`, `BTI`, `YIELD`, …) | NOP |

### 4.6 Dispatch and code layout

- Use `bl`/`blr` for guest `BL`/`RET` so the POWER link stack predicts guest returns. This
  needs a return-address-stack mismatch check, as in FEX's arm64 design.
- Guest `BLR`/`BR` are indirect branches on the count cache. A miss costs about 14 cycles, and
  POWER9 can have count-cache flush mitigations active (handbook §Dispatch `[DOC]`). Measure on
  arkamedes with the mitigation state recorded.
- Branch reach and TOC discipline are already solved in the fastppcx86 backend, so inherit them
  unchanged.

### 4.7 Self-modifying code

AArch64 JITs (V8, LuaJIT arm64, OpenJDK, .NET) change code by writing to memory and then
running the `__clear_cache` loop. On arm64 that loop is userspace-only (`CTR_EL0` + `IC`/`DC`),
with no syscall. There's no guaranteed notification, so use fastppcx86's mprotect-based SMC
tracking. Dual-mapping JITs (one writable view, one executable view) need to be tracked by
inode and offset. `[SPEC]` Check whether mtrack already handles `memfd` aliases.

---

### 4.8 Presented CPU profile

The aim is for 99% of aarch64 Linux software to run by *presenting* a CPU that software already
knows, not by emulating every corner. Distros build for the ARMv8.0-A baseline, and anything
above that is detected at run time through `HWCAP`/`HWCAP2`, `/proc/cpuinfo` or `MRS ID_AA64*`.
Everything declared must actually work. A feature is added to the profile only when its
instructions pass Pi parity.

**Target: a Cortex-A72/A76-class core.** The Pi 5's A76 is both the model and the reference
machine.

| Tier | HWCAP features | Why |
|---|---|---|
| 1 (M2–M4) | `fp`, `asimd`, `cpuid`, `atomics` (LSE), `crc32`, `aes`, `pmull`, `sha1`, `sha2` | Covers distro baselines. LSE keeps glibc and libstdc++ off the LL/SC path |
| 2 (after M4) | `asimdhp`, `fphp`, `asimdrdm`, `jscvt`, `fcma`, `lrcpc`, `dcpop`, `asimddp` (dotprod) | Fast paths in JS engines, codecs and ML runtimes. Dotprod is added once it's implemented |
| Never, initially | SVE/SVE2/SME, MTE, PAC and BTI enforcement, `sha3`/`sha512`, `i8mm`, `bf16` | Software falls back. PAC and BTI instructions are NOPs |

- **SHA1/SHA256** map one-to-one onto existing IR ops. `VSha1*` and `VSha256H/H2/U0/U1` are
  named after the ARM instructions and are already lowered on ppc64le. fastppcx86 lowered x86
  SHA-NI onto them.
- **AES:** the existing `VAESEnc/Dec` ops have x86 `AESENC` semantics. `AESE`/`AESD`/`AESMC`/
  `AESIMC` need new ops or compositions of the existing ones.
- **32-bit code:** none. There's no AArch32 and no armhf multilib; ARMv9 cores dropped AArch32
  at EL0 as well. Guest pointers are host pointers, so thunks pass them through untouched.

### 4.9 Page size and address space

**Page size.** The guest is told the **host** page size (`AT_PAGESZ` = 4096 or 65536). arm64
Linux software already runs on 4K, 16K and 64K kernels, and GNU ld's aarch64 default
max-page-size is 64K. The M0 smoke binary (Debian static `hello`) has `p_align` 0x10000 on both
PT_LOADs. The 4K granule emulation inherited from fastppcx86 is kept only as a **per-process
fallback** for binaries whose PT_LOAD `p_align` is smaller than the host page. **Every milestone
from M1 on must pass on both a 4K and a 64K host kernel**; the owner runs both.

**Address space.** Research and measurements are in
[`research/va-size/VA-SIZE-RESEARCH.md`](research/va-size/VA-SIZE-RESEARCH.md) `[MEASURED]`:

| Kernel | User VA ceiling |
|---|---|
| ppc64 64K (radix or hash) | 2^52. Default window 2^47; hints above it are honoured |
| ppc64 **4K** (radix or hash) | **2^46**. Nothing above it can be mapped |
| arm64 Pi 5 (16K, VA47) | 2^47. Identical to ppc64 64K with no hints, apart from PIE base |
| arm64 VA48 / VA52 | 2^48 / 2^52 `[SPEC]` |

- **Default:** a guest limit equal to the host's natural window. That's 47 bits on 64K and 46
  on 4K, enforced in the mmap-family syscall layer: hints above the limit are dropped, and
  `MAP_FIXED` above it gets `ENOMEM`. POWERarm's own allocations live at 2^48 and above on 64K.
- **Per-app `GuestVABits`:** 48 on 64K for MSan and TSan (needs a guest address allocator).
  42 or 39 on 4K for TSan. 52 only on request.
- **Software that cares:** released TSan and `go -race` abort below 47 bits, and MSan needs
  48-bit. V8, HotSpot/ZGC, the Go runtime, .NET, LuaJIT GC64 and SpiderMonkey probe or already
  assume at most 47 bits.
- **Inherited bug:** FEX's `DetermineVASize()` probe list lacks 46, so on 4K it detects 42 and
  guest stack and interpreter hints land inside FEX's own [1 TiB, 32 TiB) allocation window.
  This also affects fastppcx86 on 4K kernels. Fix it in both trees.
- **Top-byte-ignore:** arm64 ignores pointer tag bytes and POWER doesn't. Mask only for
  processes that opt in with `prctl(PR_SET_TAGGED_ADDR_CTRL)`. That's rare on Linux desktops
  and servers.

## 5. Linux user ABI: arm64 guest on a powerpc64 host

These differences are all in the host direction. fastppcx86 already translates x86-64 onto
powerpc64 for most of them, so much of the code is reusable. **The source side still changes:**
arm64 uses asm-generic, and x86-64 has its own conventions.

| Area | Difference | Notes |
|---|---|---|
| Syscall numbers | arm64 uses the asm-generic table (`openat` only, no `open`/`stat`/`fork`) | A smaller table than x86-64's |
| `struct stat` | asm-generic layout vs powerpc64 layout | `[SPEC]` Diff `pahole` output from both kernels' headers |
| ioctl encoding | powerpc `_IOC` uses 3 direction bits and 13 size bits; asm-generic uses 2 and 14 | **Every ioctl number needs re-encoding**, including DRM, input and sound. Already a solved class in fastppcx86 `[SPEC]` Confirm |
| termios | powerpc has its own `struct termios`, `c_cc` layout and `TCGETS` values | Translate fully |
| `O_*`, `MAP_*`, `SO_*`, `EDEADLOCK` | powerpc-specific values in several places | Generate the tables from both architectures' uapi headers, not by hand |
| Signals | Same numbers. Different `sigcontext`, `SA_RESTORER` handling and signal-stack sizes (`MINSIGSTKSZ`/`AT_MINSIGSTKSZ`) | |
| Auxv | Guest `AT_PAGESZ` (4096, or 16K/64K for a guest built that way), `AT_HWCAP`/`AT_HWCAP2`, `AT_PLATFORM="aarch64"`, `AT_MINSIGSTKSZ` | |
| vDSO | Provide a guest vDSO with `__kernel_clock_gettime`, `__kernel_gettimeofday`, `__kernel_clock_getres` and `__kernel_rt_sigreturn` | Thunk the clock functions to the host vDSO |
| Page size | The guest may be built for 4K alignment (Debian, Ubuntu) or ≥16K (RHEL 9, Asahi) | Reuse fastppcx86's 4K-on-64K granule model unchanged `[DOC]` |
| `/proc/self/*` | `exe`, `maps`, `auxv`, `cpuinfo` need guest views | |

---

## 6. Thunks, rootfs and binfmt

### 6.1 Library thunks

The data layout is nearly identical, since both are LP64 little-endian with unsigned `char`, so
the generator's layout-repacking work mostly disappears. What remains:

- **Calling convention.** The guest is AAPCS64: X0–X7, V0–V7, HFA/HVA structs passed in V
  registers, struct return through X8. The host is ELFv2: r3–r10, f1–f13, homogeneous aggregates
  in FPRs or VRs, an r12/TOC entry protocol and a parameter save area. The generator's
  `analysis.cpp`/`data_layout.cpp` (`ThunkLibs/Generator` `[CODE]`) need an AAPCS64 guest model.
- **`long double`: the two sides use different formats, and conversion loses data.**
  `[MEASURED]` on arkamedes, 2026-09-16:
  - Arch POWER's GCC 16.1.1 defaults to `-mabi=ibmlongdouble`.
  - Clang 22.1.8 defines `__LONG_DOUBLE_IBM128__`.
  - `LDBL_MANT_DIG` is 106.
  - glibc 2.43 exports both ABIs: `printf@@GLIBC_2.17` plus `__printfieee128@@GLIBC_2.32`, and
    105 `__*ieee128` symbols in total.

  So a host library built by the distro takes IBM double-double. The aarch64 guest passes
  binary128 (113 mantissa bits and a 15-bit exponent). The conversion loses data in both
  directions. The generator must **refuse to auto-thunk any signature or struct that contains
  `long double`**; those functions need a hand-written thunk or stay emulated. None of the
  first-target libraries below use `long double` in their public API. `[SPEC]` Confirm with a
  generator diagnostic pass over their headers. glibc's `__*ieee128` entry points would matter
  only if libc were thunked, and it isn't.
- **`va_list`.** A struct on aarch64, `char *` on ppc64le. Variadic functions like `printf`
  need hand-written thunks.
- **Callbacks.** A host library calling a guest function pointer needs a host→guest
  trampoline. FEX already has this machinery.
- **Unwinding and exceptions across the boundary.** Unsupported. Document it and abort
  cleanly.
- **Order (owner decision 2026-09-16):**
  1. **GL/Vulkan first**, with `libEGL`, `libdrm`, `libwayland-client`, `libX11`, `libasound`
     and `libSDL2` alongside. The host halves already exist in `ThunkLibs/` `[CODE]`, so only
     the guest halves are new. The Pi 5 builds them natively, with no cross toolchain; after
     the self-hosting milestone (§8) POWERarm builds them itself.
  2. **Then `libcrypto`/`libssl`.** The host OpenSSL runs POWER assembly paths (`vcipher`,
     `vpmsumd`), so TLS runs at native speed. OpenSSL 3's opaque pointers make most symbols
     auto-thunkable. Callbacks (BIO methods, verify, password, ex_data, allocators), `va_list`
     functions and provider loading need annotation or hand-written thunks. Guest and host
     `.so.3` versions must be compatible. Statically linked crypto (Go, ring/rustls,
     BoringSSL in Chromium/Electron/Node) doesn't benefit, so the JIT's SHA/AES/PMULL paths
     are still required.
- **GPU test ladder:**
  1. `vkcube` and `eglgears`/`glxgears`
  2. Sascha Willems' Vulkan samples, built for aarch64 (one feature per sample)
  3. Native arm64 open-source games from Arch Linux ARM: SuperTuxKart first (GL and Vulkan
     renderers, easy to benchmark), then Luanti, 0 A.D. and Xonotic. Confirm package
     availability when the rootfs is built
  4. An aarch64 Godot export, the first binary built by someone else

  Every step runs on both the 4K and 64K kernels.

### 6.2 Rootfs and binfmt

- Build an arm64 rootfs image (erofs or squashfs) from Arch Linux ARM (§6.4).
  Overlay it the way FEXRootFS does. The existing `fexrootfs*` directories show the layout.
- Register through binfmt with the `POCF` flags as in `register-fex-binfmt.sh`. Keep its
  `F`-flag warning: a rebuild leaves the old inode registered, so re-register after every
  rebuild.
- Magic and mask: `\x7fELF\x02\x01\x01` + `e_type` in {2, 3} + `e_machine` = `\xb7\x00`, using
  the same mask shape as the x86-64 entry.
- **No conflict:** this rig registers FEX only for `x86_64` and `x86` (`binfmt.d/` `[CODE]`).
  Check for a stale `qemu-aarch64` registration from `qemu-user-static` before registering.

### 6.2a Rootfs layout and lookup (decided 2026-09-16)

POWERarm follows XDG and FHS conventions, so the layout used in development is the same one
packages install to. The inherited FEX lookup code already resolves these paths
(`Source/Common/Config.cpp` `GetDataDirectory`/`GetConfigDirectory`) `[CODE]`.

| Kind | Per user (default) | System-wide (packaged) |
|---|---|---|
| Rootfs base images | `$XDG_DATA_HOME/powerarm/RootFS/<name>` (= `~/.local/share/powerarm/RootFS/`) | `/usr/share/powerarm/RootFS/<name>` |
| Per-user writable rootfs layer | `~/.local/share/powerarm/RootFS/<name>-overlay/` | none (always per user) |
| Guest thunk stubs | `~/.local/share/powerarm/GuestThunks/` | `/usr/share/powerarm/GuestThunks/` |
| Config | `~/.config/powerarm/Config.json` | `/usr/share/powerarm/Config.json` |
| Code cache, thunk preflight cache | `~/.cache/powerarm/` | n/a |

- **Lookup order:** explicit config or env → per user → system.
- **Two layers.** The base image is a read-only erofs/squashfs built from Arch Linux ARM
  packages, shipped as e.g. `powerarm-rootfs-alarm` or fetched per user. On top sits a
  writable per-user layer where `pacman -S` inside the guest installs packages. Upgrading the
  base keeps user installs, and nothing needs root, because the overlay is path redirection
  inside the emulator, not a kernel mount.
- **Guest thunk stubs live outside the rootfs,** versioned with the emulator they must match,
  so a rootfs update can't break them.
- **Minimal base before the Arch Linux ARM image exists:** if Arch's
  `aarch64-linux-gnu-glibc` cross package is installed, `/usr/aarch64-linux-gnu` provides a
  real aarch64 `ld.so` + glibc and can serve as a zero-download base for the first dynamically
  linked programs.
- **One image serves 4K and 64K hosts,** because aarch64 packages are 64K-aligned.
- **During development,** the image builder writes into `~/.local/share/powerarm/RootFS/`,
  the same place the fetcher does, so nothing moves when packages arrive.

### 6.2b Automatic thunk selection with fallback (proposed)

**Today, inherited from FEX `[CODE]`:** thunks are opt-in per library through `ThunksDB.json`
+ `ThunkConfig`. When one is enabled, the guest's `libvulkan.so.1` (and friends) is path-overlaid
with the stub. If the host library then fails to load, `Thunks.cpp:334-357` calls
`ERROR_AND_DIE`. There's no fallback, no detection and no user-facing report.

**Proposed behaviour:**
1. **Preflight before the overlay decision.** Redirecting a guest library to a thunk is decided
   at the moment the guest opens that path. A library is thunked only if all of these hold:
   - the guest stub exists
   - the host thunk half and its native dependency (e.g. host `libvulkan.so.1`) exist and
     `dlopen` cleanly
   - required symbol versions are present
   - the ABI version recorded when the stub was built matches the host library

   Otherwise the redirect is skipped, and the guest's `ld.so` loads the real aarch64 library
   from the rootfs, which runs fully emulated.
2. **Preflight runs in `POWERarmServer`, not in the guest process.** A trial `dlopen` of a large
   host library (a GL or Vulkan driver) must not pollute or slow the emulated process. Results
   are cached in `~/.cache/powerarm/thunk-preflight.json`, keyed by host library path, mtime and
   build ID, so later launches only `stat`.
3. **Decide once, never swap mid-run.** After the guest has bound symbols to a stub, there's
   nothing to fall back to. A host-side failure after preflight passed stays a hard error with a
   message naming the library and pointing at `POWERARM_THUNK_<LIB>=0`.
4. **Per-library override:** `POWERARM_THUNK_<LIB>=auto|0|1` and matching config keys.
   `auto` is the default. `1` forces the thunk even if preflight fails, for debugging.
5. **The banner.** Printed once per launch tree (an env marker stops exec'd children from
   repeating it), only when stderr is a TTY and `POWERARM_QUIET` is unset, and **before guest
   code starts**, so it can't interleave with the program's own terminal output. For example:
   ```
   POWERarm 0.1 · aarch64 → ppc64le (POWER9, 64K pages)
     thunked : vulkan (host radv 26.2), EGL, GL, drm, wayland-client
     emulated: asound (no host libasound), SDL2 (disabled in config)
     rootfs  : ArchLinuxARM-2026.09 (+ user overlay)
   ```
   Libraries opened later with `dlopen` (Vulkan ICDs, GL via libglvnd) are decided when opened.
   If a TTY is present they get a one-line follow-up; they're always logged.
5a. **Load lines.** Once a library is decided as thunked (from the cache or a fresh
    preflight), the moment its host half actually loads prints one live line on the TTY:
    ```
    Loading thunks -> vulkan (host radv 26.2) ... done 212 ms
    Loading thunks -> GL ... done 48 ms
    ```
    This is real work: the host `dlopen` of a GPU driver can take hundreds of milliseconds.
    The line shows `...` while the host `dlopen` runs and is completed in place (`\r`) with
    the timing. The same rules apply: TTY only, `POWERARM_QUIET` silences it, and it's always
    logged. Libraries that fell back are named once in the banner's `emulated:` row and don't
    get a load line.
6. **Progress bars only for work that actually takes time:**
   - fetching or unpacking a rootfs image
   - mounting and indexing a new base image
   - populating the code cache on a first launch
     (`POWERarmOfflineCompiler`, or the background cache fill)

   Each gets a real progress bar driven by bytes or blocks done. A preflight check takes
   milliseconds, and a progress bar for it would be fake. Without a TTY (desktop launches),
   all of this goes to the log only.

Belongs to M5 (thunks). The server-side preflight cache also serves the rootfs work in M3.

**Discovery, not a database (M5).** `ThunksDB.json` is replaced by discovery. Each guest stub
carries an ELF note with the sonames it replaces, the ABI and symbol versions it was generated
against, and its host-half name. The server scans `GuestThunks/` and the host thunk directory,
reads the notes and matches them against the actual host libraries. Adding a thunk means
installing its two files: no JSON edit and no POWERarm rebuild. Owner priority: a slower first
launch (up to about 0.5 s of lookup, cached afterwards) is acceptable if every direct match
gets thunked automatically.

**Deferred: auto-generated thunks for libraries without a curated thunk.** Signatures would
come from host headers or DWARF, with a rule that every symbol the guest imports must be safely
translatable. Hazards: memory ownership across allocators (`libfex_malloc` mitigates),
libc-owned objects (`FILE*`, `DIR*`, `jmp_buf`), `va_list`, `long double`, C++ ABIs, callbacks,
and late `dlsym` calls. **Owner decision 2026-09-16:** research this only after several real
applications run with curated thunks, so the study uses measured import sets rather than
guesses.

### 6.3 Scope decision: library thunks only

Decided 2026-09-16: "thunk the toolchains" means **library thunks** (§6.1). Exec redirection
to native cross compilers is out of scope.

- How much thunks help depends on the program. A GL, Vulkan or audio-heavy program gains a
  lot. A CPU-bound program, like a compiler run under emulation, gains nothing and depends
  entirely on JIT quality.
- **Don't thunk `libc`, `libstdc++` or `libgcc_s`.** They carry TLS, allocator and unwinder
  state across the boundary, which is why FEX never thunks them.
- Choose which libraries get thunks from a real measurement: profile a program under
  emulation and find its hot shared objects. Don't pick from a wishlist.

### 6.4 Rootfs: Arch Linux ARM

Decided 2026-09-16. It maps most directly onto the Arch POWER host: same package names and
`PKGBUILD` layout, and similar glibc and GCC versions. Host thunk halves and guest headers
therefore come from matching library versions.

- `[SPEC]` Check the page-size alignment of Arch Linux ARM's aarch64 packages
  (`readelf -lW` `p_align` across `/usr/lib`). Generic aarch64 builds use 4K, but some
  packages are built for 64K. Either works on the 4K-on-64K granule model. The check tells us
  which SMC granule behaviour to expect.
- Match glibc versions where possible. The guest glibc stays emulated, but thunk host halves
  link against the host's glibc 2.43.

---

## 7. Verification

The house rule applies: **parity before timing, with controls that fire.**
(`verification-discipline.md` `[DOC]`).

- **A free oracle is already on the bench.** The Raspberry Pi 5 that mounts this tree over
  sshfs is a Cortex-A76 (ARMv8.2-A: LSE, crypto, CRC32, FP16, DotProd; no SVE). Record
  single-instruction traces on the Pi and replay them in POWERarm on arkamedes. Diff registers,
  NZCV, FPSR and memory.
- **Fuzz each instruction** with a random-encoding generator over the decode table. dynarmic's
  fuzz harness is 0BSD, so its structure can be reused.
- **`qemu-aarch64` as a second oracle** for anything the A76 doesn't implement. Run it as a
  separate binary, never linked.
- **Suites:** LTP syscalls (arm64 build), glibc `make check` under emulation, the `gcc` torture
  tests cross-built for aarch64, and busybox plus Alpine arm64 end to end.
- **Positive controls:** every probe gets one deliberate miscompile (for example, flip the carry
  convention in one lowering) that the suite must catch.
- **POWER8 path:** run the whole suite with `ISA30` forced off on POWER9 silicon, per the
  existing gate.

---

## 8. Milestones

**Update 2026-09-16:** M1 no longer includes a separate interpreter. M1 and M2 are re-split in [`M1-PLAN.md`](M1-PLAN.md): the frontend goes straight to the JIT, and correctness comes from differential testing against the Pi.

| # | Deliverable | Exit criterion |
|---|---|---|
| **M0** | Fork fastppcx86 with history, rename the product, delete the x86 guest, add an empty A64 frontend and arm64 `CPUState` (§10.4) | The tree builds. The list of fixes it took is the coupling census |
| M1 | A64 interpreter plus static ELF loader with a minimal syscall set | Static arm64 `hello`, `busybox --help`, instruction-level parity against the Pi. **From M1 on, every exit criterion must pass on both 4K and 64K host kernels** |
| M2 | A64 → FEX IR frontend for base integer and branch instructions, JIT enabled | Static coreutils via JIT, fuzz parity for integer instructions |
| M3 | Dynamic linking, Arch Linux ARM rootfs overlay, binfmt, signals, vDSO, threads | Bash and Python from the arm64 rootfs, run through binfmt |
| M4 | FP/NEON/crypto/CRC, exclusive monitors plus LSE | glibc tests, OpenSSL `speed` parity, a contention test for LL/SC |
| M5 | Library thunks: GL/Vulkan first, then libcrypto/libssl (§6.1) | `vkcube` and SuperTuxKart render on the host GPU; `openssl s_client` and `curl https://` run through the thunk |
| M5.5 | **Self-hosting:** Arch Linux ARM's own `gcc`/`cmake`/`make` run under POWERarm | A real package builds under emulation, with output byte-identical to the Pi's build. POWERarm's own guest thunk halves build on the POWER9 through POWERarm |
| M6 | Performance: RAS, register-pinning census, flag elimination, ISA 3.0 paths | Measured against `qemu-aarch64` on arkamedes, with machine configuration pinned (handbook §Measuring) |

---

## 9. Owner decisions (2026-09-16)

| Question | Decision |
|---|---|
| Meaning of "toolchain thunks" | **Library thunks** only. Exec redirection is out of scope (§6.3) |
| Rootfs source | **Arch Linux ARM** (§6.4) |
| Second guest in fastppcx86, or a split | Research needed. Result: **split with shared history and rename** (§10) |
| Host `long double` | **IBM double-double**, measured. No auto-thunking of `long double` (§6.1) |
| Presented CPU | Cortex-A72/A76-class HWCAP profile, tiered (§4.8) |
| Page size | Host page size, with a 4K granule fallback per process; test on 4K and 64K kernels (§4.9) |
| Address space | Match the host window (47-bit on 64K, 46-bit on 4K), per-app `GuestVABits` override (§4.9) |
| Thunk order | GL/Vulkan, then OpenSSL (§6.1) |
| Name | Keep POWERarm; README carries a trademark non-affiliation line |

---

## 10. Second guest in fastppcx86, or split and rename?

Measured on arkamedes against `~/Development/fastppcx86` @ `4b51c0773` (branch `power9team`,
126 ahead of origin, 15,178 commits, 1,071 since 2026-05-01) `[MEASURED]`.

### 10.1 What's shared and what's x86-only

> **M0b re-measure:** after the x86 guest core was removed, see
> [`M0-CENSUS.md`](M0-CENSUS.md). It has per-subsystem diff stats, compile-error classes,
> the `POWERARM-M0-TODO` list and these regex counts before and after (4,794 → 595 matching
> lines).

Line counts cover `.cpp`/`.h`/`.inl`/`.json`/`.py`. The x86 references are a regex over
`gregs`, `xmm`, `X86State`, `RFLAG_`, `x87`/`F80`, `mxcsr`, `pf_raw`/`af_raw`, `rip`,
`CPUID`, `XGetBV`, segment state, `AVX` and `SSE`.

| Subsystem | Lines | x86 refs | Would POWERarm reuse it? |
|---|---|---|---|
| `CodeEmitter` (PPC64LE encoder) | 22,456 | 11 | Yes, almost unchanged |
| `Core/JIT/PPC64LE` (backend) | 27,305 | 656 | Yes. The refs are state offsets, the x87 lowering and a few hard-wired guest registers (e.g. `GuestRCX = StaticRegisters[1]`, `JIT.cpp:6074`) |
| `Interface/IR` + passes | 10,837 | 220 (183 in passes) | Mostly. 427 ops in total: 68 in the `F80` class, plus about 10 x86-only ops (`LoadPF/AF`, `StorePF/AF`, `CPUID`, `XGetBV`, `StoreMemTSO`, `SetRoundingMode(MXCSR)`, `VAESKeyGenAssist`) |
| `Source/Tools/LinuxEmulation` | 47,264 | 688, in 45 of 65 `.cpp` files | Partly. Granule memory, SMC, VMA tracking and the thread manager are guest-neutral in intent. Signals (`SignalDelegator.cpp`: 140 refs), `x64/` (3,181) and `x32/` (11,045) are x86 |
| `ThunkLibs` | 37,238 | 195 | Host halves yes. Generator and guest halves need an AAPCS64 model |
| `OpcodeDispatcher*` + `X86Tables` + `Frontend.cpp` | ~26,700 | all of it | **No.** Replaced by the A64 frontend |
| `CPUState` (`CoreState.h`) | 577 | Layout is x86 | **No.** `gregs[16]`, `xmm` (AVX-aligned), `flags[48]` with `RFLAG_*`, `pf_raw`/`af_raw`, segments, `mm[8]`, `FCW`, `mxcsr`, plus `static_assert`s that encode arm64-host `ldp` reach |

**One existing seam:** the backend already switches register maps by guest mode
(`x64::SRA` / `x32::SRA`, `JIT.cpp:2595-2605` `[CODE]`). An `a64::SRA` fits that shape.

### 10.2 Why a second guest inside fastppcx86 is the wrong call now

- **It would destabilize a working product.** fastppcx86 runs The Witcher 3, Cyberpunk 2077
  and RimWorld today and gains ~250 commits a month. Pulling `CPUState`, IR passes and 656
  backend refs behind a guest abstraction touches the hottest code in that tree. Every step
  risks a gaming regression that has nothing to do with ARM.
- **The CPU state can't be shared.** Supporting both guests in one tree needs either a union
  state with static asserts for both, or templated state offsets through the whole backend.
  Upstream FEX never did either, so there's no precedent to follow.
- **The names are wrong.** The binaries are `FEX*`, the environment variables `FEX_*`, the
  project is "x86", and its README promises those names won't change.
- **Nothing is lost by splitting,** as long as the split keeps history (§10.4).

### 10.3 Fork base (done 2026-09-16)

`~/Development/POWERarm` is a fresh clone of <https://github.com/daedalao/fastppcx86> on the
local branch `powerarm`. It starts at **`daedalao-wt` @ `9da3ccf9f`** (2026-09-15) and tracks
that branch for pulls. The remote is named `fastppcx86`, and its push URL is deliberately
disabled. How the branches compare `[MEASURED]`:

| Branch | Relationship to `daedalao-wt` |
|---|---|
| `main` @ `60cbfe6df` | 3 commits ahead, all PR merge commits. `git merge-tree` of the two gives exactly `daedalao-wt`'s tree (`21c63ba26`), so `main` adds no content |
| `daedalao-wt` | 68 commits ahead of `main`: 28 docs, 10 on 64K pages, 7 on the ntsync trace, THP, futex trace, tests, one libGL thunk fix, SMC and signals |
| origin `power9team` @ `57b15706b` | Fully contained, 197 behind |
| local `power9team` @ `4b51c0773` (the working tree in `~/Development/fastppcx86`) | 74 behind. Its 3 unique commits are two PR merges plus `4b51c0773` (the `Orlshr` i32 fix), which `daedalao-wt` already has in a better form as `3f1908f77` (it also fixes `Ornror`) |

**Not carried:** the uncommitted working-tree changes in `~/Development/fastppcx86`. They
aren't x86 or ntsync work but two guest-neutral Linux-layer fixes, which POWERarm will want:
- `FEXInterpreter.cpp`: a program inside the rootfs has its path resolved against the host
  filesystem, so a rootfs `python3` symlink resolved to the host's and ran natively.
- `FileManagement.cpp` / `Syscalls.cpp`: the `renameat2` EXDEV copy fallback created a 0-byte
  file for directory sources and ignored `RENAME_EXCHANGE`/`RENAME_NOREPLACE`.

They're saved, not applied, as `docs/powerarm/carry-patches/0001-*.patch`. They'd be better
committed to fastppcx86 first and then cherry-picked.

The census in §10.1 was measured on local `power9team` @ `4b51c0773`. `daedalao-wt` differs by
74 commits that don't touch the backend or IR in bulk, so the conclusions hold. Re-run the
counts at M0.

### 10.4 Recommended shape

1. **Fork with history.** Done (§10.3).
2. **Rename the product, not the internals.**
   - Rename: binaries (`POWERarm`, `POWERarmServer`, `POWERarmRootFSFetcher`), environment
     variables (`POWERARM_*`), config directory (`~/.powerarm`), `FEXServer` socket names,
     binfmt entry (`POWERarm-aarch64`) and the rootfs directory.
   - The two emulators **must not collide** when installed side by side, which is the normal
     case on this rig.
   - **Keep** `FEXCore/`, `CodeEmitter/` and `ThunkLibs/` paths and namespaces. Identical paths
     are what make `git cherry-pick` of backend fixes work in both directions.
3. **Delete the x86 guest in POWERarm:** `OpcodeDispatcher`, `X86Tables`, `x64/`, `x32/`, the
   `F80` ops and TSO. It isn't needed there, and deleting it shrinks the refactor surface.
4. **Backend fixes flow both ways by cherry-pick.** Tag commits that touch
   `CodeEmitter/PPC64LE` or `JIT/PPC64LE` with a `ppc64le/jit:` prefix. fastppcx86 already
   does this (`4b51c0773 ppc64le/jit: Orlshr at i32Bit ...`).
5. **Revisit once POWERarm's A64 frontend works (end of M2).** If cherry-picks become frequent
   and painful, extract `CodeEmitter/PPC64LE` + `JIT/PPC64LE` + IR into a shared library that
   both projects consume. By then the guest-neutral boundary will be known from real code, not
   guessed.

M0 becomes: fork, rename, delete the x86 guest, then get the tree compiling with an empty A64
frontend and an arm64 `CPUState`. Build errors from that step are the exact, complete census
of x86 coupling.

---

## Sources

- FEX-Emu: <https://github.com/FEX-Emu/FEX> (MIT)
- dynarmic: <https://github.com/lioncash/dynarmic>, <https://github.com/yuzu-mirror/dynarmic/blob/master/src/dynarmic/interface/A64/a64.h> (0BSD)
- Eden's dynarmic with the ppc64 backend: <https://git.eden-emu.dev/eden-emu/eden/src/commit/35e45fbad3ff6ad19fb862844b8a990e4dedbfff/externals/dynarmic>, <https://git.eden-emu.dev/eden-emu/eden/commit/2db4c35c54289e043c5ae4568d524e2bbaddb6a8>
- box64 ppc64le backend: <https://box86.org/2026/04/new-box64-v0-4-2-released/>, <https://github.com/ptitSeb/box64/issues/3577>
- arm64emu-user: <https://github.com/sylirre/arm64emu-user> (Apache-2.0)
- Maarch64: <https://github.com/Maarch64-Project/Maarch64> (MIT/Apache-2.0)
