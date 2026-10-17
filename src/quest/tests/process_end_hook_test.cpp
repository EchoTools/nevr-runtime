// Host test for "the process ends when the game's android_main returns" (#621): the real handler
// (src/quest/sentinel/process_end_hook.cpp), built without exceptions as the sentinel builds it, driven through
// the thunk's entry with a stand-in for libr15's android_main. No GOT slot is touched: got_pinned_test.cpp
// resolves the pinned slot in the real library. The last test ends a real process (fork) under a writer that
// saves the way the Quest code saves its sign-in state, to show the file is never torn.
//
// Built and run by `just test-quest-hooks`.

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

// A payload that is recognisably whole: "N:" + 192 KiB of one letter + ":END".
std::string Payload(unsigned n) {
  return std::to_string(n) + ":" + std::string(192 * 1024, static_cast<char>('a' + n % 26)) + ":END";
}

bool Whole(const std::string& text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string::npos || colon == 0) return false;
  const unsigned n = static_cast<unsigned>(std::strtoul(text.substr(0, colon).c_str(), nullptr, 10));
  return text == Payload(n);
}

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
  for (unsigned n = 1; !g_stop.load(std::memory_order_acquire); ++n) {
    std::string error, warning;
    if (nevr::quest_auth::AtomicWrite(g_path, Payload(n), error, warning)) g_writes.fetch_add(1, std::memory_order_release);
  }
  return nullptr;
}

// The game's android_main stand-in: returns once the writer has saved a few times, so the end comes while
// it is saving the next one.
void AndroidMainThatWaitsForTheWriter(AndroidAppOpaque*) {
  while (g_writes.load(std::memory_order_acquire) < g_waitForWrites) sched_yield();
}

void ChildEndsTheProcessUnderTheWriter(unsigned iteration) {
  g_waitForWrites = 2 + iteration % 5;
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
  int whole = 0;
  for (unsigned i = 0; i < 25; ++i) {
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
  QCHECK(whole == 25);
  // A temp file the cut left behind does not stop the next save.
  std::string error, warning;
  QCHECK(nevr::quest_auth::AtomicWrite(g_path, Payload(7), error, warning));
  QCHECK(ReadAll(g_path) == Payload(7));
  std::string cleanup = "rm -rf '" + std::string(dir) + "'";
  QCHECK(std::system(cleanup.c_str()) == 0);
}

}  // namespace

int main() {
  AnAndroidMainReturnWithoutTheHookEndsNothing();
  TheHandlerCallsTheOriginalThenTheBeforeExitCallbackThenEndsOnce();
  WithoutACallbackTheProcessStillEnds();
  ACallbackThatNeverReturnsDoesNotKeepTheProcessAlive();
  EveryReturnEndsAgain();
  ASavedFileIsWholeAfterTheProcessIsEndedUnderTheWriter();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "process_end_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("process_end_hook_test: all checks pass\n");
  return 0;
}
