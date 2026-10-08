// Built with -fno-exceptions (CMake and the host recipe set it; callback_thunk.h refuses
// otherwise). No try, no catch, no object with a destructor around a call that reaches game code:
// the handler calls the game's original and then one noexcept function in a sentinel-only frame.
#include "quest/redirect/tstring_thunks.h"

#include <cstdint>

#include "pinned_targets.h"

namespace nevr_quest::redirect {
namespace {

using sentinel::pinned::LibR15TStringThunk;
using sentinel::pinned::MatchmakingTStringThunk;

template <typename Thunk>
const char* HandleTString(typename Thunk::Fn original, const sentinel::pinned::CJsonOpaque* self,
                          const char* key, const char* fallback, std::uint32_t flag) noexcept {
  const char* const result = original(self, key, fallback, flag);
  return ApplyActive(key, result);
}

}  // namespace

HookTargets PinnedTargets() {
  return {sentinel::pinned::LibR15TString(), sentinel::pinned::MatchmakingTString()};
}

void* ThunkEntry(Slot slot) noexcept {
  return slot == Slot::kLibR15 ? LibR15TStringThunk::EntryAddress() : MatchmakingTStringThunk::EntryAddress();
}

void** ThunkOriginalOut(Slot slot) noexcept {
  return slot == Slot::kLibR15 ? LibR15TStringThunk::OriginalOut() : MatchmakingTStringThunk::OriginalOut();
}

void ArmThunk(Slot slot, bool armed) noexcept {
  if (slot == Slot::kLibR15) {
    LibR15TStringThunk::Arm(armed ? &HandleTString<LibR15TStringThunk> : nullptr);
  } else {
    MatchmakingTStringThunk::Arm(armed ? &HandleTString<MatchmakingTStringThunk> : nullptr);
  }
}

void ResetThunk(Slot slot) noexcept {
  if (slot == Slot::kLibR15) {
    LibR15TStringThunk::Reset();
  } else {
    MatchmakingTStringThunk::Reset();
  }
}

}  // namespace nevr_quest::redirect
