// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>

namespace FEXCore::Context {
class Context;
}

namespace FEX::HLE::MemRedirectSymbols {

/**
 * @brief Walk a freshly mapped AArch64 ELF for memcpy/memmove/memset and hook each
 * one it finds, for the libc mem-routine redirect (docs/powerarm/THUNKS-DESIGN.md 4.2).
 *
 * Called from TrackMmap for the mapping that first creates a code-holding ELF's
 * MappedResource -- which covers both the main executable (POWERarm's own loader
 * routes through TrackFileBackedCopy) and anything the guest's ld.so maps.
 *
 * Cheap and silent when the MemRedirect config option is off: it returns before
 * reading anything.
 *
 * @param CTX Context whose hook table is populated.
 * @param Enabled The MemRedirect config option. Passed in rather than read here: a
 *        config Getter's constructor cannot run at load time in this library.
 * @param FD The mapped file. Read with pread: section headers are file offsets and
 *        are usually not mapped at all, so they cannot be read through MapAddr.
 * @param MapAddr Where this mapping starts.
 * @param MapOffset The file offset this mapping starts at, for the load bias.
 * @param Path The file's path, for the name filter and for logging.
 */
void ScanMappedELF(FEXCore::Context::Context* CTX, bool Enabled, int FD, uint64_t MapAddr, uint64_t MapOffset, std::string_view Path);

} // namespace FEX::HLE::MemRedirectSymbols
