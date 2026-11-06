#include "quest/combat/equip_refire.h"

#include <time.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <pthread.h>

#include "quest/invite/frame_dispatch.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/mapping_perms.h"
#include "runtime/patch/equipment_refire_rules.h"

namespace nevr_quest::combat::equip_refire {

namespace rules = nevr_equipment_refire;

namespace {

namespace pinned = sentinel::pinned;

// The six GotHook handles: slot * 2 + word (0 JUMP_SLOT, 1 GLOB_DAT).
constexpr std::size_t kHookCount = 6;
sentinel::GotHook g_hooks[kHookCount];

// What the rule remembers between a setter call and its re-fire, and the lock around it. A plain pthread mutex: a std::mutex global is
// a dynamic initializer (tools/check_quest_static_init.sh refuses it), and the Tracker is constant-initialised.
rules::Tracker g_tracker;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// The game functions the re-fire calls, published before any slot is swapped.
using HandlerFn = void (*)(void* component, std::uint32_t handle);
std::atomic<std::uintptr_t> g_handler[kSlotCount];
std::atomic<std::uintptr_t> g_netGame{0};
std::atomic<std::uintptr_t> g_userId{0};

std::atomic<std::uint64_t (*)() noexcept> g_clock{nullptr};
std::atomic<std::uint64_t> g_tracked{0};
std::atomic<std::uint64_t> g_unchanged{0};
std::atomic<std::uint64_t> g_notLocal{0};
std::atomic<std::uint64_t> g_unreadable{0};
std::atomic<std::uint64_t> g_refired{0};
std::atomic<std::uint64_t> g_skipped{0};
std::atomic<std::uint64_t> g_changeLines{0};
std::atomic<bool> g_frameWorkRegistered{false};

// A test's stand-ins for the five game functions; unset in production, where they are base + the pinned addresses.
GameFunctions g_testFunctions;
std::atomic<bool> g_useTestFunctions{false};

// The sentinel's per-access read of a game object (mapping_perms.h): the range must lie in readable mappings of /proc/self/maps, then it
// is copied. The rule reads through this and fails closed on false.
bool ReadMemory(std::uintptr_t address, void* out, std::size_t size) noexcept {
  if (!sentinel::MappingsAllow(address, size, /*write=*/false)) return false;
  std::memcpy(out, reinterpret_cast<const void*>(address), size);
  return true;
}

std::uint64_t MonotonicMs() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000U + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000U;
}

// The Quest side of the shared feature (runtime/patch/equipment_refire_rules.h, "An Env is the build's side of it"): the game's memory, its
// two lookups, its handlers, the clock, the lock and the log. Every decision is the rule's.
struct QuestEnv {
  int word = 0;  // the word the setter was called through: 0 JUMP_SLOT, 1 GLOB_DAT (the change record says which)

  // NetGameMT is `mov w8, #0x8510; ldr x0, [x0, x8]; ret` (pinned_targets.h, kEquipmentRefireCode): the rule probes that field first.
  static constexpr std::size_t kNetGameField = 0x8510;

  bool operator()(std::uintptr_t address, void* out, std::size_t size) { return ReadMemory(address, out, size); }

  std::uintptr_t NetGame(std::uintptr_t arg) {
    const auto fn = reinterpret_cast<pinned::NetGameMTSig*>(g_netGame.load(std::memory_order_acquire));
    return fn != nullptr ? fn(arg) : 0;
  }
  std::uint32_t SlotOf(std::uintptr_t game, const std::uint64_t key[2]) {
    const auto fn = reinterpret_cast<pinned::UserIdSig*>(g_userId.load(std::memory_order_acquire));
    return fn != nullptr ? fn(reinterpret_cast<void*>(game), key[0], key[1]) : 0xffffffffU;
  }
  void CallHandler(rules::Slot slot, std::uintptr_t owner, std::uint32_t handle) {
    const auto fn = reinterpret_cast<HandlerFn>(g_handler[static_cast<std::size_t>(slot)].load(std::memory_order_acquire));
    if (fn != nullptr) fn(reinterpret_cast<void*>(owner), handle);
  }
  std::uint64_t NowMs() {
    const auto clock = g_clock.load(std::memory_order_acquire);
    return clock != nullptr ? clock() : MonotonicMs();
  }
  void Lock() { pthread_mutex_lock(&g_lock); }
  void Unlock() { pthread_mutex_unlock(&g_lock); }

  void LogChange(rules::Slot slot, const rules::SetterResult& r, std::uint32_t handle, std::uint64_t value) {
    switch (r.verdict) {
      case rules::Verdict::kTracked: g_tracked.fetch_add(1, std::memory_order_relaxed); break;
      case rules::Verdict::kUnchanged: g_unchanged.fetch_add(1, std::memory_order_relaxed); break;
      case rules::Verdict::kNotLocal: g_notLocal.fetch_add(1, std::memory_order_relaxed); break;
      case rules::Verdict::kUnreadable: g_unreadable.fetch_add(1, std::memory_order_relaxed); break;
      case rules::Verdict::kServer: break;
    }
    const std::uint64_t n = g_changeLines.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > kChangeLines) {
      if (n == kChangeLines + 1) {
        sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_equip_refire_change_cap",
                            {{"cap", kChangeLines},
                             {"consequence", "further setter calls are counted, not written; re-fires are unaffected"}});
      }
      return;
    }
    char handleText[16];
    char valueText[19];
    std::snprintf(handleText, sizeof(handleText), "0x%08x", static_cast<unsigned>(handle));
    sentinel::LogFields(sentinel::LogLevel::kDebug, "combat_equip_refire_change",
                        {{"msg", rules::kMsgChange},
                         {"slot", rules::SlotName(slot)},
                         {"word", word == 0 ? "jump_slot" : "glob_dat"},
                         {"verdict", rules::VerdictName(r.verdict)},
                         {"handle", handleText},
                         {"value", sentinel::HexString(valueText, value)},
                         {"read", r.read ? "yes" : "no"},
                         {"changed", r.changed ? "yes" : "no"},
                         {"local", r.local ? "yes" : "no"}});
  }
  void LogRefired(const rules::Refire& r) {
    g_refired.fetch_add(1, std::memory_order_relaxed);
    char handleText[16];
    char valueText[19];
    std::snprintf(handleText, sizeof(handleText), "0x%08x", static_cast<unsigned>(r.handle));
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_equip_refire_refired",
                        {{"msg", rules::kMsgRefired},
                         {"slot", rules::SlotName(r.slot)},
                         {"value", sentinel::HexString(valueText, r.value)},
                         {"handle", handleText},
                         {"changes", r.changes},
                         {"quiet_ms", r.quietMs}});
  }
  void LogSkipped(const rules::Refire& r, const char* reason) {
    g_skipped.fetch_add(1, std::memory_order_relaxed);
    char handleText[16];
    char valueText[19];
    std::snprintf(handleText, sizeof(handleText), "0x%08x", static_cast<unsigned>(r.handle));
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_equip_refire_skipped",
                        {{"msg", rules::kMsgSkipped},
                         {"slot", rules::SlotName(r.slot)},
                         {"value", sentinel::HexString(valueText, r.value)},
                         {"handle", handleText},
                         {"reason", reason}});
  }
};

// CR15NetPlayerEquipmentCS::Set<Weapon|Ability|Grenade>Slot through its JUMP_SLOT (Word 0) or GLOB_DAT (Word 1) word. The original first
// in the rule's order: the record's value is read before it, the verdict is made after.
template <int Slot, int Word>
void SetterHandler(typename pinned::LibR15EquipmentSetterThunk<Slot, Word>::Fn original, void* component, std::uint32_t handle,
                   std::uint64_t value) noexcept {
  QuestEnv env{Word};
  rules::OnSetter(env, g_tracker, static_cast<rules::Slot>(Slot), reinterpret_cast<std::uintptr_t>(component), handle, value,
                  /*server=*/false, [&] { original(component, handle, value); });
}

template <int Slot, int Word>
struct Word_;  // defined for each of the six by NEVR_EQUIP_WORD: the thunk and its recorded hook

// One thunk, its handler and its hook record per word (callback_thunk.h: one thunk per hooked slot, a handler is armed only through a
// record).
#define NEVR_EQUIP_WORD(S, W)                                                                          \
  using Thunk##S##W = pinned::LibR15EquipmentSetterThunk<S, W>;                                         \
  constexpr Thunk##S##W::Handler kHandler##S##W = &SetterHandler<S, W>;                                 \
  NEVR_HOOK_RECORD(kRecord##S##W, Thunk##S##W, kHandler##S##W);                                         \
  template <>                                                                                           \
  struct Word_<S, W> {                                                                                  \
    using Thunk = Thunk##S##W;                                                                          \
    static const sentinel::HookRecord<Thunk>& Record() noexcept { return kRecord##S##W; }               \
  };

NEVR_EQUIP_WORD(0, 0)
NEVR_EQUIP_WORD(0, 1)
NEVR_EQUIP_WORD(1, 0)
NEVR_EQUIP_WORD(1, 1)
NEVR_EQUIP_WORD(2, 0)
NEVR_EQUIP_WORD(2, 1)
#undef NEVR_EQUIP_WORD

constexpr const char* kStatusKey[kHookCount] = {"weapon_jump", "weapon_glob", "tac_mod_jump", "tac_mod_glob", "ordnance_jump", "ordnance_glob"};

// Arms the word's handler and installs its thunk into `target`; a refused install leaves the handler off.
template <int Slot, int Word>
sentinel::GotStatus InstallWord(sentinel::ImageLookup lookup, std::uintptr_t base) noexcept {
  using Thunk = typename Word_<Slot, Word>::Thunk;
  sentinel::GotTarget target =
      pinned::LibR15EquipmentSetter(Slot, Word == 0 ? sentinel::RelocKind::kJumpSlot : sentinel::RelocKind::kGlobDat).Owned("combat/equip_refire");
  // The slot must still hold the setter itself: a word something else already replaced is refused, not chained over.
  target.expectedOriginal = reinterpret_cast<const void*>(base + pinned::kEquipmentSetters[Slot].vaddr);
  Thunk::Arm(Word_<Slot, Word>::Record());
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hooks[Slot * 2 + Word], target, lookup);
  // A word that is refused keeps the game's call as it is; a second install over a word this module already holds must not turn the
  // working handler off.
  if (status != sentinel::GotStatus::kOk && !g_hooks[Slot * 2 + Word].installed()) Thunk::Disarm();
  return status;
}

template <int Slot, int Word>
void ResetWord() noexcept {
  g_hooks[Slot * 2 + Word].Forget();
  Word_<Slot, Word>::Thunk::Reset();
}

bool FrameWork(void* /*context*/) noexcept {
  FireDueNow();
  return true;
}

// The name of the first pin whose words are not the live image's; nullptr when all hold.
const char* FirstPinMismatch(const unsigned char* base) noexcept {
  for (std::size_t i = 0; i < pinned::kEquipmentRefireCodeCount; ++i) {
    const pinned::PinnedCode& pin = pinned::kEquipmentRefireCode[i];
    if (std::memcmp(base + pin.vaddr, pin.code, pin.words * sizeof(std::uint32_t)) != 0) return pin.name;
  }
  return nullptr;
}

constexpr const char* kRefusedConsequence = "an equipment change the scripts drop stays dropped";

void LogRefused(const char* why, const char* pin) noexcept {
  if (pin != nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_equip_refire_install",
                        {{"result", "refused"}, {"why", why}, {"pin", pin}, {"consequence", kRefusedConsequence}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_equip_refire_install",
                        {{"result", "refused"}, {"why", why}, {"consequence", kRefusedConsequence}});
  }
}

}  // namespace

const char* InstallResultName(std::size_t installed) noexcept {
  if (installed >= kWordCount) return "installed";
  return installed > 0 ? "partial" : "refused";
}

void FireDueNow() noexcept {
  QuestEnv env;
  rules::FireDue(env, g_tracker);
}

bool InstallAt(sentinel::ImageLookup lookup) noexcept {
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(pinned::kLibR15, &image)) {
    LogRefused("module_not_loaded", nullptr);
    return false;
  }
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) {
    LogRefused("no_build_id", nullptr);
    return false;
  }
  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) {
    LogRefused("build_id_mismatch", nullptr);
    return false;
  }
  const unsigned char* const base = reinterpret_cast<const unsigned char*>(image.base);
  if (const char* const mismatch = FirstPinMismatch(base)) {
    LogRefused("pin_mismatch", mismatch);
    return false;
  }
  // The frame work first: a module whose re-fire could never run installs nothing.
  if (!g_frameWorkRegistered.load(std::memory_order_acquire)) {
    if (!invite::frame::RegisterFrameWork(&FrameWork, nullptr)) {
      LogRefused("frame_work_full", nullptr);
      return false;
    }
    g_frameWorkRegistered.store(true, std::memory_order_release);
  }
  const bool stub = g_useTestFunctions.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < kSlotCount; ++i) {
    g_handler[i].store(stub ? g_testFunctions.handler[i] : image.base + pinned::kEquipmentSetters[i].handler, std::memory_order_release);
  }
  g_netGame.store(stub ? g_testFunctions.netGame : image.base + pinned::kNetGameMTVaddr, std::memory_order_release);
  g_userId.store(stub ? g_testFunctions.userId : image.base + pinned::kUserIdVaddr, std::memory_order_release);

  const sentinel::GotStatus status[kHookCount] = {
      InstallWord<0, 0>(lookup, image.base), InstallWord<0, 1>(lookup, image.base), InstallWord<1, 0>(lookup, image.base),
      InstallWord<1, 1>(lookup, image.base), InstallWord<2, 0>(lookup, image.base), InstallWord<2, 1>(lookup, image.base),
  };
  std::size_t installed = 0;
  for (const sentinel::GotStatus s : status) installed += s == sentinel::GotStatus::kOk ? 1 : 0;
  const char* const result = InstallResultName(installed);
  sentinel::LogFields(installed == kWordCount ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_equip_refire_install",
                      {{"result", result},
                       {kStatusKey[0], sentinel::GotStatusName(status[0])},
                       {kStatusKey[1], sentinel::GotStatusName(status[1])},
                       {kStatusKey[2], sentinel::GotStatusName(status[2])},
                       {kStatusKey[3], sentinel::GotStatusName(status[3])},
                       {kStatusKey[4], sentinel::GotStatusName(status[4])},
                       {kStatusKey[5], sentinel::GotStatusName(status[5])},
                       {"settle_ms", rules::kSettleMs},
                       {"consequence", installed == kWordCount
                                           ? "a slot the local player changed is applied again after it has been still for settle_ms"
                                           : (installed > 0 ? "a change that reaches a refused word is not seen; the others are applied again after settle_ms"
                                                            : kRefusedConsequence)}});
  return installed == kWordCount;
}

bool Install() noexcept { return InstallAt(&sentinel::FindLoadedImage); }

Counts CurrentCounts() noexcept {
  return {g_tracked.load(std::memory_order_relaxed),    g_unchanged.load(std::memory_order_relaxed),
          g_notLocal.load(std::memory_order_relaxed),   g_unreadable.load(std::memory_order_relaxed),
          g_refired.load(std::memory_order_relaxed),    g_skipped.load(std::memory_order_relaxed)};
}

void SetClockForTest(std::uint64_t (*clock)() noexcept) noexcept { g_clock.store(clock, std::memory_order_release); }

GameFunctions PublishedGameFunctions() noexcept {
  GameFunctions out{};
  for (std::size_t i = 0; i < kSlotCount; ++i) out.handler[i] = g_handler[i].load(std::memory_order_acquire);
  out.netGame = g_netGame.load(std::memory_order_acquire);
  out.userId = g_userId.load(std::memory_order_acquire);
  return out;
}

void SetGameFunctionsForTest(const GameFunctions* functions) noexcept {
  if (functions != nullptr) g_testFunctions = *functions;
  g_useTestFunctions.store(functions != nullptr, std::memory_order_release);
}

void ResetForTest() noexcept {
  ResetWord<0, 0>();
  ResetWord<0, 1>();
  ResetWord<1, 0>();
  ResetWord<1, 1>();
  ResetWord<2, 0>();
  ResetWord<2, 1>();
  pthread_mutex_lock(&g_lock);
  g_tracker = rules::Tracker{};
  pthread_mutex_unlock(&g_lock);
  for (std::atomic<std::uintptr_t>& h : g_handler) h.store(0);
  g_netGame.store(0);
  g_userId.store(0);
  g_clock.store(nullptr);
  g_useTestFunctions.store(false);
  g_frameWorkRegistered.store(false);
  g_tracked.store(0);
  g_unchanged.store(0);
  g_notLocal.store(0);
  g_unreadable.store(0);
  g_refired.store(0);
  g_skipped.store(0);
  g_changeLines.store(0);
}

}  // namespace nevr_quest::combat::equip_refire
