// SPDX-License-Identifier: MIT
//
// Syscall names the shared handlers under LinuxSyscalls/Syscalls/ register but
// the AArch64 (asm-generic) table does not have.
//
// The shared registration files were written for x86-64 and i386, both of which
// still carry the legacy calls asm-generic dropped (open, stat, fork, pipe, ...).
// Giving those names the value -1 lets the shared files compile unchanged;
// Arm64::RegisterSyscall ignores negative numbers, so they register nothing.
// Renamed entries map to their asm-generic number.
//
// POWERARM-M0-TODO(syscalls): move the legacy-only registrations out of the shared files (or behind an x86 guard) once fastppcx86 cherry-picks into them stop mattering, and delete this header.
#pragma once

#include "LinuxSyscalls/Arm64/SyscallsEnum.h"

namespace FEX::HLE::Arm64 {
enum LegacySyscalls_Arm64 {
  // Renamed in asm-generic.
  SYSCALL_Arm64_prlimit_64 = SYSCALL_Arm64_prlimit64,

  // Not present in asm-generic (arm64 userspace uses the *at / 2 / 1 variants).
  SYSCALL_Arm64__sysctl = -1,
  SYSCALL_Arm64_access = -1,
  SYSCALL_Arm64_afs_syscall = -1,
  SYSCALL_Arm64_alarm = -1,
  SYSCALL_Arm64_arch_prctl = -1,
  SYSCALL_Arm64_chmod = -1,
  SYSCALL_Arm64_chown = -1,
  SYSCALL_Arm64_creat = -1,
  SYSCALL_Arm64_create_module = -1,
  SYSCALL_Arm64_epoll_create = -1,
  SYSCALL_Arm64_eventfd = -1,
  SYSCALL_Arm64_fork = -1,
  SYSCALL_Arm64_get_kernel_syms = -1,
  SYSCALL_Arm64_getpgrp = -1,
  SYSCALL_Arm64_getpmsg = -1,
  SYSCALL_Arm64_inotify_init = -1,
  SYSCALL_Arm64_ioperm = -1,
  SYSCALL_Arm64_iopl = -1,
  SYSCALL_Arm64_lchown = -1,
  SYSCALL_Arm64_link = -1,
  SYSCALL_Arm64_mkdir = -1,
  SYSCALL_Arm64_mknod = -1,
  SYSCALL_Arm64_open = -1,
  SYSCALL_Arm64_pause = -1,
  SYSCALL_Arm64_pipe = -1,
  SYSCALL_Arm64_poll = -1,
  SYSCALL_Arm64_putpmsg = -1,
  SYSCALL_Arm64_query_module = -1,
  SYSCALL_Arm64_readlink = -1,
  SYSCALL_Arm64_rename = -1,
  SYSCALL_Arm64_rmdir = -1,
  SYSCALL_Arm64_signalfd = -1,
  SYSCALL_Arm64_symlink = -1,
  SYSCALL_Arm64_sysfs = -1,
  SYSCALL_Arm64_unlink = -1,
  SYSCALL_Arm64_uselib = -1,
  SYSCALL_Arm64_ustat = -1,
  SYSCALL_Arm64_vfork = -1,
  SYSCALL_Arm64_vserver = -1,
};
} // namespace FEX::HLE::Arm64
