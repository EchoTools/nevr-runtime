#include "quest/combat/wallfire.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "quest/combat/bvh.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::wallfire {

namespace {

constexpr std::uint64_t kLogFirst = 16;  // blocked shots logged one by one; after that at powers of two

using OccludedFn = bool (*)(std::uintptr_t, const float*, const float*);

sentinel::GotHook g_hook;
std::atomic<bool> g_ran{false};
std::atomic<std::uintptr_t> g_base{0};
std::atomic<std::uintptr_t> g_site{0};          // absolute: FireGunLocal's return address
std::atomic<std::uintptr_t> g_occluded{0};      // absolute: CncaGameSpace::StaticRaycastOccluded
std::atomic<std::uintptr_t> g_gameOccluded{0};  // absolute: what CncaGame's vtable +0x278 holds
std::atomic<std::uintptr_t> g_lastReturn{0};
std::atomic<bool> g_warnedGame{false};
std::atomic<std::uint64_t> g_atSite{0}, g_checked{0}, g_blocked{0}, g_nan{0}, g_game{0};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};

// CncaGameSpace -> CncaGame -> vtable -> [+0x278] is the pinned CncaGame::StaticRaycastOccluded; false (the game's answer) otherwise.
bool GameIsPinned(std::uintptr_t gs) noexcept {
  std::uintptr_t game = 0, vptr = 0, fn = 0;
  // Three SafeReads (about 1.5 us a shot): the game's QueryCollision never reads `this`, so nothing the game does on this path backs these words. A null
  // pointer of the chain fails the next read.
  if (!SafeRead(gs + kGsGame, &game, sizeof(game))) return false;
  if (!SafeRead(game, &vptr, sizeof(vptr))) return false;
  return SafeRead(vptr + kGameVtblSlot, &fn, sizeof(fn)) && fn == g_gameOccluded.load(std::memory_order_relaxed);
}

void LogBlocked(std::uint64_t n, const float* s) noexcept {
  char seg[160];
  std::snprintf(seg, sizeof(seg), "%.2f %.2f %.2f -> %.2f %.2f %.2f", static_cast<double>(s[0]), static_cast<double>(s[1]),
                static_cast<double>(s[2]), static_cast<double>(s[3]), static_cast<double>(s[4]), static_cast<double>(s[5]));
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_wallfire_blocked",
                      {{"blocked", n}, {"checked", g_checked.load(std::memory_order_relaxed)}, {"segment", seg},
                       {"msg", "a shot was refused: the segment from the player to the muzzle crosses the level's static geometry"}});
}

void LogSkippedGame() noexcept {
  sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_wallfire_skipped",
                      {{"reason", "game_chain"},
                       {"msg", "CncaGame's virtual +0x278 is not the pinned function, or a pointer of the chain is null: the game's answer is used"}});
}

// QueryCollision(this = the gamespace, the request, the CPhId): the game's function runs first. `caller` is the instruction after the game's call.
bool OnQuery(QueryThunk::Fn original, const void* caller, std::uintptr_t gs, std::uintptr_t req, std::uint64_t id) noexcept {
  if (!bvh::InCombatOrLobby()) return original(gs, req, id);
  const std::uintptr_t ret = reinterpret_cast<std::uintptr_t>(caller);
  const std::uintptr_t base = g_base.load(std::memory_order_relaxed);
  g_lastReturn.store(ret - base, std::memory_order_relaxed);
  const bool r = original(gs, req, id);
  if (ret != g_site.load(std::memory_order_relaxed) || r || gs == 0) return r;  // another caller, or the game's query already found something
  g_atSite.fetch_add(1, std::memory_order_relaxed);
  float seg[kReqSegFloats];
  std::memcpy(seg, reinterpret_cast<const void*>(req), sizeof(seg));  // the request the game's caller formed
  bool finite = true;
  for (unsigned k = 0; finite && k < kReqSegFloats; ++k) finite = std::isfinite(seg[k]);
  if (!finite) {
    g_nan.fetch_add(1, std::memory_order_relaxed);
    return r;
  }
  if (!GameIsPinned(gs)) {
    g_game.fetch_add(1, std::memory_order_relaxed);
    if (!g_warnedGame.exchange(true, std::memory_order_relaxed)) LogSkippedGame();
    return r;
  }
  g_checked.fetch_add(1, std::memory_order_relaxed);
  if (!reinterpret_cast<OccludedFn>(g_occluded.load(std::memory_order_relaxed))(gs, seg, seg + 3)) return r;
  const std::uint64_t n = g_blocked.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= kLogFirst || (n & (n - 1)) == 0) LogBlocked(n, seg);
  return true;
}

NEVR_HOOK_RECORD(kQueryRecord, QueryThunk, &OnQuery);

void Refused(const char* reason, const char* detail, const char* status, sentinel::LogLevel level) noexcept {
  sentinel::LogFields(level, "combat_wallfire_install",
                      {{"result", "refused"}, {"reason", reason}, {"detail", detail}, {"status", status},
                       {"msg", "the wall-fire check is not installed: nothing was changed, so shots through walls are not refused"}});
}

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept {
  if (legacy::Present("wallfire.off")) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_wallfire_legacy_switch",
                        {{"file", "wallfire.off"}, {"replaced_by", "combat.wallfire: false"},
                         {"msg", "the plugin's kill file is no longer read and changes nothing; set the combat key in config.yaml instead"}});
  }
  if (!on) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_wallfire_install",
                        {{"result", "off"}, {"reason", "off_config"},
                         {"msg", "combat.wallfire is false: shots through walls are not refused"}});
    return {false, "off_config", true};  // off by its key: counted apart on the phase line, neither installed nor failed
  }
  if (g_ran.exchange(true, std::memory_order_acq_rel)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_wallfire_install",
                        {{"result", "refused"}, {"reason", "already_ran"}, {"msg", "a second step installs nothing"}});
    return {false, "already_ran"};
  }
  if (!bvh::TreeInstalled()) {
    Refused("bvh_not_installed", "the static BVH is not answering CBVHTree::Occluded (combat.bvh is false or its install was refused)",
            "not_installed", sentinel::LogLevel::kWarn);
    return {false, "bvh_not_installed"};
  }
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) {
    Refused("module_not_loaded", "libr15", "module_not_loaded", sentinel::LogLevel::kError);
    return {false, "module_not_loaded"};
  }
  const std::uintptr_t base = image.base;
  std::uint32_t word = 0;
  if (!SafeRead(base + pins.site, &word, sizeof(word))) {
    Refused("site_unreadable", "FireGunLocal return site", "unmapped", sentinel::LogLevel::kError);
    return {false, "site_unreadable"};
  }
  if (word != pins.siteWord) {
    char have[19];
    Refused("site_word", sentinel::HexString(have, word), "unexpected_original", sentinel::LogLevel::kError);
    return {false, "site_word"};
  }
  std::uintptr_t slotWord = 0;
  if (!SafeRead(base + pins.occludedSlot, &slotWord, sizeof(slotWord))) {
    Refused("occluded_unreadable", "StaticRaycastOccluded slot", "unmapped", sentinel::LogLevel::kError);
    return {false, "occluded_unreadable"};
  }
  if (slotWord != base + pins.occludedFn) {
    Refused("occluded_slot", "StaticRaycastOccluded slot", "unexpected_original", sentinel::LogLevel::kError);
    return {false, "occluded_slot"};
  }
  g_base.store(base, std::memory_order_release);
  g_site.store(base + pins.site, std::memory_order_release);
  g_occluded.store(base + pins.occludedFn, std::memory_order_release);
  g_gameOccluded.store(base + pins.gameOccluded, std::memory_order_release);
  QueryThunk::Arm(kQueryRecord);
  const sentinel::GotStatus got = sentinel::InstallThunk<QueryThunk>(g_hook, QueryTarget(base, pins), lookup);
  if (got != sentinel::GotStatus::kOk) {
    Refused("hook_refused", "CncaGameSpace::QueryCollision", sentinel::GotStatusName(got), sentinel::LogLevel::kError);
    return {false, sentinel::GotStatusName(got)};
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_wallfire_install",
                      {{"result", "installed"}, {"reason", "ok"},
                       {"msg", "FireGunLocal's wall query also asks the static level for the player to muzzle segment on a combat map or in the "
                               "combat lobby"}});
  return {true, "installed"};
}

ModuleResult Install(bool on) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  const Pins* const pins = g_pinsOverride.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, pins != nullptr ? *pins : kPins, on);
}

Counters GetCounters() noexcept {
  return {g_atSite.load(), g_checked.load(), g_blocked.load(), g_nan.load(), g_game.load()};
}

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }
void SetPinsForTest(const Pins* pins) noexcept { g_pinsOverride.store(pins, std::memory_order_release); }
std::uintptr_t LastReturnForTest() noexcept { return g_lastReturn.load(std::memory_order_relaxed); }

void ResetForTest() noexcept {
  g_hook.Forget();
  QueryThunk::Reset();
  g_ran.store(false);
  g_base.store(0);
  g_site.store(0);
  g_occluded.store(0);
  g_gameOccluded.store(0);
  g_lastReturn.store(0);
  g_warnedGame.store(false);
  g_atSite = g_checked = g_blocked = g_nan = g_game = 0;
  g_lookup.store(nullptr);
  g_pinsOverride.store(nullptr);
}

}  // namespace nevr_quest::combat::wallfire
