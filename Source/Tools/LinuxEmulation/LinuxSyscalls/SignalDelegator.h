// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|common
$end_info$
*/


#pragma once

#include "LinuxSyscalls/Types.h"
#include "ArchHelpers/MContext.h"
#include "VDSO_Emulation.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/Telemetry.h>

namespace FEXCore::Context {
class Context;
}
namespace FEXCore::Core {
struct InternalThreadState;
}
namespace FEX::HLE {
enum class SignalEvent : uint32_t;
struct ThreadStateObject;
} // namespace FEX::HLE

namespace FEX::HLE {
using HostSignalDelegatorFunction = std::function<bool(FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext)>;
using HostSignalDelegatorFunctionForGuest =
  std::function<bool(FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext, GuestSigAction* GuestAction, stack_t* GuestStack)>;

class SignalDelegator final : public FEXCore::SignalDelegator, public FEXCore::Allocator::FEXAllocOperators {
public:
  constexpr static size_t MAX_SIGNALS {64};

  // Use the last signal just so we are less likely to ever conflict with something that the guest application is using
  // 64 is used internally by Valgrind
  constexpr static size_t SIGNAL_FOR_PAUSE {63};

  // Returns true if the host handled the signal
  // Arguments are the same as sigaction handler
  SignalDelegator(FEXCore::Context::Context* _CTX, const std::string_view ApplicationName, bool SupportsAVX);
  ~SignalDelegator() override;

  // Called from the signal trampoline function.
  void HandleSignal(FEX::HLE::ThreadStateObject* Thread, int Signal, void* Info, void* UContext);

  void RegisterTLSState(FEX::HLE::ThreadStateObject* Thread);
  void UninstallTLSState(FEX::HLE::ThreadStateObject* Thread);

  /**
   * @brief Registers a signal handler for the host to handle a signal
   *
   * It's a process level signal handler so one must be careful
   */
  void RegisterHostSignalHandler(int Signal, HostSignalDelegatorFunction Func, bool Required);

  /**
   * @brief Registers a signal handler for the host to handle a signal specifically for guest handling
   *
   * It's a process level signal handler so one must be careful
   */
  void RegisterHostSignalHandlerForGuest(int Signal, HostSignalDelegatorFunctionForGuest Func);
  void RegisterFrontendHostSignalHandler(int Signal, HostSignalDelegatorFunction Func, bool Required);

  /**
   * @name These functions are all for Linux signal emulation
   * @{ */
  /**
   * @brief Allows the guest to register a signal handler that is run after the host attempts to resolve the handler first
   */
  uint64_t RegisterGuestSignalHandler(int Signal, const GuestSigAction* Action, struct GuestSigAction* OldAction);

  uint64_t RegisterGuestSigAltStack(FEX::HLE::ThreadStateObject* Thread, const stack_t* ss, stack_t* old_ss);

  /**
   * @brief True when the thread has a signal that will ACTUALLY reach guest
   * code on the next drain: a deferred frame (or pending unblocked signal)
   * that is not guest-masked, not SIG_IGN, and not default-ignore.
   *
   * The distinction matters because the defer path queues frames BEFORE any
   * disposition or mask check — those are applied at drain time (masked →
   * parked in PendingSignals, SIG_IGN / default-ignore → dropped). A raw
   * "queue is non-empty" check therefore overcounts, and a caller using it
   * to synthesize an EINTR would fabricate interruptions for signals a real
   * kernel never lets interrupt a sleep (a SIG_IGN'd SIGCHLD, most
   * commonly — Wine takes those constantly).
   */
  bool ThreadHasDeliverableGuestSignal(FEX::HLE::ThreadStateObject* Thread);

  uint64_t GuestSigProcMask(FEX::HLE::ThreadStateObject* Thread, int how, const uint64_t* set, uint64_t* oldset);
  uint64_t GuestSigPending(FEX::HLE::ThreadStateObject* Thread, uint64_t* set, size_t sigsetsize);
  uint64_t GuestSigSuspend(FEX::HLE::ThreadStateObject* Thread, uint64_t* set, size_t sigsetsize);
  uint64_t GuestSigTimedWait(uint64_t* set, siginfo_t* info, const struct timespec* timeout, size_t sigsetsize);
  uint64_t GuestSignalFD(int fd, const uint64_t* set, size_t sigsetsize, int flags);
  /**  @} */

  /**
   * @brief Check to ensure the XID handler is still set to the FEX handler
   *
   * On a new thread GLIBC will set the XID handler underneath us.
   * After the first thread is created check this.
   */
  void CheckXIDHandler();

  void UninstallHostHandler(int Signal);
  void QueueSignal(pid_t tgid, pid_t tid, int Signal, siginfo_t* info, bool IgnoreMask);

  FEXCore::Context::Context* CTX;

  void SetVDSOSymbols() {
    // Get symbols from VDSO.
    VDSOPointers = FEX::VDSO::GetVDSOSymbols();
  }

  uintptr_t GetThunkCallbackRET() const override {
    return reinterpret_cast<uintptr_t>(VDSOPointers.VDSO_FEX_CallbackRET);
  }

  [[noreturn]]
  void HandleSignalHandlerReturn(bool RT) {
    using SignalHandlerReturnFunc = void (*)();

    SignalHandlerReturnFunc SignalHandlerReturn {};
    if (RT) {
      SignalHandlerReturn = reinterpret_cast<SignalHandlerReturnFunc>(Config.SignalHandlerReturnAddressRT);
    } else {
      SignalHandlerReturn = reinterpret_cast<SignalHandlerReturnFunc>(Config.SignalHandlerReturnAddress);
    }

    SignalHandlerReturn();
    FEX_UNREACHABLE;
  }

  /**
   * @brief Signals a thread with a specific core event.
   *
   * @param Thread Which thread to signal.
   * @param Event Which event to signal the event with.
   */
  void SignalThread(FEXCore::Core::InternalThreadState* Thread, SignalEvent Event);

  FEXCore::ArchHelpers::Arm64::UnalignedHandlerType GetUnalignedHandlerType() const {
    return UnalignedHandlerType;
  }

  void SaveTelemetry();

  void SpillSRA(FEXCore::Core::InternalThreadState* Thread, void* ucontext, uint32_t IgnoreMask);

private:
  // Called from the thunk handler to handle the signal
  void HandleGuestSignal(FEX::HLE::ThreadStateObject* ThreadObject, int Signal, void* Info, void* UContext);
  bool HandleFrontendSIGSEGV(FEXCore::Core::InternalThreadState* Thread, int Signal, void* Info, void* UContext);

  /**
   * @brief Registers a signal handler for the host to handle a signal
   *
   * It's a process level signal handler so one must be careful
   */
  void FrontendRegisterHostSignalHandler(int Signal, bool Required);
  void FrontendRegisterFrontendHostSignalHandler(int Signal, bool Required);

  void SetHostSignalHandler(int Signal, HostSignalDelegatorFunction Func, bool Required) {
    HostHandlers[Signal].Handlers.push_back(std::move(Func));
  }
  void SetFrontendHostSignalHandler(int Signal, HostSignalDelegatorFunction Func, bool Required) {
    HostHandlers[Signal].FrontendHandler = std::move(Func);
  }

  const fextl::string ApplicationName;
  FEX_CONFIG_OPT(HalfBarrierTSOEnabled, HALFBARRIERTSOENABLED);

  FEXCore::ArchHelpers::Arm64::UnalignedHandlerType UnalignedHandlerType {FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier};

  enum DefaultBehaviourType {
    DEFAULT_TERM,
    // Core dump based signals are supposed to have a coredump appear
    // For FEX's behaviour we don't really care right now
    DEFAULT_COREDUMP = DEFAULT_TERM,
    DEFAULT_IGNORE,
  };

  struct kernel_sigaction {
    union {
      void (*handler)(int);
      void (*sigaction)(int, siginfo_t*, void*);
    };

    uint64_t sa_flags;

    void (*restorer)();
    uint64_t sa_mask;
  };

  struct SignalHandler {
    std::atomic<bool> Installed {};
    std::atomic<bool> Required {};
    kernel_sigaction HostAction {};
    kernel_sigaction OldAction {};
    FEX::HLE::HostSignalDelegatorFunctionForGuest GuestHandler {};
    GuestSigAction GuestAction {};
    DefaultBehaviourType DefaultBehaviour {DEFAULT_TERM};

    // Callbacks
    fextl::vector<HostSignalDelegatorFunction> Handlers {};
    HostSignalDelegatorFunction FrontendHandler {};
  };

  std::array<SignalHandler, MAX_SIGNALS + 1> HostHandlers {};
  bool InstallHostThunk(int Signal);
  bool UpdateHostThunk(int Signal);

  FEX::VDSO::VDSOEntrypoints VDSOPointers {};

  bool IsAddressInDispatcher(uint64_t Address) const {
    return Address >= Config.DispatcherBegin && Address < Config.DispatcherEnd;
  }

  // The FABI bridge stubs are emitted past DispatcherEnd (see the config
  // struct comment). A PC here is a host-call crossing in flight: async
  // signals must defer exactly as for JIT-buffer PCs.
  bool IsAddressInFABIStubs(uint64_t Address) const {
    return Address >= Config.FABIStubsBegin && Address < Config.FABIStubsEnd;
  }

  ///< Returns true when the handler redirected the guest and execution resumes through the dispatcher.
  bool RestoreFrame_Arm64(FEXCore::Core::InternalThreadState* Thread, ArchHelpers::Context::ContextBackup* Context,
                          FEXCore::Core::CpuStateFrame* Frame, void* ucontext);

  ///< Setup the rt_sigframe for an AArch64 guest.
  uint64_t SetupFrame_Arm64(FEXCore::Core::InternalThreadState* Thread, ArchHelpers::Context::ContextBackup* ContextBackup,
                            FEXCore::Core::CpuStateFrame* Frame, int Signal, siginfo_t* HostSigInfo, void* ucontext,
                            GuestSigAction* GuestAction, stack_t* GuestStack, uint64_t NewGuestSP);

  enum class RestoreType {
    TYPE_REALTIME,    ///< Signal restore type is from a `realtime` signal.
    TYPE_NONREALTIME, ///< Signal restore type is from a `non-realtime` signal.
    TYPE_PAUSE,       ///< Signal restore type is from a GDB pause event.
  };
  // Where a guest handler's host frame goes (see "Abandoned guest handlers").
  struct HandlerPlacement {
    // Host SP to build the ContextBackup under; 0 = below the interrupted SP.
    uint64_t BaseSP {};
    // Interrupted host stack bytes to save with the backup: [SaveLo, SaveHi).
    uint64_t SaveLo {};
    uint64_t SaveHi {};
    uint64_t InterruptedBase {};
  };
  HandlerPlacement PlaceGuestHandler(FEXCore::Core::InternalThreadState* Thread, void* ucontext, bool WasInJIT);

public:
  ///< Guest syscall entry: marks the innermost guest handler abandoned if the guest SP has left its frame.
  void NoteGuestSyscall(FEXCore::Core::InternalThreadState* Thread);

private:

  ArchHelpers::Context::ContextBackup* StoreThreadState(FEXCore::Core::InternalThreadState* Thread, int Signal, void* ucontext,
                                                        const HandlerPlacement& Placement);
  void RestoreThreadState(FEXCore::Core::InternalThreadState* Thread, void* ucontext, RestoreType Type);
  bool HandleDispatcherGuestSignal(FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext,
                                   GuestSigAction* GuestAction, stack_t* GuestStack);
  bool HandleSignalPause(FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext);
  bool HandleSIGILL(FEXCore::Core::InternalThreadState* Thread, int Signal, void* info, void* ucontext);

  uint64_t GetNewSigMask(int Signal) const;

  std::mutex HostDelegatorMutex;
  std::mutex GuestDelegatorMutex;
  bool SupportsAVX;
};

fextl::unique_ptr<FEX::HLE::SignalDelegator>
CreateSignalDelegator(FEXCore::Context::Context* CTX, const std::string_view ApplicationName, bool SupportsAVX);
} // namespace FEX::HLE
