// SPDX-License-Identifier: MIT
#include "Interface/Context/Context.h"
#include "Interface/Core/A64Frontend/Decoder.h"
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/LookupCache.h"
#ifndef ARCHITECTURE_ppc64le
#include "Interface/Core/Dispatcher/Dispatcher.h"
#endif

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/HLE/SyscallHandler.h>

#include <FEXCore/Core/Thunks.h>
#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/Utils/TypeDefines.h>
#include "FEXCore/Debug/InternalThreadState.h"

#include <algorithm>

namespace FEXCore::Context {
fextl::unique_ptr<FEXCore::Context::Context> FEXCore::Context::Context::CreateNewContext(const FEXCore::HostFeatures& Features) {
  return fextl::make_unique<FEXCore::Context::ContextImpl>(Features);
}

void FEXCore::Context::ContextImpl::CompileRIP(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP) {
  CompileBlock(Thread->CurrentFrame, GuestRIP);
}

void FEXCore::Context::ContextImpl::CompileRIPCount(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP, uint64_t MaxInst) {
  CompileBlock(Thread->CurrentFrame, GuestRIP, MaxInst);
}

void FEXCore::Context::ContextImpl::SetSignalDelegator(FEXCore::SignalDelegator* _SignalDelegation) {
  SignalDelegation = _SignalDelegation;
}

void FEXCore::Context::ContextImpl::SetSyscallHandler(FEXCore::HLE::SyscallHandler* Handler) {
  SyscallHandler = Handler;
  SourcecodeResolver = Handler->GetSourcecodeResolver();
}

void FEXCore::Context::ContextImpl::SetThunkHandler(FEXCore::ThunkHandler* Handler) {
  ThunkHandler = Handler;
}

bool FEXCore::Context::ContextImpl::IsAddressInCodeBuffer(FEXCore::Core::InternalThreadState* Thread, uintptr_t Address) const {
  // A thread that has been through ReleaseDeadThreadResources has no backend.
  // It also has no host PC that could be in a code buffer, so "no" is both the
  // safe answer and the right one. NonMovableUniquePtr has no operator bool --
  // .get() is the null test.
  if (!Thread->CPUBackend.get()) [[unlikely]] {
    return false;
  }
  return Thread->CPUBackend->IsAddressInCodeBuffer(Address);
}

// SMC store backpatching (FEX_SMCSTOREBACKPATCH, ppc64le). All of the policy
// and the locking argument live in CodeBufferManager::TryAllocateAuxMemory
// (Interface/Core/CPUBackend.cpp); ContextImpl *is* the CodeBufferManager, so
// this is a plain forward.
FEXCore::Context::JITAuxAllocation FEXCore::Context::ContextImpl::AllocateJITAuxMemory(
  FEXCore::Core::InternalThreadState* Thread, size_t Bytes, size_t Alignment, uint64_t NearHostPC, uint64_t MaxDelta) {
  (void)Thread;
  return TryAllocateAuxMemory(Bytes, Alignment, NearHostPC, MaxDelta);
}

uint64_t FEXCore::Context::ContextImpl::GetJITCodeBufferGeneration() const {
  return CodeBufferGeneration.load(std::memory_order_acquire);
}

bool FEXCore::Context::ContextImpl::GuestRangeOverlapsCompiledCode(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) {
  // Released thread state: it holds no compiled code any more, so nothing of
  // its can overlap. See ReleaseDeadThreadResources.
  if (!Thread->LookupCache.get()) [[unlikely]] {
    return false;
  }
  return Thread->LookupCache->RangeOverlapsCompiledCode(Start, Length);
}

// Cheap compile tier (FEX_SMCCHEAPTIER); see the comment on SMCPageCounters.
namespace {
// An invalidation this large is a mapping being torn down or replaced, not
// arena churn. Counting every page of it would flood the table and drag
// unrelated code into the cheap tier.
constexpr uint64_t SMCMaxCountedInvalidationSize = 1 * 1024 * 1024;
} // namespace

void FEXCore::Context::ContextImpl::RecordCodeRangeInvalidation(uint64_t Start, uint64_t Length) {
  if (!Config.SMCCheapTier() || Length > SMCMaxCountedInvalidationSize) {
    return;
  }

  const uint64_t Base = Start & FEXCore::Utils::FEX_GUEST_PAGE_MASK;
  const uint64_t Top = FEXCore::AlignUp(Start + std::max<uint64_t>(Length, 1), FEXCore::Utils::FEX_GUEST_PAGE_SIZE);

  for (uint64_t Page = Base; Page < Top; Page += FEXCore::Utils::FEX_GUEST_PAGE_SIZE) {
    auto& Slot = SMCPageCounters[(Page >> FEXCore::Utils::FEX_GUEST_PAGE_SHIFT) & (SMCPageCounterSlots - 1)];
    if (Slot.Page.load(std::memory_order_relaxed) != Page) {
      // Steal the slot from whichever page held it.
      Slot.Page.store(Page, std::memory_order_relaxed);
      Slot.Count.store(1, std::memory_order_relaxed);
    } else {
      // Saturate rather than wrap: a page that crossed the threshold must not
      // fall back out of the cheap tier after 2^32 more invalidations.
      const uint32_t Count = Slot.Count.load(std::memory_order_relaxed);
      if (Count != ~0u) {
        Slot.Count.store(Count + 1, std::memory_order_relaxed);
      }
    }
  }
}

bool FEXCore::Context::ContextImpl::ShouldUseCheapTier(uint64_t GuestRIP) {
  if (!Config.SMCCheapTier()) {
    return false;
  }

  const uint64_t Page = GuestRIP & FEXCore::Utils::FEX_GUEST_PAGE_MASK;
  const auto& Slot = SMCPageCounters[(Page >> FEXCore::Utils::FEX_GUEST_PAGE_SHIFT) & (SMCPageCounterSlots - 1)];
  if (Slot.Page.load(std::memory_order_relaxed) != Page) {
    return false;
  }
  return Slot.Count.load(std::memory_order_relaxed) >= Config.SMCCheapTierThreshold();
}
} // namespace FEXCore::Context
