#pragma once

// #2109: a dedicated server's frame wait. It answers UDP raw pings as they arrive, so a ping never waits for the next
// tick, and it never returns before its deadline, which is what makes the overrun delta-time patch safe.
//
// WHERE THE WAIT IS. CncaGame::RunMainLoop (echovr.exe 0x14050bd00) calls CR15Game::ConfigureClocks every iteration
// (0x14050be45). Its tail runs the main clock's CClock::UpdateAndPace (0x140127a59 -> 0x140517c40 -> 0x140517c5d).
// A server's clock holds mode bits 0x18 (fixed step, paced; #511), so UpdateAndPace computes the time since the frame
// started (0x1400ce8d0: now - [clock+0x20], unsigned). When that is under the step ([clock+0x90]), it calls
// CPrecisionSleep::Wait (0x1401ce0b0) with the rest (0x1400cf4b7), then moves the frame start on by one step
// (0x1400cf4c6). The runtime owns Wait (patch/binary_bug_fixes.cpp); on a server the hook calls WaitUntil here.
//
// WHY A PING WAITED. The broadcaster's UDP socket is read in one place: the game's recvfrom wrapper (0x1401f8d90, the
// only xref of the recvfrom import, at 0x1401f8e42). Its one caller is CBroadcaster::SendRecvDrain (0x140f8e310), which
// CBroadcaster::Update (0x140f8ea10) calls once a frame from CR15NetGame::Update. A 16-byte datagram led by the
// raw-ping request symbol is answered in SBroadcasterData::ReceiveFromPeer (0x140f85fd4-0x140f86038). The answer is the
// same 16 bytes with the acknowledge symbol in front, sent through CBroadcastPort_SendPacket (0x140f89840). So a ping
// that arrives during the wait sits in the socket until the next frame: up to a whole step.
//
// WHAT THE WAIT DOES. It polls the broadcaster socket for the rest of the step. When the socket is readable, it takes
// every datagram waiting through the game's own wrapper (the record the game itself would have read). It answers a raw
// ping at once, through the game's own send, and puts every other record in a queue. The game's drain gets the queue
// back in order through a hook on the wrapper before it reads the socket again (server_frame_wait.cpp). It never
// ends the wait early: an early return would move the frame start past now and run the game's frames faster than the
// step. The last part of a millisecond goes to the high-resolution timer, because WSAPoll counts whole milliseconds.
//
// What is pure: everything in this header. WaitUntil takes its clock, poll, receive, send and timer from an Env, so
// the GTest drives it on a fake clock (src/runtime/tests/test_server_frame_wait.cpp).

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "runtime/patch/checked_patch.h"

namespace nevr_server_frame_wait {

// --- the raw ping --------------------------------------------------------------------------------------------------

/// CMatSym::Hash("raw_ping_request") and CMatSym::Hash("raw_ping_acknowledge"), the symbols ReceiveFromPeer compares
/// with and writes (strings at 0x141cd0268 and 0x141cd0280). tools/tests/test_server_frame_wait_symbols.py recomputes
/// both with the repo's CMatSym implementation.
constexpr uint64_t kRawPingRequest = 0x997279de065a03b0ULL;
constexpr uint64_t kRawPingAcknowledge = 0x4f7ae556e0b77891ULL;
/// A raw ping is exactly 16 bytes: the symbol, then the sender's 8-byte token (ReceiveFromPeer 0x140f85fd4).
constexpr std::size_t kRawPingBytes = 16;

inline uint64_t LoadLe64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

inline void StoreLe64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

/// True for a raw ping request: 16 bytes whose first 8 are the request symbol.
inline bool IsRawPingRequest(const uint8_t* data, int64_t size) {
  return data != nullptr && size == static_cast<int64_t>(kRawPingBytes) && LoadLe64(data) == kRawPingRequest;
}

/// The answer ReceiveFromPeer sends: the acknowledge symbol, then the request's token.
inline void MakeRawPingAck(const uint8_t* request, uint8_t* ack) {
  StoreLe64(ack, kRawPingAcknowledge);
  std::memcpy(ack + 8, request + 8, 8);
}

// --- the game's receive record ------------------------------------------------------------------------------------

/// The wrapper (0x1401f8d90) zero-fills a 0x80-byte address and passes 0x80 as its length to recvfrom.
constexpr std::size_t kAddressBytes = 0x80;
/// The capacity SendRecvDrain hands the wrapper for each datagram (it stores 0x4f0 as the in/out size before the call).
constexpr std::size_t kReceiveCapacity = 0x4f0;

/// One call of the wrapper, as it reported it: its return value, the state it wrote, the size it wrote, the sender's
/// address and the bytes. The wrapper returns 0 except on a Winsock error it has no state for (state 13); it writes
/// state 0 for a datagram and for "nothing waiting", 1 for an empty datagram, and 4, 5 or 10 for the connection errors
/// it names. It writes size 0 on every error.
struct Datagram {
  uint64_t result = 0;
  uint32_t status = 0;
  int64_t size = 0;
  uint8_t address[kAddressBytes] = {};
  uint8_t data[kReceiveCapacity] = {};
};

/// Nothing was waiting (WSAEWOULDBLOCK maps to this): the wait leaves the socket alone until it is readable again.
inline bool IsNothing(const Datagram& d) { return d.result == 0 && d.status == 0 && d.size == 0; }

/// An error, or a state the game must see. It is handed to the game in order, and the wait stops reading the socket.
inline bool IsError(const Datagram& d) { return d.result != 0 || d.status != 0; }

/// The records the wait took and the game has not read yet, in arrival order. One thread uses it: the game loop thread
/// runs both the wait and the drain.
template <std::size_t N>
class DatagramQueue {
 public:
  /// The next free slot, to receive into directly; null when full. Not in the queue until Commit.
  Datagram* Reserve() { return count_ < N ? &slots_[(head_ + count_) % N] : nullptr; }
  void Commit() {
    if (count_ < N) ++count_;
  }
  bool Full() const { return count_ == N; }
  std::size_t Size() const { return count_; }

  /// Hands the oldest record to the game's drain the way the wrapper would have: the address, at most `*size` bytes
  /// (the caller's capacity), then the size it copied, and the state and return value the wrapper reported. False
  /// when empty. The wait reads with the drain's own capacity (kReceiveCapacity), so a record never exceeds it; were
  /// it to, the size reported is what was copied, never more than the caller's buffer holds.
  bool PopInto(uint8_t* address, uint8_t* buffer, int64_t* size, uint32_t* status, uint64_t* result) {
    if (count_ == 0) return false;
    const Datagram& d = slots_[head_];
    std::memcpy(address, d.address, kAddressBytes);
    const int64_t capacity = *size > 0 ? *size : 0;
    const int64_t n = d.size < capacity ? d.size : capacity;
    if (n > 0) std::memcpy(buffer, d.data, static_cast<std::size_t>(n));
    *size = n > 0 ? n : 0;
    *status = d.status;
    *result = d.result;
    head_ = (head_ + 1) % N;
    --count_;
    return true;
  }

  /// Drops everything (the socket they came from was replaced). Returns how many were dropped.
  std::size_t Clear() {
    const std::size_t n = count_;
    head_ = 0;
    count_ = 0;
    return n;
  }

 private:
  Datagram slots_[N];
  std::size_t head_ = 0;
  std::size_t count_ = 0;
};

/// The queue the server uses: a frame's worth of a full match's datagrams with room to spare. When it is full, the
/// rest stay in the socket for the game's drain, behind the queued ones, in order.
constexpr std::size_t kQueueSlots = 64;

/// A socket handle that names no socket (INVALID_SOCKET; SendRecvDrain stores it in the port when it closes the socket).
constexpr uint64_t kNoSocket = ~uint64_t{0};

/// The hooks' state, on the game loop thread (server_frame_wait.cpp is the thin game side of it): the port the last
/// drain served and the socket it read, and the records the wait took, with the port and socket they came from.
///
/// One port is drained a frame: CR15NetGame::Update (0x1401bf610) calls CBroadcaster::Update on [netgame+0x28c8] at
/// 0x1401bf715 or 0x1401bfb59 (two exclusive branches, the same broadcaster), and CBroadcaster::Update calls
/// SendRecvDrain once, on [data]+0x10 (0x140f8ea44-0x140f8ea58). Nothing here assumes it: a call of the wrapper for
/// another socket passes through and leaves the queue as it is, and the queue is dropped only when its own port no
/// longer holds the socket its records came from (the game closed or replaced that socket, so it would never have
/// read them).
///
/// The port's lifetime: [netgame+0x28c8] has one writer, ConfigureEngine (0x1401460dd, at boot), and the port is
/// SBroadcasterData+0x10, which CBroadcaster::Update reaches through the same pointer every frame; a port the hooks
/// recorded is read only on the game loop thread, between that frame's drain and the next.
///
/// `readSocket(port)` reads the port's socket field (the port's second quadword).
template <std::size_t N>
class HandBack {
 public:
  /// What the next wait may serve: the port and socket of the drain that ran since the last wait.
  struct Target {
    void* port = nullptr;
    uint64_t socket = 0;
    bool ready = false;
  };

  /// CBroadcaster::SendRecvDrain is about to drain `port`.
  template <class ReadSocket>
  void OnDrain(void* port, ReadSocket&& readSocket) {
    DropIfStale(readSocket);
    drainPort_ = port;
    drainSocket_ = 0;  // OnReceive fills it in when this drain reads
    drainFresh_ = true;
  }

  /// The game's wrapper is called for `socket`. True when the call was served from the queue: the oldest record the
  /// wait took from this socket is in the out parameters. False: the caller calls the wrapper itself (the queue is
  /// empty, or its records came from another socket; they stay queued for theirs).
  bool OnReceive(uint64_t socket, uint8_t* address, uint8_t* buffer, int64_t* size, uint32_t* status, uint64_t* result) {
    drainSocket_ = socket;
    if (queue_.Size() == 0 || socket != queueSocket_) return false;
    queue_.PopInto(address, buffer, size, status, result);
    ++replayed_;
    return true;
  }

  /// The wait is about to start: the port and socket it may serve. Ready only when a drain ran since the last wait, read
  /// a socket, and its port still holds that socket, and the queue holds nothing from another port or socket.
  template <class ReadSocket>
  Target BeginWait(bool pingsInstalled, ReadSocket&& readSocket) {
    DropIfStale(readSocket);
    Target t;
    const bool fresh = drainFresh_;
    drainFresh_ = false;
    if (!pingsInstalled || !fresh || drainPort_ == nullptr || drainSocket_ == 0 || drainSocket_ == kNoSocket) return t;
    const uint64_t portSocket = readSocket(drainPort_);
    if (portSocket != drainSocket_) return t;
    if (queue_.Size() != 0 && (queuePort_ != drainPort_ || queueSocket_ != portSocket)) return t;
    queuePort_ = drainPort_;
    queueSocket_ = portSocket;
    t.port = drainPort_;
    t.socket = portSocket;
    t.ready = true;
    return t;
  }

  DatagramQueue<N>& Queue() { return queue_; }
  std::size_t Queued() const { return queue_.Size(); }

  /// Records handed back, and records dropped, since the last call.
  uint64_t TakeReplayed() { return Take(replayed_); }
  uint64_t TakeDropped() { return Take(dropped_); }

 private:
  template <class ReadSocket>
  void DropIfStale(ReadSocket& readSocket) {
    if (queue_.Size() == 0 || queuePort_ == nullptr) return;
    if (readSocket(queuePort_) != queueSocket_) dropped_ += queue_.Clear();
  }

  static uint64_t Take(uint64_t& counter) {
    const uint64_t n = counter;
    counter = 0;
    return n;
  }

  DatagramQueue<N> queue_;
  void* queuePort_ = nullptr;
  uint64_t queueSocket_ = 0;
  void* drainPort_ = nullptr;
  uint64_t drainSocket_ = 0;
  bool drainFresh_ = false;
  uint64_t replayed_ = 0;
  uint64_t dropped_ = 0;
};

// --- the wait ------------------------------------------------------------------------------------------------------

enum class PollResult { kTimeout, kReadable, kFailed };

/// A poll that says "readable" and then yields nothing this many times in one wait stops the polling for that wait, so
/// a socket that keeps signalling with nothing to read cannot turn the wait into a busy loop.
constexpr uint32_t kMaxEmptyReadablePolls = 4;
/// A poll that times out more than a millisecond before its timeout this many times in one wait stops the polling for
/// that wait too: a poll that ignores its timeout would otherwise spin until the deadline.
constexpr uint32_t kMaxEarlyTimeouts = 4;

/// What one wait did. The server sums these into its periodic line.
struct WaitStats {
  uint32_t polls = 0;
  uint32_t pingsAnswered = 0;
  uint32_t sendFailures = 0;
  uint32_t queued = 0;
  uint32_t errorsQueued = 0;
  uint32_t pollFailures = 0;
  uint32_t emptyReadable = 0;
  uint32_t earlyTimeouts = 0;
  uint32_t timerWaits = 0;
  bool queueFull = false;
  uint64_t lateUs = 0;  ///< how far past the deadline it returned
};

/// Waits until `env.NowUs()` reaches `deadlineUs`, never returning before it. While `socketReady` and nothing has gone
/// wrong, it polls the socket for whole milliseconds and serves it as above; otherwise, and for the last part of a
/// millisecond, it waits on the timer. The Env provides:
///   uint64_t NowUs();                     monotonic microseconds
///   PollResult Poll(uint32_t timeoutMs);  waits until the socket is readable or the timeout passes
///   void Receive(Datagram* out);          one call of the game's wrapper
///   bool SendAck(const uint8_t* address, const uint8_t* ack16);  the game's own send to that address
///   void SleepUs(uint64_t us);            the high-resolution timer; it may return early, never matters
template <class Env, class Queue>
WaitStats WaitUntil(Env& env, Queue& queue, uint64_t deadlineUs, bool socketReady) {
  WaitStats st;
  bool pulling = socketReady;
  for (;;) {
    const uint64_t now = env.NowUs();
    if (now >= deadlineUs) {
      st.lateUs = now - deadlineUs;
      return st;
    }
    const uint64_t remaining = deadlineUs - now;
    if (!pulling || remaining < 1000) {
      env.SleepUs(remaining);
      ++st.timerWaits;
      continue;
    }
    ++st.polls;
    const uint32_t timeoutMs = static_cast<uint32_t>(remaining / 1000);
    const PollResult polled = env.Poll(timeoutMs);
    if (polled == PollResult::kTimeout) {
      if (env.NowUs() - now + 1000 < uint64_t{timeoutMs} * 1000 && ++st.earlyTimeouts >= kMaxEarlyTimeouts) {
        pulling = false;
      }
      continue;
    }
    if (polled == PollResult::kFailed) {
      ++st.pollFailures;
      pulling = false;
      continue;
    }
    bool tookAny = false;
    for (;;) {
      Datagram* d = queue.Reserve();
      if (d == nullptr) {
        st.queueFull = true;
        pulling = false;
        break;
      }
      env.Receive(d);
      if (IsNothing(*d)) break;
      tookAny = true;
      if (!IsError(*d) && IsRawPingRequest(d->data, d->size)) {
        uint8_t ack[kRawPingBytes];
        MakeRawPingAck(d->data, ack);
        if (env.SendAck(d->address, ack)) {
          ++st.pingsAnswered;
        } else {
          ++st.sendFailures;
        }
        continue;  // the slot was never committed: the next datagram reuses it
      }
      queue.Commit();
      ++st.queued;
      if (IsError(*d)) {
        ++st.errorsQueued;
        pulling = false;
        break;
      }
    }
    if (!tookAny && pulling) {
      ++st.emptyReadable;
      if (st.emptyReadable >= kMaxEmptyReadablePolls) pulling = false;
    }
  }
}

/// How late past the game's deadline the wait returns, at least. CClock::UpdateAndPace's elapsed (0x1400ce8d0) is
/// CTimer::GetTicks (QueryPerformanceCounter, 0x1400de770) * 1,000,000 / the cached QueryPerformanceFrequency
/// (0x1400de790), rounded down: the value the wait's clock computes from the same counter, so the two agree to the
/// microsecond. (Its other branch reads a frozen time when the flag at 0x142099038 is set; that flag has 18 reads and
/// no writer.) The margin covers what that reading cannot: a counter something else reads differently. It costs
/// nothing: the game moves the frame start on by one step after the wait (0x1400cf4c6), so a late return is not carried
/// into the next frame.
constexpr uint64_t kNeverEarlyMarginUs = 50;

/// The time a wait of `microseconds` asked for at `startUs` must not end before.
inline uint64_t DeadlineUs(uint64_t startUs, int64_t microseconds) {
  return startUs + static_cast<uint64_t>(microseconds > 0 ? microseconds : 0) + kNeverEarlyMarginUs;
}

/// What the runtime's Wait hook does with a server's wait.
enum class WaitMode {
  kNotOurs,      ///< The frame wait is not active: the hook's own wait runs, and the overrun patch is not in.
  kClockOnly,    ///< Off the game loop thread, or before it is known: a never-early wait on the clock, no socket.
  kServeSocket,  ///< The game loop thread: the socket is served for the rest of the step (HandBack decides if it can be).
};

/// Every wait on an active server is never early, whatever thread makes it, so the overrun patch's guarantee does not
/// depend on which thread calls CPrecisionSleep::Wait or on the loop thread being known yet.
inline WaitMode ChooseWaitMode(bool active, bool onLoopThread) {
  if (!active) return WaitMode::kNotOurs;
  return onLoopThread ? WaitMode::kServeSocket : WaitMode::kClockOnly;
}

// --- the overrun delta-time patch ------------------------------------------------------------------------------------

/// CClock::UpdateAndPace 0x1400cf46d: the `jae` taken when the frame took at least its step (0x1400cf46a
/// `cmp rdx, rcx`: elapsed against the step). The game's target, 0x1400cf4d6, gives a fixed-step clock dt = step and
/// moves the frame start to now, so game time falls behind wall time by every overrun. The patched target,
/// 0x1400cf4e9, is the variable-step arm of the same choice (`lea rcx,[rdi+8]; call 0x1400ce8d0`: the measured elapsed
/// time): an overrun frame gets dt = elapsed, and game time keeps up with wall time. The patch is C-Luddy's, from
/// EchoTools/EchoRelay commit 21e15b28 ("Change calculation of delta-time for engine calculations"), imported here in
/// 324857cc (GamePatches/patches.cpp). It was written for this build: the bytes there are 73 67.
constexpr uintptr_t kOverrunDeltaRva = 0xCF46D;
inline constexpr uint8_t kOverrunDeltaExpected[2] = {0x73, 0x67};
inline constexpr uint8_t kOverrunDeltaPatched[2] = {0x73, 0x7a};

inline nevr_checked_patch::Site OverrunDeltaSite() {
  return nevr_checked_patch::Site{kOverrunDeltaExpected, kOverrunDeltaPatched, sizeof(kOverrunDeltaExpected)};
}

/// The patch makes the measured elapsed time the frame's dt. That time is unsigned: a wait that returned before frame
/// start + step leaves the start ahead of now, the next elapsed wraps around, and the clock's maximum dt
/// ([clock+0x60] = 10,000,000 us, CClock::CClock 0x1400cd942) becomes the frame's dt. Without the patch the same wrap
/// gives dt = step. So the patch goes in only on a server whose frame wait is active: the runtime owns
/// CPrecisionSleep::Wait and has a clock, and then every wait through it, on any thread, is a WaitUntil that never
/// returns before DeadlineUs (ChooseWaitMode). A client is never patched.
inline bool MayPatchOverrunDelta(bool isServer, bool frameWaitActive) { return isServer && frameWaitActive; }

}  // namespace nevr_server_frame_wait
