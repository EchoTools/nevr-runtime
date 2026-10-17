// Host test for "the process ends when the game's android_main returns" (#621): the real handler
// (src/quest/sentinel/process_end_hook.cpp), built without exceptions as the sentinel builds it, driven through
// the thunk's entry with a stand-in for libr15's android_main. No GOT slot is touched: got_pinned_test.cpp
// resolves the pinned slot in the real library. The last test ends a real process (fork) under a writer that
// saves the way the Quest code saves its sign-in state, to show the file is never torn.
//
// Built and run by `just test-quest-hooks`.

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "process_end_hook.h"
#include "quest/auth/atomic_write.h"
#include "quest/tests/test_check.h"

namespace {

namespace pe = nevr_quest::process_end;
using sentinel::pinned::AndroidAppOpaque;

pe::MainThunk::Fn MainEntry() { return reinterpret_cast<pe::MainThunk::Fn>(pe::MainThunk::EntryAddress()); }

std::vector<std::string> g_events;
AndroidAppOpaque* g_appSeen = nullptr;
pthread_t g_mainThread;
bool g_callbackOnOtherThread = false;

void RealAndroidMain(AndroidAppOpaque* app) {
  g_appSeen = app;
  g_events.push_back("android_main");
}
void RecordingEnder() noexcept { g_events.push_back("end"); }
void RecordingBeforeExit() noexcept {
  g_events.push_back("before_exit");
  g_callbackOnOtherThread = pthread_equal(pthread_self(), g_mainThread) == 0;
}

pthread_mutex_t g_blockMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_blockCond = PTHREAD_COND_INITIALIZER;
bool g_release = false;
void BlockingBeforeExit() noexcept {
  g_events.push_back("before_exit_started");
  pthread_mutex_lock(&g_blockMutex);
  while (!g_release) pthread_cond_wait(&g_blockCond, &g_blockMutex);
  pthread_mutex_unlock(&g_blockMutex);
}

void Reset() {
  g_events.clear();
  g_appSeen = nullptr;
  g_callbackOnOtherThread = false;
  g_release = false;
  g_mainThread = pthread_self();
  pe::ResetForTest();
  pe::MainThunk::Reset();
  *pe::MainThunk::OriginalOut() = reinterpret_cast<void*>(&RealAndroidMain);
}

void AnAndroidMainReturnWithoutTheHookEndsNothing() {
  Reset();  // thunk not armed: the entry calls straight through
  MainEntry()(reinterpret_cast<AndroidAppOpaque*>(0x42));
  QCHECK(g_events.size() == 1 && g_events[0] == "android_main");
  QCHECK(g_appSeen == reinterpret_cast<AndroidAppOpaque*>(0x42));
  QCHECK(pe::CurrentCounts().returned == 0);
}

void TheHandlerCallsTheOriginalThenTheBeforeExitCallbackThenEndsOnce() {
  Reset();
  pe::SetBeforeExit(&RecordingBeforeExit);
  pe::ArmForTest(&RecordingEnder, 500);
  MainEntry()(reinterpret_cast<AndroidAppOpaque*>(0x42));
  QCHECK(g_events.size() == 3);
  if (g_events.size() == 3) {
    QCHECK(g_events[0] == "android_main");
    QCHECK(g_events[1] == "before_exit");
    QCHECK(g_events[2] == "end");
  }
  QCHECK(g_appSeen == reinterpret_cast<AndroidAppOpaque*>(0x42));  // the game's own argument reached it
  QCHECK(g_callbackOnOtherThread);  // a helper thread, so a stuck callback cannot hold the process
  QCHECK(pe::CurrentCounts().returned == 1);
}

void WithoutACallbackTheProcessStillEnds() {
  Reset();
  pe::ArmForTest(&RecordingEnder, 500);
  MainEntry()(nullptr);
  QCHECK(g_events.size() == 2 && g_events[0] == "android_main" && g_events[1] == "end");
}

void ACallbackThatNeverReturnsDoesNotKeepTheProcessAlive() {
  Reset();
  pe::SetBeforeExit(&BlockingBeforeExit);
  pe::ArmForTest(&RecordingEnder, 50);
  const auto start = std::chrono::steady_clock::now();
  MainEntry()(nullptr);
  const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  QCHECK(g_events.size() == 3 && g_events[2] == "end");  // android_main, before_exit_started, end
  QCHECK(elapsedMs >= 40);    // it did wait for the callback
  QCHECK(elapsedMs < 2000);   // and gave up
  pthread_mutex_lock(&g_blockMutex);
  g_release = true;
  pthread_cond_broadcast(&g_blockCond);
  pthread_mutex_unlock(&g_blockMutex);
}

void EveryReturnEndsAgain() {
  Reset();
  pe::ArmForTest(&RecordingEnder, 500);
  MainEntry()(nullptr);
  MainEntry()(nullptr);
  QCHECK(pe::CurrentCounts().returned == 2);
}

// ---- the saved sign-in state survives the cut ----------------------------------------------------------

std::string g_path;
std::atomic<unsigned> g_writes{0};
std::atomic<bool> g_stop{false};
unsigned g_waitForWrites = 0;
useconds_t g_cutDelayUs = 0;

// Two payloads, each recognisably whole: a letter repeated 192 KiB, framed ":BEGIN:" ... ":END". Built once, so
// the writer spends its time saving, not building.
std::string g_payload[2];
void BuildPayloads() {
  for (int i = 0; i < 2; ++i) g_payload[i] = ":BEGIN:" + std::string(192 * 1024, static_cast<char>('a' + i)) + ":END";
}
bool Whole(const std::string& text) { return text == g_payload[0] || text == g_payload[1]; }

std::string ReadAll(const std::string& path) {
  std::string out;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    char buf[65536];
    std::size_t r;
    while ((r = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, r);
    std::fclose(f);
  }
  return out;
}

void* WriterThread(void*) {
  for (unsigned n = 0; !g_stop.load(std::memory_order_acquire); ++n) {
    std::string error, warning;
    if (nevr::quest_auth::AtomicWrite(g_path, g_payload[n % 2], error, warning)) g_writes.fetch_add(1, std::memory_order_release);
  }
  return nullptr;
}

// The game's android_main stand-in: returns once the writer has saved a few times and a little longer (a different
// delay each iteration), so the end lands at a different point of the next save: before it starts, in the middle
// of the write, in the fsync, around the rename.
void AndroidMainThatWaitsForTheWriter(AndroidAppOpaque*) {
  while (g_writes.load(std::memory_order_acquire) < g_waitForWrites) sched_yield();
  usleep(g_cutDelayUs);
}

void ChildEndsTheProcessUnderTheWriter(unsigned iteration) {
  g_waitForWrites = 2 + iteration % 3;
  g_cutDelayUs = static_cast<useconds_t>((iteration * 173) % 3000);  // 0 to 3 ms, spread over the iterations
  pthread_t writer;
  if (pthread_create(&writer, nullptr, &WriterThread, nullptr) != 0) _exit(3);
  pe::MainThunk::Reset();
  *pe::MainThunk::OriginalOut() = reinterpret_cast<void*>(&AndroidMainThatWaitsForTheWriter);
  pe::ArmForTest(&pe::EndProcess, 0);  // the real ender
  MainEntry()(nullptr);
  _exit(4);  // not reached: the handler ended the process
}

void ASavedFileIsWholeAfterTheProcessIsEndedUnderTheWriter() {
  char tmpl[] = "/tmp/process_end_test_XXXXXX";
  const char* dir = mkdtemp(tmpl);
  QCHECK(dir != nullptr);
  if (dir == nullptr) return;
  g_path = std::string(dir) + "/.credentials.json";
  BuildPayloads();
  int whole = 0;
  for (unsigned i = 0; i < 60; ++i) {
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid == 0) ChildEndsTheProcessUnderTheWriter(i);
    int status = 0;
    QCHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
    QCHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);  // the real ender: _exit(0)
    const std::string text = ReadAll(g_path);
    if (Whole(text)) ++whole;
    else std::fprintf(stderr, "iteration %u: the saved file is torn (%zu bytes)\n", i, text.size());
  }
  QCHECK(whole == 60);
  // A temp file the cut left behind does not stop the next save.
  std::string error, warning;
  QCHECK(nevr::quest_auth::AtomicWrite(g_path, g_payload[1], error, warning));
  QCHECK(ReadAll(g_path) == g_payload[1]);
  std::string cleanup = "rm -rf '" + std::string(dir) + "'";
  QCHECK(std::system(cleanup.c_str()) == 0);
}

// ---- the production install path ----------------------------------------------------------------------------

std::string g_fixtureDir;

// InstallAt on a fixture library whose android_main is called through a PLT slot, as libr15's glue calls it:
// the slot must hold the thunk's entry, and a call through it must run the handler (the after-return step and
// the ender), which only an armed handler with its after-return step published does.
void InstallAtPatchesTheSlotAndTheHandlerRunsForACallThroughIt() {
  Reset();
  void* consumer = dlopen((g_fixtureDir + "/libpefx_consumer.so").c_str(), RTLD_NOW | RTLD_LOCAL);
  QCHECK(consumer != nullptr);
  if (consumer == nullptr) return;
  using CallFn = void (*)(void*);
  using CountFn = int (*)();
  using AppFn = void* (*)();
  CallFn call = reinterpret_cast<CallFn>(dlsym(consumer, "pefx_call_android_main"));
  void* provider = dlopen((g_fixtureDir + "/libpefx_provider.so").c_str(), RTLD_NOW | RTLD_NOLOAD);
  QCHECK(call != nullptr && provider != nullptr);
  if (call == nullptr || provider == nullptr) return;
  CountFn calls = reinterpret_cast<CountFn>(dlsym(provider, "pefx_main_calls"));
  AppFn lastApp = reinterpret_cast<AppFn>(dlsym(provider, "pefx_last_app"));

  call(reinterpret_cast<void*>(0x11));  // not installed: the plain call
  QCHECK(calls() == 1 && g_events.empty());

  pe::SetBeforeExit(&RecordingBeforeExit);
  const sentinel::GotTarget target{"libpefx_consumer.so", "android_main", sentinel::RelocKind::kJumpSlot};
  QCHECK(pe::InstallAt(target, &RecordingEnder));
  call(reinterpret_cast<void*>(0x22));
  QCHECK(calls() == 2);                                  // the original still ran
  QCHECK(lastApp() == reinterpret_cast<void*>(0x22));    // with the game's argument
  QCHECK(g_events.size() == 2 && g_events[0] == "before_exit" && g_events[1] == "end");
  QCHECK(pe::CurrentCounts().returned == 1);
  dlclose(consumer);
}

}  // namespace

int main(int argc, char** argv) {
  g_fixtureDir = argc > 1 ? argv[1] : ".";
  AnAndroidMainReturnWithoutTheHookEndsNothing();
  TheHandlerCallsTheOriginalThenTheBeforeExitCallbackThenEndsOnce();
  WithoutACallbackTheProcessStillEnds();
  ACallbackThatNeverReturnsDoesNotKeepTheProcessAlive();
  EveryReturnEndsAgain();
  InstallAtPatchesTheSlotAndTheHandlerRunsForACallThroughIt();
  ASavedFileIsWholeAfterTheProcessIsEndedUnderTheWriter();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "process_end_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("process_end_hook_test: all checks pass\n");
  return 0;
}
