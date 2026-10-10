#include "hook_report.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "hook_log.h"

namespace sentinel {

namespace {

// Static storage, no heap. Every package that hooks registers its own counters (calls, faults,
// outcomes), so this is sized for all of them together.
constexpr unsigned kMaxCounters = kMaxReportCounters;

struct Counter {
  const char* name = nullptr;
  const std::atomic<std::uint64_t>* value = nullptr;
  std::uint64_t lastLogged = 0;
  bool seen = false;
  bool faultsOnly = false;  // zero is the healthy state: never reported never_fired
};

// One line decided under the lock and written after it is released.
struct Event {
  const char* name;
  std::uint64_t value;
  const char* why;
};

// g_control serialises Start/Stop/Register as a whole (thread creation and join included);
// g_mutex guards the state the reporter thread reads. Always taken in that order.
pthread_mutex_t g_control = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond;
bool g_condReady = false;
bool g_atforkRegistered = false;
pthread_t g_thread;
bool g_running = false;
bool g_stop = false;
unsigned g_firstMs = 0;
unsigned g_graceMs = 0;
unsigned g_steadyMs = 0;
Counter g_counters[kMaxCounters];
unsigned g_counterCount = 0;

// Absolute CLOCK_MONOTONIC time `ms` after `from`.
timespec After(const timespec& from, unsigned ms) {
  timespec ts = from;
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }
  return ts;
}

timespec Now() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts;
}

bool Before(const timespec& a, const timespec& b) {
  return a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec);
}

void Emit(const Event* events, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    LogFields(LogLevel::kInfo, "hook_counter",
              {{"counter", events[i].name},
               {"value", static_cast<long long>(events[i].value)},
               {"why", events[i].why}});
  }
}

// Decides what to report for one pass; called with the mutex held. `steady` is true on the
// steady passes after the grace window (they may log "changed"); `closingGrace` is true on the
// pass that ends the grace window (it logs "never_fired", and never "changed").
unsigned Decide(bool steady, bool closingGrace, Event* out) {
  unsigned n = 0;
  for (unsigned i = 0; i < g_counterCount && n < kMaxCounters; ++i) {
    Counter& c = g_counters[i];
    const std::uint64_t now = c.value->load(std::memory_order_relaxed);
    if (!c.seen) {
      if (now != 0) {
        c.seen = true;
        c.lastLogged = now;
        out[n++] = Event{c.name, now, "first_change"};
      } else if (closingGrace && !c.faultsOnly) {
        out[n++] = Event{c.name, 0, "never_fired"};
      }
    } else if (steady && now != c.lastLogged) {
      c.lastLogged = now;
      out[n++] = Event{c.name, now, "changed"};
    }
  }
  return n;
}

void* ReporterMain(void*) {
  pthread_mutex_lock(&g_mutex);
  const timespec start = Now();
  const timespec graceEnd = After(start, g_graceMs);
  bool graceClosed = false;
  while (!g_stop) {
    const timespec now = Now();
    // Fast ticks until the grace window ends (the last tick lands on its end), then steady.
    timespec deadline;
    if (!graceClosed) {
      deadline = After(now, g_firstMs);
      if (Before(graceEnd, deadline)) deadline = graceEnd;
    } else {
      deadline = After(now, g_steadyMs);
    }
    // A spurious wakeup returns 0 without g_stop; wait again for the same deadline.
    int rc = 0;
    while (!g_stop && rc != ETIMEDOUT) rc = pthread_cond_timedwait(&g_cond, &g_mutex, &deadline);
    if (g_stop) break;
    const bool closing = !graceClosed && !Before(Now(), graceEnd);
    if (closing) graceClosed = true;
    Event events[kMaxCounters];
    const unsigned n = Decide(graceClosed && !closing, closing, events);
    // Log with the mutex released: a line never blocks Register/Stop, and a fork cannot
    // catch the lock held across logging.
    pthread_mutex_unlock(&g_mutex);
    Emit(events, n);
    pthread_mutex_lock(&g_mutex);
  }
  pthread_mutex_unlock(&g_mutex);
  return nullptr;
}

void AtforkPrepare() {
  pthread_mutex_lock(&g_control);
  pthread_mutex_lock(&g_mutex);
}
void AtforkParent() {
  pthread_mutex_unlock(&g_mutex);
  pthread_mutex_unlock(&g_control);
}
void AtforkChild() {
  g_running = false;  // the reporter thread does not exist in the child
  pthread_mutex_unlock(&g_mutex);
  pthread_mutex_unlock(&g_control);
}

}  // namespace

bool RegisterReportCounter(const char* name, const std::atomic<std::uint64_t>* value, ReportKind kind) {
  pthread_mutex_lock(&g_control);
  pthread_mutex_lock(&g_mutex);
  const bool ok = !g_running && g_counterCount < kMaxCounters && name != nullptr && value != nullptr;
  if (ok) {
    g_counters[g_counterCount] = Counter{name, value, 0, false, kind == ReportKind::kFaults};
    ++g_counterCount;
  }
  pthread_mutex_unlock(&g_mutex);
  pthread_mutex_unlock(&g_control);
  if (!ok) {
    LogFields(LogLevel::kError, "hook_report",
              {{"status", "register_refused"}, {"counter", name != nullptr ? name : "(null)"}});
  }
  return ok;
}

bool StartReporter(unsigned firstMs, unsigned graceMs, unsigned steadyMs) {
  pthread_mutex_lock(&g_control);
  if (g_running) {
    pthread_mutex_unlock(&g_control);
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
  if (!g_atforkRegistered) {
    g_atforkRegistered = pthread_atfork(&AtforkPrepare, &AtforkParent, &AtforkChild) == 0;
  }
  pthread_mutex_lock(&g_mutex);
  g_stop = false;
  g_firstMs = firstMs;
  g_graceMs = graceMs;
  g_steadyMs = steadyMs;
  const unsigned counters = g_counterCount;
  pthread_mutex_unlock(&g_mutex);
  const int rc = pthread_create(&g_thread, nullptr, &ReporterMain, nullptr);
  g_running = rc == 0;
  if (rc == 0) pthread_setname_np(g_thread, "nevr-hook-rpt");  // still under g_control
  pthread_mutex_unlock(&g_control);
  if (rc != 0) {
    LogFields(LogLevel::kError, "hook_report", {{"status", "thread_create_failed"}, {"errno", rc}});
    return false;
  }
  LogFields(LogLevel::kInfo, "hook_report",
            {{"status", "reporter_started"}, {"first_ms", firstMs}, {"grace_ms", graceMs},
             {"steady_ms", steadyMs}, {"counters", counters}});
  return true;
}

void StopReporter() {
  pthread_mutex_lock(&g_control);
  if (g_running) {
    pthread_mutex_lock(&g_mutex);
    g_stop = true;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);
    pthread_join(g_thread, nullptr);  // g_control held: no other Start/Stop can interleave
    g_running = false;
  }
  pthread_mutex_lock(&g_mutex);
  g_counterCount = 0;
  pthread_mutex_unlock(&g_mutex);
  pthread_mutex_unlock(&g_control);
}

bool ReporterRunning() {
  pthread_mutex_lock(&g_control);
  const bool running = g_running;
  pthread_mutex_unlock(&g_control);
  return running;
}

}  // namespace sentinel
