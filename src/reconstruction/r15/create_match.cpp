// CR15NetGame::CreateMatch
// Quest libr15.so VA 0x136f1a0; ReVault binary 576.
// ReVault raw parent 100223811, reconstruction node 100600460; grade H: every branch of the 122-instruction body traced to the disassembly; callees named by ReVault nodes; vtable slots 0xc8 and 0xd8 unnamed.
// @0x136f1a0 — CR15NetGame::CreateMatch
/* @confidence: H */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// libr15.so VA 0x10a4370 (called through thunk 0x103e540).
// Demangled: CJson::Boolean(char const*, unsigned int, unsigned int) const.
// default_value is returned when the path is missing or not a boolean (the body
// assigns it only on the found path); a nonzero log_missing logs the miss.
class CJson {
public:
    std::uint32_t Boolean(const char* key, std::uint32_t default_value,
                          std::uint32_t log_missing) const;
};

// libr15.so VA 0x1a353d0 (called through thunk 0xff50f0).
// Demangled: CNSUser::ClientProfile(). The raw body returns this + 0x8.
class CNSUser {
public:
    CJson* ClientProfile();
};

// libr15.so VA 0x12cb908 (called through thunk 0x100cc90).
// Demangled: CMemory::Copy(void*, void const*, unsigned long long); no this argument at the call site.
class CMemory {
public:
    static void Copy(void* destination, const void* source, std::uint64_t bytes);
};

namespace NRadGame {

// Layout as in find_match.cpp. field_08 is compared with gCreateMatchKindA/B.
struct SR15NetMatchSettings {
    std::uint64_t field_00;
    std::uint64_t field_08;
    std::uint64_t field_10;
    std::uint64_t field_18;
    std::array<std::uint16_t, 16> match_ids;
    std::uint32_t match_count;
};
static_assert(offsetof(SR15NetMatchSettings, field_00) == 0x00);
static_assert(offsetof(SR15NetMatchSettings, field_08) == 0x08);
static_assert(offsetof(SR15NetMatchSettings, field_10) == 0x10);
static_assert(offsetof(SR15NetMatchSettings, field_18) == 0x18);
static_assert(offsetof(SR15NetMatchSettings, match_ids) == 0x20);
static_assert(offsetof(SR15NetMatchSettings, match_count) == 0x40);
static_assert(sizeof(SR15NetMatchSettings) == 0x48);

constexpr std::size_t kMatchIdsOffset = offsetof(SR15NetMatchSettings, match_ids);

// libr15.so VA 0x13428e0 (called through thunk 0x1038090).
// Demangled: CR15NetClientLobby::Create(void).
struct CR15NetClientLobby {
    void Create();
};

// Lobby view as in find_match.cpp: the settings copy sits at lobby + 0x698.
struct CR15NetClientLobbyPrefix {
    std::byte unknown_0000[0x698];
    SR15NetMatchSettings settings;
};
static_assert(offsetof(CR15NetClientLobbyPrefix, settings) == 0x698);
static_assert(offsetof(CR15NetClientLobbyPrefix, settings) +
              offsetof(SR15NetMatchSettings, match_count) == 0x6d8);
static_assert(offsetof(CR15NetClientLobbyPrefix, settings) +
              offsetof(SR15NetMatchSettings, match_ids) == 0x6b8);
static_assert(sizeof(CR15NetClientLobbyPrefix) == 0x6e0);

// Stored image, libr15.so: VA 0x2cac900 holds 0xea8d9b0bbdaa1c6a and
// VA 0x2cac920 holds 0x47c693e5ebaddea6. Read from the stored bytes, not from a runtime value.
extern const std::uint64_t gCreateMatchKindA;
extern const std::uint64_t gCreateMatchKindB;

// PartyCount: libr15.so VA 0x138e71c (called through thunk 0x1016460), declared as in
// cr15_net_game.hpp. CreateMatch is the function this file reconstructs.
class CR15NetGame {
public:
    std::uint64_t PartyCount(std::uint16_t index) const;
    void CreateMatch(SR15NetMatchSettings* settings);
};

// Layout view of the CR15NetGame fields CreateMatch reads (offsets as in find_match.cpp).
struct CR15NetGamePrefix {
    std::byte unknown_0000[0x40];
    CR15NetClientLobby* lobby;
    std::byte unknown_0048[0x96];
    std::uint16_t state_count;
    std::byte unknown_00e0[0x27f0];
    CNSUser* fallback_state;
    std::byte unknown_28d8[0x4c8];
    const std::uint8_t* mode_flags;
    std::byte unknown_2da8[0x61a20];
    void* match_service;  // vtable slots 0xc8 and 0xd8 are called; the class is not recovered
};
static_assert(offsetof(CR15NetGamePrefix, lobby) == 0x40);
static_assert(offsetof(CR15NetGamePrefix, state_count) == 0xde);
static_assert(offsetof(CR15NetGamePrefix, fallback_state) == 0x28d0);
static_assert(offsetof(CR15NetGamePrefix, unknown_28d8) == 0x28d8);
static_assert(offsetof(CR15NetGamePrefix, mode_flags) == 0x2da0);
static_assert(offsetof(CR15NetGamePrefix, match_service) == 0x647c8);
static_assert(sizeof(CR15NetGamePrefix) == 0x647d0);

// Raw vtable slot call: object's vtable entry at byte_offset, called with object as this.
template <typename Result>
Result InvokeUnaryVirtualAt(void* object, std::size_t byte_offset) {
    using Function = Result (*)(void*);
    auto** vtable = *reinterpret_cast<void***>(object);
    auto function = reinterpret_cast<Function>(vtable[byte_offset / sizeof(void*)]);
    return function(object);
}

// The original path runs count-dependent decrement loops after filling the remaining
// 16-bit entries. They have no memory effects; signal fences keep them in optimized builds.
static void PreserveFillPathDelay(std::uint32_t count_delta) {
    if (count_delta >= 2) {
        const std::uint32_t even_count = count_delta & ~std::uint32_t{1};
        for (std::uint32_t remaining = even_count; remaining != 0; remaining -= 2) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        count_delta -= even_count;
    }
    for (std::uint32_t remaining = count_delta; remaining != 0; --remaining) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
}

void CR15NetGame::CreateMatch(SR15NetMatchSettings* settings) {
    auto* game = reinterpret_cast<CR15NetGamePrefix*>(this);

    // 0x136f1a0-0x136f1cc: profile = fallback_state ? fallback_state->ClientProfile() : this + 0x28d8.
    CJson* const profile = game->fallback_state != nullptr
                               ? game->fallback_state->ClientProfile()
                               : reinterpret_cast<CJson*>(game->unknown_28d8);
    // 0x136f1cc-0x136f1e0: eligible = profile->Boolean("npe|firstmatch|completed", 0, 0); cbz w0.
    const std::uint32_t eligible = profile->Boolean("npe|firstmatch|completed", 0, 0);
    // 0x136f214-0x136f244: a settings kind equal to either stored value is special.
    const bool special_kind = settings->field_08 == gCreateMatchKindA ||
                              settings->field_08 == gCreateMatchKindB;
    // 0x136f1e4-0x136f200: reached when eligible or not special. A match service whose
    // slot 0xc8 returns 0 ends the call.
    if (eligible != 0 || !special_kind) {
        void* const service = game->match_service;
        if (service != nullptr && InvokeUnaryVirtualAt<std::uint32_t>(service, 0xc8) == 0) {
            return;
        }
    }

    // 0x136f248-0x136f32c: the count block.
    if (settings->match_count != 0) {
        // 0x136f250-0x136f258: bit 2 of *mode_flags forces the count to 0.
        if ((game->mode_flags[0] & 0x04U) == 0) {
            std::uint32_t capacity;
            if (game->state_count != 0) {
                // 0x136f264: PartyCount(0).
                capacity = static_cast<std::uint32_t>(PartyCount(0));
            } else if (game->match_service != nullptr) {
                // 0x136f29c: match_service slot 0xd8.
                capacity = InvokeUnaryVirtualAt<std::uint32_t>(game->match_service, 0xd8);
            } else {
                // 0x136f2bc: fallback_state != null ? 1 : 0.
                capacity = game->fallback_state != nullptr ? 1U : 0U;
            }
            if (settings->match_count < capacity) {
                const std::uint32_t count = settings->match_count;
                const std::uint32_t delta = capacity - count;
                std::memset(reinterpret_cast<std::byte*>(settings) + kMatchIdsOffset +
                                static_cast<std::size_t>(count) * sizeof(std::uint16_t),
                            0xff, static_cast<std::size_t>(delta) * sizeof(std::uint16_t));
                PreserveFillPathDelay(delta);
                settings->match_count = capacity;
            }
            // 0x136f330-0x136f33c: a capacity above 15 becomes 16.
            if (capacity > 0x0fU) {
                capacity = 0x10U;
            }
            settings->match_count = capacity;
        } else {
            // 0x136f284: flagged, count = 0.
            settings->match_count = 0;
        }
    }

    // 0x136f340-0x136f374: copy the 0x20-byte head into the lobby. The count and the ids
    // follow only when the lobby slot is not the settings object itself (the original's
    // unconditional self-copy in that case is a no-op, so it is not written here).
    CR15NetClientLobby* const lobby = game->lobby;
    auto* const view = reinterpret_cast<CR15NetClientLobbyPrefix*>(lobby);
    if (&view->settings != settings) {
        std::memcpy(&view->settings, settings, 0x20);
        view->settings.match_count = settings->match_count;
        if (settings->match_count != 0) {
            NRadEngine::CMemory::Copy(
                reinterpret_cast<std::byte*>(&view->settings) + kMatchIdsOffset,
                reinterpret_cast<const std::byte*>(settings) + kMatchIdsOffset,
                static_cast<std::uint64_t>(settings->match_count) * sizeof(std::uint16_t));
        }
    }
    // 0x136f384: tail call CR15NetClientLobby::Create on lobby.
    lobby->Create();
}

}  // namespace NRadGame
}  // namespace NRadEngine
