// Quest libr15.so VA 0x138c024; ReVault binary libr15.so.
// ReVault raw parent 100224055, reconstruction node 100600626; grade M.
#include <cstddef>
#include <cstdint>
#include <cstring>

// @0x138c024 — NRadEngine::NRadGame::CR15NetGame::BeginMultiplayerWithLobbyAddr
/* @confidence: M */

namespace NRadEngine {

class CNSUser {
public:
    std::uint64_t LocalID() const;
};

namespace CSysNet {

// CSysNet::SAddress, returned through the x8 sret pointer by CSysNet::LocalHost
// (0xfe30c0, the `mov x8, sp` before the call at 0x138c084). A trivially
// copyable return of 16 bytes or less comes back in x0/x1, so the struct is
// declared larger than that. The frame of BeginMultiplayerWithLobbyAddr keeps
// the buffer below its saved x22 at sp + 0x80, which bounds the size here. The
// true size is not established: LocalHost tail-calls Lookup (0xfd80c0).
struct SAddress {
    alignas(8) std::byte storage[0x80];
    std::uint32_t Ipv4() const;  // 0x1094448: reads the u32 at offset 4
};

std::uint16_t HToNS(std::uint16_t value);
std::uint16_t NToHS(std::uint16_t value);
SAddress LocalHost(std::uint16_t port);

}  // namespace CSysNet

namespace NRadGame {

struct SR15NetMatchSettings {
    alignas(8) std::byte raw[0x50];
};
static_assert(sizeof(SR15NetMatchSettings) == 0x50);

class CR15NetGame {
public:
    std::uint64_t BeginMultiplayer(const SR15NetMatchSettings& settings);
    void BeginMultiplayerWithLobbyAddr(std::uint64_t symbol,
                                       std::uint32_t address,
                                       std::uint16_t port,
                                       std::uint16_t mode);
};

namespace {

template <typename T>
T Load(const void* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset,
                sizeof(value));
    return value;
}

template <typename T>
void Store(void* base, std::size_t offset, const T& value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof(value));
}

void CallVirtualNoArgs(void* object, std::size_t byte_offset) {
    const std::byte* const vtable = Load<const std::byte*>(object, 0);
    using Function = void (*)(void*);
    Function function = nullptr;
    std::memcpy(&function, vtable + byte_offset, sizeof(function));
    function(object);
}

void CallVirtualWithId(void* object, std::size_t byte_offset,
                       std::uint32_t id) {
    const std::byte* const vtable = Load<const std::byte*>(object, 0);
    using Function = void (*)(void*, std::uint32_t);
    Function function = nullptr;
    std::memcpy(&function, vtable + byte_offset, sizeof(function));
    function(object, id);
}

}  // namespace

void CR15NetGame::BeginMultiplayerWithLobbyAddr(
    std::uint64_t symbol, std::uint32_t address, std::uint16_t port,
    std::uint16_t mode) {
    Store(this, 0x88, symbol);

    // 0x138c04c..0x138c070: the raw port is stored, unless HToNS(port) is zero,
    // in which case the default NToHS(0x1a88) is stored instead.
    std::uint16_t stored_port = port;
    if (CSysNet::HToNS(port) == 0) {
        stored_port = CSysNet::NToHS(0x1a88);
    }
    Store(this, 0x94, stored_port);

    // 0x138c078..0x138c090: loopback 127.0.0.1 is replaced by the local host's address.
    if (address == 0x0100007fU) {
        const CSysNet::SAddress local = CSysNet::LocalHost(
            CSysNet::HToNS(stored_port));
        address = local.Ipv4();
    }
    Store(this, 0x90, address);
    Store(this, 0x96, mode);

    if (mode == 4) {
        constexpr std::size_t spectator_flag_offset = 0x647e9;
        const std::uint8_t old_flags = Load<std::uint8_t>(
            this, spectator_flag_offset);
        Store(this, spectator_flag_offset,
              static_cast<std::uint8_t>(old_flags | 2U));
        void* const first_listener = reinterpret_cast<void*>(
            Load<std::uintptr_t>(this, 0x647c8));
        if (old_flags == 0 && first_listener != nullptr) {
            CallVirtualNoArgs(first_listener, 0x68);
            auto* const local_user = reinterpret_cast<CNSUser*>(
                Load<std::uintptr_t>(this, 0x28d0));
            if (local_user != nullptr) {
                void* const listener = reinterpret_cast<void*>(
                    Load<std::uintptr_t>(this, 0x647c8));
                const auto id = static_cast<std::uint32_t>(
                    local_user->LocalID());
                CallVirtualWithId(listener, 0x78, id);
            }
        }
    }

    // The assembly writes +0x00, +0x08, +0x10, +0x18 and +0x40 and clears nothing else.
    SR15NetMatchSettings settings;
    const std::uint64_t kind = 0x300000000ULL;
    const std::uint64_t unset = UINT64_MAX;
    const std::uint64_t no_flags = 0;
    void* const owner = reinterpret_cast<void*>(Load<std::uintptr_t>(this, 8));
    const std::uint64_t user_data = Load<std::uint64_t>(owner, 0x8560);
    const std::uint64_t game_data = Load<std::uint64_t>(owner, 0x948);
    Store(settings.raw, 0x00, kind);
    Store(settings.raw, 0x08, user_data);
    Store(settings.raw, 0x10, game_data);
    Store(settings.raw, 0x18, unset);
    Store(settings.raw, 0x40, no_flags);
    BeginMultiplayer(settings);
}

}  // namespace NRadGame
}  // namespace NRadEngine
