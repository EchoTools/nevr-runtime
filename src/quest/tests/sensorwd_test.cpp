// Host test of the scanner-sphere watchdog (src/quest/combat/sensorwd.cpp, #2184): the real handlers through the
// real thunks, installed by the real GotHook into the two vtable words (ABS64) and the JUMP_SLOT of a libr15 image built in memory, and called the way the game
// calls them: through the word. The game side is a model of OnComponentEnabled, OnComponentDisabled and SendComponentEventGlobal that records every call, and
// models of CModelCS components laid out where the handlers read them (the offsets are spelled here, not taken from the module).
//
// Moved from the plugin's test, the assertions kept: the constants and ids, the three words swapped together or not at all (a word that does not hold the game's
// function refuses the module and puts back what was done), every argument and the result passed on whole, a sphere timed from its enable and turned off by the
// tick after 6000 ms through the game's own function (the handlers do not see that call), turned back on at the next ping of its own actor, a model that is not the
// sphere never counted, the table and the ping queue at their limits, the sweep, and the state lock never held across a call into the game or a log line. What
// changed is the fixture (a libr15 image and the manager's install) and the scope: the handlers act only while a combat map is loaded and no Echo Arena level
// is. New here: an arena level, a lobby, no level, a stale answer and a missing frame leave every handler a pass-through that reads nothing, takes no lock and
// counts nothing; leaving the scope empties the table; the switch and its reasons; the legacy `sensorwd.off` file warned about; the cost on an arena path.
// The scope is the real combat level answer (combat_scope.h, #2382): the tick is installed on an image of its own and the scope row has registered the
// refresh before each test, each frame runs that refresh against the fake game before the module's callback (as the tick does), and a scope that cannot
// change refuses the install (`scope_not_live`).
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/sensorwd.h"

#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_scope_world.h"
#include "quest/tests/test_check.h"

namespace {

// Every SafeRead of the process counts here (safe_read.h's test seam, set first thing in main), so a test can show a path read nothing.
std::atomic<std::uint64_t> g_safeReads{0};
std::uint64_t SafeReadCalls() { return g_safeReads.load(std::memory_order_relaxed); }

namespace sw = nevr_quest::combat::sensorwd;
namespace tgt = nevr_quest::combat::sensorwd::targets;
namespace tick = nevr_quest::combat::tick;
namespace scope = nevr_quest::combat::scope;
namespace world = combat_test::scope_world;
namespace pinned = sentinel::pinned;
using combat_test::Image;
using EchoVR::CSymbol64Hash;

constexpr std::uint64_t kSphere = 0xac2d3f9f080ba45fULL;
constexpr std::uint64_t kTriggered = 0xfd8cf56544b904a3ULL;
constexpr std::uint64_t kFinished = 0x7cc621b8bf3248c5ULL;

// ---- the game's components: a CModelCS as the handlers read it ---------------------------------------------------------------------------
// [cs+0xd0] -> u16 table (entry index at 4 * (handle & 0xffff)); [cs+0x100] -> entries of 16 bytes; entry+8 -> the CModel; [model+0xcc8] -> its record id.
struct Component {
  alignas(16) unsigned char cs[0x200];
  alignas(16) unsigned char table[4 * 64];
  alignas(16) unsigned char entries[16 * 4];
  alignas(16) unsigned char model[0xd00];
};
constexpr int kComponents = 40;
Component g_comp[kComponents];
template <typename T>
void Put(unsigned char* base, std::size_t offset, T value) {
  std::memcpy(base + offset, &value, sizeof(value));
}
// Component i: handle (i + 1) << 16 | i, entry 1 of its table, a model whose record id is the sphere's or another's.
std::uint64_t Make(int i, bool sphere) {
  Component& c = g_comp[i];
  std::memset(&c, 0, sizeof(c));
  Put<std::uintptr_t>(c.cs, 0xd0, reinterpret_cast<std::uintptr_t>(c.table));
  Put<std::uintptr_t>(c.cs, 0x100, reinterpret_cast<std::uintptr_t>(c.entries));
  const std::uint64_t handle = (static_cast<std::uint64_t>(i + 1) << 16) | static_cast<std::uint64_t>(i);
  Put<std::uint16_t>(c.table, 4 * static_cast<std::size_t>(i), 1);
  Put<std::uintptr_t>(c.entries, 16 * 1 + 8, reinterpret_cast<std::uintptr_t>(c.model));
  Put<std::uint64_t>(c.model, 0xcc8, sphere ? kSphere : 0x1234567890abcdefULL);
  return handle;
}
void* Cs(int i) { return g_comp[i].cs; }

// ---- the game's three functions: every call -----------------------------------------------------------------------------------------
enum Which { kEnabled, kDisabled, kEvent };
struct Call {
  Which which;
  void* a0;
  std::uint64_t a1, a2, a3;
  std::uint32_t a4;
  bool lockHeld;
};
std::vector<Call> g_calls;
bool g_record = true;  // off while the cost is measured and under threads: the model then does no more than a call
std::uint64_t FakeEnabled(void* cs, std::uint64_t h) {
  if (g_record) g_calls.push_back({kEnabled, cs, h, 0, 0, 0, sw::LockHeldForTest()});
  return 0x1111111111111111ULL;
}
std::uint64_t FakeDisabled(void* cs, std::uint64_t h) {
  if (g_record) g_calls.push_back({kDisabled, cs, h, 0, 0, 0, sw::LockHeldForTest()});
  return 0x2222222222222222ULL;
}
// The game's scripts run inside the event: a TriggeredPing enables the sphere of component `g_enableInEvent` (through the word, as the script does) before it returns.
int g_enableInEvent = -1;
std::uint64_t g_enableInEventHandle = 0;
using ModelFn = std::uint64_t (*)(void*, std::uint64_t);
ModelFn EnabledWord();
void* Cs(int i);
std::uint64_t FakeEvent(void* gs, std::uint64_t sym, std::uint64_t a, std::uint64_t b, std::uint32_t flags) {
  if (g_record) g_calls.push_back({kEvent, gs, sym, a, b, flags, sw::LockHeldForTest()});
  if (g_enableInEvent >= 0 && sym == kTriggered) {
    const int i = g_enableInEvent;
    g_enableInEvent = -1;
    EnabledWord()(Cs(i), g_enableInEventHandle);
  }
  return 0x3333333333333333ULL;
}
int Count(Which which) {
  int n = 0;
  for (const Call& c : g_calls) n += c.which == which ? 1 : 0;
  return n;
}

// ---- the game's levels --------------------------------------------------------------------------------------------------------------------
constexpr std::size_t kTable0 = 0x1f48, kCount0 = 0x1f78, kGamespaceOwner = 0x8, kGamespaceLevel = 0x58;
struct Gamespace {
  std::uintptr_t words[0x60 / 8 + 1] = {};
};
struct Game {
  std::uintptr_t words[0x2000 / 8] = {};
  std::uintptr_t table0[8][2] = {};
  Gamespace spaces[8];
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

// ---- the image, the clock, the lines ---------------------------------------------------------------------------------------------------------
Image g_image;
tgt::Pins g_pins = tgt::kPins;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
std::int64_t g_nowMs = 1000;
std::int64_t Clock() noexcept { return g_nowMs; }
int g_lockHeldStages = 0;
void StageHook(int stage) noexcept {
  if (stage >= 11 && stage <= 16 && !sw::LockHeldForTest()) ++g_lockHeldStages;  // counts the stages at which the lock was NOT held (it must be held)
}

struct Line {
  sentinel::LogLevel level;
  std::string text;
  bool lockHeld;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line, sw::LockHeldForTest()}); }
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

void Build() {
  g_pins = tgt::kPins;
  g_image.Build({{tgt::kEnabledSymbol, g_pins.enabledSlot, false, false, g_pins.enabledFn, true},
                 {tgt::kDisabledSymbol, g_pins.disabledSlot, false, false, g_pins.disabledFn, true},
                 {tgt::kEventSymbol, g_pins.eventSlot, false, false, g_pins.eventFn, false}});
  const std::uintptr_t base = g_image.Base();
  g_pins.enabledFn = reinterpret_cast<std::uintptr_t>(&FakeEnabled) - base;
  g_pins.disabledFn = reinterpret_cast<std::uintptr_t>(&FakeDisabled) - base;
  g_pins.eventFn = reinterpret_cast<std::uintptr_t>(&FakeEvent) - base;
  for (const tgt::Word& w : tgt::kWords) g_image.Put<std::uint32_t>(w.vaddr, w.word);
  *g_image.Slot(g_pins.enabledSlot) = reinterpret_cast<void*>(&FakeEnabled);
  *g_image.Slot(g_pins.disabledSlot) = reinterpret_cast<void*>(&FakeDisabled);
  *g_image.Slot(g_pins.eventSlot) = reinterpret_cast<void*>(&FakeEvent);
}
using EventFn = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint32_t);
ModelFn EnabledWord() { return reinterpret_cast<ModelFn>(*g_image.Slot(g_pins.enabledSlot)); }
ModelFn DisabledWord() { return reinterpret_cast<ModelFn>(*g_image.Slot(g_pins.disabledSlot)); }
EventFn EventWord() { return reinterpret_cast<EventFn>(*g_image.Slot(g_pins.eventSlot)); }
bool AllStock() {
  return EnabledWord() == &FakeEnabled && DisabledWord() == &FakeDisabled && EventWord() == &FakeEvent;
}

// The game-thread tick on an image of its own (the tick's slot row), so the combat level answer can change as the shipped table leaves it before this
// module's row: the tick installed and the scope row's refresh registered (one tick callback).
Image g_tickImage;
bool TickImageLookup(const char* module, sentinel::ElfImage* out) {
  if (!g_tickImage.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_tickImage.Elf();
  return true;
}
void StartTheScope() {
  g_tickImage.Build({world::TickRow()});
  world::PutTickSlot(g_tickImage);
  QCHECK(world::StartScope(&TickImageLookup) && tick::Count() == 1U);
}

void Begin() {
  g_lines.clear();
  g_calls.clear();
  g_nowMs = 1000;
  g_lockHeldStages = 0;
  sw::ResetForTest();
  tick::ResetForTest();
  scope::ResetForTest();
  sw::SetClockForTest(&Clock);
  scope::SetClockForTest(&Clock);  // the combat level answer reads the same clock
  sw::SetLegacyDirForTest(nullptr);
  LoadLevels({});
  sentinel::SetLogSink(&Capture);
  StartTheScope();
}
void End() {
  sw::ResetForTest();
  tick::ResetForTest();
  scope::ResetForTest();
  sw::SetLegacyDirForTest(nullptr);
  g_image.Drop();
  g_tickImage.Drop();
  sentinel::SetLogSink(nullptr);
}
nevr_quest::combat::ModuleResult InstallOn() { return sw::InstallAt(true, &Lookup, g_pins); }
// The frame tick: the combat tick runs the scope row's refresh (registered first) and then the module's callback, once per frame.
void Frame() {
  scope::Refresh(GameAddress());
  sw::Tick(GameAddress());
}
// A combat match: the combat map is loaded, one frame has looked at it.
void Combat() {
  LoadLevels({"mpl_combat_dyson"});
  Frame();
}
// Time passes the way it does in the game: a frame every 200 ms here (the tick looks every 250 ms and the handlers believe a look for a second).
void Advance(std::int64_t ms) {
  for (std::int64_t t = 0; t < ms; t += 200) {
    g_nowMs += 200;
    Frame();
  }
}
void Ping(std::uint64_t a, std::uint64_t b) { EventWord()(nullptr, kTriggered, a, b, 0); }

// ---- the tests ---------------------------------------------------------------------------------------------------------------------------

void TheConstantsArePinned() {
  QCHECK(kTriggered == CSymbol64Hash("delegate_TriggeredPing") && kFinished == CSymbol64Hash("delegate_FinishedPing"));
  QCHECK(tgt::kPins.enabledSlot == 0x36495c8 && tgt::kPins.enabledFn == 0x15b127c && tgt::kPins.disabledSlot == 0x36495d0 && tgt::kPins.disabledFn == 0x15b12a8);
  QCHECK(tgt::kPins.eventSlot == 0x36fcae8 && tgt::kPins.eventFn == 0x180735c);
  QCHECK(tgt::kPins.disabledSlot == tgt::kPins.enabledSlot + 8);  // the next word of the same vtable
  QCHECK(sw::kWatchdogMs == 6000 && sw::kSlots == 32U && sw::kPings == 8U);
  QCHECK(std::strcmp(tgt::kEnabledSymbol, "_ZN10NRadEngine8CModelCS18OnComponentEnabledENS_18SCompactPoolHandleE") == 0);
  QCHECK(std::strcmp(tgt::kDisabledSymbol, "_ZN10NRadEngine8CModelCS19OnComponentDisabledENS_18SCompactPoolHandleE") == 0);
  QCHECK(std::strcmp(tgt::kEventSymbol, "_ZN10NRadEngine13CncaGameSpace24SendComponentEventGlobalENS_9CSymbol64ENS_6SActorES1_j") == 0);
  QCHECK(tgt::kWordCount == 34);
  QCHECK(tgt::kWords[0].vaddr == 0x15b127c && tgt::kWords[10].vaddr == 0x15b12a4 && tgt::kWords[10].word == 0xd61f0040);  // the tail call `br x2`
  QCHECK(tgt::kWords[11].vaddr == 0x15b12a8 && tgt::kWords[21].vaddr == 0x15b12d0 && tgt::kWords[21].word == 0xd61f0040);
  // the 34 words, restated from the disassembly (OnComponentEnabled, OnComponentDisabled, then the first 12 of SendComponentEventGlobal)
  static const struct {
    std::uint64_t va;
    std::uint32_t word;
  } kWant[34] = {
      {0x15b127c, 0xf9406808},
      {0x15b1280, 0x92403c29},
      {0x15b1284, 0xd37ef529},
      {0x15b1288, 0x78696908},
      {0x15b128c, 0xf9408009},
      {0x15b1290, 0x8b081128},
      {0x15b1294, 0xf9400500},
      {0x15b1298, 0xb9400501},
      {0x15b129c, 0xf9400009},
      {0x15b12a0, 0xf9403122},
      {0x15b12a4, 0xd61f0040},
      {0x15b12a8, 0xf9406808},
      {0x15b12ac, 0x92403c29},
      {0x15b12b0, 0xd37ef529},
      {0x15b12b4, 0x2a1f03e1},
      {0x15b12b8, 0x78696908},
      {0x15b12bc, 0xf9408009},
      {0x15b12c0, 0x8b081128},
      {0x15b12c4, 0xf9400500},
      {0x15b12c8, 0xf9400008},
      {0x15b12cc, 0xf9403102},
      {0x15b12d0, 0xd61f0040},
      {0x180735c, 0xd10103ff},
      {0x1807360, 0x92800008},
      {0x1807364, 0xa902fbe8},
      {0x1807368, 0xf9402c08},
      {0x180736c, 0xa9018fe2},
      {0x1807370, 0x2a0403e2},
      {0x1807374, 0xa900a3e1},
      {0x1807378, 0x910023e1},
      {0x180737c, 0x97dbd111},
      {0x1807380, 0xf9401bfe},
      {0x1807384, 0x910103ff},
      {0x1807388, 0xd65f03c0},
  };
  for (std::size_t i = 0; i < 34; ++i) QCHECK(tgt::kWords[i].vaddr == kWant[i].va && tgt::kWords[i].word == kWant[i].word);
  QCHECK(tgt::kWords[22].vaddr == 0x180735c && tgt::kWords[22].word == 0xd10103ff && tgt::kWords[33].vaddr == 0x1807388 && tgt::kWords[33].word == 0xd65f03c0);
}

void TheInstallTakesTheThreeWordsAndWritesOneLine() {
  Begin();
  Build();
  const nevr_quest::combat::ModuleResult r = InstallOn();
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
  QCHECK(EnabledWord() != &FakeEnabled && DisabledWord() != &FakeDisabled && EventWord() != &FakeEvent);
  QCHECK(tick::Count() == 2U && sw::CurrentStats().on);  // the scope's refresh, then the watchdog's tick
  const std::vector<Line> lines = Lines("combat_sensorwd_install");
  QCHECK(lines.size() == 1U);
  if (lines.size() == 1U) {
    QCHECK(lines[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(lines[0].text) ==
           "\"event\":\"combat_sensorwd_install\",\"state\":\"on\",\"reason\":\"default\",\"switch\":\"combat.sensorwd\",\"scope\":\"combat_map\","
           "\"enabled\":\"ok\",\"disabled\":\"ok\",\"event_slot\":\"ok\",\"watchdog_ms\":6000,\"msg\":\"a scanner sphere still enabled 6000 ms after its enable is "
           "turned off on the game thread, in combat maps only; an Echo Arena level passes through\"}");
  }
  End();
}

// Every argument and the result come through whole, in a combat map and out of one.
void EveryHandlerPassesEveryRegisterAndTheResultOn() {
  for (const bool combat : {true, false}) {
    Begin();
    Build();
    QCHECK(InstallOn().installed);
    if (combat) {
      Combat();
    } else {
      LoadLevels({"mpl_arena_a"});
      Frame();
    }
    g_calls.clear();
    QCHECK(EnabledWord()(Cs(1), 0x1234567890ULL) == 0x1111111111111111ULL);
    QCHECK(DisabledWord()(Cs(2), 0xabcdef0123ULL) == 0x2222222222222222ULL);
    QCHECK(EventWord()(Cs(3), 0x77, 0xaaaa, 0xbbbb, 0x99) == 0x3333333333333333ULL);
    QCHECK(g_calls.size() == 3U);
    if (g_calls.size() == 3U) {
      QCHECK(g_calls[0].which == kEnabled && g_calls[0].a0 == Cs(1) && g_calls[0].a1 == 0x1234567890ULL);
      QCHECK(g_calls[1].which == kDisabled && g_calls[1].a0 == Cs(2) && g_calls[1].a1 == 0xabcdef0123ULL);
      QCHECK(g_calls[2].which == kEvent && g_calls[2].a0 == Cs(3) && g_calls[2].a1 == 0x77 && g_calls[2].a2 == 0xaaaa && g_calls[2].a3 == 0xbbbb && g_calls[2].a4 == 0x99);
    }
    End();
  }
}

// A sphere's enable is timed; the tick turns it off after 6000 ms through the game's own function; a model that is not the sphere is never counted.
void AStuckSphereIsTurnedOffAfterSixSeconds() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  sw::SetHookForTest(&StageHook);
  Combat();
  const std::uint64_t h0 = Make(0, true), h1 = Make(1, false);
  g_nowMs += 100;
  EnabledWord()(Cs(0), h0);
  EnabledWord()(Cs(1), h1);  // another model: passes through, not counted
  sw::Stats s = sw::CurrentStats();
  QCHECK(s.enables == 1 && s.tracked == 1 && s.enabled == 1);
  g_calls.clear();
  g_nowMs += 5000;
  Frame();  // 5000 ms after the enable: not stuck yet
  QCHECK(g_calls.empty() && sw::CurrentStats().wdOff == 0);
  g_nowMs += 1000;
  Frame();  // 6000 ms after
  QCHECK(g_calls.size() == 1U && g_calls[0].which == kDisabled && g_calls[0].a0 == Cs(0) && g_calls[0].a1 == h0);
  QCHECK(g_calls.size() == 1U && !g_calls[0].lockHeld);  // the game's function runs with the state lock free
  s = sw::CurrentStats();
  QCHECK(s.wdOff == 1 && s.forced == 1 && s.enabled == 0 && s.disables == 0);  // the handlers did not see the tick's own call
  const std::vector<Line> off = Lines("combat_sensorwd_off");
  QCHECK(off.size() == 1U && Has(off[0].text, "\"age_ms\":6000") && Has(off[0].text, "\"n\":1"));
  QCHECK(g_lockHeldStages == 0);  // the lock was held at every stage that takes it
  for (const Line& l : g_lines) QCHECK(!l.lockHeld);  // no line is written under the lock
  g_calls.clear();
  g_nowMs += 6000;
  Frame();  // once only
  QCHECK(g_calls.empty() && sw::CurrentStats().wdOff == 1);
  End();
}

// A sphere that was disabled in time is not stuck; its next enable restarts the clock.
void ASphereDisabledInTimeIsLeftAlone() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  Combat();
  const std::uint64_t h = Make(0, true);
  EnabledWord()(Cs(0), h);
  Advance(4000);
  DisabledWord()(Cs(0), h);
  g_calls.clear();
  g_nowMs += 5000;
  Frame();
  QCHECK(g_calls.empty() && sw::CurrentStats().wdOff == 0 && sw::CurrentStats().disables == 1);
  EnabledWord()(Cs(0), h);
  g_calls.clear();
  g_nowMs += 5900;
  Frame();
  QCHECK(g_calls.empty());
  g_nowMs += 100;
  Frame();
  QCHECK(g_calls.size() == 1U && g_calls[0].which == kDisabled);
  End();
}

// A forced sphere comes back on at the next TriggeredPing of the actor pair it was paired with, and only that one.
void AForcedSphereComesBackAtItsActorsNextPing() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  Combat();
  const std::uint64_t h = Make(0, true);
  Ping(0x11, 0x22);
  g_nowMs += 100;  // within the pairing window of the ping
  EnabledWord()(Cs(0), h);
  g_nowMs += 6000;
  Frame();  // forced off
  QCHECK(sw::CurrentStats().forced == 1);
  g_calls.clear();
  g_nowMs += 100;
  Ping(0x11, 0x44);  // another actor that shares the first word
  Ping(0x33, 0x22);  // and one that shares the second
  Ping(0x33, 0x44);
  Frame();
  QCHECK(Count(kEnabled) == 0 && Count(kEvent) == 3);  // only the pings themselves
  QCHECK(sw::CurrentStats().forced == 1 && sw::CurrentStats().wdBack == 0);
  g_calls.clear();
  g_nowMs += 100;
  Ping(0x11, 0x22);  // its own actor
  Frame();
  QCHECK(g_calls.size() == 2U && g_calls[1].which == kEnabled && g_calls[1].a0 == Cs(0) && g_calls[1].a1 == h);
  QCHECK(sw::CurrentStats().forced == 0 && sw::CurrentStats().enabled == 1 && sw::CurrentStats().wdBack == 1);
  QCHECK(Lines("combat_sensorwd_back_on").size() == 1U);
  End();
}

// A sphere that already has its actor pair keeps it: a later enable within the pairing window of another actor's ping does not re-pair it.
void ASpheresActorPairIsKeptWhenItIsEnabledAgain() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  Combat();
  const std::uint64_t h = Make(0, true);
  Ping(0x11, 0x22);
  g_nowMs += 100;
  EnabledWord()(Cs(0), h);  // paired with (0x11, 0x22)
  DisabledWord()(Cs(0), h);
  Ping(0x55, 0x66);
  g_nowMs += 100;
  EnabledWord()(Cs(0), h);  // within the window of the other actor's ping: still (0x11, 0x22)
  g_nowMs += 6000;
  Frame();  // forced off
  QCHECK(sw::CurrentStats().forced == 1);
  g_calls.clear();
  Ping(0x55, 0x66);
  Frame();
  QCHECK(Count(kEnabled) == 0 && sw::CurrentStats().forced == 1);
  Ping(0x11, 0x22);
  Frame();
  QCHECK(Count(kEnabled) == 1 && sw::CurrentStats().forced == 0);
  End();
}

// The engine enabling a forced sphere again (the script's own EnableModel) ends the force; and a sphere turned back on is timed afresh, not turned off again at the next frame.
void AForcedSphereTheEngineEnablesIsNoLongerForcedAndABackOnRestartsTheClock() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  Combat();
  const std::uint64_t h = Make(0, true);
  Ping(0x11, 0x22);
  g_nowMs += 100;
  EnabledWord()(Cs(0), h);
  g_nowMs += 6000;
  Frame();
  QCHECK(sw::CurrentStats().forced == 1);
  EnabledWord()(Cs(0), h);  // the engine's own enable
  QCHECK(sw::CurrentStats().forced == 0 && sw::CurrentStats().enabled == 1);
  g_nowMs += 6000;
  Frame();  // forced again
  Ping(0x11, 0x22);
  g_nowMs += 100;
  Frame();  // back on at the ping
  QCHECK(sw::CurrentStats().wdBack == 1 && sw::CurrentStats().enabled == 1);
  g_calls.clear();
  g_nowMs += 200;
  Frame();  // 300 ms after the back-on: not stuck
  QCHECK(g_calls.empty() && sw::CurrentStats().wdOff == 2);
  End();
}

// The ping is recorded BEFORE the game runs: the script enables the sphere inside the event, and that enable pairs with the ping that is running.
void ThePingIsRecordedBeforeTheGamesScriptsRun() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  Combat();
  const std::uint64_t h = Make(0, true);
  g_enableInEvent = 0;
  g_enableInEventHandle = h;
  g_nowMs += 50;
  Ping(0x11, 0x22);  // the enable happens inside this call
  QCHECK(g_enableInEvent == -1 && sw::CurrentStats().enables == 1);
  g_nowMs += 6000;
  Frame();  // forced off
  QCHECK(sw::CurrentStats().forced == 1);
  g_calls.clear();
  g_nowMs += 100;
  Ping(0x11, 0x22);
  Frame();
  QCHECK(Count(kEnabled) == 1 && sw::CurrentStats().wdBack == 1);  // paired with the actor of the ping that ran the enable
  End();
}

// A sphere enabled more than 250 ms after the last ping is paired with no actor; a (0, 0) ping is counted but queues nothing; a sphere with no pair is not
// turned back on by a ping.
void PairingNeedsAPingJustBeforeTheEnable() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  Combat();
  const std::uint64_t h = Make(0, true);
  Ping(0x11, 0x22);
  g_nowMs += 300;  // too late to pair
  EnabledWord()(Cs(0), h);
  g_nowMs += 6000;
  Frame();
  QCHECK(sw::CurrentStats().forced == 1);
  g_calls.clear();
  Ping(0x11, 0x22);
  Ping(0, 0);
  Frame();
  QCHECK(Count(kEnabled) == 0 && sw::CurrentStats().forced == 1);
  QCHECK(sw::CurrentStats().pings == 3);
  End();
}

// FinishedPing is counted; every other event is two compares; the table and the ping queue have limits that are counted and said once.
void TheLimitsAreCountedAndSaid() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  Combat();
  EventWord()(nullptr, kFinished, 1, 2, 0);
  EventWord()(nullptr, 0x1234, 1, 2, 0);
  QCHECK(sw::CurrentStats().finished == 1 && sw::CurrentStats().pings == 0);
  for (unsigned i = 0; i < sw::kPings + 3; ++i) Ping(100 + i, 200 + i);
  QCHECK(sw::CurrentStats().pings == sw::kPings + 3 && sw::CurrentStats().pingsLost == 3);
  const std::vector<Line> notes = Lines("combat_sensorwd_note");
  QCHECK(notes.size() == 3U && notes[0].level == sentinel::LogLevel::kWarn && Has(notes[0].text, "\"what\":\"ping_lost\""));
  Frame();  // the tick empties the queue
  g_lines.clear();
  for (int i = 0; i < 33; ++i) EnabledWord()(Cs(i), Make(i, true));
  const sw::Stats s = sw::CurrentStats();
  QCHECK(s.tracked == sw::kSlots && s.tableFull == 1 && s.enables == 33);
  const std::vector<Line> full = Lines("combat_sensorwd_note");
  QCHECK(full.size() == 1U && Has(full[0].text, "\"what\":\"table_full\"") && full[0].level == sentinel::LogLevel::kWarn);
  End();
}

// The sweep drops a tracked sphere whose component no longer reads as the sphere, and the tick drops one that is due when it no longer does.
void ADeadSphereIsDropped() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  Combat();
  const std::uint64_t h0 = Make(0, true), h1 = Make(1, true);
  EnabledWord()(Cs(0), h0);
  EnabledWord()(Cs(1), h1);
  Put<std::uint64_t>(g_comp[0].model, 0xcc8, 0xdeadULL);  // recycled
  g_nowMs += 5100;
  Frame();  // the sweep re-reads both
  QCHECK(sw::CurrentStats().tracked == 1 && sw::CurrentStats().stale == 1);
  QCHECK(Lines("combat_sensorwd_note").size() == 1U && Has(Lines("combat_sensorwd_note")[0].text, "\"what\":\"swept\""));
  Put<std::uint64_t>(g_comp[1].model, 0xcc8, 0xbeefULL);  // recycled before it falls due
  g_calls.clear();
  g_nowMs += 900;
  Frame();
  QCHECK(g_calls.empty() && sw::CurrentStats().tracked == 0 && sw::CurrentStats().wdOff == 0 && sw::CurrentStats().stale == 2);
  End();
}

// ARENA: the hooks sit on generic words that every level uses, so the module is a pass-through in every state but a combat map with no Echo Arena level.
// Nothing is tracked, no state is touched, no line is written, and a table filled in a combat match is emptied when the match ends.
void OutsideACombatMapEveryHandlerIsAPassThrough() {
  struct Case {
    const char* what;
    std::initializer_list<const char*> levels;
  };
  const Case cases[] = {{"an arena level", {"mpl_arena_a"}},
                        {"the low-spec arena level", {"mpl_arena_a_lowspec"}},
                        {"the tutorial", {"mpl_tutorial_a"}},
                        {"an arena lobby", {"mpl_lobby_b"}},
                        {"the combat lobby", {"mpl_lobby_b_combat"}},
                        {"no level", {}},
                        {"a combat map beside an arena level", {"mpl_combat_dyson", "mpl_arena_a"}}};
  for (const Case& c : cases) {
    Begin();
    Build();
    QCHECK(InstallOn().installed);
    sw::SetHookForTest(&StageHook);
    LoadLevels(c.levels);
    g_lines.clear();
    Frame();
    // the tick's first frame outside the scope does nothing either: it writes no line of the watchdog's (not even the tick's thread line)
    for (const Line& l : g_lines) {
      if (Has(l.text, "\"event\":\"combat_sensorwd_")) std::fprintf(stderr, "  %s: the tick wrote %s\n", c.what, l.text.c_str());
      QCHECK(!Has(l.text, "\"event\":\"combat_sensorwd_"));
    }
    g_lines.clear();
    g_calls.clear();
    const std::uint64_t h = Make(0, true);
    std::uint64_t reads = 0, scans = 0;  // what the three handlers asked of the game and of the level, over the whole stream (a frame's own look is not counted)
    for (int i = 0; i < 20; ++i) {
      const std::uint64_t reads0 = SafeReadCalls(), scans0 = nevr_quest::combat::level::ScanCalls();
      Ping(0x11, 0x22);
      EnabledWord()(Cs(0), h);
      DisabledWord()(Cs(0), h);
      reads += SafeReadCalls() - reads0;
      scans += nevr_quest::combat::level::ScanCalls() - scans0;
      g_nowMs += 700;
      Frame();
    }
    // ARENA PAYS NOTHING, BY COUNT: no read of game memory (the sphere check is five reads) and no level scan on the path of any of the three words.
    if (reads != 0 || scans != 0) std::fprintf(stderr, "  %s: the handlers made %llu read(s) and %llu scan(s)\n", c.what, static_cast<unsigned long long>(reads), static_cast<unsigned long long>(scans));
    QCHECK(reads == 0 && scans == 0);
    const sw::Stats s = sw::CurrentStats();
    if (s.enables != 0 || s.disables != 0 || s.pings != 0 || s.tracked != 0 || s.wdOff != 0 || !g_lines.empty()) std::fprintf(stderr, "  not a pass-through on %s\n", c.what);
    QCHECK(s.enables == 0 && s.disables == 0 && s.pings == 0 && s.finished == 0 && s.tracked == 0 && s.wdOff == 0 && !s.inScope);
    QCHECK(g_lines.empty());
    QCHECK(Count(kEvent) == 20 && Count(kEnabled) == 20 && Count(kDisabled) == 20 && g_calls.size() == 60U);  // the game's calls, and no other
    End();
  }
}

// The count is meaningful: the same three calls in a combat map do read (the sphere check), so zero outside it is the scope's doing.
void InACombatMapTheSphereCheckReads() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  Combat();
  const std::uint64_t h = Make(0, true);
  const std::uint64_t reads0 = SafeReadCalls();
  EnabledWord()(Cs(0), h);
  QCHECK(SafeReadCalls() - reads0 >= 5);
  End();
}

// No frame yet, or a tick that has stopped: the answer is not believed (it fails closed).
void AStaleOrMissingAnswerPassesEverythingThrough() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  LoadLevels({"mpl_combat_dyson"});  // loaded, but no frame has looked yet
  const std::uint64_t h = Make(0, true);
  Ping(1, 2);
  EnabledWord()(Cs(0), h);
  QCHECK(sw::CurrentStats().pings == 0 && sw::CurrentStats().enables == 0);
  Frame();
  EnabledWord()(Cs(0), h);
  QCHECK(sw::CurrentStats().enables == 1);
  g_nowMs += scope::kStaleAfterMs;  // the look is still believed (combat_scope.h: at most kStaleAfterMs old)
  EnabledWord()(Cs(0), h);
  QCHECK(sw::CurrentStats().enables == 2);
  g_nowMs += 1;  // the tick has not looked for more than a second: not believed
  EnabledWord()(Cs(0), h);
  Ping(1, 2);
  QCHECK(sw::CurrentStats().enables == 2 && sw::CurrentStats().pings == 0);
  Frame();  // the tick is back, and looks again
  EnabledWord()(Cs(0), h);
  QCHECK(sw::CurrentStats().enables == 3);
  End();
}

// The match ends: the scope goes false at the next look, the table is emptied, one line says so, and the next match starts clean.
void LeavingTheScopeEmptiesTheTable() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  Combat();
  QCHECK(Lines("combat_sensorwd_scope").size() == 1U && Has(Lines("combat_sensorwd_scope")[0].text, "\"from\":0,\"to\":1"));
  for (int i = 0; i < 5; ++i) EnabledWord()(Cs(i), Make(i, true));
  Ping(1, 2);
  QCHECK(sw::CurrentStats().tracked == 5);
  LoadLevels({"mpl_lobby_b"});
  g_nowMs += 300;
  Frame();
  QCHECK(sw::CurrentStats().tracked == 0 && !sw::CurrentStats().inScope);
  QCHECK(Lines("combat_sensorwd_scope").size() == 2U && Has(Lines("combat_sensorwd_scope")[1].text, "\"from\":1,\"to\":0"));
  if (Lines("combat_sensorwd_scope").size() == 2U) {
    QCHECK(Golden(Lines("combat_sensorwd_scope")[0].text) ==
           "\"event\":\"combat_sensorwd_scope\",\"from\":0,\"to\":1,\"msg\":\"a combat map is loaded: the watchdog is watching the scanner spheres\"}");
    QCHECK(Golden(Lines("combat_sensorwd_scope")[1].text) ==
           "\"event\":\"combat_sensorwd_scope\",\"from\":1,\"to\":0,\"msg\":\"no combat map (or an Echo Arena level, or the combat level answer is stale): "
           "every handler passes through and the table is emptied\"}");
    QCHECK(Lines("combat_sensorwd_scope")[0].level == sentinel::LogLevel::kInfo);
  }
  g_calls.clear();
  Combat();
  g_nowMs += 300;
  Frame();
  g_nowMs += 6000;
  Frame();
  QCHECK(Count(kDisabled) == 0);  // nothing from the last match is due
  End();
}

// The tick reads the same answer the handlers do, freshness included: a combat map seen once and then never looked at again (the scope row's refresh does
// not run, while this module's callback still does) is not believed after a second, so the tick leaves the scope, empties the table and turns nothing off.
void TheTickDoesNotBelieveAStaleAnswer() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  Combat();  // one look: a combat map
  const std::uint64_t h = Make(0, true);
  EnabledWord()(Cs(0), h);
  QCHECK(sw::CurrentStats().tracked == 1 && sw::CurrentStats().inScope);
  g_calls.clear();
  for (int i = 0; i < 40; ++i) {  // 8 s of the module's callback alone
    g_nowMs += 200;
    sw::Tick(GameAddress());
  }
  QCHECK(Count(kDisabled) == 0 && sw::CurrentStats().wdOff == 0);
  QCHECK(sw::CurrentStats().tracked == 0 && !sw::CurrentStats().inScope);
  QCHECK(Lines("combat_sensorwd_scope").size() == 2U && Has(Lines("combat_sensorwd_scope").back().text, "\"from\":1,\"to\":0"));
  End();
}

// The combat level answer cannot change (the tick is not installed, or the scope row did not register its refresh): the handlers would never act, so the
// module is not installed, says why at Error, takes no word and registers nothing. Off by the key wins: nothing is asked of the scope.
void AScopeThatCannotChangeRefusesTheInstall() {
  Begin();
  Build();
  tick::ResetForTest();
  scope::ResetForTest();
  scope::SetClockForTest(&Clock);
  nevr_quest::combat::ModuleResult r = InstallOn();
  QCHECK(!r.installed && !r.off && std::strcmp(r.status, "scope_not_live") == 0 && AllStock() && tick::Count() == 0U && !sw::CurrentStats().on);
  const std::vector<Line> lines = Lines("combat_sensorwd_install");
  QCHECK(lines.size() == 1U);
  if (lines.size() == 1U) {
    QCHECK(lines[0].level == sentinel::LogLevel::kError);
    QCHECK(Golden(lines[0].text) ==
           "\"event\":\"combat_sensorwd_install\",\"state\":\"refused\",\"reason\":\"scope_not_live\",\"switch\":\"combat.sensorwd\",\"scope\":\"combat_map\","
           "\"enabled\":\"none\",\"disabled\":\"none\",\"event_slot\":\"none\",\"watchdog_ms\":6000,\"msg\":\"a scanner sphere stuck on is not turned off\"}");
  }
  // no libr15 either: the scope is asked first, so the reason is the scope's
  r = sw::InstallAt(true, [](const char*, sentinel::ElfImage*) { return false; }, g_pins);
  QCHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0);
  // the tick is installed but no row registered the refresh
  g_lines.clear();
  g_tickImage.Build({world::TickRow()});
  world::PutTickSlot(g_tickImage);
  QCHECK(tick::InstallAt(&TickImageLookup));
  r = InstallOn();
  QCHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0 && !scope::Live() && AllStock() && tick::Count() == 0U);
  // off by the key: off_config, whatever the scope
  r = sw::InstallAt(false, &Lookup, g_pins);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0 && AllStock());
  End();
}

// All three words or none: a word that does not hold the game's function refuses the module and puts back what was done; a pin that differs, another build and a
// full tick table change nothing.
void ARefusedInstallChangesNothing() {
  for (int which = 0; which < 3; ++which) {  // the word that is not the game's function: enabled, disabled, event
    Begin();
    Build();
    void* const foreign = reinterpret_cast<void*>(&End);
    const std::uint64_t slot = which == 0 ? g_pins.enabledSlot : which == 1 ? g_pins.disabledSlot : g_pins.eventSlot;
    *g_image.Slot(slot) = foreign;
    const auto r = InstallOn();
    QCHECK(!r.installed && std::strcmp(r.status, "hook") == 0);
    QCHECK(*g_image.Slot(slot) == foreign);
    if (which != 0) QCHECK(EnabledWord() == &FakeEnabled);
    if (which != 1) QCHECK(DisabledWord() == &FakeDisabled);
    if (which != 2) QCHECK(EventWord() == &FakeEvent);
    QCHECK(tick::Count() == 1U && !sw::CurrentStats().on);  // the scope's refresh alone
    QCHECK(Lines("combat_sensorwd_install")[0].level == sentinel::LogLevel::kError);
    End();
  }
  for (std::size_t i = 0; i < tgt::kWordCount; ++i) {
    Begin();
    Build();
    g_image.Put<std::uint32_t>(tgt::kWords[i].vaddr, tgt::kWords[i].word ^ 1U);
    const auto r = InstallOn();
    QCHECK(!r.installed && std::strcmp(r.status, "pin_mismatch") == 0 && AllStock() && tick::Count() == 1U);
    End();
  }
  {
    Begin();
    const auto r = sw::InstallAt(true, [](const char*, sentinel::ElfImage*) { return false; }, g_pins);
    QCHECK(!r.installed && !r.off && std::strcmp(r.status, "module_not_loaded") == 0);  // a refusal is a failure, never off
    End();
  }
  {
    Begin();
    g_pins = tgt::kPins;
    g_image.Build({{tgt::kEnabledSymbol, g_pins.enabledSlot, false, false, g_pins.enabledFn, true}}, "0123456789abcdef0123456789abcdef01234567");
    QCHECK(std::strcmp(InstallOn().status, "build_id_mismatch") == 0);
    End();
  }
  {  // the combat tick has no room: all three words are put back
    Begin();
    Build();
    while (tick::Count() < tick::kMaxCallbacks) QCHECK(tick::Add([](std::uintptr_t) noexcept {}));
    const auto r = InstallOn();
    QCHECK(!r.installed && std::strcmp(r.status, "tick_table_full") == 0 && AllStock() && !sw::CurrentStats().on);
    End();
  }
}

void TheSwitchAndTheLegacyKillFile() {
  Begin();
  Build();
  const auto off = sw::InstallAt(false, &Lookup, g_pins);
  QCHECK(!off.installed && off.off && std::strcmp(off.status, "off_config") == 0 && AllStock() && tick::Count() == 1U);  // off is its own answer
  const std::vector<Line> lines = Lines("combat_sensorwd_install");
  QCHECK(lines.size() == 1U && lines[0].level == sentinel::LogLevel::kInfo && Has(lines[0].text, "\"state\":\"off\"") && Has(lines[0].text, "\"reason\":\"off_config\""));
  End();
  char dir[] = "/tmp/sensorwd_legacy_XXXXXX";
  if (mkdtemp(dir) == nullptr) {
    QCHECK(false);
    return;
  }
  const std::string file = std::string(dir) + "/sensorwd.off";
  std::FILE* f = std::fopen(file.c_str(), "w");
  QCHECK(f != nullptr);
  if (f != nullptr) std::fclose(f);
  Begin();
  Build();
  sw::SetLegacyDirForTest(dir);
  QCHECK(InstallOn().installed);  // the file changes nothing
  const std::vector<Line> legacy = Lines("combat_sensorwd_legacy_switch");
  QCHECK(legacy.size() == 1U && legacy[0].level == sentinel::LogLevel::kWarn && Has(legacy[0].text, "\"replaced_by\":\"combat.sensorwd: false\""));
  End();
  std::remove(file.c_str());
  rmdir(dir);
}

// The first sphere event of the process is said once per kind; the thread lines name the roles.
void TheFirstSightingsAreSaidOnce() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  Combat();
  const std::uint64_t h = Make(0, true);
  Ping(1, 2);
  Ping(1, 2);
  EnabledWord()(Cs(0), h);
  EnabledWord()(Cs(0), h);
  DisabledWord()(Cs(0), h);
  const std::vector<Line> first = Lines("combat_sensorwd_first");
  QCHECK(first.size() == 3U);
  QCHECK(Has(first[0].text, "\"what\":\"ping\"") && Has(first[1].text, "\"what\":\"sphere_enable\"") && Has(first[2].text, "\"what\":\"sphere_disable\""));
  const std::vector<Line> threads = Lines("combat_sensorwd_thread");
  QCHECK(threads.size() == 4U && Has(threads[0].text, "\"role\":\"tick\""));  // tick, event, enable, disable, each on its first sighting
  End();
}

// The state lock under four threads and a ticker: the counters add up, nothing is lost.
void TheCountersAddUpUnderThreads() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  Combat();
  sentinel::SetLogSink(+[](sentinel::LogLevel, const char*) {});  // a thousand lines of thread chatter are not the point here
  g_record = false;
  const std::uint64_t h = Make(0, true);
  constexpr int kThreads = 4, kPerThread = 2000;
  std::vector<std::thread> workers;
  std::atomic<bool> stop{false};
  std::thread ticker([&] {
    while (!stop.load()) Frame();
  });
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&] {
      for (int i = 0; i < kPerThread; ++i) {
        EnabledWord()(Cs(0), h);
        DisabledWord()(Cs(0), h);
        Ping(1, 2);
      }
    });
  }
  for (std::thread& w : workers) w.join();
  stop.store(true);
  ticker.join();
  const sw::Stats s = sw::CurrentStats();
  QCHECK(s.enables == kThreads * kPerThread && s.disables == kThreads * kPerThread && s.pings == kThreads * kPerThread);
  g_record = true;
  End();
}

// The cost on an arena path, with the module installed on an Echo Arena level: every model enable and every event of the level pays this, against a direct
// call of the same stand-in. Printed, not asserted: a host number, not a Quest one.
void TheCostOnAnArenaPath() {
  Begin();
  Build();
  QCHECK(InstallOn().installed);
  LoadLevels({"mpl_arena_a"});
  Frame();
  sentinel::SetLogSink(nullptr);
  g_record = false;
  constexpr int kCalls = 2000000;
  const std::uint64_t h = Make(0, true);
  const auto best = [&](auto&& call) {
    double best = 1e18;
    for (int rep = 0; rep < 7; ++rep) {
      const auto start = std::chrono::steady_clock::now();
      volatile std::uint64_t sink = 0;
      for (int i = 0; i < kCalls; ++i) sink = sink + call();
      const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kCalls;
      if (ns < best) best = ns;
    }
    return best;
  };
  const ModelFn hooked = EnabledWord();
  const EventFn hookedEvent = EventWord();
  const double modelDirect = best([&] { return FakeEnabled(Cs(0), h); });
  const double modelHooked = best([&] { return hooked(Cs(0), h); });
  const double eventDirect = best([&] { return FakeEvent(nullptr, kTriggered, 1, 2, 0); });
  const double eventHooked = best([&] { return hookedEvent(nullptr, kTriggered, 1, 2, 0); });
  // a frame in arena (the scope row's refresh, then the module's callback), with the clock fixed, with a real clock read added, and a frame whose refresh looks at the levels (the clock moved past kLookEveryMs)
  const double frame = best([&] {
    Frame();
    return std::uint64_t{1};
  });
  const double clockRead = best([&] {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_nsec);
  });
  constexpr int kLooks = 20000;
  double look = 1e18;
  for (int rep = 0; rep < 7; ++rep) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kLooks; ++i) {
      g_nowMs += scope::kLookEveryMs;
      Frame();
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kLooks;
    if (ns < look) look = ns;
  }
  std::printf("sensorwd_test: arena path, host: OnComponentEnabled direct %.1f ns, hooked %.1f ns (+%.1f ns); SendComponentEventGlobal direct %.1f ns, hooked %.1f ns (+%.1f ns); "
              "a frame (the scope's refresh, then the module's callback) %.1f ns between looks (a real clock read %.1f ns more), %.0f ns for a frame whose refresh looks at the levels (one gamespace, every %lld ms)\n",
              modelDirect, modelHooked, modelHooked - modelDirect, eventDirect, eventHooked, eventHooked - eventDirect, frame, clockRead, look,
              static_cast<long long>(scope::kLookEveryMs));
  g_record = true;
  End();
}

}  // namespace

int main() {
  nevr_quest::combat::SetReadCounterForTest(&g_safeReads);
  TheConstantsArePinned();
  TheInstallTakesTheThreeWordsAndWritesOneLine();
  EveryHandlerPassesEveryRegisterAndTheResultOn();
  AStuckSphereIsTurnedOffAfterSixSeconds();
  ASphereDisabledInTimeIsLeftAlone();
  AForcedSphereComesBackAtItsActorsNextPing();
  ASpheresActorPairIsKeptWhenItIsEnabledAgain();
  AForcedSphereTheEngineEnablesIsNoLongerForcedAndABackOnRestartsTheClock();
  ThePingIsRecordedBeforeTheGamesScriptsRun();
  PairingNeedsAPingJustBeforeTheEnable();
  TheLimitsAreCountedAndSaid();
  ADeadSphereIsDropped();
  OutsideACombatMapEveryHandlerIsAPassThrough();
  InACombatMapTheSphereCheckReads();
  AStaleOrMissingAnswerPassesEverythingThrough();
  LeavingTheScopeEmptiesTheTable();
  TheTickDoesNotBelieveAStaleAnswer();
  AScopeThatCannotChangeRefusesTheInstall();
  ARefusedInstallChangesNothing();
  TheSwitchAndTheLegacyKillFile();
  TheFirstSightingsAreSaidOnce();
  TheCountersAddUpUnderThreads();
  TheCostOnAnArenaPath();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "sensorwd_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("sensorwd_test: all checks passed");
  return 0;
}
