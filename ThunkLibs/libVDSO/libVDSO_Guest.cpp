
/*
$info$
tags: thunklibs|VDSO
desc: Linux VDSO thunking
$end_info$
*/

// The AArch64 guest's vDSO. POWERarm maps this file into every guest and hands
// its address over as AT_SYSINFO_EHDR (VDSO_Emulation.cpp LoadVDSOThunks), in
// place of the kernel vDSO an arm64 machine would provide. It exports what
// arch/arm64/kernel/vdso exports, under the same version (libVDSO_Guest.lds):
//
//   __kernel_clock_gettime, __kernel_gettimeofday, __kernel_clock_getres
//     Guest->host thunks. Each packs its arguments and hits the thunk marker
//     (common/Guest.h); the host answers from its own vDSO where it can, so a
//     guest clock read is neither a guest nor a host syscall. The kernel
//     convention holds: 0, or a negative errno.
//
//   __kernel_rt_sigreturn
//     Where a signal handler returns to. arm64 libcs leave sa_restorer unset
//     and the kernel points X30 at this symbol; POWERarm's signal delivery does
//     the same with the address it finds here. The code is the kernel's
//     (arch/arm64/kernel/vdso/sigreturn.S), a code label (STT_NOTYPE) as there,
//     and the NOP ahead of it is the kernel's unwinder marker: unwinders
//     recognise the `mov x8, #139; svc #0` pair at the return address as a
//     signal frame.

#include <sys/time.h>
#include <time.h>

#include "common/Guest.h"

#include "thunkgen_guest_libVDSO.inl"

extern "C" {
int __kernel_gettimeofday(struct timeval* tv, struct timezone* tz) __attribute__((alias("fexfn_pack_gettimeofday")));
int __kernel_clock_gettime(clockid_t, struct timespec*) __attribute__((alias("fexfn_pack_clock_gettime")));
int __kernel_clock_getres(clockid_t, struct timespec*) __attribute__((alias("fexfn_pack_clock_getres")));
}

asm(R"(
  .text
  .balign 4
  nop
  .globl __kernel_rt_sigreturn
__kernel_rt_sigreturn:
  mov x8, #139
  svc #0
  .size __kernel_rt_sigreturn, . - __kernel_rt_sigreturn
)");
