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
  // The shared implementations under LinuxSyscalls/Syscalls/ take plain
  // integers, pointers and LP64 structs whose layout is the same for an
  // AArch64 guest, so they are wired straight to the asm-generic numbers.
  //
  // POWERARM-M0-TODO(syscalls): the shared handlers were written against x86-64 guest values; flag/struct translation (O_* flags, termios, ioctl encoding, epoll_event packing, struct stat) must be re-derived for arm64 -> powerpc64 (DESIGN.md §5).
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

  // Everything not registered above keeps the default UnimplementedSyscall
  // entry, which returns -ENOSYS.
  // POWERARM-M0-TODO(syscalls): the x86-64-specific handlers (mmap family, clone/exit, rt_sigaction/rt_sigreturn, fstat/newfstatat, prctl, arch-specific TLS) were deleted with LinuxSyscalls/x64 and need arm64 versions (M1).
}

fextl::unique_ptr<FEX::HLE::SyscallHandler>
CreateHandler(FEXCore::Context::Context* ctx, FEX::HLE::SignalDelegator* _SignalDelegation, FEX::HLE::ThunkHandler* ThunkHandler) {
  return fextl::make_unique<Arm64SyscallHandler>(ctx, _SignalDelegation, ThunkHandler);
}
} // namespace FEX::HLE::Arm64
