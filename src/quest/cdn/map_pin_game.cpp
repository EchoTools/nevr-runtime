#include "quest/cdn/map_pin_game.h"

#include <time.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "abi/game_level.h"
#include "core/map_session_pin.h"
#include "quest/cdn/map_pin_pure.h"
#include "quest/invite/frame_dispatch.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/pinned_targets.h"
#include "runtime/patch/lobby_failure_message_rules.h"

namespace nevr_quest::maps::pin {
namespace {

namespace layout = sentinel::pinned::game_layout;
namespace owner = sentinel::pinned::session_pin;
namespace core = nevr_map_session_pin;

// The clock: CLOCK_MONOTONIC in milliseconds.
constexpr std::uint64_t kMsPerSecond = 1000;
constexpr std::uint64_t kNsPerMs = 1000000;

// The consequence of a leave the game refuses past its retry window (the same words the PC's record carries).
constexpr const char* kLeaveFailedConsequence = "stays in a session whose map version differs";

// The notice is written with the lobby failure message's error-block writer, so the two layouts must be the same bytes.
static_assert(layout::kErrorBlockOffset == nevr_lobby_failure_message::kErrorBlockOffset, "the error block offset is one value");
static_assert(layout::kErrorLineBytes == nevr_lobby_failure_message::kErrorLineBytes, "the error line size is one value");
static_assert(layout::kErrorLines == nevr_lobby_failure_message::kErrorLines, "the error line count is one value");

using OwnerSymbolFn = std::uint64_t (*)(const void* lobby, const char* key, std::uint64_t fallback);
using ScheduleReturnToLobbyFn = void (*)(void* netGame);

OwnerSymbolFn g_owner_symbol = nullptr;
ScheduleReturnToLobbyFn g_schedule_return = nullptr;

template <typename Fn>
Fn FunctionAt(std::uintptr_t base, std::uint64_t vaddr) {
  const std::uintptr_t address = base + vaddr;
  Fn fn = nullptr;
  static_assert(sizeof(fn) == sizeof(address), "function pointer size");
  std::memcpy(&fn, &address, sizeof(fn));
  return fn;
}

// The pin is off for a module or build reason: the record gives the reason and the consequence only.
void Refuse(core::PinFields& refusal, const char* reason) {
  refusal = core::PinOffFields(reason, nullptr, 0, nullptr, nullptr, 0);
}

// Proves this libr15 is the pinned build. An empty `refusal` means every check passed.
void Resolve(core::PinFields& refusal) noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) return Refuse(refusal, "module_not_loaded");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refuse(refusal, "no_build_id");
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refuse(refusal, "build_id_mismatch");
  const auto* base = reinterpret_cast<const unsigned char*>(image.base);
  const auto* owner_expected = reinterpret_cast<const unsigned char*>(owner::kOwnerSymbolCode);
  if (!nevr_quest::maps::pin::CheckLibr15Entry(base, owner::kOwnerSymbolVaddr, owner_expected, sizeof(owner::kOwnerSymbolCode),
                                               "owner_symbol_mismatch", "CNSLobby::OwnerSymbol", refusal)) {
    return;
  }
  const auto* leave_expected = reinterpret_cast<const unsigned char*>(owner::kScheduleReturnToLobbyCode);
  if (!nevr_quest::maps::pin::CheckLibr15Entry(base, owner::kScheduleReturnToLobbyVaddr, leave_expected,
                                               sizeof(owner::kScheduleReturnToLobbyCode), "return_to_lobby_mismatch",
                                               "CR15NetGame::ScheduleReturnToLobby", refusal)) {
    return;
  }
  g_owner_symbol = FunctionAt<OwnerSymbolFn>(image.base, owner::kOwnerSymbolVaddr);
  g_schedule_return = FunctionAt<ScheduleReturnToLobbyFn>(image.base, owner::kScheduleReturnToLobbyVaddr);
}

const void* PointerAt(const void* object, std::size_t offset) noexcept {
  const void* value = nullptr;
  std::memcpy(&value, static_cast<const unsigned char*>(object) + offset, sizeof(value));
  return value;
}

// One record the core emitted, in the sentinel log. The shape is the PC's (docs/standards/logging.md [NEVR.MAPS]): the same
// keys for the same reason. Every kind is written; none is dropped.
void WriteEvent(const core::PinEvent& e) {
  const std::string level = core::HexText(e.level);
  const std::string pin = core::HexText(e.pin);
  switch (e.kind) {
    case core::PinEventKind::kUnpinned:
      sentinel::LogFields(sentinel::LogLevel::kInfo, "map_skew",
                          {{"reason", "map_unpinned"}, {"id", e.id.c_str()}, {"level", level.c_str()}});
      return;
    case core::PinEventKind::kSkew:
      sentinel::LogFields(sentinel::LogLevel::kWarn, "map_skew",
                          {{"reason", e.reason.c_str()}, {"id", e.id.c_str()}, {"level", level.c_str()}, {"pin", pin.c_str()},
                           {"fetch", e.fetch ? "true" : "false"}, {"notice", e.notice.c_str()}});
      return;
    case core::PinEventKind::kLeaveScheduled:
      sentinel::LogFields(sentinel::LogLevel::kInfo, "map_skew", {{"reason", "leave_scheduled"}, {"id", e.id.c_str()}});
      return;
    case core::PinEventKind::kLeaveFailed:
      sentinel::LogFields(sentinel::LogLevel::kWarn, "map_skew",
                          {{"reason", "leave_failed"},
                           {"id", e.id.c_str()},
                           {"retry_ms", core::kLeaveRetryMs},
                           {"consequence", kLeaveFailedConsequence}});
      return;
    case core::PinEventKind::kNoticeNotWritten:
      sentinel::LogFields(sentinel::LogLevel::kWarn, "map_skew", {{"reason", "notice_not_written"}, {"id", e.id.c_str()}});
      return;
    case core::PinEventKind::kLocalMap:
      // The Quest has no local source (the view never lists one), so this is not reached; it is logged if it ever is.
      sentinel::LogFields(sentinel::LogLevel::kWarn, "map_skew",
                          {{"reason", "local_map"}, {"id", e.id.c_str()}, {"level", level.c_str()}, {"pin", pin.c_str()},
                           {"local", e.local_path.c_str()}});
      return;
    case core::PinEventKind::kServerSet:
    case core::PinEventKind::kServerCleared:
      // The Quest is never a server: the core only writes the pin on a server, so this is not reached.
      sentinel::LogFields(sentinel::LogLevel::kWarn, "map_pin", {{"status", "unexpected"}, {"reason", "server_on_quest"}});
      return;
    case core::PinEventKind::kCargoServerSet:
      // The cargo pin's server record: the PC's "cargo pin set pin=<hex>" (docs/standards/logging.md [NEVR.MAPS]).
      sentinel::LogFields(sentinel::LogLevel::kInfo, "cargo_pin", {{"status", "set"}, {"pin", pin.c_str()}});
      return;
    case core::PinEventKind::kSessionDecision:
      // One decision per key change: the map and cargo reasons and which input left (the PC's "session pin decision" line).
      sentinel::LogFields(sentinel::LogLevel::kInfo, "session_pin_decision",
                          {{"map", e.reason.c_str()},
                           {"cargo", e.cargo_reason.c_str()},
                           {"leave", e.leave.c_str()},
                           {"level", level.c_str()}});
      return;
    case core::PinEventKind::kHoldArmed:
    case core::PinEventKind::kHoldReleased:
    case core::PinEventKind::kHoldDropped:
      // The core only holds a join when the access can skip the level load (CanHoldLoad), which this access does not say (the
      // KNOWN-WRONG #2762 above), so these are not reached; if one is, it is written with the PC's keys.
      sentinel::LogFields(sentinel::LogLevel::kInfo, "join_hold",
                          {{"event", e.kind == core::PinEventKind::kHoldArmed      ? "armed"
                                     : e.kind == core::PinEventKind::kHoldReleased ? "released"
                                                                                   : "dropped"},
                           {"id", e.id.c_str()},
                           {"level", level.c_str()},
                           {"reason", e.reason.c_str()}});
      return;
    case core::PinEventKind::kCargoSkew: {
      // The manifest's pin is "unknown" until this run has parsed one, as on the PC (PLAN-v7 5.5 "The decision").
      const std::string manifest = e.manifest_known ? core::HexText(e.manifest) : std::string("unknown");
      const std::string local = core::HexText(e.local);
      sentinel::LogFields(sentinel::LogLevel::kWarn, "cargo_skew",
                          {{"reason", e.reason.c_str()},
                           {"level", level.c_str()},
                           {"pin", pin.c_str()},
                           {"local", local.c_str()},
                           {"manifest", manifest.c_str()},
                           {"notice", e.notice.c_str()}});
      return;
    }
  }
}

// KNOWN-WRONG #2762: this access does not override
// CanHoldLoad/ReplayLoad, so the core's client join hold never arms on the Quest and a join to a map the device lacks leaves and
// joins again, as before. The level load to skip is CR15NetGame::LoadMap, libr15.so 0x1360bbc (260 bytes, first bytes
// ff c3 01 d1 e8 23 00 fd; searched 1041 of 1041 binaries for its "[NETGAME] Loading map" string, hits only echovr.exe and libr15.so);
// its one caller is the thunk at 0xfd6740, an indirect call, so the hook is a patch row (jump slot or glob_dat, ADR 0021) that needs a
// device check this tree cannot make. The core makes no PC-only assumption: that row plus the two overrides finish the Quest.
class QuestGameAccess final : public core::GameAccess {
 public:
  bool Callable() override { return g_owner_symbol != nullptr && invite::frame::NetGame() != nullptr; }

  bool IsServer() override { return false; }

  std::uint64_t Lobby() override {
    const void* clientLobby = PointerAt(invite::frame::NetGame(), layout::kNetGameClientLobbyOffset);
    return clientLobby == nullptr ? 0 : reinterpret_cast<std::uint64_t>(clientLobby) + layout::kClientLobbyLobbyOffset;
  }

  std::int32_t NetState() override {
    std::int32_t state = 0;
    std::memcpy(&state, static_cast<const unsigned char*>(invite::frame::NetGame()) + layout::kStateOffset, sizeof(state));
    return state;
  }

  std::uint64_t OwnerSymbol(std::uint64_t lobby, const char* key) override {
    return g_owner_symbol(reinterpret_cast<const void*>(lobby), key, 0);
  }

  void SetOwnerSymbol(std::uint64_t, const char*, std::uint64_t) override {
    // Not reached: IsServer is false here, so the core never writes the owner JSON on the Quest.
  }

  std::uint64_t MainLevel() override {
    return EchoVR::ReadMainLevelSymbol(PointerAt(invite::frame::NetGame(), layout::kGameOffset),
                                       EchoVR::kQuestMainLevelSymbolOffset);
  }

  // The notice goes into the net game's error block, through the writer the PC and the Quest's lobby failure page share.
  bool WriteNotice(const char* title, const char* line) override {
    return nevr_lobby_failure_message::WriteErrorBlock(static_cast<std::uint8_t*>(invite::frame::NetGame()), title, line,
                                                       std::strlen(line) + 1);
  }

  // The game's call queue must exist (#730): the game's function faults without it. A refusal is retried by the core, then
  // reported as leave_failed, as on the PC.
  bool ScheduleLeave() override { return ScheduleWhenQueueReady(invite::frame::NetGame(), g_schedule_return); }

  std::uint64_t NowMs() override {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * kMsPerSecond + static_cast<std::uint64_t>(now.tv_nsec) / kNsPerMs;
  }

  void Event(const core::PinEvent& event) override { WriteEvent(event); }
};

QuestGameAccess g_game;

bool TickWork(void* /*context*/) noexcept {
  core::TickWith(g_game);
  return true;
}

// The `map_pin` record for the fields of a pin that is off (or on): the same keys the PC's line carries.
void LogPinFields(sentinel::LogLevel level, const core::PinFields& fields) {
  std::vector<sentinel::LogField> list;
  list.reserve(fields.size());
  for (const core::PinField& field : fields) list.emplace_back(field.first.c_str(), field.second.c_str());
  sentinel::LogFieldList(level, "map_pin", list.data(), list.size());
}

}  // namespace

bool InstallTick() noexcept {
  static const bool installed = [] {
    core::PinFields refusal;
    Resolve(refusal);
    if (refusal.empty() && !invite::frame::RegisterFrameWork(&TickWork, nullptr)) Refuse(refusal, "frame_work_full");
    if (!refusal.empty()) {
      LogPinFields(sentinel::LogLevel::kWarn, refusal);
      return false;
    }
    sentinel::LogFields(sentinel::LogLevel::kInfo, "map_pin", {{"status", "on"}});
    return true;
  }();
  return installed;
}

std::uint64_t MainLevelNow() noexcept { return g_game.Callable() ? g_game.MainLevel() : 0; }

}  // namespace nevr_quest::maps::pin
