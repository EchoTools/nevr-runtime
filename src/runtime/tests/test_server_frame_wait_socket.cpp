// #2109: the frame wait's Env (patch/server_frame_wait_env.h) over a real UDP socket on the loopback: WSAPoll, the
// performance counter, the waitable timer, recvfrom and sendto, with the hand-back state (HandBack) the hooks use.
// The game's receive wrapper and send are replaced by stand-ins with their contract (StandInRecv, StandInSend); the
// Env, WaitUntil and HandBack are the code the server runs. What this does not cover: the hooks' install on the game's
// functions and a run under a match's traffic (owed as a live run on #2109).

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "runtime/patch/server_frame_wait.h"
#include "runtime/patch/server_frame_wait_env.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

namespace sfw = nevr_server_frame_wait;

// A broadcaster port as the hooks see it: the socket is its second quadword.
struct Port {
  uint64_t vtable = 0;
  uint64_t socket = 0;
};

uint64_t ReadPortSocket(void* port) { return static_cast<Port*>(port)->socket; }

// The game's wrapper's contract (0x1401f8d90): return 0; state 0 with the size for a datagram; state 0 and size 0 when
// nothing waits (WSAEWOULDBLOCK); state 1 for an empty datagram; an error state (13 here) and size 0 otherwise.
uint64_t __fastcall StandInRecv(uint64_t socket, uint8_t* address, uint8_t* buffer, int64_t* size, uint32_t* status) {
  std::memset(address, 0, sfw::kAddressBytes);
  int len = static_cast<int>(sfw::kAddressBytes);
  const int n = recvfrom(static_cast<SOCKET>(socket), reinterpret_cast<char*>(buffer), static_cast<int>(*size), 0,
                         reinterpret_cast<sockaddr*>(address), &len);
  if (n > 0) {
    *size = n;
    *status = 0;
    return 0;
  }
  *size = 0;
  if (n == 0) {
    *status = 1;
    return 0;
  }
  *status = WSAGetLastError() == WSAEWOULDBLOCK ? 0 : 13;
  return 0;
}

// CBroadcastPort_SendPacket's contract (0x140f89840): the bytes sent, 0 when nothing was sent.
uint64_t __fastcall StandInSend(void* port, const uint8_t* address, const uint8_t* data, uint64_t size) {
  const int n = sendto(static_cast<SOCKET>(static_cast<Port*>(port)->socket), reinterpret_cast<const char*>(data),
                       static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(address), sizeof(sockaddr_in));
  return n > 0 ? static_cast<uint64_t>(n) : 0;
}

uint64_t NowUs() {
  LARGE_INTEGER c;
  LARGE_INTEGER f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return sfw::CounterMicroseconds(c.QuadPart, f.QuadPart);
}

SOCKET BoundLoopbackSocket(bool nonBlocking) {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return s;
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    closesocket(s);
    return INVALID_SOCKET;
  }
  if (nonBlocking) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
  }
  return s;
}

sockaddr_in AddressOf(SOCKET s) {
  sockaddr_in a = {};
  int len = sizeof(a);
  getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
  return a;
}

std::vector<uint8_t> RawPing(uint64_t token) {
  std::vector<uint8_t> b(16);
  for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(sfw::kRawPingRequest >> (8 * i));
  for (int i = 0; i < 8; ++i) b[8 + i] = static_cast<uint8_t>(token >> (8 * i));
  return b;
}

class FrameWaitSocket : public ::testing::Test {
 protected:
  void SetUp() override {
    WSADATA wsa;
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsa), 0);
    server_ = BoundLoopbackSocket(true);  // the game's broadcaster socket is non-blocking
    client_ = BoundLoopbackSocket(false);
    ASSERT_NE(server_, INVALID_SOCKET);
    ASSERT_NE(client_, INVALID_SOCKET);
    DWORD timeoutMs = 2000;
    setsockopt(client_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    port_.socket = static_cast<uint64_t>(server_);
    timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                    CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (timer_ == nullptr) timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    env_.recvFn = StandInRecv;
    env_.sendFn = StandInSend;
    env_.timer = timer_;
    env_.qpcFrequency = f.QuadPart;
  }

  void TearDown() override {
    if (timer_ != nullptr) CloseHandle(timer_);
    if (client_ != INVALID_SOCKET) closesocket(client_);
    if (server_ != INVALID_SOCKET) closesocket(server_);
    WSACleanup();
  }

  // Frame N's drain, as the hooks see it: the drain hook, then one wrapper call that finds nothing waiting.
  void Drain() {
    hb_.OnDrain(&port_, ReadPortSocket);
    uint8_t address[sfw::kAddressBytes];
    uint8_t buffer[sfw::kReceiveCapacity];
    int64_t size = sfw::kReceiveCapacity;
    uint32_t status = 0;
    uint64_t result = 0;
    if (!hb_.OnReceive(port_.socket, address, buffer, &size, &status, &result)) {
      StandInRecv(port_.socket, address, buffer, &size, &status);
    }
  }

  void SendFromClient(const std::vector<uint8_t>& bytes) {
    const sockaddr_in to = AddressOf(server_);
    sendto(client_, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
  }

  SOCKET server_ = INVALID_SOCKET;
  SOCKET client_ = INVALID_SOCKET;
  HANDLE timer_ = nullptr;
  Port port_;
  sfw::SocketEnv env_;
  sfw::HandBack<8> hb_;
};

TEST_F(FrameWaitSocket, APingIsAnsweredDuringTheWaitAndTheGamesDatagramIsHandedBack) {
  Drain();
  const auto target = hb_.BeginWait(true, ReadPortSocket);
  ASSERT_TRUE(target.ready);
  env_.port = target.port;
  env_.socket = target.socket;

  const uint64_t token = 0x0123456789abcdefULL;
  std::atomic<uint64_t> pingSentUs{0};
  std::atomic<uint64_t> ackAtUs{0};
  std::vector<uint8_t> ack(16, 0);
  std::thread peer([&] {
    Sleep(20);
    SendFromClient({0xab, 0xcd});  // a game datagram first
    pingSentUs = NowUs();
    SendFromClient(RawPing(token));
    char buf[64];
    const int n = recv(client_, buf, sizeof(buf), 0);
    if (n == 16) {
      ackAtUs = NowUs();
      std::memcpy(ack.data(), buf, 16);
    }
  });

  const uint64_t startUs = env_.NowUs();
  const uint64_t deadlineUs = sfw::DeadlineUs(startUs, 200000);
  const sfw::WaitStats st = sfw::WaitUntil(env_, hb_.Queue(), deadlineUs, target.ready);
  const uint64_t endUs = env_.NowUs();
  peer.join();

  EXPECT_GE(endUs, deadlineUs) << "never early on the real clock";
  EXPECT_EQ(st.pingsAnswered, 1u);
  EXPECT_EQ(st.queued, 1u);
  ASSERT_NE(ackAtUs.load(), 0u) << "the peer got no 16-byte answer";
  uint8_t want[16];
  const auto ping = RawPing(token);
  sfw::MakeRawPingAck(ping.data(), want);
  EXPECT_EQ(std::memcmp(ack.data(), want, 16), 0) << "the ack symbol and the request's token";
  EXPECT_LT(ackAtUs.load(), endUs) << "answered while the wait ran, not after it";
  EXPECT_LT(ackAtUs.load() - pingSentUs.load(), 100000u) << "answered as it arrived, not at the 200 ms deadline";

  // The next frame's drain gets the game datagram back, from the client's address, then reads the socket itself.
  hb_.OnDrain(&port_, ReadPortSocket);
  uint8_t address[sfw::kAddressBytes];
  uint8_t buffer[sfw::kReceiveCapacity] = {};
  int64_t size = sfw::kReceiveCapacity;
  uint32_t status = 99;
  uint64_t result = 99;
  ASSERT_TRUE(hb_.OnReceive(port_.socket, address, buffer, &size, &status, &result));
  EXPECT_EQ(size, 2);
  EXPECT_EQ(buffer[0], 0xab);
  EXPECT_EQ(buffer[1], 0xcd);
  EXPECT_EQ(status, 0u);
  EXPECT_EQ(result, 0u);
  sockaddr_in from;
  std::memcpy(&from, address, sizeof(from));
  EXPECT_EQ(from.sin_port, AddressOf(client_).sin_port) << "the address the wrapper reported";
  size = sfw::kReceiveCapacity;
  EXPECT_FALSE(hb_.OnReceive(port_.socket, address, buffer, &size, &status, &result));
  size = sfw::kReceiveCapacity;
  StandInRecv(port_.socket, address, buffer, &size, &status);
  EXPECT_EQ(size, 0) << "nothing else was waiting";
  EXPECT_EQ(hb_.TakeReplayed(), 1u);
}

TEST_F(FrameWaitSocket, WaitsWithNothingArrivingEndNoEarlierThanAsked) {
  Drain();
  const auto target = hb_.BeginWait(true, ReadPortSocket);
  ASSERT_TRUE(target.ready);
  env_.port = target.port;
  env_.socket = target.socket;
  // Socket-serving waits of every length the game asks for (a 120 Hz step down to the sub-millisecond tail), then the
  // clock-only wait with no timer (a thread other than the game loop's).
  const int64_t asks[] = {8333, 4100, 1999, 1000, 999, 500, 1};
  for (int64_t ask : asks) {
    const uint64_t deadlineUs = sfw::DeadlineUs(env_.NowUs(), ask);
    sfw::WaitUntil(env_, hb_.Queue(), deadlineUs, true);
    EXPECT_GE(env_.NowUs(), deadlineUs) << "ask=" << ask;
  }
  sfw::SocketEnv clockOnly;
  clockOnly.qpcFrequency = env_.qpcFrequency;
  sfw::DatagramQueue<1> unused;
  for (int64_t ask : asks) {
    const uint64_t deadlineUs = sfw::DeadlineUs(clockOnly.NowUs(), ask);
    sfw::WaitUntil(clockOnly, unused, deadlineUs, false);
    EXPECT_GE(clockOnly.NowUs(), deadlineUs) << "clock only, ask=" << ask;
  }
  EXPECT_EQ(hb_.Queued(), 0u);
}

TEST_F(FrameWaitSocket, AClosedSocketEndsThePullingAndTheWaitStillRunsItsLength) {
  Drain();
  const auto target = hb_.BeginWait(true, ReadPortSocket);
  ASSERT_TRUE(target.ready);
  env_.port = target.port;
  env_.socket = target.socket;
  closesocket(server_);  // the game closed it; the port still says it is the socket
  server_ = INVALID_SOCKET;
  const uint64_t deadlineUs = sfw::DeadlineUs(env_.NowUs(), 20000);
  const sfw::WaitStats st = sfw::WaitUntil(env_, hb_.Queue(), deadlineUs, true);
  EXPECT_GE(env_.NowUs(), deadlineUs);
  EXPECT_LE(st.polls, 2u) << "no busy loop on a dead socket";
  EXPECT_EQ(st.pingsAnswered, 0u);
}

TEST(CounterMicroseconds, IsTheGamesRoundedDownConversionWithoutItsOverflow) {
  // 0x1400ce8d0: ticks * 1,000,000 / frequency in unsigned 64 bits, rounded down. The same value for every counter whose
  // product fits, and no wrap where it does not.
  const int64_t f = 10000000;  // QueryPerformanceFrequency on Windows 10 and Wine: 10 MHz
  const int64_t counters[] = {0, 1, 9, 10, 12345678, 864000000000LL, 18446744073709LL};
  for (int64_t c : counters) {
    const unsigned __int128 game = static_cast<unsigned __int128>(static_cast<uint64_t>(c)) * 1000000u / f;
    EXPECT_EQ(sfw::CounterMicroseconds(c, f), static_cast<uint64_t>(game)) << c;
  }
  const int64_t big = 30000000000000LL;  // past the 2^64 / 1e6 ticks where the game's product wraps (~21 days at 10 MHz)
  EXPECT_EQ(sfw::CounterMicroseconds(big, f), static_cast<uint64_t>(big / 10));
  EXPECT_EQ(sfw::CounterMicroseconds(5, 0), 0u) << "no frequency, no clock";
}

}  // namespace
