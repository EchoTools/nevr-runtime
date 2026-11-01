// CR15NetGame::BotActorId(CSymbol64) const
// Quest libr15.so: ReVault VA 0x134dd38 / ELF RVA 0x124dd38, binary 576.
// PC twin not located. The separate helper CR15ActorUtils::BotActorId is at 0x21e0c58.
// ReVault raw parent 100223285; reconstruction node 100600167; tier: reconstructed; grade: H, assessed by revault-codex.
#include <cstdint>
namespace q15t::r15::reconstruction {
struct Ops {
    virtual int headset_type();
    virtual void call_CR15ActorUtils_BotActorId(int body_type);
    virtual std::uint32_t* thread_random_index();
    virtual std::uint32_t* thread_random_words();
    virtual void seed_twister();
};
void BotActorId(std::uint64_t symbol, Ops& ops) {
    if (ops.headset_type() == 3) { ops.call_CR15ActorUtils_BotActorId(0); return; }
    switch (symbol) {
    case static_cast<std::uint64_t>(-0x349f5b2181e3508dLL):
    case 0x09990965f4db8c03ULL:
    case static_cast<std::uint64_t>(-0x2ac5638c474d38a0LL):
    case 0x4dc1ddbd89b28531ULL: case 0x339c5dc66c03b81fULL:
    case static_cast<std::uint64_t>(-0x2ab29d3bf7ad3d07LL):
    case static_cast<std::uint64_t>(-0x77753f302efdcba1LL):
    case static_cast<std::uint64_t>(-0x207da709c1787163LL):
    case static_cast<std::uint64_t>(-0x157264f44255e396LL):
    case static_cast<std::uint64_t>(-0x276b50157db94395LL):
        ops.call_CR15ActorUtils_BotActorId(0); return;
    case 0x3d5c3976578a321aULL: case 0x33bbf6842df97a3fULL:
    case 0x6b44ed1ad5234fbcULL: case 0x4311cb2c132e9299ULL:
    case 0x25c1c8218705f693ULL: case 0x61fc299f53160641ULL:
    case 0x47c693e5ebaddea6ULL:
        ops.call_CR15ActorUtils_BotActorId(1); return;
    default: break;
    }
    auto* index = ops.thread_random_index();
    if (*index == 0x270) { ops.seed_twister(); index = ops.thread_random_index(); }
    const auto word = ops.thread_random_words()[(*index)++];
    ops.call_CR15ActorUtils_BotActorId(static_cast<int>((~word) & 1));
}
} // namespace q15t::r15::reconstruction
