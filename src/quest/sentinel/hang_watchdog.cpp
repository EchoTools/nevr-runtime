#include "hang_watchdog.h"

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hook_install.h"
#include "hook_log.h"
#include "process_end_hook.h"

namespace nevr_quest::hang_watchdog {

namespace {

using sentinel::pinned::OvrMobileOpaque;
using sentinel::pinned::OvrModeParmsOpaque;

constexpr std::int64_t kNsPerMs = 1000000LL;
constexpr std::int64_t kNsPerSecond = 1000000000LL;

// ---- state ----------------------------------------------------------------------------------------------------------------

std::atomic<std::int64_t> g_lastNs{0};   // the last frame's time; 0 = not armed
std::atomic<std::int64_t> g_armNs{0};    // the arming frame's time
std::atomic<int> g_inVr{0};
std::atomic<unsigned long long> g_frames{0};
std::atomic<bool> g_on{false};           // set once by the install, read by the observer and the watcher
std::atomic<unsigned> g_seconds{kDefaultSeconds};
std::atomic<EndFn> g_end{nullptr};

std::int64_t NowNs() noexcept {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<std::int64_t>(t.tv_sec) * kNsPerSecond + t.tv_nsec;
}

// ---- the two VR-mode hooks ------------------------------------------------------------------------------------------------

// The state change runs after the call into VrApi returned: nothing of ours is live across game code.
OvrMobileOpaque* OnEnterVrMode(EnterThunk::Fn original, const OvrModeParmsOpaque* parms) noexcept {
  OvrMobileOpaque* const ovr = original(parms);
  if (ovr != nullptr) EnterVr();
  return ovr;
}

void OnLeaveVrMode(LeaveThunk::Fn original, OvrMobileOpaque* ovr) noexcept {
  LeaveVr();  // before the call: from here the game submits no frame until the next EnterVrMode
  original(ovr);
}

NEVR_HOOK_RECORD(kEnterHook, EnterThunk, &OnEnterVrMode);
NEVR_HOOK_RECORD(kLeaveHook, LeaveThunk, &OnLeaveVrMode);

sentinel::GotHook g_hooks[2];

sentinel::GotStatus RealInstall(int slot) noexcept {
  switch (slot) {
    case 0: return sentinel::InstallThunk<EnterThunk>(g_hooks[0], sentinel::pinned::LibR15EnterVrMode());
    case 1: return sentinel::InstallThunk<LeaveThunk>(g_hooks[1], sentinel::pinned::LibR15LeaveVrMode());
    default: return sentinel::GotStatus::kBadArgument;
  }
}

void EndThroughProcessEnd(const char* reason) noexcept { nevr_quest::process_end::EndNow(reason); }

// ---- switches ---------------------------------------------------------------------------------------------------------------

// The q15t data directory (module_switch.h): /sdcard/Android/data/<package>/files/q15t, the package from /proc/self/cmdline.
bool DataDir(char* out, std::size_t n) noexcept {
  char cmd[256] = "";
  FILE* f = std::fopen("/proc/self/cmdline", "re");
  if (f == nullptr) return false;
  const std::size_t r = std::fread(cmd, 1, sizeof cmd - 1, f);
  std::fclose(f);
  cmd[r] = 0;
  if (char* colon = std::strchr(cmd, ':')) *colon = 0;
  if (std::strchr(cmd, '.') == nullptr || std::strchr(cmd, '/') != nullptr || std::strlen(cmd) > 160) return false;
  const int w = std::snprintf(out, n, "/sdcard/Android/data/%.160s/files/q15t", cmd);
  return w > 0 && static_cast<std::size_t>(w) < n;
}

bool FileIn(const char* dir, const char* name, char* path, std::size_t n) noexcept {
  const int w = std::snprintf(path, n, "%s/%s", dir, name);
  return w > 0 && static_cast<std::size_t>(w) < n;
}

// ---- the watcher ------------------------------------------------------------------------------------------------------------

void* WatcherThread(void*) {
  for (;;) {
    timespec pause{static_cast<time_t>(kPollMs / 1000), static_cast<long>(kPollMs % 1000) * 1000000L};
    while (nanosleep(&pause, &pause) != 0) {
    }
    if (PollOnce(NowNs())) return nullptr;  // only a test's ender returns
  }
}

}  // namespace

// ---- state ------------------------------------------------------------------------------------------------------------------

void Beat(std::int64_t nowNs) noexcept {
  if (!g_on.load(std::memory_order_acquire)) return;
  if (g_lastNs.load(std::memory_order_relaxed) == 0) g_armNs.store(nowNs, std::memory_order_relaxed);
  g_frames.fetch_add(1, std::memory_order_relaxed);
  g_lastNs.store(nowNs, std::memory_order_release);
}

void BeatNow() noexcept { Beat(NowNs()); }

void EnterVr() noexcept {
  g_lastNs.store(0, std::memory_order_release);  // armed again by the next frame
  g_inVr.store(1, std::memory_order_release);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "hangwd_vr",
                      {{"state", "entered"}, {"frames", g_frames.load(std::memory_order_relaxed)}});
}

void LeaveVr() noexcept {
  g_inVr.store(0, std::memory_order_release);
  g_lastNs.store(0, std::memory_order_release);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "hangwd_vr",
                      {{"state", "left"}, {"frames", g_frames.load(std::memory_order_relaxed)}});
}

Verdict Check(std::int64_t nowNs, unsigned seconds) noexcept {
  Verdict v{false, 0, g_frames.load(std::memory_order_relaxed), 0};
  const std::int64_t last = g_lastNs.load(std::memory_order_acquire);
  if (last == 0 || g_inVr.load(std::memory_order_acquire) == 0) return v;
  const std::int64_t gap = nowNs - last;
  v.gapMs = gap / kNsPerMs;
  v.armedMs = (last - g_armNs.load(std::memory_order_relaxed)) / kNsPerMs;
  if (gap < static_cast<std::int64_t>(seconds) * kNsPerSecond) return v;
  // A frame or a pause that landed while this look ran wins: the state must be the one the gap was measured on.
  if (g_lastNs.load(std::memory_order_acquire) != last || g_inVr.load(std::memory_order_acquire) == 0) return v;
  v.expired = true;
  return v;
}

bool ParseSeconds(const char* text, unsigned* out) noexcept {
  if (text == nullptr) return false;
  while (std::isspace(static_cast<unsigned char>(*text)) != 0) ++text;
  if (std::isdigit(static_cast<unsigned char>(*text)) == 0) return false;
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  while (std::isspace(static_cast<unsigned char>(*end)) != 0) ++end;
  if (*end != 0 || value < kMinSeconds || value > kMaxSeconds) return false;
  *out = static_cast<unsigned>(value);
  return true;
}

Settings ReadSettings(const char* dir) noexcept {
  Settings s{false, kDefaultSeconds, "default"};
  char own[300] = "";
  if (dir == nullptr) {
    if (!DataDir(own, sizeof own)) return s;
    dir = own;
  }
  char path[400];
  if (FileIn(dir, kOffSwitch, path, sizeof path) && access(path, F_OK) == 0) {
    s.off = true;
    return s;
  }
  if (!FileIn(dir, kSecondsFile, path, sizeof path)) return s;
  FILE* f = std::fopen(path, "re");
  if (f == nullptr) return s;
  char text[32] = "";
  const std::size_t r = std::fread(text, 1, sizeof text - 1, f);
  std::fclose(f);
  text[r] = 0;
  unsigned seconds = 0;
  if (ParseSeconds(text, &seconds)) {
    s.seconds = seconds;
    s.source = "hangwd.sec";
  } else {
    s.source = "hangwd.sec_ignored";
  }
  return s;
}

bool PollOnce(std::int64_t nowNs) noexcept {
  if (!g_on.load(std::memory_order_acquire)) return false;
  const unsigned seconds = g_seconds.load(std::memory_order_relaxed);
  const Verdict v = Check(nowNs, seconds);
  if (!v.expired) return false;
  sentinel::LogFields(sentinel::LogLevel::kWarn, "hangwd_exit",
                      {{"reason", "no_frame_submitted_in_vr"},
                       {"gap_ms", v.gapMs},
                       {"limit_s", seconds},
                       {"frames", v.frames},
                       {"armed_ms", v.armedMs},
                       {"action", "ending_process"}});
  const EndFn end = g_end.load(std::memory_order_acquire);
  if (end != nullptr) end("hang_watchdog");
  return true;
}

bool InstallVia(const char* dir, InstallFn install, EndFn end, bool startWatcher, bool frameSourceLive) noexcept {
  const Settings s = ReadSettings(dir);
  if (s.off) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "q15t_hangwd_install",
                        {{"result", "off"}, {"reason", "hangwd.off"}, {"frame_source", "render_config_submit_frame"}});
    return false;
  }
  if (!frameSourceLive) {
    // No frame heartbeat: the guard would never arm. Nothing is hooked.
    sentinel::LogFields(sentinel::LogLevel::kWarn, "q15t_hangwd_install",
                        {{"result", "refused"},
                         {"reason", "frame_source_not_installed"},
                         {"frame_source", "render_config_submit_frame"}});
    return false;
  }
  g_seconds.store(s.seconds, std::memory_order_relaxed);
  g_end.store(end, std::memory_order_release);
  EnterThunk::Arm(kEnterHook);
  LeaveThunk::Arm(kLeaveHook);
  const sentinel::GotStatus enter = install(0);
  const sentinel::GotStatus leave = install(1);
  bool ok = enter == sentinel::GotStatus::kOk && leave == sentinel::GotStatus::kOk;
  const char* reason = ok ? "none" : "vr_mode_hook_refused";
  if (ok && startWatcher) {
    pthread_t thread;
    if (pthread_create(&thread, nullptr, &WatcherThread, nullptr) == 0) {
      pthread_detach(thread);
    } else {
      ok = false;
      reason = "no_watcher_thread";
    }
  }
  if (ok) g_on.store(true, std::memory_order_release);
  // A refused guard leaves the VR-mode hooks that went in as pass-through state recorders: g_on stays false, so no frame
  // arms it and the watcher (never started) cannot end the process. The caller sets the frame observer only on true.
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "q15t_hangwd_install",
                      {{"result", ok ? "on" : "refused"},
                       {"reason", reason},
                       {"limit_s", s.seconds},
                       {"limit_source", s.source},
                       {"enter_status", sentinel::GotStatusName(enter)},
                       {"leave_status", sentinel::GotStatusName(leave)},
                       {"frame_source", "render_config_submit_frame"}});
  return ok;
}

bool Install(bool frameSourceLive) noexcept {
  return InstallVia(nullptr, &RealInstall, &EndThroughProcessEnd, true, frameSourceLive);
}

void ArmForTest() noexcept {
  EnterThunk::Arm(kEnterHook);
  LeaveThunk::Arm(kLeaveHook);
}

void ResetForTest() noexcept {
  g_on.store(false);
  g_lastNs.store(0);
  g_armNs.store(0);
  g_inVr.store(0);
  g_frames.store(0);
  g_seconds.store(kDefaultSeconds);
  g_end.store(nullptr);
}

}  // namespace nevr_quest::hang_watchdog
