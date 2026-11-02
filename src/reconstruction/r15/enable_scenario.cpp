// CR15NetAIScenarios::EnableScenario
// Quest libr15.so VA 0x133c2bc; ReVault binary 576.
// ReVault raw parent 100222938, reconstruction node 100600455; grade M: every branch and field offset checked against the disassembly and raw bytes; Alloc's w1 = 16 argument and return type are not in Ghidra's prototype, and the vtable slots come from the decompiler's GOT resolution.
// @0x133c2bc — CR15NetAIScenarios::EnableScenario
/* @confidence: M */
// M: every branch and field offset checked against the disassembly and raw bytes; Alloc's w1 = 16 argument and return type are not in Ghidra's prototype, and the vtable slots come from the decompiler's GOT resolution.

#include <cstddef>
#include <cstdint>

namespace NRadEngine {

// NRadEngine::SPeer::kInvalidPeer, read through the GOT slot at 0x380fc48.
extern const std::uint64_t SPeer_kInvalidPeer;

// NRadEngine::CNSLobby::HostPeer() const, VA 0x1a2d0b4; called through thunk 0x1027cf0 at site 0x133c300
// with this = lobby + 0x18.
class CNSLobby {
public:
    std::uint64_t HostPeer() const;
};

// NRadEngine::CSmallBlockAllocator::Alloc(ulonglong), VA 0x12dee84; called through thunk 0x101bc10 at
// sites 0x133c34c, 0x133c3a0, 0x133c3b8, 0x133c3d0, 0x133c3e8, 0x133c400, 0x133c418.
// Ghidra's prototype takes one argument in x0; every call site also sets w1 = 16 (arg1 here).
// Ghidra types the return as void; call sites use x0 as the returned pointer.
void* CSmallBlockAllocator_Alloc(void* allocator, std::uint32_t arg1);

namespace NRadGame {

class CR15NetGame;

// NRadEngine::NRadGame::CR15NetGame::Broadcast<NRadEngine::NRadGame::SR15NetBotActivateScenario>,
// VA 0x13d08c8; called through thunk 0x103bae0 at site 0x133c384.
void CR15NetGame_Broadcast_SR15NetBotActivateScenario(
    CR15NetGame* game, const void* message, const void* data, std::uint64_t size,
    std::uint64_t peer, std::uint64_t flags);

class CR15NetAIScenarios {
public:
    // EScenarioState is an enum whose values 0..6 are the switch cases. Ghidra types it undefined4.
    void EnableScenario(std::uint32_t state);

    // NRadEngine::NRadGame::CR15NetAIScenarios::Disable(), VA 0x133bde0; called through thunk
    // 0x100dbb0 at site 0x133c318.
    void Disable();
};

}  // namespace NRadGame
}  // namespace NRadEngine

// Scenario vtables. Each is read from the GOT slot named in its comment; the slot holds the vtable address.
extern const unsigned char CR15NetPatrolScenario_vtable[];        // slot 0x38043f0, state 0
extern const unsigned char CR15NetRandomAttackScenario_vtable[];  // slot 0x38275c0, state 1
extern const unsigned char CR15NetWeaponAttackScenario_vtable[];  // slot 0x38129e8, state 2
extern const unsigned char CR15NetSpikeScenario_vtable[];         // slot 0x380d050, state 3
extern const unsigned char CR15AIThrowTestScenario_vtable[];      // slot 0x380eb08, state 4
extern const unsigned char CR15AIOneVOneScenario_vtable[];        // slot 0x381a158, state 5
extern const unsigned char CR15AIBDebugArenaScenario_vtable[];    // slot 0x3828fe8, state 6

namespace {
std::uint64_t LoadU64(const void* base, std::size_t offset) {
    return *reinterpret_cast<const std::uint64_t*>(
        static_cast<const std::byte*>(base) + offset);
}
void StoreU32(void* base, std::size_t offset, std::uint32_t value) {
    *reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(base) + offset) = value;
}
void StoreU64(void* base, std::size_t offset, std::uint64_t value) {
    *reinterpret_cast<std::uint64_t*>(static_cast<std::byte*>(base) + offset) = value;
}
}  // namespace

namespace NRadEngine {
namespace NRadGame {

void CR15NetAIScenarios::EnableScenario(std::uint32_t state) {
    auto* const self = reinterpret_cast<std::byte*>(this);
    // 0x133c2c8: game = *(this + 0x558)
    void* const game = reinterpret_cast<void*>(LoadU64(self, 0x558));

    // 0x133c2d4..0x133c2ec: flags = **(u64**)(game + 0x2da0). The broadcast path runs when bit 1 of
    // flags is clear and (flags & 0x44) != 0x40.
    const std::uint64_t flags =
        LoadU64(reinterpret_cast<const void*>(LoadU64(game, 0x2da0)), 0);
    if (((flags >> 1) & 1) == 0 && (flags & 0x44) != 0x40) {
        // 0x133c2f0: local_28[0] = state; local_28[1] is never written.
        std::uint32_t local_28[2];
        local_28[0] = state;

        // 0x133c2f4..0x133c308 and 0x133c35c..0x133c364: lobby = *(game + 0x40);
        // peer = lobby ? CNSLobby::HostPeer(lobby + 0x18) : SPeer::kInvalidPeer.
        void* const lobby = reinterpret_cast<void*>(LoadU64(game, 0x40));
        std::uint64_t peer = 0;
        if (lobby == nullptr) {
            peer = SPeer_kInvalidPeer;
        } else {
            peer = reinterpret_cast<const CNSLobby*>(
                static_cast<std::byte*>(lobby) + 0x18)->HostPeer();
        }

        // 0x133c368..0x133c384: Broadcast(game, &local_28, NULL, 0, peer, ~0). The sixth argument is
        // loaded from a global (slot 0x3813e10) that the decompiler shows as 0xffffffffffffffff.
        CR15NetGame_Broadcast_SR15NetBotActivateScenario(
            reinterpret_cast<CR15NetGame*>(game), local_28, nullptr, 0, peer,
            0xffffffffffffffffULL);
        return;
    }

    // 0x133c30c..0x133c318: if (this->+0x560 != 0) Disable()
    if (LoadU64(self, 0x560) != 0) {
        Disable();
    }

    // 0x133c31c..0x133c320: this->+0x568 = state, stored before the range check.
    StoreU32(self, 0x568, state);

    // 0x133c31c..0x133c324: cmp state, #6; b.hi returns without touching +0x560 (LAB_0133c388).
    if (state > 6) {
        return;
    }

    std::byte* allocated = nullptr;
    const unsigned char* vtable = nullptr;
    // 0x133c328..0x133c340: jump table at 0x2ca3c28, byte offsets 00 15 1b 21 27 2d 33 for states 0..6.
    switch (state) {
    case 0:
        // 0x133c344..0x133c358
        allocated = static_cast<std::byte*>(CSmallBlockAllocator_Alloc(self + 0x508, 16));
        vtable = CR15NetPatrolScenario_vtable;
        break;
    case 1:
        // 0x133c398..0x133c3ac
        allocated = static_cast<std::byte*>(CSmallBlockAllocator_Alloc(self + 0x508, 16));
        vtable = CR15NetRandomAttackScenario_vtable;
        break;
    case 2:
        // 0x133c3b0..0x133c3c4
        allocated = static_cast<std::byte*>(CSmallBlockAllocator_Alloc(self + 0x508, 16));
        vtable = CR15NetWeaponAttackScenario_vtable;
        break;
    case 3:
        // 0x133c3c8..0x133c3dc
        allocated = static_cast<std::byte*>(CSmallBlockAllocator_Alloc(self + 0x508, 16));
        vtable = CR15NetSpikeScenario_vtable;
        break;
    case 4:
        // 0x133c3e0..0x133c3f4
        allocated = static_cast<std::byte*>(CSmallBlockAllocator_Alloc(self + 0x508, 16));
        vtable = CR15AIThrowTestScenario_vtable;
        break;
    case 5:
        // 0x133c3f8..0x133c40c
        allocated = static_cast<std::byte*>(CSmallBlockAllocator_Alloc(self + 0x508, 16));
        vtable = CR15AIOneVOneScenario_vtable;
        break;
    case 6:
        // 0x133c410..0x133c420, then the shared tail
        allocated = static_cast<std::byte*>(CSmallBlockAllocator_Alloc(self + 0x508, 16));
        vtable = CR15AIBDebugArenaScenario_vtable;
        break;
    }

    // 0x133c424..0x133c42c: allocated[0] = vtable + 0x10; allocated[1] = 0; this->+0x560 = allocated.
    StoreU64(allocated, 0, reinterpret_cast<std::uintptr_t>(vtable + 0x10));
    StoreU64(allocated, 8, 0);
    StoreU64(self, 0x560, reinterpret_cast<std::uintptr_t>(allocated));
}

}  // namespace NRadGame
}  // namespace NRadEngine
