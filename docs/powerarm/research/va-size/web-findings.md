# How real AArch64 Linux software depends on user VA width (47-bit vs 48-bit)

Context: AArch64-guest Linux user-mode emulator on ppc64le hosts. Native arm64 Linux (4K/4-level or 64K/3-level, 48-bit VA) gives userspace `DEFAULT_MAP_WINDOW = 1<<VA_BITS_MIN` = 256 TB, with the stack and mmap base near `0xffff_xxxx_xxxx`. The question is what breaks if the guest instead sees a 47-bit (128 TB) layout: mmap without a hint stays below 2^47, and hints or MAP_FIXED above 2^47 fail or get relocated.

Research date: 2026-09-16. Source was fetched from upstream `main`/`master` branches unless a tag is named. Tags used below:
- **[DOC]**: I read it in the cited source, doc or commit.
- **[INFERENCE]**: my reasoning from that source; not tested.

---

## Summary table

| Program | VA-width assumption on aarch64 Linux | Probe or hardcode | What happens on a 47-bit guest layout | Severity |
|---|---|---|---|---|
| **TSan (C/C++), LLVM ≤ 23.1.1** | One of four fixed shadow layouts (39/42/48; 47 is only on `main`) | Probe: `vmaSize = MSB(current frame address)+1` | **FATAL "unsupported VMA range, Found 47 - Supported 39, 42 and 48"**, then abort | **High** (release toolchains) |
| **TSan (C/C++), LLVM main after 2026-07-16** | Adds `MappingAarch64_47` | Same stack probe | Works if the whole layout looks like 47-bit (PIE ~0x5555…, libs/stack 0x7c00…–0x8000…) | Low |
| **Go `-race` (TSan Go runtime)** | Accepted only 48 until 2026-09-03 (LLVM main) | Same stack probe | **FATAL "Found 47 - Supported 48"** with every Go race-detector `.syso` shipped before that fix | **High** for `-race` users |
| **MSan** | One fixed 48-bit layout; the allocator is MAP_FIXED at `0xE000_0000_0000` (above 2^47) | Hardcoded; the stack probe only skips regions above max | [INFERENCE] fails: code or allocator falls outside app ranges, re-execs without ASLR, then dies | **High** |
| **ASan** | Shadow offset 1<<36; HighMemEnd taken from the stack probe | Probe | Works (39/42-bit layouts are already supported) | None |
| **HWASan** | Dynamic shadow sized from the stack probe; needs TBI | Probe | Works as far as VA goes; TBI emulation is a separate requirement | None (VA) |
| **LSan (standalone)** | Allocator at `0x5000_0000_0000` | Hardcoded | Below 2^47, so it works | None |
| **V8** | Hardcodes 48 hardware bits, so a 47-bit/128 TB user space; hints below 2^38 | Hardcoded | Same assumption as native, so no change | None |
| **LuaJIT GC64** | GC objects must sit below 2^47 | Probe: mmap with random hints under 2^47, checks result | A 47-bit guest is *easier* (first mmap already fits) | None |
| **LuaJIT (old, pre-GC64/lightud segments)** | Lightuserdata ≤ 47 bits | Hardcoded | 48-bit native broke it; a 47-bit guest *fixes* it | None (improvement) |
| **HotSpot compressed oops/class space** | Search cap `absolute_max = 128 TB`; EOR-mode klass bases ≤ `0x7fff<<32` | Hardcoded ≤ 2^47, attach attempts are probed | Works | None |
| **HotSpot ZGC** | Max address bit probed from 46 down (never above 46) | Probe (msync + mmap) | Same result as 48-bit native (46) | None |
| **Go runtime (non-race)** | `heapAddrBits = 48`; arm64 hints `i<<40 \| 0x40<<32`, all < 2^47 | Hardcoded hints, then falls back to any address | Works; randomized heap base uses 47 bits | None |
| **jemalloc** | `LG_VADDR=48` fixed at configure time on aarch64 | Hardcoded | Works (47 ⊂ 48). Breaks only above 2^48 | None |
| **mimalloc (v2/v3)** | `MI_MAX_VABITS=48`; hints at 2–30 TiB; huge pages at 32 TiB+ | Hardcoded (only RISC-V probes) | Works | None |
| **SpiderMonkey** | JS::Value needs pointers below 2^47 | Probe (`FindAddressLimit` tries [2^47,2^48) first); caps at 2^47 | Works; actually uses the more reliable fallback path | None |
| **.NET CoreCLR GC** | Before 2026-09-16: 128 TB constant. Now: MAP_FIXED_NOREPLACE probe of 52/48/47/42/39/36 | Probe (new), hardcoded (≤ .NET 10) | Works; regions range = min(256 GB, 2×RAM, limit/2) | None* |
| **.NET executable allocator** | ±2 GB of libcoreclr | Relative | Width-independent | None |
| **Mono** | Nothing found | Not verified | [INFERENCE] no dependency expected | Unknown/low |
| **Wasmtime** | Docs say "47-32 = 15 bits", about 32k 4 GiB slots | Hardcoded sizes, OS picks addresses | Works; pooling capacity is the same as native | None |
| **QEMU linux-user (as a reference emulator)** | aarch64 `TARGET_VIRT_ADDR_SPACE_BITS = 52`; `TASK_UNMAPPED_BASE = 1<<46`; `ELF_ET_DYN_BASE = 2/3·2^48` | With `reserved_va=0`, guest VA ≈ host VA | Guest sees whatever the host gives; the PIE hint 0xAAAA… is relocated below 2^47 on x86-64 hosts | Reference |
| **PartitionAlloc** | arm64 Linux hints limited to 2^38 + 64 GB offset; pools ≤ 16 GB | Hardcoded low | Works | None |
| **JavaScriptCore / bmalloc** | `EFFECTIVE_ADDRESS_WIDTH = 48` on non-Darwin (Packed pointers, NaN boxing) | Hardcoded | Works (47 ⊂ 48) | None |
| **Julia GC** | 3-level page table covers the full 64 bits (`>>46` top index) | Hardcoded but generic | Works | None |
| **FEX-Emu (x86-64 guest on arm64 host)** | Probes host bits 57…36 with MAP_FIXED_NOREPLACE; if ≥ 48, steals [2^47, 2^48) for itself | Probe | The reverse scenario; shows the probe idiom | Reference |

\* .NET caveat: if your emulator lets a MAP_FIXED_NOREPLACE probe at `2^52 - page` succeed (a ppc64le 64K host *can* map there, see §0), the new .NET probe will report 52 bits. That is harmless for the GC sizing math, but other probers would see the same thing.

**Bottom line:** the only hard failures found are in **sanitizers with fixed shadow layouts: TSan (C/C++ and Go race) on released LLVM, and MSan**. The typical JIT/GC runtimes (V8, SpiderMonkey, LuaJIT, HotSpot, Go, .NET, JSC, jemalloc, mimalloc, Wasmtime) either target ≤ 47 bits already or probe and adapt. Most of them were written with x86-64's 128 TB in mind.

---

## 0. Host-side fact: ppc64le can actually hand out addresses above 128 TB

[DOC] `arch/powerpc/include/asm/task_size_64.h` (https://github.com/torvalds/linux/blob/master/arch/powerpc/include/asm/task_size_64.h):
- With `CONFIG_PPC_64K_PAGES`: `TASK_SIZE_USER64 = TASK_SIZE_4PB` (2^52) and `DEFAULT_MAP_WINDOW_USER64 = TASK_SIZE_128TB`.
- `arch_get_mmap_end(addr,len,flags)` returns TASK_SIZE if `addr > DEFAULT_MAP_WINDOW`, or if `MAP_FIXED` and `addr+len > DEFAULT_MAP_WINDOW`.
- So on a 64K-page ppc64le kernel, hints above 128 TB and MAP_FIXED above 128 TB **succeed** (the "large window" opt-in).
- With 4K pages the user VA is only **64 TB (46-bit)**. `TASK_SIZE_USER64 = DEFAULT_MAP_WINDOW = TASK_SIZE_64TB`.

[INFERENCE] An emulator on a 64K-page host could build a full 48-bit guest space: reserve [2^47, 2^48) with a high hint, or apply a guest_base offset. The "hints above 2^47 fail" premise therefore holds only if the emulator chooses it, or on 4K-page hosts (where even 47 bits is unavailable).

For comparison, arm64: `DEFAULT_MAP_WINDOW_64 = 1 << VA_BITS_MIN` and `TASK_SIZE_64 = 1 << vabits_actual` (https://github.com/torvalds/linux/blob/master/arch/arm64/include/asm/processor.h, lines ~55-56).

---

## 1. Sanitizers (compiler-rt)

### 1.1 TSan — the main hard failure

**Detection** [DOC] `compiler-rt/lib/tsan/rtl/tsan_platform_linux.cpp` `InitializePlatformEarly()` (main, lines ~339-355):
```c
vmaSize = (MostSignificantSetBitIndex(GET_CURRENT_FRAME()) + 1);
#if defined(__aarch64__)
# if !SANITIZER_GO
  if (vmaSize != 39 && vmaSize != 42 && vmaSize != 47 && vmaSize != 48) {
    Printf("FATAL: ThreadSanitizer: unsupported VMA range\n"); ... Die();
# else
  if (vmaSize != 47 && vmaSize != 48) { ... Die(); }
```
- The probe is **the address of the current stack frame**. No /proc and no mmap probing.
- The layout is chosen from `vmaSize` via a switch in `tsan_platform.h` (~line 836): `MappingAarch64_39/_42/_47/_48`.

**Release status** [DOC]:
- `llvmorg-23.1.1` (released 2026-09-08) and `llvmorg-22.1.8` print `"Supported 39, 42 and 48"` (C/C++) and `"Supported 48"` (Go). **47 is rejected** in every released LLVM.
- `MappingAarch64_47` was added on main by commit f020acfd395d (2026-07-16), "[tsan] Add 47-bit VMA mapping for linux/aarch64 (#205949)". Its purpose was the Android emulator with 16K pages and 47 VA bits (https://github.com/llvm/llvm-project/commit/f020acfd395d9094303e78c807bc168a46a783fa, fixes llvm/llvm-project#151931).
- Go's arm was extended by commit 2930d9f4 (2026-09-03), "[tsan] Accept a 47-bit VMA for the Go runtime on linux/aarch64 (#220040)". Its message: "on a kernel with a 47-bit user address space every Go binary built with `-race` dies at startup: `FATAL: ThreadSanitizer: unsupported VMA range / Found 47 - Supported 48`". It was verified on gVisor with a 47-bit aarch64 layout (https://github.com/llvm/llvm-project/commit/2930d9f4295571c01469741d8dc18ea0c4fd7ab5, fixes #220037).

**47-bit layout** [DOC] (`tsan_platform.h` ~218-247):
- Low app 0–0x0400_0000_0000
- Shadow 0x0aaa…–0x2800…
- Meta 0x4000…–0x5000…
- PIE 0x5555…–0x5c00…
- High app/stack 0x7c00…–0x8000_0000_0000

The 48-bit layout has PIE at 0xaaaa…–0xac00… and high app at 0xfc00…–0x1_0000_0000_0000.

**Behaviour on a 47-bit guest** [INFERENCE]:
- (a) With released TSan runtimes (Clang ≤ 23, GCC's libtsan synced from older compiler-rt, and Go's prebuilt `race_linux_arm64.syso`), any guest stack below 2^47 gives a **fatal abort at startup**.
- (b) With main-branch TSan the process works only if the rest of the layout matches the kernel's 47-bit shape: PIE at ~2/3·TASK_SIZE = 0x5555_5555_xxxx, and mmap base/stack near 0x7fff_xxxx_xxxx.
- (c) The hybrid case is also fatal: stack placed above 2^47 (so vmaSize=48) while mmap(NULL) returns addresses below 2^47. Those addresses land in the 0x5400…–0x8000… gap of `MappingAarch64_48`, and `CheckAndProtect` rejects them ("unexpected memory mapping"). TSan re-execs without ASLR and then dies (re-exec logic: commit 0784b1ee, https://github.com/llvm/llvm-project/commit/0784b1eefa36d4acbb0dacd2d18796e26313b6c5).

**Emulator precedent** [DOC]: commit 7376a706 "[tsan] fit Go/s390x mapping under QEMU (#204503)" moved a Go/s390x TSan region below 2^47. The reason was QEMU linux-user with guest_base=0 on a 4-level x86-64 host: mmap at 144 TiB failed with ENOMEM (https://github.com/llvm/llvm-project/commit/7376a706150289cf60fb3648e75e909ab6e86f68).

### 1.2 MSan — hardcoded 48-bit layout

[DOC] `compiler-rt/lib/msan/msan.h` lines 72-97 (`SANITIZER_LINUX && __aarch64__`):
- Comment: "The mapping assumes 48-bit VMA."
- App ranges: `0x0000…–0x0100_0000_0000` (low), `0x0A00…–0x0B00…` ("48-bits PIE"), `0x0E40…–0x1_0000_0000_0000` (libs). `ALLOCATOR` sits at `0x0E00_0000_0000–0x0E40…`.
- `MEM_TO_SHADOW(mem) = mem ^ 0xB00000000000`.

[DOC] `msan_allocator.cpp` lines 147-158: `AP64::kSpaceBeg = 0xE00000000000ULL` (4 T), which is **above 2^47**.

[DOC] `msan_linux.cpp` `InitShadow()`:
- Checks `MEM_IS_APP(&__msan_init)`, else prints "FATAL: Code %p is out of application range. Non-PIE build?".
- Skips layout segments with `start >= GetMaxUserVirtualAddress()` (the stack-probe value).
- On failure, `InitShadowWithReExec` re-execs with ADDR_NO_RANDOMIZE.

[INFERENCE] On a 47-bit guest the PIE lands near 0x5555_xxxx_xxxx, which is inside `shadow-15` (0x4000…–0x6000…), so it is not APP. You get the "out of application range" fatal, then a re-exec, then death. Even a non-PIE binary would hit the fixed allocator reservation at 0xE000_0000_0000, which is above the 47-bit limit. **MSan on aarch64 requires a true 48-bit layout.** No 47-bit MSan layout exists upstream for aarch64 Linux; x86-64 and loongarch have their own.

### 1.3 ASan — adapts

[DOC] `asan_mapping.h`:
- `#elif defined(__aarch64__)` gives `ASAN_SHADOW_OFFSET_CONST 0x0000001000000000` (1<<36).
- Layout comments exist for 39-, 42- and 48-bit VMA (lines ~85-108).

[DOC] `asan_rtl.cpp` `InitializeHighMemEnd()` sets `kHighMemEnd = GetMaxUserVirtualAddress()`.

[DOC] `sanitizer_linux.cpp` `GetMaxVirtualAddress()` (~1235-1250), for ppc64/aarch64/loongarch/riscv64: `return (1ULL << (MostSignificantSetBitIndex(GET_CURRENT_FRAME()) + 1)) - 1;`. The comment says "aarch64 has multiple address space layouts: 39, 42 and 47-bit".

[DOC] `asan_allocator.h`: non-Android aarch64 uses `kAllocatorSpace = ~(uptr)0` (dynamic) and a 4 T size. The comment reads "VMA is usually 48 bits and we have lots of space".

[INFERENCE] 47-bit works. HighShadow shrinks and the dynamic allocator reservation lands wherever mmap puts it.

### 1.4 HWASan

[DOC] `hwasan_linux.cpp`:
- `max_address = GetMaxUserVirtualAddress()` (line ~101).
- Dynamic shadow via `FindDynamicShadowStart(shadow_size_bytes)` (~123).
- Tagged-address ABI enabled with `prctl(PR_SET_TAGGED_ADDR_CTRL, ...)` (~188-201). Failure is fatal.

[INFERENCE] VA width does not matter. The emulator must emulate TBI plus that prctl.

### 1.5 LSan standalone

[DOC] `lsan_allocator.h` ~96-102: Android aarch64 uses `0x3000000000`. Other aarch64 uses `kAllocatorSpace = 0x500000000000` (4T), which is below 2^47. Works.

---

## 2. V8 (Node, Chromium, Deno)

**Pointer-compression cage** [DOC] `include/v8-internal.h` line 167: `kPtrComprCageReservationSize = kPtrComprCageBaseAlignment = 1<<32`. The reservation is hinted (see below); there is no bit-width assumption.

**Sandbox** [DOC] `include/v8-internal.h` ~220-320:
- `kSandboxSizeLog2 = 40` (1 TB) on non-Android/iOS/RISC-V/Loong64.
- `kSandboxGuardRegionSize = 32 GB + 32 GB` = 64 GB on each side.
- `kAdditionalTrailingGuardRegionSize = 288 GB - 64 GB`.
- `kSandboxMinimumReservationSize = 8 GB`.
- The full reservation is about 1 TB + 64 GB + 288 GB ≈ 1.35 TB.

**Address-space limit** [DOC] `src/sandbox/sandbox.cc` `DetermineAddressSpaceLimit()` (~257-335):
- `kDefaultVirtualAddressBits = 48`. Only x64 reads CPUID.
- Arm64 Android is forced to 40, iOS to 37, RISC-V reads /proc/cpuinfo.
- Arm64 Linux keeps **48**, then `hardware_virtual_address_bits -= 1` ("split 50/50"), giving a **128 TB** limit.
- `SysInfo::AddressSpaceEnd()` on POSIX returns `UINTPTR_MAX` (`src/base/sys-info.cc` ~136-148), so no software cap applies.
- `max_reservation_size = limit/4` = 32 TB, which is ≥ 1 TB, so a full sandbox is used.
- If reservation fails it falls back to partially-reserved sandboxes, halving down to 8 GB. Total failure gives `FatalProcessOutOfMemory("Failed to reserve the virtual address space for the V8 sandbox")`.

**Hints** [DOC] `src/base/platform/platform-posix.cc` `OS::GetRandomMmapAddr()` (~363-400):
- ARM64 Linux/Android: `raw_addr &= 0x3FFFFFF000` (38 bits). The comment says "default ... limited to 39 bits when using 4KB pages".
- Sanitizer builds use `raw_addr &= 0x007fffff0000; raw_addr += 0x7e8000000000` (the TSan-derived range, < 2^47).

**Wasm** [DOC via search: https://blog.stackblitz.com/posts/debugging-v8-webassembly/] Non-sandbox builds reserve about 10 GiB per memory against a 1 TiB `kAddressSpaceLimit` budget. I could not re-read that constant on current main (it seems to have moved).

**Behaviour at 47-bit** [INFERENCE] None. V8 already assumes a 47-bit user space on arm64 Linux, and every hint is below 2^38.

---

## 3. LuaJIT

[DOC] `src/lj_obj.h` (v2.1):
- `LJ_GCVMASK = ((uint64_t)1 << 47) - 1` (line 291).
- The itype tag is in bits 47+ (`it64 >> 47`).
- Lightuserdata is "segmented" to stay within 47 bits: `LJ_LIGHTUD_BITS_SEG 8`, `LJ_LIGHTUD_BITS_LO (47 - 8)` (lines 295-297, 842-844).

[DOC] `src/lj_alloc.c` (v2.1):
- `LJ_ALLOC_MBITS 47 /* 128 TB in LJ_GC64 mode */` (line 102). Non-GC64 uses 31 on x64 and 32 elsewhere.
- `mmap_probe()` (233-275) mmaps at `hint_addr`, accepts only `(addr >> LJ_ALLOC_MBITS) == 0`, and otherwise retries up to 30 times: 5 linear steps of +16 MB, then random hints `lj_prng_u64 & ((1<<47) - PAGESIZE)`.
- `MAP_32BIT` is used only for `LJ_64 && !LJ_GC64` (x64 non-GC64), lines 112-114 and 299.
- `CALL_MREMAP_MV` is NOMOVE for arm64 GC64 (line 368), so mremap cannot move blocks above 47 bits.

History [DOC]:
- LuaJIT issue #49 "47 bit address space restriction on ARM64" (https://github.com/LuaJIT/LuaJIT/issues/49).
- PR #230 "fix lightud for 48bit virtual address" (https://github.com/LuaJIT/LuaJIT/pull/230).
- Downstream breakage from "bad light userdata pointer" on 48-bit arm64 kernels, where stack and static addresses are ~0xffff…: neovim #7879 (https://github.com/neovim/neovim/issues/7879), torch7 #1035, Kong openresty-patches #47 (https://github.com/Kong/openresty-patches/pull/47).
- The fix upstream is the lightud segment table above.

**Behaviour at 47-bit** [INFERENCE] Better than native. The first unhinted mmap already passes the 47-bit check. Old distro LuaJIT builds that die with "bad light userdata pointer" on real 48-bit arm64 would *work* on a 47-bit guest. Mcode allocation is ±128 MB relative (`LJ_TARGET_JUMPRANGE`, `lj_mcode.c` ~264-320), so width does not matter there.

---

## 4. OpenJDK HotSpot

**Heap/class-space search cap** [DOC] `src/hotspot/share/runtime/os.cpp` `os::attempt_reserve_memory_between()` line 2067:
```c
char* const absolute_max = (char*)(NOT_LP64(G * 3) LP64_ONLY(G * 128 * 1024));
```
All "reserve between" searches are clamped to **128 TB = 2^47**. Compressed-oops attach modes use this (`memoryReserver.cpp`: unscaled < 4 G, zero-based < 32 G, heap-based via `HeapSearchSteps`, disjoint-base attach lists). So does compressed class space (`compressedKlass.cpp`: `reserve_address_space_for_unscaled_encoding`, `_zerobased_`, and `_16bit_move` over `[2^32, 2^48)`, which is clamped to 2^47 anyway).

**aarch64 class space** [DOC] `src/hotspot/cpu/aarch64/compressedKlass_aarch64.cpp`:
- `reserve_at_eor_compatible_address()` tries up to 64 hardcoded bases `immediates[i] << 32`, where the largest is `0x7fff<<32` = `0x7fff_0000_0000` < 2^47.
- Comment: "we still lack a good abstraction for that (see JDK-8320584), therefore we assume and hard-code 2^48".
- Each attempt uses `os::attempt_reserve_memory_at` (hint plus a result check). When all fail, the caller reserves anywhere.

**ZGC** [DOC] `src/hotspot/cpu/aarch64/gc/z/zAddress_aarch64.cpp`:
- `DEFAULT_MAX_ADDRESS_BIT = 46`, `MINIMUM_MAX_ADDRESS_BIT = 36`.
- `probe_valid_max_address_bit()` loops `i = 46 .. 37`:
  - `msync(1<<i, page, MS_ASYNC)`: 0 means valid.
  - `ENOMEM` means it tries `mmap(1<<i, PROT_NONE, MAP_NORESERVE)` without MAP_FIXED and accepts if `result == base`.
  - Other errno values log a warning and `continue`.
  - If nothing works, it mmaps a hint at 2^46 and takes the MSB of the result.
- `ZPlatformAddressOffsetBits()`: `max = probed+1-3`, `min = max-2`, clamped around the requested heap size.
- The probe never looks above 2^46, so native 48-bit and a 47-bit guest give identical results.

**Shenandoah** [INFERENCE] No colored pointers. It uses the same compressed-oops reservation path, so there is no extra width dependency (not separately verified in source).

**Behaviour at 47-bit** [INFERENCE] None. The emulator needs faithful `msync` errno (ENOMEM for unmapped but valid) and hint-honoring mmap below 2^47. Otherwise ZGC probes lower and silently shrinks the heap-address range.

---

## 5. Go runtime (arm64)

[DOC] `src/runtime/malloc.go`:
- `heapAddrBits = 48` for 64-bit non-iOS (line 213). The comment covers arm64 ("promote ios/arm64 to a 48-bit address space like every other arm64 platform").
- `arenaBaseOffset = 0xffff800000000000*IsAmd64 + 0x0a00…*IsAix` (line 314), so it is 0 on arm64.
- Hints (`mallocinit`, lines ~517-650): `case GOARCH == "arm64": p = uintptr(i)<<40 | uintptrMask&(0x0040<<32)` for i = 0x7f..0. The comment: "on arm64 ... slam the allocation at 0x40 << 32 because when using 4k pages with 3-level translation buffers, the user address space is limited to 39 bits". The highest hint is 0x7f40_0000_0000 < 2^47. Hints with i > 0x3f go to the user-arena list; the heap list starts at i=0 (0x40_0000_0000 = 256 GB).
- Race mode: `p = i<<32 | 0x00c0<<32`, restricted to `[0x00c000000000, 0x00e000000000)`.
- `randomizeHeapBase` (on by default: `internal/buildcfg/exp.go` `RandomizedHeapBase64: true`) uses `randHeapAddrBits = heapAddrBits - 1 - IsAmd64` = **47 on arm64** (line 362).
- `(*mheap).sysAlloc` (~749-850) walks hints with `sysReserve(hint)`, discarding failures. When "All of the hints failed" it uses `sysReserveAligned(nil, …)` (any address). It throws only if the result's arena index ≥ `1<<arenaBits` ("memory reservation exceeds address space limit"), i.e. an address ≥ 2^48.
- `mpagealloc_64bit.go` `sysInit`: reserves summary arrays sized for `1<<(heapAddrBits-shift)` anywhere (`sysReserve(nil, …)`).
- `tagptr_64bit.go`: lock-free stack tagged pointers assume `addrBits = 48` on arm64.

**Behaviour at 47-bit** [INFERENCE] Non-race Go works unchanged. **Go `-race` is fatal** with any TSan runtime older than 2930d9f4 (see §1.1), which covers every released Go toolchain as of this date unless Go re-vendored the syso after 2026-09-03. I did not verify Go's syso refresh.

---

## 6. jemalloc and mimalloc

### jemalloc

[DOC] `configure.ac` (dev) ~541-557:
```
aarch64)  if LG_SIZEOF_PTR = 2: LG_VADDR=32 else LG_VADDR=48
x86_64)   runtime CPUID 0x80000008 probe at configure time (cross-compile default 57)
```
LG_VADDR sizes the radix tree (`rtree`) key bits and is used for pointer packing. It is a configure-time constant, with no runtime probe on aarch64. jemalloc uses mmap(NULL) (OS-chosen addresses).

[INFERENCE] 47-bit addresses are a subset, so it works. The only failure mode is addresses ≥ 2^48 (e.g. 52-bit LVA with high hints), which would corrupt rtree lookups. That cannot happen on a 47-bit guest.

### mimalloc

[DOC] v2 `src/os.c` (master):
- `MI_DEFAULT_VIRTUAL_ADDRESS_BITS 48` (line 23).
- `MI_HINT_BASE = 2<<40` (2 TiB), `MI_HINT_AREA = 4<<40`, `MI_HINT_MAX = 30<<40` (lines 111-113). Aligned hints are used only if `virtual_address_bits >= 46` and size ≤ 1 GiB.
- Huge OS pages start at `32<<40` (line ~635).

[DOC] v2 `src/segment-map.c`: `MI_SEGMENT_MAP_MAX_ADDRESS` is 48 TiB (128 TiB under ASan, issue #881).

[DOC] v3 (dev3):
- `include/mimalloc/bits.h`: `MI_MAX_VABITS` is 47 on x64 and **48 on other 64-bit**.
- `src/prim/unix/prim.c` `unix_detect_virtual_address_bits()` probes only on RISC-V (hwprobe or /proc/cpuinfo) and otherwise returns `MI_MAX_VABITS`.
- `src/page-map.c` `_mi_page_map_init` clamps vbits; x64 forces 47. `_mi_safe_ptr_page` returns NULL for `p >= _mi_page_map_max_address`.

[INFERENCE] 47-bit works: all hints are below 32–36 TiB and the page map covers 2^48. (The v2 segment map covering only 48 TiB is a pre-existing native limitation for `mi_is_in_heap_region`, not specific to 47-bit.)

---

## 7. SpiderMonkey (Firefox)

[DOC] `js/public/Value.h`: `JSVAL_TAG_SHIFT 47` on PUNBOX64. The comment says "we only actually need 47 bits … See js::gc::MapAlignedPagesRandom".

[DOC] `js/src/gc/Memory.cpp` (https://github.com/mozilla-firefox/firefox/blob/main/js/src/gc/Memory.cpp):
- `FindAddressLimit()` (~370-398): "Exclude 48-bit and 47-bit addresses first". For `high = 47, 46` it calls `FindAddressLimitInner(high, 4)`, which does `mmap(random hint in [2^high, 2^(high+1)))` *without* MAP_FIXED and records the highest address returned. After that comes a binary search. Comment: "there appears to be no standard way to query the limit at runtime".
- `InitMemorySubsystem()` (~440-465): `numAddressBits = FindAddressLimit()`; `maxValidAddress` is **capped at `0x00007fffffffffff`**; `hugeSplit` sits at 2^46 when capped.
- `MinAddressBitsForRandomAlloc = 43`: the scattershot allocator is used only when ≥ 43 bits.
- `IsInvalidRegion()` rejects any region touching `0xffff800000000000`.
- `MapAlignedPagesRandom()`: 1024 random hinted attempts. Then `if (numAddressBits < 48)` it falls back to `MapAlignedPagesSlow` (over-allocate and trim). Otherwise it does `MOZ_CRASH("Couldn't allocate even after 1000 tries!")` for small sizes.

History [DOC]:
- Bug 1143022 "ARM64: Javascript engine incorrectly assumes virtual addresses are 47 bit" (https://bugzilla.mozilla.org/show_bug.cgi?id=1143022).
- Bug 1441473 "ARM64 mmap loop is slow and will defeat ASLR" (https://bugzilla.mozilla.org/show_bug.cgi?id=1441473).
- Bug 1502733, the refactor that introduced the random allocator (https://bugzilla.mozilla.org/show_bug.cgi?id=1502733).
- Bug 1250400, "ARM/AARCH64: Javascript engine crash".

**Behaviour at 47-bit** [INFERENCE]:
- Probing hints above 2^47 get relocated (or fail, which is also handled), so `numAddressBits = 47`.
- Random allocation stays within [page, 2^47). The `numAddressBits < 48` slow fallback is enabled, so it is *more* robust than native 48-bit.
- If the emulator *honors* hints above 2^47 (as a ppc64le 64K host can), numAddressBits becomes 48, but everything is still capped at 2^47.

---

## 8. .NET CoreCLR / Mono

**GC regions** [DOC] `src/coreclr/gc/interface.cpp` ~325-363:
- `regions_range = GCRegionRange` config, or with a hard limit `2x/5x heap_hard_limit`.
- Otherwise `max` (Server GC) or `min` (Workstation GC) of `(256 GB, 2 × total_physical_mem)`.
- Then `regions_range = min(regions_range, GetVirtualMemoryLimit()/2)`.

**Virtual limit** [DOC] `src/coreclr/gc/unix/gcenv.unix.cpp`:
- ≤ .NET 10 (`release/10.0`, ~1232-1250): `GetVirtualMemoryMaxAddress()` returns the constant `1ull << 47` ("128TB … approximate size of total user virtual address space"). RISC-V uses 2^38.
- main, commit ca72cbf5 "Probe {riscv,loongarch,arm}64 virtual memory address (#133380)" dated 2026-09-16 (https://github.com/dotnet/runtime/commit/ca72cbf525edd3fde717a64feb1ae914d67800d1): `IsUserVirtualAddressSpaceAtLeast(bits)` does `mmap((1<<bits) - page, PROT_NONE, MAP_FIXED_NOREPLACE)`. Success or `EEXIST` means valid. arm64 candidates are `{52, 48, 47, 42, 39, 36}`, falling back to 2^47.
- `GetVirtualMemoryLimit()` prefers `RLIMIT_AS` when finite.

**Executable memory** [DOC] `src/coreclr/utilcode/executableallocator.cpp` `InitLazyPreferredRange`: `reach = 0x7FFF0000` around libcoreclr. `src/coreclr/pal/src/map/virtual.cpp` `ExecutableMemoryAllocator::TryReserveInitialMemory` reserves `MaxExecutableMemorySizeNearCoreClr` near libcoreclr, probing in 8 MB steps on ARM64. All of this is relative, so it is width-independent.

**Behaviour at 47-bit** [INFERENCE]:
- .NET ≤ 10: identical to native (it already assumed 128 TB).
- main: the probe returns 47, giving a 64 TB limit. That does not constrain the 256 GB range.
- If the emulator passes a MAP_FIXED_NOREPLACE at 2^52-page through to a ppc64le 64K host, the probe says 52, which is harmless here.
- The emulator must implement MAP_FIXED_NOREPLACE semantics (EEXIST vs ENOMEM). Kernels older than 4.17 ignore the flag, which is handled by checking `result == probe`.

**Mono** Not verified in source; no web evidence of a Mono arm64 VA-width dependency was found. [INFERENCE] SGen uses OS-chosen addresses; low risk.

---

## 9. Wasmtime / Cranelift

[DOC] `crates/environ/src/tunables.rs` (64-bit defaults, ~321-337):
- `memory_reservation: 1 << 32` (4 GiB), `memory_guard_size: 32 << 20` (32 MiB), `memory_reservation_for_growth: 2 << 30`.
- GC heap: 4 GiB + 32 MiB.
- (The older 2 GiB guard default was replaced by 32 MiB.)

[DOC] `crates/wasmtime/src/config.rs` ~3847-3861 (PoolingAllocationConfig docs): "Many 64-bit systems can only actually use 48-bit addresses by default … and of those 48 bits one of them is reserved to indicate kernel-vs-userspace. This leaves 47-32=15 bits left, meaning you can only have at most 32k slots". `total_memories` defaults to 1000 (~4 TB of VA).

[INFERENCE] No bit assumption in code. The OS picks addresses, and capacity math already assumes 128 TB. No change at 47-bit.

---

## 10. QEMU linux-user (reference for how an emulator handles this)

[DOC]:
- `target/arm/cpu-param.h`: `TARGET_AARCH64` has `TARGET_VIRT_ADDR_SPACE_BITS 52` (raised from 48 when FEAT_LVA was implemented; commit 0af312b6, 2022-03-01, https://github.com/qemu/qemu/commit/0af312b6edd231e1c8d0dec12494a80bc39ac761).
- `linux-user/aarch64/target_mman.h`: `TASK_UNMAPPED_BASE (1ull << (48 - 2))` (64 TB) and `ELF_ET_DYN_BASE TARGET_PAGE_ALIGN((1ull << 48) / 3 * 2)` (0xAAAA_AAAA_A000, above 2^47).
- `linux-user/main.c` ~109-120 and 825-890: `MAX_RESERVED_VA = (1ul << TARGET_VIRT_ADDR_SPACE_BITS) - 1` when `HOST_LONG_BITS > TARGET_VIRT_ADDR_SPACE_BITS`. **`reserved_va` defaults to 0 for 64-bit guests** (auto only when target ≤ 32 bits), so `guest_addr_max = ~0ul`. With `-R/QEMU_RESERVED_VA`, `task_unmapped_base` and `elf_et_dyn_base` are rescaled to `reserved_va/3` and `2/3·reserved_va` when the defaults don't fit.
- `include/user/guest-host.h`: `g2h = x + guest_base`; `h2g_valid(x) = x - guest_base <= guest_addr_max`.
- `linux-user/mmap.c` `mmap_find_vma()` ~447-560: without reserved_va it calls host `mmap(g2h(addr), size, PROT_NONE, MAP_NORESERVE)` as a *hint*. It accepts the host result if `h2g_valid`, fixes alignment by retrying, and wraps once to low memory. `target_mmap` converts to MAP_FIXED on the found hole; guest MAP_FIXED goes straight to the host.
- `linux-user/elfload.c` ~1015-1045: for ET_DYN, `load_addr += elf_et_dyn_base`, "we prefer the kernel to choose some address rather than force the use of LOAD_ADDR via MAP_FIXED". Identity guest_base is tried first (per the LLVM commit 7376a706 message citing elfload.c#L1036-L1042).

[INFERENCE] QEMU's default aarch64 guest on a 64-bit host simply **inherits the host layout**:
- On x86-64 hosts (and on ppc64le hosts, whose default window is 128 TB) the guest stack and mmap base sit below 2^47. The PIE hint 0xAAAA… gets relocated by the kernel.
- Guest MAP_FIXED or hints above 2^47 fail with ENOMEM on x86-64 hosts. On ppc64le 64K hosts they *succeed*, because the high window opens.
- This is exactly the "47-bit guest" scenario, so the TSan Go/s390x-under-QEMU fix and the TSan-47 failures are real-world evidence of the consequences.

---

## 11. Brief items

### Chromium PartitionAlloc

[DOC] `partition_alloc/address_space_randomization.h` ~145-170: ARM64 Linux uses `ASLRMask() = AslrMask(38)` and `ASLROffset() = 0x1000000000`. Comment: "Linux on arm64 can use 39, 42, 48, or 52-bit user space … We use 39-bit as base".

[DOC] `partition_alloc_constants.h` ~330-335: `kPoolMaxSize` is 4, 8 or 16 GiB per pool.

[INFERENCE] Hints and pools are far below 2^47, so no impact. I did not find a `kHighestAddr`-style runtime probe on arm64 Linux.

### JavaScriptCore (Bun, WebKitGTK)

[DOC]:
- `Source/WTF/wtf/PlatformHave.h` ~67-76 and `Source/bmalloc/bmalloc/BPlatform.h` ~328-340: `EFFECTIVE_ADDRESS_WIDTH` is 36 if `HAVE(36BIT_ADDRESS)`, `getMSBSet(MACH_VM_MAX_ADDRESS)+1` on Darwin, and **48 otherwise**. The comment: "We strongly assume that effective address width is <= 48 in 64bit architectures (e.g. NaN boxing)".
- `wtf/Packed.h` uses it for 6-byte packed pointers.
- `wtf/WTFConfig.cpp` line 180: `highestAccessibleAddress = (1 << EFFECTIVE_ADDRESS_WIDTH) - 1`.

[INFERENCE] Hardcoded upper bound only, so 47-bit is fine. (Breaks only if addresses ≥ 2^48 appear.)

### Julia

[DOC] `src/gc-stock.h` ~285-312: the 64-bit GC page table uses `REGION_INDEX(p) = (p >> 46) & 0x3FFFF`, which covers the full 64-bit range. No VA-width dependency found. [INFERENCE] Works.

### FEX-Emu (reverse direction, x86-64 guest on arm64 host)

[DOC] `FEXCore/Source/Utils/Allocator.cpp`:
- `GetHostVABits()` (~105-133) probes `{57,52,48,47,42,39,36}` via `mmap((1<<bits) - page, MAP_FIXED_NOREPLACE)`; success or EEXIST means valid. Comment: "We can't actually determine VA size on ARM safely".
- `Setup48BitAllocatorIfExists()` (~276-290): if ≥ 48, it `StealMemoryRegion(0x8000_0000_0000, 0x1_0000_0000_0000)` and puts FEX's own allocations there, so the x86-64 guest only ever sees below 2^47.
- Doc: https://github.com/FEX-Emu/FEX/blob/main/docs/ProgrammingConcerns.md (the 32-bit variant reserves everything above 4 GB).

[INFERENCE] For your emulator this is the mirror technique: carve the guest range out of the host, and keep emulator-internal allocations outside it.

**Box64**: not verified.

---

## 12. Implications for the emulator design [INFERENCE]

1. **Choose one coherent layout and make every probe agree with it.** Several runtimes infer VA size from:
   - the stack address (TSan, ASan, HWASan, MSan's max check);
   - MAP_FIXED_NOREPLACE at `2^n - page` (.NET main, FEX);
   - hint-honoring (SpiderMonkey, ZGC);
   - msync errno (ZGC).

   A hybrid (stack above 2^47, mmap below 2^47, or high fixed probes succeeding via the ppc64le large window while defaults stay low) is worse than a consistent 47-bit or consistent 48-bit layout.
2. **A consistent 47-bit layout** is fine for all the JIT/GC runtimes surveyed. It breaks TSan (all released LLVM/GCC/Go race runtimes) and MSan on aarch64.
3. **A consistent 48-bit layout** is achievable on ppc64le 64K-page hosts, because hints and MAP_FIXED above 128 TB open the 4 PB window (§0). It is required for MSan and for released TSan. Place PIE at ~0xAAAA…, mmap base and stack near 0xFFFF…, and keep emulator-internal mappings out of [2^47, 2^48) or remap them via guest_base.
4. On **4K-page ppc64le hosts** (64 TB user VA) even 47-bit is impossible without guest_base or a software MMU. TSan-aarch64 has no 46-bit layout. Go's arm64 heap hints above 64 TB would fail but fall back.
