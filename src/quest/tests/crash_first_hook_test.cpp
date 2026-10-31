// Host test for "our crash handler stays first" (#1605): the real handler and hooks (src/quest/sentinel/crash_first_hook.cpp),
// built without exceptions as the sentinel builds it, driven through a fixture library that calls libc's sigaction and signal
// through PLT slots as libr15 does. No GOT slot of libr15 is touched: got_pinned_test.cpp resolves the pinned slots in the real
// library. Signals are real: raise() runs OnSignal on this thread, and the system's handler (the stand-in for the tombstone
// handler that was in place before the sentinel armed) runs after it.
//
// Built and run by `just test-quest-hooks`.

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/crash_first.h"
#include "crash_first_hook.h"
#include "hook_log.h"
#include "quest/tests/test_check.h"

namespace {

namespace cf = nevr_quest::crash_first;

std::mutex g_eventsMutex;
std::vector<std::string> g_events;
void Event(const char* name) {
  std::lock_guard<std::mutex> lock(g_eventsMutex);
  g_events.emplace_back(name);
}
std::vector<std::string> Events() {
  std::lock_guard<std::mutex> lock(g_eventsMutex);
  return g_events;
}
int IndexOf(const std::vector<std::string>& events, const char* name) {
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (events[i] == name) return static_cast<int>(i);
  }
  return -1;
}

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }
bool HasLine(const char* needle) {
  for (const std::string& line : g_lines) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

// What the report and the dump were handed.
std::atomic<int> g_reportSignal{0};
std::atomic<bool> g_reportHadContext{false};
std::atomic<bool> g_noteChained{false};
std::atomic<std::uint32_t> g_noteDisplaced{0};
std::atomic<bool> g_slowReport{false};
std::atomic<bool> g_reportStarted{false};

void ReportAction(int signo, siginfo_t* info, void* ucontext) noexcept {
  g_reportSignal.store(info != nullptr ? info->si_signo : -1);
  g_reportHadContext.store(ucontext != nullptr);
  (void)signo;
  Event("report");
  g_reportStarted.store(true);
  if (g_slowReport.load()) {
    timespec pause{0, 200 * 1000 * 1000};
    nanosleep(&pause, nullptr);
  }
}
void DumpAction(int, siginfo_t*, void*) noexcept { Event("dump"); }
void NoteAction(int, bool chained, std::uint32_t displaced) noexcept {
  g_noteChained.store(chained);
  g_noteDisplaced.store(displaced);
  Event("note");
}
cf::Actions TestActions() { return {&ReportAction, &DumpAction, &NoteAction}; }

void SystemHandler(int, siginfo_t*, void*) { Event("system"); }
void GameSiginfoHandler(int, siginfo_t*, void*) { Event("game"); }
void GamePlainHandler(int) { Event("game"); }

struct sigaction SiginfoAction(void (*handler)(int, siginfo_t*, void*)) {
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  sigemptyset(&action.sa_mask);
  action.sa_sigaction = handler;
  action.sa_flags = SA_SIGINFO;
  return action;
}

// The handler the kernel would call for `signo` now, read straight from libc.
void* KernelHandler(int signo) {
  struct sigaction current;
  std::memset(&current, 0, sizeof(current));
  sigaction(signo, nullptr, &current);
  return (current.sa_flags & SA_SIGINFO) != 0 ? reinterpret_cast<void*>(current.sa_sigaction)
                                               : reinterpret_cast<void*>(current.sa_handler);
}

// The state before the sentinel arms: the system's handler on every guarded signal. Then the sentinel arms.
void ArmOverSystemHandlers() {
  cf::ResetForTest();
  {
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    g_events.clear();
  }
  g_reportSignal.store(0);
  g_reportHadContext.store(false);
  g_noteChained.store(false);
  g_noteDisplaced.store(0);
  g_slowReport.store(false);
  g_reportStarted.store(false);
  const struct sigaction system = SiginfoAction(&SystemHandler);
  for (int signo : cf::kGuardedSignals) QCHECK(sigaction(signo, &system, nullptr) == 0);
  QCHECK(cf::InstallHandlers(TestActions()) == cf::kGuardedCount);
}

using SigactionFn = int (*)(int, const struct sigaction*, struct sigaction*);
using SignalFn = void (*(*)(int, void (*)(int)))(int);

std::string g_fixtureDir;
SigactionFn g_gameSigaction = nullptr;
SignalFn g_gameSignal = nullptr;

// ---- the shared bookkeeping ---------------------------------------------------------------------------------

struct Pair {
  std::uint64_t a;
  std::uint64_t b;
};

void ASlotIsEmptyUntilTheGameAsksAndThenHoldsItsLatestRequest() {
  nevr_crash_first::DisplacedSlot<Pair> slot;
  Pair out{1, 2};
  QCHECK(slot.Read(&out) == nevr_crash_first::ReadResult::kEmpty);
  QCHECK(!slot.HasBeenSet());
  slot.Store({7, 7});
  slot.Store({9, 9});
  QCHECK(slot.Read(&out) == nevr_crash_first::ReadResult::kRead);
  QCHECK(out.a == 9 && out.b == 9);
  QCHECK(slot.HasBeenSet());
}

// A reader that runs while a writer stores never sees half a record: the two halves are always equal when it reads.
void ASlotNeverShowsAHalfWrittenRecord() {
  nevr_crash_first::DisplacedSlot<Pair> slot;
  slot.Store({0, 0});
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    for (std::uint64_t i = 1; !stop.load(); ++i) slot.Store({i, i});
  });
  long reads = 0;
  for (long i = 0; i < 2000000; ++i) {
    Pair out{0, 0};
    if (slot.Read(&out) == nevr_crash_first::ReadResult::kRead) {
      ++reads;
      QCHECK(out.a == out.b);
    }
  }
  stop.store(true);
  writer.join();
  QCHECK(reads > 0);
}

void OnlyOneThreadClaimsTheFatalEvent() {
  nevr_crash_first::FatalClaim claim;
  std::atomic<int> winners{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) threads.emplace_back([&] { if (claim.TryClaim()) winners.fetch_add(1); });
  for (std::thread& t : threads) t.join();
  QCHECK(winners.load() == 1);
  QCHECK(!claim.Done());
  claim.MarkDone();
  QCHECK(claim.Done());
}

// ---- the hooks ----------------------------------------------------------------------------------------------

void TheGamesRequestIsRememberedAndOursStaysInstalled() {
  ArmOverSystemHandlers();
  for (int signo : cf::kGuardedSignals) QCHECK(KernelHandler(signo) == reinterpret_cast<void*>(&cf::OnSignal));

  const struct sigaction game = SiginfoAction(&GameSiginfoHandler);
  struct sigaction old;
  std::memset(&old, 0, sizeof(old));
  QCHECK(g_gameSigaction(SIGSEGV, &game, &old) == 0);
  // The game is told it displaced what the process had before the sentinel armed, not our handler.
  QCHECK(old.sa_sigaction == &SystemHandler);
  QCHECK(KernelHandler(SIGSEGV) == reinterpret_cast<void*>(&cf::OnSignal));  // not displaced

  // Its next query sees its own request.
  struct sigaction again;
  std::memset(&again, 0, sizeof(again));
  QCHECK(g_gameSigaction(SIGSEGV, nullptr, &again) == 0);
  QCHECK(again.sa_sigaction == &GameSiginfoHandler && (again.sa_flags & SA_SIGINFO) != 0);

  // A second request returns the first as the previous one: the game's own save-and-restore history is whole.
  const struct sigaction second = SiginfoAction(&SystemHandler);
  struct sigaction previous;
  std::memset(&previous, 0, sizeof(previous));
  QCHECK(g_gameSigaction(SIGSEGV, &second, &previous) == 0);
  QCHECK(previous.sa_sigaction == &GameSiginfoHandler);

  const cf::Counts counts = cf::CurrentCounts();
  QCHECK(counts.displaced[cf::GuardedIndex(SIGSEGV)] == 2);
  QCHECK(counts.queried == 1);
}

void AnyOtherSignalGoesToLibcUntouched() {
  ArmOverSystemHandlers();
  const cf::Counts before = cf::CurrentCounts();
  const struct sigaction usr = SiginfoAction(&GameSiginfoHandler);
  struct sigaction old;
  QCHECK(g_gameSigaction(SIGUSR1, &usr, &old) == 0);
  QCHECK(KernelHandler(SIGUSR1) == reinterpret_cast<void*>(&GameSiginfoHandler));  // really installed
  QCHECK(cf::CurrentCounts().passedThrough == before.passedThrough + 1);
  const struct sigaction dfl = [] { struct sigaction a; std::memset(&a, 0, sizeof(a)); a.sa_handler = SIG_DFL; return a; }();
  QCHECK(g_gameSigaction(SIGUSR1, &dfl, nullptr) == 0);
}

void SignalIsTreatedTheSameWay() {
  ArmOverSystemHandlers();
  void (*previous)(int) = g_gameSignal(SIGFPE, &GamePlainHandler);
  QCHECK(reinterpret_cast<void*>(previous) == reinterpret_cast<void*>(&SystemHandler));  // the pre-arm view
  QCHECK(KernelHandler(SIGFPE) == reinterpret_cast<void*>(&cf::OnSignal));
  struct sigaction asked;
  std::memset(&asked, 0, sizeof(asked));
  QCHECK(g_gameSigaction(SIGFPE, nullptr, &asked) == 0);
  QCHECK(asked.sa_handler == &GamePlainHandler && (asked.sa_flags & SA_SIGINFO) == 0 && (asked.sa_flags & SA_RESTART) != 0);
  void (*unrelated)(int) = g_gameSignal(SIGUSR2, SIG_DFL);  // another signal: libc's own answer
  QCHECK(unrelated != SIG_ERR);
}

void OursHandedBackIsNotRemembered() {
  ArmOverSystemHandlers();
  const struct sigaction game = SiginfoAction(&GameSiginfoHandler);
  QCHECK(g_gameSigaction(SIGBUS, &game, nullptr) == 0);
  const struct sigaction ours = SiginfoAction(&cf::OnSignal);
  QCHECK(g_gameSigaction(SIGBUS, &ours, nullptr) == 0);
  struct sigaction asked;
  std::memset(&asked, 0, sizeof(asked));
  QCHECK(g_gameSigaction(SIGBUS, nullptr, &asked) == 0);
  QCHECK(asked.sa_sigaction == &GameSiginfoHandler);  // a chain that called itself would recurse
}

// ---- the crash path -----------------------------------------------------------------------------------------

void OursRunsFirstThenTheGamesAndTheSystemsAfterThat() {
  ArmOverSystemHandlers();
  const struct sigaction game = SiginfoAction(&GameSiginfoHandler);
  QCHECK(g_gameSigaction(SIGFPE, &game, nullptr) == 0);
  QCHECK(g_gameSigaction(SIGFPE, &game, nullptr) == 0);
  raise(SIGFPE);
  const std::vector<std::string> events = Events();
  const std::vector<std::string> want = {"report", "dump", "game", "note", "system"};
  QCHECK(events == want);
  QCHECK(g_reportSignal.load() == SIGFPE && g_reportHadContext.load());  // the report got the signal's own info and ucontext
  QCHECK(g_noteChained.load() && g_noteDisplaced.load() == 2);
  // One-shot, as breakpad's: the system's handler is back.
  QCHECK(KernelHandler(SIGFPE) == reinterpret_cast<void*>(&SystemHandler));
}

void APlainGameHandlerIsCalledWithTheSignalNumber() {
  ArmOverSystemHandlers();
  g_gameSignal(SIGILL, &GamePlainHandler);
  raise(SIGILL);
  const std::vector<std::string> want = {"report", "dump", "game", "note", "system"};
  QCHECK(Events() == want);
}

void WithoutAGameHandlerOursRunsAndTheSystemsFollows() {
  ArmOverSystemHandlers();
  raise(SIGSEGV);
  const std::vector<std::string> want = {"report", "dump", "note", "system"};
  QCHECK(Events() == want);
  QCHECK(!g_noteChained.load() && g_noteDisplaced.load() == 0);
}

void AGameHandlerOfIgnoreOrDefaultIsNotCalled() {
  ArmOverSystemHandlers();
  g_gameSignal(SIGFPE, SIG_IGN);
  raise(SIGFPE);
  const std::vector<std::string> want = {"report", "dump", "note", "system"};
  QCHECK(Events() == want);
  QCHECK(!g_noteChained.load());
}

// A second thread that faults while the first writes the report waits for the dump: the game's handler, which may end the
// process, never runs before the minidump is written.
void ASecondThreadWaitsForTheFirstThreadsReportAndDump() {
  ArmOverSystemHandlers();
  const struct sigaction game = SiginfoAction(&GameSiginfoHandler);
  QCHECK(g_gameSigaction(SIGFPE, &game, nullptr) == 0);
  g_slowReport.store(true);
  siginfo_t info;
  std::memset(&info, 0, sizeof(info));
  info.si_signo = SIGFPE;
  info.si_code = SI_USER;
  std::thread first([&] { cf::OnSignal(SIGFPE, &info, nullptr); });
  while (!g_reportStarted.load()) sched_yield();
  std::thread second([&] { cf::OnSignal(SIGFPE, &info, nullptr); });
  first.join();
  second.join();
  const std::vector<std::string> events = Events();
  QCHECK(IndexOf(events, "report") == 0);
  const int dump = IndexOf(events, "dump");
  const int firstGame = IndexOf(events, "game");
  QCHECK(dump > 0 && firstGame > dump);  // no game handler before the dump, though the second thread arrived during the report
  int reports = 0;
  for (const std::string& e : events) if (e == "report") ++reports;
  QCHECK(reports == 1);  // one report per process
}

// ---- the install --------------------------------------------------------------------------------------------

void InstallAtPatchesBothSlotsAndLogsTheInstall() {
  void* consumer = dlopen((g_fixtureDir + "/libcffx_consumer.so").c_str(), RTLD_NOW | RTLD_LOCAL);
  QCHECK(consumer != nullptr);
  if (consumer == nullptr) return;
  g_gameSigaction = reinterpret_cast<SigactionFn>(dlsym(consumer, "cffx_sigaction"));
  g_gameSignal = reinterpret_cast<SignalFn>(dlsym(consumer, "cffx_signal"));
  QCHECK(g_gameSigaction != nullptr && g_gameSignal != nullptr);

  // Before the install the fixture's slots hold libc: a request displaces whatever is installed.
  const struct sigaction probe = SiginfoAction(&GameSiginfoHandler);
  struct sigaction saved;
  QCHECK(sigaction(SIGSEGV, nullptr, &saved) == 0);
  QCHECK(g_gameSigaction(SIGSEGV, &probe, nullptr) == 0);
  QCHECK(KernelHandler(SIGSEGV) == reinterpret_cast<void*>(&GameSiginfoHandler));
  QCHECK(sigaction(SIGSEGV, &saved, nullptr) == 0);

  const sentinel::LogSink previousSink = sentinel::SetLogSink(&CaptureLine);
  g_lines.clear();
  const sentinel::GotTarget sigactionTarget{"libcffx_consumer.so", "sigaction", sentinel::RelocKind::kJumpSlot};
  const sentinel::GotTarget signalTarget{"libcffx_consumer.so", "signal", sentinel::RelocKind::kJumpSlot};
  QCHECK(cf::InstallAt(sigactionTarget, signalTarget));
  QCHECK(HasLine("\"event\":\"crash_first_install\""));
  QCHECK(HasLine("\"result\":\"installed\""));
  sentinel::SetLogSink(previousSink);
}

void ARefusedInstallSaysSo() {
  const sentinel::LogSink previousSink = sentinel::SetLogSink(&CaptureLine);
  g_lines.clear();
  const sentinel::GotTarget missing{"libcffx_consumer.so", "no_such_symbol", sentinel::RelocKind::kJumpSlot};
  QCHECK(!cf::InstallAt(missing, missing));
  QCHECK(HasLine("\"event\":\"crash_first_install\"") && HasLine("\"result\":\"failed\""));
  sentinel::SetLogSink(previousSink);
}

void TheArmLineSaysHowManySignalsAreOurs() {
  const sentinel::LogSink previousSink = sentinel::SetLogSink(&CaptureLine);
  g_lines.clear();
  ArmOverSystemHandlers();
  QCHECK(HasLine("\"event\":\"crash_first_arm\"") && HasLine("\"handled\":5"));
  sentinel::SetLogSink(previousSink);
}

// ---- the provider libraries, hooked when they load ---------------------------------------------------------

struct Provider {
  void* handle = nullptr;
  SigactionFn sigaction = nullptr;
  SignalFn signal = nullptr;
};

Provider OpenProvider(const char* file) {
  Provider provider;
  provider.handle = dlopen((g_fixtureDir + "/" + file).c_str(), RTLD_NOW | RTLD_LOCAL);
  QCHECK(provider.handle != nullptr);
  if (provider.handle == nullptr) return provider;
  provider.sigaction = reinterpret_cast<SigactionFn>(dlsym(provider.handle, "cffx_sigaction"));
  provider.signal = reinterpret_cast<SignalFn>(dlsym(provider.handle, "cffx_signal"));
  QCHECK(provider.sigaction != nullptr && provider.signal != nullptr);
  return provider;
}

const cf::LateModule kFixtureModules[] = {
    {"libpnsrad.so", nullptr, std::nullopt, std::nullopt},
    {"libpnsradmatchmaking.so", nullptr, std::nullopt, std::nullopt},
    {"libpnsovr.so", nullptr, std::nullopt, std::nullopt},
};

// A request through `provider` is remembered and the kernel's handler stays ours: the proof the library's slots are hooked.
void ExpectHooked(const Provider& provider, int signo) {
  const struct sigaction game = SiginfoAction(&GameSiginfoHandler);
  QCHECK(provider.sigaction(signo, &game, nullptr) == 0);
  QCHECK(KernelHandler(signo) == reinterpret_cast<void*>(&cf::OnSignal));
  struct sigaction asked;
  std::memset(&asked, 0, sizeof(asked));
  QCHECK(provider.sigaction(signo, nullptr, &asked) == 0);
  QCHECK(asked.sa_sigaction == &GameSiginfoHandler);
}

void AProviderLibraryIsHookedWhenItLoadsAndNotBefore() {
  ArmOverSystemHandlers();
  cf::SetLateModulesForTest(kFixtureModules, 3);
  int unrelated = 0;
  cf::OnModuleLoaded("/data/app/x/lib/arm64/libGLESv3.so", &unrelated);  // not a provider: nothing is looked at
  cf::OnModuleLoaded("libpnsrad.so", nullptr);                            // a failed dlopen
  cf::OnModuleLoaded("libpnsrad.so", &unrelated);                         // named, but not mapped: nothing to hook
  QCHECK(cf::CurrentCounts().lateInstalls == 0);

  const sentinel::LogSink previousSink = sentinel::SetLogSink(&CaptureLine);
  g_lines.clear();
  const Provider rad = OpenProvider("libpnsrad.so");
  cf::OnModuleLoaded("/data/app/x/lib/arm64/libpnsrad.so", rad.handle);
  sentinel::SetLogSink(previousSink);
  QCHECK(cf::CurrentCounts().lateInstalls == 2);  // sigaction and signal
  QCHECK(HasLine("\"event\":\"crash_first_late\"") && HasLine("\"module\":\"libpnsrad.so\"") && HasLine("\"result\":\"installed\""));
  ExpectHooked(rad, SIGSEGV);

  // signal() through the same library is the same rule.
  void (*previous)(int) = rad.signal(SIGFPE, &GamePlainHandler);
  (void)previous;
  QCHECK(KernelHandler(SIGFPE) == reinterpret_cast<void*>(&cf::OnSignal));

  // The same mapping again, and a library that is not mapped, install nothing more.
  cf::OnModuleLoaded("libpnsrad.so", rad.handle);
  cf::OnModuleLoaded("libpnsradmatchmaking.so", rad.handle);
  QCHECK(cf::CurrentCounts().lateInstalls == 2);

  dlclose(rad.handle);
}

void EachProviderLibraryIsHookedOnItsOwnAndLibpnsradDoesNotMatchTheMatchmakingLibrary() {
  ArmOverSystemHandlers();
  cf::SetLateModulesForTest(kFixtureModules, 3);
  const Provider matchmaking = OpenProvider("libpnsradmatchmaking.so");
  cf::OnModuleLoaded("libpnsradmatchmaking.so", matchmaking.handle);
  QCHECK(cf::CurrentCounts().lateInstalls == 2);  // only the matchmaking library is mapped: "libpnsrad.so" is not its prefix match
  ExpectHooked(matchmaking, SIGBUS);

  const Provider ovr = OpenProvider("libpnsovr.so");
  cf::OnModuleLoaded("libpnsovr.so", ovr.handle);
  QCHECK(cf::CurrentCounts().lateInstalls == 4);
  ExpectHooked(ovr, SIGILL);

  const Provider rad = OpenProvider("libpnsrad.so");
  cf::OnModuleLoaded("libpnsrad.so", rad.handle);
  QCHECK(cf::CurrentCounts().lateInstalls == 6);
  ExpectHooked(rad, SIGABRT);

  // A request in one library is the game's request for the process: the chain at a crash is the latest, whoever asked.
  const struct sigaction first = SiginfoAction(&SystemHandler);
  QCHECK(matchmaking.sigaction(SIGSEGV, &first, nullptr) == 0);
  struct sigaction seen;
  std::memset(&seen, 0, sizeof(seen));
  QCHECK(ovr.sigaction(SIGSEGV, nullptr, &seen) == 0);
  QCHECK(seen.sa_sigaction == &SystemHandler);

  dlclose(matchmaking.handle);
  dlclose(ovr.handle);
  dlclose(rad.handle);
}

// A library the game closed and loaded again has fresh slots: the hook is made again for the new mapping.
void AProviderLibraryLoadedAgainIsHookedAgain() {
  ArmOverSystemHandlers();
  cf::SetLateModulesForTest(kFixtureModules, 3);
  Provider rad = OpenProvider("libpnsrad.so");
  cf::OnModuleLoaded("libpnsrad.so", rad.handle);
  QCHECK(cf::CurrentCounts().lateInstalls == 2);
  dlclose(rad.handle);
  rad = OpenProvider("libpnsrad.so");
  cf::OnModuleLoaded("libpnsrad.so", rad.handle);
  QCHECK(cf::CurrentCounts().lateInstalls == 4);
  ExpectHooked(rad, SIGSEGV);
  dlclose(rad.handle);
}

// A library whose build is not the pinned one is refused, said once, and left to the game.
void ABuildThatIsNotThePinnedOneIsRefusedAndSaidOnce() {
  ArmOverSystemHandlers();
  static const cf::LateModule wrongBuild[] = {{"libpnsovr.so", "0000000000000000000000000000000000000000", std::nullopt, std::nullopt}};
  cf::SetLateModulesForTest(wrongBuild, 1);
  const sentinel::LogSink previousSink = sentinel::SetLogSink(&CaptureLine);
  g_lines.clear();
  const Provider ovr = OpenProvider("libpnsovr.so");
  cf::OnModuleLoaded("libpnsovr.so", ovr.handle);
  cf::OnModuleLoaded("libpnsovr.so", ovr.handle);
  sentinel::SetLogSink(previousSink);
  QCHECK(cf::CurrentCounts().lateInstalls == 0);
  int refusals = 0;
  for (const std::string& line : g_lines) {
    if (line.find("\"event\":\"crash_first_late\"") != std::string::npos && line.find("\"result\":\"failed\"") != std::string::npos) ++refusals;
  }
  QCHECK(refusals == 1);
  // The game's request goes through to libc as it did before: the slot is not ours.
  const struct sigaction game = SiginfoAction(&GameSiginfoHandler);
  QCHECK(ovr.sigaction(SIGSEGV, &game, nullptr) == 0);
  QCHECK(KernelHandler(SIGSEGV) == reinterpret_cast<void*>(&GameSiginfoHandler));
  dlclose(ovr.handle);
  cf::SetLateModulesForTest(nullptr, 0);
}

}  // namespace

int main(int argc, char** argv) {
  g_fixtureDir = argc > 1 ? argv[1] : ".";
  ASlotIsEmptyUntilTheGameAsksAndThenHoldsItsLatestRequest();
  ASlotNeverShowsAHalfWrittenRecord();
  OnlyOneThreadClaimsTheFatalEvent();
  InstallAtPatchesBothSlotsAndLogsTheInstall();
  TheArmLineSaysHowManySignalsAreOurs();
  TheGamesRequestIsRememberedAndOursStaysInstalled();
  AnyOtherSignalGoesToLibcUntouched();
  SignalIsTreatedTheSameWay();
  OursHandedBackIsNotRemembered();
  OursRunsFirstThenTheGamesAndTheSystemsAfterThat();
  APlainGameHandlerIsCalledWithTheSignalNumber();
  WithoutAGameHandlerOursRunsAndTheSystemsFollows();
  AGameHandlerOfIgnoreOrDefaultIsNotCalled();
  ASecondThreadWaitsForTheFirstThreadsReportAndDump();
  ARefusedInstallSaysSo();
  AProviderLibraryIsHookedWhenItLoadsAndNotBefore();
  EachProviderLibraryIsHookedOnItsOwnAndLibpnsradDoesNotMatchTheMatchmakingLibrary();
  AProviderLibraryLoadedAgainIsHookedAgain();
  ABuildThatIsNotThePinnedOneIsRefusedAndSaidOnce();
  cf::RestoreHandlers();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "crash_first_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("crash_first_hook_test: all checks pass\n");
  return 0;
}
