// The frozen-frame exit (frozen_frame_exit.h, docs/quest/combat/frozen_frame_exit.md, #2377): an observer of the render-configuration hook's
// frame submits and a watchdog thread that ends a combat game whose frames stopped in VR.
#include "quest/combat/frozen_frame_exit.h"

#include <elf.h>
#include <pthread.h>
#include <time.h>

#include <cstring>

#include "quest/combat/frozen_frame_exit_rules.h"
#include "quest/combat/frozen_frame_exit_targets.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/pinned_targets.h"
#include "quest/sentinel/process_end_hook.h"
#include "quest/sentinel/quit_hook.h"
#include "quest/sentinel/render_config.h"

namespace nevr_quest::combat::frozen_frame_exit {

namespace {

using sentinel::LogLevel;

// ---- what the observer and the watchdog share (atomics only) ---------------------------------------------------------------------

std::atomic<std::int64_t> g_lastSubmitMs{0};  // the last watched submit, 0: not armed
std::atomic<std::int64_t> g_armedAtMs{0};     // when the stamp went from 0 to a time
std::atomic<std::uint64_t> g_submits{0}, g_arms{0};
std::atomic<bool> g_fired{false};
std::atomic<bool> g_ending{false};  // the watchdog is ending the process: the row's end must not join it
std::atomic<std::uintptr_t> g_wordAddr{0};  // the in-VR word in the running libr15, 0 before the install proved it
std::atomic<int> g_limitS{rules::kDefaultLimitS};
std::atomic<std::int64_t (*)() noexcept> g_clock{nullptr};
std::atomic<unsigned> g_pollMs{kPollMs};

std::int64_t NowMs() noexcept {
  const auto clock = g_clock.load(std::memory_order_acquire);
  if (clock != nullptr) return clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// The in-VR word, one aligned 8-byte load (the game writes it with one 8-byte str); 0 before the install proved its address.
std::uint64_t ReadInVrWord() noexcept {
  const std::uintptr_t at = g_wordAddr.load(std::memory_order_acquire);
  if (at == 0) return 0;
  return __atomic_load_n(reinterpret_cast<const std::uint64_t*>(at), __ATOMIC_RELAXED);
}

// ---- the thread ---------------------------------------------------------------------------------------------------------------

pthread_mutex_t g_control = PTHREAD_MUTEX_INITIALIZER;  // one start or stop at a time
pthread_mutex_t g_wait = PTHREAD_MUTEX_INITIALIZER;     // g_stop, g_ending's decision and the condition variable
pthread_cond_t g_cond;
bool g_condReady = false;
bool g_running = false;
bool g_stop = false;
pthread_t g_thread;

void Deadline(timespec* at, unsigned ms) noexcept {
  clock_gettime(CLOCK_MONOTONIC, at);
  at->tv_sec += ms / 1000;
  at->tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
  if (at->tv_nsec >= 1000000000L) {
    at->tv_nsec -= 1000000000L;
    ++at->tv_sec;
  }
}

void* WatchdogMain(void*) {
  pthread_mutex_lock(&g_wait);
  while (!g_stop) {
    timespec deadline;
    Deadline(&deadline, g_pollMs.load(std::memory_order_relaxed));
    int rc = 0;
    while (!g_stop && rc == 0) rc = pthread_cond_timedwait(&g_cond, &g_wait, &deadline);
    if (g_stop) break;
    pthread_mutex_unlock(&g_wait);  // never log or read game memory under the wait lock
    Tick();
    pthread_mutex_lock(&g_wait);
  }
  pthread_mutex_unlock(&g_wait);
  return nullptr;
}

int StartThread() noexcept {
  pthread_mutex_lock(&g_control);
  if (g_running) {
    pthread_mutex_unlock(&g_control);
    return 0;
  }
  if (!g_condReady) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cond, &attr);
    pthread_condattr_destroy(&attr);
    g_condReady = true;
  }
  pthread_mutex_lock(&g_wait);
  g_stop = false;
  pthread_mutex_unlock(&g_wait);
  const int rc = pthread_create(&g_thread, nullptr, &WatchdogMain, nullptr);
  if (rc == 0) {
    pthread_setname_np(g_thread, "nevr-frozenframe");
    g_running = true;
  }
  pthread_mutex_unlock(&g_control);
  return rc;
}

// Stops and joins the thread, unless the watchdog has decided to end the process (then it is left to do so). True when it joined.
bool StopThread() noexcept {
  pthread_mutex_lock(&g_control);
  bool joined = false;
  if (g_running) {
    pthread_mutex_lock(&g_wait);
    const bool ending = g_ending.load(std::memory_order_acquire);
    if (!ending) {
      g_stop = true;
      pthread_cond_signal(&g_cond);
    }
    pthread_mutex_unlock(&g_wait);
    if (!ending) {
      pthread_join(g_thread, nullptr);
      g_running = false;
      joined = true;
    }
  }
  pthread_mutex_unlock(&g_control);
  return joined;
}

// ---- how the game is ended ------------------------------------------------------------------------------------------------------

bool ProductionCanFinish() noexcept { return quit::CanFinishActivity(); }
bool ProductionFinish() noexcept { return quit::FinishActivity(); }
// The grace: a wait on the watchdog's own condition variable that nothing signals while ending (StopThread does not), so it lasts `ms`.
void ProductionGrace(unsigned ms) noexcept {
  if (!g_condReady) return;
  timespec deadline;
  Deadline(&deadline, ms);
  pthread_mutex_lock(&g_wait);
  int rc = 0;
  while (rc == 0) rc = pthread_cond_timedwait(&g_cond, &g_wait, &deadline);
  pthread_mutex_unlock(&g_wait);
}
void ProductionEnd(int exitCode, const char* reason) noexcept { process_end::EndWithExitCode(exitCode, reason); }

std::atomic<bool (*)() noexcept> g_canFinish{&ProductionCanFinish};
std::atomic<bool (*)() noexcept> g_finish{&ProductionFinish};
std::atomic<void (*)(unsigned) noexcept> g_grace{&ProductionGrace};
std::atomic<void (*)(int, const char*) noexcept> g_end{&ProductionEnd};

constexpr const char* kFiredMsg =
    "the game has submitted no frame in VR for longer than combat.frozen_frame_exit_s in a combat level: its activity is finished and the process "
    "ends after the grace, so the player lands in Home instead of a frozen view";

void Fire(std::int64_t sinceMs, int limitS, std::int64_t nowMs) noexcept {
  pthread_mutex_lock(&g_wait);
  if (g_stop) {  // the row's end is stopping the watchdog: the process is ending already
    pthread_mutex_unlock(&g_wait);
    return;
  }
  g_ending.store(true, std::memory_order_release);
  pthread_mutex_unlock(&g_wait);
  g_fired.store(true, std::memory_order_release);
  const bool canFinish = g_canFinish.load(std::memory_order_acquire)();
  const std::int64_t armedAt = g_armedAtMs.load(std::memory_order_relaxed);
  sentinel::LogFields(LogLevel::kWarn, "combat_frozen_frame_exit",
                      {{"since_submit_ms", static_cast<long long>(sinceMs)},
                       {"limit_s", limitS},
                       {"scope_bits", static_cast<long long>(scope::detail::g_bits.load(std::memory_order_relaxed))},
                       {"submits", g_submits.load(std::memory_order_relaxed)},
                       {"armed_ms", static_cast<long long>(armedAt != 0 ? nowMs - armedAt : 0)},
                       {"action", canFinish ? "finish_activity" : "no_activity"},
                       {"msg", kFiredMsg}});
  if (canFinish) g_finish.load(std::memory_order_acquire)();
  g_grace.load(std::memory_order_acquire)(kGraceMs);
  sentinel::LogFields(LogLevel::kWarn, "combat_frozen_frame_exit_end",
                      {{"grace_ms", kGraceMs},
                       {"action", "end_process"},
                       {"msg", "the game did not end within the grace: the process ends with exit code 1 (reason frozen_frame)"}});
  g_end.load(std::memory_order_acquire)(process_end::kFailureExitCode, "frozen_frame");
}

// ---- the install ------------------------------------------------------------------------------------------------------------------

constexpr const char* kRefusedMsg = "the frozen-frame exit is not installed: a combat game whose frames stop in VR stays frozen";

const char* LimitSource(bool fromFile) noexcept { return fromFile ? "config" : "default"; }

ModuleResult Refused(const char* status, int limitS, bool fromFile, const char* why = nullptr, const pinned::Failure* word = nullptr,
                     int error = 0) noexcept {
  if (word != nullptr) {
    char at[19], want[19], have[19];
    sentinel::LogFields(LogLevel::kError, "combat_frozen_frame_exit_install",
                        {{"result", "refused"}, {"status", status}, {"limit_s", limitS}, {"limit_source", LimitSource(fromFile)},
                         {"scope", "combat_or_lobby"}, {"pin", word->what}, {"vaddr", sentinel::HexString(at, word->vaddr)},
                         {"found", word->readable ? sentinel::HexString(have, word->have) : "unreadable"},
                         {"expected", sentinel::HexString(want, word->want)}, {"msg", kRefusedMsg}});
  } else if (why != nullptr) {
    sentinel::LogFields(LogLevel::kError, "combat_frozen_frame_exit_install",
                        {{"result", "refused"}, {"status", status}, {"why", why}, {"limit_s", limitS}, {"limit_source", LimitSource(fromFile)},
                         {"scope", "combat_or_lobby"}, {"msg", kRefusedMsg}});
  } else if (error != 0) {
    sentinel::LogFields(LogLevel::kError, "combat_frozen_frame_exit_install",
                        {{"result", "refused"}, {"status", status}, {"errno", error}, {"limit_s", limitS},
                         {"limit_source", LimitSource(fromFile)}, {"scope", "combat_or_lobby"}, {"msg", kRefusedMsg}});
  } else {
    sentinel::LogFields(LogLevel::kError, "combat_frozen_frame_exit_install",
                        {{"result", "refused"}, {"status", status}, {"limit_s", limitS}, {"limit_source", LimitSource(fromFile)},
                         {"scope", "combat_or_lobby"}, {"msg", kRefusedMsg}});
  }
  return {false, status};
}

// True when [vaddr, vaddr + size) lies inside one PT_LOAD segment of the image (its file-backed or zero-filled part).
bool InsideLoadSegment(const sentinel::ElfImage& image, std::uint64_t vaddr, std::size_t size) noexcept {
  for (std::size_t i = 0; i < image.phnum; ++i) {
    const Elf64_Phdr& ph = image.phdr[i];
    if (ph.p_type != PT_LOAD) continue;
    if (vaddr >= ph.p_vaddr && vaddr + size <= ph.p_vaddr + ph.p_memsz) return true;
  }
  return false;
}

}  // namespace

void Stamp() noexcept {
  std::int64_t now = NowMs();
  if (now <= 0) now = 1;  // 0 means "not armed"
  if (g_lastSubmitMs.exchange(now, std::memory_order_acq_rel) == 0) {
    g_arms.fetch_add(1, std::memory_order_relaxed);
    g_armedAtMs.store(now, std::memory_order_relaxed);
  }
  g_submits.fetch_add(1, std::memory_order_relaxed);
}

void Tick() noexcept {
  if (g_fired.load(std::memory_order_acquire)) return;
  std::int64_t last = g_lastSubmitMs.load(std::memory_order_acquire);
  // Out of VR or out of a combat level: no limit runs, and the next submit arms again. The scope is read without its freshness test: a freeze
  // stops the tick that refreshes it.
  if (ReadInVrWord() == 0 || !scope::SeenCombatOrLobby()) {
    if (last != 0) g_lastSubmitMs.compare_exchange_strong(last, 0, std::memory_order_acq_rel);
    return;
  }
  const int limitS = g_limitS.load(std::memory_order_relaxed);
  if (!rules::Expired(NowMs(), last, true, limitS)) return;
  // Both inputs once more: a submit or a leave from VR between the two reads is not a freeze.
  const bool inVr = ReadInVrWord() != 0;
  last = g_lastSubmitMs.load(std::memory_order_acquire);
  const std::int64_t now = NowMs();
  if (!rules::Expired(now, last, inVr, limitS)) return;
  Fire(now - last, limitS, now);
}

ModuleResult InstallAt(bool on, int limitS, bool limitFromFile, sentinel::ImageLookup lookup) noexcept {
  if (limitS < rules::kMinLimitS || limitS > rules::kMaxLimitS) {
    limitS = rules::kDefaultLimitS;
    limitFromFile = false;
  }
  if (!on) {
    sentinel::LogFields(LogLevel::kInfo, "combat_frozen_frame_exit_install",
                        {{"result", "off"}, {"status", "off_config"}, {"limit_s", limitS}, {"limit_source", LimitSource(limitFromFile)},
                         {"scope", "combat_or_lobby"},
                         {"msg", "combat.frozen_frame_exit is false: a combat game whose frames stop in VR stays frozen until the player ends it"}});
    return {false, "off", true};
  }
  if (!scope::Live()) return Refused("scope_not_live", limitS, limitFromFile);
  if (!render_config::SubmitSlotHeld()) return Refused("submit_slot_not_held", limitS, limitFromFile);
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return Refused("word_unreadable", limitS, limitFromFile, "module_not_loaded");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("word_unreadable", limitS, limitFromFile, "no_build_id");
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused("word_unreadable", limitS, limitFromFile, "build_id_mismatch");
  if (!InsideLoadSegment(image, targets::kInVrWordVaddr, targets::kInVrWordBytes)) {
    return Refused("word_unreadable", limitS, limitFromFile, "outside_image");
  }
  pinned::Failure word;
  if (pinned::FirstFailure(image.base, &targets::kWordPins, 1, &word)) {
    return Refused("pinned_word_mismatch", limitS, limitFromFile, nullptr, &word);
  }
  g_limitS.store(limitS, std::memory_order_relaxed);
  g_wordAddr.store(image.base + targets::kInVrWordVaddr, std::memory_order_release);
  const int rc = StartThread();
  if (rc != 0) {
    g_wordAddr.store(0, std::memory_order_release);
    return Refused("thread_not_started", limitS, limitFromFile, nullptr, nullptr, rc);
  }
  detail::g_armed.store(true, std::memory_order_release);
  sentinel::LogFields(LogLevel::kInfo, "combat_frozen_frame_exit_install",
                      {{"result", "installed"}, {"status", "ok"}, {"limit_s", limitS}, {"limit_source", LimitSource(limitFromFile)},
                       {"scope", "combat_or_lobby"},
                       {"msg", "in a combat map or the combat lobby, a game in VR that submits no frame for limit_s seconds is ended and lands in Home"}});
  return {true, "installed"};
}

ModuleResult Install(bool on, int limitS, bool limitFromFile) noexcept {
  return InstallAt(on, limitS, limitFromFile, &sentinel::FindLoadedImage);
}

void NoteEnd() noexcept {
  if (!detail::g_armed.load(std::memory_order_acquire)) return;
  const bool joined = StopThread();
  if (joined) detail::g_armed.store(false, std::memory_order_release);
  sentinel::LogFields(LogLevel::kInfo, "combat_frozen_frame_exit_counts",
                      {{"arms", g_arms.load(std::memory_order_relaxed)},
                       {"submits", g_submits.load(std::memory_order_relaxed)},
                       {"fired", g_fired.load(std::memory_order_acquire) ? 1 : 0},
                       {"joined", joined ? 1 : 0},
                       {"msg", "arms: times a submit in a combat level started the watch; submits: frames seen while watched"}});
}

std::int64_t LastSubmitMs() noexcept { return g_lastSubmitMs.load(std::memory_order_acquire); }
std::uint64_t Submits() noexcept { return g_submits.load(std::memory_order_relaxed); }
std::uint64_t Arms() noexcept { return g_arms.load(std::memory_order_relaxed); }
bool Fired() noexcept { return g_fired.load(std::memory_order_acquire); }

void SetSeamsForTest(const Seams& seams) noexcept {
  g_canFinish.store(seams.canFinish != nullptr ? seams.canFinish : &ProductionCanFinish, std::memory_order_release);
  g_finish.store(seams.finish != nullptr ? seams.finish : &ProductionFinish, std::memory_order_release);
  g_grace.store(seams.grace != nullptr ? seams.grace : &ProductionGrace, std::memory_order_release);
  g_end.store(seams.end != nullptr ? seams.end : &ProductionEnd, std::memory_order_release);
}
void SetClockForTest(std::int64_t (*nowMs)() noexcept) noexcept { g_clock.store(nowMs, std::memory_order_release); }
void SetPollMsForTest(unsigned ms) noexcept { g_pollMs.store(ms != 0 ? ms : kPollMs, std::memory_order_relaxed); }
void ArmForTest() noexcept { detail::g_armed.store(true, std::memory_order_release); }

bool ThreadRunningForTest() noexcept {
  pthread_mutex_lock(&g_control);
  const bool running = g_running;
  pthread_mutex_unlock(&g_control);
  return running;
}

void ResetForTest() noexcept {
  g_ending.store(false, std::memory_order_release);
  StopThread();
  detail::g_armed.store(false);
  g_lastSubmitMs.store(0);
  g_armedAtMs.store(0);
  g_submits.store(0);
  g_arms.store(0);
  g_fired.store(false);
  g_wordAddr.store(0);
  g_limitS.store(rules::kDefaultLimitS);
  g_clock.store(nullptr);
  g_pollMs.store(kPollMs);
  SetSeamsForTest(Seams{});
}

}  // namespace nevr_quest::combat::frozen_frame_exit
