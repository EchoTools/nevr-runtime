// #2109: a dedicated server's frame wait: raw pings answered as they arrive, the game's datagrams handed back in order,
// never an early return, and the overrun delta-time patch that depends on it. The decisions are in
// server_frame_wait.h (WaitUntil, HandBack, ChooseWaitMode, DeadlineUs) and the Env in server_frame_wait_env.h; this
// file is the game's side of them: two hooks, the game's functions handed to the Env, and the logging.

#include "runtime/patch/server_frame_wait_install.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/patching.h"
#include "runtime/hook/process_memory.h"
#include "runtime/lifecycle/crash_recovery.h"  // GameLoopThreadId
#include "runtime/patch/binary_bug_fixes.h"    // FrameWaitOwned
#include "runtime/patch/checked_patch.h"
#include "runtime/patch/server_frame_wait.h"
#include "runtime/patch/server_frame_wait_env.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace nevr_server_frame_wait {
namespace {

// The prologues the hooks and the call are written against (read from echovr.exe at each address).
constexpr uint8_t kDrainPrologue[8] = {0x40, 0x53, 0x55, 0x56, 0x41, 0x54, 0x41, 0x55};
constexpr uint8_t kRecvPrologue[8] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74};
constexpr uint8_t kSendPrologue[8] = {0x4c, 0x89, 0x4c, 0x24, 0x20, 0x55, 0x56, 0x41};

/// The port's socket is its second quadword (SendRecvDrain passes param_1[1] to the wrapper and sets it to -1 when it
/// closes the socket).
constexpr std::size_t kPortSocketOffset = 8;

/// One summary line per this many microseconds of the game loop.
constexpr uint64_t kSummaryWindowUs = 60ULL * 1000 * 1000;

using DrainFn = void(__fastcall*)(void* port, float budgetSeconds);

DrainFn s_origDrain = nullptr;
RecvFn s_origRecv = nullptr;
// CBroadcastPort_SendPacket, called, not hooked. No other thread sends on the port while the wait does: its callers
// are ReceiveFromPeer (under CBroadcaster::UpdateReceive) and SendFlush's two senders (under CBroadcaster::Update, and
// LogOut through 0x140f731d0), all on the game loop thread, and its four data references are unwind entries (.pdata
// 0x142186e1c and the chained unwind records at 0x141f95f8c, 0x141f95fa4, 0x141f95fb4), not function pointers. The
// wait runs on that same thread, so its send never overlaps the game's (lag-simulation queue included).
SendFn s_send = nullptr;

std::atomic<bool> g_active{false};  // a server whose CPrecisionSleep::Wait is the runtime's, with a clock
std::atomic<bool> g_pings{false};   // and the broadcaster hooks are in: the wait serves the socket

// Game loop thread only: the drain, the wrapper and the socket-serving wait all run there.
HandBack<kQueueSlots> g_handBack;

HANDLE g_timer = nullptr;  // the game loop thread's; a wait on another thread sleeps without it
int64_t g_qpcFrequency = 0;

// Waits off the game loop thread (or before it is known): never early, on the clock alone. Counted from any thread.
std::atomic<uint64_t> g_clockOnlyWaits{0};
std::atomic<bool> g_clockOnlyLogged{false};
std::atomic<bool> g_fallbackLogged{false};

struct Window {
  uint64_t startUs = 0;
  uint64_t waits = 0;
  uint64_t socketWaits = 0;
  uint64_t drains = 0;
  uint64_t wrapperCalls = 0;
  uint64_t pingsAnswered = 0;
  uint64_t sendFailures = 0;
  uint64_t queued = 0;
  uint64_t errorsQueued = 0;
  uint64_t pollFailures = 0;
  uint64_t earlyTimeouts = 0;
  uint64_t emptyReadable = 0;
  uint64_t queueFull = 0;
  uint64_t maxLateUs = 0;
  int lastPollError = 0;
};
Window g_window;
bool g_firstPingLogged = false;

bool OnGameLoopThread() {
  const unsigned long loop = GameLoopThreadId();
  return loop != 0 && loop == GetCurrentThreadId();
}

uint64_t ReadPortSocket(void* port) {
  return *reinterpret_cast<const volatile uint64_t*>(static_cast<const uint8_t*>(port) + kPortSocketOffset);
}

void Account(const WaitStats& st, bool socketWait, int pollError) {
  Window& w = g_window;
  ++w.waits;
  if (socketWait) ++w.socketWaits;
  w.pingsAnswered += st.pingsAnswered;
  w.sendFailures += st.sendFailures;
  w.queued += st.queued;
  w.errorsQueued += st.errorsQueued;
  w.pollFailures += st.pollFailures;
  w.earlyTimeouts += st.earlyTimeouts;
  w.emptyReadable += st.emptyReadable;
  if (st.queueFull) ++w.queueFull;
  if (st.lateUs > w.maxLateUs) w.maxLateUs = st.lateUs;
  if (pollError != 0) w.lastPollError = pollError;
}

void MaybeLogSummary(uint64_t nowUs) {
  Window& w = g_window;
  if (w.startUs == 0) {
    w.startUs = nowUs;
    return;
  }
  if (nowUs - w.startUs < kSummaryWindowUs) return;
  const uint64_t replayed = g_handBack.TakeReplayed();
  const uint64_t dropped = g_handBack.TakeDropped();
  const uint64_t clockOnly = g_clockOnlyWaits.exchange(0, std::memory_order_relaxed);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] server frame wait window_s=%llu waits=%llu socket_waits=%llu clock_only_waits=%llu drains=%llu "
      "wrapper_calls=%llu pings_answered=%llu ping_send_failures=%llu datagrams_queued=%llu "
      "datagrams_handed_back=%llu datagrams_dropped=%llu errors_handed_back=%llu poll_failures=%llu "
      "last_poll_error=%d early_timeouts=%llu empty_readable=%llu queue_full=%llu max_late_us=%llu (raw pings are "
      "answered during the wait, not on the next frame; drains=0 means the broadcaster has not run, as on a server "
      "with no service)",
      static_cast<unsigned long long>((nowUs - w.startUs) / 1000000ULL), static_cast<unsigned long long>(w.waits),
      static_cast<unsigned long long>(w.socketWaits), static_cast<unsigned long long>(clockOnly),
      static_cast<unsigned long long>(w.drains), static_cast<unsigned long long>(w.wrapperCalls),
      static_cast<unsigned long long>(w.pingsAnswered), static_cast<unsigned long long>(w.sendFailures),
      static_cast<unsigned long long>(w.queued), static_cast<unsigned long long>(replayed),
      static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(w.errorsQueued),
      static_cast<unsigned long long>(w.pollFailures), w.lastPollError,
      static_cast<unsigned long long>(w.earlyTimeouts), static_cast<unsigned long long>(w.emptyReadable),
      static_cast<unsigned long long>(w.queueFull), static_cast<unsigned long long>(w.maxLateUs));
  w = Window{};
  w.startUs = nowUs;
}

// --- the hooks ---------------------------------------------------------------------------------------------------

void __fastcall DrainHook(void* port, float budgetSeconds) {
  if (OnGameLoopThread()) {
    ++g_window.drains;
    g_handBack.OnDrain(port, ReadPortSocket);
  }
  s_origDrain(port, budgetSeconds);
}

uint64_t __fastcall RecvHook(uint64_t socket, uint8_t* address, uint8_t* buffer, int64_t* size, uint32_t* status) {
  if (OnGameLoopThread()) {
    ++g_window.wrapperCalls;
    uint64_t result = 0;
    if (g_handBack.OnReceive(socket, address, buffer, size, status, &result)) return result;
  }
  return s_origRecv(socket, address, buffer, size, status);
}

/// Whether the eight bytes at `site` are `want`; on a mismatch `actual` holds what is there.
bool PrologueMatches(const uint8_t* site, const uint8_t (&want)[8], uint8_t (&actual)[8]) {
  std::memcpy(actual, site, sizeof(actual));
  return std::memcmp(actual, want, sizeof(want)) == 0;
}

void Hex8(const uint8_t (&b)[8], char (&out)[17]) {
  for (int i = 0; i < 8; ++i) std::snprintf(out + 2 * i, 3, "%02x", b[i]);
}

/// Installs one hook after checking its prologue. Returns false (and says why at Warning) when it is not installed.
template <class Fn>
bool HookChecked(uint8_t* base, uintptr_t rva, const uint8_t (&prologue)[8], Fn* original, void* detour,
                 const char* name) {
  uint8_t* const site = base + rva;
  uint8_t actual[8];
  if (!PrologueMatches(site, prologue, actual)) {
    char want[17];
    char got[17];
    Hex8(prologue, want);
    Hex8(actual, got);
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] hook skipped name=%s va=0x%llX reason=prologue_mismatch expected=%s actual=%s", name,
        static_cast<unsigned long long>(0x140000000ULL + rva), want, got);
    return false;
  }
  *original = reinterpret_cast<Fn>(site);
  if (!PatchDetour(original, detour, name)) {
    *original = nullptr;
    return false;  // PatchDetour logged the stage and status (N126/N128)
  }
  return true;
}

nevr_checked_patch::WriteErrors g_writeErrors;  // boot thread only: Install runs once

bool ProcessWrite(void* dest, const void* src, size_t n) {
  DWORD error = ERROR_SUCCESS;
  return g_writeErrors.Note(ProcessMemcpy(dest, src, n, &error), error);
}

/// A wait off the game loop thread, or before the loop thread is known: never early, on the clock alone (no socket, no
/// shared timer). Counted, and said once.
void WaitClockOnly(int64_t microseconds) {
  SocketEnv env;
  env.qpcFrequency = g_qpcFrequency;
  const uint64_t deadlineUs = DeadlineUs(env.NowUs(), microseconds);
  DatagramQueue<1> unused;  // WaitUntil reserves from the queue only while it serves a socket
  WaitUntil(env, unused, deadlineUs, false);
  g_clockOnlyWaits.fetch_add(1, std::memory_order_relaxed);
  if (!g_clockOnlyLogged.exchange(true, std::memory_order_relaxed)) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] server frame wait: a wait on thread %lu (game loop thread %lu) waited on the clock alone "
        "wait_us=%lld: never early, no socket served (counted as clock_only_waits)",
        static_cast<unsigned long>(GetCurrentThreadId()), GameLoopThreadId(), static_cast<long long>(microseconds));
  }
}

}  // namespace

bool Wait(int64_t microseconds) {
  const WaitMode mode = ChooseWaitMode(g_active.load(std::memory_order_acquire), OnGameLoopThread());
  if (mode == WaitMode::kNotOurs) return false;
  if (mode == WaitMode::kClockOnly) {
    WaitClockOnly(microseconds);
    return true;
  }

  SocketEnv env;
  env.recvFn = s_origRecv;
  env.sendFn = s_send;
  env.timer = g_timer;
  env.qpcFrequency = g_qpcFrequency;
  const uint64_t deadlineUs = DeadlineUs(env.NowUs(), microseconds);

  // The socket the game's own drain read since the last wait, if its port still holds it (HandBack::BeginWait).
  const HandBack<kQueueSlots>::Target target =
      g_handBack.BeginWait(g_pings.load(std::memory_order_acquire), ReadPortSocket);
  env.port = target.port;
  env.socket = target.socket;

  const WaitStats st = WaitUntil(env, g_handBack.Queue(), deadlineUs, target.ready);
  if (st.pingsAnswered > 0 && !g_firstPingLogged) {
    g_firstPingLogged = true;
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] server frame wait answered its first raw ping (wait_us=%lld): pings are answered as they "
        "arrive, not on the next frame",
        static_cast<long long>(microseconds));
  }
  Account(st, target.ready, env.pollError);
  MaybeLogSummary(env.NowUs());
  return true;
}

void NoteFallbackWait(int64_t microseconds) {
  if (g_fallbackLogged.exchange(true, std::memory_order_relaxed)) return;
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.PATCH] server frame wait not active: a frame waited on the old timer (wait_us=%lld), which can end early; "
      "the overrun delta-time patch is not applied on this server, so an early end costs one step, not a 10 s dt",
      static_cast<long long>(microseconds));
}

void Install(bool isServer) {
  if (!isServer) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] server frame wait: not installed (a client): its frames are not paced by the clock's wait and it "
        "hosts no broadcaster for pings");
    return;
  }
  uint8_t* const base = reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress);
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  g_qpcFrequency = freq.QuadPart;

  g_timer = CreateWaitableTimerExW(nullptr, nullptr,
                                   CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);
  const char* timerKind = "high_resolution";
  DWORD timerError = 0;
  if (g_timer == nullptr) {
    g_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    timerKind = "standard";
    if (g_timer == nullptr) {
      timerError = GetLastError();
      timerKind = "none";
    }
  }

  // The wait: the runtime must own CPrecisionSleep::Wait and have a clock, or nothing here runs and the overrun patch
  // stays out.
  const bool waitOwned = nevr_binary_bug_fixes::FrameWaitOwned();
  g_active.store(waitOwned && g_qpcFrequency > 0, std::memory_order_release);

  // The pings: both hooks and the game's own send, or none of them.
  const char* pingState = "answered_in_wait";
  if (!waitOwned) {
    pingState = "on_next_frame (CPrecisionSleep::Wait is not the runtime's)";
  } else {
    uint8_t actual[8];
    const bool sendOk = PrologueMatches(base + nevr_patch_addresses::BROADCAST_PORT_SEND_PACKET, kSendPrologue, actual);
    if (!sendOk) {
      char want[17];
      char got[17];
      Hex8(kSendPrologue, want);
      Hex8(actual, got);
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] server frame wait: CBroadcastPort_SendPacket va=0x%llX reason=prologue_mismatch expected=%s "
          "actual=%s",
          static_cast<unsigned long long>(0x140000000ULL + nevr_patch_addresses::BROADCAST_PORT_SEND_PACKET), want,
          got);
      pingState = "on_next_frame (the game's send function does not match)";
    } else {
      s_send = reinterpret_cast<SendFn>(base + nevr_patch_addresses::BROADCAST_PORT_SEND_PACKET);
      const bool recvOk = HookChecked(base, nevr_patch_addresses::BROADCASTER_UDP_RECVFROM, kRecvPrologue, &s_origRecv,
                                      reinterpret_cast<void*>(&RecvHook), "udp_recvfrom_sockaddr_storage");
      const bool drainOk = recvOk && HookChecked(base, nevr_patch_addresses::BROADCASTER_SEND_RECV_DRAIN,
                                                 kDrainPrologue, &s_origDrain, reinterpret_cast<void*>(&DrainHook),
                                                 "CBroadcaster::SendRecvDrain");
      if (recvOk && drainOk) {
        g_pings.store(true, std::memory_order_release);
      } else {
        pingState = "on_next_frame (a broadcaster hook is not installed; see the warning above)";
      }
    }
  }

  // The overrun delta-time patch (server_frame_wait.h, MayPatchOverrunDelta).
  char deltaState[160];
  if (!MayPatchOverrunDelta(true, g_active.load(std::memory_order_acquire))) {
    std::snprintf(deltaState, sizeof(deltaState),
                  "not_applied (the frame wait is not active, so a wait could end early)");
  } else {
    uint8_t* const site = base + kOverrunDeltaRva;
    uint8_t before[2];
    std::memcpy(before, site, sizeof(before));
    const nevr_checked_patch::Result r = nevr_checked_patch::Apply(site, OverrunDeltaSite(), ProcessWrite);
    std::snprintf(deltaState, sizeof(deltaState), "%s va=0x%llX expected=7367 actual_before=%02x%02x%s",
                  nevr_checked_patch::ResultName(r), static_cast<unsigned long long>(0x140000000ULL + kOverrunDeltaRva),
                  before[0], before[1],
                  r == nevr_checked_patch::Result::WriteFailed ? " (the write failed)" : "");
    if (r == nevr_checked_patch::Result::WriteFailed) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] overrun delta-time patch write failed error=%u",
          static_cast<unsigned>(g_writeErrors.last));
    }
  }

  const bool degraded = !g_active.load() || !g_pings.load() || g_timer == nullptr;
  Log(degraded ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,
      "[NEVR.PATCH] server frame wait: active=%s (no wait on a server ends before its step, on any thread; margin %llu "
      "us) raw_pings=%s timer=%s%s overrun_delta=%s (an overrun frame's dt is the time it took, so game time keeps up "
      "with wall time)",
      g_active.load() ? "yes" : "no", static_cast<unsigned long long>(kNeverEarlyMarginUs), pingState, timerKind,
      timerError != 0 ? " (no waitable timer: the sub-millisecond tail sleeps 0 ms and checks the clock)" : "",
      deltaState);
}

}  // namespace nevr_server_frame_wait
