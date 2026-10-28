// nevr_autopilot (#1202, docs/adr/0025-quest-autopilot-route.md): a development-build Quest plugin that drives the local
// player's controllers through a route file, so two builds' performance runs see the same input.
//
// Init reads `route` (and the optional `start_file`) from its config.yaml args, parses the route, starts the reporter
// thread and registers the filter with the host (ctx->quest->register_input_filter). The filter (autopilot.cpp) never
// logs; the reporter writes route_start, route_step (Debug) and route_end through ctx->log. Built with -fno-exceptions,
// like the filter it carries.
//
// A registered filter cannot be removed (the host has no unregister), so the run it points at is never freed: Shutdown
// stops and joins the reporter and leaves the run ended, so a later call through the filter changes nothing.

#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "extension/plugin_interface.h"
#include "quest/data/sha256.h"
#include "quest/plugins/autopilot/autopilot.h"
#include "quest/plugins/autopilot/route.h"

#ifndef NEVR_AUTOPILOT_BUILD_IDS
#error "NEVR_AUTOPILOT_BUILD_IDS (the libr15 build IDs this plugin's seam is pinned to) must be defined by the build"
#endif
NEVR_DECLARE_SUPPORTED_BUILD_IDS(NEVR_AUTOPILOT_BUILD_IDS);

namespace {

using namespace nevr_autopilot;

constexpr char kPluginName[] = "nevr_autopilot";
constexpr long kStartFilePollNs = 100L * 1000 * 1000;  // ADR 0025: the start file is checked every 100 ms

struct Plugin {
  const NvrGameContext* ctx = nullptr;
  std::string id;
  std::string routePath;
  std::string startFile;
  Route route;
  Run* run = nullptr;
  sem_t wake;
  bool semReady = false;
  pthread_t thread{};
  bool threadStarted = false;
  std::atomic<bool> stop{false};
  Event start;  // the route_start event, for route_end's measured_s
  bool haveStart = false;
};

// Constant-initialised: a shipped plugin has no namespace-scope constructor (tools/check_quest_static_init.sh --plugin).
Plugin* g_plugin = nullptr;

Clocks Now() noexcept {
  const auto read = [](clockid_t id) noexcept {
    timespec ts{};
    clock_gettime(id, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * kNsPerSecond + ts.tv_nsec;
  };
  Clocks c;
  c.monotonicNs = read(CLOCK_MONOTONIC);
  c.realtimeNs = read(CLOCK_REALTIME);
  c.boottimeNs = read(CLOCK_BOOTTIME);
  return c;
}

void Wake(void* user) noexcept { sem_post(&static_cast<Plugin*>(user)->wake); }

std::string Int(std::int64_t v) {
  char b[32];
  std::snprintf(b, sizeof(b), "%" PRId64, v);
  return b;
}
std::string Uint(std::uint64_t v) {
  char b[32];
  std::snprintf(b, sizeof(b), "%" PRIu64, v);
  return b;
}
// Nanoseconds as seconds with every digit: 1500000000 -> "1.500000000".
std::string Seconds(std::int64_t ns) {
  char b[40];
  const char* sign = ns < 0 ? "-" : "";
  const std::int64_t a = ns < 0 ? -ns : ns;
  std::snprintf(b, sizeof(b), "%s%" PRId64 ".%09" PRId64, sign, a / kNsPerSecond, a % kNsPerSecond);
  return b;
}
std::string Millis(std::int64_t ns) { return Int(ns / 1000000); }
// Nanoseconds as milliseconds with every digit: 16666667 -> "16.666667".
std::string FractionalMillis(std::int64_t ns) {
  char b[40];
  std::snprintf(b, sizeof(b), "%" PRId64 ".%06" PRId64, ns / 1000000, ns % 1000000);
  return b;
}

using Fields = std::vector<std::pair<const char*, std::string>>;
void Log(const Plugin& p, std::uint32_t level, const char* message, const Fields& fields) {
  if (p.ctx == nullptr || p.ctx->log == nullptr) return;
  std::vector<NevrLogField> out;
  out.reserve(fields.size());
  for (const auto& f : fields) out.push_back({f.first, f.second.c_str()});
  p.ctx->log(p.id.c_str(), level, message, out.data(), static_cast<std::uint32_t>(out.size()));
}

void Report(Plugin& p, const Event& e) {
  switch (e.kind) {
    case Event::kStart:
      p.start = e;
      p.haveStart = true;
      Log(p, NEVR_LOG_INFO, "route_start",
          {{"name", p.route.name}, {"realtime_ms", Millis(e.at.realtimeNs)}, {"monotonic_ns", Int(e.at.monotonicNs)},
           {"boottime_ns", Int(e.at.boottimeNs)}});
      break;
    case Event::kStep:
      Log(p, NEVR_LOG_DEBUG, "route_step",
          {{"index", Uint(e.index)}, {"pass", Uint(e.pass)}, {"realtime_ms", Millis(e.at.realtimeNs)},
           {"monotonic_ns", Int(e.at.monotonicNs)}});
      break;
    case Event::kEnd: {
      const Counters& c = p.run->Count();
      const std::int64_t measured = p.haveStart ? e.at.monotonicNs - p.start.at.monotonicNs : -1;
      Log(p, NEVR_LOG_INFO, "route_end",
          {{"name", p.route.name},
           {"realtime_ms", Millis(e.at.realtimeNs)},
           {"monotonic_ns", Int(e.at.monotonicNs)},
           {"boottime_ns", Int(e.at.boottimeNs)},
           {"planned_s", Seconds(p.route.PlannedNs())},
           {"measured_s", p.haveStart ? Seconds(measured) : std::string("unknown")},
           {"frames_left", Uint(c.framesLeft.load())},
           {"frames_right", Uint(c.framesRight.load())},
           {"frames_unknown_hand", Uint(c.framesUnknownHand.load())},
           {"frames_driven_left", Uint(c.drivenLeft.load())},
           {"frames_driven_right", Uint(c.drivenRight.load())},
           {"max_frame_gap_ms", FractionalMillis(c.maxGapNs.load())},
           {"events_dropped", Uint(c.dropped.load())}});
      break;
    }
  }
}

// Waits for the semaphore or `ns`, whichever is first.
void WaitFor(Plugin& p, long ns) {
  timespec until{};
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_nsec += ns;
  until.tv_sec += until.tv_nsec / 1000000000L;
  until.tv_nsec %= 1000000000L;
  while (sem_timedwait(&p.wake, &until) != 0 && errno == EINTR) {
  }
}

void* Reporter(void* arg) {
  Plugin& p = *static_cast<Plugin*>(arg);
  if (!p.startFile.empty()) {
    while (!p.stop.load(std::memory_order_acquire) && access(p.startFile.c_str(), F_OK) != 0) WaitFor(p, kStartFilePollNs);
    if (p.stop.load(std::memory_order_acquire)) return nullptr;
    p.run->Trigger();
    Log(p, NEVR_LOG_INFO, "start_file_seen", {{"start_file", p.startFile}, {"realtime_ms", Millis(Now().realtimeNs)}});
  }
  bool ended = false;
  while (!ended) {
    Event e;
    while (p.run->Events().Pop(&e)) {
      Report(p, e);
      ended = ended || e.kind == Event::kEnd;
    }
    if (ended || p.stop.load(std::memory_order_acquire)) break;
    WaitFor(p, kStartFilePollNs);  // woken by the filter; the timeout only bounds how late a stop is seen
  }
  return nullptr;
}

void StopReporter(Plugin& p) {
  if (!p.threadStarted) return;
  p.stop.store(true, std::memory_order_release);
  sem_post(&p.wake);
  pthread_join(p.thread, nullptr);
  p.threadStarted = false;
}

bool ReadFile(const std::string& path, std::string* bytes, std::string* reason) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    *reason = std::string("cannot open: ") + std::strerror(errno);
    return false;
  }
  char buf[4096];
  bytes->clear();
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    bytes->append(buf, n);
    if (bytes->size() > kMaxFileBytes) break;  // the parser refuses it by size
  }
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  if (failed) {
    *reason = "read error";
    return false;
  }
  return true;
}

// The string value of `key` in the flat args object, "" when absent; false when present and not a string.
bool Arg(const nlohmann::json& args, const char* key, std::string* out) {
  const auto it = args.find(key);
  if (it == args.end()) {
    out->clear();
    return true;
  }
  if (!it->is_string()) return false;
  *out = it->get_ref<const std::string&>();
  return true;
}

int Refuse(Plugin& p, const std::string& line, const std::string& reason) {
  Log(p, NEVR_LOG_ERROR, "route_refused", {{"route", p.routePath}, {"line", line}, {"reason", reason}});
  return 1;
}

int Init(Plugin& p, const char* argsJson) {
  const nlohmann::json args = nlohmann::json::parse(argsJson != nullptr ? argsJson : "", nullptr, false);
  if (args.is_discarded() || !args.is_object()) return Refuse(p, "0", "the plugin's args are not a JSON object");
  if (!Arg(args, "route", &p.routePath) || p.routePath.empty()) return Refuse(p, "0", "args.route is not a path");
  if (!Arg(args, "start_file", &p.startFile)) return Refuse(p, "0", "args.start_file is not a path");
  std::string bytes, reason;
  if (!ReadFile(p.routePath, &bytes, &reason)) return Refuse(p, "0", reason);
  ParseError error;
  if (!ParseRoute(bytes, &p.route, &error)) return Refuse(p, Uint(error.line), error.reason);
  Log(p, NEVR_LOG_INFO, "route_loaded",
      {{"route", p.routePath},
       {"name", p.route.name},
       {"sha256", nevr_quest::data::Sha256Hex(bytes.data(), bytes.size())},
       {"steps", Uint(p.route.steps.size())},
       {"repeat", Uint(p.route.repeat)},
       {"planned_s", Seconds(p.route.PlannedNs())},
       {"start_after_s", Seconds(p.route.startAfterNs)},
       {"start", p.startFile.empty() ? "first_frame" : "start_file"},
       {"drives", p.route.drives[kLeft] && p.route.drives[kRight] ? "left+right"
                  : p.route.drives[kLeft]                          ? "left"
                  : p.route.drives[kRight]                         ? "right"
                                                                   : "none"}});
  const bool covered = p.ctx->ctx_size >= offsetof(NvrGameContext, quest) + sizeof(p.ctx->quest);
  const NevrQuestHostApi* quest = covered ? p.ctx->quest : nullptr;
  if (quest == nullptr || quest->struct_size < sizeof(NevrQuestHostApi) || quest->register_input_filter == nullptr) {
    Log(p, NEVR_LOG_ERROR, "input_filter", {{"result", "no_host_api"}});
    return 1;
  }
  if (sem_init(&p.wake, 0, 0) != 0) {
    Log(p, NEVR_LOG_ERROR, "input_filter", {{"result", "no_semaphore"}, {"errno", Int(errno)}});
    return 1;
  }
  p.semReady = true;
  p.run = new (std::nothrow) Run(p.route, &Now, !p.startFile.empty(), &Wake, &p);
  if (p.run == nullptr) {
    Log(p, NEVR_LOG_ERROR, "input_filter", {{"result", "no_memory"}});
    return 1;
  }
  const int rc = pthread_create(&p.thread, nullptr, &Reporter, &p);
  if (rc != 0) {
    Log(p, NEVR_LOG_ERROR, "input_filter", {{"result", "no_reporter_thread"}, {"errno", Int(rc)}});
    return 1;
  }
  p.threadStarted = true;
  const std::int32_t result = quest->register_input_filter(p.id.c_str(), &Run::FilterThunk, p.run);
  if (result != NEVR_QUEST_FILTER_OK) {
    static const char* const kNames[] = {"ok", "bad_argument", "not_dev_build", "owned", "hook_refused"};
    const char* name = result >= 0 && result <= NEVR_QUEST_FILTER_HOOK_REFUSED ? kNames[result] : "unknown";
    Log(p, NEVR_LOG_ERROR, "input_filter", {{"result", name}});
    StopReporter(p);
    return 1;
  }
  Log(p, NEVR_LOG_INFO, "input_filter", {{"result", "registered"}});
  return 0;
}

// What Shutdown and a failed init leave: no thread; a run the host never got is freed, a registered one is kept.
void Release(Plugin* p, bool registered) {
  StopReporter(*p);
  if (registered) return;  // the filter may still be called: the plugin state stays for the life of the process
  delete p->run;
  if (p->semReady) sem_destroy(&p->wake);
  delete p;
}

}  // namespace

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
  return {kPluginName, "drives the controllers through a route file (dev builds; #1202)", 1, 0, 0};
}
NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }

NEVR_PLUGIN_API int NvrPluginInitEx(const NvrGameContext* ctx, const char* args_json) {
  if (ctx == nullptr || g_plugin != nullptr) return 1;
  Plugin* p = new (std::nothrow) Plugin();
  if (p == nullptr) return 1;
  p->ctx = ctx;
  p->id = ctx->plugin_id != nullptr ? ctx->plugin_id : kPluginName;
  const int rc = Init(*p, args_json);
  if (rc != 0) {
    Release(p, false);
    return rc;
  }
  g_plugin = p;
  return 0;
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
  Plugin* p = g_plugin;
  if (p == nullptr) return;
  const bool ended = p->run->Ended();
  p->run->Stop();  // from here the filter changes nothing
  StopReporter(*p);
  if (!ended) {
    const Counters& c = p->run->Count();
    Log(*p, NEVR_LOG_WARNING, "route_stopped",
        {{"name", p->route.name},
         {"started", p->haveStart ? "true" : "false"},
         {"realtime_ms", Millis(Now().realtimeNs)},
         {"frames_driven_left", Uint(c.drivenLeft.load())},
         {"frames_driven_right", Uint(c.drivenRight.load())}});
  }
  Release(p, true);
}
