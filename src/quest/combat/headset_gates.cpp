#include "quest/combat/headset_gates.h"

#include <dlfcn.h>
#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/combat_gates.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/mapping_perms.h"

// The two entries and the two functions each continues in (callback_thunk.h rule 7). The entry is what the slot holds. On arm64 it is a
// top-level asm() block below; on the host (x86-64) it is a C++ function that reads its own return address, which is all a host test can
// stand in for. The continuation is a C++ function the entry reaches with the caller's return address (x30) untouched.
extern "C" {
__attribute__((visibility("hidden"))) std::int32_t nevr_combat_headset_tramp();
__attribute__((visibility("hidden"), noinline)) std::int32_t nevr_combat_headset_dispatch(std::uintptr_t bindSp, std::uintptr_t ret);
__attribute__((visibility("hidden"))) std::int32_t nevr_combat_party_tramp(void* self, std::uint32_t index);
__attribute__((visibility("hidden"), noinline)) std::int32_t nevr_combat_party_dispatch(void* self, std::uint32_t index, std::uintptr_t ret);
}

namespace nevr_quest::combat::headset_gates {

namespace {

namespace pinned = sentinel::pinned;

sentinel::GotHook g_partyHook;
sentinel::GotHook g_headsetHook;

// The word each entry calls in place of the game's function: the manager publishes it before the slot changes and again when the hook it
// calls leaves (hook_install.h), and the continuation reads it with an acquire load at every call.
void* g_partyNext = nullptr;
void* g_headsetNext = nullptr;

std::atomic<bool> g_ran{false};
std::atomic<std::uintptr_t> g_permLo{0}, g_permHi{0}, g_botsLo{0}, g_botsHi{0}, g_linkLo{0}, g_linkHi{0};
std::atomic<std::uintptr_t> g_game{0};
// Where the game keeps LoadLevelName (CR15Game vtable word) and what stock holds there: absolute, published by the install; 0 when the bot gate is off.
std::atomic<std::uintptr_t> g_levelNameSlot{0}, g_levelNameStock{0};
std::atomic<std::uint32_t> g_partyCalls{0}, g_partyGated{0}, g_headsetCalls{0}, g_botsGated{0}, g_botsScopedOut{0}, g_linkGated{0};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};
constexpr std::uint32_t kLogFirst = 8;

// ---- the link gate's library policy ----
enum class LinkMode : int { kAll = 0, kNone = 1, kList = 2 };
constexpr int kMaxFragments = 32;
constexpr std::size_t kFragmentLen = 48;  // a script library's file name is 20 characters
constexpr int kMaxSeenLibs = 64;
std::atomic<int> g_linkMode{static_cast<int>(LinkMode::kAll)};
char g_fragments[kMaxFragments][kFragmentLen];
std::atomic<int> g_fragmentCount{0};

struct SeenLib {
  char name[kFragmentLen];
  std::uint32_t calls;
};
pthread_mutex_t g_seenMutex = PTHREAD_MUTEX_INITIALIZER;  // a std::mutex would register a destructor at load time
SeenLib g_seen[kMaxSeenLibs];
std::uint32_t g_seenCount = 0;
std::atomic<std::uint32_t> g_seenPublished{0};

bool Within(std::uintptr_t addr, const std::atomic<std::uintptr_t>& lo, const std::atomic<std::uintptr_t>& hi) noexcept {
  return InRange(addr, lo.load(std::memory_order_acquire), hi.load(std::memory_order_acquire));
}

void LogAnswered(const char* gate, std::uint32_t n, std::uintptr_t ret, std::int32_t realAnswer) noexcept {
  char at[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_gates_answered",
                      {{"gate", gate}, {"n", n}, {"return_address", sentinel::HexString(at, ret)}, {"real_answer", realAnswer},
                       {"msg", "the game's Quest-only branch is skipped for this call: the gate answered 'not a Quest'"}});
}

const char* ScopeReason(level::Kind kind) noexcept { return kind == level::Kind::kEchoArena ? "echo_arena" : "other_level"; }

// Whether levelname's hook is on the game's LoadLevelName slot (the full-size global_mp loads): the word is anything but the stock function. An
// unreadable word, or a gate whose slot is unpublished, is "not installed".
bool LevelNameInstalled() noexcept {
  const std::uintptr_t slot = g_levelNameSlot.load(std::memory_order_acquire);
  if (slot == 0) return false;
  bool ok = true;
  const auto word = SafeRead<std::uintptr_t>(slot, &ok);
  return ok && word != 0 && word != g_levelNameStock.load(std::memory_order_acquire);
}

// Whether the combat bot can be the answer: levelname is installed, and a combat map or the combat lobby is loaded in the game the tick saw. The
// reason is a token for the line when not.
bool BotsInScope(const char** reason) noexcept {
  if (!LevelNameInstalled()) {
    *reason = "levelname_not_installed";
    return false;
  }
  const std::uintptr_t game = g_game.load(std::memory_order_acquire);
  if (game == 0) {
    *reason = "no_game";
    return false;
  }
  std::uint64_t levelId = 0;
  const level::Kind kind = level::Classify(game, /*lobby=*/true, &levelId);
  *reason = ScopeReason(kind);
  return kind == level::Kind::kCombatOrLobby;
}

void LogScopedOut(std::uint32_t n, const char* reason) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_gates_scoped_out",
                      {{"gate", "bots"}, {"n", n}, {"reason", reason},
                       {"msg", "the bot-body gate left the real answer: levelname is not installed or no combat map or combat lobby is loaded, so the game picks the bot body itself"}});
}

// The Bind (BindExpression<CR15HeadsetTypeExpression>) opens with `sub sp,sp,#0x20; stp x19,x30,[sp,#0x10]` and calls CVR::HeadsetType from that
// frame, so the entry's sp is the Bind's sp and its saved return address is at sp + 0x18: the code of the script library (lib<hash>.so) that
// called the Bind. 0 when there is no frame (a plain call on the host).
std::uintptr_t CallerOfBind(std::uintptr_t bindSp) noexcept {
  if (bindSp == 0 || (bindSp & 0xf) != 0) return 0;
  return *reinterpret_cast<const std::uintptr_t*>(bindSp + kBindSavedLrOffset);
}

bool LinkLibAnswered(const char* lib, bool known) noexcept {
  switch (static_cast<LinkMode>(g_linkMode.load(std::memory_order_relaxed))) {
    case LinkMode::kAll: return true;
    case LinkMode::kNone: return false;
    case LinkMode::kList: {
      if (!known) return false;
      const int count = g_fragmentCount.load(std::memory_order_relaxed);
      for (int i = 0; i < count; ++i) {
        if (std::strstr(lib, g_fragments[i]) != nullptr) return true;
      }
      return false;
    }
  }
  return false;
}

// One line the first time a library binds the expression, and again at its 64th bind.
void NoteLinkLib(const char* lib, bool known, bool answered, std::int32_t realAnswer) noexcept {
  std::uint32_t calls = 0;
  bool first = false;
  {
    pthread_mutex_lock(&g_seenMutex);
    SeenLib* hit = nullptr;
    for (std::uint32_t i = 0; i < g_seenCount; ++i) {
      if (std::strcmp(g_seen[i].name, lib) == 0) hit = &g_seen[i];
    }
    if (hit == nullptr && g_seenCount < static_cast<std::uint32_t>(kMaxSeenLibs)) {
      hit = &g_seen[g_seenCount++];
      std::snprintf(hit->name, sizeof(hit->name), "%s", lib);
      hit->calls = 0;
      first = true;
      g_seenPublished.store(g_seenCount, std::memory_order_relaxed);
    }
    if (hit != nullptr) calls = ++hit->calls;
    pthread_mutex_unlock(&g_seenMutex);
  }
  if (!first && calls != 64) return;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_gates_headset_bind",
                      {{"lib", known ? lib : "unknown"}, {"answered", answered ? "link" : "real"}, {"real_answer", realAnswer}, {"calls", calls}});
}

std::int32_t NextParty(void* self, std::uint32_t index) noexcept {
  const auto next = reinterpret_cast<std::int32_t (*)(void*, std::uint32_t)>(__atomic_load_n(&g_partyNext, __ATOMIC_ACQUIRE));
  return next != nullptr ? next(self, index) : 0;
}
std::int32_t NextHeadset() noexcept {
  const auto next = reinterpret_cast<std::int32_t (*)()>(__atomic_load_n(&g_headsetNext, __ATOMIC_ACQUIRE));
  return next != nullptr ? next() : 0;
}

// `ret`: the return address of the call to PartyMemberHeadsetType, which the entry took from x30. The game's function runs first; the lines are
// written after it returned, never while a game call is on the stack above them (callback_thunk.h rule 1).
std::int32_t PartyCommon(void* self, std::uint32_t index, std::uintptr_t ret) noexcept {
  const std::int32_t answer = NextParty(self, index);
  g_partyCalls.fetch_add(1, std::memory_order_relaxed);
  if (!Within(ret, g_permLo, g_permHi)) return answer;
  const std::uint32_t n = g_partyGated.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= kLogFirst) LogAnswered("perm", n, ret, answer);
  return kNotQuest;
}

// `ret`: the caller's return address. `bindSp`: the entry's sp (the caller's own sp), 0 where there is none.
std::int32_t HeadsetCommon(std::uintptr_t ret, std::uintptr_t bindSp) noexcept {
  const std::int32_t answer = NextHeadset();
  g_headsetCalls.fetch_add(1, std::memory_order_relaxed);
  if (Within(ret, g_botsLo, g_botsHi)) {
    const char* reason = "";
    if (BotsInScope(&reason)) {
      const std::uint32_t n = g_botsGated.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= kLogFirst) LogAnswered("bots", n, ret, answer);
      return kNotQuest;
    }
    const std::uint32_t n = g_botsScopedOut.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= kLogFirst) LogScopedOut(n, reason);
    return answer;
  }
  if (Within(ret, g_linkLo, g_linkHi)) {
    char lib[kFragmentLen] = "";
    const bool known = LibraryName(CallerOfBind(bindSp), lib, sizeof(lib));
    const bool answered = LinkLibAnswered(lib, known);
    NoteLinkLib(lib, known, answered, answer);
    if (!answered) return answer;
    g_linkGated.fetch_add(1, std::memory_order_relaxed);
    return kLinkHeadset;
  }
  return answer;
}

NEVR_ASM_ENTRY_RECORD(kPartyEntry, ::nevr_combat_party_tramp, ::nevr_combat_party_dispatch);
NEVR_ASM_ENTRY_RECORD(kHeadsetEntry, ::nevr_combat_headset_tramp, ::nevr_combat_headset_dispatch);

template <typename T>
bool ReadGame(std::uintptr_t address, T* out) noexcept {
  if (!sentinel::MappingsAllow(address, sizeof(T), /*write=*/false)) return false;
  std::memcpy(out, reinterpret_cast<const void*>(address), sizeof(T));
  return true;
}

// The first pinned word of `gate` that is not the live image's; nullptr when every word of the gate holds.
const Word* FirstWordMismatch(std::uintptr_t base, Gate gate, std::uint32_t* have, bool* readable) noexcept {
  for (const Word& pin : kWords) {
    if (pin.gate != gate) continue;
    *have = 0;
    *readable = ReadGame(base + pin.vaddr, have);
    if (!*readable || *have != pin.word) return &pin;
  }
  return nullptr;
}

const char* GateName(Gate gate) noexcept {
  return gate == Gate::kPerm ? "perm" : gate == Gate::kBots ? "bots" : "link";
}

// A gate's pinned words: true when they hold. A difference is one Warning and the gate is not installed.
bool WordsHold(std::uintptr_t base, Gate gate) noexcept {
  std::uint32_t have = 0;
  bool readable = false;
  const Word* const mismatch = FirstWordMismatch(base, gate, &have, &readable);
  if (mismatch == nullptr) return true;
  char at[19], haveText[19], wantText[19];
  sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_gates_hook",
                      {{"gate", GateName(gate)}, {"status", "pinned_word_mismatch"}, {"vaddr", sentinel::HexString(at, mismatch->vaddr)},
                       {"have", readable ? sentinel::HexString(haveText, have) : "unreadable"},
                       {"want", sentinel::HexString(wantText, mismatch->word)},
                       {"consequence", "the gate is not installed: its pinned code is not the pinned build's"}});
  return false;
}

void WarnLegacySwitches() noexcept {
  struct Legacy {
    const char* file;
    const char* key;
  };
  static constexpr Legacy kLegacy[] = {{"gate_perm.off", "combat.gate_perm: false"},
                                       {"gate_bots.off", "combat.gate_bots: false"},
                                       {"gate_libs.txt", "combat.gate_link_libs"}};
  for (const Legacy& entry : kLegacy) {
    if (!legacy::Present(entry.file)) continue;
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_gates_legacy_switch",
                        {{"file", entry.file}, {"replaced_by", entry.key},
                         {"msg", "the plugin's switch file is no longer read and changes nothing; set the combat key in config.yaml instead"}});
  }
}

const char* StatusOf(sentinel::GotStatus status) noexcept {
  return status == sentinel::GotStatus::kOk ? "installed" : sentinel::GotStatusName(status);
}

ModuleResult Refused(const char* token) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kError, "combat_gates_install",
                      {{"result", "refused"}, {"reason", token}, {"gate_perm", "not_tried"}, {"gate_bots", "not_tried"}, {"gate_link", "not_tried"},
                       {"consequence", "no headset gate is installed: a Quest keeps the game's own permissions, bot body and menus"}});
  return {false, token};
}

}  // namespace

bool InRange(std::uintptr_t addr, std::uintptr_t lo, std::uintptr_t hi) noexcept { return addr - lo < hi - lo; }

bool LibraryName(std::uintptr_t addr, char* out, std::size_t n) noexcept {
  Dl_info di;
  if (addr == 0 || dladdr(reinterpret_cast<void*>(addr), &di) == 0 || di.dli_fname == nullptr) return false;
  const char* slash = std::strrchr(di.dli_fname, '/');
  std::snprintf(out, n, "%s", slash != nullptr ? slash + 1 : di.dli_fname);
  return out[0] != 0;
}

void SetLinkGateLibs(const char* spec) noexcept {
  g_fragmentCount.store(0, std::memory_order_relaxed);
  g_linkMode.store(static_cast<int>(LinkMode::kAll), std::memory_order_relaxed);
  if (spec == nullptr) return;
  int count = 0;
  bool all = false;
  for (const char* p = spec; *p != 0;) {
    while (*p == ' ' || *p == ',' || *p == '\t') ++p;
    const char* start = p;
    while (*p != 0 && *p != ' ' && *p != ',' && *p != '\t') ++p;
    const std::size_t len = static_cast<std::size_t>(p - start);
    if (len == 0) continue;
    if (len == 3 && std::strncmp(start, "all", 3) == 0) {
      all = true;
    } else if (len == 4 && std::strncmp(start, "none", 4) == 0) {
      continue;
    } else if (count < kMaxFragments && len < kFragmentLen) {
      std::memcpy(g_fragments[count], start, len);
      g_fragments[count][len] = 0;
      ++count;
    }
  }
  g_fragmentCount.store(count, std::memory_order_relaxed);
  g_linkMode.store(static_cast<int>(all ? LinkMode::kAll : count > 0 ? LinkMode::kList : LinkMode::kNone), std::memory_order_relaxed);
}

std::uint32_t LinkGateLibsSeen() noexcept { return g_seenPublished.load(std::memory_order_relaxed); }

void Tick(std::uintptr_t game) noexcept {
  if (g_game.load(std::memory_order_relaxed) != game) g_game.store(game, std::memory_order_release);
}

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const Switches& switches) noexcept {
  const gates::Choice choice = gates::Decide({switches.perm, switches.bots, switches.link});
  if (!choice.perm && !choice.bots && !choice.link) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_gates_install",
                        {{"result", "off"}, {"reason", "off_config"}, {"gate_perm", choice.permReason}, {"gate_bots", choice.botsReason},
                         {"gate_link", choice.linkReason},
                         {"consequence", "no headset gate is asked for: a Quest keeps the game's own permissions, bot body and menus"}});
    return {false, "off", true};
  }
  if (g_ran.exchange(true, std::memory_order_acq_rel)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_gates_install",
                        {{"result", "refused"}, {"reason", "already_ran"}, {"consequence", "a second step installs nothing"}});
    return {false, "already_ran"};
  }
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(pinned::kLibR15, &image)) return Refused("module_not_loaded");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id");
  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch");
  const std::uintptr_t base = image.base;
  WarnLegacySwitches();

  const char* spec = switches.linkLibs != nullptr ? switches.linkLibs : kDefaultLinkLibs;
  if (choice.link) {
    SetLinkGateLibs(spec);
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_gates_link_policy",
                        {{"libs", spec[0] != 0 ? spec : "none"}, {"source", switches.linkLibs != nullptr ? "config" : "default"}});
  }

  // Each gate's status for the install line: the reason it is off, or what became of it.
  const char* permStatus = choice.perm ? "pending" : choice.permReason;
  const char* botsStatus = choice.bots ? "pending" : choice.botsReason;
  const char* linkStatus = choice.link ? "pending" : choice.linkReason;
  bool perm = choice.perm, bots = choice.bots, link = choice.link;
  if (perm && !WordsHold(base, Gate::kPerm)) {
    perm = false;
    permStatus = "pinned_word_mismatch";
  }
  if (bots && !WordsHold(base, Gate::kBots)) {
    bots = false;
    botsStatus = "pinned_word_mismatch";
  }
  if (link && !WordsHold(base, Gate::kLink)) {
    link = false;
    linkStatus = "pinned_word_mismatch";
  }
  // The bot gate reads the loaded levels of the game the tick sees: with no tick it could only ever answer the real value.
  if (bots && !tick::Add(&Tick)) {
    bots = false;
    botsStatus = "tick_table_full";
  }

  if (perm) {
    g_permLo.store(base + pins.permLo, std::memory_order_release);
    g_permHi.store(base + pins.permHi, std::memory_order_release);
    const sentinel::GotStatus got =
        sentinel::InstallAsmEntry(g_partyHook, PartyTarget(base, pins), kPartyEntry, &g_partyNext, lookup);
    permStatus = StatusOf(got);
    if (got != sentinel::GotStatus::kOk) {
      g_permLo.store(0, std::memory_order_release);
      g_permHi.store(0, std::memory_order_release);
      perm = false;
    }
    sentinel::LogFields(got == sentinel::GotStatus::kOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_gates_hook",
                        {{"slot", "party"}, {"gates", "perm"}, {"symbol", kPartySymbol}, {"status", permStatus}});
  }
  if (bots || link) {
    if (bots) {
      g_levelNameStock.store(base + pins.levelNameStock, std::memory_order_release);
      g_levelNameSlot.store(base + pins.levelNameSlot, std::memory_order_release);
      g_botsLo.store(base + pins.botsLo, std::memory_order_release);
      g_botsHi.store(base + pins.botsHi, std::memory_order_release);
    }
    if (link) {
      g_linkLo.store(base + pins.linkLo, std::memory_order_release);
      g_linkHi.store(base + pins.linkHi, std::memory_order_release);
    }
    const sentinel::GotStatus got =
        sentinel::InstallAsmEntry(g_headsetHook, HeadsetTarget(base, pins), kHeadsetEntry, &g_headsetNext, lookup);
    const char* const status = StatusOf(got);
    if (got != sentinel::GotStatus::kOk) {
      g_botsLo.store(0, std::memory_order_release);
      g_botsHi.store(0, std::memory_order_release);
      g_levelNameSlot.store(0, std::memory_order_release);
      g_linkLo.store(0, std::memory_order_release);
      g_linkHi.store(0, std::memory_order_release);
    }
    if (bots) botsStatus = status;
    if (link) linkStatus = status;
    sentinel::LogFields(got == sentinel::GotStatus::kOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_gates_hook",
                        {{"slot", "headset"}, {"gates", bots && link ? "bots,link" : bots ? "bots" : "link"}, {"symbol", kHeadsetSymbol},
                         {"status", status}});
    if (got != sentinel::GotStatus::kOk) {
      bots = false;
      link = false;
    }
  }

  const std::size_t asked = (choice.perm ? 1U : 0U) + (choice.bots ? 1U : 0U) + (choice.link ? 1U : 0U);
  const std::size_t done = (perm ? 1U : 0U) + (bots ? 1U : 0U) + (link ? 1U : 0U);
  const char* const result = done == asked ? "installed" : done == 0 ? "refused" : "partial";
  sentinel::LogFields(done == asked ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_gates_install",
                      {{"result", result}, {"reason", done == asked ? "ok" : "gate_refused"}, {"gate_perm", perm ? "installed" : permStatus},
                       {"gate_bots", bots ? "installed" : botsStatus}, {"gate_link", link ? "installed" : linkStatus},
                       {"consequence", done == asked ? "a Quest gets combat permission in a party, the combat bot body on a combat map and the combat menus"
                                                     : "a gate that is not installed leaves the game's own behaviour for its part"}});
  return {done == asked, result};
}

ModuleResult Install(const Switches& switches) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  const Pins* const pins = g_pinsOverride.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, pins != nullptr ? *pins : kPins, switches);
}

Stats CurrentStats() noexcept {
  return {g_partyCalls.load(), g_partyGated.load(), g_headsetCalls.load(), g_botsGated.load(), g_botsScopedOut.load(), g_linkGated.load()};
}

Ranges RangesForTest() noexcept {
  return {g_permLo.load(), g_permHi.load(), g_botsLo.load(), g_botsHi.load(), g_linkLo.load(), g_linkHi.load()};
}

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }
void SetPinsForTest(const Pins* pins) noexcept { g_pinsOverride.store(pins, std::memory_order_release); }

void ResetForTest() noexcept {
  g_partyHook.Forget();
  g_headsetHook.Forget();
  __atomic_store_n(&g_partyNext, static_cast<void*>(nullptr), __ATOMIC_RELEASE);
  __atomic_store_n(&g_headsetNext, static_cast<void*>(nullptr), __ATOMIC_RELEASE);
  g_ran.store(false);
  for (std::atomic<std::uintptr_t>* word : {&g_permLo, &g_permHi, &g_botsLo, &g_botsHi, &g_linkLo, &g_linkHi, &g_game, &g_levelNameSlot, &g_levelNameStock}) word->store(0);
  for (std::atomic<std::uint32_t>* counter :
       {&g_partyCalls, &g_partyGated, &g_headsetCalls, &g_botsGated, &g_botsScopedOut, &g_linkGated, &g_seenPublished}) {
    counter->store(0);
  }
  pthread_mutex_lock(&g_seenMutex);
  g_seenCount = 0;
  pthread_mutex_unlock(&g_seenMutex);
  SetLinkGateLibs(nullptr);
  g_lookup.store(nullptr);
  g_pinsOverride.store(nullptr);
}

}  // namespace nevr_quest::combat::headset_gates

// ---- the entries and their continuations ---------------------------------------------------------------------------------------

namespace hg = nevr_quest::combat::headset_gates;

extern "C" std::int32_t nevr_combat_headset_dispatch(std::uintptr_t bindSp, std::uintptr_t ret) { return hg::HeadsetCommon(ret, bindSp); }
extern "C" std::int32_t nevr_combat_party_dispatch(void* self, std::uint32_t index, std::uintptr_t ret) {
  return hg::PartyCommon(self, index, ret);
}

#if defined(__aarch64__)
#include "quest/combat/headset_gates_entry.inc"
#else
extern "C" __attribute__((visibility("hidden"), noinline)) std::int32_t nevr_combat_headset_tramp() {
  return nevr_combat_headset_dispatch(0, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
}
extern "C" __attribute__((visibility("hidden"), noinline)) std::int32_t nevr_combat_party_tramp(void* self, std::uint32_t index) {
  return nevr_combat_party_dispatch(self, index, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
}
#endif
