# POWERarm: arm64 vs ppc64le user virtual-address size

Research date 2026-09-16. Research only: no repository was modified.

Evidence tags follow `powerpc64le-handbook/docs/verification-discipline.md`:
**[CODE]** read in source (file:line), **[MEASURED]** probe output in `logs/`, **[DOC]** external documentation or upstream source,
**[SPEC]** predicted from source but not measured. Kernel citations are to `~/Development/linux-7.2.6` on the POWER9 host, unless marked otherwise.
POWERarm citations are to `~/Development/POWERarm` at `c4bfd8c81`, which is the same as `POWERarm-baseline` (`9da3ccf9f`) for every file cited here.

Files next to this document:

| File | What it is |
|---|---|
| `va_probe.c` | The VA probe: no-hint placement, a count of 1 TiB reservations, hint and `MAP_FIXED_NOREPLACE` at 2^46…2^52−page, the ceiling found by stepping one bit at a time, layout lines from `/proc/self/maps`, and five controls. It exits non-zero if a control fails. |
| `fex_vasize_probe.c` | A faithful C port of `FEXCore::Allocator::DetermineVASize()` and the placements POWERarm derives from it |
| `vm_init.c` | `/init` for throwaway KVM guests. It mounts `/proc`, drops to uid 65534, runs both probes and powers off. |
| `web-findings.md` | Per-program web research for §4, with URLs |
| `logs/probe-pi5-*` | Raspberry Pi 5, arm64 6.18.39, 16K pages, `CONFIG_ARM64_VA_BITS=47` (ASLR on and off) |
| `logs/probe-p9-radix-64k-*` | POWER9 bare metal, 7.2.6-64k, Radix |
| `logs/vm-{radix,hash}-{4k,64k}*.log` | POWER9 KVM guests booting the host's own `/boot/vmlinuz-linux-power9{,-64k}`. Hash is selected with `disable_radix`. |
| `logs/vm-tcg-power8-*`, `logs/vm-p8compat-*` | Failed attempts at a POWER8 CPU. These are negative results; see §2.4. |

---

## 0. Summary

1. **There is no single "arm64 VA size".** arm64 Linux ships user address spaces of 39 bits (4K pages, VA_BITS=39: the Raspberry Pi `kernel8.img`, Android GKI), 42 bits (64K, VA_BITS=42), 47 bits (16K, VA_BITS=47: the **Pi 5 kernel this research ran on**), 48 bits (4K/16K/64K, VA_BITS=48, or VA_BITS=52 without a hint) and 52 bits (VA_BITS=52 with a hint above 2^48). [CODE] arch/arm64/Kconfig:1454-1510, memory.h:56-64. [MEASURED] The Pi 5 has a TASK_SIZE of 2^47. [DOC] `/boot/config-6.18.39+rpt-rpi-v8` has `ARM64_VA_BITS=39`. Portable aarch64 software therefore already has to cope with 47 bits and less.
2. **ppc64le user VA depends on the page size, not the MMU.** With 64K pages (Radix or Hash): TASK_SIZE is 2^52, the default mmap window is 2^47, and a hint above the window opens the rest. With 4K pages (Radix or Hash): TASK_SIZE is **2^46** and nothing above that can be mapped. [CODE] task_size_64.h:23-39. [MEASURED] All four combinations in KVM guests, plus bare-metal 64K Radix.
3. A 64K POWER host with no hints is **indistinguishable from the Pi 5's 47-bit arm64 layout**: stack top 0x8000_0000_0000, top-down mmap below it, 127 TiB reservable. [MEASURED] Only three things differ: PIE base, above-window hints (honoured on ppc, relocated on the Pi), and `MAP_FIXED` at 2^47 or higher (succeeds on ppc, ENOMEM on the Pi).
4. On a 64K host, POWERarm already reserves [2^47, 2^48) for its own allocator, inherited from FEX. [CODE] On a 4K host, `DetermineVASize()` returns **42**, not 46, because 46 is missing from its candidate list. The guest stack and interpreter hints are then placed at about 4 TiB and 2.9 TiB, which is inside POWERarm's own internal-arena hint window. [MEASURED] with the port.
5. **Recommendation:** present a **47-bit arm64 guest** (identical to the Pi 5 / Asahi-style 16K VA_BITS=47 kernel) on 64K hosts, and a **46-bit guest** on 4K hosts. Enforce it with a small guest-side check on the mmap family (clamp hints, ENOMEM for fixed mappings above the guest limit). Keep every POWERarm allocation above the guest limit on 64K hosts. Add a per-app `GuestVABits` knob: 48 (64K hosts only) for MSan, released TSan and Go `-race`, which are the only hard 48-bit dependents found (§4); 42 or 39 for TSan on 4K hosts; 52 (LVA) only on request. Every JIT/GC runtime surveyed (V8, LuaJIT, HotSpot/ZGC, Go, SpiderMonkey, .NET, jemalloc, mimalloc, JSC) works at 47 bits, and some work better there. Details are in §6.

---

## 1. Kernel facts from source

### 1.1 ppc64 (Book3S-64)

| Symbol | Where | 64K pages | 4K pages |
|---|---|---|---|
| `TASK_SIZE_USER64` | task_size_64.h:27 / :31 | `TASK_SIZE_4PB` = 2^52 | `TASK_SIZE_64TB` = **2^46** |
| `DEFAULT_MAP_WINDOW_USER64` | :28 / :32 | `TASK_SIZE_128TB` = 2^47 | 2^46 (window = TASK_SIZE, so there is nothing to open) |
| `TASK_CONTEXT_SIZE` (hash context ids) | :29 / :38 | 512 TB | 64 TB |
| `DEFAULT_MAP_WINDOW` (Book3S) | :63-65 | window above | window above |
| `STACK_TOP_USER64` | :70 | 2^47 | 2^46 |
| `TASK_UNMAPPED_BASE_USER64` (legacy bottom-up base) | :50 | 2^45 | 2^44 |
| `arch_get_mmap_end(addr,len,flags)` | :78-81 | TASK_SIZE if `addr > DMW` **or** (`MAP_FIXED` and `addr+len > DMW`), else DMW | same (it has no effect) |
| `arch_get_mmap_base(addr,base)` | :75-76 | `base + TASK_SIZE - DMW` if `addr > DMW` | — |
| `ELF_ET_DYN_BASE` (PIE) | asm/elf.h:27-28 | `0x1_0000_0000` (4 GiB) | same |
| `STACK_RND_MASK` | asm/elf.h:122-124 | `0x3ffff >> (PAGE_SHIFT-12)` pages, about 1 GiB | same |
| `ARCH_MMAP_RND_BITS_MIN` | arch/powerpc/Kconfig:52-55 | 14 (×64K = 1 GiB) | 18 (×4K = 1 GiB) |

**The 4K limit is enforced by page-table geometry and the task size, on both MMUs.**
- Hash 4K: `H_PGD_INDEX_SIZE 9` "maps 2^9 x 128GB = 64TB" (hash-4k.h:8), and `MAX_EA_BITS_PER_CONTEXT 46` (hash-4k.h:14). [CODE]
- Radix 4K: the page tables could address 4PB (radix-4k.h:11), but `TASK_SIZE_USER64` is still 64 TB (task_size_64.h:31). [CODE]
- [MEASURED] `logs/vm-radix-4k.log` and `logs/vm-hash-4k.log` both give a ceiling of 2^46 and 63 TiB reservable. Every hint at 2^46 or above is relocated, and every FNR at 2^46 or above returns ENOMEM.

**Radix and Hash take different code paths, and the boundary rule differs by one address.**
- Radix uses `generic_get_unmapped_area[_topdown]` (slice.c:661, :681 → mm/mmap.c:690-788). The hint is honoured only if `mmap_end - len >= addr` (mm/mmap.c:708, :759), where `mmap_end` comes from `arch_get_mmap_end`, which tests **`addr > DEFAULT_MAP_WINDOW`** (strictly greater). [CODE]
- Hash uses `slice_get_unmapped_area` (slice.c:425-). It sets `high_limit = TASK_SIZE` when **`addr >= high_limit`** (slice.c:440-442), and grows the per-mm `slb_addr_limit` on first use, on every CPU (slice.c:455-462). [CODE]
- Consequence: a hint at *exactly* 2^47 is **relocated on Radix** and **honoured exactly on Hash**. [MEASURED] `vm-radix-64k.log` gives `hint 2^47 -> 0x7fffa59b0000 relocated`; `vm-hash-64k.log` gives `hint 2^47 -> 0x800000000000 EXACT`.
- Both MMUs allow `MAP_FIXED`/FNR at 2^47 or above, because of the `MAP_FIXED && addr+len > DMW` arm of `arch_get_mmap_end` and slice.c:441. [MEASURED]
- Neither MMU makes later no-hint mmaps go high after one high hint: the "after hint" line shows the next no-hint mmap back at 0x7fff…. [MEASURED] On Hash, `slb_addr_limit` stays raised for the life of the mm (slice.c:455-462) and is reset only at exec (slice.c:724). [CODE]

**The hard ceiling for `MAP_FIXED`** is `addr > TASK_SIZE - len` → ENOMEM (mm/mmap.c:858). `munmap` returns EINVAL for `start > TASK_SIZE` (mm/vma.c:1637). `mremap` returns EINVAL/ENOMEM for `new_addr > TASK_SIZE - new_len` (mm/mremap.c:1860-1868). [CODE]

**POWER8 hash.** `MMU_FTRS_POWER8` and `MMU_FTRS_POWER9` both equal `MMU_FTRS_POWER6`, which includes `MMU_FTR_68_BIT_VA` (asm/mmu.h:130-133). POWER8 therefore gets the full context range (mmu_context.c:46-49), the same 4PB/2^52 64K TASK_SIZE, and the same slice code as the POWER9 Hash guest that was measured. [SPEC] POWER8 identical to the measured POWER9-Hash rows. To confirm: run `va_probe` on POWER8 hardware, or boot a kernel not built with `-mcpu=power9` under TCG `-cpu power8` (§2.4).

### 1.2 arm64

| Symbol | Where | Value |
|---|---|---|
| `VA_BITS` | memory.h:43 | `CONFIG_ARM64_VA_BITS` ∈ {36 (16K, EXPERT), 39 (4K), 42 (64K), 47 (16K), 48, 52} (Kconfig:1454-1510; the choice default is 52, Kconfig:1447-1448) |
| `VA_BITS_MIN` | memory.h:56-64 | If VA_BITS > 48: **47 for 16K**, 48 otherwise. Else equal to VA_BITS. |
| `vabits_actual` | memory.h:235-240 | If VA_BITS > 48: read from TCR_EL1.T1SZ (52 if the CPU has LVA/LPA2, else 48). Else VA_BITS. |
| `DEFAULT_MAP_WINDOW_64` | processor.h:55 | `1 << VA_BITS_MIN` |
| `TASK_SIZE_64` | processor.h:56 | `1 << vabits_actual` |
| `STACK_TOP_MAX`, `TASK_UNMAPPED_BASE` | processor.h:80-86 | `DEFAULT_MAP_WINDOW_64`, `DEFAULT_MAP_WINDOW/4` (unless FORCE_52BIT) |
| `arch_get_mmap_end` | processor.h:97-98 | `addr > DEFAULT_MAP_WINDOW ? TASK_SIZE : DEFAULT_MAP_WINDOW` (strict >, **no MAP_FIXED arm**) |
| `arch_get_mmap_base` | processor.h:100-102 | Same shape as ppc |
| `ELF_ET_DYN_BASE` | asm/elf.h:130-134 | `2 * DEFAULT_MAP_WINDOW_64 / 3`: 0x5555_5555_4000-ish at 47 bits, 0xAAAA_AAAA_A000-ish at 48 bits |
| `STACK_RND_MASK` | asm/elf.h:195 | `0x3ffff >> (PAGE_SHIFT-12)` pages (1 GiB) |
| `ARCH_MMAP_RND_BITS_MIN/MAX` | Kconfig:296-310 | min 18/16/14 for 4K/16K/64K; max 24 (39-bit), 27 (42), 30 (47), 33/31/29 (48/52) |

**LVA rule.** A 52-bit userspace is visible only when `VA_BITS=52`, the CPU supports it, and a hint `> DEFAULT_MAP_WINDOW` is passed (processor.h:97-98). Without a hint the window stays at 2^48 (or 2^47 on 16K). [CODE] Kconfig:1473-1500 describes the same behaviour.

An arm64 16K kernel with VA_BITS=52 therefore behaves the same way as a ppc64 64K kernel: a 47-bit default window that a hint above 2^47 opens. [CODE] memory.h:57-59 against task_size_64.h:27-28.

**`MAP_FIXED` ceiling** is the same generic check against `TASK_SIZE` (mm/mmap.c:858). On a 48-bit kernel, FNR at 2^48 or above returns ENOMEM; on the Pi 5 (47-bit), FNR at 2^47 or above returns ENOMEM. [CODE] [MEASURED for 47]

### 1.3 `mmap_min_addr` and layout, both architectures

- `CONFIG_DEFAULT_MMAP_MIN_ADDR=4096` on both hosts, and the sysctl reads 4096 on both. [MEASURED] The Pi is `/boot/config-6.18.39+rpt-rpi-2712`; POWER is the `.config` of both 7.2.6 trees. In practice the lowest mappable page is the first page-size multiple: 0x4000 on the Pi, 0x10000 on 64K POWER. [SPEC]
- FNR at address 0 returns EPERM for non-root on every machine. [MEASURED] The first 4K guest run was root (CAP_SYS_RAWIO), so the control *mapped* page 0. The control caught this; `vm_init` now drops to uid 65534.
- **Top-down layout** (mm/util.c:434-478): `mmap_base = PAGE_ALIGN(STACK_TOP - gap - rnd)`, with the gap clamped to [128 MiB, 5/6·STACK_TOP]. `STACK_TOP` is 2^47 on the Pi 5 and on 64K POWER, 2^46 on 4K POWER, and 2^48 on a 48-bit arm64. [CODE]

| Layout, ASLR off (`setarch -R`) | Pi 5 arm64 16K/47 [MEASURED] | POWER9 64K [MEASURED] | POWER 4K (static probe, KVM) [MEASURED] | arm64 48-bit [SPEC] |
|---|---|---|---|---|
| PIE text | `0x555555550000` | `0x100000000` | non-PIE static | ≈`0xaaaaaaaa0000` |
| ld.so | `0x7ffff7fbc000` | `0x7ffff7f90000` | — | ≈`0xfffff7fxx000` |
| vdso | `0x7ffff7ff4000` | `0x7ffff7f80000` (vvar/vdso *below* ld.so) | `0x3fff…` | ≈`0xfffff7ffx000` |
| stack end | `0x800000000000` | `0x800000000000` | `0x3ffff…` (ASLR on) | `0x1000000000000` |
| first no-hint mmap | `0x7ffff7fb8000` | `0x7ffff7f30000` | `0x3fff…` | `0xffff…` |
| 1 TiB reservations until ENOMEM | 127 | 127 | 63 | ≈255 |

---

## 2. Measurements

### 2.1 Probe design and controls

`va_probe` maps one host page with (a) a plain hint and (b) `MAP_FIXED_NOREPLACE` at 2^46, 2^47−pg, 2^47, 2^47+pg, 2^48−pg, 2^48 and 2^52−pg. It writes a byte to prove each mapping is backed, then unmaps it. It then finds the ceiling with FNR at 2^b−16 pages for b = 40…56.

Controls (all must PASS; the binary exits 1 otherwise):
- **Must succeed:** FNR at 2^40, and FNR at 2^46−16 pages. These addresses are valid on every configuration measured.
- **Must fail:** FNR at 2^56, which is above every TASK_SIZE (ENOMEM); FNR at 0, which is below `mmap_min_addr` (EPERM); and FNR over a live mapping (EEXIST).

Every run below reports `controls_failed=0`, except the documented root run.

### 2.2 Results

| Config | Ceiling | No-hint top | hint 2^47−pg | hint 2^47 | FNR 2^47 | hint 2^47+pg / 2^48−pg | hint 2^48 | FNR 2^48 | hint/FNR 2^52−pg | Log |
|---|---|---|---|---|---|---|---|---|---|---|
| **Pi 5 arm64 16K VA47** | **2^47** | 0x7fff… | EXACT | relocated | ENOMEM | relocated / FNR ENOMEM | relocated | ENOMEM | relocated / ENOMEM | probe-pi5-arm64-16k-*.log [MEASURED] |
| arm64 4K/16K/64K VA48 | 2^48 | 0xffff… | EXACT | EXACT | EXACT | EXACT | relocated | ENOMEM | relocated / ENOMEM | [SPEC] processor.h:55-57, mmap.c:858 |
| arm64 VA52 + LVA CPU (4K/64K) | 2^52 | 0xffff… | EXACT | EXACT | EXACT | EXACT | relocated (not > 2^48) | EXACT | EXACT | [SPEC] processor.h:97-98 |
| arm64 4K VA39 (Pi `kernel8.img`) | 2^39 | 0x7f…… | relocated | relocated | ENOMEM | relocated/ENOMEM | relocated | ENOMEM | ENOMEM | [SPEC] |
| **POWER9 Radix 64K (bare metal)** | **2^52** | 0x7fff… | EXACT | **relocated** | **EXACT** | EXACT | EXACT | EXACT | EXACT | probe-p9-radix-64k-*.log [MEASURED] |
| POWER9 Radix 64K (KVM) | 2^52 | 0x7fff… | EXACT | relocated | EXACT | EXACT | EXACT | EXACT | EXACT | vm-radix-64k.log [MEASURED] |
| **POWER9 Hash 64K (KVM)** | 2^52 | 0x7fff… | EXACT | **EXACT** | EXACT | EXACT | EXACT | EXACT | EXACT | vm-hash-64k.log [MEASURED] |
| **POWER9 Radix 4K (KVM)** | **2^46** | 0x3fff… | relocated | relocated | ENOMEM | relocated / ENOMEM | relocated | ENOMEM | relocated / ENOMEM | vm-radix-4k.log [MEASURED] |
| **POWER9 Hash 4K (KVM)** | **2^46** | 0x3fff… | relocated | relocated | ENOMEM | relocated / ENOMEM | relocated | ENOMEM | relocated / ENOMEM | vm-hash-4k.log [MEASURED] |
| POWER8 Hash 64K / 4K | 2^52 / 2^46 | as Hash rows | | | | | | | | [SPEC] same code as the POWER9 Hash rows |

In every configuration a hint that is refused is *silently relocated*: the mmap succeeds somewhere else under the default window. It never fails. [MEASURED]

### 2.3 What the three arm64-vs-ppc64 divergences mean for an emulator that passes mmap through

On a 64K host, with no emulator intervention, a guest sees a 47-bit layout *except* for three things:
1. A hint above 2^47 is honoured instead of relocated. A guest that probes "is 2^47+x mappable?" concludes it has at least 48 bits, or 52.
2. FNR or `MAP_FIXED` from 2^47 up to 2^52 succeeds. Probers such as FEX's own `DetermineVASize`, TSan's layout checks and QEMU's `reserved_va` probe conclude 52 bits.
3. The PIE base is at 4 GiB rather than 2/3·TASK_SIZE. In POWERarm this is already replaced by an emulator-chosen ELF hint (§3).

On a 4K host the guest sees a strict 46-bit machine: a layout that no arm64 kernel ships. The closest real ones are 47 and 42.

### 2.4 What was not measured

- **POWER8 CPU.** Both host kernels are built `CONFIG_POWER9_CPU=y` / `TARGET_CPU="power9"`, so TCG `-cpu power8` takes a program check before Linux starts (`logs/vm-tcg-power8-hash-64k.log`). KVM `max-cpu-compat=power8` boots the kernel, but init dies with SIGILL, because the host's static glibc is POWER9-tuned (`logs/vm-p8compat-*.log`). To confirm: run `va_probe` on POWER8 hardware or with a POWER8-targeted distro kernel.
- **arm64 48-bit and 52-bit kernels.** No such kernel or hardware was available: the Pi 5 is VA47, and there is no `qemu-system-aarch64` on either machine. These rows are [SPEC] from source. To confirm: run `va_probe` on any Debian, Ubuntu or Fedora 4K arm64 box, or on Graviton or Ampere.

---

## 3. How FEX handles this, and what the ppc64le fork changed

### 3.1 Upstream FEX mechanism (still present verbatim in POWERarm)

- **Host VA probe.** `DetermineVASize()` (FEXCore/Source/Utils/Allocator.cpp:121-162) tries FNR at the top 64 pages below 2^57, 2^52, 2^48, 2^47, 2^42, 2^39 and 2^36 (list at :126-128). The first hit wins. The comment explains why: "We can't actually determine VA size on ARM safely" (:133). [CODE]
- **Hiding bit 47 from the guest.** `FEX::Allocator::InitMemoryRegions(true)` → `Setup48BitAllocatorIfExists()` (FEXInterpreter.cpp:159-163 → Allocator.cpp:293-307). If the host has 48 bits or more, it *steals* all of **[0x8000_0000_0000, 0x1_0000_0000_0000)**. `StealMemoryRegion()` (:244-291) parses `/proc/self/maps` (`CollectMemoryGaps`, :166-242) and maps every gap `PROT_NONE | MAP_NORESERVE | MAP_FIXED_NOREPLACE`. The stolen ranges then become the arena for `OSAllocator_64Bit` (`Create64BitAllocatorWithRegions`), and all of FEX's own mmap/munmap hooks route there (`AssignHookOverrides`, :100-105). [CODE]
- **Why that works for x86-64 guests on arm64 hosts.** Once bit 47 is fully occupied, the host kernel cannot satisfy any guest request there. Guest no-hint mmaps, which are passed straight through (`SyscallHandler::GuestMmap`, SyscallsSMCTracking.cpp:2150-, `::mmap` at ~:2210), land below 2^47. So do relocated hints. A guest `MAP_FIXED_NOREPLACE` into the stolen range fails with EEXIST. [CODE] A plain guest `MAP_FIXED` there *would* succeed and destroy FEX memory; upstream accepts that risk.
- **Guest-visible layout.** The guest stack is hinted at `2^min(VASize,47) − 128 MiB` (ELFCodeLoader.h:608-614). The interpreter load hint is `min(2^VASize, 2^47)/3·2` plus 28 bits of ASLR (:706-712). This reproduces the x86-64 kernel layout. [CODE]
- **32-bit guests** get a separate bitmap allocator (`Create32BitAllocator`, FEXInterpreter.cpp:185-192) and a stolen [4 GiB, 8 GiB) safety net (:166-169). [CODE]
- **`/proc/self/maps`** is passed through from the host upstream. The stolen `PROT_NONE` ranges and FEX's own mappings are visible to the guest. [CODE] Inferred from EmulatedFiles.cpp:161-193 falling through when the content is empty.

### 3.2 What the ppc64le fork (FastPPCx86 → POWERarm) changed

| Change | Where | Effect on VA |
|---|---|---|
| The probe steps in host pages instead of 4096 | Allocator.cpp:138-143, commit c5ec09b31 | On 64K the probe works; it returns **52**. [MEASURED with port] |
| The 64-bit steal allocator is made host-page-granular | 64BitAllocator.cpp (c5ec09b31) | The [2^47, 2^48) steal really engages on 64K hosts. The commit notes it "never engages on a 4K ppc64le kernel". [CODE] |
| Internal placement hint: FEX's own reservations (rpmalloc arenas, LookupCache, JIT CodeBuffers, fextl pools) are bump-allocated through a hint window of **[1 TiB, 32 TiB)** | AllocatorHooks.cpp:26-67 (`INTERNAL_ARENA_BASE/SIZE` :48-49), applied in `FEX_default_mmap` :95-100 and VirtualAlloc; commits aa92883d6 and 2205890e2 | This was motivated by Unity indexing allocations by `addr>>32` on a POWER8 host. On **4K** hosts it is the only thing keeping FEX's memory apart from the guest's, and both still share the guest's 46-bit space. On **64K** hosts the hint is fed to `OSAllocator_64Bit::Mmap`, which ignores a hint outside its regions (64BitAllocator.cpp:270-282, :392-420), so FEX memory lands in [2^47, 2^48). [CODE] |
| `HostOwnedRanges`: a snapshot of every mapping taken before the guest loads; guest `MAP_FIXED`, `munmap`, `mprotect` and `mremap` over one returns ENOMEM | HostOwnedRanges.h:17-80, FEXInterpreter.cpp:640-646, SyscallsSMCTracking.cpp:~2196-2204 | Closes upstream's plain-`MAP_FIXED` hole for the stolen range and FEX's PIE image. FEX's image sits at 4-5 GiB because ppc `ELF_ET_DYN_BASE` = 4 GiB. [CODE] |
| Synthesised `/proc/self/maps` and `smaps` from VMATracking and the granule table | EmulatedFiles.cpp:161-191, commit a2e43c82b | **64K only.** It shows guest mappings only, so the stolen [2^47, 2^48) range and host mappings are hidden. On 4K it returns empty and falls through to the real file, which shows FEX. [CODE] |
| AArch64 guest syscall table | LinuxSyscalls/Arm64/Syscalls.h:35 | Guest mmap still calls `GuestMmap(true, …)`: passthrough with no VA clamp. [CODE] |

### 3.3 Defects and hazards this implies for an AArch64 guest

1. **The 4K host is probed as 42 bits.** The candidate list {57,52,48,47,42,39,36} has no 46. On a 4K ppc64 kernel (ceiling 2^46) the probe falls through to 42. [MEASURED] `fex_vasize_probe` in `vm-{radix,hash}-4k-fexport.log` prints `DetermineVASize=42`. Consequences [CODE → MEASURED values]:
   - The guest stack hint becomes `0x3fff8000000` (≈4 TiB) and the interpreter hint `0x2aaaaaaaaaa` (≈2.9 TiB). Both are **inside** FEX's own internal hint window [1 TiB, 32 TiB).
   - These are hints, so collisions relocate instead of corrupting. But the layout (stack at 4 TiB, kernel mmap_base at about 64 TiB) matches no real arm64 kernel.
   - It defeats the stated purpose of the internal window, which is to stay "clear of the guest's executable/brk".
   - It also makes `VDSO_Emulation.cpp:665` see 42.
2. **On a 64K host the guest can still reach [2^48, 2^52).** Nothing is stolen above 2^48. An arm64-guest hint at 2^48 or above is honoured, and FNR or `MAP_FIXED` there succeeds. A real 47/48-bit arm64 kernel would relocate or return ENOMEM. A guest-side probe (TSan, QEMU-style, jemalloc runtime checks, FEX-in-guest) then concludes it has a 52-bit VA. [MEASURED] Host behaviour in `probe-p9-radix-64k-aslr.log`. [SPEC] The guest-visible effect through POWERarm.
3. **A guest hint in [2^47, 2^48) on 64K** is relocated below 2^47 because the stolen range occupies it. That matches a 47-bit arm64. `MAP_FIXED` there returns ENOMEM through HostOwnedRanges, which also matches. FNR there returns **EEXIST** (the kernel sees the `PROT_NONE` steal), where arm64-47 returns **ENOMEM**. This is a small errno-parity gap. [SPEC] Derived from SyscallsSMCTracking.cpp:~2196 and the measured host behaviour.
4. **Hash hosts pay for the steal.** Mapping [2^47, 2^48) raises `slb_addr_limit` to TASK_SIZE for every POWERarm process (slice.c:455-462) and spreads FEX memory over more 1T segments. It stays inside the first 512 TB context (`TASK_CONTEXT_SIZE`, task_size_64.h:29), so no extended context id is allocated. [SPEC] The SLB-miss cost on POWER8 is unmeasured.

---

## 4. Real aarch64 software that depends on VA width

Source: `web-findings.md`, gathered by a delegated web-research pass. Each entry there cites a URL and file:line or commit. Tags: [DOC] means read in upstream source or a commit; [INFERENCE] means reasoned, not run. One high-severity claim was re-checked here. [DOC, re-verified 2026-09-16] On llvm-project `main`, `compiler-rt/lib/tsan/rtl/tsan_platform_linux.cpp` accepts aarch64 VMA sizes {39, 42, 47, 48} for C/C++ and {47, 48} for Go.

**How software detects VA width.** No arm64 syscall reports VA size, so runtimes infer it from one of four signals. The emulator has to keep all four consistent with each other.
1. **Most significant bit of the stack address:** TSan, ASan, HWASan, MSan (sanitizer `GetMaxUserVirtualAddress`).
2. **`MAP_FIXED_NOREPLACE` at 2^n−page:** .NET main (ca72cbf5), and FEX itself.
3. **Whether hints are honoured:** SpiderMonkey `FindAddressLimit`, LuaJIT `mmap_probe`, ZGC fallback.
4. **`msync` errno:** ZGC `probe_valid_max_address_bit`.

| Program | Assumption on aarch64 | Probe or hardcode | Guest VA = 47 | Guest VA = 46 (4K host) [INFERENCE] | Needs 48? |
|---|---|---|---|---|---|
| **TSan C/C++**, released LLVM ≤ 23.1.1 and GCC libtsan | Fixed shadow layouts for 39/42/48 | Stack MSB | **FATAL "unsupported VMA range … Found 47"** [DOC] | FATAL (46 is never supported) | **Yes**, or a 42/39-bit guest |
| TSan C/C++, LLVM main (f020acfd, 2026-07-16) | Adds `MappingAarch64_47` | Stack MSB | Works *if* the layout is coherent: PIE ~0x5555…, stack and libs ~0x7c00…–0x8000… [DOC] | FATAL | No |
| **Go `-race`** (prebuilt `race_linux_arm64.syso`) | 48 only; 47 accepted on LLVM main from 2930d9f4 (2026-09-03) | Stack MSB | **FATAL "Found 47 - Supported 48"** with shipped runtimes [DOC] | FATAL | **Yes** |
| **MSan** | Single 48-bit layout; allocator `MAP_FIXED` at 0xE000_0000_0000 | Hardcoded | **Fails**: PIE at 0x5555… falls in shadow, it re-execs, then dies [INFERENCE from DOC] | Fails | **Yes** |
| ASan | Shadow offset 1<<36; HighMemEnd from stack MSB | Probe | Works [DOC/INFERENCE] | Works | No |
| HWASan | Dynamic shadow; needs TBI and `PR_SET_TAGGED_ADDR_CTRL` | Probe | Works for VA; TBI emulation is a separate requirement | Works | No |
| **V8 (Node, Chromium)** | `kDefaultVirtualAddressBits=48`, halved to 128 TB; sandbox reservation ≈1.35 TB; arm64 hints masked to 2^38 | Hardcoded | No change: it already assumes 47 [DOC] | Works (sandbox limit/4 = 16 TB, still > 1 TB) | No |
| **LuaJIT GC64** | GC objects and lightuserdata < 2^47 (`LJ_GCVMASK`, `LJ_ALLOC_MBITS 47`) | Probe: retries hinted mmaps | Works, and better than native: old builds that die with "bad light userdata pointer" on 48-bit arm64 work at 47 [DOC] | Works | **No: 48 is actively worse** |
| **OpenJDK compressed oops and class space** | Searches capped at 128 TB; aarch64 EOR klass bases ≤ 0x7fff<<32 | Hardcoded ≤ 2^47, attaches probed | Works [DOC] | Works (bases above 64 TB fail, then "anywhere") | No |
| **OpenJDK ZGC** | Heap bits probed from 46 down | Probe (`msync`, then mmap) | Same answer as native [DOC]. Requires faithful `msync` ENOMEM | Probes 45 down, so a smaller max heap-address range | No |
| **Go runtime** (without `-race`) | `heapAddrBits=48`; arm64 hints `i<<40 \| 0x40<<32`, all < 2^47; heap-base randomisation uses 47 bits | Hardcoded hints, then "anywhere" | Works [DOC] | Hints above 64 TB fail and fall back; works | No |
| jemalloc | `LG_VADDR=48` fixed at configure time | Hardcoded | Works; breaks only at 2^48 or above (a 52-bit guest) [DOC] | Works | No (52 is harmful) |
| mimalloc v2/v3 | `MI_MAX_VABITS=48`; hints 2–30 TiB, huge pages from 32 TiB | Hardcoded | Works [DOC] | Works | No |
| **SpiderMonkey** | `JSVAL_TAG_SHIFT 47`; `FindAddressLimit` caps at 0x7fff_ffff_ffff | Probe | Works; takes the more robust `< 48` fallback [DOC] | Works (43 bits or more for random allocation) | No |
| .NET CoreCLR | ≤ .NET 10: constant 2^47. main: FNR probe of 52/48/47/42/39/36 | Hardcode, now probe | Works. **On an unclamped 64K host, the probe reports 52** [DOC] | Probe → 42 on main (46 isn't a candidate); only GC sizing changes | No |
| Mono | No dependency found | — | Unverified | Unverified | — |
| Wasmtime | Docs assume 47 bits for pooling capacity (32k × 4 GiB slots) | Sizes hardcoded, OS picks addresses | Works [DOC] | Pooling capacity halves | No |
| JSC / bmalloc (Bun, WebKitGTK) | `EFFECTIVE_ADDRESS_WIDTH=48` on non-Darwin | Hardcoded | Works; breaks at 2^48 or above | Works | No |
| PartitionAlloc | ASLR mask 38 bits + 64 GB offset | Hardcoded low | Works | Works | No |
| **QEMU linux-user** (reference) | `TARGET_VIRT_ADDR_SPACE_BITS 52`; `reserved_va=0` by default for 64-bit guests; ELF_ET_DYN_BASE 2/3·2^48 | Inherits host | Its aarch64 guests on x86-64 hosts *are* 47-bit guests in practice; LLVM moved a TSan Go/s390x region below 2^47 for QEMU (7376a706) [DOC] | — | — |

**Takeaways.**
1. Among JIT/GC runtimes, none *needs* 48 bits. Several (LuaJIT, SpiderMonkey, JSC, jemalloc) are safer when no pointer is at 2^48 or above, and LuaJIT and SpiderMonkey are safer below 2^47.
2. The only hard 48-bit dependents are **MSan** and **released TSan, including Go `-race`**. TSan on LLVM main also accepts 47.
3. **46 bits (4K host) breaks every TSan and MSan** and nothing else found.
4. An **inconsistent** layout is worse than either width, for example a stack above 2^47 with mmap below, or FNR probes succeeding above the stack-derived width. TSan rejects mixed layouts with "unexpected memory mapping" [DOC]. .NET and FEX-style probers would size themselves for 52 bits.


---

## 5. Options for POWERarm

The constraints every option must respect:

| Host | Largest guest VA possible | Room for POWERarm outside the guest range |
|---|---|---|
| 64K (Radix POWER9, Hash POWER8/9) | 2^52 [MEASURED] | Plenty if the guest is 47- or 48-bit |
| 4K (Radix or Hash) | **2^46** [MEASURED] | **None.** POWERarm, its JIT caches and the guest share 64 TiB |

### (a) Present a 47-bit guest (46-bit on 4K) and let software adapt

- **Correctness:** matches a real, shipping arm64 configuration (16K VA_BITS=47: Pi 5, and the Asahi-style 16K class) byte for byte on 64K hosts, *provided* the three divergences in §2.3 are closed (see d). Software that can't run at 47 bits already fails on Pi 5 and Apple-silicon Linux, so upstreams have fixes or workarounds (§4). At 46 bits on 4K there is no arm64 twin. Programs that decode the layout from the stack address (TSan, some sanitizer builds) may reject it; adaptive programs will not (§4).
- **Cost:** nearly free. No-hint guest mmaps stay native kernel calls; the host default window *is* the guest window on 64K.
- **Hash/POWER8:** no extra SLB footprint for guest memory; FEX's own memory is the only thing above 2^47.
- **JIT/allocator:** on 64K it already works: POWERarm memory sits in [2^47, 2^48). On 4K it relies on the internal hint window, which must be fixed to key off 46 (§3.3.1).

### (b) Offer a true 48-bit guest on 64K by hinting internally

- **Mechanism:** stack top at 2^48, interpreter at 2/3·2^48, and no-hint guest mmaps placed top-down below about 2^48. The kernel won't do that last part: a hint above the window makes it search from `base + TASK_SIZE - DMW`, near 4 PB (task_size_64.h:75-76). POWERarm would have to run its own VA allocator for every guest mmap, like FEX's 32-bit bitmap allocator but over 2^48, or keep a steal of [2^47, 2^48) and hand out hints into it.
- **Correctness:** gains the Debian, Ubuntu, Fedora and Graviton layout. It also invites exactly the pointers that NaN-boxing engines (SpiderMonkey, LuaJIT non-GC64 histories, JSC) must work to avoid (§4).
- **Cost:** a lock and bitmap search on every mmap, `munmap` and `mremap`; fragmentation; `MAP_FIXED` bookkeeping; POWERarm's memory must move to 2^48 or above.
- **Hash:** guest memory sprawls over [2^47, 2^48) segments. More SLB pressure on POWER8's 32-entry SLB [SPEC]. Unmeasured.
- **4K:** impossible.

### (c) Reserve [2^47, 2^48) and keep POWERarm allocations out of guest-visible ranges

- This is what the tree already does on 64K, inherited from FEX. It is necessary but not sufficient: it doesn't cover [2^48, 2^52) (§3.3.2), and it gives the wrong FNR errno in [2^47, 2^48).
- **Better variant for the recommended default:** put POWERarm's own arena at **[2^48, 2^49)** (or above) and leave [2^47, 2^48) *unmapped but refused* by the syscall layer. Then a future opt-in 48-bit mode (b/e) needs no allocator move, and the hash `slb_addr_limit` is raised either way.
- **Cost:** one parse of `/proc/self/maps` at start-up (already done). No per-syscall cost.
- **4K:** there is nowhere to reserve. POWERarm must live inside the guest's 46 bits: keep the internal window, but place it where the guest isn't (see §6).

### (d) Translate or clamp guest mmap hints above the guest limit

This is required for (a). Emulate arm64 `mm/mmap.c` semantics against a *guest* `TASK_SIZE` G (2^47 on 64K, 2^46 on 4K) and a *guest* window W (= G unless LVA is enabled):

- `mmap` without FIXED and `addr + len > W` (and `addr <= W` for LVA semantics): pass `addr = NULL`, so the kernel's default window gives the same relocation arm64 does.
- `MAP_FIXED` or `MAP_FIXED_NOREPLACE` with `addr + len > G` (or `len > G`): return **-ENOMEM** without calling the host.
- `mremap(MREMAP_FIXED)` with `new_addr > G - new_len`: -EINVAL. `munmap` with `start > G`: -EINVAL. `mprotect` beyond: -ENOMEM. `shmat` at an address above G: -EINVAL. `brk` is already emulator-managed.
- **Radix/hash boundary:** because the clamp happens before the host call, the host's `>` vs `>=` quirk at exactly 2^47 disappears. A guest hint at 2^47 gets NULL on both, which matches arm64-47.
- **Cost:** two compares per mmap-family syscall. Negligible.
- **Risk:** none for guests. A guest can't tell a clamp from a real 47-bit kernel.

### (e) Per-app config knob

`FEX_GUEST_VA_BITS` / AppConfig `GuestVABits` ∈ {39, 42, 46, 47, 48, 52}, clamped to what the host allows:
- **39/42:** lower G and W. Reproduces Android and Pi 4K layouts for sanitizer-built or VA-fragile binaries. Costs nothing.
- **48:** requires option (b)'s allocator. Only on 64K.
- **52:** "LVA" mode. W = 2^48 (or 2^47), G = 2^52. Hints above W honoured, which the host already does natively on 64K. POWERarm's arena must then sit where the guest can't reach it, or be protected by HostOwnedRanges. Only on 64K.

A knob is cheap for 39/42/46/47/52 because they are all "clamp at a different G and W". 48 is the only expensive one.

---

## 6. Recommendation

### Default

1. **Guest VA = 47 bits on 64K hosts, 46 bits on 4K hosts,** reported identically on Radix and Hash. Model it on the arm64 16K `VA_BITS=47` kernel:
   - stack top `2^G`
   - interpreter and PIE hint `2·2^G/3`
   - arm64 ASLR widths (mmap 18 bits × 4K granule for a 4K guest; stack mask `0x3ffff` pages)
   - everything else from the host kernel's own default window, which already equals the guest window on 64K.
2. **Clamp in the syscall layer (option d)** for `mmap`, `mremap`, `munmap`, `mprotect` and `shmat`, against guest G. Arm64 errnos: ENOMEM for fixed mappings, EINVAL for munmap/mremap. It must be in place before the guest's first mmap, including the ELF loader's own `GuestMmap` calls.
3. **Fix `DetermineVASize`**: add 46 to the list (and 44/43 for completeness), or better, probe by binary search on FNR. Derive G = min(host, 47) from it and use G everywhere `VASize`/`TASK_SIZE_64` is used today:
   - ELFCodeLoader.h:608-614, :706-712
   - VDSO_Emulation.cpp:665
   - `Setup48BitAllocatorIfExists`
4. **POWERarm-owned memory above G on 64K.** Move the steal arena to start at or above 2^48 (keeping [2^47, 2^48) guest-refused by the clamp). Add the arena to HostOwnedRanges; that already happens by snapshot. Keep the synthesised `/proc/self/maps` so the guest never sees it.
5. **On 4K**, POWERarm can't leave the guest's range, so keep a fixed POWERarm slab inside it, away from the guest layout:
   - Guest layout at G=46: ELF/interp hint ≈ 2/3·2^46 ≈ 42.7 TiB, stack at 64 TiB, kernel top-down mmap area just below the stack.
   - Keep POWERarm's internal window low, at [1 TiB, 16 TiB).
   - After start-up, steal what is left of that slab (`StealMemoryRegion`), so the kernel's top-down search for guest no-hint mmaps never enters it. Today it only relies on the guest never reaching it.
   - Turn on the synthesised `/proc/self/maps` on 4K too, so guests that parse maps (V8, Go, JVM, sanitizers) don't see tens of GiB of foreign reservations.

### Fallbacks

- **Per-app `GuestVABits` knob** (option e):
  - **48, 64K hosts only:** for MSan, released-LLVM/GCC TSan and Go `-race`. These are the only programs in §4 that need 48 bits. Implement option (b):
    - stack top 2^48, PIE and interpreter at 2/3·2^48, guest no-hint mmaps placed by a POWERarm VA allocator below 2^48;
    - POWERarm's arena at 2^48 or above (which the default already puts there, so no move);
    - clamp above 2^48.
    Measure Hash SLB cost on POWER8 before enabling it anywhere by default (parity before timing).
  - **42 or 39, any host, including 4K:** lets TSan C/C++ run on 4K hosts, where 46 has no TSan layout. Only a clamp is needed.
  - **52 (LVA), 64K only:** for the rare software that hints above 2^48 on purpose. It hurts jemalloc and JSC, so never make it the default.
- **4K hosts:** MSan and Go `-race` have no working configuration (they need 48 bits). Report that clearly: a start-up warning when the guest binary links `__tsan_init`/`__msan_init` and the guest VA is unsupported.
- If a program turns out to need 48 bits and isn't a sanitizer, add it to the AppConfig list for `GuestVABits=48` rather than changing the default. Defaulting to 48 would make LuaJIT, SpiderMonkey and older JSC builds worse.

### Probes and tests that should become POWERarm regression tests

| Test | Kind | Pass criterion |
|---|---|---|
| `va_probe` built for aarch64 (static), run under POWERarm with `setarch -R` | Parity against `logs/probe-pi5-arm64-16k-noaslr.log` on 64K hosts | Same ceiling (2^47), same EXACT/relocated/ENOMEM pattern for every hint and FNR row, stack end `0x800000000000`, all 5 controls PASS. Addresses may differ only by host granule rounding. |
| Same on a 4K host | Parity against a *synthetic* 46-bit golden (the §2.2 4K rows) | Ceiling 2^46, controls PASS |
| `va_probe` natively on each host kernel (Radix/Hash × 4K/64K; KVM guest recipe in §2 via `vm_init.c`) | Host fact check, run when kernels change | Matches §2.2. Catches a kernel that changes `DEFAULT_MAP_WINDOW` or the hash `>=` quirk. |
| `fex_vasize_probe`, or a unit test in `FEXCore/unittests/APITests/Allocator.cpp` | Unit | `DetermineVASize()` returns 52 on 64K and **46** on 4K (today: 42) |
| Guest-side "no foreign VMAs below G" | Integration | The host's real `/proc/<pid>/maps` shows no `FEXMem*`, `FEXAllocator` or JIT VMA in [0, G) on 64K. On 4K, all such VMAs sit inside the declared POWERarm slab. |
| Guest `MAP_FIXED` / FNR / `mremap` / `munmap` above G | Integration | ENOMEM / ENOMEM / EINVAL / EINVAL: arm64 errnos, not EEXIST |
| Negative control: the clamp disabled by a debug knob | Proves the test can fail | The ceiling row reports 2^52 on a 64K host |
| Consumer smoke tests at G=47 (and 46 on 4K) | Soak | LuaJIT GC64 `jit.on` hello; `node -e` (V8 cage and sandbox up); `java -XX:+UseZGC -version` and `-XX:+UseCompressedOops`; Go hello with a 64 GiB `make`; `clang -fsanitize=address` hello; `-fsanitize=thread` hello (expected: FATAL at 47 with released LLVM, and passes under `GuestVABits=48`; this pair is the positive and negative control for the knob); mimalloc and jemalloc test binaries |

---

## Appendix: reproduction

```
# arm64 (Pi):          gcc -O1 -pie -fPIE -o va_probe va_probe.c && ./va_probe && setarch -R ./va_probe
# POWER host:          same, plus  gcc -O1 -o fex_vasize_probe fex_vasize_probe.c
# POWER KVM guests (no root needed, /dev/kvm is 0666 on the host):
gcc -O1 -static -o root/init vm_init.c; gcc -O1 -static -o root/va_probe va_probe.c
gcc -O1 -static -o root/fex_vasize_probe fex_vasize_probe.c
(cd root && find . | cpio -o -H newc > ../probe.cpio)
qemu-system-ppc64 -M pseries,accel=kvm -cpu host -m 2G -nographic -nodefaults -serial stdio \
  -kernel /boot/vmlinuz-linux-power9[-64k] -initrd probe.cpio -append "console=hvc0 quiet [disable_radix]"
```
