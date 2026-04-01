#include "src/pnsrad/network/CSocket.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace NRadEngine {

// @0x18009ff00 — CSocket::Close
// @confidence: H
void CSocket_Close(SOCKET* sock_ptr) {
    auto* inner = *reinterpret_cast<int64_t**>(sock_ptr);
    if (inner != nullptr) {
        *reinterpret_cast<int64_t*>(sock_ptr) = 0;
        // vtable[2] on the inner socket object — close callback
        auto** vtable = *reinterpret_cast<void***>(inner);
        using CloseFn = void(*)();
        reinterpret_cast<CloseFn>(reinterpret_cast<uintptr_t*>(vtable)[2])();
    }
}

// @0x18009ff30 — CSocket::Connect
// @confidence: H
uint64_t CSocket_Connect(SOCKET* sock_ptr, void* addr) {
#ifdef _WIN32
    // Ordinal_1 = connect (not full WSAConnect — only 3 args in decompilation)
    int64_t result = connect(*sock_ptr, reinterpret_cast<sockaddr*>(addr), 0);
    if (result == -1) {
        DWORD err = WSAGetLastError();  // Ordinal_111
        // Check against retry bitmask:
        // Valid retry errors in range [10004, 10054]:
        // 10004(WSAEINTR), 10035(WSAEWOULDBLOCK), 10036(WSAEINPROGRESS),
        // 10037(WSAEALREADY), 10053(WSAECONNABORTED), 10054(WSAECONNRESET)
        if (err - 0x2714 > 0x32 ||
            ((0x4000180000001ULL >> ((uint64_t)(err - 0x2714) & 0x3F)) & 1) == 0) {
            return TranslateWSAError(err);
        }
    } else {
        *reinterpret_cast<int64_t*>(sock_ptr) = result;  // store connected socket handle
    }
#endif
    return 0;
}

// @0x18009ff90 — CSocket::Shutdown
// @confidence: H
void CSocket_Shutdown(SOCKET* sock_ptr) {
#ifdef _WIN32
    if (*sock_ptr != INVALID_SOCKET) {
        closesocket(*sock_ptr);
    }
#endif
}

// @0x18009ffa0 — CSocket::SendTo
// @confidence: H
uint64_t CSocket_SendTo(SOCKET* sock_ptr, void* buf) {
#ifdef _WIN32
    // Ordinal_4 = sendto
    int result = sendto(*sock_ptr, reinterpret_cast<const char*>(buf), 0x10, 0, nullptr, 0);
    if (result == SOCKET_ERROR) {
        DWORD err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {  // 10035 = 0x2733
            return 0;
        }
        uint8_t lock_buf[16];
        SendLock_Acquire(lock_buf);
        uint64_t translated = TranslateWSAError(err);
        SendLock_Release();
        return translated & 0xFFFFFFFF;
    }
#endif
    return 0;
}

// @0x1800a0000 — CSocket::CreateTCP
// @confidence: H
uint64_t CSocket_CreateTCP(SOCKET* sock_ptr) {
#ifdef _WIN32
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (s == INVALID_SOCKET) {
        DWORD err = WSAGetLastError();
        return TranslateWSAError(err);
    }
    *sock_ptr = s;
#endif
    return 0;
}

} // namespace NRadEngine
