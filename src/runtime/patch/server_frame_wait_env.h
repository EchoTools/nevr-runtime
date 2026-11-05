#pragma once

// #2109: the Env WaitUntil runs on (server_frame_wait.h): the real clock (QueryPerformanceCounter), WSAPoll on the
// broadcaster socket, a waitable timer, and the game's receive wrapper and send, handed in as function pointers. The
// server hands it the game's own functions (server_frame_wait.cpp); the host-level test hands it stand-ins built on
// recvfrom and sendto over a real socket (src/runtime/tests/test_server_frame_wait_socket.cpp).

#include <winsock2.h>
#include <windows.h>

#include <cstdint>

#include "runtime/patch/server_frame_wait.h"

namespace nevr_server_frame_wait {

/// The game's recvfrom wrapper (0x1401f8d90): socket, 0x80-byte address, buffer, size (capacity in, bytes out), state.
using RecvFn = uint64_t(__fastcall*)(uint64_t socket, uint8_t* address, uint8_t* buffer, int64_t* size,
                                     uint32_t* status);
/// CBroadcastPort_SendPacket (0x140f89840): port, 0x80-byte address, bytes, size; returns the bytes sent.
using SendFn = uint64_t(__fastcall*)(void* port, const uint8_t* address, const uint8_t* data, uint64_t size);

/// Microseconds from a performance counter reading, rounded down, with no overflow (the whole and fractional parts are
/// never multiplied together): floor(counter * 1,000,000 / frequency), the value 0x1400ce8d0 computes. 0 when the
/// frequency is unknown.
inline uint64_t CounterMicroseconds(int64_t counter, int64_t frequency) {
  if (frequency <= 0 || counter < 0) return 0;
  return static_cast<uint64_t>((counter / frequency) * 1000000LL + ((counter % frequency) * 1000000LL) / frequency);
}

struct SocketEnv {
  RecvFn recvFn = nullptr;
  SendFn sendFn = nullptr;
  void* port = nullptr;
  uint64_t socket = 0;
  /// A waitable timer only this thread waits on, or null: the wait sleeps whole milliseconds (0 yields) and checks
  /// the clock again, which is never early either.
  HANDLE timer = nullptr;
  int64_t qpcFrequency = 0;
  int pollError = 0;

  uint64_t NowUs() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return CounterMicroseconds(counter.QuadPart, qpcFrequency);
  }

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
    d->result = recvFn(socket, d->address, d->data, &d->size, &d->status);
  }

  bool SendAck(const uint8_t* address, const uint8_t* ack) {
    return sendFn(port, address, ack, kRawPingBytes) == kRawPingBytes;
  }

  void SleepUs(uint64_t us) {
    if (timer != nullptr) {
      LARGE_INTEGER due;
      due.QuadPart = -static_cast<LONGLONG>(us * 10);  // relative, 100 ns units
      if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
        WaitForSingleObject(timer, static_cast<DWORD>(us / 1000) + 2);
        return;
      }
    }
    Sleep(static_cast<DWORD>(us / 1000));  // 0 yields; WaitUntil checks the clock again either way
  }
};

}  // namespace nevr_server_frame_wait
