// R15 session writer team-filter evidence (G8b).
// Quest libr15.so VA 0x13b9848 (RVA 0x12b9848); patch site 0x13bab6c.
// ReVault binary 576, raw parent 100224909, reconstruction node 100600200; tier: reconstructed; grade: L.
// Whole-project search: searched all 1,041 binaries in project echovr.
// @0x13b9848 — NRadEngine::NRadGame::NR15RestAPI::WriteSessionJson
/* @confidence: L */
/*
 * Scoped reconstruction of the G8b entrant-team filter only.
 * The remainder of WriteSessionJson is still represented by its raw parent
 * decompilation (100224909); this fragment does not reconstruct the full writer.
 */
#include <cstdint>
#include <optional>

namespace NRadEngine::NRadGame::NR15RestAPI {

struct SessionEntrantTeam {
    // The normalized bucket consumed by the writer's team name, count, and JSON loops.
    uint32_t bucket;
};

// Bits 10–12 of an entrant slot word hold the team code.
// G8b leaves 0–2 unchanged, maps 3–4 to bucket 2, and omits 5–7.
std::optional<SessionEntrantTeam>
NormalizeSessionEntrantTeam(uint16_t slotWord) {
    const uint32_t encodedTeam = (slotWord >> 10) & 0x7;
    if (encodedTeam > 4) {
        return std::nullopt;
    }

    const uint32_t bucket = encodedTeam <= 2 ? encodedTeam : 2;
    return SessionEntrantTeam{bucket};
}

} // namespace NRadEngine::NRadGame::NR15RestAPI
