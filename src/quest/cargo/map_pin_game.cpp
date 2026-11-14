#include "quest/cargo/map_pin_game.h"

#include <time.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "abi/game_level.h"
#include "core/map_session_pin.h"
#include "quest/invite/frame_dispatch.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::maps::pin {
namespace {

namespace layout = sentinel::pinned::game_layout;
namespace owner = sentinel::pinned::session_pin;
namespace core = nevr_map_session_pin;

// The clock: CLOCK_MONOTONIC in milliseconds.
constexpr std::uint64_t kMsPerSecond = 1000;
constexpr std::uint64_t kNsPerMs = 1000000;

// CR15NetGame::LoadMap: ldr x8, [x19, #0x40]; add x0, x8, #0x18 before OwnerSymbol(lobby, "level", -1). The offsets are the PC's
// (map_session_pin_game.cpp) on the same CR15NetGame and CNSLobby layout.
constexpr std::size_t kNetGameClientLobbyOffset = 0x40;
constexpr std::size_t kClientLobbyLobbyOffset = 0x18;

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

const char* Resolve() noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) return "module_not_loaded";
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return "no_build_id";
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return "build_id_mismatch";
  const auto* owner_bytes = reinterpret_cast<const unsigned char*>(image.base + owner::kOwnerSymbolVaddr);
  if (std::memcmp(owner_bytes, owner::kOwnerSymbolCode, sizeof(owner::kOwnerSymbolCode)) != 0) return "owner_symbol_mismatch";
  const auto* leave = reinterpret_cast<const unsigned char*>(image.base + owner::kScheduleReturnToLobbyVaddr);
  if (std::memcmp(leave, owner::kScheduleReturnToLobbyCode, sizeof(owner::kScheduleReturnToLobbyCode)) != 0)
    return "return_to_lobby_mismatch";
  g_owner_symbol = FunctionAt<OwnerSymbolFn>(image.base, owner::kOwnerSymbolVaddr);
  g_schedule_return = FunctionAt<ScheduleReturnToLobbyFn>(image.base, owner::kScheduleReturnToLobbyVaddr);
  return nullptr;
}

const void* PointerAt(const void* object, std::size_t offset) noexcept {
  const void* value = nullptr;
  std::memcpy(&value, static_cast<const unsigned char*>(object) + offset, sizeof(value));
  return value;
}

// SetErrorMessage(4 args)'s layout: the byte before the lines is 1, four 64-byte lines, each NUL-terminated.
void LayOutNotice(unsigned char* block, const char* title, const char* line) noexcept {
  std::memset(block, 0, layout::kErrorBlockBytes);
  block[0] = 1;
  const char* lines[2] = {title, line};
  for (std::size_t n = 0; n < 2; ++n) {
    const std::size_t len = std::min(std::strlen(lines[n]), layout::kErrorLineBytes - 1);
    std::memcpy(block + 1 + n * layout::kErrorLineBytes, lines[n], len);
  }
}

// One record the core emitted, in the sentinel log. Every kind is written; none is dropped.
void WriteEvent(const core::PinEvent& e) {
  const std::string level = std::to_string(e.level);
  const std::string pin = std::to_string(e.pin);
  switch (e.kind) {
    case core::PinEventKind::kUnpinned:
      sentinel::LogFields(sentinel::LogLevel::kInfo, "map_skew",
                          {{"reason", "map_unpinned"}, {"id", e.id.c_str()}, {"level", level.c_str()}});
      return;
    case core::PinEventKind::kSkew:
      sentinel::LogFields(sentinel::LogLevel::kWarn, "map_skew",
                          {{"reason", e.reason.c_str()}, {"id", e.id.c_str()}, {"level", level.c_str()}, {"pin", pin.c_str()},
                           {"fetch", e.fetch ? "1" : "0"}, {"notice", e.notice.c_str()}});
      return;
    case core::PinEventKind::kLeaveScheduled:
      sentinel::LogFields(sentinel::LogLevel::kInfo, "map_skew", {{"reason", "leave_scheduled"}, {"id", e.id.c_str()}});
      return;
    case core::PinEventKind::kLeaveFailed:
      sentinel::LogFields(sentinel::LogLevel::kWarn, "map_skew", {{"reason", "leave_failed"}, {"id", e.id.c_str()}});
      return;
    case core::PinEventKind::kNoticeNotWritten:
      sentinel::LogFields(sentinel::LogLevel::kWarn, "map_skew", {{"reason", "notice_not_written"}, {"id", e.id.c_str()}});
      return;
    case core::PinEventKind::kLocalMap:
      // The Quest has no local source (the view never lists one), so this is not reached; it is logged if it ever is.
      sentinel::LogFields(sentinel::LogLevel::kWarn, "map_skew", {{"reason", "local_map"}, {"id", e.id.c_str()}});
      return;
    case core::PinEventKind::kServerSet:
    case core::PinEventKind::kServerCleared:
      // The Quest is never a server: the core only writes the pin on a server, so this is not reached.
      sentinel::LogFields(sentinel::LogLevel::kWarn, "map_pin", {{"status", "unexpected"}, {"reason", "server_on_quest"}});
      return;
  }
}

class QuestGameAccess final : public core::GameAccess {
 public:
  bool Callable() override { return g_owner_symbol != nullptr && invite::frame::NetGame() != nullptr; }

  bool IsServer() override { return false; }

  std::uint64_t Lobby() override {
    const void* clientLobby = PointerAt(invite::frame::NetGame(), kNetGameClientLobbyOffset);
    return clientLobby == nullptr ? 0 : reinterpret_cast<std::uint64_t>(clientLobby) + kClientLobbyLobbyOffset;
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

  bool WriteNotice(const char* title, const char* line) override {
    LayOutNotice(static_cast<unsigned char*>(invite::frame::NetGame()) + layout::kErrorBlockOffset, title, line);
    return true;
  }

  bool ScheduleLeave() override {
    g_schedule_return(invite::frame::NetGame());
    return true;
  }

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

}  // namespace

bool InstallTick() noexcept {
  static const bool installed = [] {
    const char* refused = Resolve();
    if (refused == nullptr && !invite::frame::RegisterFrameWork(&TickWork, nullptr)) refused = "frame_work_full";
    if (refused != nullptr) {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "map_pin",
                          {{"status", "off"}, {"reason", refused}, {"consequence", "a mismatched server is not left"}});
      return false;
    }
    sentinel::LogFields(sentinel::LogLevel::kInfo, "map_pin", {{"status", "on"}});
    return true;
  }();
  return installed;
}

std::uint64_t MainLevelNow() noexcept { return g_game.Callable() ? g_game.MainLevel() : 0; }

}  // namespace nevr_quest::maps::pin
