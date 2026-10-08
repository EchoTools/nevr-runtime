#include "hook_report.h"

#include <pthread.h>
#include <time.h>

#include "hook_log.h"

namespace sentinel {

namespace {

constexpr unsigned kMaxCounters = 8;

struct Entry {
  const char* name = nullptr;
  const std::atomic<std::uint64_t>* value = nullptr;
  std::uint64_t lastLogged = 0;
  bool seen = false;
};

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond;
bool g_condReady = false;
pthread_t g_thread;
bool g_running = false;
bool g_stop = false;
unsigned g_firstMs = 0;
unsigned g_steadyMs = 0;
Entry g_counters[kMaxCounters];
unsigned g_counterCount = 0;

// Absolute CLOCK_MONOTONIC deadline `ms` from now (the condition variable uses that clock).
timespec Deadline(unsigned ms) {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }
  return ts;
}

void Report(const Entry& e, std::uint64_t now, const char* why) {
  LogFields(LogLevel::kInfo, "hook_counter",
            {{"counter", e.name}, {"value", static_cast<long long>(now)}, {"why", why}});
}

// One pass over the counters; returns whether every registered counter has been seen non-zero.
// `steady` is true once the steady cadence applies.
void Pass(bool* allSeen) {
  bool all = true;
  for (unsigned i = 0; i < g_counterCount; ++i) {
    Entry& e = g_counters[i];
    const std::uint64_t now = e.value->load(std::memory_order_relaxed);
    if (!e.seen) {
      if (now != 0) {
        e.seen = true;
        e.lastLogged = now;
        Report(e, now, "first_change");
      } else {
        all = false;
      }
    } else if (now != e.lastLogged) {
      e.lastLogged = now;
      Report(e, now, "changed");
    }
  }
  *allSeen = all;
}

void* ReporterMain(void*) {
  pthread_mutex_lock(&g_mutex);
  bool allSeen = false;
  while (!g_stop) {
    // Fast cadence until every counter has fired once, then the steady one.
    const timespec deadline = Deadline(allSeen ? g_steadyMs : g_firstMs);
    pthread_cond_timedwait(&g_cond, &g_mutex, &deadline);
    if (g_stop) break;
    Pass(&allSeen);
  }
  pthread_mutex_unlock(&g_mutex);
  return nullptr;
}

}  // namespace

bool RegisterReportCounter(const char* name, const std::atomic<std::uint64_t>* value) {
  pthread_mutex_lock(&g_mutex);
  bool ok = !g_running && g_counterCount < kMaxCounters && name != nullptr && value != nullptr;
  if (ok) {
    g_counters[g_counterCount] = Entry{name, value, 0, false};
    ++g_counterCount;
  }
  pthread_mutex_unlock(&g_mutex);
  if (!ok) {
    LogFields(LogLevel::kError, "hook_report",
              {{"status", "register_refused"}, {"counter", name != nullptr ? name : "(null)"}});
  }
  return ok;
}

bool StartReporter(unsigned firstMs, unsigned steadyMs) {
  pthread_mutex_lock(&g_mutex);
  if (g_running) {
    pthread_mutex_unlock(&g_mutex);
    return true;
  }
  if (!g_condReady) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cond, &attr);
    pthread_condattr_destroy(&attr);
    g_condReady = true;
  }
  g_stop = false;
  g_firstMs = firstMs;
  g_steadyMs = steadyMs;
  const int rc = pthread_create(&g_thread, nullptr, &ReporterMain, nullptr);
  g_running = rc == 0;
  pthread_mutex_unlock(&g_mutex);
  if (rc != 0) {
    LogFields(LogLevel::kError, "hook_report", {{"status", "thread_create_failed"}, {"errno", rc}});
    return false;
  }
  return true;
}

void StopReporter() {
  pthread_mutex_lock(&g_mutex);
  const bool running = g_running;
  if (running) {
    g_stop = true;
    pthread_cond_signal(&g_cond);
  }
  pthread_mutex_unlock(&g_mutex);
  if (running) pthread_join(g_thread, nullptr);
  pthread_mutex_lock(&g_mutex);
  g_running = false;
  g_counterCount = 0;
  pthread_mutex_unlock(&g_mutex);
}

}  // namespace sentinel
