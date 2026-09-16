# POWERarm: run AArch64 Linux programs on POWER

POWERarm is an AArch64 Linux user-mode emulator for **PPC64LE** hosts (POWER8 and later). It
translates guest A64 code to PPC64LE machine code at run time. It registers with
`binfmt_misc`, so AArch64 ELF binaries run the same way native ones do. When the host already
has a ppc64le build of a library, POWERarm calls that build through a thunk instead of
emulating it. Everything else comes from an AArch64 rootfs.

Status: **early development (M0).** The A64 frontend doesn't exist yet. Start with
[`docs/powerarm/DESIGN.md`](docs/powerarm/DESIGN.md).

- **Minimum ISA: POWER8 (2.07).** POWER9 (ISA 3.0) paths sit behind a runtime `AT_HWCAP2`
  gate.
- **Host page size:** 4K and 64K kernels.
- **Configuration:** `POWERARM_*` environment variables and `~/.powerarm`. The binfmt entry is
  `POWERarm-aarch64`.

## Building

```
git submodule update --init --recursive
CC=clang CXX=clang++ cmake -S . -B build-powerarm -GNinja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_THUNKS=OFF -DBUILD_TESTING=OFF
ninja -C build-powerarm
```

Clang is required. Build dependencies are listed in [`docs/DEPENDENCIES.md`](docs/DEPENDENCIES.md).

## Acknowledgements

POWERarm is built on the work of two projects:

- **[FEX-Emu](https://github.com/FEX-Emu/FEX).** Its JIT core, IR, Linux emulation layer, thunk
  generator and rootfs tooling are the foundation of this project. Thanks to Ryan Houdek and
  all FEX-Emu contributors. The upstream README is preserved as
  [`README.upstream.md`](README.upstream.md).
- **[fastppcx86](https://github.com/daedalao/fastppcx86).** It supplied the PPC64LE JIT backend
  and code emitter, 64K page support and the self-modifying-code subsystem for POWER.

## License

MIT. See [`LICENSE`](LICENSE).

Arm is a trademark of Arm Limited and POWER is a trademark of IBM. POWERarm is an independent
project, not affiliated with or endorsed by either.
