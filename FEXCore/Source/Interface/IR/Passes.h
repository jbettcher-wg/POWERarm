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

// Does this IR op write any bit of the packed NZCV state? Defined next to
// DeadFlagCalculationElimination's flag classification table and derived from
// it, so the two passes cannot disagree about what a flag writer is.
bool IROpWritesNZCV(IROp_Header* IROp);

fextl::unique_ptr<FEXCore::IR::Pass> CreateCompareBranchFusion();
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
