#include "quest/invite/frame_dispatch.h"

#include <pthread.h>

#include <time.h>

#include <atomic>

#include "quest/sentinel/hook_log.h"

namespace nevr_quest::invite::frame {

namespace {

using ClockFn = std::uint64_t (*)() noexcept;

struct Unit {
  WorkFn fn;
  void* context;
};

// Locks one pthread mutex for a scope. A try Guard takes the lock only when it is free; owns() says whether it did,
// and the destructor releases the lock only then. A blocking Guard waits and always holds the lock. pthread rather
// than std::mutex: a std::mutex global is a dynamic initializer with an atexit registration, which
// tools/check_quest_static_init.sh refuses in the sentinel (see src/quest/sentinel/sentinel_log.cpp).
class Guard {
 public:
  Guard(pthread_mutex_t& mutex, bool tryOnly) noexcept : mutex_(&mutex), owned_(true) {
    if (tryOnly) {
      owned_ = pthread_mutex_trylock(&mutex) == 0;
    } else {
      pthread_mutex_lock(&mutex);
    }
  }
  ~Guard() {
    if (owned_) pthread_mutex_unlock(mutex_);
  }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
  bool owns() const noexcept { return owned_; }

 private:
  pthread_mutex_t* mutex_;
  bool owned_;
};

// Held for O(1) work only: the registrations and the queue. Tick try-locks it; a post waits for it.
pthread_mutex_t g_queueMutex = PTHREAD_MUTEX_INITIALIZER;
// Held by the one Tick that drains. A second Tick try-locks it, finds it held and returns. Lock order: drain, then queue.
pthread_mutex_t g_drainMutex = PTHREAD_MUTEX_INITIALIZER;

Unit g_queue[kQueueCapacity] = {};
std::size_t g_queueHead = 0;   // index of the oldest unit
std::size_t g_queueCount = 0;  // units waiting
Unit g_frameWork[kMaxFrameWork] = {};
std::size_t g_frameWorkCount = 0;

std::atomic<std::uint64_t> g_frames{0};
std::atomic<std::uint64_t> g_ran{0};
std::atomic<std::uint64_t> g_failed{0};
std::atomic<std::uint64_t> g_dropped{0};
std::atomic<std::uint64_t> g_skipped{0};
// The first failure and the first drop of each heartbeat window are logged; the rest are only counted.
std::atomic<bool> g_failureLogged{false};
std::atomic<bool> g_dropLogged{false};

std::atomic<ClockFn> g_clock{nullptr};

// Heartbeat state. Touched only under g_drainMutex.
bool g_windowOpen = false;
std::uint64_t g_windowStartNs = 0;
Stats g_reported{0, 0, 0, 0, 0};

// clock_gettime, not std::chrono::steady_clock: a function a hook can reach may not carry a personality, and libc++'s
// steady_clock::now does (tests/quest TestHookFramesCarryNoPersonality). It reads the monotonic clock the sentinel's
// other hook-path timers read (sentinel_log.cpp MonotonicMs).
std::uint64_t SteadyNowNs() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000U + static_cast<std::uint64_t>(ts.tv_nsec);
}

std::uint64_t NowNs() noexcept {
  const ClockFn clock = g_clock.load(std::memory_order_acquire);
  return clock != nullptr ? clock() : SteadyNowNs();
}

// The heartbeat: once per window, one INFO line with what the window's Tick calls did. Under g_drainMutex.
void MaybeHeartbeat(std::uint64_t now) noexcept {
  if (!g_windowOpen) {
    g_windowOpen = true;
    g_windowStartNs = now;
    return;
  }
  if (now - g_windowStartNs < kHeartbeatNs) return;
  const Stats total = CurrentStats();
  const std::uint64_t windowMs = (now - g_windowStartNs) / 1000000ULL;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "invite_frame_dispatch",
                      {{"tag", "NEVR.INVITE"},
                       {"result", "heartbeat"},
                       {"hook", "CR15NetGame::Update"},
                       {"frames", total.frames - g_reported.frames},
                       {"window_ms", windowMs},
                       {"ran", total.ran - g_reported.ran},
                       {"failed", total.failed - g_reported.failed},
                       {"dropped", total.dropped - g_reported.dropped},
                       {"skipped", total.skipped - g_reported.skipped}});
  g_reported = total;
  g_windowStartNs = now;
  g_failureLogged.store(false, std::memory_order_relaxed);
  g_dropLogged.store(false, std::memory_order_relaxed);
}

// Runs one unit. A unit that returns false is counted and, the first time per window, logged; the drain goes on.
void RunUnit(const Unit& unit, bool persistent, std::size_t index) noexcept {
  if (unit.fn(unit.context)) {
    g_ran.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_failed.fetch_add(1, std::memory_order_relaxed);
  if (!g_failureLogged.exchange(true, std::memory_order_relaxed)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "invite_frame_dispatch",
                        {{"tag", "NEVR.INVITE"},
                         {"result", "item_failed"},
                         {"kind", persistent ? "frame" : "posted"},
                         {"index", static_cast<long long>(index)}});
  }
}

}  // namespace

bool RegisterFrameWork(WorkFn fn, void* context) noexcept {
  if (fn == nullptr) return false;
  const Guard lock(g_queueMutex, false);
  if (g_frameWorkCount >= kMaxFrameWork) return false;
  g_frameWork[g_frameWorkCount] = Unit{fn, context};
  ++g_frameWorkCount;
  return true;
}

bool PostFrameWork(WorkFn fn, void* context) noexcept {
  if (fn == nullptr) return false;
  bool queued = false;
  {
    const Guard lock(g_queueMutex, false);
    if (g_queueCount < kQueueCapacity) {
      g_queue[(g_queueHead + g_queueCount) % kQueueCapacity] = Unit{fn, context};
      ++g_queueCount;
      queued = true;
    }
  }
  if (!queued) {
    g_dropped.fetch_add(1, std::memory_order_relaxed);
    if (!g_dropLogged.exchange(true, std::memory_order_relaxed)) {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "invite_frame_dispatch",
                          {{"tag", "NEVR.INVITE"},
                           {"result", "dropped"},
                           {"capacity", static_cast<long long>(kQueueCapacity)}});
    }
  }
  return queued;
}

void Tick() noexcept {
  g_frames.fetch_add(1, std::memory_order_relaxed);
  const Guard drain(g_drainMutex, true);
  if (!drain.owns()) {
    g_skipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  MaybeHeartbeat(NowNs());

  // Copy the work out under the queue lock, then run it with no lock held, so a unit may post (the post takes the queue
  // lock). A unit posted from here waits for the next Tick: the copy below is the batch of this Tick.
  Unit frameWork[kMaxFrameWork] = {};
  std::size_t frameCount = 0;
  Unit batch[kPerTickLimit] = {};
  std::size_t batchCount = 0;
  {
    const Guard queue(g_queueMutex, true);
    if (!queue.owns()) {
      g_skipped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    frameCount = g_frameWorkCount;
    for (std::size_t i = 0; i < frameCount; ++i) frameWork[i] = g_frameWork[i];
    while (batchCount < kPerTickLimit && g_queueCount > 0) {
      batch[batchCount] = g_queue[g_queueHead];
      ++batchCount;
      g_queueHead = (g_queueHead + 1) % kQueueCapacity;
      --g_queueCount;
    }
  }
  for (std::size_t i = 0; i < frameCount; ++i) RunUnit(frameWork[i], true, i);
  for (std::size_t i = 0; i < batchCount; ++i) RunUnit(batch[i], false, i);
}

void LogInstall(bool installed, const char* reason) noexcept {
  if (installed) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "invite_frame_dispatch",
                        {{"tag", "NEVR.INVITE"}, {"result", "installed"}, {"hook", "CR15NetGame::Update"}});
    return;
  }
  sentinel::LogFields(sentinel::LogLevel::kWarn, "invite_frame_dispatch",
                      {{"tag", "NEVR.INVITE"},
                       {"result", "off"},
                       {"hook", "CR15NetGame::Update"},
                       {"reason", reason != nullptr ? reason : "unknown"}});
}

Stats CurrentStats() noexcept {
  return Stats{g_frames.load(std::memory_order_relaxed), g_ran.load(std::memory_order_relaxed),
               g_failed.load(std::memory_order_relaxed), g_dropped.load(std::memory_order_relaxed),
               g_skipped.load(std::memory_order_relaxed)};
}

void ResetForTest() noexcept {
  const Guard drain(g_drainMutex, false);
  const Guard queue(g_queueMutex, false);
  g_queueHead = 0;
  g_queueCount = 0;
  g_frameWorkCount = 0;
  g_frames.store(0, std::memory_order_relaxed);
  g_ran.store(0, std::memory_order_relaxed);
  g_failed.store(0, std::memory_order_relaxed);
  g_dropped.store(0, std::memory_order_relaxed);
  g_skipped.store(0, std::memory_order_relaxed);
  g_failureLogged.store(false, std::memory_order_relaxed);
  g_dropLogged.store(false, std::memory_order_relaxed);
  g_windowOpen = false;
  g_windowStartNs = 0;
  g_reported = Stats{0, 0, 0, 0, 0};
}

void SetClockForTest(ClockFn clock) noexcept { g_clock.store(clock, std::memory_order_release); }

pthread_mutex_t& DrainMutexForTest() noexcept { return g_drainMutex; }
pthread_mutex_t& QueueMutexForTest() noexcept { return g_queueMutex; }

}  // namespace nevr_quest::invite::frame
