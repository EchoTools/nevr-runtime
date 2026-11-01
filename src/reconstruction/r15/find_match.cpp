// @0x1396548 — NRadEngine::NRadGame::CR15NetGame::FindMatch
// Quest libr15.so VA 0x1396548 / ELF RVA 0x1296548; ReVault binary 576.
// ReVault raw parent 100224328; reconstruction node 100600202; grade M.
// PCVR counterpart RVA is not established by this reconstruction.
// The 0x382adc8 GOT-referenced UUID value has no readable initializer in the stored image.
 /* @confidence: M */
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine::NRadGame {

struct SUuid {
    std::uint64_t words[2];
};
static_assert(offsetof(SUuid, words) == 0x00);
static_assert(sizeof(SUuid) == 0x10);

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

struct CR15NetClientLobby {
    void Find(const SUuid& session, const SUuid& group);
};

// This view covers only the lobby fields touched by FindMatch; it does not
// assert the complete CR15NetClientLobby object size.
struct CR15NetClientLobbyPrefix {
    std::byte unknown_0000[0x698];
    SR15NetMatchSettings settings;
};
static_assert(offsetof(CR15NetClientLobbyPrefix, unknown_0000) == 0x00);
static_assert(offsetof(CR15NetClientLobbyPrefix, settings) == 0x698);
static_assert(offsetof(CR15NetClientLobbyPrefix, settings) +
              offsetof(SR15NetMatchSettings, match_count) == 0x6d8);
static_assert(offsetof(CR15NetClientLobbyPrefix, settings) +
              offsetof(SR15NetMatchSettings, match_ids) == 0x6b8);
static_assert(sizeof(CR15NetClientLobbyPrefix) == 0x6e0);

// The GOT slot at 0x382adc8 points to the 16-byte value copied into both Find arguments.
// ReVault's stored image has no readable initializer for the slot or pointed-to value.
extern const SUuid* const gFindMatchDefaultArgument;

template <typename Result>
Result InvokeUnaryVirtualAt(void* object, std::size_t byte_offset) {
    using Function = Result (*)(void*);
    auto** vtable = *reinterpret_cast<void***>(object);
    auto function = reinterpret_cast<Function>(vtable[byte_offset / sizeof(void*)]);
    return function(object);
}

// The layout view ends at the last field read by FindMatch; it is not a claim
// about the complete CR15NetGame object size.
struct CR15NetGamePrefix {
    std::byte unknown_0000[0x40];
    CR15NetClientLobby* lobby;
    std::byte unknown_0048[0x96];
    std::uint16_t state_count;
    std::byte unknown_00e0[0x27f0];
    void* fallback_state;
    std::byte unknown_28d8[0x4c8];
    const std::uint8_t* mode_flags;
    std::byte unknown_2da8[0x61a20];
    void* match_service;
};
static_assert(offsetof(CR15NetGamePrefix, unknown_0000) == 0x00);
static_assert(offsetof(CR15NetGamePrefix, lobby) == 0x40);
static_assert(offsetof(CR15NetGamePrefix, unknown_0048) == 0x48);
static_assert(offsetof(CR15NetGamePrefix, state_count) == 0xde);
static_assert(offsetof(CR15NetGamePrefix, unknown_00e0) == 0xe0);
static_assert(offsetof(CR15NetGamePrefix, fallback_state) == 0x28d0);
static_assert(offsetof(CR15NetGamePrefix, unknown_28d8) == 0x28d8);
static_assert(offsetof(CR15NetGamePrefix, mode_flags) == 0x2da0);
static_assert(offsetof(CR15NetGamePrefix, unknown_2da8) == 0x2da8);
static_assert(offsetof(CR15NetGamePrefix, match_service) == 0x647c8);
static_assert(sizeof(CR15NetGamePrefix) == 0x647d0);

struct CR15NetGame {
    std::uint64_t PartyCount(std::uint16_t index) const;
    void FindMatch(SR15NetMatchSettings* settings);
};

// The original path executes count-dependent decrement loops after filling the
// remaining 16-bit entries. They have no memory effects; signal fences keep
// those loops present in optimized builds.
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

void CR15NetGame::FindMatch(SR15NetMatchSettings* settings) {
    auto* game = reinterpret_cast<CR15NetGamePrefix*>(this);
    void* service = game->match_service;
    if (service != nullptr &&
        InvokeUnaryVirtualAt<std::uint32_t>(service, 0xc8) == 0) {
        return;
    }

    if (settings->match_count != 0) {
        std::uint32_t target_count = 0;
        if ((game->mode_flags[0] & 0x04U) == 0) {
            if (game->state_count != 0) {
                target_count = static_cast<std::uint32_t>(PartyCount(0));
            } else if (service != nullptr) {
                target_count = InvokeUnaryVirtualAt<std::uint32_t>(service, 0xd8);
            } else {
                target_count = game->fallback_state != nullptr ? 1U : 0U;
            }
        }

        if (settings->match_count < target_count) {
            const std::uint32_t count_delta = target_count - settings->match_count;
            std::memset(settings->match_ids.data() + settings->match_count,
                        0xff,
                        static_cast<std::size_t>(count_delta) * sizeof(std::uint16_t));
            PreserveFillPathDelay(count_delta);
        } else if (target_count > 0x0fU) {
            target_count = 0x10U;
        }
        settings->match_count = target_count;
    }

    auto* client_lobby = game->lobby;
    auto* lobby_view = reinterpret_cast<CR15NetClientLobbyPrefix*>(client_lobby);
    std::memcpy(&lobby_view->settings, settings, 0x20);
    if (&lobby_view->settings != settings) {
        lobby_view->settings.match_count = settings->match_count;
        if (settings->match_count != 0) {
            std::memcpy(lobby_view->settings.match_ids.data(),
                        settings->match_ids.data(),
                        static_cast<std::size_t>(settings->match_count) *
                            sizeof(std::uint16_t));
        }
    }

    const SUuid first_argument = *gFindMatchDefaultArgument;
    const SUuid second_argument = *gFindMatchDefaultArgument;
    client_lobby->Find(first_argument, second_argument);
}

} // namespace NRadEngine::NRadGame
