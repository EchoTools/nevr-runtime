#include "runtime/patch/coop_ai_trace.h"

#include <windows.h>

#include <array>
#include <cstring>
#include <mutex>

#include "core/logging.h"
#include "nevr_common.h"
#include "runtime/hook/patching.h"
#include "runtime/patch/coop_ai_trace_rules.h"

namespace CoopAiTrace {
namespace {

// CR15AIBBNetbot fields (the update 0x1412c1a30 and the phase mapper 0x1412c2100).
constexpr std::uintptr_t kSetupPendingOffset = 0x5F4;
constexpr std::uintptr_t kSetupOkOffset = 0x5F8;
constexpr std::uintptr_t kPhaseOffset = 0x12A0;
// The game clock the bot timers read: *(*(*(BB+0x58)+0x80)+0x10), time at +0x30. When the +0x10 slot is
// null the game falls back to a global clock (0x1404f3950); the trace reports -1 then.
constexpr std::uintptr_t kContextOffset = 0x58;
constexpr std::uintptr_t kContextNodeOffset = 0x80;
constexpr std::uintptr_t kNodeClockOffset = 0x10;
constexpr std::uintptr_t kClockTimeOffset = 0x30;

constexpr std::uint64_t kHeartbeatMs = 30'000;
constexpr std::size_t kMaxBots = 32;

// CR15AIBBNetbot vslot 4: MOV RAX,RSP; MOV [RAX+0x10],RBX; MOV [RAX+0x18],RSI; MOV [RAX+0x20],RDI; PUSH RBP.
// Runs per bot on every AI tick, so the hook logs only on a change and on the heartbeat.
constexpr std::uint64_t kUpdateVA = 0x1412C1A30;
constexpr std::array<std::uint8_t, 16> kUpdatePrologue = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48,
                                                          0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55};
using UpdateFn = void (*)(void* bot);

// Goals lookup (bot, netGame, transform): MOV [RSP+0x18],RSI; PUSH RBP; PUSH RDI; PUSH R14; LEA RBP,[RSP-0x47].
constexpr std::uint64_t kGoalsVA = 0x1412C10F0;
constexpr std::array<std::uint8_t, 16> kGoalsPrologue = {0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41,
                                                         0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81};
using GoalsFn = std::uint64_t (*)(void* bot, void* netGame, void* transform);

// Waypoints lookup (bot, netGame): MOV [RSP+0x20],RBX; PUSH RDI; SUB RSP,0x20; MOV RBX,RDX; MOV RDI,RCX.
constexpr std::uint64_t kWaypointsVA = 0x1412C13D0;
constexpr std::array<std::uint8_t, 16> kWaypointsPrologue = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x57, 0x48, 0x83,
                                                             0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9};
using WaypointsFn = std::uint64_t (*)(void* bot, void* netGame);

// The actor both lookups search (netGame, out): PUSH RBX; SUB RSP,0x20; MOV RCX,[RCX+0x40]; MOV RBX,RDX;
// TEST RCX,RCX. Called, not hooked: it has per-frame callers (RaycastAimAssist).
constexpr std::uint64_t kLookupActorVA = 0x140181D80;
constexpr std::array<std::uint8_t, 16> kLookupActorPrologue = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
                                                               0x49, 0x40, 0x48, 0x8B, 0xDA, 0x48, 0x85, 0xC9};
using LookupActorFn = std::int64_t* (*)(void* netGame, std::int64_t* out);

UpdateFn g_originalUpdate = nullptr;
GoalsFn g_originalGoals = nullptr;
WaypointsFn g_originalWaypoints = nullptr;
LookupActorFn g_lookupActor = nullptr;

std::mutex g_mutex;
BotTable<kMaxBots> g_bots;
bool g_tableFullLogged = false;

template <typename T>
T ReadAt(const void* base, std::uintptr_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
  return value;
}

std::int64_t ReadClock(const void* bot) {
  const void* context = ReadAt<const void*>(bot, kContextOffset);
  if (context == nullptr) return -1;
  const void* node = ReadAt<const void*>(context, kContextNodeOffset);
  if (node == nullptr) return -1;
  const void* clock = ReadAt<const void*>(node, kNodeClockOffset);
  if (clock == nullptr) return -1;
  return ReadAt<std::int64_t>(clock, kClockTimeOffset);
}

// Caller holds g_mutex.
BotRecord* Record(const void* bot) {
  BotRecord* record = g_bots.Find(bot);
  if (record == nullptr && !g_tableFullLogged) {
    g_tableFullLogged = true;
    Log(EchoVR::LogLevel::Warning, "[NEVR.COOPAI] more than %zu bots; bot=%p and later ones are not traced",
        kMaxBots, bot);
  }
  return record;
}

void UpdateHook(void* bot) {
  g_originalUpdate(bot);
  if (bot == nullptr) return;
  const std::uint64_t nowMs = GetTickCount64();
  std::lock_guard<std::mutex> lock(g_mutex);
  BotRecord* record = Record(bot);
  if (record == nullptr) return;
  record->state.setupPending = ReadAt<std::int32_t>(bot, kSetupPendingOffset);
  record->state.setupOk = ReadAt<std::int32_t>(bot, kSetupOkOffset);
  record->state.phase = ReadAt<std::int32_t>(bot, kPhaseOffset);
  const bool changed = !record->everLogged || !SameState(record->logged, record->state);
  if (!changed && !HeartbeatDue(record->lastLogMs, nowMs, kHeartbeatMs)) return;
  const std::int64_t clock = ReadClock(bot);
  const BotState& s = record->state;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COOPAI] %s bot=%p setup_pending=%d setup_ok=%d goals=%d waypoints=%d lookup_actor=%lld phase=%d "
      "clock=%lld clock_delta=%lld",
      changed ? "changed" : "heartbeat", bot, s.setupPending, s.setupOk, s.goalsOk, s.waypointsOk,
      static_cast<long long>(s.lookupActor), s.phase, static_cast<long long>(clock),
      static_cast<long long>(record->lastLogClock < 0 || clock < 0 ? -1 : clock - record->lastLogClock));
  record->logged = record->state;
  record->everLogged = true;
  record->lastLogMs = nowMs;
  record->lastLogClock = clock;
}

std::int64_t LookupActor(void* netGame) {
  if (g_lookupActor == nullptr) return kActorNotRead;
  if (netGame == nullptr) return -1;
  std::int64_t actor = -1;
  g_lookupActor(netGame, &actor);
  return actor;
}

std::uint64_t GoalsHook(void* bot, void* netGame, void* transform) {
  const std::int64_t actor = LookupActor(netGame);
  const std::uint64_t result = g_originalGoals(bot, netGame, transform);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (BotRecord* record = Record(bot)) {
    record->state.goalsOk = static_cast<std::int32_t>(result) != 0 ? 1 : 0;
    record->state.lookupActor = actor;
  }
  return result;
}

std::uint64_t WaypointsHook(void* bot, void* netGame) {
  const std::uint64_t result = g_originalWaypoints(bot, netGame);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (BotRecord* record = Record(bot)) record->state.waypointsOk = static_cast<std::uint8_t>(result) != 0 ? 1 : 0;
  return result;
}

template <typename Fn, std::size_t Size>
void HookChecked(std::uintptr_t gameBase, std::uint64_t va, const std::array<std::uint8_t, Size>& prologue,
                 Fn& original, PVOID detour, const char* name) {
  void* target = nevr::ResolveVA_Checked(gameBase, va);
  if (!nevr::ValidatePrologue(target, prologue.data(), prologue.size())) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.COOPAI] %s hook skipped va=0x%llx reason=prologue_mismatch", name,
        static_cast<unsigned long long>(va));
    return;
  }
  original = reinterpret_cast<Fn>(target);
  if (PatchDetour(&original, detour, name))
    Log(EchoVR::LogLevel::Info, "[NEVR.COOPAI] %s hook installed va=0x%llx", name,
        static_cast<unsigned long long>(va));
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  void* lookup = nevr::ResolveVA_Checked(gameBase, kLookupActorVA);
  if (nevr::ValidatePrologue(lookup, kLookupActorPrologue.data(), kLookupActorPrologue.size())) {
    g_lookupActor = reinterpret_cast<LookupActorFn>(lookup);
  } else {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.COOPAI] lookup actor not read va=0x%llx reason=prologue_mismatch (logged as lookup_actor=%lld)",
        static_cast<unsigned long long>(kLookupActorVA), static_cast<long long>(kActorNotRead));
  }
  HookChecked(gameBase, kGoalsVA, kGoalsPrologue, g_originalGoals, reinterpret_cast<PVOID>(&GoalsHook),
              "CoopAiGoalsLookup");
  HookChecked(gameBase, kWaypointsVA, kWaypointsPrologue, g_originalWaypoints,
              reinterpret_cast<PVOID>(&WaypointsHook), "CoopAiWaypointsLookup");
  HookChecked(gameBase, kUpdateVA, kUpdatePrologue, g_originalUpdate, reinterpret_cast<PVOID>(&UpdateHook),
              "CoopAiBlackboardUpdate");
}

}  // namespace CoopAiTrace
