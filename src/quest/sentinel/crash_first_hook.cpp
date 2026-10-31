#include "crash_first_hook.h"

#include <errno.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstring>

#include "core/crash_first.h"
#include "hook_install.h"
#include "hook_log.h"

namespace nevr_quest::crash_first {

namespace {

using SigactionThunk = sentinel::pinned::LibR15SigactionThunk;
using SignalThunk = sentinel::pinned::LibR15SignalThunk;
using sentinel::pinned::SignalHandlerFn;

// All of the state below is constant-initialised (atomics, zeroed arrays, a mutex initialiser), so it is ready when the
// sentinel's ELF constructor runs and no dynamic initialiser can run after it and blank it (sentinel.cpp, DumpDirStorage).
nevr_crash_first::DisplacedSlot<struct sigaction> g_game[kGuardedCount];
struct sigaction g_preexisting[kGuardedCount];
std::atomic<bool> g_preexistingValid{false};
std::atomic<bool> g_handlersInstalled{false};
std::atomic<std::uint32_t> g_displaced[kGuardedCount];
std::atomic<std::uint32_t> g_queried{0};
std::atomic<std::uint32_t> g_passedThrough{0};
pthread_mutex_t g_storeMutex = PTHREAD_MUTEX_INITIALIZER;  // serialises the game's requests, never taken by OnSignal
nevr_crash_first::FatalClaim g_claim;
Actions g_actions = {nullptr, nullptr, nullptr};  // written before OnSignal is installed, read only after

// Room for the handler on a thread that has no alternate stack: the report formats into a 32 KiB buffer of its own, and
// the handler's frames, libc's and breakpad's need the rest.
constexpr std::size_t kAltStackBytes = 64 * 1024;
alignas(16) char g_altStack[kAltStackBytes];

// A second thread that faults while the first writes the report and the dump waits this long for it, in steps.
constexpr int kClaimWaitSteps = 1000;
constexpr long kClaimWaitStepNs = 10 * 1000 * 1000;

bool IsOurHandler(const struct sigaction& action) noexcept {
  const void* handler = (action.sa_flags & SA_SIGINFO) != 0 ? reinterpret_cast<const void*>(action.sa_sigaction)
                                                            : reinterpret_cast<const void*>(action.sa_handler);
  return handler == reinterpret_cast<const void*>(&OnSignal);
}

// What the game is told it had before this call: its last request, else the actions that were in place before the sentinel
// armed, else the default action.
void PreviousFor(int index, struct sigaction* out) noexcept {
  if (g_game[index].Read(out) == nevr_crash_first::ReadResult::kRead) return;
  if (g_preexistingValid.load(std::memory_order_acquire)) {
    std::memcpy(out, &g_preexisting[index], sizeof(*out));
  } else {
    std::memset(out, 0, sizeof(*out));
  }
}

void Remember(int index, const struct sigaction& action) noexcept {
  if (IsOurHandler(action)) return;  // a game that hands our own action back would make the chain call itself
  pthread_mutex_lock(&g_storeMutex);
  g_game[index].Store(action);
  pthread_mutex_unlock(&g_storeMutex);
}

// libr15's sigaction. A guarded signal's request is remembered and answered, never passed to libc.
int SigactionHandler(SigactionThunk::Fn original, int signo, const struct sigaction* act, struct sigaction* old) noexcept {
  const int index = GuardedIndex(signo);
  if (index < 0) {
    g_passedThrough.fetch_add(1, std::memory_order_relaxed);
    return original(signo, act, old);
  }
  struct sigaction previous;
  PreviousFor(index, &previous);
  if (act != nullptr) {
    g_displaced[index].fetch_add(1, std::memory_order_relaxed);
    Remember(index, *act);
  } else {
    g_queried.fetch_add(1, std::memory_order_relaxed);
  }
  if (old != nullptr) std::memcpy(old, &previous, sizeof(previous));
  return 0;
}

// libr15's signal(): the same rule. The action it builds is the BSD one libc's signal() installs (SA_RESTART, nothing blocked).
SignalHandlerFn SignalHandler(SignalThunk::Fn original, int signo, SignalHandlerFn handler) noexcept {
  const int index = GuardedIndex(signo);
  if (index < 0) {
    g_passedThrough.fetch_add(1, std::memory_order_relaxed);
    return original(signo, handler);
  }
  struct sigaction previous;
  PreviousFor(index, &previous);
  struct sigaction next;
  std::memset(&next, 0, sizeof(next));
  next.sa_handler = handler;
  sigemptyset(&next.sa_mask);
  next.sa_flags = SA_RESTART;
  g_displaced[index].fetch_add(1, std::memory_order_relaxed);
  Remember(index, next);
  return previous.sa_handler;
}

NEVR_HOOK_RECORD(kSigactionHook, SigactionThunk, &SigactionHandler);
NEVR_HOOK_RECORD(kSignalHook, SignalThunk, &SignalHandler);

void WaitForClaimant() noexcept {
  for (int step = 0; step < kClaimWaitSteps && !g_claim.Done(); ++step) {
    timespec pause{0, kClaimWaitStepNs};
    nanosleep(&pause, nullptr);
  }
}

// Calls the game's remembered handler as the game asked for it. True when one was called.
bool ChainToGame(int index, int signo, siginfo_t* info, void* ucontext) {
  struct sigaction game;
  if (g_game[index].Read(&game) != nevr_crash_first::ReadResult::kRead) return false;
  if (IsOurHandler(game)) return false;
  if ((game.sa_flags & SA_SIGINFO) != 0) {
    const auto handler = game.sa_sigaction;
    if (reinterpret_cast<std::uintptr_t>(handler) <= 1) return false;  // SIG_DFL, SIG_IGN
    handler(signo, info, ucontext);
    return true;
  }
  const SignalHandlerFn handler = game.sa_handler;
  if (reinterpret_cast<std::uintptr_t>(handler) <= 1) return false;
  handler(signo);
  return true;
}

std::uint32_t DisplacedFor(int index) noexcept { return g_displaced[index].load(std::memory_order_relaxed); }

}  // namespace

int GuardedIndex(int signo) noexcept {
  for (int i = 0; i < kGuardedCount; ++i) {
    if (kGuardedSignals[i] == signo) return i;
  }
  return -1;
}

void OnSignal(int signo, siginfo_t* info, void* ucontext) {
  const int savedErrno = errno;
  const int index = GuardedIndex(signo);
  if (g_claim.TryClaim()) {
    // Ours, in order: the report needs the ucontext this signal carries, the dump is the authoritative record.
    if (g_actions.report != nullptr) g_actions.report(signo, info, ucontext);
    if (g_actions.dump != nullptr) g_actions.dump(signo, info, ucontext);
    g_claim.MarkDone();
  } else {
    WaitForClaimant();
  }
  // The system's handler goes back before the game's runs: a game handler that aborts, or that returns for a fault it
  // cannot fix, then reaches the system's handler and not this one again.
  RestoreHandlers();
  const bool chained = index >= 0 && ChainToGame(index, signo, info, ucontext);
  if (g_actions.note != nullptr) g_actions.note(signo, chained, index >= 0 ? DisplacedFor(index) : 0U);
  errno = savedErrno;
  // A signal sent by a process (kill, raise, abort) is not raised again when the handler returns; one the kernel raised
  // for a fault is, and reaches the restored handler by itself.
  if (info == nullptr || info->si_code <= 0 || signo == SIGABRT) {
    if (syscall(SYS_tgkill, getpid(), static_cast<pid_t>(syscall(SYS_gettid)), signo) < 0) _exit(1);
  }
}

int InstallHandlers(const Actions& actions) noexcept {
  if (g_handlersInstalled.exchange(true, std::memory_order_acq_rel)) return kGuardedCount;
  g_actions = actions;

  for (int i = 0; i < kGuardedCount; ++i) {
    if (sigaction(kGuardedSignals[i], nullptr, &g_preexisting[i]) != 0) std::memset(&g_preexisting[i], 0, sizeof(g_preexisting[i]));
  }
  g_preexistingValid.store(true, std::memory_order_release);

  // The calling thread gets an alternate stack unless it has one (ART gives its own threads one): a stack overflow runs
  // nothing else.
  bool altStack = false;
  stack_t current;
  if (sigaltstack(nullptr, &current) == 0 && (current.ss_sp == nullptr || (current.ss_flags & SS_DISABLE) != 0)) {
    stack_t ours;
    std::memset(&ours, 0, sizeof(ours));
    ours.ss_sp = g_altStack;
    ours.ss_size = kAltStackBytes;
    altStack = sigaltstack(&ours, nullptr) == 0;
  }

  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  sigemptyset(&action.sa_mask);
  for (int i = 0; i < kGuardedCount; ++i) sigaddset(&action.sa_mask, kGuardedSignals[i]);  // none of them interrupts the handler
  action.sa_sigaction = &OnSignal;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  int handled = 0;
  for (int i = 0; i < kGuardedCount; ++i) {
    if (sigaction(kGuardedSignals[i], &action, nullptr) == 0) ++handled;
  }
  sentinel::LogFields(handled == kGuardedCount ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "crash_first_arm",
                      {{"handled", handled}, {"of", kGuardedCount}, {"alt_stack", altStack ? "installed" : "kept_existing"}});
  return handled;
}

void RestoreHandlers() noexcept {
  if (!g_preexistingValid.load(std::memory_order_acquire)) return;
  for (int i = 0; i < kGuardedCount; ++i) {
    if (sigaction(kGuardedSignals[i], &g_preexisting[i], nullptr) != 0) signal(kGuardedSignals[i], SIG_DFL);
  }
}

// The hook handles have a non-constant initialiser (the static-init check flags a namespace-scope one: tools/check_quest_static_init.sh),
// so each lives in a function-local static, built on first use. Neither is touched by OnSignal.
sentinel::GotHook& SigactionHandle() {
  static sentinel::GotHook handle;
  return handle;
}
sentinel::GotHook& SignalHandle() {
  static sentinel::GotHook handle;
  return handle;
}

bool InstallAt(const sentinel::GotTarget& sigactionTarget, const sentinel::GotTarget& signalTarget) noexcept {
  SigactionThunk::Arm(kSigactionHook);
  SignalThunk::Arm(kSignalHook);
  const sentinel::GotStatus sigactionStatus = sentinel::InstallThunk<SigactionThunk>(SigactionHandle(), sigactionTarget);
  const sentinel::GotStatus signalStatus = sentinel::InstallThunk<SignalThunk>(SignalHandle(), signalTarget);
  const bool ok = sigactionStatus == sentinel::GotStatus::kOk && signalStatus == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "crash_first_install",
                      {{"result", ok ? "installed" : "failed"},
                       {"sigaction", sentinel::GotStatusName(sigactionStatus)},
                       {"signal", sentinel::GotStatusName(signalStatus)}});
  return ok;
}

bool Install() noexcept { return InstallAt(sentinel::pinned::LibR15Sigaction(), sentinel::pinned::LibR15Signal()); }

// ---- the provider libraries, hooked after they load ----------------------------------------------------------------

namespace {

// One pair of thunks and handles per provider library: a thunk's original-call pointer is a static of its instantiation, so
// each slot gets its own tag (callback_thunk.h). The handlers are the ones libr15's slots use. The handles are plain
// namespace-scope objects like every other hook's, because the sentinel runs no initializer but its own constructor
// (tools/check_quest_static_init.sh).
using InstallFn = sentinel::GotStatus (*)(const sentinel::GotTarget&);
struct LateOps {
  void* (*sigactionEntry)();
  InstallFn installSigaction;
  void (*forgetSigaction)();
  void* (*signalEntry)();
  InstallFn installSignal;
  void (*forgetSignal)();
};

#define NEVR_CRASH_LATE_OPS(I)                                                                                  \
  struct Late##I##SigactionTag {};                                                                              \
  struct Late##I##SignalTag {};                                                                                 \
  using Late##I##SigactionThunk = sentinel::CallbackThunk<Late##I##SigactionTag, sentinel::pinned::SigactionSig>; \
  using Late##I##SignalThunk = sentinel::CallbackThunk<Late##I##SignalTag, sentinel::pinned::SignalSig>;       \
  sentinel::GotHook& Late##I##SigactionHandle() {                                                               \
    static sentinel::GotHook handle;                                                                            \
    return handle;                                                                                              \
  }                                                                                                             \
  sentinel::GotHook& Late##I##SignalHandle() {                                                                  \
    static sentinel::GotHook handle;                                                                            \
    return handle;                                                                                              \
  }                                                                                                             \
  NEVR_HOOK_RECORD(kLate##I##Sigaction, Late##I##SigactionThunk, &SigactionHandler);                            \
  NEVR_HOOK_RECORD(kLate##I##Signal, Late##I##SignalThunk, &SignalHandler);                                     \
  void* Late##I##SigactionEntry() { return Late##I##SigactionThunk::EntryAddress(); }                           \
  void* Late##I##SignalEntry() { return Late##I##SignalThunk::EntryAddress(); }                                 \
  sentinel::GotStatus Late##I##InstallSigaction(const sentinel::GotTarget& target) {                           \
    Late##I##SigactionThunk::Arm(kLate##I##Sigaction);                                                          \
    return sentinel::InstallThunk<Late##I##SigactionThunk>(Late##I##SigactionHandle(), target);                   \
  }                                                                                                             \
  sentinel::GotStatus Late##I##InstallSignal(const sentinel::GotTarget& target) {                              \
    Late##I##SignalThunk::Arm(kLate##I##Signal);                                                                \
    return sentinel::InstallThunk<Late##I##SignalThunk>(Late##I##SignalHandle(), target);                         \
  }                                                                                                             \
  void Late##I##ForgetSigaction() { Late##I##SigactionHandle().Forget(); }                                        \
  void Late##I##ForgetSignal() { Late##I##SignalHandle().Forget(); }

NEVR_CRASH_LATE_OPS(0)
NEVR_CRASH_LATE_OPS(1)
NEVR_CRASH_LATE_OPS(2)
#undef NEVR_CRASH_LATE_OPS

const LateOps kLateOps[kLateModuleMax] = {
    {&Late0SigactionEntry, &Late0InstallSigaction, &Late0ForgetSigaction, &Late0SignalEntry, &Late0InstallSignal,
     &Late0ForgetSignal},
    {&Late1SigactionEntry, &Late1InstallSigaction, &Late1ForgetSigaction, &Late1SignalEntry, &Late1InstallSignal,
     &Late1ForgetSignal},
    {&Late2SigactionEntry, &Late2InstallSigaction, &Late2ForgetSigaction, &Late2SignalEntry, &Late2InstallSignal,
     &Late2ForgetSignal},
};

pthread_mutex_t g_lateMutex = PTHREAD_MUTEX_INITIALIZER;
const LateModule* g_lateModules = nullptr;  // under g_lateMutex; null until the first provider library loads
int g_lateCount = 0;
int g_lastLogged[kLateModuleMax][2];       // the GotStatus pair last logged for a library, -1 before the first
std::atomic<std::uint32_t> g_lateInstalls{0};

// The two module tables hold std::optional members, which the static-init check flags at namespace scope: function-local statics.
LateModule* ProductionModules() {
  static LateModule modules[kLateModuleMax];
  return modules;
}
LateModule* TestModules() {
  static LateModule modules[kLateModuleMax];
  return modules;
}

void ResetLateLog() {
  for (auto& pair : g_lastLogged) pair[0] = pair[1] = -1;
}

void UseProductionModules() {
  g_lateCount = 0;
  for (const sentinel::pinned::CrashLateModule& m : sentinel::pinned::kCrashLateModules) {
    if (g_lateCount >= kLateModuleMax) break;
    ProductionModules()[g_lateCount++] = LateModule{m.module, m.buildId, m.sigactionSlot, m.signalSlot};
  }
  g_lateModules = ProductionModules();
  ResetLateLog();
}

const char* BaseName(const char* path) {
  const char* slash = std::strrchr(path, '/');
  return slash != nullptr ? slash + 1 : path;
}

// Hooks one slot of an image unless it already holds our entry (this mapping was hooked before); a hook left from a mapping
// that is gone is forgotten first.
sentinel::GotStatus HookLateSlot(const sentinel::ElfImage& image, const sentinel::GotTarget& target, void* entry,
                                 InstallFn install, void (*forget)()) {
  const sentinel::SlotResolution slot = sentinel::ResolveSlot(image, target, sentinel::kNativeRelocs);
  if (slot.status == sentinel::GotStatus::kOk && slot.slot != nullptr && *slot.slot == entry) return sentinel::GotStatus::kOk;
  forget();
  const sentinel::GotStatus status = install(target);
  if (status == sentinel::GotStatus::kOk) g_lateInstalls.fetch_add(1, std::memory_order_relaxed);
  return status;
}

// A library that is not mapped any more keeps no reservation: its slots' addresses can be reused by the next mapping of
// anything, and a reservation left on one would refuse that mapping's install.
void ReleaseUnmappedLateModule(int index) {
  sentinel::ElfImage image;
  if (sentinel::FindLoadedImage(g_lateModules[index].module, &image)) return;
  kLateOps[index].forgetSigaction();
  kLateOps[index].forgetSignal();
}

void EnsureLateModule(int index) {
  const LateModule& module = g_lateModules[index];
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(module.module, &image)) return;  // not mapped (yet)
  const LateOps& ops = kLateOps[index];
  const sentinel::GotTarget sigactionTarget(module.module, sentinel::pinned::kSigactionSymbol, sentinel::RelocKind::kJumpSlot,
                                            module.buildId, module.sigactionSlot);
  const sentinel::GotTarget signalTarget(module.module, sentinel::pinned::kSignalSymbol, sentinel::RelocKind::kJumpSlot,
                                         module.buildId, module.signalSlot);
  const std::uint32_t installsBefore = g_lateInstalls.load(std::memory_order_relaxed);
  const sentinel::GotStatus sigactionStatus =
      HookLateSlot(image, sigactionTarget, ops.sigactionEntry(), ops.installSigaction, ops.forgetSigaction);
  const sentinel::GotStatus signalStatus =
      HookLateSlot(image, signalTarget, ops.signalEntry(), ops.installSignal, ops.forgetSignal);
  // One line per hook made and per change of outcome: a library that stays refused is logged once, not at every provider dlopen.
  const bool hooked = g_lateInstalls.load(std::memory_order_relaxed) != installsBefore;
  if (!hooked && g_lastLogged[index][0] == static_cast<int>(sigactionStatus) &&
      g_lastLogged[index][1] == static_cast<int>(signalStatus)) {
    return;
  }
  g_lastLogged[index][0] = static_cast<int>(sigactionStatus);
  g_lastLogged[index][1] = static_cast<int>(signalStatus);
  const bool ok = sigactionStatus == sentinel::GotStatus::kOk && signalStatus == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "crash_first_late",
                      {{"module", module.module},
                       {"result", ok ? "installed" : "failed"},
                       {"sigaction", sentinel::GotStatusName(sigactionStatus)},
                       {"signal", sentinel::GotStatusName(signalStatus)}});
}

}  // namespace

void OnModuleLoaded(const char* name, void* handle) noexcept {
  if (handle == nullptr || name == nullptr) return;
  const char* const base = BaseName(name);
  if (std::strstr(base, "pnsrad") == nullptr && std::strstr(base, "pnsovr") == nullptr) return;
  pthread_mutex_lock(&g_lateMutex);
  if (g_lateModules == nullptr) UseProductionModules();
  for (int i = 0; i < g_lateCount; ++i) ReleaseUnmappedLateModule(i);
  for (int i = 0; i < g_lateCount; ++i) EnsureLateModule(i);
  pthread_mutex_unlock(&g_lateMutex);
}

void SetLateModulesForTest(const LateModule* modules, int count) noexcept {
  pthread_mutex_lock(&g_lateMutex);
  if (modules == nullptr || count <= 0) {
    g_lateModules = nullptr;
    g_lateCount = 0;
  } else {
    g_lateCount = count < kLateModuleMax ? count : kLateModuleMax;
    for (int i = 0; i < g_lateCount; ++i) TestModules()[i] = modules[i];
    g_lateModules = TestModules();
  }
  ResetLateLog();
  pthread_mutex_unlock(&g_lateMutex);
}

Counts CurrentCounts() noexcept {
  Counts counts{};
  for (int i = 0; i < kGuardedCount; ++i) counts.displaced[i] = g_displaced[i].load(std::memory_order_relaxed);
  counts.queried = g_queried.load(std::memory_order_relaxed);
  counts.passedThrough = g_passedThrough.load(std::memory_order_relaxed);
  counts.lateInstalls = g_lateInstalls.load(std::memory_order_relaxed);
  return counts;
}

void ResetForTest() noexcept {
  for (int i = 0; i < kGuardedCount; ++i) {
    g_game[i].ResetForTest();
    g_displaced[i].store(0, std::memory_order_relaxed);
  }
  g_queried.store(0, std::memory_order_relaxed);
  g_passedThrough.store(0, std::memory_order_relaxed);
  g_lateInstalls.store(0, std::memory_order_relaxed);
  g_claim.ResetForTest();
  g_actions = {nullptr, nullptr, nullptr};
  g_preexistingValid.store(false, std::memory_order_relaxed);
  g_handlersInstalled.store(false, std::memory_order_relaxed);
}

}  // namespace nevr_quest::crash_first
