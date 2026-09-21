# Host-native thunks for POWERarm: AArch64 guest, ppc64le host

Written 2026-09-17. Design only: nothing here was built, measured or committed. Every `file:line`
was read in this session at commit `aaee4e554`; anything not read is marked **[unverified]**.
Facts taken from the ArchLinuxARM guest rootfs are marked **[guest view]**: this session's shell
runs under POWERarm, so `/usr`, `/lib` and `pacman` show the *guest* rootfs, never the host.
Host facts are questions for Jordan (§8).

Owner context: `DESIGN.md` §6 (thunk plan), `HANDOVER.md` open item 7. P7 (fence weakening)
measured at ≤ ~5% on a V8 workload (coordinator, 2026-09-17), so thunks are the lead lever.

## 0. Recommendation in one screen

- **Shape.** Keep FEX's thunk pipeline (guest packer → one crossing → host unpacker, and the
  reverse for callbacks). The host half is already ppc64le-native and exercised: the
  host→guest trampoline template, its TLS side channel and the JIT's `DEF_OP(Thunk)` were all
  written for this host (`Source/Tools/LinuxEmulation/Thunks.cpp:63-151`,
  `FEXCore/Source/Interface/Core/JIT/PPC64LE/BranchOps.cpp:1678-1743`). What is x86 is the
  **guest side**: five places, listed in §1.2. Replace them with an AArch64 guest ABI built on
  `HLT #imm16` (unallocated in the A64 decoder today: `A64Frontend/a64.inc:49` is commented
  out, so it currently SIGILLs), X30-based callback return, and X16/X17 for the linked-callee
  register.
- **First landing (Stage 0, §7):** the AArch64 guest-thunk ABI in the core plus **`libVDSO`** as
  the first guest stub. It is the smallest thing that exercises decode → `Thunk` op → host
  function → return and callback-return, it is deterministic (Pi goldens), and it is valuable on
  its own: `clock_gettime`/`gettimeofday` stop being syscalls, which V8 (code-server, Claude
  CLI) calls constantly. `VDSO_Emulation.cpp:660-680` already expects an AArch64 ELF at
  `GuestThunks/libVDSO-guest.so`; only the stub's x86 bytes and the CALLBACKRET encoding
  (`VDSO_Emulation.cpp:617-626`) are missing.
- **Then Vulkan + wayland-client (+ drm, xshmfence)** for Hyprland, gated on vkcube and vkmark
  native-vs-thunked (§6). EGL/GL after, because the GL host half is GLX-centric and the EGL
  interface has no Wayland platform entry points (§3.4).
- **libc string routines:** not a library thunk (agreed with `DESIGN.md` §6.3). A **symbol-level
  redirect of `memcpy`/`memmove`/`memset` only**, implemented as a custom IR entrypoint with a
  size gate, on a separate track. `strlen`/`strcmp`/`memchr` stay with the JIT (N1); the
  crossing cost exceeds the work for the short strings real programs pass (§4).
- **Later:** zlib → `libnxz` is worth a look once `/dev/crypto/nx-gzip` is usable unprivileged;
  OpenSSL is low value (§5).

## 1. What exists, what is missing

### 1.1 The pipeline as it stands (verified)

`ThunkLibs/README.md` describes the flow; the code matches it:

| Step | Where | State on this fork |
|---|---|---|
| Guest packer packs args into a `PackedArguments` struct on the guest stack | `ThunkLibs/include/common/PackedArguments.h:7-31` (`__attribute__((packed))`, members in declaration order, `rv` last) | arch-neutral |
| Guest thunk = `0F 3F` + 32-byte SHA256 of `lib:function` | `ThunkLibs/include/common/Guest.h:22-30`; the `ARCHITECTURE_arm64` branch (`:33-45`) is a deliberate `BROKEN_INSTALL` stub | **x86** |
| Frontend decodes the marker into `IR::Thunk` | `FEXCore/Source/Interface/IR/IR.json:351` defines `Thunk GPR:$ArgPtr, SHA256Sum:$ThunkNameHash`; **no A64 emitter exists** (grep `_Thunk(` over `FEXCore/Source` finds no caller; `grep -ri thunk A64Frontend/` is empty) | **missing** |
| JIT lowers `Thunk`: spill, `r3 = ArgPtr`, `r4 = CpuStateFrame`, TOC save, `bctrl`, TOC restore, sentinel-checked refill | `BranchOps.cpp:1678-1743` | ppc64le, done |
| Host lookup by hash, `dlopen("<name>-host.so")`, `fexthunks_exports_<name>()` | `Thunks.cpp:249-260, 312-375` | done |
| Host→guest callback trampoline (84-byte instances, I-cache flushed) | `Thunks.cpp:43-125, 400-482` | ppc64le, done |
| Guest re-entry for callbacks | `Core.cpp:707-709` → `PPC64Dispatcher.cpp:963-1026` (`CallbackPtr`) | **x86 return convention** |
| Path overlay of guest sonames onto stubs | `Data/ThunksDB.json`; `FileManagement.cpp:54-101` (prefix table, Arch `lib` at `:94-100`), `:550-568` (`GetEmulatedPath`) | multiarch test already spelled `aarch64-linux-gnu` (`:61`) |
| Config keys `ThunkHostLibs`, `ThunkGuestLibs`, `ThunkConfig` | `FEXCore/Source/Interface/Config/Config.json.in:212-238` | done |
| Host libs built: asound, xshmfence, vulkan, wayland-client, drm, EGL, GL (SDL2 and fex_malloc commented out) | `ThunkLibs/HostLibs/CMakeLists.txt:171-210` | done for ppc64le |

### 1.2 The five x86 residues on the guest side

1. **Guest thunk marker.** `Guest.h:22-30` emits `.byte 0xF, 0x3F` + hash and *no return
   instruction*: on x86 the frontend's thunk handling returned for it. Proposal: `HLT #0x0F3F`
   followed by the 32-byte hash; the A64 frontend emits `_Thunk(X0, hash)` then
   `ExitFunction(X30)` (the stub is entered by `bl`, so X30 is the return). `HLT` is
   unallocated today (`a64.inc:49`), so nothing changes for programs that do not carry the
   marker. Decision for Jordan (§8): recognise the marker everywhere, or only inside mappings
   of files under `GuestThunks/` and the vDSO page (keeps `HLT` SIGILL parity with the Pi
   for everything else; `unittests/A64Frontend/sigill_udf.S` shows the pattern).
2. **Callback return.** `PPC64Dispatcher.cpp:1006-1016` pushes `ThunkCallbackRet` on the guest
   stack; `BranchOps.cpp:87-109` (`CallbackReturn`) adds 8 to SP; `JIT.cpp:1923` walks the
   guest stack for the sentinel (per `M0-CENSUS.md:375`, marker list; not re-read). AArch64:
   `State.x[30] = ThunkCallbackRet`, no SP change, and the sentinel becomes an X30 compare.
   The CALLBACKRET page is still `0F 3E` (`VDSO_Emulation.cpp:617-626`, marked TODO);
   make it `HLT #0x0F3E`, decoded to `IR::CallbackReturn` (`IR.json:341`).
3. **Linked-callee register.** `Core.cpp:1626-1654` (`AddThunkTrampolineIRHandler`) stores
   the original host callee into GPR 16 and `ExitFunction`s to the guest invoker; the invoker
   reads it via a register variable (`Guest.h:124-169`, x86 `r11`). X16/X17 (IP0/IP1) are the
   right class: no caller may assume they survive a call. Write both, read X17 with the same
   `register … asm("x17")` + empty-asm trick, and add a build check that the invoker's
   prologue does not clobber it before the read (FEX did this by hand for r11, `Guest.h:141-147`).
4. **Generator guest model.** `ThunkLibs/Generator/main.cpp:98-111` parses the guest layout with
   `--target=x86_64-linux-gnu` against an x86 sysroot. Switch to `aarch64-linux-gnu` and point
   the sysroot at the ArchLinuxARM rootfs itself (`~/.local/share/powerarm/RootFS/…`, glibc
   2.43 and GCC 16 headers per `Scripts/powerarm/rootfs/README.md`): unlike the x86 case, the
   guest headers are already on the box. `data_layout.cpp:270-273` keys on
   `guest_abi.pointer_size == 4`, so the 64-bit path is what runs.
5. **Guest stub build.** Removed in M0a (`README.md` POWERarm note; `cb4008188`). Rebuild it
   for aarch64: either natively on the Pi or with Arch's `aarch64-linux-gnu-gcc` cross
   package (`DESIGN.md` §6.2a mentions the cross glibc **[unverified that the cross gcc is
   installed]**). Stubs are versioned with the emulator and live outside the rootfs
   (`DESIGN.md` §6.2a).

Two more items that are not x86 residues but need doing: the guest-stack bump allocator sets
`SP == 8 (mod 16)` for x86 callbacks (`Host.h:707-731`); AAPCS64 requires `SP == 0 (mod 16)`
at every call, so the residue must change with the guest. And `ReserveLow32HostRange`
(`Thunks.cpp:642-646`) is a dead 32-bit hook to delete.

### 1.3 ABI differences that matter, and which ones cross

What crosses the boundary is **data layout**, never a calling convention: each side's own
compiler builds the packer/unpacker, and only the packed struct pointer is handed over
(`BranchOps.cpp:1713`, `r3 = ArgPtr`). So AAPCS64 HFAs in V registers, X8 struct return, ELFv2's
parameter save area and TOC protocol never meet each other. What does cross is the byte
layout of every parameter type and every pointed-to struct. Both sides are LP64 little-endian
with unsigned `char`, so the table is short:

| Type | AArch64 (guest) | ppc64le (host) | Crosses the proposed APIs? |
|---|---|---|---|
| `long double` | IEEE binary128 | **IBM double-double, measured** (`DESIGN.md:383-397`: GCC 16 `-mabi=ibmlongdouble`, `LDBL_MANT_DIG` 106; glibc exports both `printf@@GLIBC_2.17` and `__printfieee128`) | Not in Vulkan, GL, EGL, wayland-client, drm, xshmfence, VDSO, zlib. **The generator does not refuse it today**: no `long double` handling in `Generator/*.cpp` (grep). Add the refusal before the aarch64 generator lands. If Arch POWER ever moves to `-mabi=ieeelongdouble`, the representation matches and it becomes a plain 16-byte pass-through; that is the open question in §8 |
| `va_list` | 32-byte struct | `char *` | Never marshallable. Variadic *functions* are handled by `uniform_va_type` + forced `custom_host_impl` (`analysis.cpp:569-590`); `va_list`-typed parameters need hand-written thunks |
| Aggregate passing/return (HFA/HVA vs ELFv2 homogeneous aggregates) | in V regs / X8 | in FPRs/VRs / r3 | **Does not cross** (see above). The only place a convention is compiled against the *other* side's signature is `CallbackUnpack<Sig>::CallGuestPtr` (`Host.h:743-760`), which is host C++ compiled by the host compiler and packs before crossing. Fine |
| `wchar_t` | 4 bytes, unsigned | 4 bytes, signed | Same size; no proposed API uses it |
| `__int128`, `_Float16` | 16/2 bytes | 16/2 bytes | Not in the proposed APIs. `_Float16` on the host compiler **[unverified]** |
| Bit-fields | AAPCS64 §8.1.8 | ELFv2 §2.1.3 | Expected identical (both LE, both allocate from the low end). Vulkan has none; drm has a few in ioctl structs. The generator's layout pass (`DataLayoutCompareAction`, `data_layout.h:118-130`; per-type lookup at `data_layout.cpp:270-273`; not read in full) compares guest and host layouts per type and will diagnose any mismatch |
| glibc types with per-arch sizes (`pthread_mutex_t` etc.) | e.g. 48 bytes **[unverified]** | 40 bytes **[unverified]** | Only if an API exposes them by value; none of the proposed ones do. Treat as opaque |
| `jmp_buf`, `FILE *`, `DIR *` | libc-owned | libc-owned | Never dereferenced across; keep the `DESIGN.md:540` rule |
| Function pointers | guest VA, not host-executable | host VA, not guest-executable | Every one crossing in either direction needs a trampoline (§2) |
| Page size | `AT_PAGESZ` = host (`DESIGN.md` §4.9) | 64K (or 4K on the KVM kernel) | Identical by construction; matters for the over-read argument in §4.3 |

The `has_compatible_data_layout` predicate (`Host.h:133-160`) and the generator's per-type
comparison already encode this: on an LP64/LP64 pair almost every struct compares equal and no
repacking code is emitted. The `README.md` warning that parsing against the *host* headers
produced wrong repack code was about x86-vs-ppc; with an aarch64 target it is the aarch64
rootfs headers we parse, which is exactly right.

## 2. Callbacks

### 2.1 Mechanism (verified)

- The guest registers a callback by allocating a host trampoline for (guest target, guest
  unpacker): `Guest.h:185-221` → `fex:allocate_host_trampoline_for_guest_function` →
  `MakeHostTrampolineForGuestFunction` (`Thunks.cpp:400-482`), one 84-byte instance per
  (target, unpacker), cached in a map, I-cache flushed (`:478`).
- The host library calls the trampoline as an ordinary function. It writes
  `&TrampolineInstanceInfo` to a TLS slot in `Bin/FEX` (r11 is clobbered by ppc64le PLT
  stubs, `Thunks.cpp:64-99`) and `bctr`s to the host packer `CallGuestPtr` (`Host.h:743-760`),
  which reads the slot back through `FEX_GetCallbackGuestcallPtr()` (`Host.h:112-125`, one
  PLT call), bump-allocates the packed args **on the guest stack** (`Host.h:673-740`), and
  calls `CallCallback` (`Thunks.cpp:235-247`): X0/X1 ← (unpacker, args), then
  `HandleCallback` → `ExecuteJITCallback` (`Core.cpp:707-709`, `PPC64Dispatcher.cpp:963-1026`):
  push host callee-saved registers, kill the `InSyscallInfo` sentinel, set the return
  address, `FillStaticRegs`, run the guest until it returns to `ThunkCallbackRet`, whose
  translation is `CallbackReturn` (`BranchOps.cpp:87-109`): spill, pop, `blr` back into the host.
- Function pointers going the other way (host → guest, e.g. `vkGetInstanceProcAddr` results)
  are made guest-callable with `LinkAddressToFunction` (`Guest.h:94-101`) →
  `AddThunkTrampolineIRHandler` (`Core.cpp:1626-1654`): the JIT compiles a custom IR block *at
  the host address* that stores the callee in a GPR and jumps to a guest invoker
  (`GetCallerForHostFunction`, `Guest.h:171-176`), which packs and thunk-calls
  `GuestWrapperForHostFunction::Call` (`Host.h:770-870`), whose trailing argument is the host
  pointer.
- **Asynchronous callbacks are fatal**: `CallCallback` dies if the calling host thread has
  no guest thread object (`Thunks.cpp:236-238`). A host library's own worker threads (RADV's
  shader compiler threads) can never call into the guest. Vulkan's allocation callbacks are
  the one API where a driver might, and the host half already forces `pAllocator = nullptr`
  on every entry point (`libvulkan/Host.cpp:358-372`), which is legal Vulkan.

### 2.2 The three patterns in the question

| Pattern | Handling | Cost per invocation (qualitative; not measured) |
|---|---|---|
| Vulkan loader/allocation callbacks (`VkAllocationCallbacks`) | Dropped: `opaque_type` (`libvulkan_interface.cpp:116`) and forced null (`Host.cpp:358-372`). Debug messenger callbacks go through `emit_layout_wrappers` structs (`:101-107`) and the registry of guest unpackers keyed by mangled signature (`Guest.cpp:166-185`, `Thunks.cpp:597-631`) | none (dropped) |
| `GetProcAddress` | `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` are `custom_host_impl + custom_guest_entrypoint + returns_guest_pointer` (`libvulkan_interface.cpp:24-26`); guest side resolves the name in `HostPtrInvokers` and links the host pointer (`libvulkan/Guest.cpp:32-38, 92-156`); unknown names return null or a trapping stub (`FEX_VK_TRAP_UNKNOWN`). GL: `glXGetProcAddress` (`libGL_Guest.cpp:44-73`); EGL forwards to it (`libEGL_Guest.cpp:22-24`) | one extra custom-IR block + invoker per call versus a direct thunk; the link itself is once per (name, table) |
| `qsort`-style comparators | Not applicable: libc is not thunked. The real instance in the GPU set is **wayland listeners** (`wl_proxy_add_listener`): the host `libwayland-client` calls guest listener functions during `wl_display_dispatch`, on the guest's thread, through trampolines built by the wayland host half (`ThunkLibs/libwayland-client/Host.cpp`, 381 lines; not read in detail) | full host→guest→host round trip: callee-saved push, sentinel kill, `FillStaticRegs`, dispatcher lookup, guest blocks, `SpillStaticRegs`, pop. Dozens of host instructions plus two spills; acceptable at event rates, not at per-vertex rates |

The AArch64 change (§1.2 item 2) does not alter these costs; it only changes where the return
address lives.

## 3. GPU first: Vulkan and GL on Omarchy (Hyprland, Wayland)

### 3.1 Libraries and order

| Order | Guest sonames overlaid (`Data/ThunksDB.json`) | Host half binds to | Why in this order |
|---|---|---|---|
| 1 | `libvulkan.so.1` | host Khronos loader `libvulkan.so.1` → host ICD (RADV `libvulkan_radeon.so`) | Wayland-native, no GLX; the interface already defines `VK_USE_PLATFORM_WAYLAND_KHR` (`libvulkan/Guest.cpp:12`) |
| 1 | `libwayland-client.so.0` | host `libwayland-client.so.0` | Hard prerequisite: `wl_display`/`wl_surface` are `opaque_type` in the Vulkan interface (`libvulkan_interface.cpp:126-128`), i.e. the guest's `wl_display *` must already be a host object |
| 1 | `libdrm.so.2`, `libxshmfence.so.1` | host equivalents | small; drm ioctl structs would otherwise need the `_IOC` re-encoding path (`DESIGN.md` §5) |
| 2 | `libEGL.so.1`, `libGL.so.1`, `libGLX.so.0` | host libglvnd → Mesa | needs new EGL work (§3.4) |
| 3 | `libX11.so.6` (Xwayland clients) | host libX11 | only for GLX/X11 apps under Xwayland; the `X11Manager` glue exists (`include/common/X11Manager.h`) |
| later | `libasound.so.2` | host ALSA → PipeWire plugin | audio for games; interface exists (2637 lines) |

`libxkbcommon`, `libgbm` and the toolkit libraries stay emulated (pure CPU work; no host object
crosses).

### 3.2 The guest-side shim

One `libvulkan-guest.so` (aarch64) built from `libvulkan/Guest.cpp` + generated
`thunkgen_guest_libvulkan.inl`: exported packers for every thunked entry point, the
`vkGet*ProcAddr` overrides, a constructor that calls `fex:loadlib` and `OnInit`
(`Guest.cpp:159-188`). Same for wayland-client, drm, xshmfence. The stubs carry the sonames they
replace; `DESIGN.md` §6.2b's ELF-note discovery replaces the JSON once several stubs exist.

The guest sees the stub instead of the real library at **open time**: `GetEmulatedPath`
(`FileManagement.cpp:550-568`) matches the full overlay path or, inside allow-listed library
directories (`:526-540`), the basename. This is path redirection inside the emulator, not a
mount, so `ld.so` in the guest loads the stub through an ordinary `openat`.

~~**[guest view, 2026-09-17]** The rootfs mounted for this session has: glibc … vulkan-tools
1.4.350.0-1 (`pacman -Q`) … So the guest has the real aarch64 loader and RADV for the
*fallback* path.~~

**CORRECTED 2026-09-17 — that paragraph was reading the HOST, not the guest.** The m2 base rootfs
carries no pacman database (`ArchLinuxARM-m2/var/lib/pacman/local` does not exist; it is
extracted from pinned packages, not installed by pacman). A guest `pacman` therefore falls
through, by the rootfs's ordinary path fallthrough, to the host's `/var/lib/pacman`, so
`pacman -Q` listed **Jordan's ppc64le packages**: the reported `vulkan-tools 1.4.350.0-1` is
exactly the host's (`/var/lib/pacman/local/vulkan-tools-1.4.350.0-1`), and the loader, RADV,
Mesa and wayland versions match the host's `vulkaninfo` (§8 Answers). Likewise
`/usr/share/vulkan/icd.d/radeon_icd.json` and `/usr/lib/libvulkan_radeon.so` "exist" only because
the host's files show through. **So the guest has no aarch64 Vulkan loader or RADV today, and there
is no guest fallback path yet**; a non-thunked aarch64 Vulkan app would find no usable driver.

The aside about "two rootfs layers" not merging in `readdir` is the same effect, not two rootfs
layers: `/usr/lib/libvulkan.so.1` resolves root-owned because it is the host file seen through
fallthrough, while `ls /usr/lib` lists only the uid-1000 rootfs directory. The point stands for
the thunks — path opens are what `ld.so` and the thunk overlay use — but the cause is fallthrough.

**The two-tier rootfs of `DESIGN.md` §6.2a** (read-only base plus a per-user writable
`RootFS/<name>-overlay/` where guest `pacman -S` installs) **is designed but not implemented**:
nothing in `Source/` or `FEXCore/` references an `-overlay` layer. Until it is, a guest
`pacman -S` would try to install into host paths (and fail on permissions as uid 1000), so
aarch64 packages the GPU stage needs — `vulkan-tools` for vkcube, `vkmark`, the aarch64 loader
and RADV for any fallback path — must come from a separately built rootfs (a second manifest
over the m2 pins, `build-alarm-sysroot.sh --dest`), or from implementing the overlay layer
first.

### 3.3 Host binding and what the host must have

`libvulkan-host.so` dlopens the host loader and resolves through
`vkGetInstanceProcAddr` (`Host.cpp:111-130`); it scrubs `VK_ICD_FILENAMES`, `VK_DRIVER_FILES`,
`VK_LAYER_PATH`, `VK_INSTANCE_LAYERS` etc. from the *host* loader's view, honouring
`FEX_HOST_<VAR>` overrides (`Host.cpp:36-81`), so the guest's manifests (aarch64 ICDs) never
reach the host loader. The
host loader then finds the host RADV ICD by its own default search. GL's host half links
`OpenGL::GL` (`HostLibs/CMakeLists.txt:210`) and resolves through `glXGetProcAddress`
(`libGL_Host.cpp:4`).

### 3.4 Gaps for Wayland

- **EGL.** `libEGL_interface.cpp` thunks 17 functions and no `eglGetPlatformDisplay`,
  `eglGetPlatformDisplayEXT`, `eglCreatePlatformWindowSurface`, `eglQueryString`,
  `eglGetConfigs`, `eglGetConfigAttrib`, `eglSwapInterval`, `eglBindTexImage` or
  `eglCreateImage`; `eglGetProcAddress` forwards to `glXGetProcAddress`
  (`libEGL_Guest.cpp:22-24`), which is wrong on a Wayland-only display. Wayland-native GL
  clients (GTK4, Qt6, SDL3, Electron with ozone-wayland, Ghostty, Alacritty) all take this
  path. This is new interface work, not porting.
- **GL entry points** through `eglGetProcAddress` must resolve against the host's
  `eglGetProcAddress`, and `wl_egl_window` (`libwayland-egl`) is a further small library.
- **X11/GLX** works only through Xwayland and needs the X11 thunk glue.

Hence Vulkan first. Vulkan-on-Wayland needs only the loader, wayland-client and the WSI
surface entry points already in the interface.

### 3.5 Real Omarchy workloads for the GPU set

arm64-only binaries that would use it: Zed (Vulkan through wgpu/blade, Wayland-native, no GL),
Electron apps shipped arm64-only (Chromium GPU process: EGL + GL/Vulkan via ANGLE on Wayland,
or `--disable-gpu`), Godot exports, the arm64 games in ALARM's repos (`DESIGN.md` §6.1 ladder).
Native ppc64le already covers Electron/WebKit generally, so the thunk exists for what ships
arm64 only.

## 4. libc string and memory routines

### 4.1 Full library thunk: no

`DESIGN.md` §6.3 is right and the code confirms the reasons: glibc carries TLS
(`tpidr_el0` on the guest), the allocator, `jmp_buf`, `FILE *`, the unwinder and signal
delivery state; every one of them would have to be shared across the boundary. FEX never thunks
libc for the same reason.

### 4.2 Symbol-level redirect: what it is

Hook the *guest address* of a routine with a custom IR entrypoint (`AddCustomIREntrypoint`,
`Core.cpp:1610-1623`; consulted first in `CompileBlock`, `Core.cpp:917-926`) whose IR is:

```
prologue:  if (X2 < T) fall through into the normal translation of the routine
           else: State.pc = <routine's guest address>      ; precise PC for faults, §4.3
                 Thunk(ArgPtr = frame, hash("powerarm:memcpy"))
                 ExitFunction(X30)
```

The host `ThunkedFunction` (`FEXCore/include/FEXCore/Core/Thunks.h`, `void(void*)`) receives
the frame in `r4` today (`BranchOps.cpp:1732-1735`), reads `X0..X2` from it, calls host
`memcpy`, writes `X0`. No packer, no guest stub, no rootfs change: the guest binary is
untouched. The guest and host share one address space, so guest pointers are host pointers
(`DESIGN.md` §4.8 last bullet).

Two mechanism extensions are needed:

1. **Prefix hooks.** `CompileBlock` today treats a custom IR handler as the *whole* block
   (`Core.cpp:928`, `if (!HasCustomIR) DecodeInstructionsAtEntry`). A size gate needs the
   handler to emit a prologue and then let the decoder continue at the same address into the
   fall-through block. That is a small change to the `HasCustomIR` contract (return
   "handled" vs "prefixed"), and `NeedsAddGuestCodeRanges` (`Core.cpp:1050`) must stay true
   for prefixed blocks so mtrack keeps covering the routine's own bytes.
2. **Symbol resolution.** Resolve `memcpy`/`memmove`/`memset` when the guest maps
   `libc.so.6` (the `FileManager` knows the path; the ELF symbol walker in
   `VDSO_Emulation.cpp:519-560` is reusable) and, for static binaries, from the main ELF's
   `.symtab` in the loader. **[guest view]** ALARM glibc 2.43 exports `memcpy@@GLIBC_2.17`,
   `memmove`, `memset`, `strlen`, `strcmp`, `memchr` as plain `FUNC` symbols; the whole
   `libc.so.6` has only two `IFUNC`s (`gettimeofday`, `__gettimeofday`) (`readelf -Ws`). So
   there is no resolver to chase: the exported address *is* the routine, and glibc's internal
   hidden aliases point at the same address, so internal callers are hooked too. Compiler-inlined
   small copies never call and are unaffected either way.

### 4.3 Correctness, head on

- **Host writes into guest code pages (SMC).** Default `SMCChecks` is `mtrack`
  (`Config.json.in:533-537`): pages a block was compiled from are write-protected by host
  `mprotect`. A host `memcpy` storing into such a page faults with a **host PC in host libc**.
  `SyscallHandler::HandleSegfault` (`SyscallsSMCTracking.cpp:228+`) runs as a host handler
  before guest delivery (`SignalDelegator.cpp:1494-1499`); its main path checks the VMA is
  guest-writable, invalidates the granule and unprotects (`:293-330`, granule rule in
  `SMCHostGranule.h:4-34`), returns true, and the kernel retries the host store. The only
  branch that requires a JIT PC is the call-return-stack guard (`:258`). So the fault path
  already services host stores at zero cost on the non-SMC path; this is also what
  `UnprotectGuestRangeForHostWrite` (`:206-225`) exists for on the syscall side. **Gate before
  landing:** confirm the store-emulation and store-backpatch branches (`:477-510`, `:868-872`;
  the latter checks `IsAddressInCodeBuffer`) can never act on a host-libc instruction. Vector
  stores already decline emulation (`:187`). Lazy invalidation (`FEX_SMCLAZYINVAL`) defers to
  the next block entry, which is where the routine's `ExitFunction(X30)` lands.
- **Guest SIGSEGV with a PC inside libc.** A fault inside the host routine reaches
  `HandleGuestSignal` outside JIT code; that delivery path is the one thunk host libraries
  already use, and it reports `State.pc` as "block-boundary value, may be stale"
  (`SignalDelegator.cpp:1494-1545`). The prologue above stores the routine's guest address
  into `State.pc` first, so the frame carries a sane PC and the `si_addr` is exact (same
  address space). A handler that unprotects and returns (guard-page and GC write-barrier
  patterns) resumes the host store via the normal `rt_sigreturn` restore, because the kernel
  retries the faulting host instruction. What cannot work: a handler that inspects or skips the
  *faulting instruction* by guest PC. That is the same limitation every thunk has; document it
  and keep `POWERARM_HOSTFAULTTOGUEST` semantics unchanged.
- **Over-read across a page boundary.** Guest `AT_PAGESZ` equals the host page
  (`DESIGN.md` §4.9), so host `strlen`-style aligned reads never touch a page the guest could
  not also read. With 4K-granule emulation (guest built for 4K on a 64K host) a "guest-unmapped"
  4K sub-page is host-mapped and neither the emulated nor the host routine faults on it.
  Symmetric, and moot for the recommended set (`memcpy`/`memmove`/`memset` read exactly `n`
  bytes).
- **Ordering.** The guest is AArch64, weakly ordered; there is no TSO contract to keep, and
  `memcpy` gives none. Nothing changes.
- **Stack.** The JIT runs on the host thread stack; guest SP lives in state
  (`BranchOps.cpp:87-109` `ResetStack`; `Thunks.cpp:667-681`). Host `memcpy` never sees the
  guest stack.
- **Code cache.** Entries are validated by guest bytes, emulator build id and host features
  (`CodeCache.cpp:292-348`). A hooked block's host code depends on the hook set, not on the
  guest bytes, so hooked entries must either be excluded from the cache or the key must
  include the redirect configuration. **[unverified which is cheaper; decide at landing.]**
- **Signals during the host routine.** Async guest signals arriving while in host code are
  handled as for any thunk (deferred or delivered outside JIT). No new state.

### 4.4 Which routines, and the honest expectation

The N1 row's "64% of their cycles" comes from `probes/workloads/strloop.c`, a microbenchmark
that loops `strlen`/`memchr`/`strchr`/`memcmp` over a 4 KiB buffer
(`research/neon/NEON-LOWERINGS.md:51-53, 61`). It says how expensive those loops are *per byte*
under the JIT; it says nothing about how much of a desktop app is in them. Real programs pass
short strings (identifiers, paths, hash keys); for those the crossing (`SpillForABICall`, the
16 volatile vector registers refilled unconditionally, `BranchOps.cpp:1697-1700`) costs about
what a 16-byte `strlen` costs emulated, so an unconditional redirect is a wash at best. The
size gate removes that for the length-taking routines and cannot exist for `strlen`/`strcmp`.

Recommendation: redirect **`memcpy`, `memmove`, `memset`** (and `memcmp` if a hot user shows
up) with a threshold `T` to be measured (start at 256 bytes and sweep). Leave
`strlen`/`strchr`/`memchr`/`strcmp` to N1, which attacks exactly their loop idiom in the JIT.

Goldens: `unittests/A64Frontend/strmem.c` already hashes `memcpy`/`memmove`/`memset` over
lengths 0–129 and 16 alignments (`golden.sh:35-37` builds it `gcc -static -O2`). Extend it with
lengths across `T` and page-crossing copies, and add a copy into a page that holds executed code
(a self-modifying-caller test in the style of `callret.c`). Since it is static, the hook must
resolve from the main ELF's `.symtab` for the test to exercise the redirect at all.

## 5. Later candidates

- **OpenSSL (`libcrypto`/`libssl`).** Low value: AES/SHA/PMULL are already emulated with
  hardware instructions (N6/N9 rows), and the big TLS users (Node, Go, Rust, Chromium) link
  their own crypto statically (`DESIGN.md` §6.1 order item 2). The thunk cost is high: BIO
  methods, verify/password callbacks, `ex_data`, allocators, `va_list` functions, provider
  loading, and `.so.3` version pinning. Keep it at the end of the list.
- **zlib → `libnxz` (POWER9 NX-GZIP).** `libz.so.1` is a small, C-only, callback-light API:
  `z_stream` is LP64-identical, `zalloc`/`zfree` are the only function pointers (force the
  defaults, as Vulkan does with `pAllocator`), and `state` is host-owned opaque.
  `libnxz` is described as a zlib-compatible drop-in **[unverified; not read]**. Prerequisites:
  `/dev/crypto/nx-gzip` is root-only on this box today and NX only pays for large streams
  (`libnxz` falls back to software below its threshold **[unverified]**). Value depends on who
  inflates big streams in the guest: package managers, browsers' content decoding, `git`,
  Electron's asar. Worth a measured look after the GPU set; not before the device is
  unprivileged.

## 6. Measurement

Rules from `HANDOVER.md` apply: deterministic counters over wall time, cold and warm as a pair,
`env -u LD_LIBRARY_PATH PATH=/usr/local/bin:/usr/bin` around anything under `perf`.

### 6.1 Counters that already exist

- `ThreadStats` (`FEXCore/include/FEXCore/Utils/SHMStats.h:58-77`, enabled by `ProfileStats`,
  `Config.json.in:483`): `AccumulatedJITTime`, `AccumulatedSignalTime`, `AccumulatedSMCCount`,
  `AccumulatedJITCount`, cache miss/lock times. Add **`AccumulatedThunkCount`** and
  **`AccumulatedCallbackCount`** (no such counter exists: grep) so a run reports how many
  crossings it made; that is the number to divide any saving by.
- `POWERARM_STARTUPTIMES=1` per-process phase timer (`OPTIMIZATION-CHECKLIST.md:210, 216`).
- The JIT perf map (`JIT.cpp:6112` region) with `perf record -e cycles:u` gives **cycles per
  DSO**: the thunk's acceptance number for a library is that the emulated copy of that library
  (`libvulkan.so.1`, `libvulkan_radeon.so`, `libGL*`) goes to zero in `perf report --sort dso`
  and the host copies appear instead. `perf stat -e instructions:u` over the process is the
  deterministic-ish total.

### 6.2 GPU acceptance: vkcube and vkmark, native versus thunked (owner request)

Three runs of the *same* scene on the same display, in this order:

1. **Native ppc64le** `vkcube` and `vkmark` on the host (Jordan's console; this shell cannot).
2. **aarch64 build under POWERarm with the thunk**: guest `vulkan-tools` is installed;
   `vkmark` is one `pacman -S vkmark` in the guest overlay **[guest view]**.
3. **aarch64 build fully emulated** (thunk disabled, guest loader + guest RADV): if it runs at
   all it needs the DRM `_IOC` re-encoding path and compiles shaders under emulation; record
   it as the "before" number, or record that it does not run.

Protocol: `vkcube --wsi wayland --present_mode 0 --c 3000` timed (frames / wall seconds; wall
time is the honest metric for a presentation loop), and `vkmark --winsys wayland
--present-mode immediate` at 1920×1080 (per-scene FPS and the composite score it prints).
Exact option spellings for these versions **[unverified]**. Validation layers on the host side
(`FEX_HOST_VK_INSTANCE_LAYERS`, `Host.cpp:63-81`) must report no errors in run 2.

What "comparable to native" should mean (proposed thresholds for Jordan to ratify):

| Scene class | Metric | Success |
|---|---|---|
| GPU-bound (vkmark heavy scenes at 1080p) | per-scene FPS, thunked ÷ native | **≥ 0.85** each, composite ≥ 0.85 |
| CPU/API-bound (vkcube uncapped, vkmark light scenes) | FPS ratio | **≥ 0.60**; this is where the crossing cost shows and it is the number to watch across releases |
| Any | validation errors, visual diff | zero errors; same output |

vkcube is nearly free for the GPU, so its ratio is close to a pure measure of per-call
overhead; vkmark's heavy scenes tell whether the host driver is doing the work. Both are needed.

### 6.3 Real apps

After the synthetic gate: Zed startup-to-first-frame and scroll (Vulkan, Wayland), an Electron
arm64 app with GPU compositing on and off, SuperTuxKart from the ALARM repos with the frame log
(`FEX_FRAMELOG`, `libGL_Host.cpp:131-135`). Report per-DSO cycles and crossing counts, not FPS
alone.

### 6.4 libc redirect

`strmem` goldens for correctness; for value, `perf stat -e instructions:u` and per-DSO cycles
on code-server startup and the compile slice with the redirect on/off and `T` swept. If the
slice does not move, the row is closed with that number.

## 7. Staging

| Stage | Contents | Independent test | Value |
|---|---|---|---|
| **0. Core ABI + libVDSO** (first landing) | A64 decode of `HLT #0x0F3F`+hash → `Thunk`, `HLT #0x0F3E` → `CallbackReturn`; X30 callback return in dispatcher/`CallbackReturn`/JIT sentinel; X16/X17 linked callee; `long double` refusal + aarch64 target in the generator; aarch64 guest stub build; `libVDSO-guest.so` (`libVDSO_Guest.cpp` rewritten without x86 asm; `__kernel_rt_sigreturn` already exists at `VDSO_Emulation.cpp:629-645`); thunk/callback counters | Pi goldens: a `clock_gettime`/`gettimeofday` monotonicity and `CLOCK_*` coverage test next to `cntvct.c`; `sigill_udf`-style test that a bare `HLT` still SIGILLs; `unittests/ThunkLibs/{abi,generator}.cpp` extended with an aarch64 layout case; all three suite modes; `a64diff` | vDSO clocks without syscalls (V8, code-server, Claude CLI); proves the whole crossing on something deterministic |
| **1. Vulkan + wayland-client + drm + xshmfence** | guest stubs for the four; `SP == 0 mod 16` in the bump allocator; discovery/preflight per `DESIGN.md` §6.2b (at least the fail-loud path) | `vkcube` on Hyprland; §6.2 thresholds; validation clean | Zed, Godot, Vulkan games |
| **2. EGL/GL for Wayland** | new EGL interface entries (§3.4), `eglGetProcAddress` through host EGL, `libwayland-egl`; then GLX/X11 via Xwayland | `eglgears_wayland`/`glmark2-wayland` from `mesa-demos`; SuperTuxKart GL renderer | Electron/Chromium GPU process, GL games, terminals |
| **3. libc redirect** (parallel track; needs no stubs) | prefix hooks, symbol resolution, `memcpy`/`memmove`/`memset` with `T`, cache keying | `strmem` extended, SMC self-modifying-caller test, all modes | measured on code-server and the slice; kept only if the slice or a real app moves |
| **4.** asound, SDL2, zlib/`libnxz`, OpenSSL | as value appears | per library | |

Stage 0 is independently valuable and independently testable; nothing in it needs the host GPU
or Jordan's console. Stage 1 is the first thing that needs the console.

## 8. Questions for Jordan (host facts this session cannot see)

1. **Host `long double`:** still `-mabi=ibmlongdouble` after upgrades (measured 2026-09-16,
   `DESIGN.md:383-397`)? Any intention to move Arch POWER to IEEE128? Only matters if a thunked
   API ever carries `long double`; none of the GPU set does.
2. **Host Mesa/RADV:** version (`vulkaninfo --summary`), that `libvulkan.so.1` is the Khronos
   loader and which package provides it, path of `radeon_icd.*.json`, whether RADV enumerates
   the RX 7900 XTX on this kernel (7.2.6-64k) and whether `radeonsi` GL works under Hyprland
   natively. Any `AMD_VULKAN_ICD`/`VK_*` variables set in the session.
3. **Host libglvnd/EGL/GLX:** versions; is Xwayland running; host `libwayland-client` version
   (guest is 1.25.0).
4. **Native vkcube/vkmark:** are `vulkan-tools` and `vkmark` packaged for Arch POWER, or does
   vkmark need a meson build? Run the native side of §6.2 on the console and keep the numbers.
5. **Thresholds** in §6.2: ratify or change.
6. **HLT marker scope** (§1.2 item 1): recognise everywhere, or only inside `GuestThunks/`
   mappings and the vDSO page?
7. **Cross toolchain:** is `aarch64-linux-gnu-gcc` installed on the AC922, or do guest stubs
   get built on the Pi (load concern per `HANDOVER.md`)?
8. **`/dev/crypto/nx-gzip`:** when it becomes unprivileged, and whether `libnxz` is packaged
   for Arch POWER.
9. **`libasound` on the host:** PipeWire's ALSA plugin present (for the asound thunk later)?

### Answers (2026-09-17)

Host facts come from host tools run by absolute path. Tools absent from the guest rootfs (e.g.
`vulkaninfo`, `vkcube`, `clang`, `ninja`) fall through to the host and run natively, so they see
the real system. Tools that ARE in the rootfs (`pacman`, coreutils, `bash`) return the guest's
view, so `pacman -Q` from the emulated shell is not evidence about the host.

1. **`long double` is IBM double-double, and Jordan is not moving the port to IEEE128.** So
   the generator's `long double` refusal is permanent, not a stopgap: a thunked signature that
   carries `long double` must be refused (or hand-marshalled), never passed through.
2. **Full Vulkan stack on the host** (Jordan: "full vulkan stack, and icd-loaders"). From
   `vulkaninfo --summary`: loader instance 1.4.350; **AMD Radeon RX 7900 XTX (RADV NAVI31)**,
   `DRIVER_ID_MESA_RADV`, **Mesa 26.2.2-arch1.2**, apiVersion 1.4.354, conformance 1.4.5.3,
   discrete. ICD directory holds `radeon_icd.json` only. Instance extensions include
   `VK_KHR_wayland_surface`, `VK_KHR_xcb_surface` and `VK_KHR_xlib_surface`.
3. **Wayland and Xwayland both live** in the session: `WAYLAND_DISPLAY=wayland-1`,
   `DISPLAY=:0`. (libglvnd/EGL/wayland-client versions still to read from the libraries.)
4. **`vkcube` and `vkcubepp` are installed natively; `vkmark` is not.** vkcube defaults to FIFO
   presentation, so natively it is vsync-bound (120 Hz display) and measures little. vkmark needs
   packaging for the port (Jordan is its package distributor) before it can be the §6.2
   benchmark. The guest side needs an aarch64 vkcube in the rootfs, which it does not have yet.
7. **Guest stubs build with the rootfs's own gcc under POWERarm**, the way every
   `unittests/A64Frontend` test was built this session; no cross toolchain and no Pi load needed.

**Measured 2026-09-17: the UNTHUNKED path already works, and is at native speed when GPU-bound.**
A rootfs built from `Scripts/powerarm/rootfs/alarm-vk.manifest` (the m2 roots plus `vulkan-tools
vulkan-radeon`: 113 packages, aarch64 Mesa 26.2.3 RADV, loader 1.4.357, into `RootFS/ArchLinuxARM-vk`
with `--dest`, never touching m2) lets an aarch64 `vulkaninfo` enumerate the RX 7900 XTX through the
guest's own, emulated RADV. It reports Mesa 26.2.3, the guest package, where the host runs 26.2.2, so it
is not falling through to the host driver. That driver talks to the real GPU through POWERarm's generic
ioctl re-encoding (`ABITranslation.h`, `IoctlRequestToHost`), and the DRM argument structs need no
translation because the DRM uAPI is arch-independent. vkcube on Wayland, 5000 frames, mailbox:
**native 2107 fps, guest 2145 fps (frame time 0.98x)**. vkcube is far too light to show a driver's CPU
cost (~0.47 ms a frame, one draw call), so this says nothing about API-bound rendering -- the Civilization
6 shape, many thousands of draw calls a frame -- which is exactly where a thunk earns its place. So:
games have a working unthunked graphics path today, and GPU-bound work needs no thunk. That a native
driver beats an emulated one for the driver's CPU work is not in question -- it is the premise of FEX's
thunks, and fastppcx86 already demonstrates it on this machine and GPU -- so the Vulkan thunk is NOT
gated on a benchmark; its priority is set by the owner's order (emulation first). What deserves a
measurement, once there is a real arm64 game or API-heavy app to run, is the crossing cost on very
chatty APIs, where a fixed per-call cost can eat much of what native execution saves on tiny calls
(FEX's Civilization 6 figure): a tuning step during the thunk work, not a gate before it.

**Prior art: the parent project already runs these thunks.** POWERarm is a fork of fastppcx86
(the x86-64 → ppc64le JIT), and fastppcx86's `build-thunks` carries the full working set: guest
stubs for Vulkan, GL, EGL, wayland-client, drm, xshmfence, asound and the vDSO, plus ppc64le host
libraries for all but the vDSO (`build-thunks/Guest/`, `build-thunks/HostLibs_64/`, selected with
`FEX_THUNKGUESTLIBS`/`FEX_THUNKHOSTLIBS`). Its notes record the evidence:

- **DOOM 2016 runs native Vulkan through the live thunk at 89 fps on this RX 7900 XTX**, not
  vsync-capped (120 Hz display), dropping to ~53, with the GPU never the limit, so every lost
  frame is CPU-side cost (`fastppcx86/build-agent.md`).
- **FEX PR #4061 measured Civilization 6 at 333,601 Vulkan calls per frame, ~3.96 ms of
  marshalling, near 25% of frame time.** The crossing cost is real on API-heavy workloads, which
  is what the §6.2 API-bound threshold should be read against.

So for POWERarm the GPU stage is porting the guest side of thunks already proven on this
hardware, not building thunks. x86-64 and AArch64 are both LP64 little-endian with the same
natural alignment for everything the Vulkan/wayland/drm/xshmfence interfaces carry (no
`long double`, no x87 types), so the generated host libraries may be reusable nearly as they
are. That is the first thing Stage 1 should establish, by checking the generator's own layout
comparison rather than assuming it.

**New risk, not in §9: host implicit layers.** The host loader has
`VK_LAYER_VALVE_steam_overlay_{32,64}` and `VK_LAYER_VALVE_steam_fossilize_{32,64}` registered
alongside Mesa's `anti_lag` and `device_select`. A thunked guest app enumerates through the host
loader, so it inherits every implicit layer the host has. The Steam ones are presumably x86
builds for fastppcx86, which the native loader skips (vulkaninfo runs clean), but the thunk has
to be tested with them present, and `VK_LOADER_LAYERS_DISABLE` is the escape hatch if one
misbehaves.

## 9. Risks, ranked

1. **The AArch64 guest ABI touches the JIT's callback path in three places**
   (`PPC64Dispatcher.cpp:1006`, `BranchOps.cpp:100`, `JIT.cpp:1923`) and the
   `InSyscallInfo` sentinel interlock documented at `PPC64Dispatcher.cpp:964-983`. A mistake
   there is a signal-delivery or SRA-spill bug of the class the Factorio note describes, and
   single-threaded goldens do not see it. Mitigation: Stage 0 lands with a callback test
   (a vDSO-hosted function that calls back into the guest) run in all three modes and under
   `a64diff`.
2. **Wayland-native GL is not covered by the inherited thunks.** The GL host half is GLX-first
   and EGL has no platform entry points (§3.4). If Electron-class apps are the target, that is
   new interface work with its own callback surface, and it should be scoped before promising
   dates.
3. **The libc redirect can lose.** Crossing cost versus routine length is the whole question,
   and the only evidence today (`strloop.c`) is a microbenchmark. Land it behind a knob, gate it
   on the slice and code-server numbers, and be ready to keep only `memcpy`/`memset` above a
   threshold, or nothing.

Secondary: code-cache keying for hooked blocks (§4.3); the guest `readdir` layer-merge gap
(§3.2 aside) affecting any guest tool that discovers files by listing; host worker threads
calling guest callbacks are fatal by design (§2.1).

## Appendix: verification ledger

Read in full or in the cited ranges: `ThunkLibs/README.md`; `ThunkLibs/include/common/{Guest,
Host,PackedArguments}.h`; `ThunkLibs/Generator/main.cpp` (60–128) and grep of
`analysis.cpp`/`data_layout.cpp`; `ThunkLibs/libvulkan/{Guest.cpp,Host.cpp (30–70, 100–130,
355–372),libvulkan_interface.cpp (grep)}`; `libGL_Guest.cpp`/`libGL_Host.cpp` (grep, 94–135);
`libEGL/*`; `ThunkLibs/HostLibs/CMakeLists.txt`; `Data/ThunksDB.json`;
`Source/Tools/LinuxEmulation/Thunks.cpp` (all); `VDSO_Emulation.cpp` (1–80, 519–560, 599–680,
grep); `LinuxSyscalls/FileManagement.cpp` (40–120, 526–575); `SignalDelegator.cpp` (1488–1545,
grep); `SyscallsSMCTracking.cpp` (1–80, 149–330, grep); `SMCHostGranule.h` (1–60);
`FEXCore/Source/Interface/Core/Core.cpp` (707–770, 905–935, 1569–1660);
`JIT/PPC64LE/BranchOps.cpp` (87–135, 1678–1743); `PPC64Dispatcher.cpp` (960–1030);
`IR/IR.json` (grep); `A64Frontend/` (grep; `a64.inc:49`; `IRBuilder.cpp:141-146, 195`);
`CodeCache.cpp` (grep); `SHMStats.h` (39–110); `Config.json.in` (205–240, grep);
`ELFCodeLoader.h` (grep, 1159–1160); `unittests/A64Frontend/{run.sh,README.md,strmem.c,
golden.sh}`; `unittests/ThunkLibs/` listing; `docs/powerarm/{DESIGN.md §4.7–4.9, §5, §6, §7,
HANDOVER.md, OPTIMIZATION-CHECKLIST.md (N rows, 247–316), M0-CENSUS.md (thunk sections),
M1-ABI-DIFFERENCES.md (headings), research/neon/NEON-LOWERINGS.md (40–62),
probes/workloads/strloop.c, CODE-CACHE.md (headings)}`; `Scripts/powerarm/rootfs/README.md`
and manifest head.

Guest-view probes (this shell = ArchLinuxARM rootfs under POWERarm): `pacman -Q`, `pacman -Ss`,
`readelf -Ws /usr/lib/libc.so.6`, `ls` of `/usr/lib`, `/usr/share/vulkan/icd.d`, `/usr/lib/dri`.

Not verified: `JIT.cpp:1923` contents (cited via `M0-CENSUS.md`); `libwayland-client` and
`libdrm` thunk internals; `libnxz` API and fallback behaviour; vkcube/vkmark option spellings;
host compiler `_Float16`; glibc `pthread_mutex_t` sizes; whether the aarch64 cross gcc is
installed; every host fact in §8.
