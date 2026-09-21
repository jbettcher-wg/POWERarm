# POWERarm: run AArch64 Linux programs on POWER

POWERarm is an AArch64 Linux user-mode emulator for **PPC64LE** hosts (POWER8 and later).
It translates guest A64 code to PPC64LE machine code at run time. It registers with
`binfmt_misc`, so AArch64 ELF binaries run the same way native ones do. When the host already
has a ppc64le build of a library or toolchain, POWERarm calls that build through a thunk
instead of emulating it. Everything else comes from an AArch64 rootfs.

Status: **functional, experimental.** M1 (static binaries) and M2 (self-hosting GCC toolchain)
milestones are complete. Real-world applications including VS Code, Claude Code CLI, and 3D games
run on POWER9 hosts. Active development is focused on host-optimized NEON vector lowerings,
code cache tuning, and library thunks. See [`OPTIMIZATION-CHECKLIST.md`](OPTIMIZATION-CHECKLIST.md)
and the lowering catalogue in [`research/neon/NEON-LANDINGS.md`](research/neon/NEON-LANDINGS.md).

- **Minimum ISA: POWER8 (2.07).** POWER9 (ISA 3.0) paths sit behind a runtime `AT_HWCAP2`
  gate with verified POWER8 fallbacks.
- **License: MIT.** See the top-level [`LICENSE`](../../LICENSE).
