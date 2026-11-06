#include "quest/combat/peerclamp.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/level_scope.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::peerclamp {

namespace {

namespace pinned = sentinel::pinned;

// One read of the broadcaster fields (+0x470..+0x564) covers sent, now, the chunk table, the head index and the per-chunk count.
constexpr std::size_t kBdSpan = kOffPerChunk + 4 - kOffSent;
// One read of an entry (+0x8..+0x230) covers the next index, the key and the stamp.
constexpr std::size_t kEntrySpan = kOffStamp + 8 - kOffNext;

sentinel::GotHook g_hook;
std::atomic<std::uintptr_t> g_invalid{0};  // &SAddress::kInvalid, read at install
std::atomic<std::uint64_t> g_inScope{0};
std::atomic<std::uint64_t> g_walked{0};
std::atomic<std::uint64_t> g_clamped{0};
std::atomic<std::uint64_t> g_capped{0};
std::atomic<unsigned> g_totals{0};
std::atomic<bool> g_capLogged{false};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};

constexpr const char* kConsequence =
    "a peer's last-received stamp above now is pulled down to now before the game's timeout loop, while a combat map or the combat lobby is loaded";
constexpr const char* kRefusedConsequence =
    "a stamp above now reads as 2^64 us of silence, so the game drops a new game-server peer as timed out; the game's timeout loop is unchanged";

void LogStats(bool final) noexcept {
  sentinel::LogFields(final ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug, "combat_peerclamp_stats",
                      {{"calls", CleanupThunk::Calls()},
                       {"in_scope", g_inScope.load(std::memory_order_relaxed)},
                       {"walked", g_walked.load(std::memory_order_relaxed)},
                       {"clamped", g_clamped.load(std::memory_order_relaxed)},
                       {"capped", g_capped.load(std::memory_order_relaxed)},
                       {"level_looks", scope::Looks()},
                       {"final", final ? 1 : 0},
                       {"consequence", "calls is every CleanupPeers call the game made, in_scope those made while a combat map or the combat lobby was loaded "
                                       "(the only ones read), walked the peer entries seen, clamped the stamps pulled down; level_looks is 0 when the "
                                       "level scope never ran, and then nothing is clamped"}});
}

// Written before the game's function is called, from a frame that holds no game frame above it.
void LogClamp(std::uint64_t n, std::uint32_t idx, std::uint64_t stamp, std::uint64_t now, std::uint64_t sent) noexcept {
  if (n <= kClampLines) {
    char stampText[24];
    char nowText[24];
    char diffText[24];
    char sentText[24];
    std::snprintf(stampText, sizeof(stampText), "%llu", static_cast<unsigned long long>(stamp));
    std::snprintf(nowText, sizeof(nowText), "%llu", static_cast<unsigned long long>(now));
    std::snprintf(diffText, sizeof(diffText), "%llu", static_cast<unsigned long long>(stamp - now));
    std::snprintf(sentText, sizeof(sentText), "%llu", static_cast<unsigned long long>(sent));
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_peerclamp_clamped",
                        {{"n", n}, {"idx", idx}, {"stamp", stampText}, {"now", nowText}, {"diff", diffText}, {"sent", sentText},
                         {"consequence", "a peer's last-received stamp was above now and is now now, so the timeout loop does not read it as 2^64 us of silence"}});
  } else if (n % kTotalEvery == 0 && g_totals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_peerclamp_clamped_total", {{"clamps", n}});
  }
}

// The walk: the stock loop's own list, every read never faulting, the one write the 8-byte stamp of an entry that was just read.
void Clamp(std::uintptr_t bd) noexcept {
  // A failed read leaves the span zero (the fields are read from the lowest address up, so a fault before `per` leaves per == 0): per == 0 ends the walk.
  std::uint8_t b[kBdSpan] = {};
  (void)SafeRead(bd + kOffSent, b, sizeof(b));
  std::uint32_t per;
  std::uint32_t idx;
  std::uint64_t now;
  std::uint64_t sent;
  std::uintptr_t chunks;
  std::memcpy(&sent, b + (kOffSent - kOffSent), sizeof(sent));
  std::memcpy(&now, b + (kOffNow - kOffSent), sizeof(now));
  std::memcpy(&chunks, b + (kOffChunks - kOffSent), sizeof(chunks));
  std::memcpy(&idx, b + (kOffHead - kOffSent), sizeof(idx));
  std::memcpy(&per, b + (kOffPerChunk - kOffSent), sizeof(per));
  if (per == 0) return;
  std::uint8_t invalid[kKeyLen];
  if (!SafeRead(g_invalid.load(std::memory_order_relaxed), invalid, sizeof(invalid))) return;
  for (unsigned n = 0; idx != kEnd; ++n) {
    if (n == kMaxWalk) {
      g_capped.fetch_add(1, std::memory_order_relaxed);
      if (!g_capLogged.exchange(true, std::memory_order_relaxed)) {
        sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_peerclamp_walk_capped",
                            {{"entries", kMaxWalk},
                             {"consequence", "the peer list is longer than the walk follows or loops, so the entries behind the cap keep the game's own stamps"}});
      }
      return;
    }
    // the chunk pointer; an unreadable one is 0 and the entry read below fails
    std::uintptr_t chunk = 0;
    (void)SafeRead(chunks + static_cast<std::uintptr_t>(idx / per) * sizeof(std::uintptr_t), &chunk, sizeof(chunk));
    const std::uintptr_t e = chunk + static_cast<std::uintptr_t>(idx % per) * kStride;
    std::uint8_t entry[kEntrySpan];  // +0x8 .. +0x230: next, key, stamp
    if (!SafeRead(e + kOffNext, entry, sizeof(entry))) return;
    g_walked.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t next;
    std::uint64_t stamp;
    std::memcpy(&next, entry + (kOffNext - kOffNext), sizeof(next));
    std::memcpy(&stamp, entry + (kOffStamp - kOffNext), sizeof(stamp));
    if (std::memcmp(entry + (kOffKey - kOffNext), invalid, kKeyLen) != 0 && stamp > now) {  // an entry the stock loop timeout-tests, stamped in the future
      *reinterpret_cast<std::uint64_t*>(e + kOffStamp) = now;
      LogClamp(g_clamped.fetch_add(1, std::memory_order_relaxed) + 1, idx, stamp, now, sent);
    }
    idx = next;
  }
}

void CleanupHandler(CleanupThunk::Fn original, void* bd) noexcept {
  if (!scope::InCombatMapOrLobby()) {  // arena, the lobbies arena players use, every other level: the game's function, nothing read or written
    original(bd);
    return;
  }
  const std::uint64_t inScope = g_inScope.fetch_add(1, std::memory_order_relaxed) + 1;
  Clamp(reinterpret_cast<std::uintptr_t>(bd));
  if ((inScope & (kStatsEvery - 1)) == 0) LogStats(false);
  original(bd);
}

NEVR_HOOK_RECORD(kCleanupRecord, CleanupThunk, &CleanupHandler);

ModuleResult Refused(const char* token, const char* pin = nullptr, const char* found = nullptr, const char* expected = nullptr) noexcept {
  if (pin != nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_peerclamp_install",
                        {{"result", "refused"}, {"reason", token}, {"pin", pin}, {"found", found}, {"expected", expected},
                         {"consequence", kRefusedConsequence}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_peerclamp_install",
                        {{"result", "refused"}, {"reason", token}, {"consequence", kRefusedConsequence}});
  }
  return {false, token};
}

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept {
  if (!on) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_peerclamp_install",
                        {{"result", "off"}, {"reason", "off_config"},
                         {"consequence", "combat.peerclamp is false: the game's timeout loop reads every stamp as it is, and a stamp above now drops the peer"}});
    return {false, "off_config", true};
  }
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(pinned::kLibR15, &image)) return Refused("module_not_loaded");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id");
  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch");
  std::uint32_t have = 0;
  bool readable = false;
  const std::size_t bad = FirstWordMismatch(image.base, kWords, kWordCount, &have, &readable);
  if (bad != kWordCount) {
    char at[19];
    char haveText[19];
    char wantText[19];
    return Refused("pinned_word_mismatch", sentinel::HexString(at, kWords[bad].vaddr),
                   readable ? sentinel::HexString(haveText, have) : "unreadable", sentinel::HexString(wantText, kWords[bad].word));
  }
  std::uintptr_t invalid = 0;
  if (!SafeRead(image.base + static_cast<std::uintptr_t>(pins.invalid), &invalid, sizeof(invalid)) || invalid == 0) return Refused("invalid_unreadable");
  if (!scope::Start()) return Refused("tick_table_full");  // the guard would never be in scope, so it is not installed
  g_invalid.store(invalid, std::memory_order_relaxed);
  sentinel::GotTarget target(pinned::kLibR15, kCleanupSymbol, sentinel::RelocKind::kJumpSlot, pinned::LibR15PatchedBuildId(), pins.slot);
  target.expectedOriginal = reinterpret_cast<const void*>(image.base + pins.fn);
  target = target.Owned("combat/peerclamp");
  CleanupThunk::Arm(kCleanupRecord);
  const sentinel::GotStatus status = sentinel::InstallThunk<CleanupThunk>(g_hook, target, lookup);
  const bool installed = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_peerclamp_install",
                      {{"result", installed ? "installed" : "refused"}, {"reason", installed ? "ok" : "slot_refused"},
                       {"hook", sentinel::GotStatusName(status)}, {"scope", "combat_map_or_lobby"},
                       {"consequence", installed ? kConsequence : kRefusedConsequence}});
  return {installed, installed ? "installed" : sentinel::GotStatusName(status)};
}

ModuleResult Install(bool on) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, kPins, on);
}

void NoteEnd() noexcept {
  if (g_hook.installed()) LogStats(true);
}

Stats CurrentStats() noexcept {
  return {CleanupThunk::Calls(), g_inScope.load(std::memory_order_relaxed), g_walked.load(std::memory_order_relaxed),
          g_clamped.load(std::memory_order_relaxed), g_capped.load(std::memory_order_relaxed)};
}

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }

void ResetForTest() noexcept {
  g_hook.Forget();
  CleanupThunk::Reset();
  g_invalid.store(0);
  g_inScope.store(0);
  g_walked.store(0);
  g_clamped.store(0);
  g_capped.store(0);
  g_totals.store(0);
  g_capLogged.store(false);
  g_lookup.store(nullptr);
}

}  // namespace nevr_quest::combat::peerclamp
