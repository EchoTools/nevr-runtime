// CSetFullScreenTintNode::Enter
// Quest libr15.so VA 0x282b354; ReVault binary 576.
// ReVault raw parent 100328364, reconstruction node 100600451; grade H: the cbz branch and the three loads and stores match the disassembly branch by branch; BindNode case 1 inlines the same copy; class from the libr15 mangled name at 0xbfad3c; x0 is this and x1 is never read; the global DAT_0386ce28 is unnamed.
// @0x282b354 — CSetFullScreenTintNode::Enter
/* @confidence: H */
// Grade rationale: H: the cbz branch and the three loads and stores match the disassembly branch by branch; BindNode case 1 inlines the same copy; class from the libr15 mangled name at 0xbfad3c; x0 is this and x1 is never read; the global DAT_0386ce28 is unnamed.

#include <cstddef>
#include <cstdint>
#include <cstring>

// DAT_0386ce28 (libr15.so VA 0x386ce28): global pointer to the tint state; owner not identified; Enter only reads it.
extern std::byte* g_full_screen_tint_state;

namespace NRadEngine {

struct SSetFullScreenTintNodeData;

class CSetFullScreenTintNode {
public:
    // x0 is this (the node); x1 (SSetFullScreenTintNodeData const&) is never read.
    void Enter(SSetFullScreenTintNodeData const&);

    // CScriptNode base sub-object; the first member below is at +0x8.
    [[maybe_unused]] std::byte base_[0x8];
    std::uint64_t tint_xy_;  // +0x08
    std::uint32_t tint_z_;   // +0x10
    std::uint32_t fade_;     // +0x14
};

static_assert(offsetof(CSetFullScreenTintNode, tint_xy_) == 0x08);
static_assert(offsetof(CSetFullScreenTintNode, tint_z_) == 0x10);
static_assert(offsetof(CSetFullScreenTintNode, fade_) == 0x14);

namespace {
std::uint32_t LoadU32(const std::byte* base, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}
std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}
}  // namespace

void CSetFullScreenTintNode::Enter(SSetFullScreenTintNodeData const&) {
    const std::byte* const state = g_full_screen_tint_state;
    if (state != nullptr) {
        const std::uint32_t tint_z = LoadU32(state, 0x174);
        const std::uint64_t tint_xy = LoadU64(state, 0x16c);
        tint_z_ = tint_z;
        tint_xy_ = tint_xy;
        fade_ = LoadU32(state, 0x17c);
    }
}

}  // namespace NRadEngine
