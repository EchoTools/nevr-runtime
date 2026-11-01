// Quest libr15.so VA 0x138bf64; ReVault binary libr15.so.
// ReVault raw parent 100224054, reconstruction node 100600625; grade M.
#include <cstddef>
#include <cstdint>
#include <cstring>

// @0x138bf64 — NRadEngine::NRadGame::CR15NetGame::BeginMultiplayerWithLobbyId
/* @confidence: M */

namespace NRadEngine {

struct SUuid {
    std::uint64_t words[2];
};

class CNSUser {
public:
    std::uint64_t LocalID() const;
};

namespace NRadGame {

struct SR15NetMatchSettings {
    alignas(8) std::byte raw[0x50];
};
static_assert(sizeof(SR15NetMatchSettings) == 0x50);

class CR15NetGame {
public:
    std::uint64_t BeginMultiplayer(const SR15NetMatchSettings& settings);
    void BeginMultiplayerWithLobbyId(const SUuid& lobby, std::uint16_t mode);
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

void CR15NetGame::BeginMultiplayerWithLobbyId(
    const SUuid& lobby, std::uint16_t mode) {
    auto* const self = reinterpret_cast<std::byte*>(this);
    std::memcpy(self + 0x70, &lobby, sizeof(lobby));
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
                CallVirtualWithId(
                    listener, 0x78,
                    static_cast<std::uint32_t>(local_user->LocalID()));
            }
        }
    }

    alignas(8) std::byte settings_storage[0x50];
    std::memset(settings_storage, 0, sizeof(settings_storage));
    auto* const settings = reinterpret_cast<SR15NetMatchSettings*>(
        settings_storage);
    const std::uint64_t kind = 0x300000000ULL;
    const std::uint64_t unset = UINT64_MAX;
    const std::uint64_t no_flags = 0;
    void* const owner = reinterpret_cast<void*>(Load<std::uintptr_t>(this, 8));
    const std::uint64_t user_data = Load<std::uint64_t>(owner, 0x8560);
    const std::uint64_t game_data = Load<std::uint64_t>(owner, 0x948);
    Store(settings->raw, 0x00, kind);
    Store(settings->raw, 0x08, user_data);
    Store(settings->raw, 0x10, game_data);
    Store(settings->raw, 0x18, unset);
    Store(settings->raw, 0x40, no_flags);
    BeginMultiplayer(*settings);
}

}  // namespace NRadGame
}  // namespace NRadEngine
