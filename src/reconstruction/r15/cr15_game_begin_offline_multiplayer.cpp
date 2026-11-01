// CR15Game::BeginOfflineMultiplayer
// Quest libr15.so VA 0x1301988; ReVault binary 576.
// ReVault raw parent 100221533, reconstruction node 100600452; grade M: branch structure and every offset match the 56-instruction listing; the settings tail copies indeterminate frame words and the callee's settings layout past 0x50 bytes is not established.
// @0x1301988 — CR15Game::BeginOfflineMultiplayer
/* @confidence: M */
// grade M: branch structure and every offset match the 56-instruction listing; the settings tail copies indeterminate frame words and the callee's settings layout past 0x50 bytes is not established.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CSymbol64 {
    std::uint64_t value;
};

namespace NWriteLog {
// NRadEngine::NWriteLog::WriteLog @ 0x1088c20 (called through thunk 0x104b960).
// Variadic. The first parameter is an ELogLevel passed in w0; its enumerators are not established.
void WriteLog(std::int32_t level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

namespace NRadGame {

class CR15NetGame {
public:
    // CR15NetGame::EndMultiplayer @ 0x1388c64 (called through thunk 0x102b140).
    void EndMultiplayer();
};

// The 0x50-byte prefix of SR15NetMatchSettings that this function writes. The full
// extent the callee reads is not established here.
struct SR15NetMatchSettings {
    std::uint64_t word_00;  // +0x00: 0x3'0000'0000
    std::uint64_t word_08;  // +0x08: ~0
    std::uint64_t symbol;   // +0x10: CSymbol64 argument (x1 on entry)
    std::uint64_t word_18;  // +0x18: ~0
    std::uint64_t tail[4];  // +0x20..+0x3f: copied from indeterminate frame bytes
    std::uint64_t word_40;  // +0x40: 0
    std::uint64_t word_48;  // +0x48: ~0
};

class CR15Game {
public:
    void BeginOfflineMultiplayer(CSymbol64 symbol);
    // CR15Game::BeginMultiplayer @ 0x1301d88 (called through thunk 0xfdc040).
    // The settings block is passed by address (x1).
    void BeginMultiplayer(const SR15NetMatchSettings& settings);
};

}  // namespace NRadGame
}  // namespace NRadEngine

// Unnamed 8-byte globals; the names follow the sibling reconstruction. 0x3832080 is
// file-backed (initial value -1); 0x386c568 lies beyond the end of the file content.
extern std::uint64_t global_03832080;
extern std::uint64_t global_0386c568;

namespace {
std::uint8_t LoadU8(const void* base, std::size_t offset) {
    return *reinterpret_cast<const std::uint8_t*>(
        static_cast<const std::byte*>(base) + offset);
}
std::uint64_t LoadU64(const void* base, std::size_t offset) {
    return *reinterpret_cast<const std::uint64_t*>(
        static_cast<const std::byte*>(base) + offset);
}
void StoreU64(void* base, std::size_t offset, std::uint64_t value) {
    *reinterpret_cast<std::uint64_t*>(static_cast<std::byte*>(base) + offset) = value;
}
}  // namespace

namespace NRadEngine {
namespace NRadGame {

void CR15Game::BeginOfflineMultiplayer(CSymbol64 symbol) {
    auto* const self = reinterpret_cast<std::byte*>(this);

    // sp+0x50..sp+0x6f of this frame. This function never writes them, and the binary
    // copies them into the settings tail as indeterminate values. Zero is one valid
    // instance of an indeterminate value, and keeps the copy well defined here.
    std::byte frame_tail[0x20] = {};

    SR15NetMatchSettings settings;
    settings.word_00 = 0x3'0000'0000ull;
    settings.word_08 = ~std::uint64_t{0};
    settings.symbol = symbol.value;
    settings.word_18 = ~std::uint64_t{0};
    std::memcpy(settings.tail, frame_tail, sizeof frame_tail);
    settings.word_40 = 0;
    settings.word_48 = ~std::uint64_t{0};

    NWriteLog::WriteLog(2, 0ull, "Ending multiplayer");

    // this+0x8510 is the netgame pointer. Bit 6 of the byte at (*(netgame+0x2da0))+1 selects
    // the EndMultiplayer path; a null netgame takes the first path.
    CR15NetGame* const net = reinterpret_cast<CR15NetGame*>(LoadU64(self, 0x8510));
    std::uint64_t value_7ad8 = 0;
    std::uint64_t value_8a68 = 0;
    if (net == nullptr ||
        ((LoadU8(reinterpret_cast<const void*>(LoadU64(net, 0x2da0)), 1) >> 6) & 1) == 0) {
        value_8a68 = LoadU64(self, 0x8a68) & ~std::uint64_t{0x42};
        StoreU64(self, 0x8a68, value_8a68);
        value_7ad8 = LoadU64(self, 0x7ad8);
    } else {
        net->EndMultiplayer();
        value_7ad8 = LoadU64(self, 0x7ad8) & ~std::uint64_t{0x4000};
        StoreU64(self, 0x7ad8, value_7ad8);
        value_8a68 = LoadU64(self, 0x8a68) & ~std::uint64_t{0x42};
        StoreU64(self, 0x8a68, value_8a68);
        global_03832080 = global_0386c568;
    }
    StoreU64(self, 0x7ad8, value_7ad8 & ~std::uint64_t{0x4000});
    StoreU64(self, 0x8a68, value_8a68 | 0x40);
    BeginMultiplayer(settings);
}

}  // namespace NRadGame
}  // namespace NRadEngine
