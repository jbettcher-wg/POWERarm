# POWERarm: run AArch64 Linux programs on POWER

POWERarm is a planned AArch64 Linux user-mode emulator for **PPC64LE** hosts (POWER8 and later).
It translates guest A64 code to PPC64LE machine code at run time. It registers with
`binfmt_misc`, so AArch64 ELF binaries run the same way native ones do. When the host already
has a ppc64le build of a library or toolchain, POWERarm calls that build through a thunk
instead of emulating it. Everything else comes from an AArch64 rootfs.

Status: **design phase.** The tree is a history-sharing fork of fastppcx86 (`daedalao-wt`) and is still unmodified, so the top-level README is fastppcx86's until the M0 rename. Start with [`DESIGN.md`](DESIGN.md).

- **Minimum ISA: POWER8 (2.07).** POWER9 (ISA 3.0) paths sit behind a runtime `AT_HWCAP2`
  gate, following the same rule fastppcx86 uses (`powerpc64le-handbook/docs/power8-baseline-power9-paths.md`).
- **License target: MIT.** See the reuse analysis in `DESIGN.md` §2 for what can be copied
  in and what can only be used as a test oracle.
