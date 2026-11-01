// CR15NetGame::BotActorId
// Quest libr15.so VA 0x134dd38; ReVault binary 576.
// ReVault raw parent 100223285, reconstruction node 100600456; grade H: 17 case constants read from memory in source order, every branch and tail call checked against the disassembly; GetTLS and CRndSeed::Twister bodies and the TLS global's name are not verified.
// @0x134dd38 — CR15NetGame::BotActorId
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

// Quest libr15.so: 32-bit TLS slot index of the random state, stored at VA 0x0386c350 (.bss).
// The binary does not name it; this name is ours.
extern std::uint32_t g_rnd_seed_tls_index;

namespace NRadEngine {

struct CSymbol64 {
    std::uint64_t bits;  // the whole value arrives in x1 and is compared as 64 bits
};

struct CVR {
    // NRadEngine::CVR::HeadsetType() at FUN_01b6f164 (VA 0x1b6f164), via thunk 0x10451e0.
    // Disassembly: mov w0, #3; ret.
    static int HeadsetType();
};

struct CSysThread {
    // NRadEngine::CSysThread::GetTLS(int) at FUN_0108a22c, via thunk 0x104b720.
    // Its body tail-calls an imported function; the caller uses the returned pointer.
    static void* GetTLS(int index);
};

struct CRndSeed {
    // NRadEngine::CRndSeed::Twister() at FUN_01428b28, via thunk 0xfd7bd0.
    // The receiver is the TLS block (x0 at the call site).
    void Twister();
};

namespace NRadGame {

enum ER15BodyType : std::uint32_t;

struct CR15ActorUtils {
    // NRadEngine::NRadGame::CR15ActorUtils::BotActorId(ER15BodyType) at VA 0x21e0c58, via thunk 0xfdac70.
    // Disassembly: returns the 64-bit table entry indexed by the 32-bit argument.
    static std::uint64_t BotActorId(ER15BodyType body_type);
};

class CR15NetGame {
public:
    // NRadEngine::NRadGame::CR15NetGame::BotActorId(NRadEngine::CSymbol64) const, VA 0x134dd38.
    std::uint64_t BotActorId(CSymbol64 symbol) const;
    // Other members are not recovered.
};

}  // namespace NRadGame
}  // namespace NRadEngine

namespace {

std::uint32_t LoadU32(const void* base, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

void StoreU32(void* base, std::size_t offset, std::uint32_t value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof value);
}

}  // namespace

namespace NRadEngine::NRadGame {

std::uint64_t CR15NetGame::BotActorId(CSymbol64 symbol) const {
    // The receiver (x0) is not read; the first call overwrites it.
    if (CVR::HeadsetType() == 3) {
        return CR15ActorUtils::BotActorId(static_cast<ER15BodyType>(0));
    }
    switch (symbol.bits) {
    case 0xcb60a4de7e1caf73ULL:  // -0x349f5b2181e3508d
    case 0x09990965f4db8c03ULL:
    case 0xd53a9c73b8b2c760ULL:  // -0x2ac5638c474d38a0
    case 0x4dc1ddbd89b28531ULL:
    case 0x339c5dc66c03b81fULL:
    case 0xd54d62c40852c2f9ULL:  // -0x2ab29d3bf7ad3d07
    case 0x888ac0cfd102345fULL:  // -0x77753f302efdcba1
    case 0xdf8258f63e878e9dULL:  // -0x207da709c1787163
    case 0xea8d9b0bbdaa1c6aULL:  // -0x157264f44255e396
    case 0xd894afea8246bc6bULL:  // -0x276b50157db94395
        return CR15ActorUtils::BotActorId(static_cast<ER15BodyType>(0));
    case 0x3d5c3976578a321aULL:
    case 0x33bbf6842df97a3fULL:
    case 0x6b44ed1ad5234fbcULL:
    case 0x4311cb2c132e9299ULL:
    case 0x25c1c8218705f693ULL:
    case 0x61fc299f53160641ULL:
    case 0x47c693e5ebaddea6ULL:
        return CR15ActorUtils::BotActorId(static_cast<ER15BodyType>(1));
    default:
        break;
    }

    // Random index: 0x270 32-bit words at TLS +0x9c0, index at TLS +0x1380.
    void* const tls = CSysThread::GetTLS(static_cast<int>(g_rnd_seed_tls_index));
    std::uint32_t index = LoadU32(tls, 0x1380);
    if (index == 0x270) {
        reinterpret_cast<CRndSeed*>(tls)->Twister();
        index = LoadU32(tls, 0x1380);
    }
    StoreU32(tls, 0x1380, index + 1);
    const std::uint32_t word = LoadU32(tls, 0x9c0 + static_cast<std::size_t>(index) * 4);
    return CR15ActorUtils::BotActorId(static_cast<ER15BodyType>((~word) & 1u));
}

}  // namespace NRadEngine::NRadGame
