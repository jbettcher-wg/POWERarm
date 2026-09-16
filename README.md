# POWERarm: run AArch64 Linux programs on POWER

POWERarm is a planned AArch64 Linux user-mode emulator for **PPC64LE** hosts (POWER8 and later).
It translates guest A64 code to PPC64LE machine code at run time. It registers with
`binfmt_misc`, so AArch64 ELF binaries run the same way native ones do. When the host already
has a ppc64le build of a library or toolchain, POWERarm calls that build through a thunk
instead of emulating it. Everything else comes from an AArch64 rootfs.

Status: **M0, rename done.** The tree is a history-sharing fork of fastppcx86 (`daedalao-wt`).
The product surface has been renamed so both emulators install side by side; the x86 guest is
still present and the A64 frontend doesn't exist yet. Start with
[`docs/powerarm/DESIGN.md`](docs/powerarm/DESIGN.md).

- **Minimum ISA: POWER8 (2.07).** POWER9 (ISA 3.0) paths sit behind a runtime `AT_HWCAP2`
  gate, following the same rule fastppcx86 uses.
- **License: MIT.** See [`LICENSE`](LICENSE) and the reuse analysis in `DESIGN.md` §2.

## Names

POWERarm never uses fastppcx86's names, so neither emulator can pick up the other's settings,
server or binfmt entry.

| What | fastppcx86 | POWERarm |
|---|---|---|
| Loader | `FEX`, `FEXInterpreter` | `POWERarm`, `POWERarmInterpreter` |
| Tools | `FEXBash`, `FEXServer`, `FEXRootFSFetcher`, `FEXConfig`, `FEXGetConfig`, `FEXOfflineCompiler`, `FEXpidof` | `POWERarmBash`, `POWERarmServer`, `POWERarmRootFSFetcher`, `POWERarmConfig`, `POWERarmGetConfig`, `POWERarmOfflineCompiler`, `POWERarmpidof` |
| Environment | `FEX_<NAME>` | `POWERARM_<NAME>` |
| Directories | `~/.fex-emu`, `$XDG_*_HOME/fex-emu`, `$prefix/{share,lib}/fex-emu` | `~/.powerarm`, `$XDG_*_HOME/powerarm`, `$prefix/{share,lib}/powerarm` |
| Server socket | `<uid>.FEXServer.Socket` | `<uid>.POWERarmServer.Socket` |
| binfmt_misc | `FEX-x86`, `FEX-x86_64` | `POWERarm-aarch64` |

All of these come from `POWERARM_EXE_PREFIX`, `POWERARM_DIR_NAME` and `POWERARM_ENV_PREFIX` in
the top-level `CMakeLists.txt`. Source paths (`FEXCore/`, `CodeEmitter/`, `ThunkLibs/`,
`Source/`), C++ namespaces and CMake target names deliberately keep their FEX names, so backend
commits `git cherry-pick` cleanly between the two trees. The code still spells its knobs
`getenv("FEX_...")`. The linker redirects those calls to `Source/POWERarm/EnvPrefix.cpp`, which
looks up `POWERARM_...` instead.

## Building

```
git submodule update --init --recursive
CC=clang CXX=clang++ cmake -S . -B build-powerarm -GNinja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_THUNKS=OFF -DBUILD_TESTING=OFF
ninja -C build-powerarm
```

Build dependencies are the same as fastppcx86's; see [`docs/DEPENDENCIES.md`](docs/DEPENDENCIES.md).

## Provenance

POWERarm is derived from fastppcx86 (<https://github.com/daedalao/fastppcx86>), which is in turn
derived from the FEX-Emu project (<https://github.com/FEX-Emu/FEX>). It is distributed under the
same MIT license. See [`LICENSE`](LICENSE), which is unmodified. Both projects were forked and
heavily use LLM-generated code.

The upstream FEX-Emu README is preserved verbatim as [`README.upstream.md`](README.upstream.md)
and is not maintained here. Most of `docs/` outside `docs/powerarm/` is inherited from
fastppcx86 and describes the x86 guest.

Arm is a trademark of Arm Limited and POWER is a trademark of IBM. POWERarm is an independent project, not affiliated with or endorsed by either.
