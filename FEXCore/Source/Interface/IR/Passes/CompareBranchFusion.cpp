// SPDX-License-Identifier: MIT
/*
$info$
tags: ir|opts
desc: Fuses a flag-setting compare into the CondJump/NZCVSelect that consume it
$end_info$
*/

#include "Interface/IR/Passes/CompareBranchFusion.h"
#include "Interface/IR/IR.h"
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/Passes.h"

#include <FEXCore/IR/IR.h>

#include <optional>

namespace FEXCore::IR {

// ---------------------------------------------------------------------------
// Compare fusion.
//
// A64 `cmp w0, #n ; b.cc` reaches the backend as
//     SubNZCV i32 w0, #n                    # writes packed NZCV
//     CondJump %Invalid, %Invalid, ..., cc, FromNZCV=1
// and CMN, ADDS and SUBS likewise as AddNZCV, AddWithFlags and SubWithFlags.
// The PPC64LE lowering of a FromNZCV consumer reads N/Z from CR0 and C/V from
// XER, which is where subfco./addco. leave them, and at i32 the producer has
// to pre-shift both operands so the flags sit at bit 63: `sldi ; li+sldi ;
// subfco.` for a W compare with an immediate. Consumers of C or V first
// project XER into CR1 (`mcrxrx`) and compose CR bits (MapNZCVCC).
//
// DEF_OP(CondJump) and DEF_OP(Select) also have a direct-compare arm for
// consumers that carry real Cmp1/Cmp2: EmitCompare into cr7 plus MapCC, i.e.
// cmpw/cmpd/cmplw/cmpld(+i) and a bc or isel, with no XER traffic. This code
// rewrites the first form into the second where the NZCV a consumer reads
// provably comes from a compare earlier in the same block, and drops the
// compare when nothing else reads its flags.
//
// ---------------------------------------------------------------------------
// WHEN, and why it runs inside DeadFlagCalculationElimination.
//
// DFCE calls Run() for every block once its flag liveness has converged, so
// Run() knows whether any NZCV bit is live out of the block. Per compare P it
// collects the NZCV readers between P and the next NZCV writer, then:
//
//  * P's flags are otherwise dead -- every reader is fusable, and P's flags
//    are overwritten in full later in the block, or no NZCV bit is live out
//    of it: fuse every reader and drop P (SubNZCV/AddNZCV are removed,
//    Sub/AddWithFlags demoted to Sub/Add, exactly DFCE's own rewrites).
//    That is the win: `cmpwi cr7 ; bc` in place of four instructions plus
//    the CR/XER work.
//  * Otherwise P stays and its flags stay exact in CR0/XER for whoever needs
//    them: a B.cond in another block, a CSEL the pass cannot take, ADC/SBC,
//    CCMP, MRS NZCV, an exit (the next unit, the dispatcher and signal frames
//    all see NZCV). Only readers whose NZCV form reads C or V are still
//    fused: one cmp replaces the XER projection and its CR composites. EQ,
//    NE, MI and PL read CR0 directly, so fusing them would only add a cmp.
//
// Live-out flags therefore never depend on the rewrite: P is dropped only on
// DFCE's converged answer that no NZCV bit is read before being rewritten,
// which is the same judgement DFCE applies to every other flag writer, with
// the same caveat (NZCV in a signal frame taken after the last reader is not
// exact, a trade FEX already makes).
//
// Signals need one more rule, because a fused consumer is still a guest
// instruction that reads NZCV: a handler that resumes the guest AT that
// instruction (through the dispatcher, from the signal frame) must find the
// flags P computed. So P also stays when an op that can raise a signal (a
// guest memory access, a syscall, ...) sits between it and a fused consumer
// (MayRaiseSignal). Asynchronous signals are drained only at block entries
// and on branch back edges, and a back-edge drain point reports the branch's
// target, never the branch (EmitEdgeSuspendInterruptCheck in the backend).
// unittests/A64Frontend/sigpreempt.c exercises both.
//
// Nothing here moves flag state: a fused consumer compares SSA values into
// cr7 and leaves CR0/XER alone (see DEF_OP(CondJump) and DEF_OP(Select)),
// so rewriting a reader in the middle of P's flag range is legal while other
// readers of P remain.
//
// ---------------------------------------------------------------------------
// CORRECTNESS: the two lowering arms must agree on what each CondClass means.
//
// FEX's packed NZCV is ARM-sense, and so is the CondClass enum (the A64
// frontend maps condition codes 0-13 onto it one to one; SubNZCV's IR.json
// description: "Carry flag uses arm64 definition"). What has to be checked is
// "Cond evaluated against the NZCV of P" == "Cond evaluated against a compare
// of P's operands".
//
// SUBTRACT producers (CMP, SUBS): after Sub*(Size, a, b) the backend emits
// subfco. on operands pre-shifted left by 64-Size for Size < 64, so every flag
// sits at the operand-size boundary:
//     Z = (a - b == 0 at Size)         <=> a == b
//     N = sign bit of (a - b) at Size
//     C = carry out of (~b + a + 1)    <=> a >=u b     (NOT borrow, ARM sense)
//     V = signed overflow of a - b at Size
// MapNZCVCC decodes the conditions from exactly those; EmitCompare + MapCC
// decode them from cmpw/cmpd (signed) or cmplw/cmpld (unsigned):
//
//   EQ   Z              <=> a == b        cmp + CC_EQ          identical
//   NEQ  !Z             <=> a != b        cmp + CC_NE          identical
//   UGE  C              <=> a >=u b       cmpl + CC_GE         identical
//   ULT  !C             <=> a <u  b       cmpl + CC_LT         identical
//   UGT  C && !Z        <=> a >u  b       cmpl + CC_GT         identical
//   ULE  !(C && !Z)     <=> a <=u b       cmpl + CC_LE         identical
//   SLT  N != V         <=> a <s  b       cmp  + CC_LT         identical
//   SGE  N == V         <=> a >=s b       cmp  + CC_GE         identical
//   SGT  (N==V) && !Z   <=> a >s  b       cmp  + CC_GT         identical
//   SLE  !((N==V)&&!Z)  <=> a <=s b       cmp  + CC_LE         identical
//
// (N!=V / N==V are the textbook signed-compare-from-subtract identities; they
// are what makes the pre-shift load-bearing at Size < 64.)
//
// ADD producers (CMN, ADDS) against a constant c (mod 2^n, n = Size in bits),
// compared as a against k = -c (mod 2^n). a + c and a - k are the same n-bit
// value, so Z and N agree; C and V do not in two places:
//   EQ/NEQ           a + c == 0 <=> a == k                     always
//   UGE/ULT/UGT/ULE  C = carry out of a + c <=> a + c >= 2^n <=> a >=u 2^n - c,
//                    which is a >=u k exactly when c != 0. For c == 0 the add
//                    never carries (C = 0) while a >=u 0 always holds: EXCLUDED.
//   SGE/SLT/SGT/SLE  N != V <=> the true sum a_s + c_s < 0 <=> a_s < -c_s,
//                    which is a <s k exactly when -c_s is representable, i.e.
//                    c != 2^(n-1). EXCLUDED for that one value.
// A register operand has no compile-time c, so a register CMN/ADDS is never
// fused. CMN's immediate is 12 bits (optionally LSL 12), so only the c == 0
// exclusion is reachable from the guest; both are checked.
//
// EXCLUDED conditions, because the two arms do NOT agree:
//   MI / PL   read N alone. MapCC maps them onto CC_LT / CC_GE, which after a
//             cmp is the *signed compare* N!=V -- the two differ exactly when
//             the operation overflows. Not an identity, EXCEPT against zero:
//             a - 0 and a + 0 never overflow, so V == 0 and MI == SLT-vs-0,
//             PL == SGE-vs-0 exactly. That case is fused as SLT/SGE.
//   VS / VC   read V. A cmp does not produce V at all; MapCC sends them to
//             CR0.SO, which after cmp is XER.SO, a sticky bit unrelated to it.
//   FLU/FGE/FLEU/FGT/FU/FNU  are FP codes, never produced against these.
//   TSTZ/TSTNZ read Cmp2 as a bit position; no NZCV meaning.
//   AL        unconditional; the frontend emits no reader for it.
//
// Size: the four producers are i32/i64 on this frontend, which is also the
// range EmitCompare's integer path handles exactly -- cmpw/cmplw compare bits
// 32:63 with the right extension, matching the 32-bit boundary the shifted
// subfco. used, and dirty upper bits (W operands read from X registers) are
// ignored by both. Anything else is left alone.
//
// Operands: EmitCompare requires Cmp1 in a register (it calls GetReg on it
// unconditionally) and handles a constant only in Cmp2, where it covers the
// full 64-bit range (16-bit immediate forms, else LoadConstant into TMP4). So
// a constant Src2 is fusable as-is and a constant Src1 (`cmp xzr, x1`, an
// inlined zero) is not. For an ADD producer the negated constant is created as
// a new InlineConstant, sign-extended from 32 bits at i32 so that `cmn w0, #1`
// becomes `cmpwi cr7, w0, -1`; cmpw/cmplw read only its low word.
// ---------------------------------------------------------------------------

namespace {
  bool IsFusableCond(CondClass Cond) {
    switch (Cond) {
    case CondClass::EQ:
    case CondClass::NEQ:
    case CondClass::UGE:
    case CondClass::ULT:
    case CondClass::UGT:
    case CondClass::ULE:
    case CondClass::SGE:
    case CondClass::SLT:
    case CondClass::SGT:
    case CondClass::SLE: return true;
    default: return false;
    }
  }

  bool IsUnsignedCond(CondClass Cond) {
    return Cond == CondClass::UGE || Cond == CondClass::ULT || Cond == CondClass::UGT || Cond == CondClass::ULE;
  }

  bool IsSignedCond(CondClass Cond) {
    return Cond == CondClass::SGE || Cond == CondClass::SLT || Cond == CondClass::SGT || Cond == CondClass::SLE;
  }

  // The FromNZCV lowering of these reads C or V: MapNZCVCC projects XER into
  // CR1 and composes CR bits. EQ/NEQ/MI/PL read CR0 alone.
  bool NZCVFormReadsXER(CondClass Cond) {
    return IsUnsignedCond(Cond) || IsSignedCond(Cond);
  }

  // EmitCompare's Cmp1 must be a real register value.
  bool IsRegisterOperand(IRListView& IR, OrderedNodeWrapper Arg) {
    if (Arg.IsInvalid() || Arg.IsImmediate()) {
      return false;
    }

    auto Op = IR.GetOp<IROp_Header>(Arg)->Op;
    return Op != OP_INLINECONSTANT && Op != OP_INLINEENTRYPOINTOFFSET;
  }

  bool IsUsableOperand(IRListView& IR, OrderedNodeWrapper Arg) {
    if (Arg.IsInvalid() || Arg.IsImmediate()) {
      return false;
    }

    // InlineEntrypointOffset is a constant the compare path does not decode
    // (only InlineConstant is), so it must be in a register there too.
    auto Op = IR.GetOp<IROp_Header>(Arg)->Op;
    return Op != OP_INLINEENTRYPOINTOFFSET;
  }

  bool GetConstant(IREmitter* IREmit, IRListView& IR, OrderedNodeWrapper Arg, uint64_t* Value) {
    if (Arg.IsInvalid() || Arg.IsImmediate()) {
      return false;
    }
    auto Hdr = IR.GetOp<IROp_Header>(Arg);
    if (Hdr->Op == OP_INLINECONSTANT) {
      *Value = Hdr->C<IROp_InlineConstant>()->Constant;
      return true;
    }
    // Declines PatchSite-tagged constants, whose value may change at run time.
    return IREmit->IsValueConstant(Arg, Value);
  }

  // Can this op raise a signal (a fault, a trap, a syscall)? A signal handler
  // may resume the guest at an instruction of its choosing, through the
  // dispatcher with NZCV from the signal frame; if that is a fused consumer
  // whose compare was dropped, the consumer would read flags that were never
  // written. So a compare stays when an op like this sits between it and a
  // fused consumer. Asynchronous signals need no such care: on this backend
  // they are delivered only at drain points, which sit at block entries and on
  // the backward legs of branches (see DEF_OP(CondJump) for why the latter
  // report the branch target, not the branch).
  bool MayRaiseSignal(IROps Op) {
    switch (Op) {
    case OP_GUESTOPCODE:
    case OP_INLINECONSTANT:
    case OP_INLINEENTRYPOINTOFFSET:
    case OP_STOREREGISTER:
    case OP_STORECONTEXT: return false;
    // Guest memory reads, which the IR does not mark as side effects.
    case OP_LOADMEM:
    case OP_LOADMEMTSO:
    case OP_LOADMEMREV:
    case OP_VLOADVECTORMASKED:
    case OP_VLOADVECTORGATHERMASKED:
    case OP_VLOADVECTORGATHERMASKEDQPS:
    case OP_VLOADVECTORELEMENT:
    case OP_VBROADCASTFROMMEM:
    case OP_VLOADTWOGPRS: return true;
    default: return HasSideEffects(Op);
    }
  }

  // DEF_OP(Select) materialises inline-constant True/False values with a bare
  // 16-bit `li`; anything wider would silently truncate.
  bool FitsSelectLi(IRListView& IR, OrderedNodeWrapper Arg) {
    if (Arg.IsInvalid()) {
      return false;
    }
    auto Hdr = IR.GetOp<IROp_Header>(Arg);
    if (Hdr->Op == OP_INLINEENTRYPOINTOFFSET) {
      return false;
    }
    if (Hdr->Op == OP_INLINECONSTANT) {
      int64_t V = static_cast<int64_t>(Hdr->C<IROp_InlineConstant>()->Constant);
      return V >= -32768 && V <= 32767;
    }
    return true;
  }

  // A compare the direct-compare arms can take, reduced to "compare Cmp1 with
  // Cmp2 (or with NegatedConst)".
  struct Producer {
    IROp_Header* Op {};
    Ref Node {};
    bool Fusable {};
    bool IsAdd {};
    // Src2 is a constant that is zero at the producer's size: V is known 0.
    bool AgainstZero {};
    // ADD producers: the addend c at the producer's size, and -c sign-extended
    // from that size.
    uint64_t AddConst {};
    uint64_t NegatedConst {};
    uint64_t SignBit {};
  };

  Producer AnalyzeProducer(IREmitter* IREmit, IRListView& IR, Ref Node, IROp_Header* Op) {
    Producer P {.Op = Op, .Node = Node};
    switch (Op->Op) {
    case OP_SUBWITHFLAGS:
    case OP_SUBNZCV: break;
    case OP_ADDWITHFLAGS:
    case OP_ADDNZCV: P.IsAdd = true; break;
    default: return P;
    }

    if (Op->Size != OpSize::i32Bit && Op->Size != OpSize::i64Bit) {
      return P;
    }
    if (!IsRegisterOperand(IR, Op->Args[0]) || !IsUsableOperand(IR, Op->Args[1])) {
      return P;
    }

    const bool Is64 = Op->Size == OpSize::i64Bit;
    const uint64_t Mask = Is64 ? ~0ULL : 0xFFFF'FFFFULL;
    P.SignBit = Is64 ? (1ULL << 63) : (1ULL << 31);

    uint64_t C {};
    const bool IsConst = GetConstant(IREmit, IR, Op->Args[1], &C);
    if (P.IsAdd) {
      if (!IsConst) {
        return P;
      }
      P.AddConst = C & Mask;
      const uint64_t Neg = (0 - P.AddConst) & Mask;
      P.NegatedConst = Is64 ? Neg : static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(Neg))));
    }
    P.AgainstZero = IsConst && (C & Mask) == 0;
    P.Fusable = true;
    return P;
  }

  // The condition a consumer needs after fusing against P, or nullopt if the
  // pair is not an identity (see the tables above).
  std::optional<CondClass> ResolveFusedCond(const Producer& P, CondClass Cond) {
    if (Cond == CondClass::MI || Cond == CondClass::PL) {
      if (P.AgainstZero) {
        return Cond == CondClass::MI ? CondClass::SLT : CondClass::SGE;
      }
      return std::nullopt;
    }

    if (!IsFusableCond(Cond)) {
      return std::nullopt;
    }

    if (P.IsAdd) {
      if (IsUnsignedCond(Cond) && P.AddConst == 0) {
        return std::nullopt;
      }
      if (IsSignedCond(Cond) && P.AddConst == P.SignBit) {
        return std::nullopt;
      }
    }
    return Cond;
  }
} // namespace

void CompareFusion::Run(IREmitter* IREmit, IRListView& IR, Ref Block, bool NZCVLiveOut, bool RemoveDeadProducers) {
  Consumers.clear();
  DeadProducers.clear();

  Producer Cur {};
  // Some reader of Cur's flags cannot be fused, or a fused one follows an op
  // that may raise a signal: Cur must stay.
  bool CurKept = false;
  bool SignalSinceCur = false;
  size_t CurFirst = 0;

  // Decide Cur's consumers [CurFirst, end) once its flag range is closed.
  // FlagsNeededLater: Cur's flags (some of them) are read after the range.
  auto Close = [&](bool FlagsNeededLater) {
    if (!Cur.Fusable) {
      return;
    }
    const bool Dead = !CurKept && !FlagsNeededLater;
    for (size_t i = CurFirst; i < Consumers.size(); ++i) {
      Consumers[i].Apply = Dead || Consumers[i].NZCVFormReadsXER;
    }
    if (Dead && RemoveDeadProducers && Consumers.size() > CurFirst) {
      DeadProducers.push_back(Cur.Node);
    }
  };

  for (auto [CodeNode, IROp] : IR.GetCode(Block)) {
    const bool Reads = IROpReadsNZCV(IROp);
    const bool Writes = IROpWritesNZCV(IROp);
    if (!Reads && !Writes && MayRaiseSignal(IROp->Op)) {
      SignalSinceCur = true;
    }

    // A read happens before the op's own write (CCMP, ADCS).
    if (Reads) {
      std::optional<CondClass> Fused;
      CondClass Orig {};
      if (Cur.Fusable) {
        if (IROp->Op == OP_CONDJUMP) {
          auto Op = IROp->C<IROp_CondJump>();
          // FromNZCV and the vector-compare mode are mutually exclusive, and
          // the rewrite would reinterpret FPR-class Cmp1/Cmp2 as GPRs.
          if (Op->FromNZCV && Op->VCmpElementSize == OpSize::iInvalid) {
            Orig = Op->Cond;
            Fused = ResolveFusedCond(Cur, Orig);
          }
        } else if (IROp->Op == OP_NZCVSELECT) {
          auto Op = IROp->C<IROp_NZCVSelect>();
          if (FitsSelectLi(IR, Op->TrueVal) && FitsSelectLi(IR, Op->FalseVal)) {
            Orig = Op->Cond;
            Fused = ResolveFusedCond(Cur, Orig);
          }
        }
      }

      if (Fused) {
        Consumers.push_back({
          .Node = CodeNode,
          .Producer = Cur.Node,
          .AgainstNegatedConst = Cur.IsAdd,
          .NegatedConst = Cur.NegatedConst,
          .CompareSize = Cur.Op->Size,
          .Cond = *Fused,
          .NZCVFormReadsXER = NZCVFormReadsXER(Orig),
          .Apply = false,
        });
        if (SignalSinceCur) {
          CurKept = true;
        }
      } else {
        CurKept = true;
      }
    }

    if (Writes) {
      // A partial writer leaves some of Cur's flags in place for later
      // readers, which are no longer attributed to Cur: keep it.
      Close(!IROpWritesAllNZCV(IROp));
      Cur = AnalyzeProducer(IREmit, IR, CodeNode, IROp);
      CurKept = false;
      SignalSinceCur = false;
      CurFirst = Consumers.size();
    }
  }
  Close(NZCVLiveOut);

  for (const auto& C : Consumers) {
    if (!C.Apply) {
      continue;
    }

    auto IROp = IR.GetOp<IROp_Header>(C.Node);
    // The producer's operands as they are NOW. Recording them when the
    // consumer was found was the Firefox miscompile: in
    //     cmp x9, x1 ; csel x9, x9, x1, hi ; cmp x9, x2 ; csel x9, x9, x2, lo
    // the second compare reads the first CSEL's result as its SSA value (the
    // frontend's cache of context-backed X registers hands it straight on),
    // and rewriting that CSEL replaces its node. ReplaceUsesWithAfter updates
    // the second compare but not a Ref copied out of it, so the second Select
    // compared a removed node: whatever register the allocator left there.
    auto ProducerOp = IR.GetOp<IROp_Header>(C.Producer);
    Ref Cmp1 = IR.GetNode(ProducerOp->Args[0]);
    Ref Cmp2 {};
    if (C.AgainstNegatedConst) {
      IREmit->SetWriteCursorBefore(C.Node);
      Cmp2 = IREmit->_InlineConstant(C.NegatedConst);
    } else {
      Cmp2 = IR.GetNode(ProducerOp->Args[1]);
    }

    if (IROp->Op == OP_CONDJUMP) {
      auto Op = IROp->CW<IROp_CondJump>();
      IREmit->ReplaceNodeArgument(C.Node, 0, Cmp1);
      IREmit->ReplaceNodeArgument(C.Node, 1, Cmp2);
      Op->CompareSize = C.CompareSize;
      Op->Cond = C.Cond;
      Op->FromNZCV = false;
    } else {
      // NZCVSelect -> Select. Legal mid-flag-range ONLY because ppc64le's
      // DEF_OP(Select) compares into cr7 on every arm and thus clobbers
      // neither CR0 nor XER (see the comment there, which points back here).
      auto Op = IROp->C<IROp_NZCVSelect>();
      IREmit->SetWriteCursorBefore(C.Node);
      Ref NewSelect = IREmit->_Select(IROp->Size, C.CompareSize, C.Cond, Cmp1, Cmp2, IR.GetNode(Op->TrueVal), IR.GetNode(Op->FalseVal));
      IREmit->ReplaceUsesWithAfter(C.Node, NewSelect, C.Node);
      IREmit->Remove(C.Node);
    }
  }

  // Every reader of these was fused and nothing reads their flags afterwards.
  // Same rewrites DFCE applies to a dead flag writer.
  for (Ref Node : DeadProducers) {
    auto IROp = IR.GetOp<IROp_Header>(Node);
    switch (IROp->Op) {
    case OP_SUBWITHFLAGS: IROp->Op = OP_SUB; break;
    case OP_ADDWITHFLAGS: IROp->Op = OP_ADD; break;
    case OP_SUBNZCV:
    case OP_ADDNZCV:
      if (Node->GetUses() == 0) {
        IREmit->Remove(Node);
      }
      break;
    default: break;
    }
  }
}

} // namespace FEXCore::IR
