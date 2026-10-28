// nevr_autopilot (#1202, docs/adr/0025-quest-autopilot-route.md): a development-build Quest plugin that drives the local
// player's controllers through a route file, so two builds' performance runs see the same input.
//
// Init reads `route` (and the optional `start_file`) from its config.yaml args, parses the route, starts the reporter
// thread and registers the filter with the host (ctx->quest->register_input_filter). The filter (autopilot.cpp) never
// logs; the reporter writes route_start, route_step (Debug) and route_end through ctx->log. Built with -fno-exceptions,
// like the filter it carries.
//
// A registered filter cannot be removed (the host has no unregister), so the run it points at is never freed: Shutdown
// stops and joins the reporter and leaves the run ended, so a later call through the filter changes nothing. The state
// lives in static storage, so init has no allocation that can fail and a failed init leaves nothing behind.

#include "quest/plugins/autopilot/plugin.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/data/sha256.h"
#include "quest/plugins/autopilot/autopilot.h"
#include "quest/plugins/autopilot/route.h"

#ifndef NEVR_AUTOPILOT_BUILD_IDS
#error "NEVR_AUTOPILOT_BUILD_IDS (the libr15 build IDs this plugin's seam is pinned to) must be defined by the build"
#endif
NEVR_DECLARE_SUPPORTED_BUILD_IDS(NEVR_AUTOPILOT_BUILD_IDS);

namespace nevr_autopilot::plugin {

namespace {

constexpr char kPluginName[] = "nevr_autopilot";
constexpr int kStartFilePollMs = 100;  // ADR 0025: the start file is checked every 100 ms

struct Plugin {
  const NvrGameContext* ctx = nullptr;
  std::string id;
  std::string routePath;
  std::string startFile;
  Route route;
  std::optional<Run> run;
  int wakeFd = -1;
  pthread_t thread{};
  std::atomic<bool> stop{false};
  Event start;  // the route_start event, for route_end's measured_s
  bool haveStart = false;

  ~Plugin() {
    if (wakeFd >= 0) close(wakeFd);
  }
};

// Constant-initialised: a shipped plugin has no namespace-scope constructor (tools/check_quest_static_init.sh --plugin).
alignas(Plugin) unsigned char g_storage[sizeof(Plugin)];
Plugin* g_plugin = nullptr;  // the registered plugin, from a successful init on

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

// The filter's wake: one eventfd write, which neither blocks (EFD_NONBLOCK) nor locks.
void Wake(void* user) noexcept {
  const std::uint64_t one = 1;
  const ssize_t wrote = write(static_cast<Plugin*>(user)->wakeFd, &one, sizeof(one));
  static_cast<void>(wrote);  // a full counter (2^64 - 2 wakes) still leaves the descriptor readable
}

// `v` (below 10^width) with leading zeros to `width` digits.
std::string Padded(std::int64_t v, std::size_t width) {
  const std::string s = std::to_string(v);
  return std::string(width - s.size(), '0') + s;
}
// Non-negative nanoseconds as seconds with every digit: 1500000000 -> "1.500000000".
std::string Seconds(std::int64_t ns) { return std::to_string(ns / kNsPerSecond) + "." + Padded(ns % kNsPerSecond, 9); }
std::string Millis(std::int64_t ns) { return std::to_string(ns / 1000000); }
// Non-negative nanoseconds as milliseconds with every digit: 16666667 -> "16.666667".
std::string FractionalMillis(std::int64_t ns) { return std::to_string(ns / 1000000) + "." + Padded(ns % 1000000, 6); }

using Fields = std::vector<std::pair<const char*, std::string>>;
void Log(const Plugin& p, std::uint32_t level, const char* message, const Fields& fields) {
  if (p.ctx->log == nullptr) return;
  std::vector<NevrLogField> out;
  for (const auto& f : fields) out.push_back({f.first, f.second.c_str()});
  p.ctx->log(p.id.c_str(), level, message, out.data(), static_cast<std::uint32_t>(out.size()));
}

void Report(Plugin& p, const Event& e) {
  switch (e.kind) {
    case Event::kStart:
      p.start = e;
      p.haveStart = true;
      Log(p, NEVR_LOG_INFO, "route_start",
          {{"name", p.route.name},
           {"realtime_ms", Millis(e.at.realtimeNs)},
           {"monotonic_ns", std::to_string(e.at.monotonicNs)},
           {"boottime_ns", std::to_string(e.at.boottimeNs)}});
      break;
    case Event::kStep:
      Log(p, NEVR_LOG_DEBUG, "route_step",
          {{"index", std::to_string(e.index)},
           {"pass", std::to_string(e.pass)},
           {"realtime_ms", Millis(e.at.realtimeNs)},
           {"monotonic_ns", std::to_string(e.at.monotonicNs)}});
      break;
    case Event::kEnd: {
      // Start is the first event a run queues, into an empty ring, so it is never dropped and always precedes End.
      const Counters& c = p.run->Count();
      Log(p, NEVR_LOG_INFO, "route_end",
          {{"name", p.route.name},
           {"realtime_ms", Millis(e.at.realtimeNs)},
           {"monotonic_ns", std::to_string(e.at.monotonicNs)},
           {"boottime_ns", std::to_string(e.at.boottimeNs)},
           {"planned_s", Seconds(p.route.PlannedNs())},
           {"measured_s", Seconds(e.at.monotonicNs - p.start.at.monotonicNs)},
           {"frames_left", std::to_string(c.framesLeft.load())},
           {"frames_right", std::to_string(c.framesRight.load())},
           {"frames_unknown_hand", std::to_string(c.framesUnknownHand.load())},
           {"frames_driven_left", std::to_string(c.drivenLeft.load())},
           {"frames_driven_right", std::to_string(c.drivenRight.load())},
           {"max_frame_gap_ms", FractionalMillis(c.maxGapNs.load())},
           {"events_dropped", std::to_string(c.dropped.load())}});
    }
  }
}

// Waits for a wake or `timeoutMs` (-1: only a wake), and clears the wake. False once Shutdown has asked the reporter to
// stop.
bool Wait(Plugin& p, int timeoutMs) {
  pollfd fd{p.wakeFd, POLLIN, 0};
  if (poll(&fd, 1, timeoutMs) > 0) {
    std::uint64_t count = 0;
    const ssize_t got = read(p.wakeFd, &count, sizeof(count));
    static_cast<void>(got);  // EAGAIN: another wake was already taken; either way the ring is drained next
  }
  return !p.stop.load(std::memory_order_acquire);
}

void* Reporter(void* arg) {
  Plugin& p = *static_cast<Plugin*>(arg);
  const char* why = "stopped";
  if (!p.startFile.empty()) {
    while (access(p.startFile.c_str(), F_OK) != 0) {
      if (!Wait(p, kStartFilePollMs)) {
        Log(p, NEVR_LOG_DEBUG, "reporter_end", {{"reason", why}});
        return nullptr;
      }
    }
    p.run->Trigger();
    Log(p, NEVR_LOG_INFO, "start_file_seen", {{"start_file", p.startFile}, {"realtime_ms", Millis(Now().realtimeNs)}});
  }
  for (;;) {
    Event e;
    bool ended = false;
    while (!ended && p.run->Events().Pop(&e)) {
      Report(p, e);
      ended = e.kind == Event::kEnd;
    }
    if (ended) {
      why = "route_end";
      break;
    }
    if (!Wait(p, -1)) break;  // woken by the filter after each event, and by Shutdown
  }
  Log(p, NEVR_LOG_DEBUG, "reporter_end", {{"reason", why}});
  return nullptr;
}

// Asks the reporter to stop, wakes it and joins it. Called once for each started thread.
void StopReporter(Plugin& p) {
  p.stop.store(true, std::memory_order_release);
  Wake(&p);
  pthread_join(p.thread, nullptr);
}

bool ReadFile(const std::string& path, std::string* bytes, std::string* reason) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    *reason = std::string("cannot open: ") + std::strerror(errno);
    return false;
  }
  // One byte past the limit, so the parser refuses a larger file by its size rather than reading a truncated one.
  bytes->resize(kMaxFileBytes + 1);
  bytes->resize(std::fread(&(*bytes)[0], 1, bytes->size(), f));
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  if (failed) {
    *reason = "read error";
    return false;
  }
  return true;
}

// The string value of `key` in the flat args object, left empty when absent; false when present and not a string.
bool Arg(const nlohmann::json& args, const char* key, std::string* out) {
  const auto it = args.find(key);
  if (it == args.end()) return true;
  if (!it->is_string()) return false;
  *out = it->get_ref<const std::string&>();
  return true;
}

bool Refuse(Plugin& p, const std::string& line, const std::string& reason) {
  Log(p, NEVR_LOG_ERROR, "route_refused", {{"route", p.routePath}, {"line", line}, {"reason", reason}});
  return false;
}

const char* Drives(const Route& r) {
  if (r.drives[kLeft] && r.drives[kRight]) return "left+right";
  if (r.drives[kLeft]) return "left";
  if (r.drives[kRight]) return "right";
  return "none";
}

// The host's refusal token for a NevrQuestFilterResult other than OK.
const char* RefusalName(std::int32_t result) {
  switch (result) {
    case NEVR_QUEST_FILTER_BAD_ARGUMENT: return "bad_argument";
    case NEVR_QUEST_FILTER_NOT_DEV_BUILD: return "not_dev_build";
    case NEVR_QUEST_FILTER_OWNED: return "owned";
    case NEVR_QUEST_FILTER_HOOK_REFUSED: return "hook_refused";
    default: return "unknown";
  }
}

bool Init(Plugin& p, const char* argsJson, const Os& os) {
  const nlohmann::json args = nlohmann::json::parse(argsJson != nullptr ? argsJson : "", nullptr, false);
  if (args.is_discarded() || !args.is_object()) return Refuse(p, "0", "the plugin's args are not a JSON object");
  if (!Arg(args, "route", &p.routePath) || p.routePath.empty()) return Refuse(p, "0", "args.route is not a path");
  if (!Arg(args, "start_file", &p.startFile)) return Refuse(p, "0", "args.start_file is not a path");
  std::string bytes, reason;
  if (!ReadFile(p.routePath, &bytes, &reason)) return Refuse(p, "0", reason);
  ParseError error;
  if (!ParseRoute(bytes, &p.route, &error)) return Refuse(p, std::to_string(error.line), error.reason);
  Log(p, NEVR_LOG_INFO, "route_loaded",
      {{"route", p.routePath},
       {"name", p.route.name},
       {"sha256", nevr_quest::data::Sha256Hex(bytes.data(), bytes.size())},
       {"steps", std::to_string(p.route.steps.size())},
       {"repeat", std::to_string(p.route.repeat)},
       {"planned_s", Seconds(p.route.PlannedNs())},
       {"start_after_s", Seconds(p.route.startAfterNs)},
       {"start", p.startFile.empty() ? "first_frame" : "start_file"},
       {"drives", Drives(p.route)}});
  const bool covered = p.ctx->ctx_size >= offsetof(NvrGameContext, quest) + sizeof(p.ctx->quest);
  const NevrQuestHostApi* quest = covered ? p.ctx->quest : nullptr;
  if (quest == nullptr || quest->struct_size < sizeof(NevrQuestHostApi) || quest->register_input_filter == nullptr) {
    Log(p, NEVR_LOG_ERROR, "input_filter", {{"result", "no_host_api"}});
    return false;
  }
  p.wakeFd = os.openWake();
  if (p.wakeFd < 0) {
    Log(p, NEVR_LOG_ERROR, "input_filter", {{"result", "no_wake_fd"}, {"errno", std::to_string(errno)}});
    return false;
  }
  p.run.emplace(p.route, &Now, !p.startFile.empty(), &Wake, &p);
  const int rc = os.startThread(&p.thread, &Reporter, &p);
  if (rc != 0) {
    Log(p, NEVR_LOG_ERROR, "input_filter", {{"result", "no_reporter_thread"}, {"errno", std::to_string(rc)}});
    return false;
  }
  const std::int32_t result = quest->register_input_filter(p.id.c_str(), &Run::FilterThunk, &*p.run);
  if (result != NEVR_QUEST_FILTER_OK) {
    Log(p, NEVR_LOG_ERROR, "input_filter", {{"result", RefusalName(result)}});
    StopReporter(p);
    return false;
  }
  Log(p, NEVR_LOG_INFO, "input_filter", {{"result", "registered"}});
  return true;
}

int OpenWake() noexcept { return eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK); }
int StartThread(pthread_t* thread, void* (*body)(void*), void* arg) noexcept {
  return pthread_create(thread, nullptr, body, arg);
}
constexpr Os kRealOs{&OpenWake, &StartThread};

}  // namespace

const Os& RealOs() noexcept { return kRealOs; }

int InitWith(const NvrGameContext* ctx, const char* argsJson, const Os& os) {
  if (ctx == nullptr || g_plugin != nullptr) return 1;
  Plugin* p = new (g_storage) Plugin();
  p->ctx = ctx;
  p->id = ctx->plugin_id != nullptr ? ctx->plugin_id : kPluginName;
  if (!Init(*p, argsJson, os)) {
    p->~Plugin();  // no thread is left (a started one was joined) and the host holds nothing of it
    return 1;
  }
  g_plugin = p;
  return 0;
}

void Shutdown() {
  Plugin* p = g_plugin;
  if (p == nullptr) return;  // no init succeeded (the loader calls Shutdown after a failed init)
  const bool ended = p->run->Ended();
  p->run->Stop();  // from here the filter changes nothing
  StopReporter(*p);
  if (!ended) {
    const Counters& c = p->run->Count();
    Log(*p, NEVR_LOG_WARNING, "route_stopped",
        {{"name", p->route.name},
         {"started", p->haveStart ? "true" : "false"},
         {"realtime_ms", Millis(Now().realtimeNs)},
         {"frames_driven_left", std::to_string(c.drivenLeft.load())},
         {"frames_driven_right", std::to_string(c.drivenRight.load())}});
  }
}

}  // namespace nevr_autopilot::plugin

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
  return {"nevr_autopilot", "drives the controllers through a route file (dev builds; #1202)", 1, 0, 0};
}
NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }

NEVR_PLUGIN_API int NvrPluginInitEx(const NvrGameContext* ctx, const char* args_json) {
  return nevr_autopilot::plugin::InitWith(ctx, args_json, nevr_autopilot::plugin::RealOs());
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) { nevr_autopilot::plugin::Shutdown(); }
