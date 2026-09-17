// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>

namespace FEXCore::Context {
class Context;
}
namespace FEXCore::Core {
struct InternalThreadState;
}
namespace FEX::HLE {
class SyscallHandler;
}

namespace FEX::AOT {
// POWERARM_AOTTRANSLATE: compile the statically discovered entries of the
// already-mapped main ELF (ElfFD, loaded at LoadBias) into the code cache
// instead of running it. Mode "entries" compiles function entries only,
// "calls" adds call return points, anything else also far branch targets. Returns the process exit status.
int TranslateMainElf(FEXCore::Context::Context* CTX, FEX::HLE::SyscallHandler* Handler, FEXCore::Core::InternalThreadState* Thread,
                     int ElfFD, uint64_t LoadBias, std::string_view Mode);
} // namespace FEX::AOT
