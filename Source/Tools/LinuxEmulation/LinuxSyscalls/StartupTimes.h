// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/fextl/fmt.h>

#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

namespace FEX::HLE {
// POWERARM_STARTUPTIMES=1: one stderr line per process with the wall time of
// each startup and shutdown phase of main(), plus the CPU time the process had
// already used when main() was entered (host ld.so and static constructors).
// A guest exit_group reports from the syscall handler, a return from
// ExecuteThread from the end of main(). Off by default; when off, each mark is a predicted-not-taken branch.
struct StartupTimes {
  enum Phase {
    MAIN,
    CONFIG,
    SERVER,
    LOADER,
    CORE,
    MAP,
    GUEST,
    SAVE,
    END,
    COUNT
  };
  bool Enabled {};
  // Points at main()'s program name, which outlives every report.
  std::string_view Name {};
  uint64_t PreMainCPUNS {};
  uint64_t Marks[COUNT] {};

  static uint64_t Now(clockid_t Clock) {
    timespec TS {};
    clock_gettime(Clock, &TS);
    return static_cast<uint64_t>(TS.tv_sec) * 1000000000ULL + static_cast<uint64_t>(TS.tv_nsec);
  }
  void Init() {
    const char* Env = getenv("POWERARM_STARTUPTIMES");
    Enabled = Env && *Env && *Env != '0';
    if (Enabled) {
      PreMainCPUNS = Now(CLOCK_PROCESS_CPUTIME_ID);
      Marks[MAIN] = Now(CLOCK_MONOTONIC);
    }
  }
  void Mark(Phase P) {
    if (Enabled) [[unlikely]] {
      Marks[P] = Now(CLOCK_MONOTONIC);
    }
  }
  void Report() {
    if (!Enabled) {
      return;
    }
    Mark(END);
    auto MS = [&](Phase From, Phase To) {
      return (Marks[To] && Marks[From]) ? static_cast<double>(Marks[To] - Marks[From]) / 1e6 : -1.0;
    };
    rusage RU {};
    getrusage(RUSAGE_SELF, &RU);
    auto Line = fextl::fmt::format("POWERarm startup [{}] {}: premain-cpu {:.2f} config {:.2f} server {:.2f} loader {:.2f} core {:.2f} map {:.2f} "
                                   "guest {:.2f} save {:.2f} teardown {:.2f} main {:.2f} ms; user {:.2f} sys {:.2f} ms; minflt {} majflt {}\n",
                                   ::getpid(), Name, static_cast<double>(PreMainCPUNS) / 1e6, MS(MAIN, CONFIG), MS(CONFIG, SERVER),
                                   MS(SERVER, LOADER), MS(LOADER, CORE), MS(CORE, MAP), MS(MAP, GUEST), MS(GUEST, SAVE), MS(SAVE, END),
                                   MS(MAIN, END), RU.ru_utime.tv_sec * 1e3 + RU.ru_utime.tv_usec / 1e3,
                                   RU.ru_stime.tv_sec * 1e3 + RU.ru_stime.tv_usec / 1e3, RU.ru_minflt, RU.ru_majflt);
    (void)::write(STDERR_FILENO, Line.data(), Line.size());
  }
};
inline StartupTimes StartupTimer;
} // namespace FEX::HLE
