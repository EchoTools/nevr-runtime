// CR15VRMenuWidgetCS::UpdateExclusive
// Quest libr15.so VA 0x2099c68; ReVault binary 576.
// ReVault raw parent 100297616, reconstruction node 100600458; grade M: every branch and the five-entry jump table checked against the bytes; callee identities are Ghidra thunk-target names, not byte-confirmed (GOT words read 0x00ecfb40 in three slots).
// @0x2099c68 — CR15VRMenuWidgetCS::UpdateExclusive
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// NRadEngine::CVR::ShowSystemUI(), VA 0x1b73990, called through thunk 0x1047880.
// Declared static: the callee's adrp/add overwrite x0 before any read.
struct CVR {
    static void ShowSystemUI();
};

// NRadEngine::CncaGameSpace::GetST(), VA 0x18ff2fc, called through thunk 0xfe2f50.
// The body is a bare `ret`: GetST returns this.
class CncaGameSpace {
public:
    CncaGameSpace* GetST();
};

namespace NRadGame {

// NRadEngine::NRadGame::CR15Game members. Each callee receives x0 = *(st + 0x8).
class CR15Game {
public:
    void QuitToMain();                   // VA 0x1300854, thunk 0xfd5870
    void RequestUnpause();               // VA 0x1300734, thunk 0x10058e0
    void BeginMultiplayerTutorial();     // VA 0x13013f4, thunk 0xfd3b70
    void BeginMultiplayerNPETutorial();  // VA 0x1301930, thunk 0x1020260
};

class CR15VRMenuWidgetCS {
public:
    void UpdateExclusive();              // VA 0x2099c68
};

namespace {

// Raw byte-offset access: the layout of CR15VRMenuWidgetCS is not recovered.
void* LoadPtr(const void* base, std::size_t offset) {
    void* value = nullptr;
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

std::uint64_t LoadU64(const void* base, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

void StoreU64(void* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof value);
}

constexpr std::size_t kGameSpaceOffset = 0x80;   // *(this + 0x80), passed to GetST
constexpr std::size_t kActionOffset = 0x1c8;     // 64-bit action selector
constexpr std::size_t kReceiverOffset = 0x8;     // *(st + 0x8), receiver of the case call
constexpr std::uint64_t kActionDone = ~std::uint64_t{0};  // -1, stored on every exit

}  // namespace

// Extent 0x2099c68..0x2099d00 (the ret). Jump table: bytes 00 05 0a 0f 14 at VA 0x32b4b72;
// case k goes to 0x2099ca0 + 4 * byte[k], i.e. 0x2099ca0, 0x2099cb4, 0x2099cc8, 0x2099cdc, 0x2099cf0.
void CR15VRMenuWidgetCS::UpdateExclusive() {
    // 0x2099c74: bl GetST(*(this + 0x80)); taken on every path.
    CncaGameSpace* const st =
        static_cast<CncaGameSpace*>(LoadPtr(this, kGameSpaceOffset))->GetST();

    // 0x2099c78..0x2099c80: action = *(this + 0x1c8); b.hi is an unsigned compare with 4,
    // so any value above 4 skips the dispatch and goes straight to the final store.
    const std::uint64_t action = LoadU64(this, kActionOffset);
    if (action <= 4) {
        // 0x2099c84: x0 = *(st + 0x8); every case passes this same value.
        void* const receiver = LoadPtr(st, kReceiverOffset);
        switch (action) {
        case 0:  // 0x2099ca0: bl ShowSystemUI; then the final store (break).
            CVR::ShowSystemUI();
            break;
        case 1:  // 0x2099cb4
            static_cast<CR15Game*>(receiver)->QuitToMain();
            StoreU64(this, kActionOffset, kActionDone);
            return;
        case 2:  // 0x2099cc8
            static_cast<CR15Game*>(receiver)->RequestUnpause();
            StoreU64(this, kActionOffset, kActionDone);
            return;
        case 3:  // 0x2099cdc
            static_cast<CR15Game*>(receiver)->BeginMultiplayerTutorial();
            StoreU64(this, kActionOffset, kActionDone);
            return;
        case 4:  // 0x2099cf0
            static_cast<CR15Game*>(receiver)->BeginMultiplayerNPETutorial();
            StoreU64(this, kActionOffset, kActionDone);
            return;
        }
    }
    // 0x2099ca4: default and case 0 end here.
    StoreU64(this, kActionOffset, kActionDone);
}

}  // namespace NRadGame
}  // namespace NRadEngine
