// #2109: a dedicated server's frame wait: raw pings answered as they arrive, the game's datagrams handed back in order,
// never an early return, and the overrun delta-time patch that depends on it. The decisions are in
// server_frame_wait.h; this file is the game's side of them: two hooks, the Env WaitUntil runs on, and the logging.

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
#include "runtime/hook/hook_liveness.h"
#include "runtime/hook/patching.h"
#include "runtime/hook/process_memory.h"
#include "runtime/lifecycle/crash_recovery.h"  // GameLoopThreadId
#include "runtime/patch/binary_bug_fixes.h"    // FrameWaitOwned
#include "runtime/patch/checked_patch.h"
#include "runtime/patch/server_frame_wait.h"

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
using RecvFn = uint64_t(__fastcall*)(uint64_t socket, uint8_t* address, uint8_t* buffer, int64_t* size,
                                     uint32_t* status);
using SendFn = uint64_t(__fastcall*)(void* port, const uint8_t* address, const uint8_t* data, uint64_t size);

DrainFn s_origDrain = nullptr;
RecvFn s_origRecv = nullptr;
SendFn s_send = nullptr;  // CBroadcastPort_SendPacket: called, not hooked

std::atomic<bool> g_active{false};  // a server whose CPrecisionSleep::Wait is the runtime's
std::atomic<bool> g_pings{false};   // and the broadcaster hooks are in: the wait serves the socket

// Game loop thread only: the drain, the wrapper and the wait all run there (anything else passes through).
DatagramQueue<kQueueSlots> g_queue;
uint64_t g_queueSocket = 0;   // the socket the queued records came from
void* g_drainPort = nullptr;  // the port the last drain served
uint64_t g_drainSocket = 0;   // the socket that drain passed to the wrapper
bool g_drainFresh = false;    // a drain ran since the last wait

HANDLE g_timer = nullptr;
int64_t g_qpcFrequency = 0;

struct Window {
  uint64_t startUs = 0;
  uint64_t waits = 0;
  uint64_t socketWaits = 0;
  uint64_t pingsAnswered = 0;
  uint64_t sendFailures = 0;
  uint64_t queued = 0;
  uint64_t errorsQueued = 0;
  uint64_t replayed = 0;
  uint64_t dropped = 0;
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

/// Monotonic microseconds from QPC, overflow-safe (the whole and fractional parts are never multiplied together).
/// 0 when the frequency is unknown.
uint64_t QpcMicroseconds() {
  const int64_t pf = g_qpcFrequency;
  if (pf <= 0) return 0;
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  const int64_t pc = counter.QuadPart;
  return static_cast<uint64_t>((pc / pf) * 1000000LL + ((pc % pf) * 1000000LL) / pf);
}

struct GameEnv {
  void* port = nullptr;
  uint64_t socket = 0;
  int pollError = 0;

  uint64_t NowUs() { return QpcMicroseconds(); }

  PollResult Poll(uint32_t timeoutMs) {
    WSAPOLLFD pfd = {};
    pfd.fd = static_cast<SOCKET>(socket);
    pfd.events = POLLRDNORM;
    const int r = WSAPoll(&pfd, 1, static_cast<INT>(timeoutMs));
    if (r == SOCKET_ERROR) {
      pollError = WSAGetLastError();
      return PollResult::kFailed;
    }
    if (r == 0) return PollResult::kTimeout;
    if ((pfd.revents & POLLNVAL) != 0) {
      pollError = WSAENOTSOCK;
      return PollResult::kFailed;
    }
    return PollResult::kReadable;  // data, or POLLERR/POLLHUP: the wrapper reports which, the game handles it
  }

  void Receive(Datagram* d) {
    d->size = static_cast<int64_t>(kReceiveCapacity);
    d->status = 0;
    d->result = s_origRecv(socket, d->address, d->data, &d->size, &d->status);
  }

  bool SendAck(const uint8_t* address, const uint8_t* ack) {
    return s_send(port, address, ack, kRawPingBytes) == kRawPingBytes;
  }

  void SleepUs(uint64_t us) {
    if (g_timer != nullptr) {
      LARGE_INTEGER due;
      due.QuadPart = -static_cast<LONGLONG>(us * 10);  // relative, 100 ns units
      if (SetWaitableTimerEx(g_timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
        WaitForSingleObject(g_timer, static_cast<DWORD>(us / 1000) + 2);
        return;
      }
    }
    Sleep(static_cast<DWORD>(us / 1000));  // 0 yields; WaitUntil checks the clock again either way
  }
};

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
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] server frame wait window_s=%llu waits=%llu socket_waits=%llu pings_answered=%llu "
      "ping_send_failures=%llu datagrams_queued=%llu datagrams_handed_back=%llu datagrams_dropped=%llu "
      "errors_handed_back=%llu poll_failures=%llu last_poll_error=%d early_timeouts=%llu empty_readable=%llu "
      "queue_full=%llu max_late_us=%llu (raw pings are answered during the wait, not on the next frame)",
      static_cast<unsigned long long>((nowUs - w.startUs) / 1000000ULL), static_cast<unsigned long long>(w.waits),
      static_cast<unsigned long long>(w.socketWaits), static_cast<unsigned long long>(w.pingsAnswered),
      static_cast<unsigned long long>(w.sendFailures), static_cast<unsigned long long>(w.queued),
      static_cast<unsigned long long>(w.replayed), static_cast<unsigned long long>(w.dropped),
      static_cast<unsigned long long>(w.errorsQueued), static_cast<unsigned long long>(w.pollFailures),
      w.lastPollError, static_cast<unsigned long long>(w.earlyTimeouts),
      static_cast<unsigned long long>(w.emptyReadable), static_cast<unsigned long long>(w.queueFull),
      static_cast<unsigned long long>(w.maxLateUs));
  w = Window{};
  w.startUs = nowUs;
}

// --- the hooks ---------------------------------------------------------------------------------------------------

void __fastcall DrainHook(void* port, float budgetSeconds) {
  nevr_hook_liveness::Mark(nevr_hook_liveness::kBroadcasterSendRecvDrain);
  if (OnGameLoopThread()) {
    g_drainPort = port;
    g_drainSocket = 0;  // the wrapper hook fills it in when this drain reads
    g_drainFresh = true;
  }
  s_origDrain(port, budgetSeconds);
}

uint64_t __fastcall RecvHook(uint64_t socket, uint8_t* address, uint8_t* buffer, int64_t* size, uint32_t* status) {
  nevr_hook_liveness::Mark(nevr_hook_liveness::kBroadcasterUdpRecvFrom);
  if (OnGameLoopThread()) {
    g_drainSocket = socket;
    if (g_queue.Size() != 0) {
      if (socket == g_queueSocket) {
        uint64_t result = 0;
        g_queue.PopInto(address, buffer, size, status, &result);
        ++g_window.replayed;
        return result;
      }
      // The game replaced its socket after the wait read from the old one: those records belong to a socket it
      // closed, as they would have if it had read them itself before closing.
      g_window.dropped += g_queue.Clear();
    }
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

}  // namespace

bool Wait(int64_t microseconds) {
  if (!g_active.load(std::memory_order_acquire) || !OnGameLoopThread()) return false;
  GameEnv env;
  const uint64_t startUs = env.NowUs();
  if (startUs == 0) return false;
  const uint64_t deadlineUs = startUs + static_cast<uint64_t>(microseconds > 0 ? microseconds : 0);

  // The socket the game's own drain read since the last wait, if its port still says it is the port's socket. A
  // port or socket from an earlier frame is never used: the broadcaster may have closed or replaced it since.
  bool socketReady = false;
  if (g_pings.load(std::memory_order_acquire) && g_drainFresh && g_drainPort != nullptr && g_drainSocket != 0 &&
      g_drainSocket != static_cast<uint64_t>(INVALID_SOCKET)) {
    const uint64_t portSocket =
        *reinterpret_cast<const volatile uint64_t*>(static_cast<const uint8_t*>(g_drainPort) + kPortSocketOffset);
    if (portSocket == g_drainSocket && (g_queue.Size() == 0 || g_queueSocket == portSocket)) {
      env.port = g_drainPort;
      env.socket = portSocket;
      g_queueSocket = portSocket;
      socketReady = true;
    }
  }
  g_drainFresh = false;

  const WaitStats st = WaitUntil(env, g_queue, deadlineUs, socketReady);
  if (st.pingsAnswered > 0 && !g_firstPingLogged) {
    g_firstPingLogged = true;
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] server frame wait answered its first raw ping (wait_us=%lld): pings are answered as they "
        "arrive, not on the next frame",
        static_cast<long long>(microseconds));
  }
  Account(st, socketReady, env.pollError);
  MaybeLogSummary(env.NowUs());
  return true;
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

  // The wait: the runtime must own CPrecisionSleep::Wait, or nothing here runs and the overrun patch stays out.
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
                  "not_applied (CPrecisionSleep::Wait is not the runtime's, so a wait could end early)");
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
      "[NEVR.PATCH] server frame wait: active=%s (a frame never ends before its step) raw_pings=%s timer=%s%s "
      "overrun_delta=%s (an overrun frame's dt is the time it took, so game time keeps up with wall time)",
      g_active.load() ? "yes" : "no", pingState, timerKind,
      timerError != 0 ? " (no waitable timer: the sub-millisecond tail sleeps 0 ms and checks the clock)" : "",
      deltaState);
}

}  // namespace nevr_server_frame_wait
