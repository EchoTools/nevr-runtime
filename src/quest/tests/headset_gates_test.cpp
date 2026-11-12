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

#include <dlfcn.h>
#include <sys/mman.h>
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
#include "quest/combat/levelname.h"
#include "quest/sentinel/hook_install.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

extern "C" char __start_gate_perm[], __stop_gate_perm[];
extern "C" char __start_gate_bots[], __stop_gate_bots[];
extern "C" char __start_gate_link[], __stop_gate_link[];
extern "C" char __start_gate_zz[], __stop_gate_zz[];

// The two continuations the entries reach (headset_gates.cpp): the host entries call them with the caller's return address and no Bind frame, so a test that
// wants a frame (the Bind's sp and its saved return address) calls them itself.
extern "C" std::int32_t nevr_combat_headset_dispatch(std::uintptr_t bindSp, std::uintptr_t ret);
extern "C" std::int32_t nevr_combat_party_dispatch(void* self, std::uint32_t index, std::uintptr_t ret);

// dladdr, as LibraryName asks it: while g_fakeLibrary is set every address is in that file (a Bind's caller can be named at will); else libc's answer.
static const char* g_fakeLibrary = nullptr;
extern "C" int dladdr(const void* address, Dl_info* info) noexcept {
  using RealDladdr = int (*)(const void*, Dl_info*);
  static const RealDladdr real = reinterpret_cast<RealDladdr>(dlsym(RTLD_NEXT, "dladdr"));
  if (g_fakeLibrary == nullptr) return real(address, info);
  info->dli_fname = g_fakeLibrary;
  return 1;
}

namespace {

namespace hg = nevr_quest::combat::headset_gates;
namespace pinned = sentinel::pinned;
using combat_test::Image;
using EchoVR::CSymbol64Hash;

constexpr std::int32_t kRealKind = 3;  // CVR::HeadsetType() answers 3, the Quest kind, in this build

std::uintptr_t g_partySelf = 0;  // what the game's party function was last called with
std::uint32_t g_partyIndex = 0;
constexpr std::uintptr_t kPartySelfArg = 0x1230;
constexpr std::uint32_t kPartyIndexArg = 7;
extern "C" __attribute__((noinline)) std::int32_t PartyReal(void* self, std::uint32_t index) {
  g_partySelf = reinterpret_cast<std::uintptr_t>(self);
  g_partyIndex = index;
  return kRealKind;
}
extern "C" __attribute__((noinline)) std::int32_t HeadsetReal() { return kRealKind; }

using PartyFn = std::int32_t (*)(void*, std::uint32_t);
using HeadFn = std::int32_t (*)();

// The callers: one function per range the gates name, and one outside every range. `volatile` keeps the call from being a tail call, so the
// return address is inside the function.
__attribute__((noinline, section("gate_perm"))) std::int32_t PartyCallerPerm(PartyFn f) {
  volatile std::int32_t r = f(reinterpret_cast<void*>(kPartySelfArg), kPartyIndexArg);
  return r;
}
__attribute__((noinline)) std::int32_t PartyCallerOut(PartyFn f) {
  volatile std::int32_t r = f(reinterpret_cast<void*>(kPartySelfArg), kPartyIndexArg);
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

// CR15Game's LoadLevelName vtable word: levelname's hook (any word but the stock function) or the stock function.
void SetLevelname(bool installed) {
  g_image.Put<std::uintptr_t>(hg::kPins.levelNameSlot, g_image.Base() + (installed ? 0x1234560 : hg::kPins.levelNameStock));
}

// A libr15 holding the pinned words and both slots holding the game's functions (the stand-ins); the gates' ranges are the test's sections.
// `withLevelname`: the image also holds the four ABS64 words of CR15Game::LoadLevelName that the runtime's levelname hooks (levelname_targets.h), each
// holding the stock function, so the runtime's own levelname can be installed on the same image the gates read.
void Build(bool withLevelname = false) {
  g_pins = hg::kPins;
  std::vector<combat_test::Reloc> relocs = {{hg::kPartySymbol, g_pins.partySlot, false, false, g_pins.partyFn},
                                            {hg::kHeadsetSymbol, g_pins.headsetSlot, false, false, g_pins.headsetFn}};
  namespace ln = nevr_quest::combat::levelname::targets;
  if (withLevelname) {
    for (const std::uint64_t slot : ln::kSlots) relocs.push_back({ln::kSymbol, slot, false, false, ln::kFnVaddr, /*abs64=*/true});
  }
  g_image.Build(relocs);
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
  SetLevelname(true);
  if (withLevelname) {
    for (const std::uint64_t slot : ln::kSlots) g_image.Put<std::uintptr_t>(slot, base + ln::kFnVaddr);  // the stock function in all four
  }
  hg::SetImageLookupForTest(&Lookup);
  hg::SetPinsForTest(&g_pins);
}
PartyFn PartySlot() { return reinterpret_cast<PartyFn>(*g_image.Slot(g_pins.partySlot)); }
HeadFn HeadSlot() { return reinterpret_cast<HeadFn>(*g_image.Slot(g_pins.headsetSlot)); }

void Begin() {
  g_lines.clear();
  hg::ResetForTest();
  nevr_quest::combat::tick::ResetForTest();
  nevr_quest::combat::legacy::SetDirectoryForTest(nullptr);
  LoadLevels({});
  sentinel::SetLogSink(&Capture);
}
void End() {
  hg::ResetForTest();
  nevr_quest::combat::tick::ResetForTest();
  nevr_quest::combat::legacy::SetDirectoryForTest(nullptr);
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
  QCHECK(d.levelNameSlot == 0x362ef90 && d.levelNameStock == 0x1202ecc);  // levelname.cpp kSlots[0], kFnVaddr; got_pinned_test checks the slot names CR15Game::LoadLevelName
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
  // The list itself: one name changed is a menu that keeps the real answer (or a library that gets the wrong one).
  QCHECK(text == "lib555db38933fef859.so,lib1da85b18cbad6297.so,lib4e984de06cafe2ab.so,lib2d5ebd0e1895f269.so,libcab86dd5921bafc7.so,lib5e311be23be97835.so,libbad7ce9b6c6df2cb.so,libf36316e246f2ca21.so,lib8325751e75df5860.so,lib18000621255c6ea5.so,lib1f28bd4fa7d724fb.so,lib3ded824fccc2bf6d.so,lib5ddc3dcdad4a3ed5.so,lib8515ab072bc095a8.so,libb56c58de2b68fc61.so,libb7882b9d804f86c1.so,libb8bd659eb6ed3927.so,libbc170a382fda5499.so,libd280bb8333ad45ed.so,libfb4ad3853db57943.so,libf808c072bb49da47.so");
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
    const std::vector<Line> hooks = Lines("combat_gates_hook");
    QCHECK(hooks.size() == 1U && Has(hooks[0].text, "\"slot\":\"headset\"") && Has(hooks[0].text, "\"gates\":\"link\""));
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
    const std::vector<Line> hooks = Lines("combat_gates_hook");
    QCHECK(hooks.size() == 1U && Has(hooks[0].text, "\"slot\":\"headset\"") && Has(hooks[0].text, "\"gates\":\"bots\""));
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
    const std::vector<Line> hooks = Lines("combat_gates_hook");
    QCHECK(hooks.size() == 1U && Has(hooks[0].text, "\"slot\":\"party\"") && Has(hooks[0].text, "\"gates\":\"perm\""));
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
    for (std::size_t i = 0; i < scoped.size(); ++i) {
      QCHECK(scoped[i].level == sentinel::LogLevel::kInfo);
      QCHECK(Has(scoped[i].text, "\"n\":" + std::to_string(i + 1)));  // the first eight, counted from 1
    }
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
struct LaterPartyTag {};
using LaterParty = sentinel::CallbackThunk<LaterPartyTag, std::int32_t(void*, std::uint32_t)>;
std::int32_t g_laterPartyCalls = 0;
std::int32_t LaterPartyHandler(LaterParty::Fn original, void* self, std::uint32_t index) noexcept {
  ++g_laterPartyCalls;
  return original(self, index);
}
NEVR_HOOK_RECORD(kLaterParty, LaterParty, &LaterPartyHandler);

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

  // The party slot's entry holds its slot the same way (#2180 review): a later handler is refused, one Warning names it and the chain with the entry.
  LaterParty::Reset();
  g_laterPartyCalls = 0;
  LaterParty::Arm(kLaterParty);
  sentinel::GotHook laterParty;
  g_lines.clear();
  QCHECK_STATUS(sentinel::InstallThunk<LaterParty>(laterParty, hg::PartyTarget(g_image.Base(), g_pins).Owned("later_party_module"), &Lookup),
                sentinel::GotStatus::kSlotHeldByEntry);
  QCHECK(!laterParty.installed());
  int partyRefusals = 0;
  for (const Line& l : g_lines) {
    if (l.level == sentinel::LogLevel::kWarn && Has(l.text, "\"status\":\"slot_held_by_entry\"")) {
      ++partyRefusals;
      QCHECK(Has(l.text, "\"owner\":\"later_party_module\"") && Has(l.text, "combat_gates(asm_entry)"));
    }
  }
  QCHECK(partyRefusals == 1);
  QCHECK(PartyCallerPerm(PartySlot()) == hg::kNotQuest && PartyCallerOut(PartySlot()) == kRealKind && g_laterPartyCalls == 0);
  End();
  LaterHead::Reset();
  LaterParty::Reset();
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
    QCHECK(Has(install[0].text, "\"result\":\"partial\"") && Has(install[0].text, "\"reason\":\"gate_refused\""));
    QCHECK(Has(install[0].text, "leaves the game's own behaviour for its part"));  // the refused consequence, not the installed one
  }
  const hg::Ranges ranges = hg::RangesForTest();
  QCHECK(ranges.permLo == 0 && ranges.permHi == 0);  // a refused gate leaves no range published
  QCHECK(ranges.botsLo != ranges.botsHi && ranges.linkLo != ranges.linkHi);
  const std::vector<Line> hooks = Lines("combat_gates_hook");
  QCHECK(hooks.size() == 2U);
  if (hooks.size() == 2U) {
    QCHECK(hooks[0].level == sentinel::LogLevel::kError && Has(hooks[0].text, "\"slot\":\"party\"") && Has(hooks[0].text, "\"gates\":\"perm\"") &&
           Has(hooks[0].text, std::string("\"symbol\":\"") + hg::kPartySymbol + "\"") && Has(hooks[0].text, "\"status\":\"original_mismatch\""));
    QCHECK(hooks[1].level == sentinel::LogLevel::kInfo && Has(hooks[1].text, "\"slot\":\"headset\"") &&
           Has(hooks[1].text, std::string("\"symbol\":\"") + hg::kHeadsetSymbol + "\"") && Has(hooks[1].text, "\"status\":\"installed\""));
  }
  End();
}

// The headset slot is one entry for two gates: a foreign word there refuses both, publishes neither range and leaves the party gate alone.
void AForeignHeadsetSlotRefusesBothGatesOnIt() {
  Begin();
  Build();
  CombatMapLoaded();
  *g_image.Slot(g_pins.headsetSlot) = reinterpret_cast<void*>(0x4242420);
  const nevr_quest::combat::ModuleResult r = hg::Install(AllLibs());
  QCHECK(!r.installed && std::strcmp(r.status, "partial") == 0);
  QCHECK(*g_image.Slot(g_pins.headsetSlot) == reinterpret_cast<void*>(0x4242420));
  QCHECK(PartyCallerPerm(PartySlot()) == hg::kNotQuest);  // the other slot's gate went in
  const hg::Ranges ranges = hg::RangesForTest();
  QCHECK(ranges.botsLo == 0 && ranges.botsHi == 0 && ranges.linkLo == 0 && ranges.linkHi == 0);
  QCHECK(ranges.permLo != ranges.permHi);
  const std::vector<Line> install = Lines("combat_gates_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kError);
    QCHECK(Has(install[0].text, "\"result\":\"partial\"") && Has(install[0].text, "\"reason\":\"gate_refused\""));
    QCHECK(Has(install[0].text, "\"gate_perm\":\"installed\"") && Has(install[0].text, "\"gate_bots\":\"original_mismatch\"") &&
           Has(install[0].text, "\"gate_link\":\"original_mismatch\""));
  }
  const std::vector<Line> hooks = Lines("combat_gates_hook");
  QCHECK(hooks.size() == 2U);
  if (hooks.size() == 2U) {
    QCHECK(hooks[1].level == sentinel::LogLevel::kError && Has(hooks[1].text, "\"slot\":\"headset\"") && Has(hooks[1].text, "\"gates\":\"bots,link\"") &&
           Has(hooks[1].text, std::string("\"symbol\":\"") + hg::kHeadsetSymbol + "\"") && Has(hooks[1].text, "\"status\":\"original_mismatch\""));
  }
  End();
}

// Both slots foreign: nothing is installed, the result is refused, and no range is left published.
void ABothForeignSlotsAreRefusedAndNothingIsPublished() {
  Begin();
  Build();
  CombatMapLoaded();
  *g_image.Slot(g_pins.partySlot) = reinterpret_cast<void*>(0x4242420);
  *g_image.Slot(g_pins.headsetSlot) = reinterpret_cast<void*>(0x4242428);
  const nevr_quest::combat::ModuleResult r = hg::Install(AllLibs());
  QCHECK(!r.installed && std::strcmp(r.status, "refused") == 0);
  const hg::Ranges ranges = hg::RangesForTest();
  QCHECK(ranges.permLo == 0 && ranges.permHi == 0 && ranges.botsLo == 0 && ranges.botsHi == 0 && ranges.linkLo == 0 && ranges.linkHi == 0);
  const std::vector<Line> install = Lines("combat_gates_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"result\":\"refused\"") &&
           Has(install[0].text, "\"reason\":\"gate_refused\""));
    QCHECK(Has(install[0].text, "\"gate_perm\":\"original_mismatch\"") && Has(install[0].text, "\"gate_bots\":\"original_mismatch\"") &&
           Has(install[0].text, "\"gate_link\":\"original_mismatch\""));
  }
  End();
}

// The bot gate reads the loaded levels of the game the tick saw, so it registers a tick callback; with the table full it is refused alone.
void DummyTick(std::uintptr_t) noexcept {}
void AFullTickTableRefusesTheBotGateAlone() {
  Begin();
  Build();
  CombatMapLoaded();
  for (std::size_t i = 0; i < nevr_quest::combat::tick::kMaxCallbacks; ++i) QCHECK(nevr_quest::combat::tick::Add(&DummyTick));
  const nevr_quest::combat::ModuleResult r = hg::Install(AllLibs());
  QCHECK(!r.installed && std::strcmp(r.status, "partial") == 0);
  const std::vector<Line> install = Lines("combat_gates_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kError);
    QCHECK(Has(install[0].text, "\"gate_perm\":\"installed\"") && Has(install[0].text, "\"gate_bots\":\"tick_table_full\"") &&
           Has(install[0].text, "\"gate_link\":\"installed\""));
  }
  const std::vector<Line> hooks = Lines("combat_gates_hook");
  QCHECK(hooks.size() == 2U && Has(hooks[1].text, "\"gates\":\"link\""));
  const hg::Ranges ranges = hg::RangesForTest();
  QCHECK(ranges.botsLo == ranges.botsHi);  // the bot gate's range is empty
  const HeadFn head = HeadSlot();
  QCHECK(HeadCallerBots(head) == kRealKind);  // not installed: the real answer in a combat map
  QCHECK(HeadCallerLink(head) == hg::kLinkHeadset && PartyCallerPerm(PartySlot()) == hg::kNotQuest);
  QCHECK(nevr_quest::combat::tick::Count() == nevr_quest::combat::tick::kMaxCallbacks);
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
        char vaddr[40], have[40], want[40];
        std::snprintf(vaddr, sizeof vaddr, "\"vaddr\":\"0x%016llx\"", static_cast<unsigned long long>(changed->vaddr));
        std::snprintf(have, sizeof have, "\"have\":\"0x%016llx\"", static_cast<unsigned long long>(changed->word ^ 1U));
        std::snprintf(want, sizeof want, "\"want\":\"0x%016llx\"", static_cast<unsigned long long>(changed->word));
        QCHECK(Has(l.text, vaddr) && Has(l.text, have) && Has(l.text, want));
      }
    }
    QCHECK(mismatches == 1);
    const std::vector<Line> install = Lines("combat_gates_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError);
    if (install.size() == 1U) QCHECK(Has(install[0].text, std::string("\"gate_") + c.name + "\":\"pinned_word_mismatch\""));
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
  {  // nothing asked: one Info line, no slot touched, and no libr15 needed
    Begin();
    hg::Switches sw;
    sw.perm = sw.bots = sw.link = false;
    const nevr_quest::combat::ModuleResult r = hg::Install(sw);
    QCHECK(!r.installed && r.off && std::strcmp(r.status, "off") == 0);
    const std::vector<Line> install = Lines("combat_gates_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
    if (install.size() == 1U) {
      QCHECK(Has(install[0].text, "\"result\":\"off\"") && Has(install[0].text, "\"reason\":\"off_config\"") && Has(install[0].text, "\"gate_perm\":\"off_config\"") &&
             Has(install[0].text, "\"gate_bots\":\"off_config\"") && Has(install[0].text, "\"gate_link\":\"off_config\""));
    }
    End();
  }
}

// The bot gate depends on the game, not on a switch (#2180 review B1): the combat bot (body 1) exists only in the full-size global_mp, which loads only
// where levelname's hook is on the game's LoadLevelName slot. With the stock function in that slot (levelname off, anywhere, by any switch) the gate
// answers the real value on a combat map; with a hook there it answers. The slot is read at each call, so a levelname that installs later is seen.
void TheBotGateNeedsLevelnameInstalledInTheGame() {
  Begin();
  Build();
  CombatMapLoaded();
  QCHECK(Installed(AllLibs()));
  const HeadFn head = HeadSlot();
  QCHECK(HeadCallerBots(head) == hg::kNotQuest);  // a hook on the slot
  SetLevelname(false);                            // the stock function: global_mp_lowspec loads
  QCHECK(HeadCallerBots(head) == kRealKind);
  QCHECK(HeadCallerLink(head) == hg::kLinkHeadset && PartyCallerPerm(PartySlot()) == hg::kNotQuest);  // the other gates do not depend on it
  SetLevelname(true);                             // levelname installs after the gates
  QCHECK(HeadCallerBots(head) == hg::kNotQuest);
  g_image.Put<std::uintptr_t>(hg::kPins.levelNameSlot, 0);  // an unreadable or empty word is "not installed"
  QCHECK(HeadCallerBots(head) == kRealKind);
  const std::vector<Line> scoped = Lines("combat_gates_scoped_out");
  QCHECK(!scoped.empty() && Has(scoped[0].text, "\"reason\":\"levelname_not_installed\""));
  const hg::Stats s = hg::CurrentStats();
  QCHECK(s.botsGated == 2 && s.botsScopedOut == 2);
  End();
}

// levelname moved into the runtime (#2181) and the bot gate still sees it: the gate reads CR15Game's LoadLevelName vtable word and acts where it is not
// the stock function, so the runtime's levelname must install into that very word. This installs the real levelname (levelname::InstallAt, the four words,
// the real GotHook) on the image the gates read: the gate answers once it is in, keeps the game's choice before it and after it is taken out, and keeps it
// with `combat.levelname` off (the module installs nothing then). The two modules' pins are the same word and the same stock function.
void TheBotGateSeesTheRuntimesLevelname() {
  namespace ln = nevr_quest::combat::levelname;
  QCHECK(hg::kPins.levelNameSlot == ln::targets::kSlots[0] && hg::kPins.levelNameStock == ln::targets::kFnVaddr);
  constexpr const char* kNoLegacyDir = "/nonexistent-levelname-legacy-dir";
  Begin();
  ln::ResetForTest();
  ln::SetImageLookupForTest(&Lookup);
  nevr_quest::combat::legacy::SetDirectoryForTest(kNoLegacyDir);
  Build(/*withLevelname=*/true);
  CombatMapLoaded();
  QCHECK(Installed(AllLibs()));
  const HeadFn head = HeadSlot();
  const std::uintptr_t stock = g_image.Base() + ln::targets::kFnVaddr;
  QCHECK(g_image.Get<std::uintptr_t>(hg::kPins.levelNameSlot) == stock);
  QCHECK(HeadCallerBots(head) == kRealKind);  // levelname not in: the cut level loads, the combat bot is not in it
  // combat.levelname off: the module installs nothing, the word stays the game's, the gate keeps the game's choice
  QCHECK(!ln::InstallAt(&Lookup, /*on=*/false).installed);
  QCHECK(g_image.Get<std::uintptr_t>(hg::kPins.levelNameSlot) == stock && HeadCallerBots(head) == kRealKind);
  // combat.levelname on: the real hook takes the slot the gate reads
  QCHECK(ln::InstallAt(&Lookup, /*on=*/true).installed);
  QCHECK(g_image.Get<std::uintptr_t>(hg::kPins.levelNameSlot) != stock);
  QCHECK(HeadCallerBots(head) == hg::kNotQuest);
  ln::ResetForTest();  // the hook out again: the word is the stock function and the gate leaves the game's choice
  QCHECK(g_image.Get<std::uintptr_t>(hg::kPins.levelNameSlot) == stock && HeadCallerBots(head) == kRealKind);
  const hg::Stats s = hg::CurrentStats();
  QCHECK(s.botsGated == 1 && s.botsScopedOut == 3);
  ln::SetImageLookupForTest(nullptr);
  End();
}

// The range compare's edges (#2180 review M06): the first instruction of the range is in, the last is in, the one after it is not, and the one before is not.
void TheRangeIsHalfOpen() {
  QCHECK(!hg::InRange(0xfff, 0x1000, 0x1100));
  QCHECK(hg::InRange(0x1000, 0x1000, 0x1100));
  QCHECK(hg::InRange(0x10fc, 0x1000, 0x1100));
  QCHECK(!hg::InRange(0x1100, 0x1000, 0x1100));  // the upper edge is the next instruction, not the caller
  QCHECK(!hg::InRange(0x1000, 0, 0) && !hg::InRange(0, 0, 0) && !hg::InRange(0x1000, 0x1000, 0x1000));  // an empty range gates nothing
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
  {  // an empty text is configured too, and reads as none
    Begin();
    Build();
    CombatMapLoaded();
    hg::Switches empty;
    empty.linkLibs = "";
    QCHECK(Installed(empty));
    const std::vector<Line> emptyPolicy = Lines("combat_gates_link_policy");
    QCHECK(emptyPolicy.size() == 1U && Has(emptyPolicy[0].text, "\"libs\":\"none\"") && Has(emptyPolicy[0].text, "\"source\":\"config\""));
    QCHECK(emptyPolicy[0].level == sentinel::LogLevel::kInfo);
    QCHECK(HeadCallerLink(HeadSlot()) == kRealKind);
    End();
  }
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
  nevr_quest::combat::legacy::SetDirectoryForTest(dir);
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
    if (install.size() == 1U) {
      QCHECK(Has(install[0].text, "\"result\":\"refused\"") && Has(install[0].text, "\"gate_perm\":\"not_tried\"") &&
             Has(install[0].text, "\"gate_bots\":\"not_tried\"") && Has(install[0].text, "\"gate_link\":\"not_tried\""));
    }
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
    const std::vector<Line> install = Lines("combat_gates_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"build_id_mismatch\""));
    End();
  }
  {
    Begin();
    Build();
    QCHECK(Installed(AllLibs()));
    const nevr_quest::combat::ModuleResult again = hg::Install({});
    QCHECK(!again.installed && std::strcmp(again.status, "already_ran") == 0);
    const std::vector<Line> install = Lines("combat_gates_install");
    QCHECK(install.size() == 2U);
    if (install.size() == 2U) {
      QCHECK(install[1].level == sentinel::LogLevel::kWarn && Has(install[1].text, "\"reason\":\"already_ran\""));
    }
    End();
  }
}


// A build with no build-ID note is refused before any slot, with its own token and nothing tried.
void ABuildWithNoIdIsRefusedBeforeAnySlot() {
  Begin();
  g_image.Build({{hg::kPartySymbol, hg::kPins.partySlot, false, false, hg::kPins.partyFn},
                 {hg::kHeadsetSymbol, hg::kPins.headsetSlot, false, false, hg::kPins.headsetFn}},
                nullptr, /*note=*/false);
  hg::SetImageLookupForTest(&Lookup);
  const nevr_quest::combat::ModuleResult r = hg::Install({});
  QCHECK(!r.installed && std::strcmp(r.status, "no_build_id") == 0);
  const std::vector<Line> install = Lines("combat_gates_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError);
  if (install.size() == 1U) {
    QCHECK(Has(install[0].text, "\"reason\":\"no_build_id\"") && Has(install[0].text, "\"gate_perm\":\"not_tried\""));
  }
  End();
}

// No lookup at all is a refusal, not a call through a null function.
void AnInstallWithNoLookupIsRefused() {
  Begin();
  const nevr_quest::combat::ModuleResult r = hg::InstallAt(nullptr, hg::kPins, {});
  QCHECK(!r.installed && std::strcmp(r.status, "module_not_loaded") == 0);
  End();
}

// A pinned word that cannot be read (its page is gone) is a mismatch that says so, not a word of zeros that happens to differ.
void AnUnreadablePinnedWordRefusesItsGateAndSaysSo() {
  Begin();
  Build();
  CombatMapLoaded();
  const std::uintptr_t page = (g_image.Base() + hg::kWords[0].vaddr) & ~static_cast<std::uintptr_t>(0xfff);
  QCHECK(mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_NONE) == 0);
  const nevr_quest::combat::ModuleResult r = hg::Install(AllLibs());
  QCHECK(mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_READ | PROT_WRITE) == 0);
  QCHECK(!r.installed && std::strcmp(r.status, "partial") == 0);
  int mismatches = 0;
  for (const Line& l : Lines("combat_gates_hook")) {
    if (!Has(l.text, "pinned_word_mismatch")) continue;
    ++mismatches;
    char vaddr[40], want[40];
    std::snprintf(vaddr, sizeof vaddr, "\"vaddr\":\"0x%016llx\"", static_cast<unsigned long long>(hg::kWords[0].vaddr));
    std::snprintf(want, sizeof want, "\"want\":\"0x%016llx\"", static_cast<unsigned long long>(hg::kWords[0].word));
    QCHECK(Has(l.text, "\"gate\":\"perm\"") && Has(l.text, vaddr) && Has(l.text, want) && Has(l.text, "\"have\":\"unreadable\""));
  }
  QCHECK(mismatches == 1);
  End();
}

// The game's code is read-only in the real image: the word check reads it, it never asks to write.
void PinnedWordsInReadOnlyCodeAreRead() {
  Begin();
  Build();
  CombatMapLoaded();
  for (const hg::Word& w : hg::kWords) {
    const std::uintptr_t page = (g_image.Base() + w.vaddr) & ~static_cast<std::uintptr_t>(0xfff);
    QCHECK(mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_READ) == 0);
  }
  QCHECK(Installed(AllLibs()));
  for (const hg::Word& w : hg::kWords) {
    const std::uintptr_t page = (g_image.Base() + w.vaddr) & ~static_cast<std::uintptr_t>(0xfff);
    QCHECK(mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_READ | PROT_WRITE) == 0);
  }
  End();
}

// With nothing installed the continuations have no game function to call: the answer is 0, and no range gates anything.
void ACallBeforeTheInstallGetsZeroFromTheMissingGamesFunction() {
  Begin();
  QCHECK(nevr_combat_party_dispatch(nullptr, 0, 0) == 0);
  QCHECK(nevr_combat_headset_dispatch(0, 0) == 0);
  End();
}

// The game's party function is called with the entry's own arguments, whether or not the gate answers for that caller.
void ThePartyCallReachesTheGamesFunctionWithItsArguments() {
  Begin();
  Build();
  CombatMapLoaded();
  QCHECK(Installed(AllLibs()));
  g_partySelf = 0;
  g_partyIndex = 0;
  QCHECK(PartyCallerPerm(PartySlot()) == hg::kNotQuest);
  QCHECK(g_partySelf == kPartySelfArg && g_partyIndex == kPartyIndexArg);
  g_partySelf = 0;
  g_partyIndex = 0;
  QCHECK(PartyCallerOut(PartySlot()) == kRealKind);
  QCHECK(g_partySelf == kPartySelfArg && g_partyIndex == kPartyIndexArg);
  End();
}

std::uintptr_t HexField(const std::string& text, const char* key) {
  const std::string at = std::string("\"") + key + "\":\"0x";
  const std::size_t pos = text.find(at);
  if (pos == std::string::npos) return 0;
  return static_cast<std::uintptr_t>(std::strtoull(text.c_str() + pos + at.size(), nullptr, 16));
}

// The first eight gated answers of each gate are logged at Info with the gate's name, its running count, the caller's return address (inside the gate's own
// range) and the real answer.
void TheAnsweredLinesNameTheGateTheCountAndTheCaller() {
  Begin();
  Build();
  CombatMapLoaded();
  QCHECK(Installed(AllLibs()));
  for (int i = 0; i < 12; ++i) {
    PartyCallerPerm(PartySlot());
    HeadCallerBots(HeadSlot());
  }
  const std::vector<Line> answered = Lines("combat_gates_answered");
  QCHECK(answered.size() == 16U);
  int perm = 0, bots = 0;
  for (const Line& l : answered) {
    QCHECK(l.level == sentinel::LogLevel::kInfo);
    const bool isPerm = Has(l.text, "\"gate\":\"perm\"");
    const bool isBots = Has(l.text, "\"gate\":\"bots\"");
    QCHECK(isPerm != isBots);
    int& count = isPerm ? perm : bots;
    ++count;
    const std::uintptr_t ret = HexField(l.text, "return_address");
    const std::uintptr_t lo = reinterpret_cast<std::uintptr_t>(isPerm ? __start_gate_perm : __start_gate_bots);
    const std::uintptr_t hi = reinterpret_cast<std::uintptr_t>(isPerm ? __stop_gate_perm : __stop_gate_bots);
    QCHECK(ret >= lo && ret < hi);
    QCHECK(Has(l.text, "\"real_answer\":3"));
    QCHECK(Has(l.text, "\"n\":" + std::to_string(count)));
  }
  QCHECK(perm == 8 && bots == 8);
  QCHECK(hg::CurrentStats().partyGated == 12 && hg::CurrentStats().botsGated == 12);
  End();
}

// ---- the Bind's frame and the library policy ------------------------------------------------------------------------------------------
// The host entry passes no Bind frame, so these call the continuation themselves: the entry's sp (a frame, 16-byte aligned, the saved return address at
// sp + 0x18) and a return address inside the link range. The fake dladdr names the library the saved address is in.

struct alignas(16) BindFrame {
  std::uintptr_t words[8];
};
constexpr std::uintptr_t kSavedLr = 0x1000;  // any address: the fake dladdr names all of them

std::int32_t LinkCall(const char* lib, bool misaligned = false) {
  static BindFrame frame;
  for (std::uintptr_t& w : frame.words) w = 0;
  frame.words[3] = kSavedLr;  // sp + 0x18
  frame.words[4] = kSavedLr;  // what a frame read from sp + 8 would find
  const std::string path = std::string("/data/app/") + lib;
  g_fakeLibrary = path.c_str();
  const std::uintptr_t sp = reinterpret_cast<std::uintptr_t>(&frame) + (misaligned ? 8 : 0);
  const std::int32_t r = nevr_combat_headset_dispatch(sp, reinterpret_cast<std::uintptr_t>(__start_gate_link));
  g_fakeLibrary = nullptr;
  return r;
}
bool Answers(const char* spec, const char* lib) {
  hg::SetLinkGateLibs(spec);
  const std::int32_t r = LinkCall(lib);
  QCHECK(r == hg::kLinkHeadset || r == kRealKind);
  return r == hg::kLinkHeadset;
}

void TheBindsCallerIsReadFromItsFrame() {
  Begin();
  Build();
  CombatMapLoaded();
  QCHECK(Installed(AllLibs()));
  hg::SetLinkGateLibs("libA.so");
  QCHECK(LinkCall("libA.so") == hg::kLinkHeadset);                  // the saved return address at sp + 0x18 names the library
  QCHECK(LinkCall("libB.so") == kRealKind);                         // another library: the real answer
  QCHECK(LinkCall("libA.so", /*misaligned=*/true) == kRealKind);    // a frame that is not 16-byte aligned names nothing
  const std::vector<Line> binds = Lines("combat_gates_headset_bind");
  QCHECK(binds.size() == 3U);
  if (binds.size() == 3U) {
    QCHECK(binds[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(binds[0].text, "\"lib\":\"libA.so\"") && Has(binds[0].text, "\"answered\":\"link\"") && Has(binds[0].text, "\"real_answer\":3") &&
           Has(binds[0].text, "\"calls\":1"));  // the file name, not the directory
    QCHECK(Has(binds[1].text, "\"lib\":\"libB.so\"") && Has(binds[1].text, "\"answered\":\"real\"") && Has(binds[1].text, "\"real_answer\":3"));
    QCHECK(Has(binds[2].text, "\"lib\":\"unknown\"") && Has(binds[2].text, "\"answered\":\"real\""));
  }
  End();
}

void TheLinkPolicyAnswersTheLibrariesItNames() {
  Begin();
  Build();
  CombatMapLoaded();
  QCHECK(Installed(AllLibs()));
  // all, none, empty
  QCHECK(Answers("all", "libA.so"));
  QCHECK(!Answers("none", "libA.so"));
  QCHECK(!Answers("", "libA.so"));
  // a list: comma, space, tab and any mix of them separate; a name is a part of the library's file name, not a prefix or all of it
  QCHECK(Answers("libA.so,libB.so", "libA.so") && Answers("libA.so,libB.so", "libB.so") && !Answers("libA.so,libB.so", "libC.so"));
  QCHECK(Answers("libA.so libB.so", "libA.so") && Answers("libA.so libB.so", "libB.so") && !Answers("libA.so libB.so", "libC.so"));
  QCHECK(Answers("libA.so\tlibB.so", "libA.so") && Answers("libA.so\tlibB.so", "libB.so") && !Answers("libA.so\tlibB.so", "libC.so"));
  QCHECK(Answers(" ,\t libA.so ,, libB.so \t", "libA.so") && Answers(" ,\t libA.so ,, libB.so \t", "libB.so") && !Answers(" ,\t libA.so ,, libB.so \t", "libC.so"));
  QCHECK(Answers("libA.so, ", "libA.so") && !Answers("libA.so, ", "libZ.so"));  // a trailing separator is no empty name that matches everything
  QCHECK(Answers("libA", "libA.so") && Answers("A.so", "libA.so") && !Answers("A.so", "libB.so"));
  QCHECK(!Answers("libA.so", "libA.sx"));
  // all wins over names, wherever it stands; none is dropped from a list
  QCHECK(Answers("all libA.so", "libC.so") && Answers("libA.so all", "libC.so"));
  QCHECK(Answers("none libA.so", "libA.so") && !Answers("none libA.so", "libnone.so") && !Answers("none", "libnone.so"));
  // `all` and `none` are whole words: a longer word is a name
  QCHECK(!Answers("allx", "libfoo.so") && Answers("allx", "liballx.so"));
  QCHECK(!Answers("nonex", "libfoo.so") && Answers("nonex", "libnonex.so"));
  // a later list replaces an earlier one, and the name's end is where its text ended (the row holds the stale tail of the longer one)
  QCHECK(Answers("libVERYLONGNAMEEEEEEEEEE.so", "libVERYLONGNAMEEEEEEEEEE.so"));
  QCHECK(Answers("libA.so", "libA.so") && !Answers("libA.so", "libA.sx"));
  // 32 names fit, a 33rd is dropped; the first and the 32nd answer
  std::string many;
  for (int i = 1; i <= 33; ++i) many += (i > 1 ? " x" : "x") + std::string(i < 10 ? "0" : "") + std::to_string(i);
  QCHECK(Answers(many.c_str(), "libx01.so") && Answers(many.c_str(), "libx32.so") && !Answers(many.c_str(), "libx33.so"));
  // a name of 47 characters fits (the longest file name the gate keeps), and answers a library of that name
  const std::string longest(47, 'q');
  QCHECK(Answers(longest.c_str(), longest.c_str()));
  QCHECK(hg::LinkGateLibsSeen() < 64);
  End();
}

// Each library's first bind and its 64th are logged, once each; another library has its own count.
void TheMenuGateLogsEachLibraryOnceAndItsSixtyFourthBind() {
  Begin();
  Build();
  CombatMapLoaded();
  QCHECK(Installed(AllLibs()));
  for (int i = 1; i <= 63; ++i) QCHECK(LinkCall("libA.so") == hg::kLinkHeadset);
  QCHECK(Lines("combat_gates_headset_bind").size() == 1U);
  QCHECK(LinkCall("libA.so") == hg::kLinkHeadset);  // the 64th
  std::vector<Line> binds = Lines("combat_gates_headset_bind");
  QCHECK(binds.size() == 2U);
  if (binds.size() == 2U) QCHECK(Has(binds[1].text, "\"lib\":\"libA.so\"") && Has(binds[1].text, "\"calls\":64"));
  for (int i = 65; i <= 130; ++i) QCHECK(LinkCall("libA.so") == hg::kLinkHeadset);
  QCHECK(Lines("combat_gates_headset_bind").size() == 2U);  // neither the 65th nor the 128th
  QCHECK(LinkCall("libB.so") == hg::kLinkHeadset);
  binds = Lines("combat_gates_headset_bind");
  QCHECK(binds.size() == 3U);
  if (binds.size() == 3U) QCHECK(Has(binds[2].text, "\"lib\":\"libB.so\"") && Has(binds[2].text, "\"calls\":1"));
  QCHECK(hg::LinkGateLibsSeen() == 2);
  hg::SetLinkGateLibs("libA.so");
  QCHECK(LinkCall("libC.so") == kRealKind);  // a library the policy leaves out is named in the line, and said not to be answered
  binds = Lines("combat_gates_headset_bind");
  QCHECK(binds.size() == 4U);
  if (binds.size() == 4U) QCHECK(Has(binds[3].text, "\"lib\":\"libC.so\"") && Has(binds[3].text, "\"answered\":\"real\""));
  End();
}

// A library slot that an earlier run used starts its count at 1 again.
void AReusedLibrarySlotStartsItsCountAtOne() {
  Begin();
  Build();
  CombatMapLoaded();
  QCHECK(Installed(AllLibs()));
  QCHECK(LinkCall("libA.so") == hg::kLinkHeadset);
  const std::vector<Line> binds = Lines("combat_gates_headset_bind");
  QCHECK(binds.size() == 1U);
  if (binds.size() == 1U) QCHECK(Has(binds[0].text, "\"calls\":1"));
  End();
}

// Sixty-four libraries are remembered; one more is answered and has no count and no line.
void SixtyFourLibrariesAreRemembered() {
  Begin();
  Build();
  CombatMapLoaded();
  QCHECK(Installed(AllLibs()));
  for (int i = 0; i < 70; ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "lib%02d.so", i);
    QCHECK(LinkCall(name) == hg::kLinkHeadset);
  }
  QCHECK(hg::LinkGateLibsSeen() == 64);
  QCHECK(Lines("combat_gates_headset_bind").size() == 64U);
  QCHECK(LinkCall("lib00.so") == hg::kLinkHeadset);  // one that is in the table counts on, silently
  QCHECK(Lines("combat_gates_headset_bind").size() == 64U);
  End();
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
  TheBotGateNeedsLevelnameInstalledInTheGame();
  TheBotGateSeesTheRuntimesLevelname();
  TheRangeIsHalfOpen();
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
  AForeignHeadsetSlotRefusesBothGatesOnIt();
  ABothForeignSlotsAreRefusedAndNothingIsPublished();
  AFullTickTableRefusesTheBotGateAlone();
  ABuildWithNoIdIsRefusedBeforeAnySlot();
  AnInstallWithNoLookupIsRefused();
  AnUnreadablePinnedWordRefusesItsGateAndSaysSo();
  PinnedWordsInReadOnlyCodeAreRead();
  ACallBeforeTheInstallGetsZeroFromTheMissingGamesFunction();
  ThePartyCallReachesTheGamesFunctionWithItsArguments();
  TheAnsweredLinesNameTheGateTheCountAndTheCaller();
  TheBindsCallerIsReadFromItsFrame();
  TheLinkPolicyAnswersTheLibrariesItNames();
  TheMenuGateLogsEachLibraryOnceAndItsSixtyFourthBind();
  AReusedLibrarySlotStartsItsCountAtOne();
  SixtyFourLibrariesAreRemembered();
  if (quest_test::Failures() == 0) std::printf("headset_gates_test: all checks pass\n");
  return quest_test::Failures() == 0 ? 0 : 1;
}
