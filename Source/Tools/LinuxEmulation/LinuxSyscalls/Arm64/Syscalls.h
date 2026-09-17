// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|syscalls-arm64
$end_info$
*/

// AArch64 guest syscall table.
//
// Numbers are the asm-generic ones (Arm64/SyscallsEnum.h, generated). The
// dispatcher (SyscallHandler::HandleSyscall) indexes Definitions by the guest
// number in X8; every slot starts out as UnimplementedSyscall (-ENOSYS) and is
// filled by the Register* functions.
#pragma once

#include "LinuxSyscalls/Syscalls.h"
#include "LinuxSyscalls/Arm64/SyscallsEnum.h"
#include "LinuxSyscalls/Arm64/LegacySyscallsEnum.h"

#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/string.h>

namespace FEX::HLE {
class SignalDelegator;
class ThunkHandler;
} // namespace FEX::HLE

namespace FEX::HLE::Arm64 {
class Arm64SyscallHandler final : public FEX::HLE::SyscallHandler {
public:
  Arm64SyscallHandler(FEXCore::Context::Context* ctx, FEX::HLE::SignalDelegator* _SignalDelegation, FEX::HLE::ThunkHandler* ThunkHandler);

  void* GuestMmap(FEXCore::Core::InternalThreadState* Thread, void* addr, size_t length, int prot, int flags, int fd, off_t offset) override {
    return FEX::HLE::SyscallHandler::GuestMmap(true, Thread, addr, length, prot, flags, fd, offset);
  }
  uint64_t GuestMunmap(FEXCore::Core::InternalThreadState* Thread, void* addr, uint64_t length) override {
    return FEX::HLE::SyscallHandler::GuestMunmap(true, Thread, addr, length);
  }

  void RegisterSyscall_64(int SyscallNumber,
#ifdef DEBUG_STRACE
                          const fextl::string& TraceFormatString,
#endif
                          void* SyscallHandler, int ArgumentCount) override {
    auto& Def = Definitions.at(SyscallNumber);
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
    LOGMAN_THROW_A_FMT(Def.Ptr == reinterpret_cast<void*>(&UnimplementedSyscall), "Oops overwriting sysall problem, {}", SyscallNumber);
#endif
    Def.Ptr = SyscallHandler;
    Def.NumArgs = ArgumentCount;
#ifdef DEBUG_STRACE
    Def.StraceFmt = TraceFormatString;
#endif
  }

private:
  void RegisterSyscallHandlers();
};

// arm64-specific handlers: the ones whose arguments or results need arm64 ->
// ppc64le translation, or that have no shared implementation.
void RegisterMemory(FEX::HLE::SyscallHandler* Handler);
void RegisterFD(FEX::HLE::SyscallHandler* Handler);
void RegisterThread(FEX::HLE::SyscallHandler* Handler);

fextl::unique_ptr<FEX::HLE::SyscallHandler>
CreateHandler(FEXCore::Context::Context* ctx, FEX::HLE::SignalDelegator* _SignalDelegation, FEX::HLE::ThunkHandler* ThunkHandler);

// Deduces the argument count from the handler's signature and registers it.
template<typename R, typename... Args>
void RegisterSyscall(SyscallHandler* Handler, int SyscallNumber, const char* Name, R (*fn)(FEXCore::Core::CpuStateFrame* Frame, Args...)) {
  if (SyscallNumber < 0) {
    // A shared handler for a syscall AArch64 does not have (see LegacySyscallsEnum.h).
    return;
  }
#ifdef DEBUG_STRACE
  auto TraceFormatString = fextl::string(Name) + "(" + CollectArgsFmtString<Args...>() + ") = {}";
#endif
  Handler->RegisterSyscall_64(SyscallNumber,
#ifdef DEBUG_STRACE
                              TraceFormatString,
#endif
                              reinterpret_cast<void*>(fn), sizeof...(Args));
}

// Non-capturing lambdas convert to function pointers, but not during argument matching.
template<class F>
void RegisterSyscall(SyscallHandler* _Handler, int num, const char* name, F f) {
  RegisterSyscall(_Handler, num, name, +f);
}

} // namespace FEX::HLE::Arm64
