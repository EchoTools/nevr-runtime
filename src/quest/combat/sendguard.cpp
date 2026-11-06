#include "quest/combat/sendguard.h"

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::sendguard {

namespace {

namespace pinned = sentinel::pinned;

sentinel::GotHook g_hook;

// GameSpaceMT, published before the slot is swapped.
std::atomic<std::uintptr_t> g_lookup{0};
std::atomic<GameSpaceMtSig*> g_testLookup{nullptr};
std::atomic<unsigned> g_dropped{0};
std::atomic<unsigned> g_passed{0};

constexpr std::uint64_t kNoLevel = ~0ULL;  // data[0] == -1: send within the current space
constexpr std::size_t kSpaceChain = 0x30;
constexpr std::size_t kSpaceOffset = 0x80;
constexpr const char* kConsequence = "a script send to a game space that is not loaded is dropped, not faulted";
constexpr const char* kRefusedConsequence = "a script send to a game space that is not loaded still faults the game";

const unsigned char* Load(const unsigned char* from, std::size_t offset) noexcept {
  return from == nullptr ? nullptr : *reinterpret_cast<const unsigned char* const*>(from + offset);
}

// True when the stock function would read a null game space: the node names a level and the lookup finds none. Every pointer is the one
// the stock function reads next, so a null on the way (Load answers null for a null link) leaves the stock function to do what it does.
// The lookup is published before the thunk is armed, so a handler that runs finds it. Personality-free: it is on the stack across the
// lookup, which is game code (callback_thunk.h rule 5).
bool NamedLevelAbsent(const void* context, const std::uint64_t* data) noexcept {
  if (data == nullptr || data[0] == kNoLevel) return false;
  const auto lookup = reinterpret_cast<GameSpaceMtSig*>(g_lookup.load(std::memory_order_acquire));
  // 0x2725a90 ldr x8,[x0]; 0x2725aa0 ldr x9,[x8]; 0x2725aa8 ldr x9,[x9,#0x30]; 0x2725aac ldr x9,[x9]; 0x2725ab0 ldr x19,[x9,#0x80]
  const unsigned char* x9 = Load(Load(static_cast<const unsigned char*>(context), 0), 0);
  x9 = Load(Load(x9, kSpaceChain), 0);
  if (x9 == nullptr) return false;
  void* const space = const_cast<unsigned char*>(Load(x9, kSpaceOffset));
  return lookup(space, data[0]) == nullptr;
}

// Written after the lookup returned and instead of the call to the stock function, so no game frame is live above it.
void LogDrop(const std::uint64_t* data) noexcept {
  const unsigned n = g_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > kDropLines) return;
  char level[19];
  char event[19];
  char actor[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_sendguard_dropped",
                      {{"n", n},
                       {"level_key", sentinel::HexString(level, data[0])},
                       {"event_key", sentinel::HexString(event, data[1])},
                       {"actor_key", sentinel::HexString(actor, data[2])},
                       {"consequence", "the send was dropped: the game space the node names is not loaded (the stock function would fault)"}});
}

void EnterHandler(EnterThunk::Fn original, void* context, const void* data) noexcept {
  const std::uint64_t* const node = static_cast<const std::uint64_t*>(data);
  if (NamedLevelAbsent(context, node)) {
    LogDrop(node);
    return;
  }
  g_passed.fetch_add(1, std::memory_order_relaxed);
  original(context, data);
}

NEVR_HOOK_RECORD(kEnterHook, EnterThunk, &EnterHandler);

// The name of the first pin whose words are not the live image's; nullptr when all hold.
const char* FirstPinMismatch(const unsigned char* base) noexcept {
  for (std::size_t i = 0; i < kCodeCount; ++i) {
    const pinned::PinnedCode& pin = kCode[i];
    if (std::memcmp(base + pin.vaddr, pin.code, pin.words * sizeof(std::uint32_t)) != 0) return pin.name;
  }
  return nullptr;
}

ModuleResult Refused(const char* why, const char* pin) noexcept {
  if (pin != nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_sendguard_install",
                        {{"result", "refused"}, {"why", why}, {"pin", pin}, {"consequence", kRefusedConsequence}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_sendguard_install",
                        {{"result", "refused"}, {"why", why}, {"consequence", kRefusedConsequence}});
  }
  return {false, why};
}

// True when the old plugin's switch file is in `dir`.
bool LegacySwitchPresent(const char* dir) noexcept {
  if (dir == nullptr) return false;
  char path[320];
  const int wrote = std::snprintf(path, sizeof(path), "%s/sendguard.off", dir);
  return wrote >= 0 && static_cast<std::size_t>(wrote) < sizeof(path) && access(path, F_OK) == 0;
}

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, bool enabled, const char* legacySwitchDir) noexcept {
  // The old plugin's switch file changes nothing now; one that is there is told so.
  if (LegacySwitchPresent(legacySwitchDir)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_sendguard_legacy_switch",
                        {{"key", "combat.sendguard"},
                         {"file", "sendguard.off"},
                         {"consequence", "the file is no longer read; set combat.sendguard: false in config.yaml to turn the module off"}});
  }
  if (!enabled) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_sendguard_install",
                        {{"result", "off"},
                         {"reason", "off_config"},
                         {"consequence", "combat.sendguard is false: a send to a game space that is not loaded reaches the stock function"}});
    return {true, "off_config"};
  }
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(pinned::kLibR15, &image)) return Refused("module_not_loaded", nullptr);
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id", nullptr);
  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch", nullptr);
  const unsigned char* const base = reinterpret_cast<const unsigned char*>(image.base);
  if (const char* const mismatch = FirstPinMismatch(base)) return Refused("pin_mismatch", mismatch);

  GameSpaceMtSig* const stand = g_testLookup.load(std::memory_order_acquire);
  g_lookup.store(stand != nullptr ? reinterpret_cast<std::uintptr_t>(stand) : image.base + kGameSpaceMtStubVaddr,
                 std::memory_order_release);
  sentinel::GotTarget target = EnterTarget().Owned("combat/sendguard");
  // The slot must still hold Enter itself: a word something else already replaced is refused, not chained over.
  target.expectedOriginal = reinterpret_cast<const void*>(image.base + kEnterVaddr);
  EnterThunk::Arm(kEnterHook);
  const sentinel::GotStatus status = sentinel::InstallThunk<EnterThunk>(g_hook, target, lookup);
  const bool installed = status == sentinel::GotStatus::kOk;
  // A refused slot keeps the game's call as it is; the handler goes off with it.
  if (!installed && !g_hook.installed()) EnterThunk::Disarm();
  sentinel::LogFields(installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_sendguard_install",
                      {{"result", installed ? "installed" : "refused"},
                       {"hook", sentinel::GotStatusName(status)},
                       {"consequence", installed ? kConsequence : kRefusedConsequence}});
  return {installed, installed ? "installed" : sentinel::GotStatusName(status)};
}

ModuleResult Install() noexcept {
  return InstallAt(&sentinel::FindLoadedImage, combat::Config().sendguard.on, kLegacySwitchDir);
}

Stats CurrentStats() noexcept { return {g_dropped.load(std::memory_order_relaxed), g_passed.load(std::memory_order_relaxed)}; }

void SetLookupForTest(GameSpaceMtSig* lookup) noexcept { g_testLookup.store(lookup, std::memory_order_release); }

void ResetForTest() noexcept {
  g_hook.Forget();
  EnterThunk::Reset();
  g_lookup.store(0);
  g_testLookup.store(nullptr);
  g_dropped.store(0);
  g_passed.store(0);
}

}  // namespace nevr_quest::combat::sendguard
