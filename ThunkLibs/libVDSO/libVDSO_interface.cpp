#include <common/GeneratorInterface.h>

#include <sys/time.h>
#include <sys/types.h>
#include <time.h>

template<auto>
struct fex_gen_config {};

// The arm64 kernel vDSO's clock entry points (arch/arm64/kernel/vdso). Each is
// served on the host by FEX::VDSO (Source/Tools/LinuxEmulation/VDSO_Emulation.cpp),
// not by a host library, so this interface generates guest packers only.
//
// Not here: time and getcpu (the arm64 vDSO has neither) and getrandom
// (__kernel_getrandom, arm64 since Linux 6.12; glibc then runs its vgetrandom
// state machine through it -- left for later, getrandom stays a syscall).
template<>
struct fex_gen_config<gettimeofday> {};
template<>
struct fex_gen_config<clock_gettime> {};
template<>
struct fex_gen_config<clock_getres> {};
