// Built -fno-exceptions (callback_thunk.h refuses otherwise). The handler path (OnBooleanHandler, GateResult) is
// reachable from the thunk entry, so the frame sensor requires it to be personality-free, and it never logs: it only
// counts (hook_log.h: logging is unsafe on a game call path).
#include "quest/social/social_invite_gate.h"

#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "pinned_targets.h"

namespace quest_social {
namespace {

using sentinel::LogFields;
using sentinel::LogLevel;

std::atomic<std::uint64_t> g_forced{0};  // constant-initialised: no static initialiser

bool IsGatePath(const char* path) noexcept {
  if (path == nullptr) return false;
  const char* want = kFirstMatchPath;
  while (*want != '\0' && *path == *want) {
    ++path;
    ++want;
  }
  return *want == '\0' && *path == '\0';
}

}  // namespace

sentinel::GotTarget LibR15Boolean() {
  return {sentinel::pinned::kLibR15, kBooleanSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::kLibR15BuildId,
          kBooleanSlotVaddr};
}

std::uint32_t GateResult(const char* path, std::uint32_t original) noexcept { return IsGatePath(path) ? 1U : original; }

// Runs on whatever thread reads a boolean. The frame has no landing pad, so a game exception thrown by the original
// passes through it untouched, and nothing here can throw into the game.
std::uint32_t OnBooleanHandler(GateThunk::Fn original, const void* json, const char* path, std::uint32_t defaultValue,
                               std::uint32_t logMissing) noexcept {
  const std::uint32_t value = original(json, path, defaultValue, logMissing);
  const std::uint32_t result = GateResult(path, value);
  if (result != value) g_forced.fetch_add(1, std::memory_order_relaxed);
  return result;
}

NEVR_HOOK_RECORD(kInviteGateHook, GateThunk, &OnBooleanHandler);

GateCounters InviteGateCounters() noexcept { return GateCounters{g_forced}; }

void ResetInviteGateCountersForTest() noexcept { g_forced.store(0, std::memory_order_relaxed); }

bool RegisterInviteGateCounters() {
  bool ok = sentinel::RegisterReportCounter("social_boolean_calls", &GateThunk::CallCounter());
  ok = sentinel::RegisterReportCounter("social_invite_gate_forced", &g_forced, sentinel::ReportKind::kFaults) && ok;
  return ok;
}

sentinel::GotStatus InstallInviteGate() {
  static sentinel::GotHook hook;
  GateThunk::Arm(kInviteGateHook);
  const sentinel::GotStatus status = sentinel::InstallThunk<GateThunk>(hook, LibR15Boolean());
  if (status != sentinel::GotStatus::kOk) {
    GateThunk::Disarm();
    LogFields(LogLevel::kError, "social_invite_gate", {{"status", "hook_failed"}, {"got", sentinel::GotStatusName(status)}});
    return status;
  }
  LogFields(LogLevel::kInfo, "social_invite_gate", {{"status", "ok"}});
  return status;
}

}  // namespace quest_social
