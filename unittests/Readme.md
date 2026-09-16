# Unit tests

Host-side tests only. The x86 guest suites inherited from FEX-Emu (ASM, 32Bit_ASM,
InstructionCountCI, gcc-target-tests, posixtest, gvisor, FEXLinuxTests) were removed in
POWERarm M0a; AArch64 guest suites replace them in later milestones.

- [APITests](APITests): Catch2 tests of host utility code (allocator, argument parser, filesystem, ...)
- [ThunkLibs](ThunkLibs): thunk generator tests (`BUILD_THUNKS=ON`)
- [Utilities](Utilities): test-run helpers
