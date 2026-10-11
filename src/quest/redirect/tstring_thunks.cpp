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
using sentinel::pinned::LibR15CreateConnectionThunk;
using sentinel::pinned::MatchmakingTStringThunk;

std::atomic<ApplyFn> g_apply{nullptr};
std::atomic<ApplyUrlFn> g_applyUrl{nullptr};

template <typename Thunk>
const char* HandleTString(typename Thunk::Fn original, const sentinel::pinned::CJsonOpaque* self,
                          const char* key, const char* fallback, std::uint32_t flag) noexcept {
  const char* const result = original(self, key, fallback, flag);
  const ApplyFn apply = g_apply.load(std::memory_order_acquire);
  return apply == nullptr ? result : apply(key, result);
}

// CSysHttp::CreateConnection(handle, url): the URL goes through the decision before the original connects.
int HandleCreateConnection(LibR15CreateConnectionThunk::Fn original, unsigned long* handle,
                           const char* url) noexcept {
  const ApplyUrlFn apply = g_applyUrl.load(std::memory_order_acquire);
  return original(handle, apply == nullptr ? url : apply(url));
}

// One record per thunk entry: the build-time frame sensor roots its walk at each record's entry and
// handler.
NEVR_HOOK_RECORD(kLibR15Hook, LibR15TStringThunk, &HandleTString<LibR15TStringThunk>);
NEVR_HOOK_RECORD(kMatchmakingHook, MatchmakingTStringThunk, &HandleTString<MatchmakingTStringThunk>);
NEVR_HOOK_RECORD(kCreateConnectionHook, LibR15CreateConnectionThunk, &HandleCreateConnection);

}  // namespace

HookTargets PinnedTargets() {
  return {sentinel::pinned::LibR15TString(), sentinel::pinned::MatchmakingTString(),
          sentinel::pinned::LibR15CreateConnection()};
}

void SetApply(ApplyFn apply) noexcept { g_apply.store(apply, std::memory_order_release); }
void SetApplyUrl(ApplyUrlFn apply) noexcept { g_applyUrl.store(apply, std::memory_order_release); }

sentinel::GotStatus InstallThunk(Slot slot, sentinel::GotHook& hook, const sentinel::GotTarget& target,
                                 sentinel::ImageLookup lookup) noexcept {
  switch (slot) {
    case Slot::kLibR15: return sentinel::InstallThunk<LibR15TStringThunk>(hook, target, lookup);
    case Slot::kMatchmaking: return sentinel::InstallThunk<MatchmakingTStringThunk>(hook, target, lookup);
    case Slot::kCreateConnection: return sentinel::InstallThunk<LibR15CreateConnectionThunk>(hook, target, lookup);
  }
  return sentinel::GotStatus::kNotInstalled;
}

void* ThunkEntry(Slot slot) noexcept {
  switch (slot) {
    case Slot::kLibR15: return LibR15TStringThunk::EntryAddress();
    case Slot::kMatchmaking: return MatchmakingTStringThunk::EntryAddress();
    case Slot::kCreateConnection: return LibR15CreateConnectionThunk::EntryAddress();
  }
  return nullptr;
}

void** ThunkOriginalOut(Slot slot) noexcept {
  switch (slot) {
    case Slot::kLibR15: return LibR15TStringThunk::OriginalOut();
    case Slot::kMatchmaking: return MatchmakingTStringThunk::OriginalOut();
    case Slot::kCreateConnection: return LibR15CreateConnectionThunk::OriginalOut();
  }
  return nullptr;
}

void ArmThunk(Slot slot, bool armed) noexcept {
  switch (slot) {
    case Slot::kLibR15:
      if (armed) LibR15TStringThunk::Arm(kLibR15Hook); else LibR15TStringThunk::Disarm();
      break;
    case Slot::kMatchmaking:
      if (armed) MatchmakingTStringThunk::Arm(kMatchmakingHook); else MatchmakingTStringThunk::Disarm();
      break;
    case Slot::kCreateConnection:
      if (armed) LibR15CreateConnectionThunk::Arm(kCreateConnectionHook); else LibR15CreateConnectionThunk::Disarm();
      break;
  }
}

void ResetThunk(Slot slot) noexcept {
  switch (slot) {
    case Slot::kLibR15: LibR15TStringThunk::Reset(); break;
    case Slot::kMatchmaking: MatchmakingTStringThunk::Reset(); break;
    case Slot::kCreateConnection: LibR15CreateConnectionThunk::Reset(); break;
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
  ok = sentinel::RegisterReportCounter("http_create_connection_calls", &LibR15CreateConnectionThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("http_create_connection_thunk_faults",
                                       &LibR15CreateConnectionThunk::FaultCounter(),
                                       sentinel::ReportKind::kFaults) && ok;
  return ok;
}

}  // namespace nevr_quest::redirect
