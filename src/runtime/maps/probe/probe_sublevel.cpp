// PB2, P-SUBLEVEL: source A is the game object's two sorted level tables, read on every game-thread frame (the
// enabled table +0x1F50, and +0x1F90, the loaded levels that are not enabled); source B is the log-only Enable/Disable
// detours. Each successful B call is paired with the add or del it causes in the enabled table.

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "core/logging.h"
#include "runtime/hook/addresses.h"
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/maps/probe/probe_game.h"

namespace nevr_maps_probe {

namespace {

namespace addr = nevr_patch_addresses;

// More levels than this is not a table this probe understands (config_alias_hook.cpp kMaxGameSpaces).
constexpr std::uint64_t kMaxGameSpaces = 1024;
constexpr std::size_t kEntryBytes = 16;  // {level symbol, level resource}

// Game thread only.
SublevelMatcher g_matcher;
bool g_haveBase = false;
std::vector<std::uint64_t> g_enabled;
std::vector<std::uint64_t> g_loaded;
std::uint64_t g_lastMain = 0;

bool ReadQword(const unsigned char* at, std::uint64_t* value) {
  if (!nevr_crash_recovery::IsReadableMemory(at, sizeof(*value))) return false;
  std::memcpy(value, at, sizeof(*value));
  return true;
}

bool ReadTable(const unsigned char* game, std::uintptr_t data_offset, std::uintptr_t count_offset,
               std::vector<std::uint64_t>* levels) {
  std::uint64_t data = 0;
  std::uint64_t count = 0;
  if (!ReadQword(game + data_offset, &data) || !ReadQword(game + count_offset, &count)) return false;
  if (count == 0) return true;
  const auto* entries = reinterpret_cast<const unsigned char*>(data);
  if (count > kMaxGameSpaces || entries == nullptr ||
      !nevr_crash_recovery::IsReadableMemory(entries, static_cast<std::size_t>(count) * kEntryBytes)) {
    return false;
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    std::uint64_t level = 0;
    std::memcpy(&level, entries + i * kEntryBytes, sizeof(level));
    levels->push_back(level);
  }
  return true;
}

// Both sorted.
bool ReadSpaces(std::vector<std::uint64_t>* enabled, std::vector<std::uint64_t>* loaded) {
  enabled->clear();
  loaded->clear();
  const void* net_game = NetGame();
  std::uint64_t game_address = 0;
  if (net_game == nullptr ||
      !ReadQword(static_cast<const unsigned char*>(net_game) + addr::NETGAME_GAME_OFFSET, &game_address) ||
      game_address == 0) {
    return false;
  }
  const auto* game = reinterpret_cast<const unsigned char*>(game_address);
  std::vector<std::uint64_t> disabled;
  if (!ReadTable(game, addr::GAME_ENABLED_SPACE_TABLE_OFFSET, addr::GAME_ENABLED_SPACE_TABLE_COUNT_OFFSET, enabled) ||
      !ReadTable(game, addr::GAME_SPACE_TABLE_OFFSET, addr::GAME_SPACE_TABLE_COUNT_OFFSET, &disabled)) {
    return false;
  }
  std::sort(enabled->begin(), enabled->end());
  std::sort(disabled.begin(), disabled.end());
  std::set_union(enabled->begin(), enabled->end(), disabled.begin(), disabled.end(), std::back_inserter(*loaded));
  loaded->erase(std::unique(loaded->begin(), loaded->end()), loaded->end());
  return true;
}

void LogA(bool add, std::uint64_t level, const char* table, std::uint64_t frame) {
  Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] probe_sub src=A %s level=%s table=%s frame=%llu", add ? "add" : "del",
      HexLevel(level).c_str(), table, static_cast<unsigned long long>(frame));
}

void LogSummary() {
  Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] %s dropped=%u", g_matcher.Summary().c_str(), DroppedLevelEvents());
}

}  // namespace

void SublevelStart(const char* hook_refusal) {
  if (hook_refusal == nullptr) {
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] probe_sub hook=installed");
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] probe_sub hook=refused reason=%s (source A is still logged)",
        hook_refusal);
  }
}

void SublevelTick(std::uint64_t frame, const std::vector<LevelEvent>& events) {
  std::vector<std::string> lines;
  for (const LevelEvent& event : events) {
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] probe_sub src=B %s level=%s ok=%d noop=%d frame=%llu",
        event.enable ? "enable" : "disable", HexLevel(event.level).c_str(), event.ok ? 1 : 0, event.noop ? 1 : 0,
        static_cast<unsigned long long>(event.frame));
    if (event.ok && !event.noop) g_matcher.OnB(event.enable, event.level, event.frame);
  }

  std::vector<std::uint64_t> enabled;
  std::vector<std::uint64_t> loaded;
  if (ReadSpaces(&enabled, &loaded)) {
    if (!g_haveBase) {
      g_haveBase = true;
      Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] probe_sub src=A base enabled=%u loaded=%u frame=%llu",
          static_cast<unsigned>(enabled.size()), static_cast<unsigned>(loaded.size()),
          static_cast<unsigned long long>(frame));
    } else {
      std::vector<std::uint64_t> added;
      std::vector<std::uint64_t> removed;
      DiffLevels(g_enabled, enabled, &added, &removed);
      for (std::uint64_t level : added) {
        LogA(true, level, "enabled", frame);
        g_matcher.OnA(true, level, frame, &lines);
      }
      for (std::uint64_t level : removed) {
        LogA(false, level, "enabled", frame);
        g_matcher.OnA(false, level, frame, &lines);
      }
      DiffLevels(g_loaded, loaded, &added, &removed);
      for (std::uint64_t level : added) LogA(true, level, "loaded", frame);
      for (std::uint64_t level : removed) LogA(false, level, "loaded", frame);
    }
    g_enabled.swap(enabled);
    g_loaded.swap(loaded);
  }

  g_matcher.Expire(frame, false, &lines);
  LogLines(lines);

  const std::uint64_t main = MainLevel();
  if (main != g_lastMain) {
    g_lastMain = main;
    LogSummary();
  }
}

void SublevelFinish() {
  std::vector<std::string> lines;
  g_matcher.Expire(0, true, &lines);
  LogLines(lines);
  LogSummary();
}

}  // namespace nevr_maps_probe
