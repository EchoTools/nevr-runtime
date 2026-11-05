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
  /// (the caller's capacity), then the size, state and return value the wrapper reported. False when empty.
  bool PopInto(uint8_t* address, uint8_t* buffer, int64_t* size, uint32_t* status, uint64_t* result) {
    if (count_ == 0) return false;
    const Datagram& d = slots_[head_];
    std::memcpy(address, d.address, kAddressBytes);
    const int64_t capacity = *size;
    const int64_t n = d.size < capacity ? d.size : capacity;
    if (n > 0) std::memcpy(buffer, d.data, static_cast<std::size_t>(n));
    *size = d.size;
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
/// gives dt = step. So the patch goes in only on a server whose CPrecisionSleep::Wait is the runtime's (WaitUntil
/// never returns before its deadline); a client is never patched.
inline bool MayPatchOverrunDelta(bool isServer, bool frameWaitOwned) { return isServer && frameWaitOwned; }

}  // namespace nevr_server_frame_wait
