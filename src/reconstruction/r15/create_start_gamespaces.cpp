// CR15Game::CreateStartGameSpaces
// Quest libr15.so VA 0x12f59e8; ReVault binary 576.
// ReVault raw parent 100221261, reconstruction node 100600465; grade M: both branches and every store checked against the disassembly; vtable slots +0x100/+0x338/+0x340 and the two GOT-held values are unresolved, SLevelLoad is raw offsets.
// @0x12f59e8 — CR15Game::CreateStartGameSpaces
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

// Image data this function reads. The file holds UINT64_MAX at each of the three.
extern const std::uint64_t g_3832078;  // @0x03832078
extern const std::uint64_t g_3832080;  // @0x03832080
extern const std::uint64_t g_3832088;  // @0x03832088

// Pointer slots. The file holds 0 in both, so the value they hold is set when the image loads.
extern const std::byte* const g_slot_0380cb28;  // @0x0380cb28: points at a record, u64 at +0x00 and u32 at +0x08
extern const float* const g_slot_0381c0f8;      // @0x0381c0f8: points at the float delta

namespace NRadEngine {

// NRadEngine::SLevelLoad, by its use as the parameter of CncaGame::RequireGameSpace. Its size and
// fields are not recovered: this function writes the first 0x38 bytes by offset.
struct SLevelLoad {
    std::byte bytes[0x38];
};

namespace NWriteLog {
// NRadEngine::NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...)
// @0x01088c20; called through the stub at 0x0104b960.
void WriteLog(std::int32_t level, std::uint64_t category, const char* format, ...);
}  // namespace NWriteLog

// NRadEngine::CncaGame::RequireGameSpace(SLevelLoad const&, unsigned int), receiver first.
// @0x018d3e88; called through the stub at 0x0104b030.
void CncaGame_RequireGameSpace(std::byte* game, const SLevelLoad* level_load, std::uint32_t flag);

namespace NRadGame {

class CR15Game {
public:
    // CR15Game::CreateStartGameSpaces() @0x012f59e8.
    int CreateStartGameSpaces();
    // NRadEngine::NRadGame::CR15Game::WaitOnLevelLoads(unsigned int) @0x012f5c54; stub 0x0100d700.
    void WaitOnLevelLoads(std::uint32_t flag);
};

// Vtable slots of CR15Game, read through the object's vptr at +0x00. ReVault recovers no vtable
// for this class, so the slots are UNRESOLVED; each type is what the call site passes.
// UNRESOLVED: vtable slot +0x100; x0 = this, the int result is returned.
using VSlot100Fn = int (*)(CR15Game* self);
// UNRESOLVED: vtable slot +0x338; x0 = this, x1 = SLevelLoad const*, w2 = int, s0 = float. Result not read.
using VSlot338Fn = void (*)(CR15Game* self, const SLevelLoad* level_load, std::int32_t flag, float delta);
// UNRESOLVED: vtable slot +0x340; x0 = this, w1 = int, x2 = SLevelLoad const*, w3 = int, s0 = float. Result is a pointer.
using VSlot340Fn = void* (*)(CR15Game* self, std::int32_t mode, const SLevelLoad* level_load, std::int32_t flag, float delta);

}  // namespace NRadGame
}  // namespace NRadEngine

namespace {

std::byte* LoadPtr(const std::byte* base, std::size_t offset) {
    std::byte* value = nullptr;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::uint32_t LoadU32(const std::byte* base, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

void StoreU32(std::byte* base, std::size_t offset, std::uint32_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

// Reads the function address held in a vtable slot.
template <typename Fn>
Fn LoadSlot(const std::byte* vtable, std::size_t offset) {
    return reinterpret_cast<Fn>(reinterpret_cast<std::uintptr_t>(LoadPtr(vtable, offset)));
}

}  // namespace

namespace NRadEngine::NRadGame {

int CR15Game::CreateStartGameSpaces() {
    auto* const self = reinterpret_cast<std::byte*>(this);
    // 0x012f5a18: WriteLog(2, 0, "Creating start gamespaces"); the string is at 0x02ca07d2.
    NWriteLog::WriteLog(2, 0, "Creating start gamespaces");
    // 0x012f5a1c..0x012f5a2c: this->+0x7ad8 |= 0x100.
    StoreU64(self, 0x7ad8, LoadU64(self, 0x7ad8) | 0x100);

    // 0x012f5a20..0x012f5a3c: existing = vtable +0x100 (this); a nonzero result is returned as is.
    const int existing = LoadSlot<VSlot100Fn>(LoadPtr(self, 0), 0x100)(this);
    if (existing != 0) {
        return existing;
    }

    // 0x012f5a40..0x012f5a64: delta is read from the GOT-held float; this->+0x7b0c = 3.
    const float delta = *g_slot_0381c0f8;
    StoreU32(self, 0x7b0c, 3);
    // 0x012f5a5c: the pointer the records below read their words from.
    const std::byte* const k = g_slot_0380cb28;

    // 0x012f5a68..0x012f5a9c: first record, +0x28 and +0x30 zeroed, then the 0xff fill of its +0x30 word
    // through the stub at 0x01047300 (Fill at 0x012cafcc, which tail-jumps through the thunk at 0x00fe71f0
    // to the import memset, GOT slot 0x037cafe0).
    SLevelLoad level_1{};
    StoreU64(level_1.bytes, 0x00, g_3832078);
    StoreU64(level_1.bytes, 0x08, ~std::uint64_t{0});
    StoreU64(level_1.bytes, 0x10, ~std::uint64_t{0});
    StoreU64(level_1.bytes, 0x18, LoadU64(k, 0));
    StoreU32(level_1.bytes, 0x20, LoadU32(k, 8));
    StoreU32(level_1.bytes, 0x24, 2);
    StoreU64(level_1.bytes, 0x28, 0);
    StoreU64(level_1.bytes, 0x30, 0);
    std::memset(level_1.bytes + 0x30, 0xff, 4);
    // 0x012f5aa0..0x012f5ab8: vtable +0x338(this, &level_1, 1, delta).
    LoadSlot<VSlot338Fn>(LoadPtr(self, 0), 0x338)(this, &level_1, 1, delta);

    // 0x012f5abc..0x012f5b00: same record shape with g_3832080, then vtable +0x338(this, &level_2, 1, delta).
    SLevelLoad level_2{};
    StoreU64(level_2.bytes, 0x00, g_3832080);
    StoreU64(level_2.bytes, 0x08, ~std::uint64_t{0});
    StoreU64(level_2.bytes, 0x10, ~std::uint64_t{0});
    StoreU64(level_2.bytes, 0x18, LoadU64(k, 0));
    StoreU32(level_2.bytes, 0x20, LoadU32(k, 8));
    StoreU32(level_2.bytes, 0x24, 2);
    StoreU64(level_2.bytes, 0x28, 0);
    StoreU64(level_2.bytes, 0x30, 0);
    std::memset(level_2.bytes + 0x30, 0xff, 4);
    LoadSlot<VSlot338Fn>(LoadPtr(self, 0), 0x338)(this, &level_2, 1, delta);

    // 0x012f5b04..0x012f5b48: third record. Words +0x00, +0x18 and +0x20 come from this+0x948, +0x950 and
    // +0x958; the -1 and *k values stored there first are overwritten before the call and are not kept.
    SLevelLoad level_3{};
    StoreU64(level_3.bytes, 0x00, LoadU64(self, 0x948));
    StoreU64(level_3.bytes, 0x08, ~std::uint64_t{0});
    StoreU64(level_3.bytes, 0x10, ~std::uint64_t{0});
    StoreU64(level_3.bytes, 0x18, LoadU64(self, 0x950));
    StoreU32(level_3.bytes, 0x20, LoadU32(self, 0x958));
    StoreU32(level_3.bytes, 0x24, 2);
    StoreU64(level_3.bytes, 0x28, 0);
    StoreU64(level_3.bytes, 0x30, 0);
    std::memset(level_3.bytes + 0x30, 0xff, 4);
    // 0x012f5b48..0x012f5b64: vtable +0x340(this, 1, &level_3, 1, delta); the result is not read.
    LoadSlot<VSlot340Fn>(LoadPtr(self, 0), 0x340)(this, 1, &level_3, 1, delta);

    // 0x012f5b68..0x012f5b9c: fourth record. +0x30 gets the 0xff fill and is then zeroed. Its first
    // word is written with g_3832088 at 0x012f5ba0 (the -1 stored before that is not kept).
    SLevelLoad level_4{};
    StoreU64(level_4.bytes, 0x08, ~std::uint64_t{0});
    StoreU64(level_4.bytes, 0x10, ~std::uint64_t{0});
    StoreU64(level_4.bytes, 0x18, LoadU64(k, 0));
    StoreU32(level_4.bytes, 0x20, LoadU32(k, 8));
    StoreU32(level_4.bytes, 0x24, 2);
    StoreU64(level_4.bytes, 0x28, 0);
    StoreU64(level_4.bytes, 0x30, 0);
    std::memset(level_4.bytes + 0x30, 0xff, 4);
    std::memset(level_4.bytes + 0x30, 0, 4);
    StoreU64(level_4.bytes, 0x00, g_3832088);
    // 0x012f5ba0..0x012f5bcc: created = vtable +0x340(this, 4, &level_4, 1, -1.0f).
    auto* const created = static_cast<std::byte*>(
        LoadSlot<VSlot340Fn>(LoadPtr(self, 0), 0x340)(this, 4, &level_4, 1, -1.0f));

    // 0x012f5bd0..0x012f5be0: this->+0x7af0 = created; created->+0x10 = the word at this->+0x7af8.
    StoreU64(self, 0x7af0, reinterpret_cast<std::uintptr_t>(created));
    StoreU64(created, 0x10, LoadU64(self, 0x7af8));

    // 0x012f5be4..0x012f5c04: fifth record (g_3832088 first word), +0x30 gets the 0xff fill.
    SLevelLoad level_5{};
    StoreU64(level_5.bytes, 0x00, g_3832088);
    StoreU64(level_5.bytes, 0x08, ~std::uint64_t{0});
    StoreU64(level_5.bytes, 0x10, ~std::uint64_t{0});
    StoreU64(level_5.bytes, 0x18, LoadU64(k, 0));
    StoreU32(level_5.bytes, 0x20, LoadU32(k, 8));
    StoreU32(level_5.bytes, 0x24, 2);
    StoreU64(level_5.bytes, 0x28, 0);
    StoreU64(level_5.bytes, 0x30, 0);
    std::memset(level_5.bytes + 0x30, 0xff, 4);
    // 0x012f5c08..0x012f5c14: CncaGame::RequireGameSpace(this, &level_5, 1) through the stub at 0x0104b030.
    CncaGame_RequireGameSpace(self, &level_5, 1);
    // 0x012f5c18..0x012f5c20: CR15Game::WaitOnLevelLoads(1) through the stub at 0x0100d700.
    WaitOnLevelLoads(1);
    // 0x012f5c24: return 0; the existing result was 0 on this path.
    return 0;
}

}  // namespace NRadEngine::NRadGame
