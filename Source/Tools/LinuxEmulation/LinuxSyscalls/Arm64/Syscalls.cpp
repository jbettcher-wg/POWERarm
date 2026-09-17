// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|syscalls-arm64
$end_info$
*/

#include "LinuxSyscalls/Syscalls.h"
#include "LinuxSyscalls/Arm64/Syscalls.h"
#include "LinuxSyscalls/Arm64/SyscallsEnum.h"

#include <FEXCore/HLE/SyscallHandler.h>

namespace FEX::HLE::Arm64 {
Arm64SyscallHandler::Arm64SyscallHandler(FEXCore::Context::Context* ctx, FEX::HLE::SignalDelegator* _SignalDelegation,
                                         FEX::HLE::ThunkHandler* ThunkHandler)
  : SyscallHandler {ctx, _SignalDelegation, ThunkHandler} {
  OSABI = FEXCore::HLE::SyscallOSABI::OS_LINUX64;

  RegisterSyscallHandlers();
}

void Arm64SyscallHandler::RegisterSyscallHandlers() {
  // The shared implementations under LinuxSyscalls/Syscalls/ are wired to
  // the asm-generic numbers. Where an argument differs between arm64 and
  // powerpc they translate it through Arm64/ABITranslation.h (O_* flags,
  // SOL_SOCKET option names); everything else they pass is identical on both,
  // which gen_abi_tables.py verifies (struct layouts, F_*, SA_*, signal
  // numbers, MADV_*, RLIMIT_*, ...). The handlers that need more than that
  // live in Arm64/.
  FEX::HLE::RegisterEpoll(this);
  FEX::HLE::RegisterFD(this);
  FEX::HLE::RegisterFS(this);
  FEX::HLE::RegisterInfo(this);
  FEX::HLE::RegisterIO(this);
  FEX::HLE::RegisterMemory(this);
  FEX::HLE::RegisterSignals(this);
  FEX::HLE::RegisterThread(this);
  FEX::HLE::RegisterTimer(this);
  FEX::HLE::RegisterNotImplemented(this);
  FEX::HLE::RegisterStubs(this);
  FEX::HLE::RegisterPassthrough(this);

  FEX::HLE::Arm64::RegisterMemory(this);
  FEX::HLE::Arm64::RegisterFD(this);
  FEX::HLE::Arm64::RegisterSignals(this);
  FEX::HLE::Arm64::RegisterThread(this);

  // Everything not registered above keeps the default UnimplementedSyscall
  // entry, which stops the process with an "Unhandled system call" message.
  // arm64 has no arch-specific TLS syscall (TPIDR_EL0 is set by clone's tls
  // argument and by MSR in guest code).
  // POWERARM-M1-TODO(syscalls): rt_sigreturn is wired to the delegator but real signal frames (fpsimd/esr records, the vDSO trampoline) are deferred; see the signals markers.
}

fextl::unique_ptr<FEX::HLE::SyscallHandler>
CreateHandler(FEXCore::Context::Context* ctx, FEX::HLE::SignalDelegator* _SignalDelegation, FEX::HLE::ThunkHandler* ThunkHandler) {
  return fextl::make_unique<Arm64SyscallHandler>(ctx, _SignalDelegation, ThunkHandler);
}
} // namespace FEX::HLE::Arm64
