// Host test for the autopilot's run and its plugin entry points (#1202, src/quest/plugins/autopilot/autopilot.h and
// plugin.cpp; docs/adr/0025-quest-autopilot-route.md). The filter is driven with a fake clock; the plugin is driven through
// its exported functions with a fake host (a context whose quest API records the registered filter and whose log records
// every line), a real route file in a temporary directory, and its real reporter thread.
//
// Built and run by `just test-quest-autopilot`.

#include <dirent.h>
#include <fcntl.h>
#include <link.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "nevr/plugin_interface.h"
#include "quest/plugins/autopilot/autopilot.h"
#include "quest/plugins/autopilot/plugin.h"
#include "quest/plugins/autopilot/route.h"
#include "quest/tests/test_check.h"

extern "C" {
NvrPluginInfo NvrPluginGetInfo(void);
uint32_t NvrPluginGetApiVersion(void);
int NvrPluginInitEx(const NvrGameContext* ctx, const char* args_json);
void NvrPluginShutdown(void);
}

namespace {

namespace ap = nevr_autopilot;

// ---- the fake clock ---------------------------------------------------------------------------------------------------

std::int64_t g_now = 0;
ap::Clocks FakeClock() noexcept {
  ap::Clocks c;
  c.monotonicNs = g_now;
  c.realtimeNs = g_now + 1000;
  c.boottimeNs = g_now + 2000;
  return c;
}

ap::Route Parse(const char* text) {
  ap::Route r;
  ap::ParseError e;
  if (!ap::ParseRoute(text, &r, &e)) std::fprintf(stderr, "route refused, line %zu: %s\n", e.line, e.reason.c_str());
  return r;
}

NevrQuestControllerState Real(std::uint32_t hand) {
  NevrQuestControllerState s{};
  s.hand = hand;
  s.buttons = 0x55u;
  s.touches = 0x66u;
  s.index_trigger = 0.3f;
  s.grip_trigger = 0.4f;
  s.stick_x = 0.1f;
  s.stick_y = 0.2f;
  return s;
}
bool Same(const NevrQuestControllerState& a, const NevrQuestControllerState& b) {
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}
// One frame: both hands polled at g_now.
void Frame(ap::Run& run, NevrQuestControllerState* left, NevrQuestControllerState* right) {
  run.Filter(left);
  run.Filter(right);
}

std::vector<ap::Event> Drain(ap::Run& run) {
  std::vector<ap::Event> out;
  ap::Event e;
  while (run.Events().Pop(&e)) out.push_back(e);
  return out;
}

constexpr std::int64_t kMs = 1000000;

// ---- the run ----------------------------------------------------------------------------------------------------------

void TheRouteDrivesOnlyItsHandsAndOnlyWhileItRuns() {
  // start_after 0.1, steps 0.1 (left stick forward) and 0.05 (rest), repeat 2: planned 0.3 s.
  const ap::Route route = Parse("autopilot-route 1\nname t\nstart_after 0.1\nrepeat 2\nstep 0.1 left.stick=0,1\nstep 0.05\n");
  QCHECK(route.drives[ap::kLeft] && !route.drives[ap::kRight]);
  ap::Run run(route, &FakeClock, false);
  g_now = 5000 * kMs;  // the first frame: the start point
  NevrQuestControllerState l = Real(NEVR_QUEST_HAND_LEFT), r = Real(NEVR_QUEST_HAND_RIGHT);
  Frame(run, &l, &r);
  // Waiting for start_after: nothing changes, nothing is counted.
  QCHECK(Same(l, Real(NEVR_QUEST_HAND_LEFT)) && Same(r, Real(NEVR_QUEST_HAND_RIGHT)));
  QCHECK(Drain(run).empty());
  QCHECK(run.Count().framesLeft.load() == 0u);
  // The first step: the left hand is the step's whole state, the right hand (never named) is the controller's.
  g_now += 100 * kMs;
  l = Real(NEVR_QUEST_HAND_LEFT);
  r = Real(NEVR_QUEST_HAND_RIGHT);
  Frame(run, &l, &r);
  QCHECK(l.stick_x == 0.0f && l.stick_y == 1.0f);
  QCHECK(l.buttons == 0u && l.index_trigger == 0.0f && l.grip_trigger == 0.0f);
  QCHECK(l.touches == (0x20u | 0x400u | 0x200u));
  QCHECK(l.hand == NEVR_QUEST_HAND_LEFT);
  QCHECK(Same(r, Real(NEVR_QUEST_HAND_RIGHT)));
  std::vector<ap::Event> ev = Drain(run);
  QCHECK(ev.size() == 2u);
  if (ev.size() == 2u) {
    QCHECK(ev[0].kind == ap::Event::kStart);
    QCHECK(ev[0].at.monotonicNs == 5100 * kMs && ev[0].at.realtimeNs == 5100 * kMs + 1000 &&
           ev[0].at.boottimeNs == 5100 * kMs + 2000);
    QCHECK(ev[1].kind == ap::Event::kStep && ev[1].index == 0u && ev[1].pass == 0u);
  }
  // Same step, next frame: no event.
  g_now += 16 * kMs;
  Frame(run, &l, &r);
  QCHECK(Drain(run).empty());
  // The rest step: the left hand is at rest.
  g_now = 5200 * kMs;
  l = Real(NEVR_QUEST_HAND_LEFT);
  Frame(run, &l, &r);
  QCHECK(l.stick_y == 0.0f && l.touches == 0x300u && l.buttons == 0u);
  ev = Drain(run);
  QCHECK(ev.size() == 1u && ev[0].kind == ap::Event::kStep && ev[0].index == 1u && ev[0].pass == 0u);
  // The second pass.
  g_now = 5250 * kMs;
  l = Real(NEVR_QUEST_HAND_LEFT);
  Frame(run, &l, &r);
  QCHECK(l.stick_y == 1.0f);
  ev = Drain(run);
  QCHECK(ev.size() == 1u && ev[0].kind == ap::Event::kStep && ev[0].index == 0u && ev[0].pass == 1u);
  // The planned end (start 5000 + 100 + 300): done; nothing is changed or counted from then on.
  g_now = 5400 * kMs;
  l = Real(NEVR_QUEST_HAND_LEFT);
  Frame(run, &l, &r);
  QCHECK(Same(l, Real(NEVR_QUEST_HAND_LEFT)));
  QCHECK(run.Ended());
  ev = Drain(run);
  QCHECK(ev.size() == 1u && ev[0].kind == ap::Event::kEnd && ev[0].at.monotonicNs == 5400 * kMs);
  g_now += 16 * kMs;
  Frame(run, &l, &r);
  QCHECK(Drain(run).empty());
  // Frames inside the route: 4 per hand (5100, 5116, 5200, 5250); driven: the left hand only.
  QCHECK(run.Count().framesLeft.load() == 4u);
  QCHECK(run.Count().framesRight.load() == 4u);
  QCHECK(run.Count().drivenLeft.load() == 4u);
  QCHECK(run.Count().drivenRight.load() == 0u);
  QCHECK(run.Count().framesUnknownHand.load() == 0u);
  QCHECK(run.Count().maxGapNs.load() == 84 * kMs);  // 5116 -> 5200
  QCHECK(run.Count().dropped.load() == 0u);
}

void AnUnknownHandIsCountedAndNeverDriven() {
  const ap::Route route = Parse("autopilot-route 1\nname t\nstep 1 left.trigger=1 right.trigger=1\n");
  ap::Run run(route, &FakeClock, false);
  g_now = 1000 * kMs;
  NevrQuestControllerState u = Real(NEVR_QUEST_HAND_UNKNOWN);
  run.Filter(&u);
  QCHECK(Same(u, Real(NEVR_QUEST_HAND_UNKNOWN)));
  NevrQuestControllerState odd = Real(7);  // not a hand the ABI names: treated as unknown
  g_now = 1500 * kMs;
  run.Filter(&odd);
  QCHECK(Same(odd, Real(7)));
  QCHECK(run.Count().framesUnknownHand.load() == 2u);
  // Neither is a hand: no hand's frame count and no gap.
  QCHECK(run.Count().framesLeft.load() == 0u && run.Count().framesRight.load() == 0u);
  QCHECK(run.Count().maxGapNs.load() == 0);
  NevrQuestControllerState r = Real(NEVR_QUEST_HAND_RIGHT);
  run.Filter(&r);
  QCHECK(r.index_trigger == 1.0f && (r.buttons & 0x20000000u) != 0);
  QCHECK(run.Count().drivenRight.load() == 1u && run.Count().drivenLeft.load() == 0u);
  QCHECK(run.Count().framesRight.load() == 1u && run.Count().framesLeft.load() == 0u);
}

void TheStartWaitsForTheTrigger() {
  const ap::Route route = Parse("autopilot-route 1\nname t\nstep 0.1 right.grip=1\n");
  ap::Run run(route, &FakeClock, true);
  g_now = 1000 * kMs;
  NevrQuestControllerState r = Real(NEVR_QUEST_HAND_RIGHT);
  run.Filter(&r);
  g_now = 2000 * kMs;
  run.Filter(&r);
  QCHECK(Same(r, Real(NEVR_QUEST_HAND_RIGHT)));
  QCHECK(Drain(run).empty());
  QCHECK(!run.Triggered());
  run.Trigger();
  // The start point is the first frame after the trigger, not the first frame the filter saw.
  g_now = 3000 * kMs;
  run.Filter(&r);
  QCHECK(r.grip_trigger == 1.0f);
  const std::vector<ap::Event> ev = Drain(run);
  QCHECK(ev.size() == 2u && ev[0].kind == ap::Event::kStart && ev[0].at.monotonicNs == 3000 * kMs);
  g_now = 3099 * kMs;
  run.Filter(&r);
  QCHECK(!run.Ended());
  g_now = 3100 * kMs;
  run.Filter(&r);
  QCHECK(run.Ended());
}

void AFrameThatJumpsPastTheEndStillReportsTheStart() {
  // The start delay ends at 10 ms and the route at 20 ms; the frame after the start point comes at 50 ms (a stalled game):
  // no frame ran inside the route, and route_start still comes before route_end.
  const ap::Route route = Parse("autopilot-route 1\nname t\nstart_after 0.01\nstep 0.01 left.grip=1\n");
  ap::Run run(route, &FakeClock, false);
  NevrQuestControllerState l = Real(NEVR_QUEST_HAND_LEFT);
  g_now = 1000 * kMs;
  run.Filter(&l);
  g_now = 1050 * kMs;
  run.Filter(&l);
  QCHECK(Same(l, Real(NEVR_QUEST_HAND_LEFT)));
  const std::vector<ap::Event> ev = Drain(run);
  QCHECK(ev.size() == 2u);
  if (ev.size() == 2u) {
    QCHECK(ev[0].kind == ap::Event::kStart && ev[0].at.monotonicNs == 1050 * kMs);
    QCHECK(ev[1].kind == ap::Event::kEnd && ev[1].at.monotonicNs == 1050 * kMs);
  }
  QCHECK(run.Count().drivenLeft.load() == 0u);
}

void StopEndsTheRunWithoutAnEnd() {
  const ap::Route route = Parse("autopilot-route 1\nname t\nstep 10 left.grip=1\n");
  ap::Run run(route, &FakeClock, false);
  g_now = 1000 * kMs;
  NevrQuestControllerState l = Real(NEVR_QUEST_HAND_LEFT);
  run.Filter(&l);
  Drain(run);
  run.Stop();
  l = Real(NEVR_QUEST_HAND_LEFT);
  g_now += 16 * kMs;
  run.Filter(&l);
  QCHECK(Same(l, Real(NEVR_QUEST_HAND_LEFT)));
  QCHECK(Drain(run).empty());
}

int g_wakes = 0;
void CountWake(void*) noexcept { ++g_wakes; }

void AFullRingDropsAndCounts() {
  // 70 one-hundredth steps, one frame in each: 1 start + 70 steps = 71 events for a 64-cell ring nobody drains.
  std::string text = "autopilot-route 1\nname t\n";
  for (int i = 0; i < 70; ++i) text += "step 0.01 left.grip=1\n";
  const ap::Route route = Parse(text.c_str());
  g_wakes = 0;
  ap::Run run(route, &FakeClock, false, &CountWake, nullptr);
  NevrQuestControllerState l = Real(NEVR_QUEST_HAND_LEFT);
  for (int i = 0; i < 70; ++i) {
    g_now = 1000 * kMs + i * 10 * kMs;
    run.Filter(&l);
  }
  QCHECK(run.Count().dropped.load() == 71u - ap::EventRing::kCells);
  QCHECK(g_wakes == static_cast<int>(ap::EventRing::kCells));  // one wake per queued event, none per dropped one
  QCHECK(Drain(run).size() == ap::EventRing::kCells);
}

void TheRingKeepsOrderAcrossWraps() {
  ap::EventRing ring;
  ap::Event e;
  for (std::uint32_t round = 0; round < 3; ++round) {
    for (std::uint32_t i = 0; i < ap::EventRing::kCells; ++i) {
      e.index = round * 1000 + i;
      QCHECK(ring.Push(e));
    }
    QCHECK(!ring.Push(e));
    for (std::uint32_t i = 0; i < ap::EventRing::kCells; ++i) {
      ap::Event out;
      QCHECK(ring.Pop(&out) && out.index == round * 1000 + i);
    }
    ap::Event none;
    QCHECK(!ring.Pop(&none));
  }
}

void TheRingTakesTwoProducers() {
  ap::EventRing ring;
  std::atomic<int> pushed{0};
  const auto producer = [&](std::uint32_t base) {
    for (std::uint32_t i = 0; i < 20000; ++i) {
      ap::Event e;
      e.pass = base;
      e.index = i;
      while (!ring.Push(e)) std::this_thread::yield();
      pushed.fetch_add(1);
    }
  };
  std::thread a(producer, 1), b(producer, 2);
  std::uint32_t next[3] = {0, 0, 0};
  int popped = 0;
  bool ordered = true;
  while (popped < 40000) {
    ap::Event e;
    if (!ring.Pop(&e)) continue;
    if (e.pass < 1 || e.pass > 2 || e.index != next[e.pass]) ordered = false;
    if (e.pass >= 1 && e.pass <= 2) next[e.pass] = e.index + 1;
    ++popped;
  }
  a.join();
  b.join();
  QCHECK(ordered);
  QCHECK(pushed.load() == 40000);
  QCHECK(next[1] == 20000u && next[2] == 20000u);
}

// ---- the plugin, through its exports ----------------------------------------------------------------------------------

struct Logged {
  std::string pluginId;
  std::uint32_t level;
  std::string message;
  std::map<std::string, std::string> fields;
};
std::mutex g_logMutex;
std::vector<Logged> g_log;
// When set, HostLog holds a reporter_end line for 200 ms before it records it, so a caller that returns before the
// reporter finished cannot have seen it.
std::atomic<bool> g_slowReporterEnd{false};
void HostLog(const char* plugin_id, uint32_t level, const char* message, const NevrLogField* fields, uint32_t n) {
  Logged l{plugin_id, level, message, {}};
  for (uint32_t i = 0; i < n; ++i) l.fields[fields[i].key] = fields[i].value;
  if (g_slowReporterEnd.load() && l.message == "reporter_end") std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_log.push_back(l);
}
bool FindLog(const char* message, Logged* out) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  for (const Logged& l : g_log) {
    if (l.message == message) {
      if (out != nullptr) *out = l;
      return true;
    }
  }
  return false;
}
std::vector<Logged> AllLog(const char* message) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  std::vector<Logged> out;
  for (const Logged& l : g_log) {
    if (l.message == message) out.push_back(l);
  }
  return out;
}
std::size_t CountLog(const char* message) { return AllLog(message).size(); }
// Every line so far came from `id`.
bool AllFrom(const char* id) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  for (const Logged& l : g_log) {
    if (l.pluginId != id) return false;
  }
  return !g_log.empty();
}

NevrQuestInputFilterFn g_filter = nullptr;
void* g_user = nullptr;
std::int32_t g_registerResult = NEVR_QUEST_FILTER_OK;
int g_registers = 0;
std::string g_registeredId;
std::int32_t Register(const char* id, NevrQuestInputFilterFn filter, void* user) {
  ++g_registers;
  g_registeredId = id;
  if (g_registerResult != NEVR_QUEST_FILTER_OK) return g_registerResult;
  g_filter = filter;
  g_user = user;
  return NEVR_QUEST_FILTER_OK;
}
const NevrQuestHostApi kApi{sizeof(NevrQuestHostApi), &Register, nullptr};

NvrGameContext Context(const NevrQuestHostApi* api) {
  NvrGameContext ctx{};
  ctx.flags = NEVR_HOST_IS_CLIENT | NEVR_HOST_PLATFORM_QUEST;
  ctx.ctx_size = sizeof(NvrGameContext);
  ctx.plugin_id = "nevr_autopilot";
  ctx.log = &HostLog;
  ctx.quest = api;
  return ctx;
}

// The injected operating-system calls: the real ones, or one that fails.
int FailWake() noexcept {
  errno = EMFILE;
  return -1;
}
int FailThread(pthread_t*, void* (*)(void*), void*) noexcept { return EAGAIN; }
const ap::plugin::Os kNoWake{&FailWake, ap::plugin::RealOs().startThread};
const ap::plugin::Os kNoThread{ap::plugin::RealOs().openWake, &FailThread};

// What the process holds: open descriptors and threads (Linux /proc). A plugin that refused leaves both as they were.
std::size_t Entries(const char* dir) {
  std::size_t n = 0;
  if (DIR* d = opendir(dir)) {
    while (dirent* e = readdir(d)) n += e->d_name[0] != '.' ? 1u : 0u;
    closedir(d);
  }
  return n;
}
std::size_t Fds() { return Entries("/proc/self/fd"); }
std::size_t Threads() { return Entries("/proc/self/task"); }
// The test's own thread, when no reporter runs (every thread a test starts, it joins). A joined thread can stay listed
// for a moment after pthread_join returns (the kernel clears its tid before the task is gone), so a count is waited for.
constexpr std::size_t kMainOnly = 1;
// Waits up to `ms` for the thread count to fall to `n`.
bool ThreadsFallTo(std::size_t n, int ms) {
  for (int t = 0; t < ms && Threads() != n; t += 5) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return Threads() == n;
}

std::int64_t ClockNs(clockid_t id) {
  timespec ts{};
  clock_gettime(id, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
// The clocks a test brackets a line with: a logged value must fall between two of these.
struct Bracket {
  std::int64_t monotonic = ClockNs(CLOCK_MONOTONIC);
  std::int64_t realtime = ClockNs(CLOCK_REALTIME);
  std::int64_t boottime = ClockNs(CLOCK_BOOTTIME);
};
bool Between(const std::string& logged, std::int64_t lo, std::int64_t hi) {
  const long long v = std::atoll(logged.c_str());
  return !logged.empty() && v >= lo && v <= hi;
}
// A line's clocks fall inside [before, after]: realtime_ms, and monotonic_ns and boottime_ns when `all`.
bool ClocksInside(const Logged& l, const Bracket& before, const Bracket& after, bool all) {
  bool ok = Between(l.fields.at("realtime_ms"), before.realtime / 1000000, after.realtime / 1000000);
  if (all) {
    ok = ok && Between(l.fields.at("monotonic_ns"), before.monotonic, after.monotonic) &&
         Between(l.fields.at("boottime_ns"), before.boottime, after.boottime);
  }
  if (!ok) std::fprintf(stderr, "%s: a clock outside the run's bracket\n", l.message.c_str());
  return ok;
}
bool Matches(const std::string& value, const char* pattern) { return std::regex_match(value, std::regex(pattern)); }

std::string g_dir;
std::string WriteRoute(const char* name, const std::string& text) {
  const std::string path = g_dir + "/" + name;
  std::FILE* f = std::fopen(path.c_str(), "wb");
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  return path;
}
std::string Args(const std::string& route, const std::string& startFile = "") {
  std::string a = "{\"route\":\"" + route + "\"";
  if (!startFile.empty()) a += ",\"start_file\":\"" + startFile + "\"";
  return a + "}";
}
void ResetHost() {
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_log.clear();
  g_filter = nullptr;
  g_user = nullptr;
  g_registerResult = NEVR_QUEST_FILTER_OK;
  g_registers = 0;
  g_registeredId.clear();
}
// Polls the filter like the game would, every 5 ms, until `message` is logged or `ms` passes.
bool DriveUntil(const char* message, int ms) {
  for (int t = 0; t < ms; t += 5) {
    if (g_filter != nullptr) {
      NevrQuestControllerState l = Real(NEVR_QUEST_HAND_LEFT), r = Real(NEVR_QUEST_HAND_RIGHT);
      g_filter(g_user, &l);
      g_filter(g_user, &r);
    }
    if (FindLog(message, nullptr)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return FindLog(message, nullptr);
}

void ThePluginNamesItself() {
  const NvrPluginInfo info = NvrPluginGetInfo();
  QCHECK(std::strcmp(info.name, "nevr_autopilot") == 0);
  QCHECK(std::strcmp(info.description, "drives the controllers through a route file (dev builds; #1202)") == 0);
  QCHECK(info.version_major == 1u && info.version_minor == 0u && info.version_patch == 0u);
  QCHECK(NvrPluginGetApiVersion() == NEVR_PLUGIN_API_VERSION);
}

// One init the plugin refuses: it returns 1, logs `message` at Error with `field`=`value`, registers nothing that
// stays, and leaves no descriptor and no thread behind. A Shutdown after it (the loader's lifecycle) logs nothing.
void Refused(const std::string& args, const NevrQuestHostApi* api, const ap::plugin::Os& os, const char* message,
             const char* field, const char* value) {
  ResetHost();
  const std::size_t fds = Fds();
  const NvrGameContext ctx = Context(api);
  QCHECK(ap::plugin::InitWith(&ctx, args.c_str(), os) == 1);
  Logged l;
  const bool found = FindLog(message, &l);
  if (!found || l.fields[field] != value) {
    std::fprintf(stderr, "args %s: expected %s %s=%s, got %s\n", args.c_str(), message, field, value,
                 found ? l.fields[field].c_str() : "<no line>");
  }
  QCHECK(found && l.fields[field] == value);
  QCHECK(l.level == NEVR_LOG_ERROR);
  QCHECK(g_filter == nullptr);
  QCHECK(Fds() == fds);
  QCHECK(ThreadsFallTo(kMainOnly, 1000));  // none was started
  const std::size_t lines = g_log.size();
  NvrPluginShutdown();
  QCHECK(g_log.size() == lines);
}

void InitRefusesWhatItCannotRun() {
  const std::string good = WriteRoute("good.txt", "autopilot-route 1\nname g\nstep 0.05 left.grip=1\n");
  const std::string bad = WriteRoute("bad.txt", "autopilot-route 1\nname g\nstep 0.05 left.wheel=1\n");
  // 64 KiB and one byte: refused by its size, not read in part.
  std::string big = "autopilot-route 1\nname g\n";
  big += std::string(64 * 1024 - big.size(), '#');
  big += "\nstep 1\n";
  const std::string tooBig = WriteRoute("big.txt", big);
  const ap::plugin::Os& os = ap::plugin::RealOs();
  const NevrQuestHostApi small{sizeof(std::uint32_t), &Register, nullptr};
  const NevrQuestHostApi noFn{sizeof(NevrQuestHostApi), nullptr, nullptr};
  Refused("not json", &kApi, os, "route_refused", "reason", "the plugin's args are not a JSON object");
  Refused("[1]", &kApi, os, "route_refused", "reason", "the plugin's args are not a JSON object");
  Refused("{}", &kApi, os, "route_refused", "reason", "args.route is not a path");
  Refused("{\"route\":\"\"}", &kApi, os, "route_refused", "reason", "args.route is not a path");
  Refused("{\"route\":3}", &kApi, os, "route_refused", "reason", "args.route is not a path");
  Refused("{\"route\":\"" + good + "\",\"start_file\":1}", &kApi, os, "route_refused", "reason",
          "args.start_file is not a path");
  Refused(Args(g_dir + "/absent.txt"), &kApi, os, "route_refused", "reason", "cannot open: No such file or directory");
  Refused(Args(g_dir), &kApi, os, "route_refused", "reason", "read error");  // a directory opens and does not read
  Refused(Args(bad), &kApi, os, "route_refused", "line", "3");
  Refused(Args(bad), &kApi, os, "route_refused", "reason", "unknown control 'wheel'");
  Refused(Args(bad), &kApi, os, "route_refused", "route", bad.c_str());
  Refused(Args(tooBig), &kApi, os, "route_refused", "reason", "the file is larger than 64 KiB");
  Refused(Args(good), nullptr, os, "input_filter", "result", "no_host_api");
  Refused(Args(good), &small, os, "input_filter", "result", "no_host_api");
  Refused(Args(good), &noFn, os, "input_filter", "result", "no_host_api");
  Refused(Args(good), &kApi, kNoWake, "input_filter", "result", "no_wake_fd");
  Refused(Args(good), &kApi, kNoWake, "input_filter", "errno", "24");
  Refused(Args(good), &kApi, kNoThread, "input_filter", "result", "no_reporter_thread");
  Refused(Args(good), &kApi, kNoThread, "input_filter", "errno", "11");
  QCHECK(g_registers == 0);
  // A context too small to carry `quest` is treated as no host API, whatever lies past its end.
  ResetHost();
  NvrGameContext shortCtx = Context(&kApi);
  shortCtx.ctx_size = static_cast<std::uint32_t>(offsetof(NvrGameContext, quest));
  QCHECK(NvrPluginInitEx(&shortCtx, Args(good).c_str()) == 1);
  QCHECK(g_registers == 0);
  QCHECK(NvrPluginInitEx(nullptr, Args(good).c_str()) == 1);
}

void EachHostRefusalIsNamedAndTheReporterJoined() {
  const std::string good = WriteRoute("good.txt", "autopilot-route 1\nname g\nstep 0.05 left.grip=1\n");
  const struct {
    std::int32_t result;
    const char* name;
  } cases[] = {{NEVR_QUEST_FILTER_BAD_ARGUMENT, "bad_argument"},
               {NEVR_QUEST_FILTER_NOT_DEV_BUILD, "not_dev_build"},
               {NEVR_QUEST_FILTER_OWNED, "owned"},
               {NEVR_QUEST_FILTER_HOOK_REFUSED, "hook_refused"},
               {99, "unknown"}};
  for (const auto& c : cases) {
    ResetHost();
    g_registerResult = c.result;
    g_slowReporterEnd = true;
    const std::size_t fds = Fds();
    const NvrGameContext ctx = Context(&kApi);
    QCHECK(NvrPluginInitEx(&ctx, Args(good).c_str()) == 1);
    g_slowReporterEnd = false;
    Logged l;
    QCHECK(FindLog("input_filter", &l) && l.fields["result"] == c.name && l.level == NEVR_LOG_ERROR);
    QCHECK(g_registers == 1);
    // The reporter was stopped and joined before init returned: its last line is already there.
    Logged end;
    QCHECK(FindLog("reporter_end", &end) && end.fields["reason"] == "stopped" && end.level == NEVR_LOG_DEBUG);
    QCHECK(Fds() == fds);
    QCHECK(ThreadsFallTo(kMainOnly, 1000));
  }
}

// What route_loaded says of a route, read from inits the host refuses (a refused init leaves the process free to init
// again), and the plugin id its lines and its registration carry.
void RouteLoadedDescribesTheRoute() {
  const auto loaded = [](const std::string& text, const char* id) {
    ResetHost();
    g_registerResult = NEVR_QUEST_FILTER_NOT_DEV_BUILD;
    const std::string path = WriteRoute("described.txt", text);
    NvrGameContext ctx = Context(&kApi);
    ctx.plugin_id = id;
    QCHECK(NvrPluginInitEx(&ctx, Args(path).c_str()) == 1);
    Logged l;
    QCHECK(FindLog("route_loaded", &l) && l.level == NEVR_LOG_INFO && l.fields["route"] == path);
    return l;
  };
  const std::string head = "autopilot-route 1\nname d\n";
  QCHECK(loaded(head + "step 1 left.grip=1\n", "nevr_autopilot").fields["drives"] == "left");
  QCHECK(loaded(head + "step 1 right.grip=1\n", "nevr_autopilot").fields["drives"] == "right");
  QCHECK(loaded(head + "step 1\n", "nevr_autopilot").fields["drives"] == "none");
  QCHECK(loaded(head + "step 1 left.grip=1 right.grip=1\n", "nevr_autopilot").fields["drives"] == "left+right");
  Logged l = loaded("autopilot-route 1\nname fixed\nstart_after 12.5\nstep 1.25 left.grip=1\n", "other_id");
  QCHECK(l.fields["name"] == "fixed");
  QCHECK(l.fields["sha256"] == "83120a6aba1e779540cbe992ffa3d7ceba2df52057200434c957a4f8f452c0da");
  QCHECK(l.fields["steps"] == "1");
  QCHECK(l.fields["repeat"] == "1");
  QCHECK(l.fields["planned_s"] == "1.250000000");
  QCHECK(l.fields["start_after_s"] == "12.500000000");
  QCHECK(l.fields["start"] == "first_frame");
  QCHECK(l.fields.size() == 9u);
  // The context's plugin id names every line and the registration; without one, the plugin's own name does.
  QCHECK(AllFrom("other_id") && g_registeredId == "other_id");
  loaded(head + "step 1\n", nullptr);
  QCHECK(AllFrom("nevr_autopilot") && g_registeredId == "nevr_autopilot");
}

void ARunLogsItsWindow() {
  ResetHost();
  const std::string text =
      "autopilot-route 1\nname window\nstart_after 0.02\nrepeat 2\nstep 0.05 left.stick=0,1\nstep 0.03 right.trigger=1\n";
  const std::string path = WriteRoute("window.txt", text);
  const NvrGameContext ctx = Context(&kApi);
  QCHECK(ThreadsFallTo(kMainOnly, 1000));
  const Bracket before;
  QCHECK(NvrPluginInitEx(&ctx, Args(path).c_str()) == 0);
  Logged loaded;
  QCHECK(FindLog("route_loaded", &loaded));
  QCHECK(loaded.fields["name"] == "window");
  QCHECK(loaded.fields["steps"] == "2");
  QCHECK(loaded.fields["repeat"] == "2");
  QCHECK(loaded.fields["planned_s"] == "0.160000000");
  QCHECK(loaded.fields["start_after_s"] == "0.020000000");
  QCHECK(loaded.fields["drives"] == "left+right");
  Logged reg;
  QCHECK(FindLog("input_filter", &reg) && reg.fields["result"] == "registered" && reg.level == NEVR_LOG_INFO);
  QCHECK(reg.fields.size() == 1u);
  QCHECK(g_filter != nullptr && g_registeredId == "nevr_autopilot");
  QCHECK(Threads() == kMainOnly + 1);  // the reporter
  QCHECK(DriveUntil("route_end", 3000));
  const Bracket after;
  Logged start, end;
  QCHECK(FindLog("route_start", &start) && start.level == NEVR_LOG_INFO);
  QCHECK(FindLog("route_end", &end) && end.level == NEVR_LOG_INFO);
  QCHECK(start.fields.size() == 4u && start.fields["name"] == "window");
  QCHECK(ClocksInside(start, before, after, true));
  QCHECK(end.fields.size() == 13u && end.fields["name"] == "window");
  QCHECK(ClocksInside(end, before, after, true));
  QCHECK(end.fields["planned_s"] == "0.160000000");
  QCHECK(Matches(end.fields["measured_s"], "[0-9]+\\.[0-9]{9}"));
  QCHECK(Matches(end.fields["max_frame_gap_ms"], "[0-9]+\\.[0-9]{6}"));
  const double measured = std::atof(end.fields["measured_s"].c_str());
  const double gap = std::atof(end.fields["max_frame_gap_ms"].c_str());
  QCHECK(gap > 0.0);
  QCHECK(measured >= 0.160 && measured < 0.160 + gap / 1000.0 + 0.001);  // late by less than one frame gap
  const long long measuredNs = std::atoll(end.fields["monotonic_ns"].c_str()) - std::atoll(start.fields["monotonic_ns"].c_str());
  QCHECK(std::llround(measured * 1e9) == measuredNs);
  QCHECK(std::atoll(end.fields["frames_left"].c_str()) > 0);
  QCHECK(std::atoll(end.fields["frames_right"].c_str()) > 0);
  QCHECK(end.fields["frames_left"] == end.fields["frames_driven_left"]);
  QCHECK(end.fields["frames_right"] == end.fields["frames_driven_right"]);
  QCHECK(end.fields["frames_unknown_hand"] == "0");
  QCHECK(end.fields["events_dropped"] == "0");
  // Four steps (two per pass) at Debug, in order, each inside the run.
  const std::vector<Logged> steps = AllLog("route_step");
  QCHECK(steps.size() == 4u);
  const char* const order[4][2] = {{"0", "0"}, {"1", "0"}, {"0", "1"}, {"1", "1"}};
  for (std::size_t i = 0; i < steps.size() && i < 4; ++i) {
    QCHECK(steps[i].level == NEVR_LOG_DEBUG && steps[i].fields.size() == 4u);
    QCHECK(steps[i].fields.at("index") == order[i][0] && steps[i].fields.at("pass") == order[i][1]);
    QCHECK(Between(steps[i].fields.at("monotonic_ns"), before.monotonic, after.monotonic));
    QCHECK(ClocksInside(steps[i], before, after, false));
  }
  // The reporter ends after route_end, on its own.
  QCHECK(ThreadsFallTo(kMainOnly, 1000));
  Logged done;
  QCHECK(FindLog("reporter_end", &done) && done.fields["reason"] == "route_end");
  // A second init in the same process is refused: one run per process.
  QCHECK(NvrPluginInitEx(&ctx, Args(path).c_str()) == 1);
  NvrPluginShutdown();
  QCHECK(CountLog("route_stopped") == 0u);  // it ended
}

}  // namespace

// The start file and the shutdown of an unfinished run: a separate process each, since the plugin keeps one run per
// process.
int ChildStartFile() {
  const std::string path = WriteRoute("trigger.txt", "autopilot-route 1\nname trig\nstep 0.05 right.grip=1\n");
  const std::string startFile = g_dir + "/start";
  ::unlink(startFile.c_str());
  const NvrGameContext ctx = Context(&kApi);
  QCHECK(NvrPluginInitEx(&ctx, Args(path, startFile).c_str()) == 0);
  Logged loaded;
  QCHECK(FindLog("route_loaded", &loaded) && loaded.fields["start"] == "start_file");
  // Without the file, a frame changes nothing.
  QCHECK(!DriveUntil("route_start", 300));
  NevrQuestControllerState r = Real(NEVR_QUEST_HAND_RIGHT);
  g_filter(g_user, &r);
  QCHECK(Same(r, Real(NEVR_QUEST_HAND_RIGHT)));
  const Bracket before;
  std::FILE* f = std::fopen(startFile.c_str(), "wb");
  std::fclose(f);
  QCHECK(DriveUntil("start_file_seen", 1000));
  const Bracket after;
  Logged seen;
  QCHECK(FindLog("start_file_seen", &seen) && seen.level == NEVR_LOG_INFO && seen.fields.size() == 2u);
  QCHECK(seen.fields["start_file"] == startFile);
  QCHECK(ClocksInside(seen, before, after, false));
  QCHECK(DriveUntil("route_start", 1000));
  r = Real(NEVR_QUEST_HAND_RIGHT);
  g_filter(g_user, &r);
  QCHECK(r.grip_trigger == 1.0f);
  QCHECK(DriveUntil("route_end", 2000));
  NvrPluginShutdown();
  return quest_test::Failures() == 0 ? 0 : 1;
}

// Shutdown before the route ends: route_stopped, the reporter joined before Shutdown returns, and the filter inert.
// While it waits, the reporter takes no CPU.
int ChildStopped() {
  const std::string path = WriteRoute("long.txt", "autopilot-route 1\nname long\nstep 60 left.grip=1\n");
  const NvrGameContext ctx = Context(&kApi);
  QCHECK(NvrPluginInitEx(&ctx, Args(path).c_str()) == 0);
  QCHECK(DriveUntil("route_start", 1000));
  const std::int64_t cpu0 = ClockNs(CLOCK_PROCESS_CPUTIME_ID);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const std::int64_t idleCpuNs = ClockNs(CLOCK_PROCESS_CPUTIME_ID) - cpu0;
  if (idleCpuNs >= 60 * kMs) std::fprintf(stderr, "the waiting reporter used %lld ns of CPU in 300 ms\n",
                                          static_cast<long long>(idleCpuNs));
  QCHECK(idleCpuNs < 60 * kMs);
  g_slowReporterEnd = true;
  const Bracket before;
  NvrPluginShutdown();
  const Bracket after;
  Logged done;
  QCHECK(FindLog("reporter_end", &done) && done.fields["reason"] == "stopped");
  QCHECK(ThreadsFallTo(kMainOnly, 1000));
  Logged stopped;
  QCHECK(FindLog("route_stopped", &stopped) && stopped.level == NEVR_LOG_WARNING);
  QCHECK(stopped.fields.size() == 5u);
  QCHECK(stopped.fields["name"] == "long" && stopped.fields["started"] == "true");
  QCHECK(ClocksInside(stopped, before, after, false));
  QCHECK(std::atoll(stopped.fields["frames_driven_left"].c_str()) > 0);
  QCHECK(stopped.fields["frames_driven_right"] == "0");
  // After shutdown the filter changes nothing.
  NevrQuestControllerState l = Real(NEVR_QUEST_HAND_LEFT);
  g_filter(g_user, &l);
  QCHECK(Same(l, Real(NEVR_QUEST_HAND_LEFT)));
  QCHECK(CountLog("route_end") == 0u);
  return quest_test::Failures() == 0 ? 0 : 1;
}

// Shutdown while the reporter still waits for the start file: it stops within one poll, and the run never started.
int ChildStoppedWaiting() {
  const std::string path = WriteRoute("waiting.txt", "autopilot-route 1\nname waiting\nstep 1 left.grip=1\n");
  const std::string startFile = g_dir + "/never";
  ::unlink(startFile.c_str());
  const NvrGameContext ctx = Context(&kApi);
  QCHECK(NvrPluginInitEx(&ctx, Args(path, startFile).c_str()) == 0);
  QCHECK(!DriveUntil("route_start", 200));
  NvrPluginShutdown();
  Logged done;
  QCHECK(FindLog("reporter_end", &done) && done.fields["reason"] == "stopped");
  QCHECK(ThreadsFallTo(kMainOnly, 1000));
  QCHECK(CountLog("start_file_seen") == 0u);
  Logged stopped;
  QCHECK(FindLog("route_stopped", &stopped) && stopped.fields["started"] == "false");
  QCHECK(stopped.fields["frames_driven_left"] == "0");
  return quest_test::Failures() == 0 ? 0 : 1;
}

// The library carries its NEVR build-ID note, which the Quest loader refuses a plugin without (build_ids_undeclared):
// read from this program's own PT_NOTE segments, where plugin.cpp's note is linked.
void TheImageDeclaresItsBuildIds() {
  struct Found {
    bool seen = false;
    std::string desc;
  } found;
  dl_iterate_phdr(
      [](dl_phdr_info* info, std::size_t, void* data) -> int {
        if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') return 0;  // the main program only
        Found* f = static_cast<Found*>(data);
        for (int i = 0; i < info->dlpi_phnum; ++i) {
          const ElfW(Phdr)& ph = info->dlpi_phdr[i];
          if (ph.p_type != PT_NOTE) continue;
          const unsigned char* at = reinterpret_cast<const unsigned char*>(info->dlpi_addr + ph.p_vaddr);
          const unsigned char* const end = at + ph.p_memsz;
          while (end - at >= static_cast<std::ptrdiff_t>(sizeof(ElfW(Nhdr)))) {
            ElfW(Nhdr) n;
            std::memcpy(&n, at, sizeof n);
            const unsigned char* name = at + sizeof n;
            const std::size_t nameLen = (n.n_namesz + 3u) & ~std::size_t{3};
            const std::size_t descLen = (n.n_descsz + 3u) & ~std::size_t{3};
            if (static_cast<std::size_t>(end - name) < nameLen + descLen) break;
            if (n.n_type == 1 && n.n_namesz == 5 && std::memcmp(name, "NEVR", 5) == 0) {
              f->seen = true;
              f->desc.assign(reinterpret_cast<const char*>(name + nameLen), n.n_descsz);
            }
            at = name + nameLen + descLen;
          }
        }
        return 1;
      },
      &found);
  QCHECK(found.seen);
  QCHECK(found.desc == NEVR_AUTOPILOT_BUILD_IDS);
}

// With stdin closed the wake eventfd is descriptor 0, a valid one: a refused init closes it, and an init that gets it
// runs the route.
int ChildFdZero() {
  const std::string path = WriteRoute("zero.txt", "autopilot-route 1\nname zero\nstep 0.2 left.grip=1\n");
  const NvrGameContext ctx = Context(&kApi);
  ::close(0);
  QCHECK(ap::plugin::InitWith(&ctx, Args(path).c_str(), kNoThread) == 1);
  QCHECK(::fcntl(0, F_GETFD) == -1 && errno == EBADF);
  QCHECK(ap::plugin::InitWith(&ctx, Args(path).c_str(), ap::plugin::RealOs()) == 0);
  char target[64] = {};
  QCHECK(::readlink("/proc/self/fd/0", target, sizeof target - 1) > 0);
  QCHECK(std::strcmp(target, "anon_inode:[eventfd]") == 0);
  QCHECK(DriveUntil("route_end", 3000));
  ap::plugin::Shutdown();
  return quest_test::Failures() == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: autopilot_run_test <scratch dir> [start_file|stopped|stopped_waiting|fd_zero]\n");
    return 2;
  }
  g_dir = argv[1];
  if (argc == 3 && std::strcmp(argv[2], "start_file") == 0) return ChildStartFile();
  if (argc == 3 && std::strcmp(argv[2], "stopped") == 0) return ChildStopped();
  if (argc == 3 && std::strcmp(argv[2], "stopped_waiting") == 0) return ChildStoppedWaiting();
  if (argc == 3 && std::strcmp(argv[2], "fd_zero") == 0) return ChildFdZero();
  TheRouteDrivesOnlyItsHandsAndOnlyWhileItRuns();
  AnUnknownHandIsCountedAndNeverDriven();
  TheStartWaitsForTheTrigger();
  AFrameThatJumpsPastTheEndStillReportsTheStart();
  StopEndsTheRunWithoutAnEnd();
  AFullRingDropsAndCounts();
  TheRingKeepsOrderAcrossWraps();
  TheRingTakesTwoProducers();
  ThePluginNamesItself();
  TheImageDeclaresItsBuildIds();
  InitRefusesWhatItCannotRun();
  EachHostRefusalIsNamedAndTheReporterJoined();
  RouteLoadedDescribesTheRoute();
  ARunLogsItsWindow();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "autopilot_run_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("autopilot_run_test: all checks pass\n");
  return 0;
}
