// Host test for the Quest frame dispatcher (src/quest/invite/frame_dispatch.h, #966 S7).
// Built by `just test-quest-hooks` with the same flags as the sentinel (-fno-exceptions, -Werror).
#include "quest/invite/frame_dispatch.h"

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "quest/sentinel/hook_log.h"

namespace fd = nevr_quest::invite::frame;

namespace {

int g_failures = 0;
#define FD_CHECK(cond)                                                          \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++g_failures;                                                             \
    }                                                                           \
  } while (0)

// The sink. LogFields may run on a posting thread, so the capture is locked.
std::mutex g_linesMutex;
std::vector<std::string> g_lines;
std::vector<sentinel::LogLevel> g_levels;

void CaptureSink(sentinel::LogLevel level, const char* line) {
  std::lock_guard<std::mutex> lock(g_linesMutex);
  g_lines.emplace_back(line);
  g_levels.push_back(level);
}

int CountLines(const char* needle) {
  std::lock_guard<std::mutex> lock(g_linesMutex);
  int n = 0;
  for (const std::string& line : g_lines) {
    if (line.find(needle) != std::string::npos) ++n;
  }
  return n;
}

// The line that holds `needle`, or an empty string.
std::string FirstLineWith(const char* needle) {
  std::lock_guard<std::mutex> lock(g_linesMutex);
  for (const std::string& line : g_lines) {
    if (line.find(needle) != std::string::npos) return line;
  }
  return std::string();
}

// The game-thread side: the work items record what ran, in order. Items run inside Tick on the calling thread.
std::vector<int> g_order;
int g_failId = -1;  // the item with this id returns false
int g_failId2 = -1;  // and so does the item with this id
int g_chainNext = -1;  // ChainPush posts this id when it runs

std::uint64_t g_fakeNs = 0;
std::uint64_t FakeClock() noexcept { return g_fakeNs; }

bool Push(void* context) noexcept {
  const int id = *static_cast<int*>(context);
  if (id == g_failId || id == g_failId2) return false;
  g_order.push_back(id);
  return true;
}

bool ChainPush(void* context) noexcept {
  const int id = *static_cast<int*>(context);
  g_order.push_back(id);
  if (g_chainNext >= 0) {
    static int next = 0;
    next = g_chainNext;
    g_chainNext = -1;
    fd::PostFrameWork(&Push, &next);  // posted while a Tick drains: must wait for the next Tick
  }
  return true;
}

void Reset() {
  fd::ResetForTest();
  fd::SetClockForTest(&FakeClock);
  g_fakeNs = 0;
  g_order.clear();
  g_failId = -1;
  g_failId2 = -1;
  g_chainNext = -1;
  std::lock_guard<std::mutex> lock(g_linesMutex);
  g_lines.clear();
  g_levels.clear();
}

bool OrderIs(const std::vector<int>& want) { return g_order == want; }

// 1. Posted work runs only on Tick, once each, in the order it was posted.
void PostedWorkRunsOnlyOnTickOnceInOrder() {
  Reset();
  static int a = 1, b = 2, c = 3;
  FD_CHECK(fd::PostFrameWork(&Push, &a));
  FD_CHECK(fd::PostFrameWork(&Push, &b));
  FD_CHECK(fd::PostFrameWork(&Push, &c));
  FD_CHECK(g_order.empty());  // nothing ran at post time
  fd::Tick();
  FD_CHECK(OrderIs({1, 2, 3}));
  fd::Tick();
  FD_CHECK(OrderIs({1, 2, 3}));  // not run again
}

// 2. Posts from other threads, while the game thread ticks, all run, each producer's units in order.
void PostsFromAnotherThreadRun() {
  Reset();
  static int ids[200];
  for (int i = 0; i < 200; ++i) ids[i] = i;
  // A producer gives up after a bounded number of refusals, so a broken dispatcher fails the check instead of hanging.
  std::atomic<int> gaveUp{0};
  auto producer = [&gaveUp](int first) {
    for (int i = first; i < first + 100; ++i) {
      int tries = 0;
      while (!fd::PostFrameWork(&Push, &ids[i])) {
        if (++tries > 200000) {
          gaveUp.fetch_add(1);
          return;
        }
        std::this_thread::yield();
      }
    }
  };
  std::thread t1(producer, 0);
  std::thread t2(producer, 100);
  for (int spin = 0; spin < 200000 && g_order.size() < 200; ++spin) {
    fd::Tick();
    std::this_thread::yield();
  }
  t1.join();
  t2.join();
  for (int spin = 0; spin < 1000 && g_order.size() < 200; ++spin) fd::Tick();
  FD_CHECK(gaveUp.load() == 0);
  FD_CHECK(g_order.size() == 200);
  int lastA = -1, lastB = 99;
  bool inOrder = true;
  for (int id : g_order) {
    if (id < 100) {
      if (id <= lastA) inOrder = false;
      lastA = id;
    } else {
      if (id <= lastB) inOrder = false;
      lastB = id;
    }
  }
  FD_CHECK(inOrder);
}

// 3. Registered frame work runs on every Tick, before the posted work of that Tick.
void RegisteredWorkRunsEveryTick() {
  Reset();
  static int regId = 7, posted = 1;
  FD_CHECK(fd::RegisterFrameWork(&Push, &regId));
  FD_CHECK(fd::PostFrameWork(&Push, &posted));
  fd::Tick();
  FD_CHECK(OrderIs({7, 1}));
  fd::Tick();
  FD_CHECK(OrderIs({7, 1, 7}));
}

// Holds `lockToHold` from another thread for up to a second, or until the Tick has returned, and returns how long
// the Tick took. A Tick that blocks on the lock shows as a slow one, and the holder still releases the lock, so the test
// cannot hang.
std::chrono::steady_clock::duration TickWhileHeld(pthread_mutex_t& lockToHold) {
  std::atomic<bool> held{false};
  std::atomic<bool> tickReturned{false};
  std::thread holder([&]() {
    pthread_mutex_lock(&lockToHold);
    held.store(true);
    for (int i = 0; i < 200 && !tickReturned.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    pthread_mutex_unlock(&lockToHold);
  });
  while (!held.load()) std::this_thread::yield();
  const auto start = std::chrono::steady_clock::now();
  fd::Tick();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  tickReturned.store(true);
  holder.join();
  return elapsed;
}

// 3b. Registration holds kMaxFrameWork and refuses more; a null unit is refused by both calls.
void RegistrationLimitAndNullRefused() {
  Reset();
  // The null checks come first: once the registry is full, the cap would refuse a null unit for another reason.
  FD_CHECK(!fd::RegisterFrameWork(nullptr, nullptr));
  FD_CHECK(!fd::PostFrameWork(nullptr, nullptr));
  static int regs[fd::kMaxFrameWork + 1];
  for (std::size_t i = 0; i < fd::kMaxFrameWork; ++i) {
    regs[i] = static_cast<int>(i);
    FD_CHECK(fd::RegisterFrameWork(&Push, &regs[i]));
  }
  regs[fd::kMaxFrameWork] = 99;
  FD_CHECK(!fd::RegisterFrameWork(&Push, &regs[fd::kMaxFrameWork]));
}

// 4. A Tick that finds the drain held returns at once, runs nothing and counts a skip; the work runs on a later free Tick.
void ContendedDrainSkipsWithoutBlocking() {
  Reset();
  static int ids[5] = {10, 11, 12, 13, 14};
  for (int i = 0; i < 5; ++i) FD_CHECK(fd::PostFrameWork(&Push, &ids[i]));
  const auto elapsed = TickWhileHeld(fd::DrainMutexForTest());
  FD_CHECK(elapsed < std::chrono::milliseconds(500));
  FD_CHECK(g_order.empty());
  FD_CHECK(fd::CurrentStats().skipped == 1);
  fd::Tick();
  FD_CHECK(OrderIs({10, 11, 12, 13, 14}));
}

// 4b. The same when the queue is held (a post in flight): the Tick skips, and the work waits for the next free Tick.
void ContendedQueueSkipsWithoutBlocking() {
  Reset();
  static int ids[3] = {20, 21, 22};
  for (int i = 0; i < 3; ++i) FD_CHECK(fd::PostFrameWork(&Push, &ids[i]));
  const auto elapsed = TickWhileHeld(fd::QueueMutexForTest());
  FD_CHECK(elapsed < std::chrono::milliseconds(500));
  FD_CHECK(g_order.empty());
  FD_CHECK(fd::CurrentStats().skipped == 1);
  fd::Tick();
  FD_CHECK(OrderIs({20, 21, 22}));
}

// Holds `lockToHold` from another thread for 100 ms and returns how long ResetForTest took to return. A reset that
// takes both locks waits for the holder; one that does not returns at once.
std::chrono::steady_clock::duration ResetWhileHeld(pthread_mutex_t& lockToHold) {
  std::atomic<bool> held{false};
  std::thread holder([&]() {
    pthread_mutex_lock(&lockToHold);
    held.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pthread_mutex_unlock(&lockToHold);
  });
  while (!held.load()) std::this_thread::yield();
  const auto start = std::chrono::steady_clock::now();
  fd::ResetForTest();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  holder.join();
  return elapsed;
}

// 4c. ResetForTest waits for a Tick that holds the drain or the queue, so it never clears state under a drain.
void ResetWaitsForHeldLocks() {
  Reset();
  FD_CHECK(ResetWhileHeld(fd::DrainMutexForTest()) >= std::chrono::milliseconds(50));
  FD_CHECK(ResetWhileHeld(fd::QueueMutexForTest()) >= std::chrono::milliseconds(50));
}

// 4d. A post waits for a Tick that holds the queue; contention does not refuse it.
void PostWaitsForTheQueueLock() {
  Reset();
  static int id = 30;
  std::atomic<bool> held{false};
  std::thread holder([&]() {
    pthread_mutex_lock(&fd::QueueMutexForTest());
    held.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pthread_mutex_unlock(&fd::QueueMutexForTest());
  });
  while (!held.load()) std::this_thread::yield();
  const auto start = std::chrono::steady_clock::now();
  const bool queued = fd::PostFrameWork(&Push, &id);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  holder.join();
  FD_CHECK(queued);
  FD_CHECK(elapsed >= std::chrono::milliseconds(50));
  FD_CHECK(fd::CurrentStats().dropped == 0);
  fd::Tick();
  FD_CHECK(OrderIs({30}));
}

// 4e. A post does not wait for a Tick that drains: the drain lock belongs to the Tick, not to the post.
void PostIsNotHeldByADrain() {
  Reset();
  static int id = 40;
  std::atomic<bool> held{false};
  std::thread holder([&]() {
    pthread_mutex_lock(&fd::DrainMutexForTest());
    held.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pthread_mutex_unlock(&fd::DrainMutexForTest());
  });
  while (!held.load()) std::this_thread::yield();
  const auto start = std::chrono::steady_clock::now();
  const bool queued = fd::PostFrameWork(&Push, &id);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  holder.join();
  FD_CHECK(queued);
  FD_CHECK(elapsed < std::chrono::milliseconds(50));
  fd::Tick();  // the drain is free again: the post runs on this Tick
  FD_CHECK(OrderIs({40}));
}

// 5. A unit that returns false is counted and logged, and the next unit still runs.
void FailingUnitIsContainedAndLogged() {
  Reset();
  static int ids[4] = {1, 2, 3, 4};
  g_failId = 2;
  g_failId2 = 4;
  for (int i = 0; i < 4; ++i) FD_CHECK(fd::PostFrameWork(&Push, &ids[i]));
  fd::Tick();
  FD_CHECK(OrderIs({1, 3}));
  const fd::Stats s = fd::CurrentStats();
  FD_CHECK(s.ran == 2);
  FD_CHECK(s.failed == 2);
  FD_CHECK(CountLines("\"result\":\"item_failed\"") == 1);  // two failures, one line per window
  FD_CHECK(CountLines("\"tag\":\"NEVR.INVITE\"") >= 1);
  std::lock_guard<std::mutex> lock(g_linesMutex);
  bool warned = false;
  for (std::size_t i = 0; i < g_lines.size(); ++i) {
    if (g_lines[i].find("\"result\":\"item_failed\"") != std::string::npos && g_levels[i] == sentinel::LogLevel::kWarn) {
      warned = true;
    }
  }
  FD_CHECK(warned);
}

// 6. At most kPerTickLimit posted units run in one Tick; the rest wait.
void PerTickLimitBoundsEachTick() {
  Reset();
  static int ids[100];
  for (int i = 0; i < 100; ++i) {
    ids[i] = i;
    FD_CHECK(fd::PostFrameWork(&Push, &ids[i]));
  }
  fd::Tick();
  FD_CHECK(g_order.size() == fd::kPerTickLimit);
  fd::Tick();
  FD_CHECK(g_order.size() == 100);
  FD_CHECK(!g_order.empty() && g_order.back() == 99);
}

// 7. A full queue refuses the post, counts a drop and logs the first one.
void FullQueueRefusesAndCountsDrops() {
  Reset();
  static int ids[fd::kQueueCapacity + 2];
  for (std::size_t i = 0; i < fd::kQueueCapacity; ++i) {
    ids[i] = static_cast<int>(i);
    FD_CHECK(fd::PostFrameWork(&Push, &ids[i]));
  }
  ids[fd::kQueueCapacity] = -5;
  ids[fd::kQueueCapacity + 1] = -6;
  FD_CHECK(!fd::PostFrameWork(&Push, &ids[fd::kQueueCapacity]));
  FD_CHECK(!fd::PostFrameWork(&Push, &ids[fd::kQueueCapacity + 1]));
  FD_CHECK(fd::CurrentStats().dropped == 2);
  FD_CHECK(CountLines("\"result\":\"dropped\"") == 1);  // two drops, one line per window
}

// 8. A unit posted by a unit that is running waits for the next Tick.
void PostedDuringDrainRunsNextTick() {
  Reset();
  static int first = 1;
  g_chainNext = 2;
  FD_CHECK(fd::PostFrameWork(&ChainPush, &first));
  fd::Tick();
  FD_CHECK(OrderIs({1}));
  fd::Tick();
  FD_CHECK(OrderIs({1, 2}));
}

// 9. One heartbeat per minute, counting the Tick calls of that minute.
void HeartbeatOncePerMinuteWithFrames() {
  Reset();
  fd::Tick();
  fd::Tick();
  fd::Tick();  // the first Tick opens the window at t=0
  FD_CHECK(CountLines("\"result\":\"heartbeat\"") == 0);
  g_fakeNs = 59ULL * 1000000000ULL;
  fd::Tick();
  FD_CHECK(CountLines("\"result\":\"heartbeat\"") == 0);
  g_fakeNs = 60ULL * 1000000000ULL;
  fd::Tick();
  FD_CHECK(CountLines("\"result\":\"heartbeat\"") == 1);
  const std::string line = FirstLineWith("\"result\":\"heartbeat\"");
  FD_CHECK(line.find("\"frames\":5") != std::string::npos);
  FD_CHECK(line.find("\"window_ms\":60000") != std::string::npos);
  g_fakeNs = 61ULL * 1000000000ULL;
  fd::Tick();
  FD_CHECK(CountLines("\"result\":\"heartbeat\"") == 1);
  g_fakeNs = 120ULL * 1000000000ULL;
  fd::Tick();  // the second window opened at 60 s and holds the Tick calls at 61 s and 120 s: two
  FD_CHECK(CountLines("\"result\":\"heartbeat\"") == 2);
  FD_CHECK(CountLines("\"frames\":2,\"window_ms\":60000") == 1);
}

// 10. The install line names the hook and the result; an off line names the reason.
void InstallLineNamesHookAndResult() {
  Reset();
  fd::LogInstall(true, "");
  fd::LogInstall(false, "slot_changed");
  const std::string installed = FirstLineWith("\"result\":\"installed\"");
  FD_CHECK(installed.find("\"tag\":\"NEVR.INVITE\"") != std::string::npos);
  FD_CHECK(installed.find("\"hook\":\"CR15NetGame::Update\"") != std::string::npos);
  FD_CHECK(installed.find("\"event\":\"invite_frame_dispatch\"") != std::string::npos);
  const std::string off = FirstLineWith("\"result\":\"off\"");
  FD_CHECK(off.find("\"reason\":\"slot_changed\"") != std::string::npos);
  FD_CHECK(off.find("\"hook\":\"CR15NetGame::Update\"") != std::string::npos);
  std::lock_guard<std::mutex> lock(g_linesMutex);
  FD_CHECK(g_levels.size() == 2);
  if (g_levels.size() == 2) {
    FD_CHECK(g_levels[0] == sentinel::LogLevel::kInfo);
    FD_CHECK(g_levels[1] == sentinel::LogLevel::kWarn);
  }
}

}  // namespace

int main() {
  sentinel::SetLogSink(&CaptureSink);
  PostedWorkRunsOnlyOnTickOnceInOrder();
  PostsFromAnotherThreadRun();
  RegisteredWorkRunsEveryTick();
  RegistrationLimitAndNullRefused();
  ContendedDrainSkipsWithoutBlocking();
  ContendedQueueSkipsWithoutBlocking();
  ResetWaitsForHeldLocks();
  PostWaitsForTheQueueLock();
  PostIsNotHeldByADrain();
  FailingUnitIsContainedAndLogged();
  PerTickLimitBoundsEachTick();
  FullQueueRefusesAndCountsDrops();
  PostedDuringDrainRunsNextTick();
  HeartbeatOncePerMinuteWithFrames();
  InstallLineNamesHookAndResult();
  fd::ResetForTest();
  sentinel::SetLogSink(nullptr);
  if (g_failures != 0) {
    std::fprintf(stderr, "frame_dispatch_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("frame_dispatch_test: all checks pass\n");
  return 0;
}
