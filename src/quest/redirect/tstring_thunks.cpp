// Built with -fno-exceptions (CMake and the host recipe set it; callback_thunk.h refuses
// otherwise). No try, no catch, no object with a destructor around a call that reaches game code:
// the handler calls the game's original and then one noexcept function pointer.
#include "quest/redirect/tstring_thunks.h"

#include <atomic>
#include <cstdint>

#include "hook_install.h"
#include "hook_report.h"
#include "pinned_targets.h"

namespace nevr_quest::redirect {
namespace {

using sentinel::pinned::LibR15TStringThunk;
using sentinel::pinned::MatchmakingTStringThunk;

std::atomic<ApplyFn> g_apply{nullptr};

template <typename Thunk>
const char* HandleTString(typename Thunk::Fn original, const sentinel::pinned::CJsonOpaque* self,
                          const char* key, const char* fallback, std::uint32_t flag) noexcept {
  const char* const result = original(self, key, fallback, flag);
  const ApplyFn apply = g_apply.load(std::memory_order_acquire);
  return apply == nullptr ? result : apply(key, result);
}

// One record per thunk entry: the build-time frame sensor roots its walk at each record's entry and
// handler.
NEVR_HOOK_RECORD(kLibR15Hook, LibR15TStringThunk, &HandleTString<LibR15TStringThunk>);
NEVR_HOOK_RECORD(kMatchmakingHook, MatchmakingTStringThunk, &HandleTString<MatchmakingTStringThunk>);

}  // namespace

HookTargets PinnedTargets() {
  return {sentinel::pinned::LibR15TString(), sentinel::pinned::MatchmakingTString()};
}

void SetApply(ApplyFn apply) noexcept { g_apply.store(apply, std::memory_order_release); }

sentinel::GotStatus InstallThunk(Slot slot, sentinel::GotHook& hook, const sentinel::GotTarget& target,
                                 sentinel::ImageLookup lookup) noexcept {
  return slot == Slot::kLibR15 ? sentinel::InstallThunk<LibR15TStringThunk>(hook, target, lookup)
                               : sentinel::InstallThunk<MatchmakingTStringThunk>(hook, target, lookup);
}

void* ThunkEntry(Slot slot) noexcept {
  return slot == Slot::kLibR15 ? LibR15TStringThunk::EntryAddress() : MatchmakingTStringThunk::EntryAddress();
}

void** ThunkOriginalOut(Slot slot) noexcept {
  return slot == Slot::kLibR15 ? LibR15TStringThunk::OriginalOut() : MatchmakingTStringThunk::OriginalOut();
}

void ArmThunk(Slot slot, bool armed) noexcept {
  if (slot == Slot::kLibR15) {
    if (armed) LibR15TStringThunk::Arm(kLibR15Hook); else LibR15TStringThunk::Disarm();
  } else {
    if (armed) MatchmakingTStringThunk::Arm(kMatchmakingHook); else MatchmakingTStringThunk::Disarm();
  }
}

void ResetThunk(Slot slot) noexcept {
  if (slot == Slot::kLibR15) {
    LibR15TStringThunk::Reset();
  } else {
    MatchmakingTStringThunk::Reset();
  }
}

bool RegisterThunkCounters() noexcept {
  bool ok = sentinel::RegisterReportCounter("tstring_libr15_calls", &LibR15TStringThunk::CallCounter());
  ok = sentinel::RegisterReportCounter("tstring_libr15_thunk_faults", &LibR15TStringThunk::FaultCounter(),
                                       sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("tstring_matchmaking_calls", &MatchmakingTStringThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("tstring_matchmaking_thunk_faults",
                                       &MatchmakingTStringThunk::FaultCounter(),
                                       sentinel::ReportKind::kFaults) && ok;
  return ok;
}

}  // namespace nevr_quest::redirect
