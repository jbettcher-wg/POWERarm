// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|syscalls-arm64
desc: clone and execve for the arm64 guest
$end_info$
*/

#include "LinuxSyscalls/Syscalls.h"
#include "LinuxSyscalls/Arm64/Syscalls.h"

#include <FEXCore/fextl/vector.h>

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <signal.h>

namespace FEX::HLE::Arm64 {
namespace {
  // Copies a NULL-terminated guest pointer array into a host vector. The
  // strings stay guest pointers; the host execveat faults on a bad one.
  // Returns false (EFAULT) if the array itself can't be read.
  bool CopyStringArray(char* const* Array, fextl::vector<const char*>& Out) {
    if (!Array) {
      return true;
    }
    for (size_t i = 0;; ++i) {
      const char* Entry {};
      if (FaultSafeUserMemAccess::CopyFromUser(&Entry, &Array[i], sizeof(Entry)) != 0) {
        return false;
      }
      if (!Entry) {
        break;
      }
      Out.push_back(Entry);
    }
    Out.push_back(nullptr);
    return true;
  }

} // namespace

void RegisterThread(FEX::HLE::SyscallHandler* Handler) {
  // arm64 selects CONFIG_CLONE_BACKWARDS: clone(flags, newsp, parent_tid, tls, child_tid).
  REGISTER_SYSCALL_IMPL(
    clone, ([](FEXCore::Core::CpuStateFrame* Frame, uint64_t flags, void* stack, pid_t* parent_tid, void* tls, pid_t* child_tid) -> uint64_t {
      FEX::HLE::clone3_args args {
        .Type = TypeOfClone::TYPE_CLONE2,
        .args =
          {
            .flags = flags & ~static_cast<uint64_t>(CSIGNAL),
            .pidfd = 0,
            .child_tid = reinterpret_cast<uint64_t>(child_tid),
            .parent_tid = reinterpret_cast<uint64_t>(parent_tid),
            .exit_signal = flags & CSIGNAL,
            .stack = reinterpret_cast<uint64_t>(stack),
            .stack_size = 0,
            .tls = reinterpret_cast<uint64_t>(tls),
            .set_tid = 0,
            .set_tid_size = 0,
            .cgroup = 0,
          },
      };
      // POWERARM-M1-TODO(syscalls): CLONE_THREAD guests run on FEX's thread path, which is untested for the arm64 guest; M1 covers fork-style clone only.
      return CloneHandler(Frame, &args);
    }));

  REGISTER_SYSCALL_IMPL(execve, [](FEXCore::Core::CpuStateFrame* Frame, const char* pathname, char* const argv[], char* const envp[]) -> uint64_t {
    fextl::vector<const char*> Args;
    fextl::vector<const char*> Envp;
    // ExecveHandler dereferences the path before the host kernel sees it.
    GuestPath Path(pathname);
    if (Path.error()) {
      return Path.error();
    }
    if (!CopyStringArray(argv, Args) || !CopyStringArray(envp, Envp)) {
      return -EFAULT;
    }
    auto* const* ArgsPtr = argv ? const_cast<char* const*>(Args.data()) : nullptr;
    auto* const* EnvpPtr = envp ? const_cast<char* const*>(Envp.data()) : nullptr;
    return FEX::HLE::ExecveHandler(Frame, Path.c_str(), ArgsPtr, EnvpPtr, FEX::HLE::ExecveAtArgs::Empty());
  });

  REGISTER_SYSCALL_IMPL(execveat, ([](FEXCore::Core::CpuStateFrame* Frame, int dirfd, const char* pathname, char* const argv[],
                                      char* const envp[], int flags) -> uint64_t {
                          fextl::vector<const char*> Args;
                          fextl::vector<const char*> Envp;
                          GuestPath Path(pathname);
                          if (Path.error()) {
                            return Path.error();
                          }
                          if (!CopyStringArray(argv, Args) || !CopyStringArray(envp, Envp)) {
                            return -EFAULT;
                          }
                          auto* const* ArgsPtr = argv ? const_cast<char* const*>(Args.data()) : nullptr;
                          auto* const* EnvpPtr = envp ? const_cast<char* const*>(Envp.data()) : nullptr;
                          FEX::HLE::ExecveAtArgs AtArgs {
                            .dirfd = dirfd,
                            .flags = flags,
                          };
                          return FEX::HLE::ExecveHandler(Frame, Path.c_str(), ArgsPtr, EnvpPtr, AtArgs);
                        }));
}
} // namespace FEX::HLE::Arm64
