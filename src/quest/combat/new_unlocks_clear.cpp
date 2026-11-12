#include "quest/combat/new_unlocks_clear.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include <pthread.h>

#include "quest/invite/frame_dispatch.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/mapping_perms.h"
#include "runtime/compat/social_party.h"
#include "runtime/patch/new_unlocks_clear_rules.h"

namespace nevr_quest::combat::new_unlocks_clear {

namespace rules = nevr_new_unlocks_clear;

namespace {

namespace pinned = sentinel::pinned;

// The two GotHook handles: word 0 JUMP_SLOT, word 1 GLOB_DAT. The count is spelled in this file because tools/tests/test_quest_hook_budget.py
// resolves a size by name across every header, and another header's kWordCount is 34.
constexpr std::size_t kHookCount = 2;
sentinel::GotHook g_hooks[kHookCount];

// What the rule remembers between the clear and the acknowledgement, the net game it was made on, and the lock around both. A plain pthread
// mutex: a std::mutex global is a dynamic initializer (tools/check_quest_static_init.sh refuses it), and the Hold is constant-initialised.
rules::Hold g_hold;
std::uintptr_t g_heldNetGame = 0;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

std::atomic<std::uintptr_t> g_jsonEmpty{0};
std::atomic<bool> g_frameWorkRegistered{false};

// A test's stand-in for CJson::Empty; unset in production, where it is base + the pinned address.
GameFunctions g_testFunctions;
std::atomic<bool> g_useTestFunctions{false};

// The sentinel's per-access read of a game object (mapping_perms.h): the range must lie in readable mappings of /proc/self/maps, then it is
// copied; fails closed on false.
bool ReadU64(std::uintptr_t address, std::uint64_t* out) noexcept {
  if (!sentinel::MappingsAllow(address, sizeof(*out), /*write=*/false)) return false;
  std::memcpy(out, reinterpret_cast<const void*>(address), sizeof(*out));
  return true;
}

// The game's own test for "an update was sent and is not yet acknowledged": the buffer at user + 0x28 is not empty. False when the function is
// not published (the hook is never armed then).
bool UpdateInFlight(std::uintptr_t user) noexcept {
  const auto fn = reinterpret_cast<pinned::JsonEmptySig*>(g_jsonEmpty.load(std::memory_order_acquire));
  return fn != nullptr && fn(reinterpret_cast<void*>(user + rules::kUserInFlightOffset)) == 0;
}

// The account the line is about: the social layer's id of the local user, the one the lobby records carry (0 until it is known).
std::uint64_t LocalUserId() noexcept { return nevr_social_party::Global().Snapshot().selfId; }

void LogEnd(const char* event, const char* msg, std::uint64_t count) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kInfo, event, {{"msg", msg}, {"user_id", LocalUserId()}, {"count", count}});
}

// CR15NetGame::RemoveAllNewlyUnlockedItems through its JUMP_SLOT (Word 0) or GLOB_DAT (Word 1) word. The original runs first, as the game's own
// clear; the rule is asked after it, so the list has emptied at once whatever the rule says.
template <int Word>
void ClearHandler(typename pinned::LibR15NewUnlocksClearThunk<Word>::Fn original, void* netGame) noexcept {
  const auto game = reinterpret_cast<std::uintptr_t>(netGame);
  std::uint64_t before = 0;
  std::uint64_t user = 0;
  const bool readable = ReadU64(game + rules::kNewUnlocksCountOffset, &before) && ReadU64(game + rules::kNetGameUserOffset, &user) && user != 0 &&
                        sentinel::MappingsAllow(static_cast<std::uintptr_t>(user), rules::kUserFlagsOffset + sizeof(std::uint32_t), /*write=*/true);
  original(netGame);
  if (!readable) return;  // the game's clear ran as it always did; there is nothing the rule can read
  bool held = false;
  pthread_mutex_lock(&g_lock);
  if (g_hold.OnClear(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(user)), before, UpdateInFlight(static_cast<std::uintptr_t>(user))) ==
      rules::ClearVerdict::kHeld) {
    g_heldNetGame = game;
    held = true;
  }
  pthread_mutex_unlock(&g_lock);
  if (held) LogEnd("combat_new_unlocks_clear_held", rules::kMsgHeld, before);
}

template <int Word>
struct Word_;  // defined for each of the two by NEVR_UNLOCKS_WORD: the thunk and its recorded hook

// One thunk, its handler and its hook record per word (callback_thunk.h: one thunk per hooked slot, a handler is armed only through a record).
#define NEVR_UNLOCKS_WORD(W)                                                                          \
  using Thunk##W = pinned::LibR15NewUnlocksClearThunk<W>;                                              \
  constexpr Thunk##W::Handler kHandler##W = &ClearHandler<W>;                                          \
  NEVR_HOOK_RECORD(kRecord##W, Thunk##W, kHandler##W);                                                 \
  template <>                                                                                          \
  struct Word_<W> {                                                                                    \
    using Thunk = Thunk##W;                                                                            \
    static const sentinel::HookRecord<Thunk>& Record() noexcept { return kRecord##W; }                 \
  };

NEVR_UNLOCKS_WORD(0)
NEVR_UNLOCKS_WORD(1)
#undef NEVR_UNLOCKS_WORD

// Arms the word's handler and installs its thunk into `target`; a refused install leaves the handler off.
template <int Word>
sentinel::GotStatus InstallWord(sentinel::ImageLookup lookup, std::uintptr_t base) noexcept {
  using Thunk = typename Word_<Word>::Thunk;
  sentinel::GotTarget target =
      pinned::LibR15NewUnlocksClear(Word == 0 ? sentinel::RelocKind::kJumpSlot : sentinel::RelocKind::kGlobDat).Owned("combat/new_unlocks_clear");
  // The slot must still hold the clear itself: a word something else already replaced is refused, not chained over.
  target.expectedOriginal = reinterpret_cast<const void*>(base + pinned::kNewUnlocksClear.vaddr);
  Thunk::Arm(Word_<Word>::Record());
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hooks[Word], target, lookup);
  // A word that is refused keeps the game's call as it is; a second install over a word this module already holds must not turn the working
  // handler off.
  if (status != sentinel::GotStatus::kOk && !g_hooks[Word].installed()) Thunk::Disarm();
  return status;
}

template <int Word>
void ResetWord() noexcept {
  g_hooks[Word].Forget();
  Word_<Word>::Thunk::Reset();
}

bool FrameWork(void* /*context*/) noexcept {
  PollNow();
  return true;
}

// The name of the first pin whose words are not the live image's; nullptr when all hold.
const char* FirstPinMismatch(const unsigned char* base) noexcept {
  for (std::size_t i = 0; i < pinned::kNewUnlocksClearCodeCount; ++i) {
    const pinned::PinnedCode& pin = pinned::kNewUnlocksClearCode[i];
    if (std::memcmp(base + pin.vaddr, pin.code, pin.words * sizeof(std::uint32_t)) != 0) return pin.name;
  }
  return nullptr;
}

constexpr const char* kRefusedConsequence = "a clear of the new unlocks made while a profile update is unacknowledged is lost, as the game makes it";

void LogRefused(const char* why, const char* pin) noexcept {
  if (pin != nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_new_unlocks_clear_install",
                        {{"result", "refused"}, {"why", why}, {"pin", pin}, {"consequence", kRefusedConsequence}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_new_unlocks_clear_install",
                        {{"result", "refused"}, {"why", why}, {"consequence", kRefusedConsequence}});
  }
}

}  // namespace

const char* InstallResultName(std::size_t installed) noexcept {
  if (installed >= kWordCount) return "installed";
  return installed > 0 ? "partial" : "refused";
}

void PollNow() noexcept {
  pthread_mutex_lock(&g_lock);
  const std::uintptr_t held = reinterpret_cast<std::uintptr_t>(g_hold.HeldUser());
  if (held == 0) {  // one pointer test per frame while nothing is held
    pthread_mutex_unlock(&g_lock);
    return;
  }
  const std::uint64_t heldCount = g_hold.HeldCount();
  // The user the clear was held for must still be the net game's user: after a log-out the pointer can be freed or reused, and the dirty bit of
  // another object is never touched.
  // No mapping scan here: the pages were checked once, when the clear was held, and what ends a hold is seen from the frame hook's own live
  // net game. A different net game than the one the clear was made on is a session that ended; a different user pointer in the same net game is a
  // logout. Either drops the hold before anything is read through the old user.
  const std::uintptr_t liveGame = reinterpret_cast<std::uintptr_t>(invite::frame::NetGame());
  std::uint64_t current = 0;
  std::uint64_t countNow = 0;
  if (liveGame != g_heldNetGame) {
    current = 0;
  } else {
    std::memcpy(&current, reinterpret_cast<const void*>(liveGame + rules::kNetGameUserOffset), sizeof(current));
    std::memcpy(&countNow, reinterpret_cast<const void*>(liveGame + rules::kNewUnlocksCountOffset), sizeof(countNow));
  }
  if (current != held) {  // 0 when the net game is another one
    g_hold.Drop();
    g_heldNetGame = 0;
    pthread_mutex_unlock(&g_lock);
    LogEnd("combat_new_unlocks_clear_dropped", rules::kMsgDropped, heldCount);
    return;
  }
  const rules::PollVerdict verdict = g_hold.OnPoll(reinterpret_cast<const void*>(held), UpdateInFlight(held), countNow);
  if (verdict == rules::PollVerdict::kRearm) {
    // An atomic OR: the game's own code sets and clears the same word (the acknowledgement handler ORs this bit too).
    __atomic_fetch_or(reinterpret_cast<std::uint32_t*>(held + rules::kUserFlagsOffset), rules::kUserDirtyBit, __ATOMIC_SEQ_CST);
    g_heldNetGame = 0;
  } else if (verdict == rules::PollVerdict::kSuperseded) {
    g_heldNetGame = 0;
  }
  const std::uint64_t rearmedCount = g_hold.LastCount();
  pthread_mutex_unlock(&g_lock);
  if (verdict == rules::PollVerdict::kRearm) LogEnd("combat_new_unlocks_clear_rearmed", rules::kMsgRearmed, rearmedCount);
  if (verdict == rules::PollVerdict::kSuperseded) LogEnd("combat_new_unlocks_clear_superseded", rules::kMsgSuperseded, heldCount);
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
  // The frame work first: a module whose re-arm could never run installs nothing.
  if (!g_frameWorkRegistered.load(std::memory_order_acquire)) {
    if (!invite::frame::RegisterFrameWork(&FrameWork, nullptr)) {
      LogRefused("frame_work_full", nullptr);
      return false;
    }
    g_frameWorkRegistered.store(true, std::memory_order_release);
  }
  const bool stub = g_useTestFunctions.load(std::memory_order_acquire);
  g_jsonEmpty.store(stub ? g_testFunctions.jsonEmpty : image.base + pinned::kJsonEmptyVaddr, std::memory_order_release);

  const sentinel::GotStatus status[kWordCount] = {InstallWord<0>(lookup, image.base), InstallWord<1>(lookup, image.base)};
  std::size_t installed = 0;
  for (const sentinel::GotStatus s : status) installed += s == sentinel::GotStatus::kOk ? 1 : 0;
  sentinel::LogFields(installed == kWordCount ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_new_unlocks_clear_install",
                      {{"result", InstallResultName(installed)},
                       {"jump_slot", sentinel::GotStatusName(status[0])},
                       {"glob_dat", sentinel::GotStatusName(status[1])},
                       {"consequence", installed == kWordCount
                                           ? "a clear made while a profile update is unacknowledged is sent after the acknowledgement"
                                           : (installed > 0 ? "a clear that reaches a refused word is left as the game makes it; the other word re-arms it"
                                                            : kRefusedConsequence)}});
  return installed == kWordCount;
}

bool Install() noexcept { return InstallAt(&sentinel::FindLoadedImage); }

GameFunctions PublishedGameFunctions() noexcept {
  GameFunctions out{};
  out.jsonEmpty = g_jsonEmpty.load(std::memory_order_acquire);
  return out;
}

void SetGameFunctionsForTest(const GameFunctions* functions) noexcept {
  if (functions != nullptr) g_testFunctions = *functions;
  g_useTestFunctions.store(functions != nullptr, std::memory_order_release);
}

void ResetForTest() noexcept {
  ResetWord<0>();
  ResetWord<1>();
  pthread_mutex_lock(&g_lock);
  g_hold = rules::Hold{};
  g_heldNetGame = 0;
  pthread_mutex_unlock(&g_lock);
  g_jsonEmpty.store(0);
  g_useTestFunctions.store(false);
  g_frameWorkRegistered.store(false);
}

}  // namespace nevr_quest::combat::new_unlocks_clear
