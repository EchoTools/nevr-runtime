// CR15NetGame::FindMatch (G12/G12b offline combat entry; scoped partial)
// Quest libr15.so VA 0x1396548 (RVA 0x1296548); PC twin not located by project-wide search.
// ReVault binary 576, raw parent 100224328, reconstruction node 100600202; tier: reconstructed; grade: L.
// Search: `CR15NetGame::FindMatch`, searched all 1,041 binaries in project echovr.
// @0x1396548 — NRadEngine::NRadGame::CR15NetGame::FindMatch
/* @confidence: L */
/*
 * Scoped reconstruction of the G12/G12b branch at FindMatch's call to
 * CR15NetClientLobby::Find. The rest of FindMatch remains in raw parent
 * 100224328: settings normalization/copy and the original fallback call.
 */
#include <array>
#include <cstdint>

namespace NRadEngine::NRadGame {

enum class FindCombatMapPolicy {
    Dyson,   // G12
    Rotate,  // optional G12b
};

struct CR15NetClientLobby;
struct CR15NetGame;

constexpr std::uint64_t kEchoCombat = 0x3D5C3976578A321AULL;
constexpr std::uint64_t kEchoCombatPracticeAI = 0x25C1C8218705F693ULL;
constexpr std::uint64_t kCombatDyson = 0x43E2DA7914642604ULL;
constexpr std::array<std::uint64_t, 4> kRotatingCombatMaps{
    0x43E2DA7914642604ULL, // mpl_combat_dyson
    0x42670F2BED45703CULL, // mpl_combat_combustion
    0xDF5CA7B7DFA383D4ULL, // mpl_combat_fission
    0x43E2DA7A0C623A19ULL, // mpl_combat_gauss
};

// Returns true when the cave diverts the original matchmaking call into G4.
bool TryG12OfflineCombat(CR15NetClientLobby* lobby,
                        FindCombatMapPolicy mapPolicy) {
    CR15NetGame* game = lobby->NetGame();
    const std::uint64_t gameFlags = game->Flags();

    // The cave accepts offline clients only; dedicated-server mode keeps Find().
    if ((gameFlags & 0x44) != 0x40) {
        return false;
    }

    // LOBBY_SETTINGS begins at lobby+0x698: gametype is +0x08,
    // level name is +0x10. The settings have already been copied by FindMatch.
    std::uint64_t& gameType = lobby->Settings().gameType;
    std::uint64_t& levelName = lobby->Settings().levelName;
    if (gameType != kEchoCombat) {
        return false;
    }

    gameType = kEchoCombatPracticeAI;

    // -1 (unset) and 0 both request the default map.
    if (levelName == UINT64_MAX || levelName == 0) {
        if (mapPolicy == FindCombatMapPolicy::Dyson) {
            levelName = kCombatDyson;
        } else {
            // G12b reads the low two bits, then increments the stored byte
            // without masking it; subsequent reads apply the mask again.
            std::uint8_t& cursor = lobby->RotationByteAt0x6dc();
            levelName = kRotatingCombatMaps[cursor & 3];
            cursor = static_cast<std::uint8_t>(cursor + 1);
        }
    }

    // The cave branches to G4's offline entry at cave+0x20. That path queues
    // BeginOfflineMultiplayer with these lobby settings and returns through
    // FindMatch's epilogue instead of issuing the matchmaking request.
    lobby->BeginOfflineThroughG4();
    return true;
}

// At the call site, false preserves the original CR15NetClientLobby::Find call.
void CR15NetGame::FindMatchG12Branch(CR15NetClientLobby* lobby,
                                     FindCombatMapPolicy mapPolicy) {
    if (TryG12OfflineCombat(lobby, mapPolicy)) {
        return;
    }
    lobby->FindWithOriginalArguments();
}

} // namespace NRadEngine::NRadGame
