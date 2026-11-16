// #2242: the drone's death explosion from the stock drone script. See drone_script.h for what this module does and why.

#include "runtime/combat/drone_script.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "runtime/combat/drone_script_pins.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::drone_script {
namespace {

constexpr std::size_t kRingEntries = 3;          // the cloud's offset: three dwords, one point (ring_offsets(1, 0.0))
constexpr std::uint32_t kRingStride = 4;         // bytes per dword
constexpr std::uint16_t kRefPointFlags = 0x0101;  // refpoint and leavebehind, the two bytes at +0x40
constexpr float kUnit = 1.0f;                    // emission, alpha, colour: the stock values the veil effect sets
constexpr std::size_t kQwordBytes = 8;           // a qword field

// The state of the drone's own death, shared by the dispatcher wrapper and the stubs.
std::uint8_t* g_token = nullptr;                 // the exe's DEATH_TOKEN_RVA block, set once at Register
std::atomic<bool> g_warned_unpinned{false};      // the Warning about a detonator that is not the pinned build, once

template <typename T>
void Store(std::uint8_t* at, std::uint32_t off, const T& value) {
    std::memcpy(at + off, &value, sizeof value);
}

template <typename T>
T Load(const std::uint8_t* at, std::uint32_t off) {
    T value{};
    std::memcpy(&value, at + off, sizeof value);
    return value;
}

using DetonatorCtor = void (*)(void* node);
using DetonatorExec = std::int64_t (*)(void* node, void* ctx);

// Runs one of the detonator's functions at its RVA from its base (the base is checked by DetonatorPinned first).
template <typename Fn>
Fn DetonatorFn(const std::uint8_t* det, std::uint32_t rva) {
    return reinterpret_cast<Fn>(const_cast<std::uint8_t*>(det) + rva);
}

// The guard both stubs share: the detonator is loaded, its four functions are the pinned bytes, and the context is
// enabled. A loaded detonator that is not the pinned build is logged once and not called.
const std::uint8_t* ReadyDetonator(const std::uint8_t* ctx) {
    if (!ContextEnabled(ctx)) return nullptr;
    const auto* det = static_cast<const std::uint8_t*>(nevr_script_hooks::LoadedBase(kDetonatorScript));
    if (det == nullptr) return nullptr;
    if (DetonatorPinned(det)) return det;
    bool expected = false;
    if (g_warned_unpinned.compare_exchange_strong(expected, true)) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.COMBAT] drone explosion not run: the detonator script (190ed14fe3da5ed1) is loaded but its node "
            "functions are not the bytes this build was written against, so the stubs call none of them");
    }
    return nullptr;
}

}  // namespace

void RecordDispatch(std::uint8_t* token, void* ctx) {
    Store(token, kTokenLastCtx, ctx);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(token + kTokenNodesRun));
}

void BuildSpawnNode(std::uint8_t* node, float life, float scale, float duration) {
    node[kSpawnDurationOn] = duration > 0.0f ? 1 : 0;
    Store(node, kSpawnDuration, duration);
    node[kSpawnKillAtEnd] = 1;
    Store(node, kSpawnAsset, kDeathAsset);
    Store(node, kSpawnActor, kSelfSymbol);
    Store(node, kSpawnRefPoint, kRefPointFlags);
    Store(node, kSpawnEmission, kUnit);
    Store(node, kSpawnLife, life);
    Store(node, kSpawnAlpha, kUnit);
    Store(node, kSpawnColorR, kUnit);
    Store(node, kSpawnColorG, kUnit);
    Store(node, kSpawnColorB, kUnit);
    for (std::uint32_t off : kSpawnParticle) Store(node, off, scale);
    for (std::uint32_t off : kSpawnScale) Store(node, off, scale);
    for (std::size_t k = 0; k < kRingEntries; ++k) Store(node, kSpawnRing + static_cast<std::uint32_t>(k * kRingStride), 0u);
}

void BuildSoundNode(std::uint8_t* node, std::uint32_t event) {
    Store(node, kSoundActor, kSelfSymbol);
    Store(node, kSoundEvent, event);
}

bool PinnedBytesMatch(const void* at, const std::uint8_t* pin, std::size_t len) {
    return at != nullptr && std::memcmp(at, pin, len) == 0;
}

bool DetonatorPinned(const std::uint8_t* base) {
    if (base == nullptr) return false;
    return PinnedBytesMatch(base + kSpawnCtorRva, pins::kSpawnCtorPin, pins::kSpawnCtorPinLen) &&
           PinnedBytesMatch(base + kSpawnExecRva, pins::kSpawnExecPin, pins::kSpawnExecPinLen) &&
           PinnedBytesMatch(base + kSoundCtorRva, pins::kSoundCtorPin, pins::kSoundCtorPinLen) &&
           PinnedBytesMatch(base + kSoundExecRva, pins::kSoundExecPin, pins::kSoundExecPinLen);
}

bool ContextEnabled(const std::uint8_t* ctx) {
    return ctx != nullptr && ctx[kCtxDisabled] == 0;
}

}  // namespace nevr_combat::drone_script

// The dispatcher wrapper. The hook overwrites the stock script's bytes at kDispatchRva with a jump here, so this runs with
// the node context in rcx and the stack as the stock dispatcher was entered. It records the context and then jumps to the
// trampoline the hook table made for the displaced instructions (nevr_drone_dispatch_original), which runs them and
// continues in the stock dispatcher. Every register it changes is restored; the stack is the entry stack at the jump.
extern "C" void nevr_drone_dispatch_stub();
extern "C" {
void* nevr_drone_dispatch_original = nullptr;
}

extern "C" void NevrDroneDispatchRecord(void* ctx) {
    nevr_combat::drone_script::RecordDispatch(nevr_combat::drone_script::g_token, ctx);
}

// The stub's frame. The asm below pushes kStubPushes registers after rbp, aligns rsp to kStubStackAlign, reserves
// kStubShadowBytes (the callee's home space), and restores rsp with `lea rsp, [rbp-kStubFrameBytes]`. The numbers written in
// the asm are these constants; the static_asserts after the asm fail if the two drift apart. A change to the push list
// must change kStubPushes and the asm together.
constexpr std::size_t kStubPushes = 7;        // rcx, rdx, r8, r9, r10, r11, rax
constexpr std::size_t kStubStackAlign = 16;   // `and rsp, -16` (the mask is -kStubStackAlign)
constexpr std::size_t kStubShadowBytes = 32;  // `sub rsp, 32`
constexpr std::size_t kStubSlotBytes = sizeof(std::uint64_t);  // one push
constexpr std::size_t kStubFrameBytes = kStubPushes * kStubSlotBytes;  // bytes between rbp and the last push (`lea rsp, [rbp-56]`)

// The wrapper saves the integer registers it changes and no xmm registers. The stock dispatcher is entered from callers
// that pass only rcx (execute_node_block and follow_node_chain, revault), so no float argument is live at the hook, and
// NevrDroneDispatchRecord is integer-only (it stores a pointer and adds to a dword), so it clobbers no xmm register. If a
// caller ever passes a float, or the recorder takes one, this stub must save xmm0-xmm5 too.
asm(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl nevr_drone_dispatch_stub\n"
    "nevr_drone_dispatch_stub:\n"
    "  push rbp\n"
    "  mov rbp, rsp\n"
    "  push rcx\n"
    "  push rdx\n"
    "  push r8\n"
    "  push r9\n"
    "  push r10\n"
    "  push r11\n"
    "  push rax\n"
    "  and rsp, -16\n"
    "  sub rsp, 32\n"
    "  call NevrDroneDispatchRecord\n"
    "  lea rsp, [rbp-56]\n"
    "  pop rax\n"
    "  pop r11\n"
    "  pop r10\n"
    "  pop r9\n"
    "  pop r8\n"
    "  pop rdx\n"
    "  pop rcx\n"
    "  pop rbp\n"
    "  jmp qword ptr [rip + nevr_drone_dispatch_original]\n"
    ".att_syntax prefix\n");

static_assert(kStubPushes * kStubSlotBytes == kStubFrameBytes, "the frame is the pushes");
static_assert(kStubFrameBytes == 56, "the asm's `lea rsp, [rbp-56]` is kStubFrameBytes");
static_assert(kStubStackAlign == 16 && kStubShadowBytes == 32, "the asm's `and rsp, -16` and `sub rsp, 32`");

namespace nevr_combat::drone_script {

void __fastcall SpawnDeathEffect(void* ctx) {
    const std::uint8_t* det = ReadyDetonator(static_cast<const std::uint8_t*>(ctx));
    if (det == nullptr) return;
    alignas(16) std::uint8_t node[kNodeBufferBytes] = {};
    DetonatorFn<DetonatorCtor>(det, kSpawnCtorRva)(node);
    BuildSpawnNode(node, kDeathLife, kDeathScale, kDeathDuration);
    // Each explosion starts its cloud fresh (the stock spawn stub clears the context's state the same way).
    std::uint8_t* state = static_cast<std::uint8_t*>(ctx) + kCtxState;
    std::memset(state, 0, kQwordBytes);
    DetonatorFn<DetonatorExec>(det, kSpawnExecRva)(node, ctx);
}

void __fastcall PlayDeathSound(void* ctx) {
    const std::uint8_t* det = ReadyDetonator(static_cast<const std::uint8_t*>(ctx));
    if (det == nullptr) return;
    alignas(16) std::uint8_t node[kNodeBufferBytes] = {};
    DetonatorFn<DetonatorCtor>(det, kSoundCtorRva)(node);
    BuildSoundNode(node, kPointSoundEvent);
    DetonatorFn<DetonatorExec>(det, kSoundExecRva)(node, ctx);
}

#ifdef NEVR_TEST_HOOKS
void SetTokenForTest(std::uint8_t* token) {
    g_token = token;
}
#endif

void Register() {
    auto* exe = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (exe == nullptr) return;
    g_token = exe + kTokenRva;
    // DroneFix calls these through the token (SpawnDeathFx). The stub code lives as long as the process does.
    void* spawn = reinterpret_cast<void*>(&SpawnDeathEffect);
    void* sound = reinterpret_cast<void*>(&PlayDeathSound);
    Store(g_token, kTokenSpawnStub, spawn);
    Store(g_token, kTokenSoundStub, sound);
    RegisterHooks();
}

void RegisterHooks() {
    static const nevr_script_hooks::Hook kRows[] = {
        {kScript, kDispatchRva, kDispatchBytes, sizeof kDispatchBytes, reinterpret_cast<void*>(&nevr_drone_dispatch_stub),
         &nevr_drone_dispatch_original, "drone script dispatcher: death token context"},
    };
    nevr_script_hooks::Register(kRows, sizeof kRows / sizeof kRows[0]);
}

}  // namespace nevr_combat::drone_script
