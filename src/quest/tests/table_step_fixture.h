// What the four resource table-step host tests (meshstep_test, instmodel_test, shaderstep_test, tablestep_test) share: the game the scope reads,
// the log capture and the libr15 image helpers (#2177, src/quest/combat/table_step.h).
//
// The game is a model: CR15Game's two gamespace tables and a gamespace's level, laid out where combat_level.cpp reads them (the offsets are
// spelled here, not taken from the header, as combat_level_test does), read by process_vm_readv as in production. A test sets the loaded levels
// with PutLevel and the module's scope reads them through table_step::SetGameForTest.
#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/table_step.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

namespace table_step_test {

// ---- the game -------------------------------------------------------------------------------------------------------------------

constexpr std::size_t kTable0 = 0x1f48, kCount0 = 0x1f78, kTable1 = 0x1f88, kCount1 = 0x1fb8;
constexpr std::size_t kGamespaceOwner = 0x8, kGamespaceLevel = 0x58;

struct Gamespace {
  std::uintptr_t words[0x60 / 8 + 1] = {};
};
struct Game {
  std::uintptr_t words[0x2000 / 8] = {};
  std::uintptr_t table0[8][2] = {};
  std::uintptr_t table1[8][2] = {};
  Gamespace spaces[8];
};
inline Game g_game;
inline std::uintptr_t GameAddress() { return reinterpret_cast<std::uintptr_t>(g_game.words); }
inline std::uintptr_t ModelGame() noexcept { return GameAddress(); }
inline std::uintptr_t NoModelGame() noexcept { return 0; }
inline void ClearGame() { g_game = Game{}; }

// A gamespace of the game holding `level`, in `table` (0 or 1) at `row`.
inline void PutLevel(int table, int row, std::uint64_t level) {
  Gamespace& gs = g_game.spaces[table * 4 + row];
  gs.words[kGamespaceOwner / 8] = GameAddress();
  gs.words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(level);
  std::uintptr_t(*rows)[2] = table == 0 ? g_game.table0 : g_game.table1;
  rows[row][0] = 0x5555;
  rows[row][1] = reinterpret_cast<std::uintptr_t>(&gs);
  g_game.words[(table == 0 ? kTable0 : kTable1) / 8] = reinterpret_cast<std::uintptr_t>(rows);
  std::uintptr_t& count = g_game.words[(table == 0 ? kCount0 : kCount1) / 8];
  if (static_cast<std::uintptr_t>(row) + 1 > count) count = static_cast<std::uintptr_t>(row) + 1;
}
inline std::uint64_t Level(const char* name) { return EchoVR::CSymbol64Hash(name); }

// What the loaded levels say; the scope reads the model.
enum class Where { kCombatMap, kArena, kLobbyArenaUses, kCombatLobby, kCombatMapDuringArena, kNothingLoaded };
inline void SetWhere(Where where) {
  ClearGame();
  switch (where) {
    case Where::kCombatMap: PutLevel(0, 0, Level("mpl_combat_gauss")); break;
    case Where::kArena: PutLevel(0, 0, Level("mpl_arena_a")); break;
    case Where::kLobbyArenaUses: PutLevel(0, 0, Level("mpl_lobby_b")); break;
    case Where::kCombatLobby: PutLevel(0, 0, Level("mpl_lobby_b_combat")); break;
    case Where::kCombatMapDuringArena:  // a transition: the combat map is still loaded when the arena level is stored
      PutLevel(0, 0, Level("mpl_combat_gauss"));
      PutLevel(1, 0, Level("mpl_arena_b"));
      break;
    case Where::kNothingLoaded: break;
  }
}

// ---- the log --------------------------------------------------------------------------------------------------------------------

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
inline std::vector<Line> g_lines;
inline void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }
inline bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
inline std::vector<Line> Lines(const char* event) {
  std::vector<Line> out;
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const Line& l : g_lines) {
    if (Has(l.text, key)) out.push_back(l);
  }
  return out;
}
// The line from its `"event"` key on: the timestamp is the one part of a line a test cannot name.
inline std::string Golden(const std::string& line) {
  const std::size_t at = line.find("\"event\"");
  return at == std::string::npos ? line : line.substr(at);
}

// ---- the image ------------------------------------------------------------------------------------------------------------------

inline combat_test::Image g_image;
inline bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
// Writes the pinned words into the image at their vaddrs.
template <typename Words, std::size_t N>
void PutWords(const Words (&words)[N]) {
  for (const auto& w : words) g_image.Put<std::uint32_t>(w.vaddr, w.word);
}

// ---- the plugin's data directory --------------------------------------------------------------------------------------------------

// A directory holding the kill file `file` (or none, with nullptr), made for one check and removed by RemoveLegacyDir.
inline std::string MakeLegacyDir(const char* file) {
  char dir[] = "/tmp/table_step_legacy_XXXXXX";
  QCHECK(mkdtemp(dir) != nullptr);
  if (file != nullptr) {
    const std::string path = std::string(dir) + "/" + file;
    const int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
    QCHECK(fd >= 0);
    if (fd >= 0) close(fd);
  }
  return dir;
}
inline void RemoveLegacyDir(const std::string& dir, const char* file) {
  if (file != nullptr) unlink((dir + "/" + file).c_str());
  rmdir(dir.c_str());
}

inline void Begin() {
  g_lines.clear();
  nevr_quest::combat::table_step::ResetForTest();
  nevr_quest::combat::table_step::SetGameForTest(&ModelGame);
  nevr_quest::combat::table_step::SetLegacyDirForTest("/nonexistent-table-step-test-dir");
  sentinel::SetLogSink(&Capture);
}
inline void End() {
  nevr_quest::combat::table_step::ResetForTest();
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
}

}  // namespace table_step_test
