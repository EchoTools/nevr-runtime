#include "quest/cargo/map_pin_game.h"

#include <time.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "abi/game_level.h"
#include "quest/cargo/map_pin.h"
#include "quest/invite/frame_dispatch.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::maps::pin {
namespace {

namespace layout = sentinel::pinned::game_layout;

// NRadEngine::CNSLobby::OwnerSymbol(char const*, CSymbol64) const: add x0, x0, #0x310; mov w3, wzr; b CJson::Symbol.
// The branch is pc-relative inside libr15, so its word is fixed in the file.
constexpr std::uint64_t kOwnerSymbolVaddr = 0x192db34;
constexpr std::uint32_t kOwnerSymbolCode[3] = {0x910c4000, 0x2a1f03e3, 0x17d89651};
using OwnerSymbolFn = std::uint64_t (*)(const void* lobby, const char* key, std::uint64_t fallback);

// NRadEngine::NRadGame::CR15NetGame::ScheduleReturnToLobby(): str x19, [sp, #-0x20]!; sub x9, sp, #0xa0;
// stp x29, x30, [sp, #0x10]; add x29, sp, #0x10. Logs "[NETGAME] Scheduling return to lobby" and queues the return.
constexpr std::uint64_t kScheduleReturnToLobbyVaddr = 0x124aa6c;
constexpr std::uint32_t kScheduleReturnToLobbyCode[4] = {0xf81e0ff3, 0xd10283e9, 0xa9017bfd, 0x910043fd};
using ScheduleReturnToLobbyFn = void (*)(void* netGame);

// CR15NetGame::LoadMap: ldr x8, [x19, #0x40]; add x0, x8, #0x18 before OwnerSymbol(lobby, "level", -1).
constexpr std::size_t kNetGameClientLobbyOffset = 0x40;
constexpr std::size_t kClientLobbyLobbyOffset = 0x18;

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
  const auto* owner = reinterpret_cast<const unsigned char*>(image.base + kOwnerSymbolVaddr);
  if (std::memcmp(owner, kOwnerSymbolCode, sizeof(kOwnerSymbolCode)) != 0) return "owner_symbol_mismatch";
  const auto* leave = reinterpret_cast<const unsigned char*>(image.base + kScheduleReturnToLobbyVaddr);
  if (std::memcmp(leave, kScheduleReturnToLobbyCode, sizeof(kScheduleReturnToLobbyCode)) != 0) return "return_to_lobby_mismatch";
  g_owner_symbol = FunctionAt<OwnerSymbolFn>(image.base, kOwnerSymbolVaddr);
  g_schedule_return = FunctionAt<ScheduleReturnToLobbyFn>(image.base, kScheduleReturnToLobbyVaddr);
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

class QuestGameAccess final : public GameAccess {
 public:
  bool Callable() override { return g_owner_symbol != nullptr && invite::frame::NetGame() != nullptr; }

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

  std::uint64_t MainLevel() override {
    return EchoVR::ReadMainLevelSymbol(PointerAt(invite::frame::NetGame(), layout::kGameOffset),
                                       EchoVR::kQuestMainLevelSymbolOffset);
  }

  void WriteNotice(const char* title, const char* line) override {
    LayOutNotice(static_cast<unsigned char*>(invite::frame::NetGame()) + layout::kErrorBlockOffset, title, line);
  }

  bool ScheduleLeave() override {
    g_schedule_return(invite::frame::NetGame());
    return true;
  }

  std::uint64_t NowMs() override {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000u + static_cast<std::uint64_t>(now.tv_nsec) / 1000000u;
  }

  void LogSkew(const char* reason, const std::string& id, const std::string& notice) override {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "map_skew",
                        {{"reason", reason}, {"id", id.c_str()}, {"notice", notice.c_str()}});
  }
};

QuestGameAccess g_game;

bool TickWork(void* /*context*/) noexcept {
  TickWith(g_game);
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
