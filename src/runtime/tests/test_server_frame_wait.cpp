// #2109: a dedicated server's frame wait answers UDP raw pings as they arrive, hands the game every other datagram it
// took back in order, never returns before its deadline, and gates the overrun delta-time patch on that guarantee.
// Everything here drives runtime/patch/server_frame_wait.h on a fake clock and a fake socket; nothing needs the game.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "runtime/patch/server_frame_wait.h"

namespace {

namespace sfw = nevr_server_frame_wait;
using sfw::Datagram;
using sfw::PollResult;

// A datagram as the game's receive wrapper reports it.
Datagram MakeDatagram(const std::vector<uint8_t>& bytes, uint8_t addressTag = 0x11) {
  Datagram d;
  d.size = static_cast<int64_t>(bytes.size());
  std::memcpy(d.data, bytes.data(), bytes.size());
  d.address[0] = 2;  // AF_INET
  d.address[4] = addressTag;
  return d;
}

std::vector<uint8_t> RawPing(uint64_t token) {
  std::vector<uint8_t> b(16);
  for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(sfw::kRawPingRequest >> (8 * i));
  for (int i = 0; i < 8; ++i) b[8 + i] = static_cast<uint8_t>(token >> (8 * i));
  return b;
}

// A fake clock and socket. Datagrams arrive at fixed times; Poll waits until the next arrival or its timeout, whichever
// is first, and Receive takes one arrived datagram (or reports nothing waiting). Every send is recorded with its time.
struct FakeEnv {
  struct Arrival {
    uint64_t atUs;
    Datagram d;
  };
  uint64_t now = 0;
  std::deque<Arrival> pending;   // not yet arrived
  std::deque<Datagram> arrived;  // in the socket
  struct Sent {
    uint64_t atUs;
    uint8_t address0;
    uint8_t address4;
    std::vector<uint8_t> bytes;
  };
  std::vector<Sent> sent;
  // Knobs for the failure cases.
  bool pollFails = false;
  bool pollReturnsAtOnce = false;  // a poll that ignores its timeout (returns Timeout immediately)
  bool sleepReturnsAtOnce = false; // a timer that fails (returns immediately)
  bool sendFails = false;
  int spuriousReadable = 0;        // polls that say readable with nothing waiting
  int polls = 0;
  int sleeps = 0;

  void Deliver() {
    while (!pending.empty() && pending.front().atUs <= now) {
      arrived.push_back(pending.front().d);
      pending.pop_front();
    }
  }
  uint64_t NowUs() { return now; }
  PollResult Poll(uint32_t timeoutMs) {
    ++polls;
    if (pollFails) return PollResult::kFailed;
    Deliver();
    if (spuriousReadable > 0) {
      --spuriousReadable;
      return PollResult::kReadable;
    }
    if (!arrived.empty()) return PollResult::kReadable;
    if (pollReturnsAtOnce) {
      now += 1;  // a poll that ignores its timeout: no wait at all (one tick so the test ends)
      return PollResult::kTimeout;
    }
    const uint64_t until = now + uint64_t{timeoutMs} * 1000;
    if (!pending.empty() && pending.front().atUs <= until) {
      now = pending.front().atUs;
      Deliver();
      return PollResult::kReadable;
    }
    now = until;
    return PollResult::kTimeout;
  }
  void Receive(Datagram* out) {
    Deliver();
    if (arrived.empty()) {
      *out = Datagram{};  // nothing waiting: result 0, status 0, size 0
      return;
    }
    *out = arrived.front();
    arrived.pop_front();
  }
  bool SendAck(const uint8_t* address, const uint8_t* ack) {
    if (sendFails) return false;
    sent.push_back({now, address[0], address[4], std::vector<uint8_t>(ack, ack + sfw::kRawPingBytes)});
    return true;
  }
  void SleepUs(uint64_t us) {
    ++sleeps;
    if (sleepReturnsAtOnce) {
      now += 1;  // a failed timer: no wait at all (one tick so the test ends)
      return;
    }
    now += us;
  }
};

using Queue = sfw::DatagramQueue<8>;

// --- the raw ping ------------------------------------------------------------------------------------------------

TEST(RawPing, TheSymbolsAreTheGamesHashesOfTheirNames) {
  // CMatSym::Hash("raw_ping_request") and CMatSym::Hash("raw_ping_acknowledge"): SBroadcasterData::ReceiveFromPeer
  // hashes the strings at echovr.exe 0x141cd0268 and 0x141cd0280 (0x140f85fda-0x140f8601c). tools/tests recomputes them.
  EXPECT_EQ(sfw::kRawPingRequest, 0x997279de065a03b0ULL);
  EXPECT_EQ(sfw::kRawPingAcknowledge, 0x4f7ae556e0b77891ULL);
  EXPECT_EQ(sfw::kRawPingBytes, 16u);
}

TEST(RawPing, OnlyASixteenByteDatagramWithTheRequestSymbolIsAPing) {
  const auto ping = RawPing(0x0123456789abcdefULL);
  EXPECT_TRUE(sfw::IsRawPingRequest(ping.data(), 16));
  EXPECT_FALSE(sfw::IsRawPingRequest(ping.data(), 15));
  EXPECT_FALSE(sfw::IsRawPingRequest(ping.data(), 17));
  EXPECT_FALSE(sfw::IsRawPingRequest(nullptr, 16));
  auto ack = ping;
  ack[0] ^= 1;
  EXPECT_FALSE(sfw::IsRawPingRequest(ack.data(), 16));
}

TEST(RawPing, TheAnswerIsTheAckSymbolAndTheSameToken) {
  // What ReceiveFromPeer sends back (0x140f8601c-0x140f86033): the first 8 bytes become the ack symbol, the rest stay.
  const auto ping = RawPing(0x0123456789abcdefULL);
  uint8_t ack[16];
  sfw::MakeRawPingAck(ping.data(), ack);
  const uint8_t want[16] = {0x91, 0x78, 0xb7, 0xe0, 0x56, 0xe5, 0x7a, 0x4f,
                            0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01};
  EXPECT_EQ(std::memcmp(ack, want, 16), 0);
}

// --- the wait ----------------------------------------------------------------------------------------------------

TEST(ServerFrameWait, APingIsAnsweredWhenItArrivesNotAtTheDeadline) {
  FakeEnv env;
  Queue q;
  env.pending.push_back({2000, MakeDatagram(RawPing(7))});
  const auto st = sfw::WaitUntil(env, q, 8333, true);
  ASSERT_EQ(env.sent.size(), 1u);
  EXPECT_EQ(env.sent[0].atUs, 2000u) << "answered 2 ms in, as it arrived";
  EXPECT_EQ(env.sent[0].address4, 0x11) << "answered to the address it came from";
  EXPECT_EQ(st.pingsAnswered, 1u);
  EXPECT_GE(env.now, 8333u) << "and the frame still waits out its step";
  EXPECT_EQ(q.Size(), 0u) << "a ping is answered, not handed to the game";
}

TEST(ServerFrameWait, AnIdleStepWaitAnswersEveryPingAsItArrives) {
  // 6 a second: a 166,666 us wait. Without the answer in the wait, each ping would sit until the next frame.
  FakeEnv env;
  Queue q;
  const uint64_t at[] = {1000, 50000, 50001, 120000, 166000};
  for (uint64_t t : at) env.pending.push_back({t, MakeDatagram(RawPing(t))});
  sfw::WaitUntil(env, q, 166666, true);
  ASSERT_EQ(env.sent.size(), 5u);
  for (size_t i = 0; i < 5; ++i) EXPECT_EQ(env.sent[i].atUs, at[i]) << i;
}

TEST(ServerFrameWait, AGameDatagramIsHandedBackInOrderAndAPingBehindItIsStillAnswered) {
  FakeEnv env;
  Queue q;
  env.pending.push_back({1000, MakeDatagram({0xaa, 0xbb, 0xcc}, 0x21)});
  env.pending.push_back({1000, MakeDatagram(RawPing(9), 0x22)});
  env.pending.push_back({3000, MakeDatagram({0xdd}, 0x23)});
  const auto st = sfw::WaitUntil(env, q, 8333, true);
  ASSERT_EQ(env.sent.size(), 1u);
  EXPECT_EQ(env.sent[0].atUs, 1000u) << "the ping behind a game datagram does not wait for the frame";
  EXPECT_EQ(st.queued, 2u);
  ASSERT_EQ(q.Size(), 2u);

  // The game's drain gets them back in arrival order, bytes, address and status as the wrapper reported them.
  uint8_t address[sfw::kAddressBytes];
  uint8_t buffer[sfw::kReceiveCapacity];
  int64_t size = sfw::kReceiveCapacity;
  uint32_t status = 99;
  uint64_t result = 0;
  ASSERT_TRUE(q.PopInto(address, buffer, &size, &status, &result));
  EXPECT_EQ(size, 3);
  EXPECT_EQ(buffer[0], 0xaa);
  EXPECT_EQ(buffer[2], 0xcc);
  EXPECT_EQ(address[4], 0x21);
  EXPECT_EQ(status, 0u);
  EXPECT_EQ(result, 0u);
  size = sfw::kReceiveCapacity;
  ASSERT_TRUE(q.PopInto(address, buffer, &size, &status, &result));
  EXPECT_EQ(size, 1);
  EXPECT_EQ(buffer[0], 0xdd);
  EXPECT_EQ(address[4], 0x23);
  EXPECT_FALSE(q.PopInto(address, buffer, &size, &status, &result)) << "then the game reads its socket again";
}

TEST(ServerFrameWait, ItNeverReturnsBeforeItsDeadline) {
  // A poll that ignores its timeout and a timer that does not wait: the loop keeps going until the clock says so.
  for (int knobs = 0; knobs < 4; ++knobs) {
    FakeEnv env;
    env.pollReturnsAtOnce = (knobs & 1) != 0;
    env.sleepReturnsAtOnce = (knobs & 2) != 0;
    Queue q;
    const auto st = sfw::WaitUntil(env, q, 8333, true);
    EXPECT_GE(env.now, 8333u) << "knobs=" << knobs;
    if (env.pollReturnsAtOnce) {
      EXPECT_EQ(st.earlyTimeouts, sfw::kMaxEarlyTimeouts) << "a poll that ignores its timeout is given up on";
      EXPECT_EQ(env.polls, static_cast<int>(sfw::kMaxEarlyTimeouts)) << "no busy loop of polls, knobs=" << knobs;
    }
    FakeEnv noSocket;
    noSocket.sleepReturnsAtOnce = env.sleepReturnsAtOnce;
    sfw::WaitUntil(noSocket, q, 8333, false);
    EXPECT_GE(noSocket.now, 8333u) << "no socket, knobs=" << knobs;
  }
}

TEST(ServerFrameWait, ADeadlineAlreadyPastReturnsAtOnce) {
  FakeEnv env;
  env.now = 9000;
  Queue q;
  const auto st = sfw::WaitUntil(env, q, 8333, true);
  EXPECT_EQ(env.polls, 0);
  EXPECT_EQ(env.sleeps, 0);
  EXPECT_EQ(st.lateUs, 667u);
}

TEST(ServerFrameWait, TheLastMillisecondIsWaitedOnTheTimerNotPolled) {
  // WSAPoll counts whole milliseconds: the sub-millisecond tail goes to the timer so the frame does not run late.
  FakeEnv env;
  Queue q;
  sfw::WaitUntil(env, q, 8333, true);
  EXPECT_EQ(env.now, 8333u);
  EXPECT_GE(env.sleeps, 1);
}

TEST(ServerFrameWait, AnErrorIsHandedToTheGameAndEndsThePullingForThisWait) {
  FakeEnv env;
  Queue q;
  Datagram err;
  err.status = 13;  // the wrapper's "Winsock error" state
  err.result = 0x55;
  env.pending.push_back({1000, err});
  env.pending.push_back({2000, MakeDatagram(RawPing(1))});
  const auto st = sfw::WaitUntil(env, q, 8333, true);
  EXPECT_EQ(q.Size(), 1u) << "the error goes to the game, which decides what a broken socket means";
  EXPECT_EQ(st.errorsQueued, 1u);
  EXPECT_TRUE(env.sent.empty()) << "after an error the socket is left to the game for the rest of the wait";
  EXPECT_GE(env.now, 8333u);
  EXPECT_LE(env.polls, 2) << "no busy loop on a broken socket";
}

TEST(ServerFrameWait, APollThatFailsFallsBackToTheTimer) {
  FakeEnv env;
  env.pollFails = true;
  Queue q;
  const auto st = sfw::WaitUntil(env, q, 8333, true);
  EXPECT_EQ(st.pollFailures, 1u);
  EXPECT_EQ(env.polls, 1);
  EXPECT_GE(env.now, 8333u);
}

TEST(ServerFrameWait, ReadableWithNothingToReadDoesNotSpin) {
  FakeEnv env;
  env.spuriousReadable = 1000;
  Queue q;
  const auto st = sfw::WaitUntil(env, q, 8333, true);
  EXPECT_LE(env.polls, static_cast<int>(sfw::kMaxEmptyReadablePolls) + 1);
  EXPECT_GE(st.emptyReadable, sfw::kMaxEmptyReadablePolls);
  EXPECT_GE(env.now, 8333u);
}

TEST(ServerFrameWait, AFullQueueLeavesTheRestInTheSocketForTheGame) {
  FakeEnv env;
  sfw::DatagramQueue<2> small;
  for (int i = 0; i < 5; ++i) env.pending.push_back({1000, MakeDatagram({static_cast<uint8_t>(i)})});
  const auto st = sfw::WaitUntil(env, small, 8333, true);
  EXPECT_EQ(small.Size(), 2u);
  EXPECT_TRUE(st.queueFull);
  EXPECT_EQ(env.arrived.size(), 3u) << "the other three stay in the socket, after the two queued, in order";
  EXPECT_EQ(env.polls, 1) << "a full queue ends the polling for this wait at once";
  EXPECT_GE(env.now, 8333u);
}

TEST(ServerFrameWait, AnAckThatCannotBeSentIsCountedNotQueued) {
  FakeEnv env;
  env.sendFails = true;
  Queue q;
  env.pending.push_back({1000, MakeDatagram(RawPing(3))});
  const auto st = sfw::WaitUntil(env, q, 8333, true);
  EXPECT_EQ(st.sendFailures, 1u);
  EXPECT_EQ(st.pingsAnswered, 0u);
  EXPECT_EQ(q.Size(), 0u) << "the game's own send would fail the same way";
}

// --- the replay ----------------------------------------------------------------------------------------------------

TEST(DatagramQueue, PopIntoCopiesNoMoreThanTheCallersCapacity) {
  Queue q;
  Datagram* d = q.Reserve();
  ASSERT_NE(d, nullptr);
  d->size = 10;
  for (int i = 0; i < 10; ++i) d->data[i] = static_cast<uint8_t>(i);
  q.Commit();
  uint8_t address[sfw::kAddressBytes];
  uint8_t buffer[8];
  std::memset(buffer, 0xee, sizeof(buffer));
  int64_t size = 4;
  uint32_t status = 0;
  uint64_t result = 0;
  ASSERT_TRUE(q.PopInto(address, buffer, &size, &status, &result));
  EXPECT_EQ(buffer[3], 3);
  for (int i = 4; i < 8; ++i) EXPECT_EQ(buffer[i], 0xee) << "byte " << i << " is past the caller's capacity";
  EXPECT_EQ(size, 4) << "the size reported is what was copied: the drain never reads past what it was given";
}

TEST(DatagramQueue, PopIntoCopiesAWholeDatagramThatFits) {
  Queue q;
  Datagram* d = q.Reserve();
  ASSERT_NE(d, nullptr);
  d->size = 300;
  for (int i = 0; i < 300; ++i) d->data[i] = static_cast<uint8_t>(i * 7);
  q.Commit();
  uint8_t address[sfw::kAddressBytes];
  uint8_t buffer[sfw::kReceiveCapacity] = {};
  int64_t size = sfw::kReceiveCapacity;
  uint32_t status = 0;
  uint64_t result = 0;
  ASSERT_TRUE(q.PopInto(address, buffer, &size, &status, &result));
  EXPECT_EQ(size, 300);
  for (int i = 0; i < 300; ++i) ASSERT_EQ(buffer[i], static_cast<uint8_t>(i * 7)) << i;
}

TEST(DatagramQueue, AReservationNotCommittedIsNotQueued) {
  Queue q;
  ASSERT_NE(q.Reserve(), nullptr);
  EXPECT_EQ(q.Size(), 0u);
  ASSERT_NE(q.Reserve(), nullptr);
  q.Commit();
  EXPECT_EQ(q.Size(), 1u);
}

TEST(DatagramQueue, ClearDropsEverything) {
  Queue q;
  q.Reserve();
  q.Commit();
  q.Reserve();
  q.Commit();
  EXPECT_EQ(q.Clear(), 2u);
  EXPECT_EQ(q.Size(), 0u);
}

// --- the hand-back state (HandBack) ------------------------------------------------------------------------------

// A broadcaster port as the hooks see it: its socket is the second quadword.
struct FakePort {
  uint64_t vtable = 0;
  uint64_t socket = 0;
};

uint64_t ReadSocket(void* port) { return static_cast<FakePort*>(port)->socket; }

using HandBack = sfw::HandBack<8>;

// Queues one game datagram the way the wait does: received into a reserved slot, then committed.
void QueueDatagram(HandBack& hb, uint8_t first) {
  sfw::Datagram* d = hb.Queue().Reserve();
  ASSERT_NE(d, nullptr);
  *d = MakeDatagram({first});
  hb.Queue().Commit();
}

struct Drained {
  bool served = false;
  int64_t size = 0;
  uint8_t first = 0;
};

Drained Receive(HandBack& hb, uint64_t socket) {
  uint8_t address[sfw::kAddressBytes];
  uint8_t buffer[sfw::kReceiveCapacity] = {};
  Drained r;
  r.size = sfw::kReceiveCapacity;
  uint32_t status = 0;
  uint64_t result = 0;
  r.served = hb.OnReceive(socket, address, buffer, &r.size, &status, &result);
  r.first = buffer[0];
  return r;
}

TEST(HandBack, AFrameDrainsWhatTheWaitBeforeItTook) {
  // Frame N's drain reads socket 7; the wait that ends frame N serves socket 7 and queues two datagrams; frame N+1's
  // drain gets them back in order, then reads the socket itself.
  FakePort port{0, 7};
  HandBack hb;
  hb.OnDrain(&port, ReadSocket);
  EXPECT_FALSE(Receive(hb, 7).served) << "an empty queue: the wrapper reads the socket";
  const auto target = hb.BeginWait(true, ReadSocket);
  ASSERT_TRUE(target.ready);
  EXPECT_EQ(target.port, &port);
  EXPECT_EQ(target.socket, 7u);
  QueueDatagram(hb, 0xa1);
  QueueDatagram(hb, 0xa2);
  hb.OnDrain(&port, ReadSocket);
  const auto first = Receive(hb, 7);
  const auto second = Receive(hb, 7);
  EXPECT_TRUE(first.served);
  EXPECT_EQ(first.first, 0xa1);
  EXPECT_TRUE(second.served);
  EXPECT_EQ(second.first, 0xa2);
  EXPECT_FALSE(Receive(hb, 7).served) << "then the game reads its socket again";
  EXPECT_EQ(hb.TakeReplayed(), 2u);
  EXPECT_EQ(hb.TakeDropped(), 0u);
}

TEST(HandBack, AReadForAnotherSocketPassesThroughAndLeavesTheQueue) {
  // Two ports drained in one frame (the game drains one, revault 0x140f8ea58; this guards the shape anyway): port B's
  // read must not take or drop what the wait queued from port A.
  FakePort a{0, 7};
  FakePort b{0, 9};
  HandBack hb;
  hb.OnDrain(&a, ReadSocket);
  Receive(hb, 7);
  ASSERT_TRUE(hb.BeginWait(true, ReadSocket).ready);
  QueueDatagram(hb, 0xa1);
  hb.OnDrain(&b, ReadSocket);
  EXPECT_FALSE(Receive(hb, 9).served) << "port B's wrapper call reads B's socket";
  EXPECT_EQ(hb.Queued(), 1u) << "and A's record is still queued";
  hb.OnDrain(&a, ReadSocket);
  const auto r = Receive(hb, 7);
  EXPECT_TRUE(r.served);
  EXPECT_EQ(r.first, 0xa1);
  EXPECT_EQ(hb.TakeDropped(), 0u);
}

TEST(HandBack, TheQueueIsDroppedOnlyWhenItsOwnPortNoLongerHoldsItsSocket) {
  FakePort a{0, 7};
  HandBack hb;
  hb.OnDrain(&a, ReadSocket);
  Receive(hb, 7);
  ASSERT_TRUE(hb.BeginWait(true, ReadSocket).ready);
  QueueDatagram(hb, 0xa1);
  QueueDatagram(hb, 0xa2);
  a.socket = sfw::kNoSocket;  // SendRecvDrain closed the socket: the game would never have read them
  hb.OnDrain(&a, ReadSocket);
  EXPECT_EQ(hb.Queued(), 0u);
  EXPECT_EQ(hb.TakeDropped(), 2u);
  EXPECT_FALSE(Receive(hb, sfw::kNoSocket).served);
}

TEST(HandBack, AReplacedSocketIsNotServedWithTheOldOnesRecords) {
  FakePort a{0, 7};
  HandBack hb;
  hb.OnDrain(&a, ReadSocket);
  Receive(hb, 7);
  ASSERT_TRUE(hb.BeginWait(true, ReadSocket).ready);
  QueueDatagram(hb, 0xa1);
  a.socket = 11;  // the game opened a new socket on the same port
  hb.OnDrain(&a, ReadSocket);
  EXPECT_FALSE(Receive(hb, 11).served) << "socket 11 never had socket 7's datagram";
  EXPECT_EQ(hb.TakeDropped(), 1u);
}

TEST(HandBack, AWaitServesOnlyAFreshDrainWhosePortStillHoldsTheSocketItRead) {
  FakePort a{0, 7};
  HandBack hb;
  EXPECT_FALSE(hb.BeginWait(true, ReadSocket).ready) << "no drain yet";
  hb.OnDrain(&a, ReadSocket);
  EXPECT_FALSE(hb.BeginWait(true, ReadSocket).ready) << "the drain read nothing: no socket to serve";
  hb.OnDrain(&a, ReadSocket);
  Receive(hb, 7);
  EXPECT_FALSE(hb.BeginWait(false, ReadSocket).ready) << "the broadcaster hooks are not installed";
  hb.OnDrain(&a, ReadSocket);
  Receive(hb, 7);
  ASSERT_TRUE(hb.BeginWait(true, ReadSocket).ready);
  EXPECT_FALSE(hb.BeginWait(true, ReadSocket).ready) << "a drain from an earlier frame is never used again";
  hb.OnDrain(&a, ReadSocket);
  Receive(hb, 7);
  a.socket = 11;
  EXPECT_FALSE(hb.BeginWait(true, ReadSocket).ready) << "the port no longer holds the socket the drain read";
  hb.OnDrain(&a, ReadSocket);
  Receive(hb, sfw::kNoSocket);
  EXPECT_FALSE(hb.BeginWait(true, ReadSocket).ready) << "a closed socket is never served";
}

TEST(HandBack, AWaitDoesNotServeAnotherPortWhileThisOnesRecordsAreQueued) {
  FakePort a{0, 7};
  FakePort b{0, 9};
  HandBack hb;
  hb.OnDrain(&a, ReadSocket);
  Receive(hb, 7);
  ASSERT_TRUE(hb.BeginWait(true, ReadSocket).ready);
  QueueDatagram(hb, 0xa1);
  hb.OnDrain(&b, ReadSocket);
  Receive(hb, 9);
  EXPECT_FALSE(hb.BeginWait(true, ReadSocket).ready) << "one queue, one port: B waits until A's records are read";
  EXPECT_EQ(hb.Queued(), 1u);
}

TEST(HandBack, TheWaitAndTheHandBackTogetherKeepArrivalOrderAcrossFrames) {
  // The full path on the fake socket: frame N's drain, the wait (a game datagram, a ping, another game datagram), frame
  // N+1's drain through OnReceive with the wrapper behind it, and a datagram that arrived after the wait.
  FakePort port{0, 7};
  HandBack hb;
  FakeEnv env;
  env.pending.push_back({1000, MakeDatagram({0x01}, 0x21)});
  env.pending.push_back({1500, MakeDatagram(RawPing(5), 0x22)});
  env.pending.push_back({2000, MakeDatagram({0x02}, 0x23)});
  env.pending.push_back({9000, MakeDatagram({0x03}, 0x24)});  // after the wait's deadline
  hb.OnDrain(&port, ReadSocket);
  Receive(hb, 7);
  const auto target = hb.BeginWait(true, ReadSocket);
  ASSERT_TRUE(target.ready);
  sfw::WaitUntil(env, hb.Queue(), 8333, target.ready);
  ASSERT_EQ(env.sent.size(), 1u);
  EXPECT_EQ(env.sent[0].atUs, 1500u);
  env.now = 9500;  // the next frame
  hb.OnDrain(&port, ReadSocket);
  std::vector<uint8_t> order;
  for (int i = 0; i < 4; ++i) {
    uint8_t address[sfw::kAddressBytes];
    uint8_t buffer[sfw::kReceiveCapacity] = {};
    int64_t size = sfw::kReceiveCapacity;
    uint32_t status = 0;
    uint64_t result = 0;
    if (!hb.OnReceive(7, address, buffer, &size, &status, &result)) {
      sfw::Datagram d;
      env.Receive(&d);  // the wrapper reads the socket itself
      if (sfw::IsNothing(d)) break;
      order.push_back(d.data[0]);
      continue;
    }
    order.push_back(buffer[0]);
  }
  EXPECT_EQ(order, (std::vector<uint8_t>{0x01, 0x02, 0x03}));
}

// --- every server wait is never early (ChooseWaitMode, DeadlineUs) -----------------------------------------------

TEST(WaitMode, AnActiveServerWaitIsNeverEarlyOnAnyThread) {
  EXPECT_EQ(sfw::ChooseWaitMode(true, true), sfw::WaitMode::kServeSocket);
  EXPECT_EQ(sfw::ChooseWaitMode(true, false), sfw::WaitMode::kClockOnly)
      << "off the loop thread, or before it is known: a never-early wait on the clock, not the old wait";
  EXPECT_EQ(sfw::ChooseWaitMode(false, true), sfw::WaitMode::kNotOurs);
  EXPECT_EQ(sfw::ChooseWaitMode(false, false), sfw::WaitMode::kNotOurs);
}

TEST(WaitMode, TheOverrunPatchFollowsTheSameFlagAsTheWait) {
  // MayPatchOverrunDelta and ChooseWaitMode read one flag: the patch is in exactly when no server wait can end early.
  for (bool active : {false, true}) {
    const bool neverEarlyOnEveryThread = sfw::ChooseWaitMode(active, true) != sfw::WaitMode::kNotOurs &&
                                         sfw::ChooseWaitMode(active, false) != sfw::WaitMode::kNotOurs;
    EXPECT_EQ(sfw::MayPatchOverrunDelta(true, active), neverEarlyOnEveryThread) << "active=" << active;
  }
}

TEST(WaitDeadline, TheDeadlineIsTheAskPlusTheMargin) {
  EXPECT_GT(sfw::kNeverEarlyMarginUs, 0u);
  EXPECT_LE(sfw::kNeverEarlyMarginUs, 1000u) << "a margin, not a slower frame";
  EXPECT_EQ(sfw::DeadlineUs(1000, 8333), 1000u + 8333u + sfw::kNeverEarlyMarginUs);
  EXPECT_EQ(sfw::DeadlineUs(1000, 0), 1000u + sfw::kNeverEarlyMarginUs);
  EXPECT_EQ(sfw::DeadlineUs(1000, -5), 1000u + sfw::kNeverEarlyMarginUs) << "a negative ask is no wait, not a wrap";
}

TEST(WaitDeadline, AClockOnlyWaitEndsNoEarlierThanTheAskPlusTheMargin) {
  // The clock-only path is WaitUntil with no socket: a timer that returns at once still loops until the deadline.
  FakeEnv env;
  env.now = 500;
  env.sleepReturnsAtOnce = true;
  sfw::DatagramQueue<1> unused;
  sfw::WaitUntil(env, unused, sfw::DeadlineUs(env.NowUs(), 8333), false);
  EXPECT_GE(env.now, 500u + 8333u + sfw::kNeverEarlyMarginUs);
  EXPECT_EQ(env.polls, 0) << "no socket is polled off the loop thread";
}

// --- the overrun delta-time patch ------------------------------------------------------------------------------

TEST(OverrunDelta, TheSiteIsTheJaeInUpdateAndPaceAndOnlyItsTargetMoves) {
  // echovr.exe 0x1400cf46d in CClock::UpdateAndPace: `jae 0x1400cf4d6` (73 67) -> `jae 0x1400cf4e9` (73 7a).
  const auto site = sfw::OverrunDeltaSite();
  ASSERT_EQ(site.size, 2u);
  EXPECT_EQ(site.expected[0], 0x73);
  EXPECT_EQ(site.patched[0], 0x73) << "the same jae: only the displacement changes";
  const uint64_t next = 0x1400cf46dULL + 2;
  EXPECT_EQ(next + site.expected[1], 0x1400cf4d6ULL) << "the fixed-step arm: dt = step";
  EXPECT_EQ(next + site.patched[1], 0x1400cf4e9ULL) << "the measured arm: dt = elapsed";
  EXPECT_EQ(sfw::kOverrunDeltaRva, 0xCF46Du);
}

TEST(OverrunDelta, ThePatchGoesInOnlyOnAServerWhoseFrameWaitCannotReturnEarly) {
  // With the patch, a wait that returns before frame start + step makes the next elapsed wrap around, and the clock's
  // 10 s maximum dt (CClock::CClock 0x1400cd942) becomes the frame's dt. Only the runtime's frame wait rules that out.
  EXPECT_TRUE(sfw::MayPatchOverrunDelta(true, true));
  EXPECT_FALSE(sfw::MayPatchOverrunDelta(true, false));
  EXPECT_FALSE(sfw::MayPatchOverrunDelta(false, true));
  EXPECT_FALSE(sfw::MayPatchOverrunDelta(false, false));
}

TEST(OverrunDelta, TheCheckedPatchWritesOnlyOverTheExpectedBytes) {
  uint8_t code[2] = {0x73, 0x67};
  auto write = [](void* dest, const void* src, size_t n) {
    std::memcpy(dest, src, n);
    return true;
  };
  EXPECT_EQ(nevr_checked_patch::Apply(code, sfw::OverrunDeltaSite(), write), nevr_checked_patch::Result::Applied);
  EXPECT_EQ(code[1], 0x7a);
  EXPECT_EQ(nevr_checked_patch::Apply(code, sfw::OverrunDeltaSite(), write),
            nevr_checked_patch::Result::AlreadyApplied);
  uint8_t other[2] = {0x7e, 0x67};  // a jle: some other build
  EXPECT_EQ(nevr_checked_patch::Apply(other, sfw::OverrunDeltaSite(), write), nevr_checked_patch::Result::Mismatch);
  EXPECT_EQ(other[0], 0x7e);
}

}  // namespace
