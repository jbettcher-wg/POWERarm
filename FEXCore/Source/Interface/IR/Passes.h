// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/vector.h>

#include <cstdint>

namespace FEXCore {
struct HostFeatures;
} // namespace FEXCore

namespace FEXCore::Utils {
class IntrusivePooledAllocator;
}

namespace FEXCore::IR {
class IRListView;
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

// The same answers as bit masks (N=8 Z=4 C=2 V=1), for the NZCV exit-site
// census, which has to walk a producer's flags forward bit by bit the way DFCE
// does. Derived from the same table for the same reason as the predicates above.
unsigned IROpNZCVRead(IROp_Header* IROp);
unsigned IROpNZCVWrite(IROp_Header* IROp);

// The NZCV PRESERVE BRACKET, marked per SSA node: 1 on the LoadNZCV that saves
// the guest's flags, 2 on the StoreNZCV that puts them back, 0 everywhere else.
// Out is grown and zeroed as needed, so a caller that keeps it across compiles
// stops allocating.
//
// The A64 frontend wraps every guest op whose lowering clobbers the host state
// the guest's NZCV lives in -- the LDXR/STXR/CAS/LD<op> family, whose
// lwarx/stwcx./CAS lowering records into CR0 -- in exactly that pair. In this
// pass's model the pair reads all four bits and writes all four; at the guest
// level it does neither. Anything answering a question about GUEST NZCV
// liveness must cancel it, or every unit that starts with a lock claims a
// live-in it does not have.
void MarkNZCVPreserveBrackets(IRListView& CurrentIR, fextl::vector<uint8_t>& Out);

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
