// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|syscalls-arm64
desc: arm64 guest <-> ppc64le host argument and struct translation
$end_info$
*/

// Translation helpers over the generated tables in GeneratedABI.h. Everything
// here is a pure function of its inputs so the host-side unit tests
// (Arm64/Tests) exercise exactly what the syscall handlers run.
#pragma once

#include "LinuxSyscalls/Arm64/GeneratedABI.h"

#include <cstdint>
#include <cstring>

namespace FEX::HLE::Arm64::ABI {

enum class Direction {
  ToHost,
  ToGuest,
};

// Translates a flag word through a FlagSet.
//
// - A bit is translated by name. A bit set in the input that the table does
//   not know is dropped and reported through Unknown.
// - A field (O_ACCMODE, MAP_TYPE, CBAUD, NLDLY, ...) is translated by value. A
//   value with no named counterpart is kept as-is when both architectures use
//   the same mask for the field (so O_ACCMODE 3 or an invalid MAP_TYPE still
//   reaches the kernel and gets the kernel's own answer); otherwise it is
//   dropped and reported through Unknown.
inline uint64_t TranslateFlags(const FlagSet& Set, uint64_t In, Direction Dir, uint64_t* Unknown = nullptr) {
  const bool ToHost = Dir == Direction::ToHost;
  uint64_t Out {};
  uint64_t Known {};
  uint64_t Lost {};

  for (size_t i = 0; i < Set.NumBits; ++i) {
    const auto& Bit = Set.Bits[i];
    const uint64_t From = ToHost ? Bit.Guest : Bit.Host;
    const uint64_t To = ToHost ? Bit.Host : Bit.Guest;
    Known |= From;
    if (In & From) {
      Out |= To;
    }
  }

  for (size_t i = 0; i < Set.NumFields; ++i) {
    const auto& Field = Set.Fields[i];
    const uint64_t FromMask = ToHost ? Field.GuestMask : Field.HostMask;
    const uint64_t ToMask = ToHost ? Field.HostMask : Field.GuestMask;
    Known |= FromMask;
    const uint64_t Value = In & FromMask;
    bool Found = false;
    for (size_t j = 0; j < Field.NumValues; ++j) {
      const auto& V = Field.Values[j];
      if ((ToHost ? V.Guest : V.Host) == Value) {
        Out |= ToHost ? V.Host : V.Guest;
        Found = true;
        break;
      }
    }
    if (!Found) {
      if (FromMask == ToMask) {
        Out |= Value;
      } else {
        Lost |= Value;
      }
    }
  }

  if (Unknown) {
    *Unknown = (In & ~Known) | Lost;
  }
  return Out;
}

inline uint64_t FlagsToHost(const FlagSet& Set, uint64_t In, uint64_t* Unknown = nullptr) {
  return TranslateFlags(Set, In, Direction::ToHost, Unknown);
}

inline uint64_t FlagsToGuest(const FlagSet& Set, uint64_t In, uint64_t* Unknown = nullptr) {
  return TranslateFlags(Set, In, Direction::ToGuest, Unknown);
}

// O_* flags. Unknown bits are dropped: open(2) ignores bits it does not know,
// so dropping them is what an arm64 kernel does too.
inline int OpenFlagsToHost(uint64_t GuestFlags) {
  return static_cast<int>(FlagsToHost(OpenFlags, static_cast<uint32_t>(GuestFlags)));
}

inline uint64_t OpenFlagsToGuest(uint64_t HostFlags) {
  return FlagsToGuest(OpenFlags, static_cast<uint32_t>(HostFlags));
}

///// termios /////

// Copies the flag words, line discipline and control characters. The host
// speeds are left as they are: the caller loads them from the host tty first
// (TCSETS on an asm-generic kernel does not take speeds from userspace either).
inline void TermiosToHost(const GuestTermios& G, HostTermios* H) {
  H->c_iflag = FlagsToHost(TermiosIFlag, G.c_iflag);
  H->c_oflag = FlagsToHost(TermiosOFlag, G.c_oflag);
  H->c_cflag = FlagsToHost(TermiosCFlag, G.c_cflag);
  H->c_lflag = FlagsToHost(TermiosLFlag, G.c_lflag);
  H->c_line = G.c_line;
  for (const auto& Index : TermiosCC) {
    H->c_cc[Index.Host] = G.c_cc[Index.Guest];
  }
}

inline void TermiosToGuest(const HostTermios& H, GuestTermios* G) {
  *G = GuestTermios {};
  G->c_iflag = FlagsToGuest(TermiosIFlag, H.c_iflag);
  G->c_oflag = FlagsToGuest(TermiosOFlag, H.c_oflag);
  G->c_cflag = FlagsToGuest(TermiosCFlag, H.c_cflag);
  G->c_lflag = FlagsToGuest(TermiosLFlag, H.c_lflag);
  G->c_line = H.c_line;
  for (const auto& Index : TermiosCC) {
    G->c_cc[Index.Guest] = H.c_cc[Index.Host];
  }
}

// struct termios2 is struct termios plus the speeds on arm64. powerpc's own
// struct termios already carries the speeds, so TCGETS2/TCSETS*2 map onto the
// host's TCGETS/TCSETS* family.
inline void Termios2ToHost(const GuestTermios2& G, HostTermios* H) {
  GuestTermios Base {};
  memcpy(&Base, &G, sizeof(Base));
  static_assert(offsetof(GuestTermios2, c_cc) == offsetof(GuestTermios, c_cc));
  TermiosToHost(Base, H);
  H->c_ispeed = G.c_ispeed;
  H->c_ospeed = G.c_ospeed;
}

inline void Termios2ToGuest(const HostTermios& H, GuestTermios2* G) {
  GuestTermios Base {};
  TermiosToGuest(H, &Base);
  *G = GuestTermios2 {};
  memcpy(G, &Base, sizeof(Base));
  G->c_ispeed = H.c_ispeed;
  G->c_ospeed = H.c_ospeed;
}

///// ioctl request encoding /////

inline const IoctlEntry* FindTerminalIoctl(uint32_t GuestRequest) {
  for (const auto& Entry : TerminalIoctls) {
    if (Entry.Guest == GuestRequest) {
      return &Entry;
    }
  }
  return nullptr;
}

// Re-encodes an _IOC request from the asm-generic layout (2 direction bits,
// 14 size bits) to the powerpc one (3 direction bits, 13 size bits). Returns
// false when the size does not fit powerpc's 13 bits.
//
// A request with no direction and no size is either _IO() or a legacy
// hand-numbered request; the two cannot be told apart from the number. Legacy
// terminal numbers (type 'T') are the same on powerpc and pass unchanged;
// everything else is taken to be _IO() and gets powerpc's explicit _IOC_NONE.
inline bool IoctlRequestToHost(uint32_t Guest, uint32_t* Host) {
  const uint32_t Dir = (Guest >> GUEST_U_IOC_DIRSHIFT) & GUEST_U_IOC_DIRMASK;
  const uint32_t Size = (Guest >> GUEST_U_IOC_SIZESHIFT) & GUEST_U_IOC_SIZEMASK;
  const uint32_t Low = Guest & ((1U << GUEST_U_IOC_SIZESHIFT) - 1);
  const uint32_t Type = (Guest >> GUEST_U_IOC_TYPESHIFT) & GUEST_U_IOC_TYPEMASK;

  if (Dir == GUEST_U_IOC_NONE && Size == 0 && Type == 'T') {
    *Host = Guest;
    return true;
  }
  if (Size > HOST_U_IOC_SIZEMASK) {
    return false;
  }

  uint32_t HostDir {};
  if (Dir == GUEST_U_IOC_NONE) {
    HostDir = HOST_U_IOC_NONE;
  }
  if (Dir & GUEST_U_IOC_READ) {
    HostDir |= HOST_U_IOC_READ;
  }
  if (Dir & GUEST_U_IOC_WRITE) {
    HostDir |= HOST_U_IOC_WRITE;
  }
  *Host = (HostDir << HOST_U_IOC_DIRSHIFT) | (Size << HOST_U_IOC_SIZESHIFT) | Low;
  return true;
}

inline bool IoctlRequestToGuest(uint32_t Host, uint32_t* Guest) {
  const uint32_t Dir = (Host >> HOST_U_IOC_DIRSHIFT) & HOST_U_IOC_DIRMASK;
  const uint32_t Size = (Host >> HOST_U_IOC_SIZESHIFT) & HOST_U_IOC_SIZEMASK;
  const uint32_t Low = Host & ((1U << HOST_U_IOC_SIZESHIFT) - 1);
  if (Dir == 0 && Size == 0) {
    *Guest = Host;
    return true;
  }
  uint32_t GuestDir {};
  if (Dir & HOST_U_IOC_READ) {
    GuestDir |= GUEST_U_IOC_READ;
  }
  if (Dir & HOST_U_IOC_WRITE) {
    GuestDir |= GUEST_U_IOC_WRITE;
  }
  if (Dir & ~(HOST_U_IOC_READ | HOST_U_IOC_WRITE | HOST_U_IOC_NONE)) {
    return false;
  }
  *Guest = (GuestDir << GUEST_U_IOC_DIRSHIFT) | (Size << GUEST_U_IOC_SIZESHIFT) | Low;
  return true;
}

///// errno /////

// Rewrites a raw syscall result carrying a powerpc-only errno value.
inline uint64_t HostResultToGuest(uint64_t Result) {
  const int64_t Signed = static_cast<int64_t>(Result);
  if (Signed < 0 && Signed >= -4095) {
    for (const auto& Fix : HostToGuestErrno) {
      if (-Signed == Fix.Host) {
        return static_cast<uint64_t>(-static_cast<int64_t>(Fix.Guest));
      }
    }
  }
  return Result;
}

///// SOL_SOCKET option names /////

inline int SocketOptionToHost(int GuestOptName) {
  for (const auto& Opt : SocketOptions) {
    if (Opt.Guest == static_cast<uint64_t>(GuestOptName)) {
      return static_cast<int>(Opt.Host);
    }
  }
  return GuestOptName;
}

} // namespace FEX::HLE::Arm64::ABI
