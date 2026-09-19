// SPDX-License-Identifier: MIT
#pragma once

#include "Interface/IR/IR.h"

#include <FEXCore/IR/IR.h>
#include <FEXCore/fextl/vector.h>

namespace FEXCore::IR {
class IREmitter;

// Compare fusion (CompareBranchFusion.cpp): rewrites the NZCV consumers of a
// flag-setting compare into direct compares of the compare's operands.
//
// Not a pass of its own. DeadFlagCalculationElimination runs it block by
// block once its flag liveness has converged, because whether a fusion pays,
// and whether the compare can then be dropped, both depend on who else reads
// the compare's flags. Owned by the DFCE pass object so the scratch vectors
// stop allocating after the first few compiles.
class CompareFusion final {
public:
  // NZCVLiveOut: some NZCV bit is live at the end of Block (the union of its
  // successors' live-in flags, or all of them at an exit). RemoveDeadProducers
  // false keeps a compare whose flags become dead (DFCE's
  // DisableDFCEStoreElim knob).
  void Run(IREmitter* IREmit, IRListView& IR, Ref Block, bool NZCVLiveOut, bool RemoveDeadProducers);

private:
  struct Consumer {
    Ref Node;
    // The compare whose operands this consumer compares. Its operands are
    // read when the consumer is rewritten, not when it is found: an earlier
    // rewrite may have replaced one of them (a CSEL whose result the next
    // compare reads is itself rewritten into a new Select node).
    Ref Producer;
    // An AddNZCV/AddWithFlags producer: compare against NegatedConst, whose
    // node is created when the consumer is rewritten.
    bool AgainstNegatedConst;
    uint64_t NegatedConst;
    OpSize CompareSize;
    CondClass Cond;
    // The consumer's FromNZCV lowering needs XER (C or V), so fusing it pays
    // even if the compare has to stay.
    bool NZCVFormReadsXER;
    bool Apply;
  };

  fextl::vector<Consumer> Consumers;
  fextl::vector<Ref> DeadProducers;
};

} // namespace FEXCore::IR
