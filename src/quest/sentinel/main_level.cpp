#include "quest/sentinel/main_level.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "abi/game_level.h"
#include "quest/invite/frame_dispatch.h"
#include "quest/plugin/plugin_platform.h"
#include "quest/plugin/plugin_runtime.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/pinned_targets.h"
#include "runtime/log/symcache.h"
#include "runtime/patch/ui_texture_mips_rules.h"

namespace evr {
extern const SymCacheEntry kSymCacheData[];  // the merged PC and Quest symbol names, sorted by hash (symcache_data.cpp)
extern const int kSymCacheCount;
}  // namespace evr

namespace nevr_quest::main_level {
namespace {

// The game object: the netgame the frame hook passed before this Tick holds it at netgame+8 (join_game_quest.cpp GameOf reads it
// the same way). Null before the first NetGame::Update.
void* GameObject() noexcept {
  void* netGame = nevr_quest::invite::frame::NetGame();
  if (netGame == nullptr) return nullptr;
  void* game = nullptr;
  std::memcpy(&game, static_cast<const unsigned char*>(netGame) + sentinel::pinned::game_layout::kGameOffset, sizeof(game));
  return game;
}

// The level's name from the generated table, or null when the table does not know the symbol.
const char* NameOf(std::uint64_t symbol) noexcept {
  return nevr_ui_texture_mips::NameForHash(evr::kSymCacheData, static_cast<std::size_t>(evr::kSymCacheCount), symbol);
}

// The symbol as the log reads it: "none", the name the table knows, else the hex value (`hex` holds the hex value).
const char* Label(std::uint64_t symbol, char (&hex)[19]) noexcept {
  if (symbol == 0) return "none";
  if (const char* name = NameOf(symbol)) return name;
  return sentinel::HexString(hex, symbol);
}

// The last symbol logged. A change is logged once, whichever context reads it first.
std::atomic<std::uint64_t> g_lastLogged{0};

plugin::MainLevel ReadMainLevel() {
  const std::uint64_t symbol = EchoVR::ReadMainLevelSymbol(GameObject(), EchoVR::kQuestMainLevelSymbolOffset);
  const std::uint64_t before = g_lastLogged.exchange(symbol);
  if (before != symbol) {
    char fromHex[19];
    char toHex[19];
    char symbolHex[19];
    sentinel::LogFields(sentinel::LogLevel::kInfo, "main_level_changed",
                        {{"from", Label(before, fromHex)},
                         {"to", Label(symbol, toHex)},
                         {"symbol", sentinel::HexString(symbolHex, symbol)}});
  }
  plugin::MainLevel level;
  level.symbol = symbol;
  level.name = NameOf(symbol);
  return level;
}

// The frame work: one per NetGame::Update, before that Tick's posted work. It never waits (plugin::RefreshMainLevel try-locks).
bool RefreshWork(void* /*context*/) noexcept {
  plugin::RefreshMainLevel();
  return true;
}

}  // namespace

bool Install() noexcept {
  static const bool registered = [] {
    plugin::SetMainLevelSource(&ReadMainLevel);
    const bool ok = nevr_quest::invite::frame::RegisterFrameWork(&RefreshWork, nullptr);
    if (!ok) sentinel::LogFields(sentinel::LogLevel::kWarn, "main_level", {{"status", "off"}, {"reason", "frame_work_full"}});
    return ok;
  }();
  return registered;
}

}  // namespace nevr_quest::main_level
