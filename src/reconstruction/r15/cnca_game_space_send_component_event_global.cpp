// NRadEngine::CncaGameSpace::SendComponentEventGlobal
// Quest libr15.so VA 0x190735c; ReVault binary 576.
// ReVault raw parent 100256042, reconstruction node 100600587; grade M: 0 placeholders; all 12 instructions checked; the callee is named (0x190281c) but the bl goes through PLT stub 0xffb7c0, a JUMP_SLOT the loader fills; record words and this+0x58 by raw offset; compiles clean under -Wall -Wextra -Werror.
// @0x190735c — NRadEngine::CncaGameSpace::SendComponentEventGlobal
/* @confidence: M */

#include <cstddef>
#include <cstdint>

namespace NRadEngine {

// The 0x28-byte record the callee reads. ReVault has no layout for it, so each
// 8-byte word is named by its offset.
struct SComponentEvent {
    std::uint64_t at_00;  // +0x00: CSymbol64 event id (x1 on entry)
    std::uint64_t at_08;  // +0x08: this->+0x58 of the game space
    std::uint64_t at_10;  // +0x10: SActor (x2 on entry), one 8-byte register
    std::uint64_t at_18;  // +0x18: CSymbol64 (x3 on entry)
    std::uint64_t at_20;  // +0x20: all ones (-1), the trailing word
};
static_assert(sizeof(SComponentEvent) == 0x28);
static_assert(offsetof(SComponentEvent, at_20) == 0x20);

class CncaGameSpace {
public:
    // VA 0x190281c: the 2-argument overload, reached from the bl at 0x190737c
    // through PLT stub 0xffb7c0 (ReVault thunk_FUN_0190281c, a JUMP_SLOT the loader fills).
    void SendComponentEventGlobal(const SComponentEvent& event, std::uint32_t flags);
    // VA 0x190735c. Entry: x0 this, x1 event id, x2 actor, x3 symbol, w4 flags.
    void SendComponentEventGlobal(std::uint64_t event_id, std::uint64_t actor,
                                  std::uint64_t symbol, std::uint32_t flags);

    alignas(8) std::byte before_58_[0x58];  // +0x00..+0x57: not recovered
    std::uint64_t word_58_;                 // +0x58: loaded on entry, no name in ReVault
};
static_assert(offsetof(CncaGameSpace, word_58_) == 0x58);

// Builds the record on the stack from x1, this->+0x58, x2, x3 and -1, then calls
// the 2-argument overload with the record's address and the flags.
void CncaGameSpace::SendComponentEventGlobal(std::uint64_t event_id, std::uint64_t actor,
                                             std::uint64_t symbol, std::uint32_t flags) {
    SComponentEvent event{};
    event.at_00 = event_id;
    event.at_08 = word_58_;
    event.at_10 = actor;
    event.at_18 = symbol;
    event.at_20 = ~std::uint64_t{0};
    SendComponentEventGlobal(event, flags);
}

}  // namespace NRadEngine
