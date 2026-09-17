# Build dependencies

Reference host: IBM POWER8 S822LC (8335-GCA), Arch POWER, ppc64le, 4K-page kernel.
Every package and version below is one actually installed on that host and resolved by
CMake there, not a guess from the build files.

Validated against a full-featured configuration (`CMakeCache.txt`, 2026-08-16): thunks
on, FEXConfig on, tests on. POWERarm M0a (2026-09-16) removed the Wine bridge, the game
launcher, Steam support, 32-bit and x86-64 guest thunk stubs and the x86 guest test
suites; the dependencies that existed only for those are gone from this list.

Package names are Arch POWER, from the repositories ArchPOWER ships. **On other
distributions, match by the header or library file, not by the package name** — the
file columns below are the portable part; the names are not, and have not been
verified anywhere but ArchPOWER.

## 1. Core — required for every configuration

| Package | Version | Provides | Required by |
|---|---|---|---|
| `clang` | 22.1.8-1 | `/usr/bin/clang{,++}` | Hard requirement. GCC is a `FATAL_ERROR` in `CMakeLists.txt`; minimum is clang 13.0 |
| `lld` | 22.1.8-1 | `/usr/bin/ld.lld` | Default linker for the clang build |
| `cmake` | 4.4.0-1 | | `cmake_minimum_required(VERSION 3.14)` |
| `ninja` | 1.13.2-3 | | Generator |
| `python` | 3.14.6-1 | | `find_package(Python 3.9 REQUIRED COMPONENTS Interpreter)` — codegen |
| `git` | 2.55.0-1 | | Submodules; version/hash stamping |
| `pkgconf` | 2.5.1-1 | `pkg-config` | `Data/CMake/Findxxhash.cmake`. Part of `base-devel` |
| `glibc` | 2.43+r37 | | |
| `gcc-libs` | 16.1.1+r346 | libstdc++ | C++20 (`CMAKE_CXX_STANDARD 20`) |
| `fmt` | 12.1.0-2 | `/usr/lib/cmake/fmt` | `find_package(fmt QUIET)`. Falls back to `External/fmt` |
| `xxhash` | 0.8.3-1 | `/usr/include/xxhash.h` | `CodeCache.cpp`, `Core.cpp`, `OpcodeDispatcher.h`, `SMCSoftInvalidate.h` |
| `range-v3` | 0.12.0-2.1 | `/usr/lib/cmake/range-v3` | `find_package(range-v3 QUIET)`. Falls back to `External/range-v3` |

Optional but on by default in every known-good config:

| Package | Version | Gate |
|---|---|---|
| `gdb` | 17.2-1 | `ENABLE_GDB_SYMBOLS`, auto-detected from `gdb/jit-reader.h`. Silently OFF if absent |
| `ccache` | 4.13.2-1 | `ENABLE_CCACHE` (default ON) |

### Submodules

No system package exists for these; `git submodule update --init` or the build fails
at configure time with "does not contain a CMakeLists.txt".

| Submodule | Needed when |
|---|---|
| `External/unordered_dense` | Always — Arch POWER has no `unordered_dense` package (`unordered_dense_DIR-NOTFOUND`) |
| `External/jemalloc_glibc` | Always at default `ENABLE_JEMALLOC_GLIBC_ALLOC=ON` |
| `External/rpmalloc` | Always at default `ENABLE_FEX_ALLOCATOR=ON` |
| `Source/Common/cpp-optparse` | Always. No system package |
| `External/drm-headers` | Always — 32-bit ioctl emulation in LinuxEmulation (pending removal in M0b). No system package |
| `External/fmt`, `External/xxhash`, `External/range-v3` | Only if the system copy is absent |
| `External/Vulkan-Headers` | `BUILD_THUNKS`. Used in preference to any system copy |
| `External/Catch2` | `BUILD_TESTING`. Arch POWER has no Catch2 3 package (`Catch2_DIR-NOTFOUND`) |
| `External/vixl` | `BUILD_TESTING` or `ENABLE_VIXL_{DISASSEMBLER,SIMULATOR}`. aarch64 disassembler for emitter tests only |
| `External/tracy` | `ENABLE_FEXCORE_PROFILER` with `FEXCORE_PROFILER_BACKEND=tracy` |

## 2. `BUILD_THUNKS=ON`

Host halves only. `BUILD_GUEST_THUNKS=ON` is a configure error until AArch64 guest stubs
exist (POWERarm M5).

### Generator

| Package | Version | Resolved | Why |
|---|---|---|---|
| `clang` | 22.1.8-1 | `/usr/lib/cmake/clang` | `find_package(Clang REQUIRED CONFIG)` — `ThunkLibs/Generator/CMakeLists.txt` |
| `llvm` | 22.1.8-2 | `/usr/lib/cmake/llvm` | thunkgen links `clang-cpp LLVM` |

libclang-cpp is a build dependency independently of clang-the-compiler: thunkgen is a
libtooling program that parses the interface headers. Until M5 it still parses them as
the inherited x86-64 guest ABI; an `x86_64-pc-linux-gnu` cross sysroot (ArchPOWER
`x86_64-pc-linux-gnu-glibc` + `-gcc`) or `X86_DEV_ROOTFS` gives that parse real x86
headers. Without one, the libasound, libGL and libvulkan interface parses fail with
`'type_traits' file not found`; the other host halves still build.

Never point the x86 interface parse at ppc64le headers to make a build succeed — that
parse decides guest data layout and will silently emit wrong repack code.

### Host-side halves — compile and link time

| Package | Version | Header / library | Consumer |
|---|---|---|---|
| `libglvnd` | 1.7.0-3 | `GL/{gl,glx,glext,glxext}.h`, `EGL/egl.h`, `/usr/lib/libGLX.so` | `libGL_Host.cpp`, `libEGL_Host.cpp`; `find_package(OpenGL REQUIRED)` (`ThunkLibs/HostLibs/CMakeLists.txt`) |
| `libxcb` | 1.17.0-1.2 | `xcb/xcb.h` | `libGL_Host.cpp`, `include/common/X11Manager.h` |
| `libx11` | 1.8.13-1 | `X11/Xlib.h` | `include/common/X11Manager.h` |
| `libdrm` | 2.4.134-1 | `xf86drm.h`, `/usr/include/libdrm` | `libdrm/Host.cpp`, `libdrm_interface.cpp` |
| `libxshmfence` | 1.3.3-1.1 | `X11/xshmfence.h` | `libxshmfence/Host.cpp` |
| `wayland` | 1.25.0-1 | `wayland-client.h`, `/usr/include/wayland` | `libwayland-client/Host.cpp` |
| `alsa-lib` | 1.2.16.1-1 | `alsa/asoundlib.h` | `libasound/libasound_Host.cpp`. Built even though the thunk ships disabled |
| `vulkan-icd-loader` | 1.4.350.0-1 | `/usr/lib/libvulkan.so` | Vulkan thunk target |

SDL2 is **not** a dependency — that thunk is commented out in
`ThunkLibs/HostLibs/CMakeLists.txt`.

## 3. GUI

| Option | Package | Version | CMake |
|---|---|---|---|
| `BUILD_FEXCONFIG` | `qt6-base` + `qt6-declarative` | 6.11.1-1 / 6.11.1-3 | `find_package(Qt6 COMPONENTS Qml Quick Widgets)`, Qt5 fallback — `Source/Tools/CMakeLists.txt` |

## 4. Tests (`BUILD_TESTING=ON`)

Host-side only (APITests, emitter tests, thunkgen tests): the `Catch2` and `vixl`
submodules. No assembler or guest binaries are needed.

## 5. Runtime only

`dlopen`'d by the host thunks at load time, so invisible to the linker and to
`namcap`. A missing one makes `fexldr_init_<lib>` return false and the thunk silently
stops working.

`libglvnd` · `vulkan-icd-loader` + an ICD · `libdrm` · `libxshmfence` · `wayland` ·
`libx11` · `libxcb`

Optional: `squashfuse` / `erofs-utils` (RootFS images), `wget`, `xz`,
`pipewire-pulse` or `pulseaudio` (guests speak the PulseAudio protocol).

## 6. Availability

Everything in sections 1-4 is a stock package in the ArchPOWER repositories except
the submodules (§1): `git submodule update --init --recursive`. All public HTTPS; no
credentials needed.

Non-ppc64le and non-Arch builders: the file columns in §1-4 identify what is actually
`#include`d or linked. Resolve those to your distribution's package names locally —
this document does not claim to know them.

## 7. Install lines

Full configuration:

```sh
pacman -S --needed \
  clang lld llvm cmake ninja python git pkgconf ccache gdb \
  fmt xxhash range-v3 \
  qt6-base qt6-declarative \
  libglvnd libx11 libxcb libdrm libxshmfence wayland alsa-lib vulkan-icd-loader
git submodule update --init --recursive
```

Core only — no thunks, no GUI, no tests — needs section 1 alone:

```sh
cmake -S . -B build -GNinja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF -DBUILD_TESTING=OFF
```

## 8. Host requirements

- **ppc64le.** `CMakeLists.txt` rejects other processors. x86-64 hosts need
  `ENABLE_X86_HOST_DEBUG=True` and are debug-only.
- **4K or 64K-page kernel.** On a 64K kernel `POWERARM_HOSTPAGEMODE=auto` (the default)
  runs a binary natively when every PT_LOAD of it and its interpreter has
  `p_align` >= 64K, and uses the 4K granule emulation otherwise
  (`docs/powerarm/DESIGN.md` §4.9).
- `TUNE_CPU` defaults to `native`, which bakes `-march=native` into the binary. Set
  `-DTUNE_CPU=none` for anything another machine will run.
- `BUILD_TESTS=False` is not enough to skip tests — `unittests/` is gated on
  `BUILD_TESTING`.

## 9. Divergence from the ArchPOWER package

`packaging/archpower/PKGBUILD` is the packaged subset, not a superset:

- It omits `ccache` (build caching off) and tests — correct for a package.
- Its `pkgver` is a committed placeholder; `pkgver()` recomputes at build time.
