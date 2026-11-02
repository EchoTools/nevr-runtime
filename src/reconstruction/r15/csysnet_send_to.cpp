// NRadEngine::CSysNet::SendTo
// Quest libr15.so VA 0x10997c0; ReVault binary 576.
// ReVault raw parent 100208379, reconstruction node 100600567; grade M: all 88 instructions and the 112-entry errno table checked; SAddress size is the 0x80 addrlen, ENetConnState and the return type are unpinned.
// @0x10997c0 — NRadEngine::CSysNet::SendTo
/* @confidence: M */

#include <cerrno>
#include <cstddef>
#include <cstdint>

// Declared locally: <sys/socket.h> is not available under mingw. Linux/bionic AArch64 (LP64) layouts.
struct sockaddr {
    std::uint16_t sa_family;  // sa_family_t: the address family (AF_* value)
    char sa_data[14];         // protocol-specific address bytes
};
static_assert(sizeof(sockaddr) == 16);
static_assert(offsetof(sockaddr, sa_family) == 0);
static_assert(offsetof(sockaddr, sa_data) == 2);
using socklen_t = std::uint32_t;  // the unsigned 32-bit length of a socket address
// ssize_t sendto(int, const void*, size_t, int, const sockaddr*, socklen_t): sends to dest_addr; -1 with errno on failure.
extern "C" std::int64_t sendto(int sockfd, const void* buf, std::uint64_t len, int flags,
                               const sockaddr* dest_addr, socklen_t addrlen);

extern "C" {
// Import "__gnu_strerror_r" at thunk 0x0fd43b0 (GNU strerror_r); its result is not read.
char* __gnu_strerror_r(int error, char* buffer, std::size_t length);
}

namespace NRadEngine {

// Written as a raw 32-bit word; ReVault has no ENetConnState type.
enum ENetConnState : std::int32_t {};
static_assert(sizeof(ENetConnState) == 4);

namespace NWriteLog {
// NRadEngine::NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...)
void WriteLog(std::int32_t level, std::uint64_t category, const char* format, ...);
}  // namespace NWriteLog

class CErrMsg {
public:
    // NRadEngine::CErrMsg::CreateAndAddError; the decompiled result is undefined8.
    static std::uint64_t CreateAndAddError(const char* file, std::int32_t line, std::uint64_t hash,
                                           const char* format, ...);
};

class CSysNet {
public:
    // 0x80 bytes of address storage: the address length the code passes to sendto.
    struct alignas(8) SAddress {
        std::byte storage[0x80];
    };
    static_assert(sizeof(SAddress) == 0x80);
    static_assert(offsetof(SAddress, storage) == 0);

    static std::uint64_t SendTo(std::uint64_t socket_fd, const SAddress& address, const void* data,
                                std::uint64_t& length, ENetConnState& state);
};

std::uint64_t CSysNet::SendTo(std::uint64_t socket_fd, const SAddress& address, const void* data,
                              std::uint64_t& length, ENetConnState& state) {
    const int sent = static_cast<int>(::sendto(
        static_cast<int>(socket_fd), data,
        static_cast<std::size_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(length))), 0,
        reinterpret_cast<const sockaddr*>(&address), 0x80));
    if (sent != -1) {
        length = static_cast<std::uint64_t>(static_cast<std::int64_t>(sent));
        state = static_cast<ENetConnState>(0);
        return 0;
    }
    length = 0;
    const int error = errno;
    switch (error) {
    case 4:
    case 11:
    case 115:
        state = static_cast<ENetConnState>(0);
        return 0;
    case 13:
    case 99:
        state = static_cast<ENetConnState>(6);
        return 0;
    case 101:
        state = static_cast<ENetConnState>(3);
        return 0;
    case 102:
        state = static_cast<ENetConnState>(4);
        return 0;
    case 103:
        NWriteLog::WriteLog(4, 0, "[SYSNET] 'Connection Aborted' unexpected for connectionless UDP");
        [[fallthrough]];
    default: {
        state = static_cast<ENetConnState>(13);
        char buffer[0x80] = {};
        __gnu_strerror_r(error, buffer, 0x80);
        return CErrMsg::CreateAndAddError(
            "d:\\projects\\rad\\dev\\src\\engine\\libs\\os\\network\\csysnet_win_xb1_linux_android.cpp",
            0x245, 0x65998e2379f54a6ULL, "Socket Error 0x%08X:\n\t'%s'", error, buffer);
    }
    case 104:
        state = static_cast<ENetConnState>(5);
        return 0;
    case 105:
        state = static_cast<ENetConnState>(2);
        return 0;
    case 110:
        state = static_cast<ENetConnState>(10);
        return 0;
    case 113:
        state = static_cast<ENetConnState>(9);
        return 0;
    }
}

}  // namespace NRadEngine
