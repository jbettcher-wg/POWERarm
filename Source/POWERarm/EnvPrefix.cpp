// SPDX-License-Identifier: MIT
/*
 * POWERarm environment-variable namespace.
 *
 * Everything inherited from FEX reads its knobs as FEX_<NAME>. POWERarm must not
 * pick up (or leak into its children) the variables meant for a fastppcx86 on
 * the same machine, so every FEX_<NAME> lookup is answered from
 * POWERARM_<NAME> instead. The linker routes the libc calls here with
 * --wrap (see CMakeLists.txt); nothing else in the tree needs to know.
 *
 * Constraints: these run before the allocator is set up, from signal-adjacent
 * paths and from static initialisers, so no allocation and no logging.
 */
#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifndef POWERARM_ENV_PREFIX
#error "POWERARM_ENV_PREFIX must be defined by the build"
#endif

extern "C" {
char* __real_getenv(const char* Name);
char* __real_secure_getenv(const char* Name);
int __real_setenv(const char* Name, const char* Value, int Overwrite);
int __real_unsetenv(const char* Name);

__attribute__((visibility("hidden"))) char* __wrap_getenv(const char* Name);
__attribute__((visibility("hidden"))) char* __wrap_secure_getenv(const char* Name);
__attribute__((visibility("hidden"))) int __wrap_setenv(const char* Name, const char* Value, int Overwrite);
__attribute__((visibility("hidden"))) int __wrap_unsetenv(const char* Name);
}

namespace {
constexpr char LegacyPrefix[] = "FEX_";
constexpr size_t LegacyPrefixLen = sizeof(LegacyPrefix) - 1;
constexpr char NewPrefix[] = POWERARM_ENV_PREFIX;
constexpr size_t NewPrefixLen = sizeof(NewPrefix) - 1;

// Returns Name unchanged unless it starts with FEX_, in which case the
// translated name is written into Buffer. nullptr means the translated name
// would not fit; callers treat that as "not set".
const char* Translate(const char* Name, char* Buffer, size_t BufferSize) {
  if (!Name || strncmp(Name, LegacyPrefix, LegacyPrefixLen) != 0) {
    return Name;
  }
  const size_t SuffixLen = strlen(Name + LegacyPrefixLen);
  if (NewPrefixLen + SuffixLen + 1 > BufferSize) {
    return nullptr;
  }
  memcpy(Buffer, NewPrefix, NewPrefixLen);
  memcpy(Buffer + NewPrefixLen, Name + LegacyPrefixLen, SuffixLen + 1);
  return Buffer;
}
constexpr size_t MaxName = 256;
} // namespace

char* __wrap_getenv(const char* Name) {
  char Buffer[MaxName];
  const char* Real = Translate(Name, Buffer, sizeof(Buffer));
  return Real ? __real_getenv(Real) : nullptr;
}

char* __wrap_secure_getenv(const char* Name) {
  char Buffer[MaxName];
  const char* Real = Translate(Name, Buffer, sizeof(Buffer));
  return Real ? __real_secure_getenv(Real) : nullptr;
}

int __wrap_setenv(const char* Name, const char* Value, int Overwrite) {
  char Buffer[MaxName];
  const char* Real = Translate(Name, Buffer, sizeof(Buffer));
  if (!Real) {
    errno = EINVAL;
    return -1;
  }
  return __real_setenv(Real, Value, Overwrite);
}

int __wrap_unsetenv(const char* Name) {
  char Buffer[MaxName];
  const char* Real = Translate(Name, Buffer, sizeof(Buffer));
  if (!Real) {
    errno = EINVAL;
    return -1;
  }
  return __real_unsetenv(Real);
}
