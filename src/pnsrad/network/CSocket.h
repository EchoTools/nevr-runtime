#ifndef PNSRAD_NETWORK_CSOCKET_H
#define PNSRAD_NETWORK_CSOCKET_H

/* @module: pnsrad.dll */
/* @purpose: Low-level socket wrappers around Winsock2 */

#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#else
// Stub types for non-Windows compilation
using SOCKET = int64_t;
#define INVALID_SOCKET (-1)
#endif

namespace NRadEngine {

// @0x18009ff00 — CSocket::Close
// Closes a socket handle. Nulls the pointer and calls vtable[2] (close callback).
// @confidence: H
void CSocket_Close(SOCKET* sock_ptr);

// @0x18009ff30 — CSocket::Connect
// Connects a socket (Ordinal_1 = WSAConnect).
// On failure, checks WSA error code against a known-retry bitmask.
// Returns 0 on success, error code on failure.
// @confidence: H
uint64_t CSocket_Connect(SOCKET* sock_ptr, void* addr);

// @0x18009ff90 — CSocket::Shutdown
// Calls closesocket (Ordinal_3) if handle is valid (not -1).
// @confidence: H
void CSocket_Shutdown(SOCKET* sock_ptr);

// @0x18009ffa0 — CSocket::SendTo
// Sends data via sendto (Ordinal_4).
// Returns 0 on success. On failure with error 10035 (WSAEWOULDBLOCK), returns 0.
// Otherwise returns translated error code.
// @confidence: H
uint64_t CSocket_SendTo(SOCKET* sock_ptr, void* buf);

// @0x1800a0000 — CSocket::CreateTCP
// Creates a TCP socket via WSASocketW(AF_INET=2, SOCK_STREAM=1, IPPROTO_TCP=6).
// Returns 0 on success, error code on failure.
// @confidence: H
uint64_t CSocket_CreateTCP(SOCKET* sock_ptr);

// Error translation helper
// @0x1800a0050
extern uint64_t TranslateWSAError(uint32_t wsa_error);

// Lock helpers for thread-safe send operations
// @0x180095c20
extern void SendLock_Acquire(void* lock_buf);
// @0x180095cb0
extern void SendLock_Release();

} // namespace NRadEngine

#endif // PNSRAD_NETWORK_CSOCKET_H
