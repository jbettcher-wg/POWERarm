// SPDX-License-Identifier: MIT
//
// Host-side tests for the arm64 -> ppc64le user-ABI translators.
//
// Three independent sources are compared:
//  - GeneratedABI.h: values the generator read out of objects compiled from the
//    kernel tree's uapi headers with clang for both targets;
//  - Arm64Reference.inc / Ppc64leReference.inc: the same names compiled natively
//    with gcc against each machine's installed uapi headers and run there;
//  - this machine's glibc headers, and the live kernel (a pty and fstat).
//
// Runs natively on the ppc64le host. Exit status is the number of failures.

#include "LinuxSyscalls/Arm64/ABITranslation.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>

using namespace FEX::HLE::Arm64::ABI;

namespace {
int Checks = 0;
int Failures = 0;

void Check(bool Ok, const char* Expr, const char* File, int Line, const std::string& Detail = {}) {
  ++Checks;
  if (!Ok) {
    ++Failures;
    fprintf(stderr, "FAIL %s:%d: %s %s\n", File, Line, Expr, Detail.c_str());
  }
}

std::string Hex(uint64_t A, uint64_t B) {
  char Buf[64];
  snprintf(Buf, sizeof(Buf), "(0x%" PRIx64 " vs 0x%" PRIx64 ")", A, B);
  return Buf;
}

#define CHECK(e) Check((e), #e, __FILE__, __LINE__)
#define CHECK_EQ(a, b) Check(static_cast<uint64_t>(a) == static_cast<uint64_t>(b), #a " == " #b, __FILE__, __LINE__, Hex(static_cast<uint64_t>(a), static_cast<uint64_t>(b)))

struct RefConst {
  const char* Name;
  uint64_t Generated;
  uint64_t Reference;
};

struct RefField {
  const char* Struct;
  const char* Field;
  size_t GeneratedOffset;
  size_t GeneratedSize;
  size_t ReferenceOffset;
  size_t ReferenceSize;
};

// Each reference file is included twice: once for its constants, once for its layouts.
#define REF(C, V) {#C, C, V},
#define SIZE(S, N)
#define FIELD(S, F, O, Z)
const RefConst GuestConsts[] = {
#include "Arm64Reference.inc"
};
const RefConst HostConsts[] = {
#include "Ppc64leReference.inc"
};
#undef REF
#undef SIZE
#undef FIELD

#define REF(C, V)
#define SIZE(S, N) {#S, "sizeof", 0, sizeof(S), 0, N},
#define FIELD(S, F, O, Z) {#S, #F, offsetof(S, F), sizeof(S::F), O, Z},
const RefField GuestFields[] = {
#include "Arm64Reference.inc"
};
const RefField HostFields[] = {
#include "Ppc64leReference.inc"
};
#undef REF
#undef SIZE
#undef FIELD

void TestReferences() {
  for (const auto& C : GuestConsts) {
    Check(C.Generated == C.Reference, C.Name, "Arm64Reference.inc", 0, Hex(C.Generated, C.Reference));
  }
  for (const auto& C : HostConsts) {
    Check(C.Generated == C.Reference, C.Name, "Ppc64leReference.inc", 0, Hex(C.Generated, C.Reference));
  }
  auto CheckFields = [](const RefField* Begin, const RefField* End) {
    for (auto* F = Begin; F != End; ++F) {
      std::string Name = std::string(F->Struct) + "." + F->Field;
      Check(F->GeneratedOffset == F->ReferenceOffset && F->GeneratedSize == F->ReferenceSize, Name.c_str(), "reference layout", 0,
            Hex(F->GeneratedOffset, F->ReferenceOffset) + " size " + Hex(F->GeneratedSize, F->ReferenceSize));
    }
  };
  CheckFields(std::begin(GuestFields), std::end(GuestFields));
  CheckFields(std::begin(HostFields), std::end(HostFields));
  printf("reference: %zu arm64 constants, %zu ppc64le constants, %zu+%zu struct fields\n", std::size(GuestConsts),
         std::size(HostConsts), std::size(GuestFields), std::size(HostFields));
}

// The HOST_* side must agree with what this machine's glibc compiles to.
void TestHostAgainstLibc() {
  CHECK_EQ(HOST_O_DIRECTORY, O_DIRECTORY);
  CHECK_EQ(HOST_O_NOFOLLOW, O_NOFOLLOW);
  CHECK_EQ(HOST_O_DIRECT, O_DIRECT);
  CHECK_EQ(HOST_O_LARGEFILE, 0200000); // glibc defines O_LARGEFILE as 0 on 64-bit; this is asm/fcntl.h
  CHECK_EQ(HOST_O_CLOEXEC, O_CLOEXEC);
  CHECK_EQ(HOST_O_TMPFILE, O_TMPFILE);
  CHECK_EQ(HOST_MAP_NORESERVE, MAP_NORESERVE);
  CHECK_EQ(HOST_MAP_LOCKED, MAP_LOCKED);
  CHECK_EQ(HOST_MAP_FIXED_NOREPLACE, MAP_FIXED_NOREPLACE);
  CHECK_EQ(HOST_MCL_CURRENT, MCL_CURRENT);
  CHECK_EQ(HOST_MCL_FUTURE, MCL_FUTURE);
  CHECK_EQ(HOST_MCL_ONFAULT, MCL_ONFAULT);
  CHECK_EQ(HOST_ICANON, ICANON);
  CHECK_EQ(HOST_ECHO, ECHO);
  CHECK_EQ(HOST_IXON, IXON);
  CHECK_EQ(HOST_CSIZE, CSIZE);
  CHECK_EQ(HOST_CS8, CS8);
  CHECK_EQ(HOST_VMIN, VMIN);
  CHECK_EQ(HOST_VTIME, VTIME);
  // Not compared with libc: glibc's TCGETS family expands with sizeof(glibc struct
  // termios), 60 bytes, and so does its Bnnn speeds (plain numbers since 2.42).
  // The kernel's numbers are in Ppc64leReference.inc, and TestTermios drives a
  // real pty with HOST_TCGETS/HOST_TCSETS.
  CHECK_EQ(HOST_TIOCGWINSZ, TIOCGWINSZ);
  CHECK_EQ(HOST_TIOCGPGRP, TIOCGPGRP);
  CHECK_EQ(HOST_TIOCGPTN, TIOCGPTN);
  CHECK_EQ(HOST_FIONREAD, FIONREAD);
  CHECK_EQ(HOST_EDEADLOCK, EDEADLOCK);
  CHECK_EQ(HOST_EDEADLK, EDEADLK);
  // glibc's struct stat on ppc64le is the kernel's.
  CHECK_EQ(sizeof(struct stat), sizeof(HostStat));
  CHECK_EQ(offsetof(struct stat, st_nlink), offsetof(HostStat, st_nlink));
  CHECK_EQ(offsetof(struct stat, st_mode), offsetof(HostStat, st_mode));
  CHECK_EQ(offsetof(struct stat, st_blksize), offsetof(HostStat, st_blksize));
  CHECK_EQ(offsetof(struct stat, st_mtim), offsetof(HostStat, st_mtime_sec));
}

void TestFlagTables() {
  // Every bit round-trips, and no two guest bits land on the same host bit.
  for (const auto* Set : {&OpenFlags, &ProtFlags, &MapFlags, &MclFlags, &TermiosIFlag, &TermiosOFlag, &TermiosCFlag, &TermiosLFlag}) {
    uint64_t HostSeen = 0;
    for (size_t i = 0; i < Set->NumBits; ++i) {
      const auto& B = Set->Bits[i];
      uint64_t Unknown = ~0ULL;
      CHECK_EQ(FlagsToHost(*Set, B.Guest, &Unknown), B.Host);
      CHECK_EQ(Unknown, 0);
      CHECK_EQ(FlagsToGuest(*Set, B.Host, &Unknown), B.Guest);
      CHECK_EQ(Unknown, 0);
      CHECK((HostSeen & B.Host) == 0);
      HostSeen |= B.Host;
    }
    for (size_t i = 0; i < Set->NumFields; ++i) {
      const auto& F = Set->Fields[i];
      for (size_t j = 0; j < F.NumValues; ++j) {
        CHECK_EQ(FlagsToHost(*Set, F.Values[j].Guest) & F.HostMask, F.Values[j].Host);
        CHECK_EQ(FlagsToGuest(*Set, F.Values[j].Host) & F.GuestMask, F.Values[j].Guest);
      }
    }
  }

  // The differences that matter, spelled against the libc of this host.
  const uint64_t GuestOpen = GUEST_O_DIRECTORY | GUEST_O_NOFOLLOW | GUEST_O_DIRECT | GUEST_O_LARGEFILE | GUEST_O_CLOEXEC | GUEST_O_RDWR;
  CHECK_EQ(OpenFlagsToHost(GuestOpen), O_DIRECTORY | O_NOFOLLOW | O_DIRECT | 0200000 | O_CLOEXEC | O_RDWR);
  CHECK_EQ(OpenFlagsToGuest(O_DIRECTORY | O_NOFOLLOW | O_DIRECT | 0200000 | O_CLOEXEC | O_RDWR), GuestOpen);
  CHECK_EQ(OpenFlagsToHost(GUEST_O_TMPFILE | GUEST_O_WRONLY), O_TMPFILE | O_WRONLY);
  CHECK_EQ(OpenFlagsToHost(3), 3); // O_ACCMODE 3 (ioctl-only open) stays
  CHECK_EQ(OpenFlagsToHost(GUEST_O_DIRECT), 0400000);
  CHECK_EQ(OpenFlagsToHost(GUEST_O_LARGEFILE), 0200000);

  uint64_t Unknown {};
  CHECK_EQ(FlagsToHost(MapFlags, GUEST_MAP_PRIVATE | GUEST_MAP_ANONYMOUS | GUEST_MAP_NORESERVE | GUEST_MAP_STACK, &Unknown),
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK);
  CHECK_EQ(Unknown, 0);
  CHECK_EQ(FlagsToHost(MapFlags, GUEST_MAP_SHARED_VALIDATE | GUEST_MAP_LOCKED), MAP_SHARED_VALIDATE | MAP_LOCKED);

  // PROT_BTI shares its value with powerpc PROT_SAO; it must never become SAO.
  CHECK_EQ(GUEST_PROT_BTI, HOST_PROT_SAO);
  CHECK_EQ(FlagsToHost(ProtFlags, GUEST_PROT_READ | GUEST_PROT_BTI | GUEST_PROT_MTE, &Unknown), PROT_READ);
  CHECK_EQ(Unknown, GUEST_PROT_BTI | GUEST_PROT_MTE);

  CHECK_EQ(FlagsToHost(MclFlags, GUEST_MCL_CURRENT | GUEST_MCL_FUTURE | GUEST_MCL_ONFAULT), MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT);
  CHECK_EQ(FlagsToHost(MclFlags, 8, &Unknown), 0);
  CHECK_EQ(Unknown, 8);
}

void TestTermios() {
  // A cooked terminal, as arm64 userspace would pass it.
  GuestTermios G {};
  G.c_iflag = GUEST_ICRNL | GUEST_IXON | GUEST_IUTF8 | GUEST_IMAXBEL;
  G.c_oflag = GUEST_OPOST | GUEST_ONLCR | GUEST_NL1 | GUEST_CR2 | GUEST_TAB3 | GUEST_BS1 | GUEST_VT1 | GUEST_FF1;
  G.c_cflag = GUEST_CS7 | GUEST_CREAD | GUEST_PARENB | GUEST_HUPCL | GUEST_CLOCAL | GUEST_B115200 | (GUEST_B57600 << GUEST_IBSHIFT) | GUEST_CRTSCTS;
  G.c_lflag = GUEST_ISIG | GUEST_ICANON | GUEST_ECHO | GUEST_ECHOE | GUEST_ECHOK | GUEST_ECHOCTL | GUEST_ECHOKE | GUEST_IEXTEN | GUEST_TOSTOP | GUEST_NOFLSH;
  G.c_line = 3;
  for (int i = 0; i < 19; ++i) {
    G.c_cc[i] = 0x40 + i;
  }

  HostTermios H {};
  H.c_ispeed = 1234;
  H.c_ospeed = 5678;
  TermiosToHost(G, &H);
  CHECK_EQ(H.c_iflag, ICRNL | IXON | IUTF8 | IMAXBEL);
  CHECK_EQ(H.c_oflag, OPOST | ONLCR | NL1 | CR2 | TAB3 | BS1 | VT1 | FF1);
  CHECK_EQ(H.c_cflag, CS7 | CREAD | PARENB | HUPCL | CLOCAL | HOST_B115200 | (HOST_B57600 << HOST_IBSHIFT) | CRTSCTS);
  CHECK_EQ(H.c_lflag, ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN | TOSTOP | NOFLSH);
  CHECK_EQ(H.c_line, 3);
  CHECK_EQ(H.c_ispeed, 1234); // preserved for the caller
  CHECK_EQ(H.c_cc[VINTR], 0x40 + GUEST_VINTR);
  CHECK_EQ(H.c_cc[VMIN], 0x40 + GUEST_VMIN);
  CHECK_EQ(H.c_cc[VTIME], 0x40 + GUEST_VTIME);
  CHECK_EQ(H.c_cc[VEOL], 0x40 + GUEST_VEOL);
  CHECK_EQ(H.c_cc[VEOL2], 0x40 + GUEST_VEOL2);
  CHECK_EQ(H.c_cc[VSTART], 0x40 + GUEST_VSTART);
  CHECK_EQ(H.c_cc[VSUSP], 0x40 + GUEST_VSUSP);
  CHECK_EQ(H.c_cc[VWERASE], 0x40 + GUEST_VWERASE);
  CHECK_EQ(H.c_cc[VDISCARD], 0x40 + GUEST_VDISCARD);

  GuestTermios Back {};
  TermiosToGuest(H, &Back);
  CHECK_EQ(Back.c_iflag, G.c_iflag);
  CHECK_EQ(Back.c_oflag, G.c_oflag);
  CHECK_EQ(Back.c_cflag, G.c_cflag);
  CHECK_EQ(Back.c_lflag, G.c_lflag);
  CHECK_EQ(Back.c_line, G.c_line);
  for (int i = 0; i <= 16; ++i) {
    CHECK_EQ(Back.c_cc[i], G.c_cc[i]);
  }

  // BOTHER and termios2 speeds.
  GuestTermios2 G2 {};
  memcpy(&G2, &G, sizeof(G));
  G2.c_cc[17] = G2.c_cc[18] = 0; // unnamed slots are not carried
  G2.c_cflag = (G2.c_cflag & ~GUEST_CBAUD) | GUEST_BOTHER;
  G2.c_ispeed = 31250;
  G2.c_ospeed = 250000;
  Termios2ToHost(G2, &H);
  CHECK_EQ(H.c_cflag & CBAUD, BOTHER);
  CHECK_EQ(H.c_ospeed, 250000);
  GuestTermios2 Back2 {};
  Termios2ToGuest(H, &Back2);
  CHECK(memcmp(&Back2, &G2, sizeof(G2)) == 0);

  // powerpc-only NL2/NL3 have no arm64 value: dropped to NL0.
  HostTermios HNL {};
  HNL.c_oflag = OPOST | NL3;
  TermiosToGuest(HNL, &Back);
  CHECK_EQ(Back.c_oflag, GUEST_OPOST);

  // Live: a pty's termios survives host -> guest -> host exactly.
  int Master = posix_openpt(O_RDWR | O_NOCTTY);
  CHECK(Master >= 0);
  if (Master >= 0 && grantpt(Master) == 0 && unlockpt(Master) == 0) {
    int Slave = open(ptsname(Master), O_RDWR | O_NOCTTY);
    CHECK(Slave >= 0);
    HostTermios Live {};
    CHECK(syscall(SYS_ioctl, Slave, (unsigned long)HOST_TCGETS, &Live) == 0);
    GuestTermios LiveGuest {};
    TermiosToGuest(Live, &LiveGuest);
    HostTermios Again = Live;
    TermiosToHost(LiveGuest, &Again);
    CHECK(memcmp(&Again, &Live, sizeof(Live)) == 0);
    // Set raw mode through the guest struct, and read it back through the host.
    LiveGuest.c_lflag &= ~(GUEST_ICANON | GUEST_ECHO | GUEST_ISIG);
    LiveGuest.c_cc[GUEST_VMIN] = 1;
    LiveGuest.c_cc[GUEST_VTIME] = 0;
    TermiosToHost(LiveGuest, &Again);
    CHECK(syscall(SYS_ioctl, Slave, (unsigned long)HOST_TCSETS, &Again) == 0);
    struct termios Libc {};
    CHECK(tcgetattr(Slave, &Libc) == 0);
    CHECK((Libc.c_lflag & (ICANON | ECHO | ISIG)) == 0);
    CHECK_EQ(Libc.c_cc[VMIN], 1);
    CHECK_EQ(Libc.c_cc[VTIME], 0);
    close(Slave);
  }
  if (Master >= 0) {
    close(Master);
  }
}

void TestStat() {
  HostStat H {};
  H.st_dev = 0x1122334455667788ULL;
  H.st_ino = 0x0102030405060708ULL;
  H.st_nlink = 7;
  H.st_mode = 0100644;
  H.st_uid = 1000;
  H.st_gid = 1001;
  H.st_rdev = 0x10203;
  H.st_size = 0x7fffffffffLL;
  H.st_blksize = 4096;
  H.st_blocks = 12345;
  H.st_atime_sec = 1700000000;
  H.st_atime_nsec = 111;
  H.st_mtime_sec = static_cast<uint64_t>(-5);
  H.st_mtime_nsec = 222;
  H.st_ctime_sec = 1800000000;
  H.st_ctime_nsec = 333;

  GuestStat G {};
  CHECK(ConvertStat(H, &G));
  // Read back at the offsets the arm64 reference measured, not through GuestStat.
  const auto* Raw = reinterpret_cast<const uint8_t*>(&G);
  auto At = [&](const char* Field) -> uint64_t {
    for (const auto& F : GuestFields) {
      if (strcmp(F.Struct, "GuestStat") == 0 && strcmp(F.Field, Field) == 0) {
        uint64_t V = 0;
        memcpy(&V, Raw + F.ReferenceOffset, F.ReferenceSize);
        if (F.ReferenceSize == 4 && strcmp(Field, "st_blksize") == 0) {
          V = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(V)));
        }
        return V;
      }
    }
    Check(false, Field, "stat reference field missing", 0);
    return 0;
  };
  CHECK_EQ(At("st_dev"), H.st_dev);
  CHECK_EQ(At("st_ino"), H.st_ino);
  CHECK_EQ(At("st_nlink"), 7);
  CHECK_EQ(At("st_mode"), 0100644);
  CHECK_EQ(At("st_uid"), 1000);
  CHECK_EQ(At("st_gid"), 1001);
  CHECK_EQ(At("st_rdev"), 0x10203);
  CHECK_EQ(At("st_size"), 0x7fffffffffLL);
  CHECK_EQ(At("st_blksize"), 4096);
  CHECK_EQ(At("st_blocks"), 12345);
  CHECK_EQ(At("st_atime_sec"), 1700000000);
  CHECK_EQ(At("st_atime_nsec"), 111);
  CHECK_EQ(At("st_mtime_sec"), static_cast<uint64_t>(-5));
  CHECK_EQ(At("st_mtime_nsec"), 222);
  CHECK_EQ(At("st_ctime_sec"), 1800000000);
  CHECK_EQ(At("st_ctime_nsec"), 333);

  H.st_nlink = 1ULL << 32;
  CHECK(!ConvertStat(H, &G)); // EOVERFLOW, as the arm64 kernel's cp_new_stat

  // Live: raw fstat into HostStat agrees with glibc's fstat.
  HostStat Raw2 {};
  struct stat Libc {};
  CHECK(syscall(SYS_fstat, 0, &Raw2) == 0 || errno == EBADF);
  if (fstat(0, &Libc) == 0) {
    CHECK_EQ(Raw2.st_mode, Libc.st_mode);
    CHECK_EQ(Raw2.st_ino, Libc.st_ino);
    CHECK_EQ(Raw2.st_mtime_nsec, Libc.st_mtim.tv_nsec);
  }
}

void TestIoctl() {
  // _IOC re-encoding, checked against this host's own request numbers.
  uint32_t Host {};
  CHECK(IoctlRequestToHost(GUEST_TIOCGPTN, &Host));
  CHECK_EQ(Host, TIOCGPTN);
  CHECK(IoctlRequestToHost(GUEST_TIOCSPTLCK, &Host));
  CHECK_EQ(Host, TIOCSPTLCK);
  CHECK(IoctlRequestToHost(GUEST_TIOCGPTPEER, &Host));
  CHECK_EQ(Host, 0x5441); // legacy 'T' request with no size: unchanged
  CHECK(IoctlRequestToHost(GUEST_TIOCSCTTY, &Host));
  CHECK_EQ(Host, TIOCSCTTY);
  // _IOWR keeps both direction bits; the size moves from 14 to 13 bits.
  constexpr uint32_t GuestIOWR = (3U << 30) | (0x40 << 16) | ('d' << 8) | 0x00;
  CHECK(IoctlRequestToHost(GuestIOWR, &Host));
  CHECK_EQ(Host, (6U << 29) | (0x40 << 16) | ('d' << 8));
  constexpr uint32_t GuestIO = ('d' << 8) | 0x01;
  CHECK(IoctlRequestToHost(GuestIO, &Host));
  CHECK_EQ(Host, (1U << 29) | ('d' << 8) | 0x01);
  constexpr uint32_t TooBig = (2U << 30) | (0x2000 << 16) | ('x' << 8);
  CHECK(!IoctlRequestToHost(TooBig, &Host));
  uint32_t Guest {};
  CHECK(IoctlRequestToGuest(TIOCGPTN, &Guest));
  CHECK_EQ(Guest, GUEST_TIOCGPTN);

  // The terminal table: every entry that has a host request names the host's own number.
  const struct {
    uint32_t Guest;
    unsigned long Host;
  } Known[] = {
    {GUEST_TCGETS, HOST_TCGETS},    {GUEST_TCSETS, HOST_TCSETS},    {GUEST_TCSETSW, HOST_TCSETSW}, {GUEST_TCSETSF, HOST_TCSETSF},
    {GUEST_TIOCGWINSZ, TIOCGWINSZ}, {GUEST_TIOCSWINSZ, TIOCSWINSZ}, {GUEST_TIOCGPGRP, TIOCGPGRP}, {GUEST_TIOCSPGRP, TIOCSPGRP},
    {GUEST_FIONREAD, FIONREAD},     {GUEST_FIONBIO, FIONBIO},       {GUEST_FIOCLEX, FIOCLEX},     {GUEST_TCFLSH, TCFLSH},
    {GUEST_TIOCGPTN, TIOCGPTN},     {GUEST_TIOCSPTLCK, TIOCSPTLCK}, {GUEST_TCSBRK, TCSBRK},       {GUEST_TIOCOUTQ, TIOCOUTQ},
  };
  for (const auto& K : Known) {
    const auto* E = FindTerminalIoctl(K.Guest);
    CHECK(E != nullptr);
    if (E) {
      CHECK_EQ(E->Host, static_cast<uint32_t>(K.Host));
    }
  }
  const auto* T2 = FindTerminalIoctl(GUEST_TCGETS2);
  CHECK(T2 && T2->Arg == IoctlArg::Termios2Get && T2->Host == 0);
  CHECK(FindTerminalIoctl(GUEST_TCGETS)->Arg == IoctlArg::TermiosGet);
  CHECK(FindTerminalIoctl(GUEST_TIOCGSERIAL)->Arg == IoctlArg::Unsupported);
}

void TestErrno() {
  CHECK_EQ(HostResultToGuest(static_cast<uint64_t>(-EDEADLOCK)), static_cast<uint64_t>(-GUEST_EDEADLOCK));
  CHECK_EQ(GUEST_EDEADLOCK, 35);
  CHECK_EQ(HostResultToGuest(static_cast<uint64_t>(-EDEADLK)), static_cast<uint64_t>(-35));
  CHECK_EQ(HostResultToGuest(58), 58); // a positive result is not an errno
  CHECK_EQ(HostResultToGuest(static_cast<uint64_t>(-ENOENT)), static_cast<uint64_t>(-ENOENT));
}

void TestSocketOptions() {
  CHECK_EQ(SocketOptionToHost(GUEST_SO_PASSCRED), SO_PASSCRED);
  CHECK_EQ(SocketOptionToHost(GUEST_SO_PEERCRED), SO_PEERCRED);
  CHECK_EQ(SocketOptionToHost(GUEST_SO_RCVLOWAT), SO_RCVLOWAT);
  CHECK_EQ(SocketOptionToHost(GUEST_SO_SNDTIMEO_OLD), 19);
  CHECK_EQ(SocketOptionToHost(GUEST_SO_REUSEADDR), SO_REUSEADDR);
}
} // namespace

int main() {
  TestReferences();
  TestHostAgainstLibc();
  TestFlagTables();
  TestTermios();
  TestStat();
  TestIoctl();
  TestErrno();
  TestSocketOptions();
  printf("%d checks, %d failures\n", Checks, Failures);
  return Failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
