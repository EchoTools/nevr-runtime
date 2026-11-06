#include "quest/combat/voipdrop.h"

#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::voipdrop {

namespace {

namespace pinned = sentinel::pinned;

sentinel::GotHook g_hook;
Fns g_f{};  // set before the slot is swapped, read-only after
std::atomic<std::uint64_t> g_packets{0};
std::atomic<std::uint64_t> g_probed{0};
std::atomic<std::uint64_t> g_dropped{0};
std::atomic<unsigned> g_totals{0};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};

using AllocFn = void* (*)(void* allocator, std::uint32_t bytes);
using FreeFn = void (*)(void* allocator, void* page);

constexpr const char* kConsequence = "a voice packet whose receiver table needs a page that cannot be allocated is dropped, not written through a null page";
constexpr const char* kRefusedConsequence = "a voice packet whose receiver table needs a page that cannot be allocated still crashes the game";

void LogStats(bool final) noexcept {
  sentinel::LogFields(final ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug, "combat_voipdrop_stats",
                      {{"packets", g_packets.load(std::memory_order_relaxed)},
                       {"probed", g_probed.load(std::memory_order_relaxed)},
                       {"dropped", g_dropped.load(std::memory_order_relaxed)},
                       {"final", final ? 1 : 0},
                       {"consequence", "packets is every voice packet that reached the handler, probed those whose table needed a page, dropped those "
                                       "whose page could not be allocated; packets over the run's time is the call rate of ReceiveVoip"}});
}

// AllocPage's own request, given straight back. False = that allocator has no block of this size now. True also on any doubt (an unreadable vtable): the
// game's function then decides. Personality-free: it is on the stack across game calls (callback_thunk.h rule 5).
bool PageAvailable(std::uintptr_t cs, std::uint32_t bytes) noexcept {
  void* const was = g_f.currentPtr();
  g_f.setCurrent(g_f.blockAllocator(reinterpret_cast<const void*>(cs + kPageBlock)));
  void* const allocator = g_f.current();
  bool available = false;
  if (allocator != nullptr) {
    bool ok = true;
    const auto vtable = SafeRead<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(allocator), &ok);
    const auto request = SafeRead<std::uintptr_t>(vtable + kVtAlloc, &ok);
    const auto release = SafeRead<std::uintptr_t>(vtable + kVtFree, &ok);
    if (!ok || request == 0 || release == 0) {
      available = true;
    } else {
      void* const page = reinterpret_cast<AllocFn>(request)(allocator, bytes);
      if (page != nullptr) reinterpret_cast<FreeFn>(release)(allocator, page);
      available = page != nullptr;
    }
  }
  g_f.setCurrent(was);
  return available;
}

// Written instead of the call to the game's function, so no game frame is live above it.
void LogDrop(std::uint32_t pageBytes, std::uint64_t n) noexcept {
  if (n <= kDropLines) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_voipdrop_dropped",
                        {{"n", n}, {"page_bytes", pageBytes},
                         {"consequence", "no memory for a voice table page: the packet was dropped, so that speaker is silent until memory returns"}});
  } else if (n % kTotalEvery == 0 && g_totals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_voipdrop_dropped_total",
                        {{"n", n}, {"consequence", "voice packets dropped so far for want of a table page"}});
  }
}

void ReceiveHandler(ReceiveVoipThunk::Fn original, void* self, std::uint64_t handle, const void* event, const void* data, std::uint32_t bytes) noexcept {
  const std::uint64_t seen = g_packets.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto cs = reinterpret_cast<std::uintptr_t>(self);  // null or unreadable: the reads fail and the game's function runs
  bool ok = true;
  const auto head = SafeRead<std::int32_t>(cs + kFreeHead, &ok);
  if (ok && head == -1) {  // the next packet needs a fresh page
    const auto page = SafeRead<std::uint32_t>(cs + kPageBytes, &ok);
    if (ok) {
      g_probed.fetch_add(1, std::memory_order_relaxed);
      if (!PageAvailable(cs, page)) {
        LogDrop(page, g_dropped.fetch_add(1, std::memory_order_relaxed) + 1);
        return;
      }
    }
  }
  original(self, handle, event, data, bytes);
  // After the game's function returned, so no game frame is live above the log.
  if ((seen & (kStatsEvery - 1)) == 0) LogStats(false);
}

NEVR_HOOK_RECORD(kReceiveVoipRecord, ReceiveVoipThunk, &ReceiveHandler);

ModuleResult Refused(const char* token, const char* pin = nullptr, const char* found = nullptr, const char* expected = nullptr) noexcept {
  if (pin != nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_voipdrop_install",
                        {{"result", "refused"}, {"reason", token}, {"pin", pin}, {"found", found}, {"expected", expected},
                         {"consequence", kRefusedConsequence}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_voipdrop_install",
                        {{"result", "refused"}, {"reason", token}, {"consequence", kRefusedConsequence}});
  }
  return {false, token};
}

}  // namespace

Fns ResolveFnsWith(void* context, FindSymbolFn find) noexcept {
  Fns f{};
  f.blockAllocator = reinterpret_cast<void* (*)(const void*)>(find(context, kBlockAllocatorSymbol));
  f.currentPtr = reinterpret_cast<void* (*)()>(find(context, kCurrentPtrSymbol));
  f.setCurrent = reinterpret_cast<void (*)(void*)>(find(context, kSetCurrentSymbol));
  f.current = reinterpret_cast<void* (*)()>(find(context, kCurrentSymbol));
  return f;
}

Fns ResolveFns() noexcept {
  void* const lib = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  if (lib == nullptr) return Fns{};
  const Fns f = ResolveFnsWith(lib, [](void* context, const char* symbol) -> void* { return dlsym(context, symbol); });
  dlclose(lib);
  return f;
}

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const Fns& fns, bool on, const char* legacyDir) noexcept {
  // The old plugin's switch file changes nothing now; one that is there is told so.
  if (LegacySwitchFilePresent(legacyDir, "voipdrop.off")) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_voipdrop_legacy_switch",
                        {{"key", "combat.voipdrop"}, {"file", "voipdrop.off"},
                         {"consequence", "the file is no longer read; set combat.voipdrop: false in config.yaml to turn the module off"}});
  }
  if (!on) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_voipdrop_install",
                        {{"result", "off"}, {"reason", "off_config"},
                         {"consequence", "combat.voipdrop is false: a voice packet whose table page cannot be allocated still reaches the game's function"}});
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
  if (fns.blockAllocator == nullptr || fns.currentPtr == nullptr || fns.setCurrent == nullptr || fns.current == nullptr) return Refused("export_missing");
  g_f = fns;  // before the slot changes: the handler never sees a half-set table
  sentinel::GotTarget target(pinned::kLibR15, kReceiveVoipSymbol, sentinel::RelocKind::kJumpSlot, pinned::LibR15PatchedBuildId(), pins.slot);
  target.expectedOriginal = reinterpret_cast<const void*>(image.base + pins.fn);
  target = target.Owned("combat/voipdrop");
  ReceiveVoipThunk::Arm(kReceiveVoipRecord);
  const sentinel::GotStatus status = sentinel::InstallThunk<ReceiveVoipThunk>(g_hook, target, lookup);
  const bool installed = status == sentinel::GotStatus::kOk;
  // A refused slot keeps the game's call as it is; the handler goes off with it.
  if (!installed && !g_hook.installed()) ReceiveVoipThunk::Disarm();
  sentinel::LogFields(installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_voipdrop_install",
                      {{"result", installed ? "installed" : "refused"}, {"reason", installed ? "ok" : "slot_refused"},
                       {"hook", sentinel::GotStatusName(status)}, {"consequence", installed ? kConsequence : kRefusedConsequence}});
  return {installed, installed ? "installed" : sentinel::GotStatusName(status)};
}

ModuleResult Install(bool on) noexcept {
  char dir[320];
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, kPins, on ? ResolveFns() : Fns{}, on,
                   LegacySwitchDir(dir, sizeof(dir)) ? dir : nullptr);
}

void NoteEnd() noexcept {
  if (g_hook.installed()) LogStats(true);
}

Stats CurrentStats() noexcept {
  return {g_packets.load(std::memory_order_relaxed), g_probed.load(std::memory_order_relaxed), g_dropped.load(std::memory_order_relaxed)};
}

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }

void ResetForTest() noexcept {
  g_hook.Forget();
  ReceiveVoipThunk::Reset();
  g_f = Fns{};
  g_packets.store(0);
  g_probed.store(0);
  g_dropped.store(0);
  g_totals.store(0);
  g_lookup.store(nullptr);
}

}  // namespace nevr_quest::combat::voipdrop
