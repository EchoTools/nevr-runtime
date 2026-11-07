// Host test of the combat music module (src/quest/combat/music.cpp, #2184; moved from the q15t plugin's music_test): the real handler through the real
// thunk, installed by the real GotHook into the JUMP_SLOT of a libr15 image built in memory, and called the way the game calls it: through the slot.
// The game side is a model of CWwiseEngine::PostSoundEvent that records every call's registers.
//
// Moved from the plugin's test, the assertions kept: the 24 bank ids and their kinds, the near misses and the high bits of the event register, the Stops
// before each Play (order, objects, flags 0, no callback, no cookie, no seek, an id pointer of their own), the game's own arguments and result untouched, no
// Stop on any other event, the unload tick (once, on the right object and self, not before a Play, 0.5 s period, re-armed by a new Play), the refusals (a foreign
// word, a wrong build, a pin that differs, a missing library) with the slot left alone, and the pin table's fingerprint. What changed is the fixture (a libr15
// image and the manager's install, not a fake slot page) and the scope: a Play acts only while a combat map is loaded and no Echo Arena level is. New here: an
// arena level, a lobby and an unknown level pass the Play through untouched, the switch and its reasons, the legacy `music.off` file warned about, and the
// cost of a post that is not in the bank.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/music.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

namespace {

namespace mu = nevr_quest::combat::music;
namespace tgt = nevr_quest::combat::music::targets;
namespace tick = nevr_quest::combat::tick;
namespace pinned = sentinel::pinned;

// Every SafeRead of the process counts here (safe_read.h's test seam, set first thing in main), so a test can show a path read nothing.
std::atomic<std::uint64_t> g_safeReads{0};
std::uint64_t SafeReadCalls() { return g_safeReads.load(std::memory_order_relaxed); }

using combat_test::Image;
using EchoVR::CSymbol64Hash;

// ---- the game's PostSoundEvent: every call's registers ---------------------------------------------------------------------------------
struct Call {
  void* self;
  std::uint32_t event, object, flagsValue;
  void* callback;
  void* cookie;
  std::uint32_t seekMs;
  std::uint32_t* playingId;
  std::uint32_t playingIdBefore;
};
std::vector<Call> g_calls;
constexpr std::uint64_t kResult = 0xfeedbeef00000001ULL;  // the game's own answer, handed back whole
std::uint32_t g_nextId = 100;
bool g_record = true;  // off while the cost is measured: the model then does no more than a call and a store

std::uint64_t FakePost(void* self, std::uint32_t event, std::uint32_t object, std::uint32_t* flags, void* callback, void* cookie, std::uint32_t seekMs,
                       std::uint32_t* playingId) {
  if (g_record) g_calls.push_back({self, event, object, flags != nullptr ? *flags : 0xdeadU, callback, cookie, seekMs, playingId, playingId != nullptr ? *playingId : 0xdeadU});
  if (playingId != nullptr) *playingId = g_record ? ++g_nextId : 1;
  return kResult;
}

// ---- the game's levels (as pfx_test spells them) ----------------------------------------------------------------------------------------
constexpr std::size_t kTable0 = 0x1f48, kCount0 = 0x1f78, kGamespaceOwner = 0x8, kGamespaceLevel = 0x58;
struct Gamespace {
  std::uintptr_t words[0x60 / 8 + 1] = {};
};
// The gamespaces still loading: init fibers (the gamespace at fiber + 0x30) in a table of pointers at game + 0x1ed8, counted at + 0x1f08 (spelled here, as combat_level_test does).
constexpr std::size_t kFiberRows = 0x1ed8, kFiberCount = 0x1f08, kFiberGamespace = 0x30;
struct Game {
  std::uintptr_t words[0x2000 / 8] = {};
  std::uintptr_t table0[8][2] = {};
  Gamespace spaces[8];
  std::uintptr_t fiberRows[4] = {};
  std::uintptr_t fibers[4][8] = {};
  Gamespace flight[4];
};
Game g_game;
std::uintptr_t GameAddress() { return reinterpret_cast<std::uintptr_t>(g_game.words); }
void LoadLevels(std::initializer_list<const char*> names) {
  g_game = Game{};
  int row = 0;
  for (const char* name : names) {
    Gamespace& gs = g_game.spaces[row];
    gs.words[kGamespaceOwner / 8] = GameAddress();
    gs.words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(CSymbol64Hash(name));
    g_game.table0[row][0] = 0x5555;
    g_game.table0[row][1] = reinterpret_cast<std::uintptr_t>(&gs);
    ++row;
  }
  g_game.words[kTable0 / 8] = reinterpret_cast<std::uintptr_t>(g_game.table0);
  g_game.words[kCount0 / 8] = static_cast<std::uintptr_t>(row);
}

// A gamespace still loading that holds `name`: in the init-fiber table, in neither of the two loaded tables.
void AddInFlight(const char* name) {
  const std::uintptr_t row = g_game.words[kFiberCount / 8];
  Gamespace& gs = g_game.flight[row];
  gs.words[kGamespaceOwner / 8] = GameAddress();
  gs.words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(CSymbol64Hash(name));
  g_game.fibers[row][kFiberGamespace / 8] = reinterpret_cast<std::uintptr_t>(&gs);
  g_game.fiberRows[row] = reinterpret_cast<std::uintptr_t>(g_game.fibers[row]);
  g_game.words[kFiberRows / 8] = reinterpret_cast<std::uintptr_t>(g_game.fiberRows);
  g_game.words[kFiberCount / 8] = row + 1;
}

// ---- the image, the clock, the lines -------------------------------------------------------------------------------------------------------
Image g_image;
tgt::Pins g_pins = tgt::kPins;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
std::int64_t g_nowMs = 1000;
std::int64_t Clock() noexcept { return g_nowMs; }

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
std::vector<Line> Lines(const char* event) {
  std::vector<Line> out;
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const Line& l : g_lines) {
    if (Has(l.text, key)) out.push_back(l);
  }
  return out;
}
std::string Golden(const std::string& line) {
  const std::size_t at = line.find("\"event\"");
  return at == std::string::npos ? line : line.substr(at);
}

// A libr15 holding the pinned code words and the slot holding the model of the game's function.
void Build() {
  g_pins = tgt::kPins;
  g_image.Build({{tgt::kSymbol, g_pins.slotVaddr, false, false, g_pins.stockVaddr}});
  const std::uintptr_t base = g_image.Base();
  g_pins.stockVaddr = reinterpret_cast<std::uintptr_t>(&FakePost) - base;
  for (const tgt::Word& w : tgt::kWords) g_image.Put<std::uint32_t>(w.vaddr, w.word);
  *g_image.Slot(g_pins.slotVaddr) = reinterpret_cast<void*>(&FakePost);
}
tgt::Sig* Slot() { return reinterpret_cast<tgt::Sig*>(*g_image.Slot(g_pins.slotVaddr)); }

void Begin() {
  g_lines.clear();
  g_calls.clear();
  g_nextId = 100;
  g_nowMs = 1000;
  mu::ResetForTest();
  tick::ResetForTest();
  mu::SetClockForTest(&Clock);
  mu::SetLegacyDirForTest(nullptr);
  LoadLevels({});
  sentinel::SetLogSink(&Capture);
}
void End() {
  mu::ResetForTest();
  tick::ResetForTest();
  mu::SetLegacyDirForTest(nullptr);
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
}
nevr_quest::combat::ModuleResult InstallOn() { return mu::InstallAt(true, &Lookup, g_pins); }

// The frame tick has seen the game (what the combat tick does each frame).
void Frame() { mu::Tick(GameAddress()); }

// One post through the slot, with the registers the model records; the id word starts at 0.
std::uint32_t g_flags = 0;
std::uint32_t g_id = 0;
std::uint64_t Post(std::uint32_t event, std::uint32_t object, void* self = reinterpret_cast<void*>(0x5e1f)) {
  g_flags = 0x77;
  g_id = 0;
  return Slot()(self, event, object, &g_flags, reinterpret_cast<void*>(0xca11), reinterpret_cast<void*>(0xc00c1e), 250, &g_id);
}

constexpr std::uint32_t kPlayCapture = 0x2f22c384, kPlayPayload = 0xb1b06cfb, kStopA = 0xde586538, kStopB = 0x2acba759;

// ---- the tests ----------------------------------------------------------------------------------------------------------------------------

// The bank, restated here as the plugin's test did: 24 ids, each of its kind; near misses and the unused values are not in it.
void TheBankIsTwentyFourIdsOfThreeKinds() {
  static const std::uint32_t kPlay[2] = {0x2f22c384, 0xb1b06cfb};
  static const std::uint32_t kStop[2] = {0xde586538, 0x2acba759};
  static const std::uint32_t kState[20] = {0x045d3c23, 0x215e97af, 0x4a61827d, 0x6a599cee, 0x7a9959a5, 0x98a1b868, 0xa3117938,
                                           0xaee8f745, 0xd3282d2b, 0xd9a48db0, 0xd9a48db1, 0xd9a48db3, 0xdb7c7cf9, 0xe34a79b4,
                                           0xe34a79b5, 0xe34a79b6, 0xe44bd2d0, 0xf0a25298, 0xf7898c0b, 0xffae8834};
  for (const std::uint32_t id : kPlay) QCHECK(mu::Classify(id) == mu::Kind::kPlay);
  for (const std::uint32_t id : kStop) QCHECK(mu::Classify(id) == mu::Kind::kStop);
  for (const std::uint32_t id : kState) QCHECK(mu::Classify(id) == mu::Kind::kState);
  // the module's own lists are the same 24 ids
  QCHECK(sizeof(mu::kPlay) / sizeof(mu::kPlay[0]) == 2U && sizeof(mu::kStop) / sizeof(mu::kStop[0]) == 2U && sizeof(mu::kState) / sizeof(mu::kState[0]) == 20U);
  for (std::size_t i = 0; i < 2; ++i) QCHECK(mu::kPlay[i] == kPlay[i] && mu::kStop[i] == kStop[i]);
  for (std::size_t i = 0; i < 20; ++i) QCHECK(mu::kState[i] == kState[i]);
  // near misses of every bank id, and the values a table could mistake for one
  for (const std::uint32_t id : kPlay) QCHECK(mu::Classify(id + 1) == mu::Kind::kNone && mu::Classify(id - 1) == mu::Kind::kNone);
  for (const std::uint32_t id : kStop) QCHECK(mu::Classify(id + 1) == mu::Kind::kNone && mu::Classify(id - 1) == mu::Kind::kNone);
  QCHECK(mu::Classify(0) == mu::Kind::kNone && mu::Classify(0xffffffffU) == mu::Kind::kNone && mu::Classify(0x80000000U) == mu::Kind::kNone);
  QCHECK(mu::Classify(0xd9a48db2) == mu::Kind::kNone);  // between the 20 states' neighbours db0, db1 and db3
  QCHECK(mu::Classify(0x045d3c22) == mu::Kind::kNone && mu::Classify(0xffae8835) == mu::Kind::kNone);  // outside the table at either end
}

void ThePinsAreTheReadelfAndDisassemblyPins() {
  QCHECK(tgt::kSlotVaddr == 0x36d76e8ULL && tgt::kStockVaddr == 0x2549264ULL);
  QCHECK(tgt::kPins.slotVaddr == 0x36d76e8ULL && tgt::kPins.stockVaddr == 0x2549264ULL);
  QCHECK(std::strcmp(tgt::kSymbol, "_ZN10NRadEngine12CWwiseEngine14PostSoundEventENS_12CSoundSymbolEjNS_7CFlagsTIjLy1EEEPFviPvES4_jRj") == 0);
  QCHECK(tgt::kWordCount == 29);
  // the 29 words, restated from the disassembly (0x2649264..0x26492d4 less 0x100000)
  static const struct {
    std::uint64_t va;
    std::uint32_t word;
  } kWant[29] = {
      {0x2549264, 0xd10183ff},
      {0x2549268, 0xa9016bfb},
      {0x254926c, 0xa90263f9},
      {0x2549270, 0xa9035bf7},
      {0x2549274, 0xa90453f5},
      {0x2549278, 0xa9057bf3},
      {0x254927c, 0xb9400068},
      {0x2549280, 0x2a0203e9},
      {0x2549284, 0x3100045f},
      {0x2549288, 0xaa0103f4},
      {0x254928c, 0xda9f1135},
      {0x2549290, 0xaa0703f6},
      {0x2549294, 0x2a0603f3},
      {0x2549298, 0xaa0503f7},
      {0x254929c, 0xaa0403f8},
      {0x25492a0, 0xaa0303f9},
      {0x25492a4, 0xaa0003fa},
      {0x25492a8, 0x2a1403e0},
      {0x25492ac, 0xaa1503e1},
      {0x25492b0, 0x2a0803e2},
      {0x25492b4, 0xaa0403e3},
      {0x25492b8, 0xaa0503e4},
      {0x25492bc, 0x2a1f03e5},
      {0x25492c0, 0xaa1f03e6},
      {0x25492c4, 0x2a1f03e7},
      {0x25492c8, 0x9410088e},
      {0x25492cc, 0xb90002c0},
      {0x25492d0, 0x340003c0},
      {0x25492d4, 0x34000b73},
  };
  for (std::size_t i = 0; i < 29; ++i) QCHECK(tgt::kWords[i].vaddr == kWant[i].va && tgt::kWords[i].word == kWant[i].word);
  QCHECK(tgt::kWords[0].vaddr == 0x2549264 && tgt::kWords[0].word == 0xd10183ff);   // sub sp, sp, #0x60
  QCHECK(tgt::kWords[6].vaddr == 0x254927c && tgt::kWords[6].word == 0xb9400068);   // ldr w8, [x3]: the flags are a pointer
  QCHECK(tgt::kWords[12].vaddr == 0x2549294 && tgt::kWords[12].word == 0x2a0603f3); // mov w19, w6: the seek
  QCHECK(tgt::kWords[26].vaddr == 0x25492cc && tgt::kWords[26].word == 0xb90002c0); // str w0, [x22]: *playing id
  QCHECK(tgt::kWords[28].vaddr == 0x25492d4 && tgt::kWords[28].word == 0x34000b73); // cbz w19: seek 0, no seek call
  for (std::size_t i = 1; i < tgt::kWordCount; ++i) QCHECK(tgt::kWords[i].vaddr == tgt::kWords[i - 1].vaddr + 4);
}

// The install: the slot is taken by the module's thunk, the line says so, and the tick is registered.
void TheInstallTakesTheSlotAndWritesOneLine() {
  Begin();
  Build();
  const nevr_quest::combat::ModuleResult r = InstallOn();
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
  QCHECK(Slot() != &FakePost);
  QCHECK(tick::Count() == 1U);
  const std::vector<Line> lines = Lines("combat_music_install");
  QCHECK(lines.size() == 1U);
  if (lines.size() == 1U) {
    QCHECK(lines[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(lines[0].text) ==
           "\"event\":\"combat_music_install\",\"state\":\"on\",\"reason\":\"default\",\"switch\":\"combat.music\",\"scope\":\"combat_map\","
           "\"status\":\"ok\",\"msg\":\"PostSoundEvent posts both music Stops before each combat music Play and when the combat map unloads; an Echo Arena "
           "level and every other sound pass through\"}");
  }
  End();
}

// Every other event reaches the game's function with the same registers and returns its answer whole; nothing is counted, nothing posted, nothing logged.
void AnEventOutsideTheBankPassesStraightThrough() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  LoadLevels({"mpl_combat_dyson"});
  Frame();
  g_calls.clear();
  g_lines.clear();
  for (const std::uint32_t event : {0x12345678U, 0U, 0xffffffffU, kPlayCapture + 1, kStopA - 1, 0xd9a48db2U}) {
    g_calls.clear();
    QCHECK(Post(event, 7) == kResult);
    QCHECK(g_calls.size() == 1U);
    if (g_calls.size() == 1U) {
      QCHECK(g_calls[0].event == event && g_calls[0].object == 7 && g_calls[0].flagsValue == 0x77 && g_calls[0].callback == reinterpret_cast<void*>(0xca11));
      QCHECK(g_calls[0].cookie == reinterpret_cast<void*>(0xc00c1e) && g_calls[0].seekMs == 250 && g_calls[0].playingId == &g_id);
    }
  }
  const mu::Stats s = mu::CurrentStats();
  QCHECK(s.bankPosts == 0 && s.plays == 0 && s.stopsPosted == 0);
  QCHECK(g_lines.empty());
  End();
}

// A Play in a combat map: both Stops on its object first, then the Play with the game's own registers.
void APlayInACombatMapPostsBothStopsFirst() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  LoadLevels({"mpl_combat_dyson"});
  Frame();
  g_calls.clear();
  void* const self = reinterpret_cast<void*>(0xabc0);
  QCHECK(Post(kPlayCapture, 8, self) == kResult);  // the game's answer, whole
  QCHECK(g_calls.size() == 3U);
  if (g_calls.size() == 3U) {
    // the extra Stops: PostEvent(stop, object, flags 0, no callback, no cookie), seek 0, an id word of their own that starts at 0
    QCHECK(g_calls[0].event == kStopA && g_calls[1].event == kStopB);
    for (int i = 0; i < 2; ++i) {
      QCHECK(g_calls[i].self == self && g_calls[i].object == 8 && g_calls[i].flagsValue == 0);
      QCHECK(g_calls[i].callback == nullptr && g_calls[i].cookie == nullptr && g_calls[i].seekMs == 0);
      QCHECK(g_calls[i].playingId != nullptr && g_calls[i].playingId != &g_id && g_calls[i].playingIdBefore == 0);
    }
    // then the Play, with the game's own arguments
    QCHECK(g_calls[2].event == kPlayCapture && g_calls[2].self == self && g_calls[2].object == 8 && g_calls[2].flagsValue == 0x77);
    QCHECK(g_calls[2].callback == reinterpret_cast<void*>(0xca11) && g_calls[2].cookie == reinterpret_cast<void*>(0xc00c1e));
    QCHECK(g_calls[2].seekMs == 250 && g_calls[2].playingId == &g_id);
  }
  QCHECK(g_id == 103);  // the game's own function wrote the Play's id
  const mu::Stats s = mu::CurrentStats();
  QCHECK(s.bankPosts == 1 && s.plays == 1 && s.playsActed == 1 && s.playsOutOfScope == 0 && s.stopsPosted == 2);
  const std::vector<Line> play = Lines("combat_music_play");
  QCHECK(play.size() == 1U);
  if (play.size() == 1U) {
    QCHECK(Has(play[0].text, "\"music_event\":\"0x000000002f22c384\"") && Has(play[0].text, "\"object\":8") && Has(play[0].text, "\"stops_posted\":2"));
    QCHECK(Has(play[0].text, "\"in_combat_map\":1") && Has(play[0].text, "\"prev_object\":4294967295"));
  }
  End();
}

// The second Play: on a new object it stops the previous Play's object too (four Stops, own object first); on the same object, two.
void ASecondPlayAlsoStopsThePreviousObject() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  LoadLevels({"mpl_combat_gauss"});
  Frame();
  Post(kPlayPayload, 8);
  g_calls.clear();
  Post(kPlayPayload, 9);
  QCHECK(g_calls.size() == 5U);
  if (g_calls.size() == 5U) {
    QCHECK(g_calls[0].object == 9 && g_calls[1].object == 9 && g_calls[2].object == 8 && g_calls[3].object == 8);
    QCHECK(g_calls[0].event == kStopA && g_calls[1].event == kStopB && g_calls[2].event == kStopA && g_calls[3].event == kStopB);
    QCHECK(g_calls[4].event == kPlayPayload && g_calls[4].object == 9);
  }
  g_calls.clear();
  Post(kPlayPayload, 9);  // the same object again: no second pair
  QCHECK(g_calls.size() == 3U);
  const std::vector<Line> play = Lines("combat_music_play");
  QCHECK(play.size() == 3U && Has(play[1].text, "\"prev_object\":8") && Has(play[1].text, "\"stops_posted\":4"));
  End();
}

// A Stop and a SetState are counted and passed through; neither posts anything of its own.
void AStopAndAStateArePassedThrough() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  LoadLevels({"mpl_combat_dyson"});
  Frame();
  g_calls.clear();
  QCHECK(Post(kStopA, 8) == kResult && Post(0xffae8834U, 8) == kResult && Post(0x045d3c23U, 8) == kResult);
  QCHECK(g_calls.size() == 3U && g_calls[0].event == kStopA && g_calls[1].event == 0xffae8834U && g_calls[2].event == 0x045d3c23U);
  const mu::Stats s = mu::CurrentStats();
  QCHECK(s.bankPosts == 3 && s.plays == 0 && s.stopsPosted == 0);
  const std::vector<Line> first = Lines("combat_music_first");
  QCHECK(first.size() == 1U && Has(first[0].text, "\"kind\":\"stop\""));  // once, for the first bank post
  End();
}

// ARENA: arena has its own music and runs on the same APK. A Play posted with no combat map loaded, on any Echo Arena level, in the lobbies, with an arena
// level still loaded beside a combat map (a transition), or before the first frame, is the game's call and nothing else.
void APlayOutsideACombatMapIsTheGamesCallAndNothingElse() {
  struct Case {
    const char* what;
    std::initializer_list<const char*> levels;
    bool frame;
  };
  const Case cases[] = {{"an arena level", {"mpl_arena_a"}, true},
                        {"the low-spec arena level", {"mpl_arena_a_lowspec"}, true},
                        {"the second arena level", {"mpl_arena_b"}, true},
                        {"the tutorial", {"mpl_tutorial_a"}, true},
                        {"an arena lobby", {"mpl_lobby_b"}, true},
                        {"the social lobby", {"mpl_lobby_b_social"}, true},
                        {"the combat lobby", {"mpl_lobby_b_combat"}, true},
                        {"no level", {}, true},
                        {"a combat map beside an arena level", {"mpl_combat_dyson", "mpl_arena_a"}, true},
                        {"a combat map before the first frame", {"mpl_combat_dyson"}, false}};
  for (const Case& c : cases) {
    Begin();
    Build();
    QCHECK(InstallOn().installed);
    LoadLevels(c.levels);
    if (c.frame) Frame();
    g_calls.clear();
    QCHECK(Post(kPlayCapture, 8) == kResult);
    QCHECK(g_calls.size() == 1U);  // the Play alone: no Stop before it
    if (g_calls.size() == 1U) {
      QCHECK(g_calls[0].event == kPlayCapture && g_calls[0].object == 8 && g_calls[0].flagsValue == 0x77 && g_calls[0].playingId == &g_id);
      QCHECK(g_calls[0].callback == reinterpret_cast<void*>(0xca11) && g_calls[0].seekMs == 250);
    }
    const mu::Stats s = mu::CurrentStats();
    QCHECK(s.plays == 1 && s.playsActed == 0 && s.playsOutOfScope == 1 && s.stopsPosted == 0);
    QCHECK(Has(Lines("combat_music_play")[0].text, "\"in_combat_map\":0"));
    // and the tick, however long it runs, posts nothing: no Play acted, so nothing is owed at the end of a match
    g_calls.clear();
    for (int i = 0; i < 10; ++i) {
      g_nowMs += 600;
      Frame();
    }
    QCHECK(g_calls.empty() && mu::CurrentStats().tickStops == 0);
    End();
  }
}

// The unload tick: not before a Play, not while the combat map is loaded, once when it is gone (on the Play's object and engine), every 0.5 s, re-armed by a Play.
void TheTickStopsTheMusicOnceWhenTheCombatMapIsGone() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  LoadLevels({"mpl_combat_dyson"});
  Frame();
  g_calls.clear();
  for (int i = 0; i < 5; ++i) {  // no Play yet: a tick posts nothing, even with the map gone
    g_nowMs += 600;
    if (i == 3) LoadLevels({"mpl_lobby_b"});
    Frame();
  }
  QCHECK(g_calls.empty());
  LoadLevels({"mpl_combat_dyson"});
  void* const self = reinterpret_cast<void*>(0x5e1f);
  Post(kPlayCapture, 8, self);
  g_calls.clear();
  g_nowMs += 600;
  Frame();
  QCHECK(g_calls.empty());  // the combat map is still loaded
  LoadLevels({"mpl_lobby_b"});
  g_nowMs += 100;
  Frame();
  QCHECK(g_calls.empty());  // the look is not due yet (the last one was 100 ms ago)
  g_nowMs += 450;
  Frame();
  QCHECK(g_calls.size() == 2U);
  if (g_calls.size() == 2U) {
    QCHECK(g_calls[0].event == kStopA && g_calls[1].event == kStopB && g_calls[0].object == 8 && g_calls[1].object == 8);
    QCHECK(g_calls[0].self == self && g_calls[1].self == self && g_calls[0].flagsValue == 0 && g_calls[0].callback == nullptr && g_calls[0].seekMs == 0);
  }
  const std::vector<Line> unload = Lines("combat_music_unload");
  QCHECK(unload.size() == 1U && Has(unload[0].text, "\"object\":8") && Has(unload[0].text, "\"stops_posted\":2"));
  g_calls.clear();
  for (int i = 0; i < 4; ++i) {  // once only
    g_nowMs += 600;
    Frame();
  }
  QCHECK(g_calls.empty() && mu::CurrentStats().tickStops == 1);
  // a new Play re-arms it
  LoadLevels({"mpl_combat_fission"});
  Frame();
  Post(kPlayPayload, 9, self);
  LoadLevels({});
  g_calls.clear();
  g_nowMs += 600;
  Frame();
  QCHECK(g_calls.size() == 2U && g_calls[0].object == 9);
  QCHECK(mu::CurrentStats().tickStops == 2);
  End();
}

// A combat map that is still loaded beside an Echo Arena level during a transition keeps the music: the tick stops it when the map is gone, as the plugin's did.
void TheTickLooksForTheMapNotForTheScope() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  LoadLevels({"mpl_combat_dyson"});
  Frame();
  Post(kPlayCapture, 8);
  LoadLevels({"mpl_combat_dyson", "mpl_arena_a"});
  g_calls.clear();
  g_nowMs += 600;
  Frame();
  QCHECK(g_calls.empty());
  End();
}

// The refusals leave the slot as it is and say why at Error; nothing is registered when no hook was made.
void ARefusedInstallChangesNothing() {
  struct Case {
    const char* reason;
  };
  {  // a library that is not loaded
    Begin();
    Build();
    const auto r = mu::InstallAt(true, [](const char*, sentinel::ElfImage*) { return false; }, g_pins);
    QCHECK(!r.installed && std::strcmp(r.status, "module_not_loaded") == 0);
    QCHECK(Slot() == &FakePost && tick::Count() == 0U);
    QCHECK(Has(Lines("combat_music_install")[0].text, "\"reason\":\"module_not_loaded\"") && Lines("combat_music_install")[0].level == sentinel::LogLevel::kError);
    End();
  }
  {  // a library of another build
    Begin();
    g_pins = tgt::kPins;
    g_image.Build({{tgt::kSymbol, g_pins.slotVaddr, false, false, g_pins.stockVaddr}}, "0123456789abcdef0123456789abcdef01234567");
    for (const tgt::Word& w : tgt::kWords) g_image.Put<std::uint32_t>(w.vaddr, w.word);
    *g_image.Slot(g_pins.slotVaddr) = reinterpret_cast<void*>(&FakePost);
    g_pins.stockVaddr = reinterpret_cast<std::uintptr_t>(&FakePost) - g_image.Base();
    const auto r = InstallOn();
    QCHECK(!r.installed && std::strcmp(r.status, "build_id_mismatch") == 0 && Slot() == &FakePost);
    End();
  }
  {  // a library with no build id
    Begin();
    g_pins = tgt::kPins;
    g_image.Build({{tgt::kSymbol, g_pins.slotVaddr, false, false, g_pins.stockVaddr}}, nullptr, /*note=*/false);
    const auto r = InstallOn();
    QCHECK(!r.installed && std::strcmp(r.status, "no_build_id") == 0);
    End();
  }
  for (std::size_t i = 0; i < tgt::kWordCount; ++i) {  // every pinned word, flipped alone
    Begin();
    Build();
    g_image.Put<std::uint32_t>(tgt::kWords[i].vaddr, tgt::kWords[i].word ^ 1U);
    const auto r = InstallOn();
    QCHECK(!r.installed && std::strcmp(r.status, "pin_mismatch") == 0);
    QCHECK(Slot() == &FakePost && tick::Count() == 0U);
    End();
  }
  {  // the slot holds a word that is not the game's function (another runtime's)
    Begin();
    Build();
    *g_image.Slot(g_pins.slotVaddr) = reinterpret_cast<void*>(&End);
    const auto r = InstallOn();
    QCHECK(!r.installed && std::strcmp(r.status, "hook") == 0);
    QCHECK(*g_image.Slot(g_pins.slotVaddr) == reinterpret_cast<void*>(&End));  // left as it was
    QCHECK(Has(Lines("combat_music_install")[0].text, "\"reason\":\"hook\"") && !Has(Lines("combat_music_install")[0].text, "\"status\":\"ok\""));
    End();
  }
  {  // the combat tick has no room
    Begin();
    Build();
    for (std::size_t i = 0; i < tick::kMaxCallbacks; ++i) QCHECK(tick::Add([](std::uintptr_t) noexcept {}));
    const auto r = InstallOn();
    QCHECK(!r.installed && std::strcmp(r.status, "tick_table_full") == 0 && Slot() == &FakePost);
    End();
  }
}

// The switch: off by the key, no hook and no tick, one Info line that says so; the plugin's kill file is warned about and changes nothing.
void TheSwitchAndTheLegacyKillFile() {
  Begin();
  Build();
  const auto off = mu::InstallAt(false, &Lookup, g_pins);
  QCHECK(!off.installed && std::strcmp(off.status, "off_config") == 0);
  QCHECK(Slot() == &FakePost && tick::Count() == 0U);
  const std::vector<Line> lines = Lines("combat_music_install");
  QCHECK(lines.size() == 1U && lines[0].level == sentinel::LogLevel::kInfo && Has(lines[0].text, "\"state\":\"off\"") && Has(lines[0].text, "\"reason\":\"off_config\""));
  QCHECK(Lines("combat_music_legacy_switch").empty());
  End();
  // a kill file in the plugin's directory: a Warning that names the key, and the module installs as the key says
  char dir[] = "/tmp/music_legacy_XXXXXX";
  if (mkdtemp(dir) == nullptr) {
    QCHECK(false);
    return;
  }
  const std::string file = std::string(dir) + "/music.off";
  std::FILE* f = std::fopen(file.c_str(), "w");
  QCHECK(f != nullptr);
  if (f != nullptr) std::fclose(f);
  Begin();
  Build();
  mu::SetLegacyDirForTest(dir);
  QCHECK(InstallOn().installed);
  const std::vector<Line> legacy = Lines("combat_music_legacy_switch");
  QCHECK(legacy.size() == 1U && legacy[0].level == sentinel::LogLevel::kWarn && Has(legacy[0].text, "\"replaced_by\":\"combat.music: false\""));
  End();
  std::remove(file.c_str());
  rmdir(dir);
}

// The line budget: the first kMaxPlayLines Plays are written, the rest only counted.
void PlayLinesAreCapped() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  LoadLevels({"mpl_combat_dyson"});
  Frame();
  for (std::size_t i = 0; i < mu::kMaxPlayLines + 10; ++i) Post(kPlayCapture, 8);
  QCHECK(Lines("combat_music_play").size() == mu::kMaxPlayLines);
  QCHECK(mu::CurrentStats().plays == mu::kMaxPlayLines + 10);
  End();
}

// ARENA'S OWN MUSIC STAYS STOCK: the same stream of posts (all 24 bank ids, an arena-music-like set of other ids, odd registers) goes to the game's function
// directly and through the installed module on an Echo Arena level and in the lobbies arena players use; the game sees the same calls, in the same order, with
// the same registers, and answers the same. Nothing is posted that the game's own stream did not hold.
void ArenaMusicIsExactlyStock() {
  struct Level {
    const char* what;
    const char* name;
  };
  const Level levels[] = {{"an arena level", "mpl_arena_a"}, {"the arena lobby", "mpl_lobby_b"}, {"the social lobby", "mpl_lobby_b_social"}, {"no level", nullptr}};
  std::vector<std::uint32_t> stream;
  for (const std::uint32_t id : mu::kPlay) stream.push_back(id);
  for (const std::uint32_t id : mu::kStop) stream.push_back(id);
  for (const std::uint32_t id : mu::kState) stream.push_back(id);
  for (std::uint32_t i = 0; i < 40; ++i) stream.push_back(CSymbol64Hash("mp_arena_music") * 2654435761U + i * 0x9e3779b1U);
  stream.push_back(0);
  stream.push_back(0xffffffffU);
  const auto run = [&](bool hooked, const Level& level) {
    Begin();
    Build();
    if (hooked) QCHECK(InstallOn().installed);
    if (level.name != nullptr) LoadLevels({level.name});
    if (hooked) Frame();
    g_calls.clear();
    std::vector<std::uint64_t> results;
    tgt::Sig* const post = hooked ? Slot() : &FakePost;
    std::uint32_t object = 0;
    for (const std::uint32_t event : stream) {
      ++object;
      g_flags = 0x77 + (object & 3);
      g_id = 0;
      results.push_back(post(reinterpret_cast<void*>(0x5e1f), event, object, &g_flags, reinterpret_cast<void*>(0xca11), reinterpret_cast<void*>(0xc00c1e),
                             static_cast<std::uint32_t>(object * 3), &g_id));
      results.push_back(g_id);
    }
    const std::vector<Call> calls = g_calls;
    const mu::Stats stats = mu::CurrentStats();
    if (hooked) {
      for (int i = 0; i < 4; ++i) {  // and the tick, over a long arena match, posts nothing
        g_nowMs += 600;
        Frame();
      }
    }
    const std::size_t extra = g_calls.size() - calls.size();
    End();
    return std::make_tuple(calls, results, stats.stopsPosted + extra);
  };
  for (const Level& level : levels) {
    const auto direct = run(false, level);
    const auto through = run(true, level);
    const std::vector<Call>& a = std::get<0>(direct);
    const std::vector<Call>& b = std::get<0>(through);
    QCHECK(a.size() == stream.size() && b.size() == a.size());
    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i) {
      same = a[i].self == b[i].self && a[i].event == b[i].event && a[i].object == b[i].object && a[i].flagsValue == b[i].flagsValue &&
             a[i].callback == b[i].callback && a[i].cookie == b[i].cookie && a[i].seekMs == b[i].seekMs && a[i].playingIdBefore == b[i].playingIdBefore;
    }
    if (!same) std::fprintf(stderr, "  arena stream differs on %s\n", level.what);
    QCHECK(same);
    QCHECK(std::get<1>(direct) == std::get<1>(through));  // every answer and every playing id the game wrote
    QCHECK(std::get<2>(through) == 0);                    // no extra Stop was posted, by a Play or by the tick
  }
}

// ARENA PAYS NOTHING, BY COUNT. A post that is not in the bank reads no game memory and asks no level; a Stop or a SetState asks nothing either; a Play asks the
// level once (a few reads), never per post. A scan on every sound, or a read before the bank test, passes the stream test above and fails here.
void ArenaPaysNothingByCount() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  LoadLevels({"mpl_arena_a"});
  Frame();
  sentinel::SetLogSink(nullptr);
  const auto counts = [] { return std::make_pair(SafeReadCalls(), nevr_quest::combat::level::ScanCalls()); };
  const auto before = counts();
  for (std::uint32_t i = 0; i < 2000; ++i) Post(0x12345678U + i * 7919U, i);  // none of them is in the bank
  const auto afterOthers = counts();
  QCHECK(afterOthers.first == before.first && afterOthers.second == before.second);
  for (const std::uint32_t id : mu::kStop) Post(id, 3);
  for (const std::uint32_t id : mu::kState) Post(id, 3);
  const auto afterBank = counts();
  QCHECK(afterBank.first == before.first && afterBank.second == before.second);
  for (const std::uint32_t id : mu::kPlay) Post(id, 3);
  const auto afterPlays = counts();
  QCHECK(afterPlays.second - before.second == 2);  // one ask per Play
  // and the tick, with no acted Play, asks nothing in a long arena match
  for (int i = 0; i < 10; ++i) {
    g_nowMs += 600;
    Frame();
  }
  QCHECK(nevr_quest::combat::level::ScanCalls() == afterPlays.second);
  End();
}

// MAP TO MAP: a Play posted while its combat map is still building (its gamespace is in the init-fiber table, not yet in the loaded tables) is the map's Play,
// whether the lobby or the previous combat map is the one still loaded. Upstream posts the Stops on every Play; here the ask is made with the init-fiber table
// read, so the Stops are posted in these states and not in the states where the new level is arena's.
void APlayWhileItsMapIsStillBuildingPostsTheStops() {
  struct Case {
    const char* what;
    const char* loaded;
    const char* loading;
    bool acts;
  };
  const Case cases[] = {{"a combat map building after the lobby", "mpl_lobby_b", "mpl_combat_gauss", true},
                        {"a combat map building after the combat lobby", "mpl_lobby_b_combat", "mpl_combat_dyson", true},
                        {"the next combat map building while the previous one is still loaded", "mpl_combat_dyson", "mpl_combat_fission", true},
                        {"an arena level building while a combat map is still loaded", "mpl_combat_dyson", "mpl_arena_a", false},
                        {"a lobby building while a combat map is still loaded", "mpl_combat_dyson", "mpl_lobby_b", false}};
  for (const Case& c : cases) {
    Begin();
    Build();
    QCHECK(InstallOn().installed);
    LoadLevels({c.loaded});
    AddInFlight(c.loading);
    Frame();
    g_calls.clear();
    QCHECK(Post(kPlayCapture, 8) == kResult);
    if (c.acts != (g_calls.size() == 3U)) std::fprintf(stderr, "  map to map: %s\n", c.what);
    QCHECK(g_calls.size() == (c.acts ? 3U : 1U));
    QCHECK(mu::CurrentStats().playsActed == (c.acts ? 1U : 0U));
    End();
  }
  // and a map that is not loading, with nothing loaded, is still not a combat match
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  LoadLevels({"mpl_lobby_b"});
  Frame();
  g_calls.clear();
  Post(kPlayCapture, 8);
  QCHECK(g_calls.size() == 1U);
  End();
}

// The cost of a post that is not in the bank, with the module installed on an arena level (what an arena player's every sound pays), against a direct call
// of the same function. Printed, not asserted: it is a host number, not a Quest one.
void TheCostOfAPostOutsideTheBank() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  LoadLevels({"mpl_arena_a"});
  Frame();
  sentinel::SetLogSink(nullptr);
  g_record = false;
  constexpr int kCalls = 2000000;
  std::uint32_t flags = 0, id = 0;
  tgt::Sig* const hooked = Slot();
  auto run = [&](tgt::Sig* fn) {
    double best = 1e18;
    for (int rep = 0; rep < 7; ++rep) {
      g_calls.clear();
      const auto start = std::chrono::steady_clock::now();
      volatile std::uint64_t sink = 0;
      for (int i = 0; i < kCalls; ++i) {
        sink = sink + fn(nullptr, 0x12345678U + static_cast<std::uint32_t>(i & 7), 1, &flags, nullptr, nullptr, 0, &id);
      }
      const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kCalls;
      if (ns < best) best = ns;
    }
    return best;
  };
  const double direct = run(&FakePost);
  const double through = run(hooked);
  g_record = true;
  double classify = 1e18;
  for (int rep = 0; rep < 7; ++rep) {
    const auto start = std::chrono::steady_clock::now();
    volatile std::uint32_t sink = 0;
    for (int i = 0; i < kCalls; ++i) sink = sink + static_cast<std::uint32_t>(mu::Classify(0x12345678U + static_cast<std::uint32_t>(i & 7)));
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kCalls;
    if (ns < classify) classify = ns;
  }
  std::printf("music_test: Classify alone, host: %.1f ns\n", classify);
  std::printf("music_test: a post outside the bank, host: direct %.1f ns, through the thunk and the bank search %.1f ns (+%.1f ns); a bank Classify alone is a 5-step search\n",
              direct, through, through - direct);
  End();
}

}  // namespace

int main() {
  nevr_quest::combat::SetReadCounterForTest(&g_safeReads);
  TheBankIsTwentyFourIdsOfThreeKinds();
  ThePinsAreTheReadelfAndDisassemblyPins();
  TheInstallTakesTheSlotAndWritesOneLine();
  AnEventOutsideTheBankPassesStraightThrough();
  APlayInACombatMapPostsBothStopsFirst();
  ASecondPlayAlsoStopsThePreviousObject();
  AStopAndAStateArePassedThrough();
  APlayOutsideACombatMapIsTheGamesCallAndNothingElse();
  TheTickStopsTheMusicOnceWhenTheCombatMapIsGone();
  TheTickLooksForTheMapNotForTheScope();
  ARefusedInstallChangesNothing();
  TheSwitchAndTheLegacyKillFile();
  PlayLinesAreCapped();
  ArenaMusicIsExactlyStock();
  ArenaPaysNothingByCount();
  APlayWhileItsMapIsStillBuildingPostsTheStops();
  TheCostOfAPostOutsideTheBank();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "music_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("music_test: all checks passed");
  return 0;
}
