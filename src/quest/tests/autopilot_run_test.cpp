// Host test for the autopilot's run and its plugin entry points (#1202, src/quest/plugins/autopilot/autopilot.h and
// plugin.cpp; docs/adr/0025-quest-autopilot-route.md). The filter is driven with a fake clock; the plugin is driven through
// its exported functions with a fake host (a context whose quest API records the registered filter and whose log records
// every line), a real route file in a temporary directory, and its real reporter thread.
//
// Built and run by `just test-quest-autopilot`.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "extension/plugin_interface.h"
#include "quest/plugins/autopilot/autopilot.h"
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
  run.Filter(&odd);
  QCHECK(Same(odd, Real(7)));
  QCHECK(run.Count().framesUnknownHand.load() == 2u);
  NevrQuestControllerState r = Real(NEVR_QUEST_HAND_RIGHT);
  run.Filter(&r);
  QCHECK(r.index_trigger == 1.0f && (r.buttons & 0x20000000u) != 0);
  QCHECK(run.Count().drivenRight.load() == 1u);
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
  std::uint32_t level;
  std::string message;
  std::map<std::string, std::string> fields;
};
std::mutex g_logMutex;
std::vector<Logged> g_log;
void HostLog(const char* plugin_id, uint32_t level, const char* message, const NevrLogField* fields, uint32_t n) {
  Logged l{level, message, {}};
  for (uint32_t i = 0; i < n; ++i) l.fields[fields[i].key] = fields[i].value;
  QCHECK(std::strcmp(plugin_id, "nevr_autopilot") == 0);
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
std::size_t CountLog(const char* message) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  std::size_t n = 0;
  for (const Logged& l : g_log) n += l.message == message ? 1u : 0u;
  return n;
}

NevrQuestInputFilterFn g_filter = nullptr;
void* g_user = nullptr;
std::int32_t g_registerResult = NEVR_QUEST_FILTER_OK;
int g_registers = 0;
std::int32_t Register(const char* id, NevrQuestInputFilterFn filter, void* user) {
  ++g_registers;
  QCHECK(std::strcmp(id, "nevr_autopilot") == 0);
  if (g_registerResult != NEVR_QUEST_FILTER_OK) return g_registerResult;
  g_filter = filter;
  g_user = user;
  return NEVR_QUEST_FILTER_OK;
}
const NevrQuestHostApi kApi{sizeof(NevrQuestHostApi), &Register};

NvrGameContext Context(const NevrQuestHostApi* api) {
  NvrGameContext ctx{};
  ctx.flags = NEVR_HOST_IS_CLIENT | NEVR_HOST_PLATFORM_QUEST;
  ctx.ctx_size = sizeof(NvrGameContext);
  ctx.plugin_id = "nevr_autopilot";
  ctx.log = &HostLog;
  ctx.quest = api;
  return ctx;
}

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
  QCHECK(NvrPluginGetApiVersion() == NEVR_PLUGIN_API_VERSION);
}

void InitRefusesWhatItCannotRun() {
  const std::string good = WriteRoute("good.txt", "autopilot-route 1\nname g\nstep 0.05 left.grip=1\n");
  const std::string bad = WriteRoute("bad.txt", "autopilot-route 1\nname g\nstep 0.05 left.wheel=1\n");
  struct Case {
    std::string args;
    const NevrQuestHostApi* api;
    const char* message;
    const char* field;
    const char* value;
  };
  const NevrQuestHostApi small{sizeof(std::uint32_t), &Register};
  const Case cases[] = {
      {"not json", &kApi, "route_refused", "reason", "the plugin's args are not a JSON object"},
      {"[1]", &kApi, "route_refused", "reason", "the plugin's args are not a JSON object"},
      {"{}", &kApi, "route_refused", "reason", "args.route is not a path"},
      {"{\"route\":3}", &kApi, "route_refused", "reason", "args.route is not a path"},
      {"{\"route\":\"" + good + "\",\"start_file\":1}", &kApi, "route_refused", "reason", "args.start_file is not a path"},
      {Args(g_dir + "/absent.txt"), &kApi, "route_refused", "line", "0"},
      {Args(bad), &kApi, "route_refused", "line", "3"},
      {Args(good), nullptr, "input_filter", "result", "no_host_api"},
      {Args(good), &small, "input_filter", "result", "no_host_api"},
  };
  for (const Case& c : cases) {
    ResetHost();
    const NvrGameContext ctx = Context(c.api);
    QCHECK(NvrPluginInitEx(&ctx, c.args.c_str()) != 0);
    Logged l;
    const bool found = FindLog(c.message, &l);
    if (!found || l.fields[c.field] != c.value) {
      std::fprintf(stderr, "args %s: expected %s %s=%s, got %s\n", c.args.c_str(), c.message, c.field, c.value,
                   found ? l.fields[c.field].c_str() : "<no line>");
    }
    QCHECK(found && l.fields[c.field] == c.value);
    QCHECK(l.level == NEVR_LOG_ERROR);
    QCHECK(g_filter == nullptr);
  }
  // A context too small to carry `quest` is treated as no host API, whatever lies past its end.
  ResetHost();
  NvrGameContext shortCtx = Context(&kApi);
  shortCtx.ctx_size = static_cast<std::uint32_t>(offsetof(NvrGameContext, quest));
  QCHECK(NvrPluginInitEx(&shortCtx, Args(good).c_str()) != 0);
  QCHECK(g_registers == 0);
  // The host refuses: init fails with the host's token, and the plugin can init again.
  ResetHost();
  g_registerResult = NEVR_QUEST_FILTER_NOT_DEV_BUILD;
  const NvrGameContext ctx = Context(&kApi);
  QCHECK(NvrPluginInitEx(&ctx, Args(good).c_str()) != 0);
  Logged l;
  QCHECK(FindLog("input_filter", &l) && l.fields["result"] == "not_dev_build");
  QCHECK(FindLog("route_loaded", nullptr));
}

void ARunLogsItsWindow() {
  ResetHost();
  const std::string text = "autopilot-route 1\nname window\nstart_after 0.02\nrepeat 2\nstep 0.05 left.stick=0,1\nstep 0.03 right.trigger=1\n";
  const std::string path = WriteRoute("window.txt", text);
  const NvrGameContext ctx = Context(&kApi);
  QCHECK(NvrPluginInitEx(&ctx, Args(path).c_str()) == 0);
  Logged loaded;
  QCHECK(FindLog("route_loaded", &loaded));
  QCHECK(loaded.level == NEVR_LOG_INFO);
  QCHECK(loaded.fields["route"] == path);
  QCHECK(loaded.fields["name"] == "window");
  QCHECK(loaded.fields["steps"] == "2");
  QCHECK(loaded.fields["repeat"] == "2");
  QCHECK(loaded.fields["planned_s"] == "0.160000000");
  QCHECK(loaded.fields["start_after_s"] == "0.020000000");
  QCHECK(loaded.fields["start"] == "first_frame");
  QCHECK(loaded.fields["drives"] == "left+right");
  QCHECK(loaded.fields["sha256"].size() == 64u);
  Logged reg;
  QCHECK(FindLog("input_filter", &reg) && reg.fields["result"] == "registered" && reg.level == NEVR_LOG_INFO);
  QCHECK(g_filter != nullptr);
  QCHECK(DriveUntil("route_end", 3000));
  Logged start, end;
  QCHECK(FindLog("route_start", &start));
  QCHECK(FindLog("route_end", &end));
  QCHECK(start.fields["name"] == "window");
  QCHECK(!start.fields["realtime_ms"].empty() && !start.fields["monotonic_ns"].empty() &&
         !start.fields["boottime_ns"].empty());
  QCHECK(end.fields["planned_s"] == "0.160000000");
  const double measured = std::atof(end.fields["measured_s"].c_str());
  const double gap = std::atof(end.fields["max_frame_gap_ms"].c_str());
  QCHECK(measured >= 0.160 && measured < 0.160 + gap / 1000.0 + 0.001);  // late by less than one frame gap
  QCHECK(std::atoll(end.fields["frames_left"].c_str()) > 0);
  QCHECK(end.fields["frames_left"] == end.fields["frames_driven_left"]);
  QCHECK(end.fields["frames_right"] == end.fields["frames_driven_right"]);
  QCHECK(end.fields["frames_unknown_hand"] == "0");
  QCHECK(end.fields["events_dropped"] == "0");
  QCHECK(std::atoll(end.fields["monotonic_ns"].c_str()) > std::atoll(start.fields["monotonic_ns"].c_str()));
  // Four steps (two per pass), at Debug.
  QCHECK(CountLog("route_step") == 4u);
  Logged step;
  QCHECK(FindLog("route_step", &step) && step.level == NEVR_LOG_DEBUG && step.fields["index"] == "0" &&
         step.fields["pass"] == "0");
  // A second init in the same process is refused: one run per process.
  QCHECK(NvrPluginInitEx(&ctx, Args(path).c_str()) != 0);
  NvrPluginShutdown();
  QCHECK(CountLog("route_stopped") == 0u);  // it ended
}

}  // namespace

// The start file and the shutdown of an unfinished run: a separate process, since the plugin keeps one run per process.
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
  std::FILE* f = std::fopen(startFile.c_str(), "wb");
  std::fclose(f);
  QCHECK(DriveUntil("start_file_seen", 1000));
  QCHECK(DriveUntil("route_start", 1000));
  r = Real(NEVR_QUEST_HAND_RIGHT);
  g_filter(g_user, &r);
  QCHECK(r.grip_trigger == 1.0f);
  QCHECK(DriveUntil("route_end", 2000));
  NvrPluginShutdown();
  return quest_test::Failures() == 0 ? 0 : 1;
}

int ChildStopped() {
  const std::string path = WriteRoute("long.txt", "autopilot-route 1\nname long\nstep 60 left.grip=1\n");
  const NvrGameContext ctx = Context(&kApi);
  QCHECK(NvrPluginInitEx(&ctx, Args(path).c_str()) == 0);
  QCHECK(DriveUntil("route_start", 1000));
  NvrPluginShutdown();
  Logged stopped;
  QCHECK(FindLog("route_stopped", &stopped) && stopped.level == NEVR_LOG_WARNING && stopped.fields["started"] == "true");
  QCHECK(stopped.fields["name"] == "long");
  QCHECK(std::atoll(stopped.fields["frames_driven_left"].c_str()) > 0);
  // After shutdown the filter changes nothing.
  NevrQuestControllerState l = Real(NEVR_QUEST_HAND_LEFT);
  g_filter(g_user, &l);
  QCHECK(Same(l, Real(NEVR_QUEST_HAND_LEFT)));
  QCHECK(CountLog("route_end") == 0u);
  return quest_test::Failures() == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: autopilot_run_test <scratch dir> [start_file|stopped]\n");
    return 2;
  }
  g_dir = argv[1];
  if (argc == 3 && std::strcmp(argv[2], "start_file") == 0) return ChildStartFile();
  if (argc == 3 && std::strcmp(argv[2], "stopped") == 0) return ChildStopped();
  TheRouteDrivesOnlyItsHandsAndOnlyWhileItRuns();
  AnUnknownHandIsCountedAndNeverDriven();
  TheStartWaitsForTheTrigger();
  AFrameThatJumpsPastTheEndStillReportsTheStart();
  StopEndsTheRunWithoutAnEnd();
  AFullRingDropsAndCounts();
  TheRingKeepsOrderAcrossWraps();
  TheRingTakesTwoProducers();
  ThePluginNamesItself();
  InitRefusesWhatItCannotRun();
  ARunLogsItsWindow();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "autopilot_run_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("autopilot_run_test: all checks pass\n");
  return 0;
}
