// Host test of the headset gates (src/quest/combat/headset_gates.cpp, #2180): the real entries, installed by the real GotHook into the two
// JUMP_SLOTs of a libr15 image built in memory, and called the way the game calls them: through the slot, from functions whose addresses the
// gates' ranges name. A call that returns into a gate's range gets the gate's answer; every other caller gets the real one.
//
// Moved from the q15t plugin's gates_test: the assertions on the answers are the same (a call from the gated range answers "not a Quest", any
// other caller the real answer, the link range its own kind, both edges of the link range, an empty range gates nothing, the library policy, a
// foreign slot word refused and left alone). What changed is the fixture (the plugin's fake slot page is a libr15 image and the manager's
// install, not a slot swap), the permission range and the bot range being two ranges (the plugin test used one section for both), and the bot
// gate answering only while a combat map is loaded (the game's levels are modelled below). New here: an entry installed above a handler that
// was on the slot first keeps answering, a handler that asks to join above an entry is refused (#2219), the switches and their reasons, a pinned
// word that differs refuses its gate alone, and the legacy switch files.
//
// On the host the entries are C++ functions that read their own return address (the arm64 entries are the top-level asm() blocks the frame
// sensor checks: src/quest/combat/check/headset_gates.sh runs those under qemu).
//
// Built and run by `just test-quest-combat`.
#include "quest/combat/headset_gates.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/legacy_switch.h"
#include "quest/sentinel/hook_install.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

extern "C" char __start_gate_perm[], __stop_gate_perm[];
extern "C" char __start_gate_bots[], __stop_gate_bots[];
extern "C" char __start_gate_link[], __stop_gate_link[];
extern "C" char __start_gate_zz[], __stop_gate_zz[];

namespace {

namespace hg = nevr_quest::combat::headset_gates;
namespace pinned = sentinel::pinned;
using combat_test::Image;
using EchoVR::CSymbol64Hash;

constexpr std::int32_t kRealKind = 3;  // CVR::HeadsetType() answers 3, the Quest kind, in this build

extern "C" __attribute__((noinline)) std::int32_t PartyReal(void*, std::uint32_t) { return kRealKind; }
extern "C" __attribute__((noinline)) std::int32_t HeadsetReal() { return kRealKind; }

using PartyFn = std::int32_t (*)(void*, std::uint32_t);
using HeadFn = std::int32_t (*)();

// The callers: one function per range the gates name, and one outside every range. `volatile` keeps the call from being a tail call, so the
// return address is inside the function.
__attribute__((noinline, section("gate_perm"))) std::int32_t PartyCallerPerm(PartyFn f) {
  volatile std::int32_t r = f(nullptr, 0);
  return r;
}
__attribute__((noinline)) std::int32_t PartyCallerOut(PartyFn f) {
  volatile std::int32_t r = f(nullptr, 0);
  return r;
}
__attribute__((noinline, section("gate_bots"))) std::int32_t HeadCallerBots(HeadFn f) {
  volatile std::int32_t r = f();
  return r;
}
__attribute__((noinline, section("gate_link"))) std::int32_t HeadCallerLink(HeadFn f) {
  volatile std::int32_t r = f();
  return r;
}
__attribute__((noinline, section("gate_zz"))) std::int32_t HeadCallerAfter(HeadFn f) {
  volatile std::int32_t r = f();
  return r;
}
__attribute__((noinline)) std::int32_t HeadCallerOut(HeadFn f) {
  volatile std::int32_t r = f();
  return r;
}

// ---- the game's levels --------------------------------------------------------------------------------------------------------------
// CR15Game's two gamespace tables and a gamespace's level, laid out where combat_level.cpp reads them (the offsets are spelled here, not taken
// from the header), read by process_vm_readv as in production.
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

// ---- the image, the lines ---------------------------------------------------------------------------------------------------------

Image g_image;
hg::Pins g_pins = hg::kPins;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}

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
int Warnings() {
  int n = 0;
  for (const Line& l : g_lines) n += l.level == sentinel::LogLevel::kWarn ? 1 : 0;
  return n;
}
int Errors() {
  int n = 0;
  for (const Line& l : g_lines) n += l.level == sentinel::LogLevel::kError ? 1 : 0;
  return n;
}

// A libr15 holding the pinned words and both slots holding the game's functions (the stand-ins); the gates' ranges are the test's sections.
void Build() {
  g_pins = hg::kPins;
  g_image.Build({{hg::kPartySymbol, g_pins.partySlot, false, false, g_pins.partyFn},
                 {hg::kHeadsetSymbol, g_pins.headsetSlot, false, false, g_pins.headsetFn}});
  const std::uintptr_t base = g_image.Base();
  // The real functions are not in the image, so the pins hold the stand-ins' offsets, and the ranges are the test's sections'.
  g_pins.partyFn = reinterpret_cast<std::uintptr_t>(&PartyReal) - base;
  g_pins.headsetFn = reinterpret_cast<std::uintptr_t>(&HeadsetReal) - base;
  g_pins.permLo = reinterpret_cast<std::uintptr_t>(__start_gate_perm) - base;
  g_pins.permHi = reinterpret_cast<std::uintptr_t>(__stop_gate_perm) - base;
  g_pins.botsLo = reinterpret_cast<std::uintptr_t>(__start_gate_bots) - base;
  g_pins.botsHi = reinterpret_cast<std::uintptr_t>(__stop_gate_bots) - base;
  g_pins.linkLo = reinterpret_cast<std::uintptr_t>(__start_gate_link) - base;
  g_pins.linkHi = reinterpret_cast<std::uintptr_t>(__stop_gate_link) - base;
  for (const hg::Word& w : hg::kWords) g_image.Put<std::uint32_t>(w.vaddr, w.word);
  *g_image.Slot(g_pins.partySlot) = reinterpret_cast<void*>(&PartyReal);
  *g_image.Slot(g_pins.headsetSlot) = reinterpret_cast<void*>(&HeadsetReal);
  hg::SetImageLookupForTest(&Lookup);
  hg::SetPinsForTest(&g_pins);
}
PartyFn PartySlot() { return reinterpret_cast<PartyFn>(*g_image.Slot(g_pins.partySlot)); }
HeadFn HeadSlot() { return reinterpret_cast<HeadFn>(*g_image.Slot(g_pins.headsetSlot)); }

void Begin() {
  g_lines.clear();
  hg::ResetForTest();
  nevr_quest::combat::tick::ResetForTest();
  nevr_quest::combat::legacy::SetDirForTest(nullptr);
  LoadLevels({});
  sentinel::SetLogSink(&Capture);
}
void End() {
  hg::ResetForTest();
  nevr_quest::combat::tick::ResetForTest();
  nevr_quest::combat::legacy::SetDirForTest(nullptr);
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
}

// The install, with the captured lines printed when it does not install (a failing check then says why).
bool Installed(const hg::Switches& sw) {
  const nevr_quest::combat::ModuleResult r = hg::Install(sw);
  if (!r.installed) {
    for (const Line& l : g_lines) std::fprintf(stderr, "  line: %s\n", l.text.c_str());
  }
  return r.installed;
}
// Every switch on and the menu gate answering every library: on the host a Bind's library cannot be named, so only `all` answers the link range.
hg::Switches AllLibs() {
  hg::Switches sw;
  sw.linkLibs = "all";
  return sw;
}

// A combat map is loaded and the frame tick has seen the game.
void CombatMapLoaded() {
  LoadLevels({"mpl_combat_dyson"});
  hg::Tick(GameAddress());
}

// ---- the tests ------------------------------------------------------------------------------------------------------------------

void ThePinsAreTheReadelfAndDisassemblyPins() {
  const hg::Pins& d = hg::kPins;
  QCHECK(d.partySlot == 0x36fd880 && d.partyFn == 0x129b168 && d.headsetSlot == 0x36f9fd8 && d.headsetFn == 0x1a6f164);
  QCHECK(d.permLo == 0x232c794 && d.permHi == 0x232c854 && d.botsLo == 0x124dd38 && d.botsHi == 0x124e004);
  QCHECK(d.linkLo == 0x202ba74 && d.linkHi == 0x202bac8);
  QCHECK(hg::kBindSavedLrOffset == 0x18);
  // Each call site's return address is inside its range, and the range's first word is the function's first instruction.
  QCHECK(0x232c814 >= d.permLo && 0x232c814 < d.permHi);
  QCHECK(0x124dd44 >= d.botsLo && 0x124dd44 < d.botsHi);
  QCHECK(0x202baa4 >= d.linkLo && 0x202baa4 < d.linkHi);
  static const struct {
    std::uint64_t va;
    std::uint32_t word;
    hg::Gate gate;
  } kWant[13] = {{0x232c794, 0xd10103ff, hg::Gate::kPerm}, {0x232c810, 0x97b07ec8, hg::Gate::kPerm}, {0x232c814, 0x71000c1f, hg::Gate::kPerm},
                 {0x232c818, 0x54fffe00, hg::Gate::kPerm}, {0x124dd38, 0xa9bf7bf3, hg::Gate::kBots}, {0x124dd40, 0x97f3dd28, hg::Gate::kBots},
                 {0x124dd44, 0x71000c1f, hg::Gate::kBots}, {0x202ba74, 0xb4000341, hg::Gate::kLink}, {0x202ba7c, 0xd10083ff, hg::Gate::kLink},
                 {0x202ba80, 0xa9017bf3, hg::Gate::kLink}, {0x202baa0, 0x97bc65d0, hg::Gate::kLink}, {0x202baa4, 0x7100101f, hg::Gate::kLink},
                 {0x202bac8, 0x52800028, hg::Gate::kLink}};
  QCHECK(hg::kWordCount == 13);
  for (std::size_t i = 0; i < 13; ++i) {
    QCHECK(hg::kWords[i].vaddr == kWant[i].va && hg::kWords[i].word == kWant[i].word && hg::kWords[i].gate == kWant[i].gate);
  }
  QCHECK(std::strcmp(hg::kPartySymbol, "_ZNK10NRadEngine8NRadGame11CR15NetGame22PartyMemberHeadsetTypeEj") == 0);
  QCHECK(std::strcmp(hg::kHeadsetSymbol, "_ZN10NRadEngine3CVR11HeadsetTypeEv") == 0);
  QCHECK(hg::kLinkHeadset >= 1 && hg::kLinkHeadset <= 4 && hg::kLinkHeadset != 3);  // a kind the Bind maps, and not the Quest kind
  QCHECK(hg::kNotQuest != 3);
}

// The production pins, not the test's: on an image holding the pinned words and the pinned slots, the install publishes the three ranges at
// base + the pinned offsets (empty for a gate that is off), and swaps both slots. A call into the production ranges cannot be made on the host.
void TheInstallPublishesTheProductionRanges() {
  for (const int variant : {0, 1}) {  // 0: every gate on; 1: only the menu gate
    Begin();
    g_image.Build({{hg::kPartySymbol, hg::kPins.partySlot, false, false, hg::kPins.partyFn},
                   {hg::kHeadsetSymbol, hg::kPins.headsetSlot, false, false, hg::kPins.headsetFn}});
    const std::uintptr_t base = g_image.Base();
    for (const hg::Word& w : hg::kWords) g_image.Put<std::uint32_t>(w.vaddr, w.word);
    *g_image.Slot(hg::kPins.partySlot) = reinterpret_cast<void*>(base + hg::kPins.partyFn);
    *g_image.Slot(hg::kPins.headsetSlot) = reinterpret_cast<void*>(base + hg::kPins.headsetFn);
    hg::SetImageLookupForTest(&Lookup);
    hg::Switches sw;
    if (variant == 1) sw.perm = sw.bots = false;
    QCHECK(Installed(sw));
    const hg::Ranges r = hg::RangesForTest();
    QCHECK(*g_image.Slot(hg::kPins.headsetSlot) != reinterpret_cast<void*>(base + hg::kPins.headsetFn));
    QCHECK(r.linkLo == base + hg::kPins.linkLo && r.linkHi == base + hg::kPins.linkHi);
    if (variant == 0) {
      QCHECK(*g_image.Slot(hg::kPins.partySlot) != reinterpret_cast<void*>(base + hg::kPins.partyFn));
      QCHECK(r.permLo == base + hg::kPins.permLo && r.permHi == base + hg::kPins.permHi);
      QCHECK(r.botsLo == base + hg::kPins.botsLo && r.botsHi == base + hg::kPins.botsHi);
    } else {
      QCHECK(*g_image.Slot(hg::kPins.partySlot) == reinterpret_cast<void*>(base + hg::kPins.partyFn));  // gate_perm off: the slot is untouched
      QCHECK(r.permLo == r.permHi && r.botsLo == r.botsHi);  // a gate that is off has an empty range
    }
    End();
  }
}

// The default policy names the 20 combat menu libraries and the lobby script, once each, in the shape of a script library file name.
void TheDefaultPolicyIsTheTwentyOneLibraries() {
  std::vector<std::string> libs;
  std::string text = hg::kDefaultLinkLibs;
  for (std::size_t at = 0; at <= text.size();) {
    std::size_t comma = text.find(',', at);
    if (comma == std::string::npos) comma = text.size();
    libs.push_back(text.substr(at, comma - at));
    at = comma + 1;
  }
  QCHECK(libs.size() == 21U);
  int lobby = 0;
  for (const std::string& lib : libs) {
    QCHECK(lib.size() == 22U && lib.compare(0, 3, "lib") == 0 && lib.compare(lib.size() - 3, 3, ".so") == 0);
    lobby += lib == "libf808c072bb49da47.so" ? 1 : 0;
  }
  QCHECK(lobby == 1);
  QCHECK(text.size() + 1 < hg::kMaxLinkLibsText);
}

// The answers, through the slots: a call that returns into a gate's range answers the gate's value, every other caller the real one. All three
// gates on, a combat map loaded.
void EachGateAnswersItsOwnCallerAndNoOther() {
  Begin();
  Build();
  CombatMapLoaded();
  const nevr_quest::combat::ModuleResult r = hg::Install(AllLibs());
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
  QCHECK(nevr_quest::combat::tick::Count() == 1);  // the bot gate registered the callback that tells it the game
  const PartyFn party = PartySlot();
  const HeadFn head = HeadSlot();
  QCHECK(party != &PartyReal && head != &HeadsetReal);  // both slots replaced
  QCHECK(PartyCallerPerm(party) == hg::kNotQuest);      // a call from the gated range must answer not-a-Quest
  QCHECK(PartyCallerOut(party) == kRealKind);           // any other caller must get the real answer
  QCHECK(HeadCallerBots(head) == hg::kNotQuest);
  QCHECK(HeadCallerOut(head) == kRealKind);
  // the Bind's range gets its own answer: a real kind (4, link) that is not the Quest kind (3)
  QCHECK(HeadCallerLink(head) == hg::kLinkHeadset);
  QCHECK(HeadCallerBots(head) == hg::kNotQuest);  // the bots range keeps its own answer next to the link range
  QCHECK(HeadCallerOut(head) == kRealKind);       // any other caller still gets the real answer next to the link range
  // callers on both sides of the link range keep the real answer: the range has an upper edge as well as a lower one
  const auto after = reinterpret_cast<std::uintptr_t>(__start_gate_zz);
  QCHECK(after >= reinterpret_cast<std::uintptr_t>(__stop_gate_link) || after < reinterpret_cast<std::uintptr_t>(__start_gate_link));
  QCHECK(HeadCallerAfter(head) == kRealKind);
  QCHECK(after >= reinterpret_cast<std::uintptr_t>(__stop_gate_link));  // the linker placed the 'after' section above the link section
  const hg::Stats s = hg::CurrentStats();
  QCHECK(s.partyCalls == 2 && s.partyGated == 1);
  QCHECK(s.headsetCalls == 6 && s.botsGated == 2 && s.linkGated == 1 && s.botsScopedOut == 0);
  End();
}

// The library policy: a caller from the link range that cannot be named (the host has no Bind frame) is answered only by `all`.
void TheLibraryPolicyDecidesWhoGetsTheLinkAnswer() {
  Begin();
  Build();
  CombatMapLoaded();
  QCHECK(Installed(AllLibs()));
  const HeadFn head = HeadSlot();
  hg::SetLinkGateLibs("none");
  QCHECK(HeadCallerLink(head) == kRealKind);
  hg::SetLinkGateLibs("lib555db38933fef859.so,lib1da85b18cbad6297.so");
  QCHECK(HeadCallerLink(head) == kRealKind);  // a library that cannot be named is not in the list
  hg::SetLinkGateLibs("all");
  QCHECK(HeadCallerLink(head) == hg::kLinkHeadset);
  hg::SetLinkGateLibs("");
  QCHECK(HeadCallerLink(head) == kRealKind);  // an empty policy is none
  hg::SetLinkGateLibs(nullptr);
  QCHECK(HeadCallerLink(head) == hg::kLinkHeadset);  // no policy is all (the install always sets one)
  End();
}

// The link gate alone (the bot range empty) gates nothing else; the bot gate alone leaves the link range alone.
void AnEmptyRangeGatesNothing() {
  {
    Begin();
    Build();
    CombatMapLoaded();
    hg::Switches sw;
    sw.perm = false;
    sw.bots = false;
    sw.linkLibs = "all";
    QCHECK(Installed(sw));
    const HeadFn head = HeadSlot();
    QCHECK(HeadCallerLink(head) == hg::kLinkHeadset);
    QCHECK(HeadCallerBots(head) == kRealKind && HeadCallerOut(head) == kRealKind);  // the empty bot range must gate nothing, not everything
    QCHECK(PartySlot() == &PartyReal);  // gate_perm off: its slot is not touched
    QCHECK(nevr_quest::combat::tick::Count() == 0);  // gate_bots off: no tick callback is registered
    End();
  }
  {
    Begin();
    Build();
    CombatMapLoaded();
    hg::Switches sw;
    sw.perm = false;
    sw.link = false;
    QCHECK(Installed(sw));
    const HeadFn head = HeadSlot();
    QCHECK(HeadCallerBots(head) == hg::kNotQuest);
    QCHECK(HeadCallerLink(head) == kRealKind && HeadCallerOut(head) == kRealKind);
    End();
  }
  {
    Begin();
    Build();
    hg::Switches sw;
    sw.bots = false;
    sw.link = false;
    QCHECK(Installed(sw));
    QCHECK(HeadSlot() == &HeadsetReal);  // neither gate on the headset slot: the slot is not touched
    QCHECK(PartyCallerPerm(PartySlot()) == hg::kNotQuest);
    End();
  }
}

// The bot gate acts only while a combat map (or the combat lobby) is loaded. Arena and combat run on the same APK: in an Echo Arena level BotActorId
// chooses a bot body at random for a symbol it does not list, and body 1 does not exist there, so the gate leaves the real answer.
void TheBotGateActsOnlyWhereACombatMapOrLobbyIsLoaded() {
  Begin();
  Build();
  QCHECK(Installed(AllLibs()));
  const HeadFn head = HeadSlot();
  // before the frame tick has seen the game: no level is known, the real answer
  QCHECK(HeadCallerBots(head) == kRealKind);
  LoadLevels({"mpl_combat_dyson"});
  hg::Tick(GameAddress());
  QCHECK(HeadCallerBots(head) == hg::kNotQuest);
  LoadLevels({"mpl_lobby_b_combat"});
  QCHECK(HeadCallerBots(head) == hg::kNotQuest);
  for (const char* arena : {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a"}) {
    LoadLevels({arena});
    QCHECK(HeadCallerBots(head) == kRealKind);
  }
  LoadLevels({"mpl_combat_dyson", "mpl_arena_a"});  // a transition: an arena level that is loaded wins
  QCHECK(HeadCallerBots(head) == kRealKind);
  for (const char* other : {"mpl_lobby_b", "mpl_lobby_b2", "mpl_lobby_b_arena", "mpl_lobby_b_social"}) {  // the lobbies arena players use
    LoadLevels({other});
    QCHECK(HeadCallerBots(head) == kRealKind);
  }
  LoadLevels({});
  QCHECK(HeadCallerBots(head) == kRealKind);
  LoadLevels({"mpl_combat_gauss"});
  QCHECK(HeadCallerBots(head) == hg::kNotQuest);  // and back
  // The scope is the bot gate's alone: the menu and the permission gate answer in an arena level too (they are menu and party questions).
  LoadLevels({"mpl_arena_a"});
  QCHECK(HeadCallerLink(head) == hg::kLinkHeadset && PartyCallerPerm(PartySlot()) == hg::kNotQuest);
  const hg::Stats s = hg::CurrentStats();
  QCHECK(s.botsGated == 3 && s.botsScopedOut == 11);
  // The scoped-out answers are logged (the first eight), each with its reason: a headset run can read why a bot call kept the game's answer.
  const std::vector<Line> scoped = Lines("combat_gates_scoped_out");
  QCHECK(scoped.size() == 8U);
  if (scoped.size() == 8U) {
    QCHECK(Has(scoped[0].text, "\"gate\":\"bots\"") && Has(scoped[0].text, "\"reason\":\"no_game\""));
    QCHECK(Has(scoped[1].text, "\"reason\":\"echo_arena\"") && Has(scoped[5].text, "\"reason\":\"echo_arena\"") &&
           Has(scoped[6].text, "\"reason\":\"other_level\""));
  }
  End();
}

// 2219: an entry acts on the game's own call site, so it must be the newest hook on its slot. A handler that was on the slot first is called by the
// entry, and the gate keeps answering; a handler that asks to join above the entry is refused, and the gate keeps answering.
std::int32_t g_olderPartyCalls = 0, g_olderHeadCalls = 0;
struct OlderPartyTag {};
struct OlderHeadTag {};
using OlderParty = sentinel::CallbackThunk<OlderPartyTag, std::int32_t(void*, std::uint32_t)>;
using OlderHead = sentinel::CallbackThunk<OlderHeadTag, std::int32_t()>;
std::int32_t OlderPartyHandler(OlderParty::Fn original, void* self, std::uint32_t index) noexcept {
  ++g_olderPartyCalls;
  return original(self, index);
}
std::int32_t OlderHeadHandler(OlderHead::Fn original) noexcept {
  ++g_olderHeadCalls;
  return original();
}
NEVR_HOOK_RECORD(kOlderParty, OlderParty, &OlderPartyHandler);
NEVR_HOOK_RECORD(kOlderHead, OlderHead, &OlderHeadHandler);
struct LaterTag {};
using LaterHead = sentinel::CallbackThunk<LaterTag, std::int32_t()>;
std::int32_t g_laterCalls = 0;
std::int32_t LaterHeadHandler(LaterHead::Fn original) noexcept {
  ++g_laterCalls;
  return original();
}
NEVR_HOOK_RECORD(kLaterHead, LaterHead, &LaterHeadHandler);

void AGateInstalledAboveAnOlderHandlerKeepsAnswering() {
  Begin();
  Build();
  CombatMapLoaded();
  g_olderPartyCalls = g_olderHeadCalls = 0;
  OlderParty::Reset();
  OlderHead::Reset();
  sentinel::GotHook olderParty, olderHead;
  OlderParty::Arm(kOlderParty);
  OlderHead::Arm(kOlderHead);
  const std::uintptr_t base = g_image.Base();
  QCHECK_STATUS(sentinel::InstallThunk<OlderParty>(olderParty, hg::PartyTarget(base, g_pins).Owned("older_party"), &Lookup), sentinel::GotStatus::kOk);
  QCHECK_STATUS(sentinel::InstallThunk<OlderHead>(olderHead, hg::HeadsetTarget(base, g_pins).Owned("older_head"), &Lookup), sentinel::GotStatus::kOk);
  g_lines.clear();
  QCHECK(Installed(AllLibs()));
  QCHECK(Errors() == 0);
  const PartyFn party = PartySlot();
  const HeadFn head = HeadSlot();
  QCHECK(party != OlderParty::EntryAddress() && head != OlderHead::EntryAddress());  // the entries are above the older handlers
  QCHECK(PartyCallerPerm(party) == hg::kNotQuest && HeadCallerBots(head) == hg::kNotQuest && HeadCallerLink(head) == hg::kLinkHeadset);
  QCHECK(PartyCallerOut(party) == kRealKind && HeadCallerOut(head) == kRealKind);
  QCHECK(g_olderPartyCalls == 2 && g_olderHeadCalls == 3);  // the older handlers were called for every call, and the game's function behind them
  olderParty.Forget();  // the registry is process-wide and the next image may map at this address
  olderHead.Forget();
  End();
  OlderParty::Reset();
  OlderHead::Reset();
}

void ALaterHandlerCannotJoinAboveAGateAndTheGateKeepsAnswering() {
  Begin();
  Build();
  CombatMapLoaded();
  QCHECK(Installed(AllLibs()));
  LaterHead::Reset();
  g_laterCalls = 0;
  LaterHead::Arm(kLaterHead);
  sentinel::GotHook later;
  g_lines.clear();
  QCHECK_STATUS(sentinel::InstallThunk<LaterHead>(later, hg::HeadsetTarget(g_image.Base(), g_pins).Owned("later_module"), &Lookup),
                sentinel::GotStatus::kSlotHeldByEntry);
  QCHECK(!later.installed());
  // the refusal is one Warning that names the hook that lost and the chain with the gate's entry in it
  int refusals = 0;
  for (const Line& l : g_lines) {
    if (l.level == sentinel::LogLevel::kWarn && Has(l.text, "\"status\":\"slot_held_by_entry\"")) {
      ++refusals;
      QCHECK(Has(l.text, "\"owner\":\"later_module\"") && Has(l.text, "combat_gates(asm_entry)"));
    }
  }
  QCHECK(refusals == 1);
  // the gate answers as before, and the refused handler was never called
  const HeadFn head = HeadSlot();
  QCHECK(HeadCallerBots(head) == hg::kNotQuest && HeadCallerLink(head) == hg::kLinkHeadset && HeadCallerOut(head) == kRealKind);
  QCHECK(g_laterCalls == 0);
  End();
  LaterHead::Reset();
}

// A slot that does not hold the pinned function is refused and left alone, and the other slot's gate goes in.
void AForeignSlotWordIsRefusedAndLeftAlone() {
  Begin();
  Build();
  CombatMapLoaded();
  *g_image.Slot(g_pins.partySlot) = reinterpret_cast<void*>(0x4242420);
  const nevr_quest::combat::ModuleResult r = hg::Install(AllLibs());
  QCHECK(!r.installed && std::strcmp(r.status, "partial") == 0);
  QCHECK(*g_image.Slot(g_pins.partySlot) == reinterpret_cast<void*>(0x4242420));
  QCHECK(HeadSlot() != &HeadsetReal && HeadCallerLink(HeadSlot()) == hg::kLinkHeadset);
  const std::vector<Line> install = Lines("combat_gates_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError);
  if (install.size() == 1U) {
    QCHECK(Has(install[0].text, "\"gate_perm\":\"original_mismatch\"") && Has(install[0].text, "\"gate_bots\":\"installed\"") &&
           Has(install[0].text, "\"gate_link\":\"installed\""));
  }
  End();
}

// A pinned word that differs refuses its own gate and nothing else; the Warning names the gate, the address, the word read and the word wanted.
void APinnedWordThatDiffersRefusesItsGateAlone() {
  struct Case {
    hg::Gate gate;
    const char* name;
  } const cases[] = {{hg::Gate::kPerm, "perm"}, {hg::Gate::kBots, "bots"}, {hg::Gate::kLink, "link"}};
  for (const Case& c : cases) {
    Begin();
    Build();
    CombatMapLoaded();
    const hg::Word* changed = nullptr;
    for (const hg::Word& w : hg::kWords) {
      if (w.gate == c.gate) {
        changed = &w;
        break;
      }
    }
    g_image.Put<std::uint32_t>(changed->vaddr, changed->word ^ 1U);
    const nevr_quest::combat::ModuleResult r = hg::Install(AllLibs());
    QCHECK(!r.installed && std::strcmp(r.status, "partial") == 0);
    const std::vector<Line> hooks = Lines("combat_gates_hook");
    int mismatches = 0;
    for (const Line& l : hooks) {
      if (Has(l.text, "pinned_word_mismatch")) {
        ++mismatches;
        QCHECK(l.level == sentinel::LogLevel::kWarn && Has(l.text, std::string("\"gate\":\"") + c.name + "\""));
        QCHECK(Has(l.text, "\"vaddr\":\"0x") && Has(l.text, "\"have\":\"0x") && Has(l.text, "\"want\":\"0x"));
      }
    }
    QCHECK(mismatches == 1);
    // The gate that was refused answers the real value; the others answer theirs.
    const PartyFn party = PartySlot();
    const HeadFn head = HeadSlot();
    QCHECK((PartyCallerPerm(party) == hg::kNotQuest) == (c.gate != hg::Gate::kPerm));
    QCHECK((HeadCallerBots(head) == hg::kNotQuest) == (c.gate != hg::Gate::kBots));
    QCHECK((HeadCallerLink(head) == hg::kLinkHeadset) == (c.gate != hg::Gate::kLink));
    End();
  }
}

// The switches and their reasons, on the install line: every gate asked for, then each key off, then the dependency, then nothing asked.
void TheInstallLineNamesEachGateAndWhy() {
  {
    Begin();
    Build();
    CombatMapLoaded();
    QCHECK(Installed({}));
    const std::vector<Line> install = Lines("combat_gates_install");
    QCHECK(install.size() == 1U);
    if (install.size() == 1U) {
      QCHECK(install[0].level == sentinel::LogLevel::kInfo);
      QCHECK(Golden(install[0].text) ==
             "\"event\":\"combat_gates_install\",\"result\":\"installed\",\"reason\":\"ok\",\"gate_perm\":\"installed\",\"gate_bots\":\"installed\","
             "\"gate_link\":\"installed\",\"consequence\":\"a Quest gets combat permission in a party, the combat bot body on a combat map and the "
             "combat menus\"}");
    }
    const std::vector<Line> hooks = Lines("combat_gates_hook");
    QCHECK(hooks.size() == 2U);
    if (hooks.size() == 2U) {
      QCHECK(Has(hooks[0].text, "\"slot\":\"party\"") && Has(hooks[0].text, "\"gates\":\"perm\"") && Has(hooks[0].text, "\"status\":\"installed\""));
      QCHECK(Has(hooks[1].text, "\"slot\":\"headset\"") && Has(hooks[1].text, "\"gates\":\"bots,link\"") && Has(hooks[1].text, "\"status\":\"installed\""));
    }
    const std::vector<Line> policy = Lines("combat_gates_link_policy");
    QCHECK(policy.size() == 1U && Has(policy[0].text, "\"source\":\"default\"") && Has(policy[0].text, "libf808c072bb49da47.so"));
    QCHECK(Warnings() == 0 && Errors() == 0);
    End();
  }
  {  // each key off
    Begin();
    Build();
    hg::Switches sw;
    sw.perm = false;
    sw.link = false;
    QCHECK(Installed(sw));
    const std::vector<Line> install = Lines("combat_gates_install");
    QCHECK(install.size() == 1U);
    if (install.size() == 1U) {
      QCHECK(Has(install[0].text, "\"gate_perm\":\"off_config\"") && Has(install[0].text, "\"gate_bots\":\"installed\"") &&
             Has(install[0].text, "\"gate_link\":\"off_config\""));
    }
    QCHECK(Lines("combat_gates_link_policy").empty());  // no menu gate, no policy to state
    End();
  }
  {  // the dependency: levelname off takes the bot gate with it, the others go in
    Begin();
    Build();
    hg::Switches sw;
    sw.levelname = false;
    sw.linkLibs = "all";
    QCHECK(Installed(sw));
    const std::vector<Line> install = Lines("combat_gates_install");
    QCHECK(install.size() == 1U);
    if (install.size() == 1U) {
      QCHECK(Has(install[0].text, "\"gate_bots\":\"needs_levelname\"") && Has(install[0].text, "\"gate_perm\":\"installed\"") &&
             Has(install[0].text, "\"gate_link\":\"installed\""));
    }
    LoadLevels({"mpl_combat_dyson"});
    hg::Tick(GameAddress());
    QCHECK(HeadCallerBots(HeadSlot()) == kRealKind);  // the bot range is empty: a combat map does not make the gate act
    QCHECK(HeadCallerLink(HeadSlot()) == hg::kLinkHeadset);
    End();
  }
  {  // nothing asked: one Info line, no slot touched, and no libr15 needed
    Begin();
    hg::Switches sw;
    sw.perm = sw.bots = sw.link = false;
    const nevr_quest::combat::ModuleResult r = hg::Install(sw);
    QCHECK(!r.installed && std::strcmp(r.status, "off") == 0);
    const std::vector<Line> install = Lines("combat_gates_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
    if (install.size() == 1U) {
      QCHECK(Has(install[0].text, "\"result\":\"off\"") && Has(install[0].text, "\"gate_perm\":\"off_config\"") &&
             Has(install[0].text, "\"gate_bots\":\"off_config\"") && Has(install[0].text, "\"gate_link\":\"off_config\""));
    }
    End();
  }
}

// The policy text from `combat.gate_link_libs` replaces the default, and the line says where it came from.
void AConfiguredPolicyReplacesTheDefault() {
  Begin();
  Build();
  CombatMapLoaded();
  hg::Switches sw;
  sw.linkLibs = "none";
  QCHECK(Installed(sw));
  const std::vector<Line> policy = Lines("combat_gates_link_policy");
  QCHECK(policy.size() == 1U && Has(policy[0].text, "\"libs\":\"none\"") && Has(policy[0].text, "\"source\":\"config\""));
  QCHECK(HeadCallerLink(HeadSlot()) == kRealKind);  // none: the real answer everywhere
  End();
}

// The first eight gated answers of the permission and bot gates are logged with the caller's address and the real answer; the rest are counted.
void TheFirstAnswersAreLoggedWithTheRealAnswer() {
  Begin();
  Build();
  CombatMapLoaded();
  QCHECK(Installed(AllLibs()));
  for (int i = 0; i < 12; ++i) PartyCallerPerm(PartySlot());
  const std::vector<Line> answered = Lines("combat_gates_answered");
  QCHECK(answered.size() == 8U);
  if (!answered.empty()) {
    QCHECK(Has(answered[0].text, "\"gate\":\"perm\"") && Has(answered[0].text, "\"n\":1") && Has(answered[0].text, "\"real_answer\":3") &&
           Has(answered[0].text, "\"return_address\":\"0x"));
    QCHECK(Has(answered.back().text, "\"n\":8"));
  }
  QCHECK(hg::CurrentStats().partyGated == 12);
  End();
}

// The menu gate logs one line the first time a library binds the expression, and the host cannot name a library (no Bind frame).
void TheMenuGateLogsTheFirstBindOfALibrary() {
  Begin();
  Build();
  CombatMapLoaded();
  QCHECK(Installed(AllLibs()));
  for (int i = 0; i < 3; ++i) HeadCallerLink(HeadSlot());
  const std::vector<Line> binds = Lines("combat_gates_headset_bind");
  QCHECK(binds.size() == 1U);
  if (binds.size() == 1U) {
    QCHECK(Has(binds[0].text, "\"lib\":\"unknown\"") && Has(binds[0].text, "\"answered\":\"link\"") && Has(binds[0].text, "\"real_answer\":3") &&
           Has(binds[0].text, "\"calls\":1"));
  }
  QCHECK(hg::LinkGateLibsSeen() == 1);
  End();
}

// The plugin's switch files are not read: one found is a Warning naming the key that replaces it, and the gate is still on.
void ALegacySwitchFileIsAWarningAndChangesNothing() {
  char dir[] = "/tmp/headset_gates_legacy_XXXXXX";
  if (mkdtemp(dir) == nullptr) std::abort();
  for (const char* name : {"gate_perm.off", "gate_bots.off", "gate_libs.txt"}) {
    std::FILE* f = std::fopen((std::string(dir) + "/" + name).c_str(), "w");
    if (f == nullptr) std::abort();
    std::fclose(f);
  }
  Begin();
  Build();
  CombatMapLoaded();
  nevr_quest::combat::legacy::SetDirForTest(dir);
  QCHECK(Installed(AllLibs()));
  const std::vector<Line> legacy = Lines("combat_gates_legacy_switch");
  QCHECK(legacy.size() == 3U);
  for (const Line& l : legacy) QCHECK(l.level == sentinel::LogLevel::kWarn);
  if (legacy.size() == 3U) {
    QCHECK(Has(legacy[0].text, "\"file\":\"gate_perm.off\"") && Has(legacy[0].text, "combat.gate_perm: false"));
    QCHECK(Has(legacy[1].text, "\"file\":\"gate_bots.off\"") && Has(legacy[1].text, "combat.gate_bots: false"));
    QCHECK(Has(legacy[2].text, "\"file\":\"gate_libs.txt\"") && Has(legacy[2].text, "combat.gate_link_libs"));
  }
  QCHECK(PartyCallerPerm(PartySlot()) == hg::kNotQuest && HeadCallerBots(HeadSlot()) == hg::kNotQuest);  // the files turned nothing off
  End();
  for (const char* name : {"gate_perm.off", "gate_bots.off", "gate_libs.txt"}) unlink((std::string(dir) + "/" + name).c_str());
  rmdir(dir);
}

// Each refusal before a slot: no libr15, a build ID that is not the patched build's, a second step.
void ARefusedStepTouchesNothingAndSaysWhy() {
  {
    Begin();  // no image
    const nevr_quest::combat::ModuleResult r = hg::Install({});
    QCHECK(!r.installed && std::strcmp(r.status, "module_not_loaded") == 0);
    const std::vector<Line> install = Lines("combat_gates_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"module_not_loaded\""));
    End();
  }
  {
    Begin();
    g_image.Build({{hg::kPartySymbol, hg::kPins.partySlot, false, false, hg::kPins.partyFn},
                   {hg::kHeadsetSymbol, hg::kPins.headsetSlot, false, false, hg::kPins.headsetFn}},
                  "0123456789abcdef0123456789abcdef01234567");
    hg::SetImageLookupForTest(&Lookup);
    const nevr_quest::combat::ModuleResult r = hg::Install({});
    QCHECK(!r.installed && std::strcmp(r.status, "build_id_mismatch") == 0);
    End();
  }
  {
    Begin();
    Build();
    QCHECK(Installed(AllLibs()));
    const nevr_quest::combat::ModuleResult again = hg::Install({});
    QCHECK(!again.installed && std::strcmp(again.status, "already_ran") == 0);
    End();
  }
}

}  // namespace

int main() {
  ThePinsAreTheReadelfAndDisassemblyPins();
  TheInstallPublishesTheProductionRanges();
  TheDefaultPolicyIsTheTwentyOneLibraries();
  EachGateAnswersItsOwnCallerAndNoOther();
  TheLibraryPolicyDecidesWhoGetsTheLinkAnswer();
  AnEmptyRangeGatesNothing();
  TheBotGateActsOnlyWhereACombatMapOrLobbyIsLoaded();
  AGateInstalledAboveAnOlderHandlerKeepsAnswering();
  ALaterHandlerCannotJoinAboveAGateAndTheGateKeepsAnswering();
  AForeignSlotWordIsRefusedAndLeftAlone();
  APinnedWordThatDiffersRefusesItsGateAlone();
  TheInstallLineNamesEachGateAndWhy();
  AConfiguredPolicyReplacesTheDefault();
  TheFirstAnswersAreLoggedWithTheRealAnswer();
  TheMenuGateLogsTheFirstBindOfALibrary();
  ALegacySwitchFileIsAWarningAndChangesNothing();
  ARefusedStepTouchesNothingAndSaysWhy();
  if (quest_test::Failures() == 0) std::printf("headset_gates_test: all checks pass\n");
  return quest_test::Failures() == 0 ? 0 : 1;
}
