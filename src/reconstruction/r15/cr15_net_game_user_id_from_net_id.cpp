// NRadEngine::NRadGame::CR15NetGame::UserIdFromNetId
// Quest libr15.so VA 0x138e6e0; ReVault binary 576.
// ReVault raw parent 100224130, reconstruction node 100600643; grade unrecorded: this node text has no @confidence line.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CncaGameSpaceMT;

struct SActorGUID {
    std::uint64_t words[2];
};
static_assert(sizeof(SActorGUID) == 0x10);

namespace NRadGame {

class CR15NetActorCS {
public:
    // CR15NetActorCS::FindNetActorGUIDGlobal(uint64_t, CncaGameSpaceMT*) @0x23060ec (thunk 0x1050fe0).
    static SActorGUID FindNetActorGUIDGlobal(std::uint64_t net_id, CncaGameSpaceMT* game_space);
};

class CR15NetGame {
public:
    // CR15NetGame::UserId(SActorGUID) const @0x135a9b8 (thunk 0x1044c30); the 16-byte value is passed in x1:x2.
    std::uint32_t UserId(SActorGUID actor_guid) const;
    std::uint32_t UserIdFromNetId(std::uint64_t net_id);
};

}  // namespace NRadGame
}  // namespace NRadEngine

namespace NRadEngine::NRadGame {

std::uint32_t CR15NetGame::UserIdFromNetId(std::uint64_t net_id) {
    auto* const self = reinterpret_cast<const std::byte*>(this);
    CncaGameSpaceMT* const game_space = [&] {
        CncaGameSpaceMT* value = nullptr;
        std::memcpy(&value, self + 0x2b08, sizeof(value));
        return value;
    }();
    const SActorGUID actor_guid = CR15NetActorCS::FindNetActorGUIDGlobal(net_id, game_space);
    return UserId(actor_guid);
}

}  // namespace NRadEngine::NRadGame
