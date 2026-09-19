// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/fextl/memory.h>

namespace FEXCore {
struct HostFeatures;
} // namespace FEXCore

namespace FEXCore::Utils {
class IntrusivePooledAllocator;
}

namespace FEXCore::IR {
class Pass;
class RegisterAllocationPass;
struct IROp_Header;

// Does this IR op write any / every bit of the packed NZCV state, or read any?
// Defined next to DeadFlagCalculationElimination's flag classification table
// and derived from it, so DFCE and compare fusion (which DFCE runs) cannot
// disagree about what a flag writer or reader is.
bool IROpWritesNZCV(IROp_Header* IROp);
bool IROpWritesAllNZCV(IROp_Header* IROp);
bool IROpReadsNZCV(IROp_Header* IROp);

fextl::unique_ptr<FEXCore::IR::Pass> CreateDeadFlagCalculationEliminination();
fextl::unique_ptr<FEXCore::IR::Pass> CreateScalarSplatChain();
fextl::unique_ptr<FEXCore::IR::RegisterAllocationPass> CreateRegisterAllocationPass();

namespace Validation {
  fextl::unique_ptr<FEXCore::IR::Pass> CreateIRValidation();
} // namespace Validation

namespace Debug {
  fextl::unique_ptr<FEXCore::IR::Pass> CreateIRDumper();
}
} // namespace FEXCore::IR
