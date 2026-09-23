// SPDX-License-Identifier: MIT
/*
$info$
tags: ir|opts
$end_info$
*/

#include "Interface/IR/Passes/RegisterAllocationPass.h"
#include "Interface/IR/IR.h"
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/RegisterAllocationData.h"
#include "Interface/IR/Passes.h"
#include <FEXCore/IR/IR.h>
#include <FEXCore/Utils/EnumUtils.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/Profiler.h>
#include <FEXCore/fextl/vector.h>
#include <bit>
#include <cstdint>

using namespace FEXCore;

namespace FEXCore::IR {
namespace {
  struct RegisterClassData {
    uint32_t Available;
    uint32_t Count;

    // If bit R of Available is 0, then RegToSSA[R] is the node currently
    // allocated to R. Else, RegToSSA[R] is UNDEFINED, no need to clear this
    // when freeing registers.
    Ref RegToSSA[32];
  };

  IR::RegClass GetRegClassFromNode(IR::IRListView* IR, IR::IROp_Header* IROp) {
    const auto Class = IR::GetRegClass(IROp->Op);
    if (Class != IR::RegClass::Complex) {
      return Class;
    }

    // Complex register class handling
    switch (IROp->Op) {
    case IR::OP_LOADCONTEXT: return IROp->C<IR::IROp_LoadContext>()->Class;
    case IR::OP_LOADREGISTER: return IROp->C<IR::IROp_LoadRegister>()->Class;
    case IR::OP_LOADCONTEXTINDEXED: return IROp->C<IR::IROp_LoadContextIndexed>()->Class;
    case IR::OP_LOADMEM:
    case IR::OP_LOADMEMTSO: return IROp->C<IR::IROp_LoadMem>()->Class;
    case IR::OP_FILLREGISTER: return IROp->C<IR::IROp_FillRegister>()->Class;
    default: return IR::RegClass::Invalid;
    }
  };
} // Anonymous namespace

class ConstrainedRAPass final : public RegisterAllocationPass {
public:
  explicit ConstrainedRAPass() {}
  void Run(IREmitter* IREmit) override;
  void AddRegisters(IR::RegClass Class, uint32_t RegisterCount) override;
  bool TryPostRAMerge(Ref LastNode, Ref CodeNode, IROp_Header* IROp);

private:
  RegisterClassData Classes[IR::NumClasses];

  IREmitter* IREmit {};
  IRListView* IR {};

  // Map of nodes to their preferred register, to coalesce load/store reg.
  fextl::vector<PhysicalRegister> PreferredReg;

  // Map of assigned registers. Does not grow beyond the initial set.
  fextl::vector<PhysicalRegister> SSAToReg;

  // Maps defs to their assigned spill slot + 1, or 0 if not spilled.
  fextl::vector<unsigned> SpillSlots;

  // Phase 2 of spill-slot reuse: ReleaseSlot is now wired into kill-bit
  // processing (see the main source-arg loop).  Slots dropped onto FreeSlots
  // are recycled by AcquireSlot before the high-water mark bumps.  Spill
  // lifetimes never cross block boundaries, so BlockSpills/FreeSlots/
  // BlockSlotHighWater all reset per block; the function-wide SpillSlots
  // count is the max of per-block peaks rather than the sum of all spills.
  struct SpillRange {
    uint32_t SSAID;     // The spilled SSA value's ID
    uint32_t DefIP;     // IP at which the spill was inserted (block-local)
    uint32_t LastUseIP; // IP at which the slot can be reclaimed (Phase 2)
    uint32_t Slot;      // Assigned slot index
  };
  fextl::vector<SpillRange> BlockSpills;
  fextl::vector<uint32_t> FreeSlots; // Reusable indices; always empty in Phase 1
  uint32_t BlockSlotHighWater {0};
  uint32_t GlobalSlotHighWater {0};

  // Phase 2: AcquireSlot pops from the free pool when possible; otherwise it
  // bumps the per-block high-water mark.  Free slots come from kill-bit
  // releases earlier in the same block.
  uint32_t AcquireSlot() {
    if (!FreeSlots.empty()) {
      uint32_t s = FreeSlots.back();
      FreeSlots.pop_back();
      return s;
    }
    return BlockSlotHighWater++;
  }

  // Phase 2: invoked when a spilled SSA value finally dies (kill-bit fires
  // on its last consumer).  The slot it occupied becomes available for any
  // subsequent spill within the same block.  Slots never cross block
  // boundaries (BlockSpills/FreeSlots/BlockSlotHighWater all reset per block),
  // so this is purely intra-block reuse on top of Phase 1's per-block reset.
  void ReleaseSlot(uint32_t slot) {
    FreeSlots.push_back(slot);
  }

  // Next-use distance relative to the block end of each source, last first.
  fextl::vector<uint32_t> SourcesNextUses;

  // Sources that have been seen
  fextl::vector<bool> Seen;

  // SourcesNextUses is read backwards, this tracks the index
  int64_t SourceIndex {};

  bool Rematerializable(IROp_Header* IROp) {
    // A PatchSite-tagged constant (FEX_SMCSEMANTICPATCH) must flow to its
    // consumers from its one materialisation window — a rematerialized copy
    // is untagged, so the SMC fault handler would patch a window nothing
    // executes.
    return IROp->Op == OP_CONSTANT && IROp->C<IROp_Constant>()->PatchSite == 0;
  }

  Ref InsertFill(Ref Node) {
    IROp_Header* IROp = IR->GetOp<IROp_Header>(Node);

    // Remat if we can
    if (Rematerializable(IROp)) {
      const auto Op = IROp->C<IR::IROp_Constant>();
      uint64_t Const = Op->Constant;
      return IREmit->_Constant(Const, Op->Pad, Op->MaxBytes);
    }

    // Otherwise fill from stack
    uint32_t SlotPlusOne = SpillSlots[IR->GetID(Node).Value];
    LOGMAN_THROW_A_FMT(SlotPlusOne >= 1, "Node must have been spilled");

    const auto RegClass = GetRegClassFromNode(IR, IROp);
    return IREmit->_FillRegister(IROp->Size, IROp->ElementSize, SlotPlusOne - 1, RegClass);
  };

  // IP of next-use of each source. IPs are measured from the end of the
  // region, so we don't need to size it up-front.
  fextl::vector<uint32_t> NextUses;

  // Allocation regions (warm G6). Normally one block, since a block can be
  // entered from anywhere and nothing may be assumed about the register file
  // at its head. The A64 frontend marks the exception: a block whose only
  // in-unit predecessor is the block that branches into it, and that carries
  // the frontend's GPR value cache across that edge, sets
  // IROp_CodeBlock::RegionPred to its predecessor's ID + 1. Such a block is
  // allocated as a continuation of that predecessor -- registers are not
  // freed and spill slots are not recycled at the edge -- so the values the
  // frontend kept live actually survive in their host registers. Everything
  // the region does is still one straight-line path: the successor runs only
  // after the predecessor, so a backward walk over the chain sees every live
  // range exactly as it does inside a single block.
  //
  // Region holds the chain currently being allocated, predecessor first.
  fextl::vector<Ref> Region;
  // RegionNext[ID] is the block that continues block ID's region, or nullptr.
  // RegionIsMember[ID] is set for every block that is some region's
  // continuation, i.e. must not start a region of its own.
  fextl::vector<Ref> RegionNext;
  fextl::vector<bool> RegionIsMember;

  bool AnySpilled {};

  // Phase 1: monotonically increasing per-instruction counter for the forward
  // pass.  Used to stamp DefIP on SpillRange entries.  Reset per block.
  // Phase 2 will use this (together with LastUseIP) to time slot release.
  uint32_t ForwardIP {0};

  bool IsValidArg(OrderedNodeWrapper Arg) {
    if (Arg.IsInvalid()) {
      return false;
    }

    auto Op = IR->GetOp<IROp_Header>(Arg)->Op;
    return Op != OP_INLINECONSTANT && Op != OP_INLINEENTRYPOINTOFFSET;
  };

  RegisterClassData* GetClass(PhysicalRegister Reg) {
    return &Classes[Reg.Class];
  };

  uint32_t GetRegBits(PhysicalRegister Reg) {
    return 1u << Reg.Reg;
  };

  bool IsInRegisterFile(Ref Node) {
    auto ID = IR->GetID(Node).Value;
    LOGMAN_THROW_A_FMT(ID < SSAToReg.size(), "Only old nodes looked up");

    PhysicalRegister Reg = SSAToReg[ID];
    RegisterClassData* Class = GetClass(Reg);

    return (Class->Available & GetRegBits(Reg)) == 0 && Class->RegToSSA[Reg.Reg] == Node;
  };

  void FreeReg(PhysicalRegister Reg) {
    RegisterClassData* Class = GetClass(Reg);
    uint32_t RegBits = GetRegBits(Reg);

    LOGMAN_THROW_A_FMT(!(Class->Available & RegBits), "Register double-free");

    Class->Available |= RegBits;
  };

  bool HasSource(IROp_Header* I, PhysicalRegister Reg) {
    int NumArgs = IR::GetRAArgs(I->Op);
    for (int s = 0; s < NumArgs; ++s) {
      if (I->Args[s].IsImmediate()) {
        // When spilling for a destination, we'll see register sources
        if (PhysicalRegister(I->Args[s]) == Reg) {
          return true;
        }
      } else {
        // When spilling for SRA correctness, we'll see SSA sources. This is
        // pretty obscure.
        auto V = I->Args[s];
        V.ClearKill();

        if (IsValidArg(V) && SSAToReg[V.ID().Value] == Reg) {
          return true;
        }
      }
    }

    return false;
  };

  Ref DecodeSRANode(const IROp_Header* IROp, Ref Node) {
    if (IROp->Op == OP_LOADREGISTER) {
      const auto* Op = IROp->C<IR::IROp_LoadRegister>();
      if (Op->Class == RegClass::FPR && Op->Reg >= 16) {
        return nullptr;
      }
      return Node;
    } else if (IROp->Op == OP_STOREREGISTER) {
      const auto Reg = PhysicalRegister(Node);
      if (Reg.AsRegClass() == RegClass::FPRFixed && Reg.Reg >= 16) {
        return nullptr;
      }
      auto V = IROp->C<IR::IROp_StoreRegister>()->Value;
      V.ClearKill();
      return IR->GetNode(V);
    }

    return nullptr;
  };

  PhysicalRegister DecodeSRAReg(const IROp_Header* IROp, Ref Node) {
    if (IROp->Op == OP_STOREREGISTER) {
      return PhysicalRegister(Node);
    } else {
      const IROp_LoadRegister* Op = IROp->C<IR::IROp_LoadRegister>();

      LOGMAN_THROW_A_FMT(Op->Class == RegClass::GPR || Op->Class == RegClass::FPR, "SRA classes");
      if (Op->Class == RegClass::FPR) {
        return PhysicalRegister {RegClass::FPRFixed, uint8_t(Op->Reg)};
      } else {
        return PhysicalRegister {RegClass::GPRFixed, uint8_t(Op->Reg)};
      }
    }
  };

  bool IsTrivial(Ref Node, const IROp_Header* Header) {
    switch (Header->Op) {
    case OP_ALLOCATEGPR: return true;
    case OP_ALLOCATEGPRAFTER: return true;
    case OP_ALLOCATEFPR: return true;
    case OP_RMWHANDLE: return PhysicalRegister(Node) == PhysicalRegister(Header->Args[0]);
    case OP_LOADREGISTER: return PhysicalRegister(Node) == DecodeSRAReg(Header, Node);
    case OP_STOREREGISTER: return PhysicalRegister(Header->Args[0]) == DecodeSRAReg(Header, Node);
    default: return false;
    }
  }

  // Helper macro to walk the set bits b in a 32-bit word x, using ffs to get
  // the next set bit and then clearing on each iteration.
#define foreach_bit(b, x) for (uint32_t __x = (x), b; ((b) = __builtin_ffs(__x) - 1, __x); __x &= ~(1u << (b)))

  // Next-use distances for the rest of the current allocation REGION, measured
  // from the region's end. A region is one block in all but the warm-G6 case,
  // where the frontend has chained a block onto its sole in-unit predecessor
  // (IROp_CodeBlock::RegionPred); the walk then has to cover every block from
  // the region end back to Until, because the forward pass keeps popping
  // SourcesNextUses straight through the internal block boundaries.
  void CalculateNextUses(IROp_Header* Until) {
    SourcesNextUses.clear();
    // resize() alone leaves stale next-use entries from earlier blocks (the
    // SSA count doesn't change between calls). Today that is benign only by
    // accident — dead defs are the sole readers of stale entries, and a
    // stale 0 means "spill me first", which is the right answer for a dead
    // def anyway. Zero explicitly so the "0 = no later use in this region"
    // invariant is real rather than accidental.
    NextUses.assign(IR->GetSSACount(), 0);

    // IP relative to the end of the region.
    uint32_t IP = 1;
    bool Done = false;

    for (size_t RI = Region.size(); RI-- > 0 && !Done;) {
      auto* BlockIROp = IR->GetOp<IROp_CodeBlock>(Region[RI]);

      // We grab these nodes this way so we can iterate easily
      auto CodeBegin = IR->at(BlockIROp->Begin);
      auto CodeLast = IR->at(BlockIROp->Last);

      while (1) {
        auto [CodeNode, IROp] = CodeLast();
        if (IROp == Until) {
          Done = true;
          break;
        }
        // End of iteration gunk

        const int NumArgs = IR::GetRAArgs(IROp->Op);
        for (int i = NumArgs - 1; i >= 0; --i) {
          auto V = IROp->Args[i];
          V.ClearKill();

          if (IsValidArg(V)) {
            const uint32_t Index = V.ID().Value;

            SourcesNextUses.push_back(NextUses[Index]);
            NextUses[Index] = IP;
          }
        }

        // IP is relative to region end and we iterate backwards, so increment.
        ++IP;

        // Rest is iteration gunk
        if (CodeLast == CodeBegin) {
          break;
        }
        --CodeLast;
      }
    }

    SourceIndex = SourcesNextUses.size();
  }

  void SpillReg(RegisterClassData* Class, IROp_Header* Exclude) {
    // We're about to use next-use information, so calculate it.
    if (!AnySpilled) {
      CalculateNextUses(Exclude);
    }

    // Find the best node to spill according to the "furthest-first" heuristic.
    // Since we defined IPs relative to the end of the block, the furthest
    // next-use has the /smallest/ unsigned IP.
    Ref Candidate = nullptr;
    uint32_t BestDistance = UINT32_MAX;
    uint8_t BestReg = ~0;
    uint32_t Allocated = ((Class->Count == 32) ? ~0u : ((1u << Class->Count) - 1)) & ~Class->Available;

    foreach_bit(i, Allocated) {
      Ref Node = Class->RegToSSA[i];
      auto Reg = SSAToReg[IR->GetID(Node).Value];

      LOGMAN_THROW_A_FMT(Node != nullptr, "Invariant3");
      LOGMAN_THROW_A_FMT(Reg.Reg == i, "Invariant4");

      // Skip any source used by the current instruction, it is unspillable.
      if (!HasSource(Exclude, Reg)) {
        uint32_t NextUse = NextUses[IR->GetID(Node).Value];

        // Prioritize remat over spilling. It is typically cheaper to remat a
        // constant multiple times than to spill a single value.
        if (!Rematerializable(IR->GetOp<IROp_Header>(Node))) {
          NextUse += 100000;
        }

        if (NextUse < BestDistance) {
          BestDistance = NextUse;
          BestReg = i;
          Candidate = Node;
        }
      }
    }

    LOGMAN_THROW_A_FMT(Candidate != nullptr, "must've found something..");

    PhysicalRegister Reg = SSAToReg[IR->GetID(Candidate).Value];
    LOGMAN_THROW_A_FMT(Reg.Reg == BestReg, "Invariant6");

    IROp_Header* Header = IR->GetOp<IROp_Header>(Candidate);
    uint32_t Value = IR->GetID(Candidate).Value;
    bool Spilled = !SpillSlots.empty() && SpillSlots[Value] != 0;

    // If we already spilled the Candidate, we don't need to spill again.
    // Similarly, if we can rematerialize the instruction, we don't spill it.
    // (PatchSite-tagged constants are not rematerializable — see
    // Rematerializable — so they spill like ordinary values.)
    if (!Spilled && !(Header->Op == OP_CONSTANT && Header->C<IROp_Constant>()->PatchSite == 0)) {
      LOGMAN_THROW_A_FMT(Reg.AsRegClass() == GetRegClassFromNode(IR, Header), "Consistent");

      // SpillSlots allocation is deferred.
      if (SpillSlots.empty()) {
        SpillSlots.resize(IR->GetSSACount(), 0);
      }

      // Phase 2 of spill-slot reuse: route allocation through AcquireSlot
      // (pulls from FreeSlots first, falls back to the per-block bump
      // allocator).  Record the spill range for diagnostic/validation use.
      uint32_t Slot = AcquireSlot();
      BlockSpills.push_back({Value, /*DefIP=*/ForwardIP, /*LastUseIP=*/ForwardIP + NextUses[Value], Slot});

      // We must map here in case we're spilling something we shuffled.
      auto SpillOp = IREmit->_SpillRegister(OrderedNodeWrapper::FromImmediate(Reg.Raw), Slot, Reg.AsRegClass());
      SpillOp.first->Header.Size = Header->Size;
      SpillOp.first->Header.ElementSize = Header->ElementSize;
      SpillSlots[Value] = Slot + 1;
    }

    // Now that we've spilled the value, take it out of the register file
    FreeReg(Reg);
    AnySpilled = true;
  };

  void RemapReg(Ref Node, PhysicalRegister Reg) {
    RegisterClassData* Class = GetClass(Reg);
    Class->RegToSSA[Reg.Reg] = Node;

    uint32_t Index = IR->GetID(Node).Value;
    if (Index < SSAToReg.size()) {
      SSAToReg[Index] = Reg;
    }
  };

  // Record a given assignment of register Reg to Node.
  void SetReg(Ref Node, PhysicalRegister Reg) {
    RegisterClassData* Class = GetClass(Reg);
    uint32_t RegBits = GetRegBits(Reg);

    LOGMAN_THROW_A_FMT((Class->Available & RegBits) == RegBits, "Precondition");

    Class->Available &= ~RegBits;

    RemapReg(Node, Reg);
    Node->Reg = Reg.Raw;
  };

  // Assign a register for a given Node, spilling if necessary.
  void AssignReg(IROp_Header* IROp, Ref CodeNode, IROp_Header* Pivot) {
    const uint32_t Node = IR->GetID(CodeNode).Value;

    // Prioritize preferred registers.
    if (Node < PreferredReg.size()) {
      if (PhysicalRegister Reg = PreferredReg[Node]; !Reg.IsInvalid()) {
        RegisterClassData* Class = GetClass(Reg);
        uint32_t RegBits = GetRegBits(Reg);

        if ((Class->Available & RegBits) == RegBits) {
          SetReg(CodeNode, Reg);
          return;
        }
      }
    }

    // Try to handle tied registers. This can fail, the JIT will insert moves.
    if (int TiedIdx = IR::TiedSource(IROp->Op); TiedIdx >= 0) {
      auto Reg = PhysicalRegister(IROp->Args[TiedIdx]);
      RegisterClassData* Class = GetClass(Reg);
      uint32_t RegBits = GetRegBits(Reg);

      if (Reg.AsRegClass() != RegClass::GPRFixed && Reg.AsRegClass() != RegClass::FPRFixed && (Class->Available & RegBits) == RegBits) {
        SetReg(CodeNode, Reg);
        return;
      }
    }

    // Try to coalesce reserved pairs. Just a heuristic to remove some moves.
    if (IROp->Op == OP_ALLOCATEGPR && IROp->C<IROp_AllocateGPR>()->ForPair) {
      uint32_t Available = Classes[FEXCore::ToUnderlying(RegClass::GPR)].Available;

      // Only choose base register R if R and R + 1 are both free
      Available &= (Available >> 1);

      // Only consider aligned registers in the pair region
      constexpr uint32_t EVEN_BITS = 0x55555555;
      Available &= (EVEN_BITS & ((1u << PairRegs) - 1));

      if (Available) {
        unsigned Reg = std::countr_zero(Available);
        SetReg(CodeNode, PhysicalRegister(RegClass::GPR, Reg));
        return;
      }
    } else if (IROp->Op == OP_ALLOCATEGPRAFTER) {
      uint32_t Available = Classes[FEXCore::ToUnderlying(RegClass::GPR)].Available;
      auto After = PhysicalRegister(IROp->Args[0]);
      if ((After.Reg & 1) == 0 && Available & (1ull << (After.Reg + 1))) {
        SetReg(CodeNode, PhysicalRegister(RegClass::GPR, After.Reg + 1));
        return;
      }
    }

    RegClass ClassType = GetRegClassFromNode(IR, IROp);
    RegisterClassData* Class = &Classes[FEXCore::ToUnderlying(ClassType)];

    // Spill to make room in the register file.
    if (!Class->Available) {
      IREmit->SetWriteCursorBefore(CodeNode);
      SpillReg(Class, Pivot);
    }

    // Assign a free register in the appropriate class.
    LOGMAN_THROW_A_FMT(Class->Available != 0, "Post-condition of spilling");
    unsigned Reg = std::countr_zero(Class->Available);
    SetReg(CodeNode, PhysicalRegister(ClassType, Reg));
  };
};

void ConstrainedRAPass::AddRegisters(IR::RegClass Class, uint32_t RegisterCount) {
  LOGMAN_THROW_A_FMT(RegisterCount <= 32, "Up to 32 regs supported");

  Classes[FEXCore::ToUnderlying(Class)].Count = RegisterCount;
}

inline bool KillMove(IROp_Header* LastOp, IROp_Header* IROp, Ref LastNode, Ref CodeNode) {
  // 32-bit moves in x86_64 are represented as a Bfe, detect them.
  if (LastOp->Op == OP_BFE && LastOp->C<IR::IROp_Bfe>()->lsb == 0 && LastOp->C<IR::IROp_Bfe>()->Width == 32) {
    auto Op = IROp->Op;

    if (Op == OP_AND) {
      // Rewrite "mov wA, wB; and xA, xA, xC" into "and wA, wB, wC", since
      // ((b & 0xffffffff) & c) == (b & c) & 0xffffffff.
      IROp->Size = OpSize::i32Bit;
      return true;
    } else if (IROp->Size == OpSize::i32Bit) {
      // Any op whose low 32 result bits depend only on the low 32 input bits
      // may absorb the 32-bit mov. OP_ADD qualifies for the same reason
      // OP_SUB does: carries propagate upward only.
      return Op == OP_OR || Op == OP_XOR || Op == OP_AND || Op == OP_ADD || Op == OP_SUB || Op == OP_LSHL || Op == OP_LSHR || Op == OP_ASHR;
    }
  }

  return LastOp->Op == OP_STOREREGISTER;
}

inline bool IsSignext(const IROp_Header* IROp, OrderedNodeWrapper Src, OpSize Size) {
  if (IROp->Op == OP_SBFE) {
    auto Sbfe = IROp->C<IR::IROp_Sbfe>();
    return Sbfe->Width == 1 && Sbfe->lsb == (IR::OpSizeAsBits(Size) - 1) && Sbfe->Src == Src;
  } else {
    return false;
  }
}

inline bool IsZero(const IROp_Header* IROp) {
  // A PatchSite-tagged constant's value can be rewritten at runtime by the SMC
  // semantic patcher; folding on its compile-time value would go stale.
  return IROp->Op == OP_CONSTANT && IROp->C<IROp_Constant>()->Constant == 0 && IROp->C<IROp_Constant>()->PatchSite == 0;
}

bool ConstrainedRAPass::TryPostRAMerge(Ref LastNode, Ref CodeNode, IROp_Header* IROp) {
  auto LastOp = IR->GetOp<IROp_Header>(LastNode);

  if (IROp->Op == OP_PUSH && LastOp->Op == OP_PUSH) {
    auto SP = PhysicalRegister(CodeNode);
    auto Push = IR->GetOp<IROp_Push>(CodeNode);
    auto LastPush = IR->GetOp<IROp_Push>(LastNode);

    if (LastOp->Size == IROp->Size && LastPush->ValueSize == Push->ValueSize && SP == PhysicalRegister(LastNode) &&
        SP == PhysicalRegister(IROp->Args[1]) && SP == PhysicalRegister(LastOp->Args[1]) && SP != PhysicalRegister(IROp->Args[0]) &&
        SP != PhysicalRegister(LastOp->Args[0]) && Push->ValueSize >= OpSize::i32Bit) {

      IREmit->SetWriteCursorBefore(LastNode);
      IREmit->_PushTwo(IROp->Size, Push->ValueSize, IROp->Args[0], LastOp->Args[0], IROp->Args[1]);
      IREmit->RemovePostRA(CodeNode);
      return true;
    }
  } else if (IROp->Op == OP_POP) {
    auto SP = PhysicalRegister(IROp->Args[0]);

    if (LastOp->Op == OP_POP && LastOp->Size == IROp->Size && IROp->Size >= OpSize::i32Bit && SP == PhysicalRegister(LastOp->Args[0])) {
      IREmit->SetWriteCursorBefore(LastNode);
      IREmit->_PopTwo(IROp->Size, IROp->Args[0], LastOp->Args[1], IROp->Args[1]);
      IREmit->RemovePostRA(CodeNode);
      return true;
    }
  } else if ((IROp->Op == OP_DIV || IROp->Op == OP_UDIV) && IROp->Size >= OpSize::i32Bit) {
    // If Upper came from a sign/zero extension, we only need a 64-bit division.
    auto Op = IROp->CW<IR::IROp_Div>();
    if (!Op->Upper.IsInvalid() && PhysicalRegister(Op->Upper) == PhysicalRegister(LastNode)) {
      if (IROp->Op == OP_DIV ? IsSignext(LastOp, Op->Lower, IROp->Size) : IsZero(LastOp)) {
        Op->Upper.SetInvalid();
        return PhysicalRegister(LastNode) == PhysicalRegister(Op->OutRemainder);
      }
    }
  }

  // Merge moves that are immediately consumed.
  //
  // x86 code inserts such moves to workaround x86's 2-address code. Because
  // arm64 is 3-address code, we can optimize these out.
  //
  // Note we rely on the short-circuiting here.
  if (PhysicalRegister(LastNode) == PhysicalRegister(CodeNode) && KillMove(LastOp, IROp, LastNode, CodeNode)) {
    LOGMAN_THROW_A_FMT(!PhysicalRegister(CodeNode).IsInvalid(), "invariant");

    int NumArgs = IR::GetRAArgs(IROp->Op);
    for (int s = 0; s < NumArgs; ++s) {
      if (IROp->Args[s].IsImmediate() && PhysicalRegister(IROp->Args[s]) == PhysicalRegister(LastNode)) {
        IROp->Args[s].SetImmediate(PhysicalRegister(LastOp->Args[0]).Raw);
      }
    }

    return true;
  }

  return false;
}

void ConstrainedRAPass::Run(IREmitter* IREmit_) {
  FEXCORE_PROFILE_SCOPED("PassManager::RA");

  IREmit = IREmit_;
  auto IR_ = IREmit->ViewIR();
  IR = &IR_;

  PreferredReg.resize(IR->GetSSACount(), PhysicalRegister::Invalid());
  SSAToReg.resize(IR->GetSSACount(), PhysicalRegister::Invalid());
  Seen.resize(IR->GetSSACount(), false);

  // Region prepass. RegionPred is 0 on every block unless the frontend asked
  // for a continuation, so an ordinary unit pays one walk of the block list
  // and allocates nothing.
  const uint32_t BlockCount = IR->GetHeader()->BlockCount;
  bool AnyRegion = false;
  for (auto [BlockNode, BlockHeader] : IR->GetBlocks()) {
    auto BlockIROp = BlockHeader->C<IR::IROp_CodeBlock>();
    if (BlockIROp->RegionPred == 0 || BlockIROp->ID >= BlockCount) {
      continue;
    }
    const uint32_t PredID = BlockIROp->RegionPred - 1;
    // The predecessor is always emitted before its continuation, and each
    // block continues at most one region; anything else is ignored, which
    // costs the optimisation and nothing else.
    if (PredID >= BlockIROp->ID) {
      continue;
    }
    if (!AnyRegion) {
      RegionNext.assign(BlockCount, nullptr);
      RegionIsMember.assign(BlockCount, false);
      AnyRegion = true;
    }
    if (RegionNext[PredID] || RegionIsMember[BlockIROp->ID]) {
      continue;
    }
    RegionNext[PredID] = BlockNode;
    RegionIsMember[BlockIROp->ID] = true;
  }

  for (auto [BlockNode, BlockHeader] : IR->GetBlocks()) {
    auto BlockIROp = BlockHeader->CW<IR::IROp_CodeBlock>();

    // Blocks that continue another block's region are allocated with it.
    if (AnyRegion && BlockIROp->ID < BlockCount && RegionIsMember[BlockIROp->ID]) {
      continue;
    }

    Region.clear();
    Region.push_back(BlockNode);
    if (AnyRegion) {
      for (Ref Next = RegionNext[BlockIROp->ID]; Next;) {
        Region.push_back(Next);
        Next = RegionNext[IR->GetOp<IROp_CodeBlock>(Next)->ID];
      }
    }

    // Spilling is region-local, so reset this per-region
    AnySpilled = false;

    // Phase 1 of spill-slot reuse: reset per-region bookkeeping.  Slots do not
    // cross region boundaries (spilling is strictly region-local), so the
    // high-water mark and pool both restart fresh each region.
    BlockSpills.clear();
    FreeSlots.clear();
    BlockSlotHighWater = 0;
    ForwardIP = 0;

    // At the start of each region, all registers are available.
    for (auto& Class : Classes) {
      Class.Available = (Class.Count == 32) ? ~0u : ((1u << Class.Count) - 1);
    }

    // Backwards pass: analyze kill bits and SRA affinities. Over the whole
    // region, last block first, so a value used in a later block of the chain
    // keeps its register until its real last use.
    for (size_t RI = Region.size(); RI-- > 0;) {
      auto* RegionBlockIROp = IR->GetOp<IROp_CodeBlock>(Region[RI]);
      // Reverse iteration is not yet working with the iterators
      // We grab these nodes this way so we can iterate easily
      auto CodeBegin = IR->at(RegionBlockIROp->Begin);
      auto CodeLast = IR->at(RegionBlockIROp->Last);

      while (1) {
        auto [CodeNode, IROp] = CodeLast();
        // End of iteration gunk

        // Record preferred registers for SRA. We also record the Node accessing
        // each register, used below. Since we initialized Class->Available,
        // RegToSSA is otherwise undefined so we can stash our temps there.
        if (auto Node = DecodeSRANode(IROp, CodeNode); Node != nullptr) {
          auto Reg = DecodeSRAReg(IROp, CodeNode);

          PreferredReg[IR->GetID(Node).Value] = Reg;
          GetClass(Reg)->RegToSSA[Reg.Reg] = CodeNode;
        }

        // Coalescing an SRA store is equivalent to hoisting the store,
        // implying write-after-write and read-after-write hazards. We can only
        // coalesce if there is no intervening load/store.
        //
        // Since we're walking backwards, RegToSSA tracks
        // the first load/store after CodeNode. That first instruction is the
        // store in question iff there is no intervening load/store.
        //
        // Reset PreferredReg if that is not the case, ensuring SRA correctness.
        if (auto Reg = PreferredReg[IR->GetID(CodeNode).Value]; !Reg.IsInvalid()) {
          auto Node = GetClass(Reg)->RegToSSA[Reg.Reg];
          IROp_Header* Header = IR->GetOp<IROp_Header>(Node);

          if (CodeNode != DecodeSRANode(Header, Node)) {
            PreferredReg[IR->GetID(CodeNode).Value] = PhysicalRegister::Invalid();
          }
        }

        const int NumArgs = IR::GetRAArgs(IROp->Op);
        for (int i = NumArgs - 1; i >= 0; --i) {
          const auto& Arg = IROp->Args[i];
          if (!Arg.IsInvalid()) {
            const uint32_t Index = Arg.ID().Value;
            if (!Seen[Index]) {
              Seen[Index] = true;
              IROp->Args[i].SetKill();
            }
          }
        }

        // Rest is iteration gunk
        if (CodeLast == CodeBegin) {
          break;
        }
        --CodeLast;
      }
    }

    // NextUses currently contains first use distances, the exact initialization
    // assumed by the forward pass. Do not reset it.

    // Forward pass: Assign registers, spilling & optimizing as we go.
    for (size_t RI = 0; RI < Region.size(); ++RI) {
      // Last nontrivial instruction, for merging as we go. Reset at every block
      // boundary: post-RA merging rewrites a pair of instructions the backend
      // emits back to back, and a block boundary is a branch target, so the
      // predecessor's last instruction is never adjacent to it.
      Ref LastNode = nullptr;

      for (auto [CodeNode, IROp] : IR->GetCode(Region[RI])) {
        bool AnySpilledBeforeThisInstruction = AnySpilled;

        // These do not read or write registers, and must be skipped for merging.
        // Since we'd be doing this check anyway for merging, do the check now so
        // we can skip the rest of the logic too.
        if (IROp->Op == OP_GUESTOPCODE || IROp->Op == OP_INLINECONSTANT) {
          continue;
        }

        // Phase 1 of spill-slot reuse: ForwardIP holds the current instruction's
        // IP for the duration of this iteration; SpillReg reads it as DefIP for
        // any spills emitted here.  Incremented at the bottom of the loop body.

        // Static registers must be consistent at SRA load/store. Evict to ensure.
        if (auto Node = DecodeSRANode(IROp, CodeNode); Node != nullptr) {
          auto Reg = DecodeSRAReg(IROp, CodeNode);
          RegisterClassData* Class = &Classes[Reg.Class];

          if (!(Class->Available & (1u << Reg.Reg))) {
            Ref Old = Class->RegToSSA[Reg.Reg];

            if (Old != Node) {
              // Before inserting instructions, we need to set the cursor and
              // reset LastNode so we don't merge across an inserted copy.
              // Otherwise, we would erroneously miss the copy when determining if
              // we can merge, and end up unsoundly merging a mov+xchg sequence.
              IREmit->SetWriteCursorBefore(CodeNode);
              LastNode = nullptr;

              Ref Copy;

              if (Reg.AsRegClass() == RegClass::FPRFixed) {
                IROp_Header* Header = IR->GetOp<IROp_Header>(Old);
                Copy = IREmit->_VMov(Header->Size, OrderedNodeWrapper::FromImmediate(Reg.Raw));
              } else {
                Copy = IREmit->_Copy(OrderedNodeWrapper::FromImmediate(Reg.Raw));
              }

              FreeReg(Reg);
              AssignReg(IR->GetOp<IROp_Header>(Copy), Copy, IROp);
              RemapReg(Old, PhysicalRegister(Copy));
            }
          }
        }

        // Fill all sources that are not already in the register file.
        //
        // This happens before freeing killed sources, since we need all sources in
        // the register file simultaneously.
        //
        // Also update next-use info, again only relevant if we've spilled.
        int NumArgs = IR::GetRAArgs(IROp->Op);

        if (AnySpilledBeforeThisInstruction) {
          for (int s = 0; s < NumArgs; ++s) {
            auto V = IROp->Args[s];
            V.ClearKill();

            if (!IsValidArg(V)) {
              continue;
            }

            Ref Old = IR->GetNode(V);

            SourceIndex--;
            LOGMAN_THROW_A_FMT(SourceIndex >= 0, "Consistent source count");
            NextUses[V.ID().Value] = SourcesNextUses[SourceIndex];

            if (!IsInRegisterFile(Old)) {
              IREmit->SetWriteCursorBefore(CodeNode);
              LastNode = nullptr;

              Ref Fill = InsertFill(Old);

              AssignReg(IR->GetOp<IROp_Header>(Fill), Fill, IROp);
              RemapReg(Old, PhysicalRegister(Fill));
            }
          }
        }

        for (int s = 0; s < NumArgs; ++s) {
          if (IROp->Args[s].IsInvalid()) {
            continue;
          }

          bool Kill = IROp->Args[s].HasKill();
          IROp->Args[s].ClearKill();
          Ref Node = IR->GetNode(IROp->Args[s]);
          auto ID = IR->GetID(Node).Value;
          auto Reg = SSAToReg[ID];

          if (!Reg.IsInvalid()) {
            if (Kill) {
              LOGMAN_THROW_A_FMT(IsInRegisterFile(Node), "sources in file");
              FreeReg(Reg);

              // Phase 2 of spill-slot reuse: if the dying SSA had been spilled,
              // its slot is now reclaimable.  The kill bit fires on the LAST
              // consumer of the original SSA; any intermediate InsertFill
              // re-reads are already past.  Zero out SpillSlots[ID] so a stray
              // double-release is impossible (the entry is also harmless to
              // leave, but zero documents intent).
              if (ID < SpillSlots.size() && SpillSlots[ID] != 0) {
                ReleaseSlot(SpillSlots[ID] - 1);
                SpillSlots[ID] = 0;
              }
            }

            IROp->Args[s].SetImmediate(Reg.Raw);
          }
        }

        // Assign destinations.
        if (GetHasDest(IROp->Op) && PhysicalRegister(CodeNode).IsInvalid()) {
          AssignReg(IROp, CodeNode, IROp);
        }

        if (IsTrivial(CodeNode, IROp)) {
          // Delete instructions that only exist for RA
          IREmit->RemovePostRA(CodeNode);
        } else if (LastNode && TryPostRAMerge(LastNode, CodeNode, IROp)) {
          // Merge adjacent instructions
          IREmit->RemovePostRA(LastNode);
          LastNode = nullptr;
        } else {
          LastNode = CodeNode;
        }

        // Phase 1 of spill-slot reuse: advance the per-region IP counter so that
        // the next instruction's spills (if any) carry a distinct DefIP.
        ++ForwardIP;
      }
    }

    if (AnySpilled) {
      LOGMAN_THROW_A_FMT(SourceIndex == 0, "Consistent source count in region");
    }

    // Phase 1 of spill-slot reuse: roll this region's peak slot usage into the
    // function-wide high-water mark, which becomes the final SpillSlots count.
    GlobalSlotHighWater = std::max(GlobalSlotHighWater, BlockSlotHighWater);
  }

  // Phase 1 of spill-slot reuse: publish the function-wide peak slot usage
  // into the IR header.  Replaces the per-spill `IR->GetHeader()->SpillSlots++`
  // bump that used to live in SpillReg.  Reset for the next Run() invocation.
  IR->GetHeader()->SpillSlots = GlobalSlotHighWater;
  GlobalSlotHighWater = 0;

  PreferredReg.clear();
  SSAToReg.clear();
  SpillSlots.clear();
  NextUses.clear();
  Seen.clear();
  BlockSpills.clear();
  FreeSlots.clear();
  Region.clear();
  RegionNext.clear();
  RegionIsMember.clear();

  IR->GetHeader()->PostRA = true;
}

fextl::unique_ptr<IR::RegisterAllocationPass> CreateRegisterAllocationPass() {
  return fextl::make_unique<ConstrainedRAPass>();
}
} // namespace FEXCore::IR
