// NRadEngine::CncaGame::RequireGameSpace
// Quest libr15.so VA 0x18d3e88; ReVault binary 576.
// ReVault raw parent 100255119, reconstruction node 100600588; grade H: all 47 instructions checked against the disassembly; the callee is the named CLevelLoadSet::Append (through the stub at 0x010407b0); no placeholders; CncaGame and SLevelLoad are raw offsets.
// @0x18d3e88 — NRadEngine::CncaGame::RequireGameSpace
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// NRadEngine::SLevelLoad, the reference parameter of CncaGame::RequireGameSpace. Only the bytes
// this function reads are modelled: the u64 key at +0x00 and the u32 flags at +0x30. The rest of
// the 0x38 bytes is not recovered.
struct SLevelLoad {
    alignas(8) std::byte raw[0x38];
};
static_assert(sizeof(SLevelLoad) == 0x38);

// NRadEngine::CLevelLoadSet::Append(SLevelLoad const&). The set is at CncaGame +0x9d0. The call is a
// tail call (b 0x010407b0), so Append receives x0 = set and x1 = load. Append never reads w2, the
// flag, which the stub passes through.
class CLevelLoadSet {
  public:
    void Append(const SLevelLoad& load);
};

// NRadEngine::CncaGame. ReVault has no layout for it, so the bytes this function touches are raw:
// the CLevelLoadSet at +0x9d0 (entries of 0x38 bytes, each with its u64 key at +0x00 and its u32
// flags at +0x30) and the u32 count at +0x1ed0, which is the set's count at set+0x1500.
class CncaGame {
  public:
    void RequireGameSpace(const SLevelLoad& load, unsigned int flag);
    // Storage up to the end of the count at +0x1ed0 (+0x1ed4), rounded up to alignment 8.
    alignas(8) std::byte raw[0x1ed8];
};
static_assert(sizeof(CncaGame) >= 0x1ed4);

// Removes the entry whose key equals load's key when flag is zero, shifting later entries down and
// shrinking the count. Otherwise ORs load's flags into the matching entry, or appends load when no
// entry matches.
void CncaGame::RequireGameSpace(const SLevelLoad& load, unsigned int flag) {
    constexpr std::size_t kSet = 0x9d0;     // CLevelLoadSet inside CncaGame
    constexpr std::size_t kCount = 0x1ed0;  // u32 count, the set's count at set+0x1500
    constexpr std::size_t kEntry = 0x38;    // one SLevelLoad entry of the set
    constexpr std::size_t kKey = 0x00;      // u64 key, in a load and in an entry
    constexpr std::size_t kFlags = 0x30;    // u32 flags, in a load and in an entry

    std::byte* const game = raw;
    std::uint64_t key = 0;
    std::memcpy(&key, load.raw + kKey, sizeof key);
    std::uint32_t count = 0;
    std::memcpy(&count, game + kCount, sizeof count);

    if (flag == 0) {
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint64_t entry_key = 0;
            std::memcpy(&entry_key, game + kSet + i * kEntry + kKey, sizeof entry_key);
            if (entry_key != key) {
                continue;
            }
            const std::uint32_t remaining = count - 1;
            for (std::uint32_t d = i; d < remaining; ++d) {
                std::memcpy(game + kSet + d * kEntry, game + kSet + (d + 1) * kEntry, kEntry);
            }
            std::memcpy(game + kCount, &remaining, sizeof remaining);
            return;
        }
        return;
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        std::byte* const entry = game + kSet + i * kEntry;
        std::uint64_t entry_key = 0;
        std::memcpy(&entry_key, entry + kKey, sizeof entry_key);
        if (entry_key == key) {
            std::uint32_t entry_flags = 0;
            std::uint32_t load_flags = 0;
            std::memcpy(&entry_flags, entry + kFlags, sizeof entry_flags);
            std::memcpy(&load_flags, load.raw + kFlags, sizeof load_flags);
            entry_flags |= load_flags;
            std::memcpy(entry + kFlags, &entry_flags, sizeof entry_flags);
            return;
        }
    }
    reinterpret_cast<CLevelLoadSet*>(game + kSet)->Append(load);
}

}  // namespace NRadEngine
