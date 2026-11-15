// #2242: the drone's death explosion from the stock drone script (runtime/combat/drone_script.h): the dispatcher's record,
// the two stubs' node layout, and the pins. The layout tests run the detonator's own constructors (the pinned stock
// bytes, in executable memory), then the stub's writes, and check the node: so a field the stub writes is one the
// stock constructor is shown to leave where the stub expects it.
//
// NOT shown by these tests: the detonator's Execute running in a match, the explosion or its sound, and the dispatcher
// hook installed in a run (no live run here).

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "runtime/combat/drone_script.h"
#include "runtime/combat/drone_script_pins.h"

namespace drone = nevr_combat::drone_script;
namespace pins = nevr_combat::drone_script::pins;

namespace {

// The values under test, named so each literal in a check says what it is.
constexpr std::size_t kTokenBytes = 0x40;            // the token block the record tests write into
constexpr std::uint8_t kFill = 0xA5;                  // what the untouched bytes hold
constexpr std::uint8_t kPoison = 0xCC;                // what the node holds before the constructor runs
constexpr std::uint32_t kZeroCount = 0;               // the node count's start
constexpr std::uint32_t kFirstCount = 1;              // after one dispatch
constexpr std::uint32_t kSecondCount = 2;            // after two dispatches
constexpr std::uint32_t kCountBytes = 4;             // a dword field
constexpr std::uint32_t kCtxBytes = 8;               // a qword field
constexpr std::uint32_t kRingStride = 4;             // a ring entry is a dword
constexpr std::size_t kRingEntries = 3;              // the cloud's offset: three dwords
constexpr std::uint16_t kExpectedRefPoint = 0x0101;  // refpoint and leavebehind
constexpr int kFakeResult = 119;                     // the fake trampoline's return value
constexpr std::uint64_t kCallerR10 = 0x1010101010101010ULL;  // the caller's r10, which the stub must hand on intact
constexpr std::uint64_t kCallerR11 = 0x1111111111111111ULL;  // the caller's r11, likewise
constexpr std::size_t kFakeImageBytes = 0x4000;      // a detonator image big enough for its four RVAs
constexpr std::uint32_t kFlipOffset = 0x10;          // a byte inside the spawn Execute
constexpr std::uint8_t kFlipMask = 0xFF;             // flips every bit of that byte
constexpr std::uint32_t kStockDispatchRva = 0x5D74;  // the stock dispatcher, RVA in the drone script
constexpr std::uint32_t kStockTokenRva = 0x2104D60;  // DroneFix's DEATH_TOKEN_RVA, in echovr.exe
constexpr std::uint32_t kStockTokenCtxOffset = 0x20;  // +0x20 the context (drone_script.h kTokenLastCtx)
constexpr std::uint32_t kStockTokenCountOffset = 0x18;  // +0x18 the node count (drone_script.h kTokenNodesRun)
constexpr std::uint32_t kStockTokenSpawnOffset = 0x28;  // +0x28 the spawn stub (drone_script.h kTokenSpawnStub)
constexpr std::uint32_t kStockTokenSoundOffset = 0x30;  // +0x30 the sound stub (drone_script.h kTokenSoundStub)

std::uint64_t U64(const std::uint8_t* at) {
    std::uint64_t v = 0;
    std::memcpy(&v, at, sizeof v);
    return v;
}
std::uint32_t U32(const std::uint8_t* at) {
    std::uint32_t v = 0;
    std::memcpy(&v, at, sizeof v);
    return v;
}
std::uint16_t U16(const std::uint8_t* at) {
    std::uint16_t v = 0;
    std::memcpy(&v, at, sizeof v);
    return v;
}
float F32(const std::uint8_t* at) {
    float v = 0;
    std::memcpy(&v, at, sizeof v);
    return v;
}

// Copies a pinned function into executable memory and returns its entry point.
void* Executable(const std::uint8_t* code, std::size_t len) {
    void* mem = VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (mem != nullptr) std::memcpy(mem, code, len);
    return mem;
}

using NodeCtor = void (*)(void*);

}  // namespace

// The dispatcher wrapper, through an assembly caller and a fake trampoline: the caller sets r10 and r11 (which a C
// caller would not see preserved), calls the wrapper with rcx and rdx; the fake trampoline records what it was given.
extern "C" std::int64_t nevr_test_call_stub(void* ctx, void* arg2);
extern "C" std::uint64_t g_test_in_r10;
extern "C" std::uint64_t g_test_in_r11;
extern "C" std::uint64_t g_test_seen_rcx;
extern "C" std::uint64_t g_test_seen_rdx;
extern "C" std::uint64_t g_test_seen_r10;
extern "C" std::uint64_t g_test_seen_r11;
extern "C" std::uint64_t g_test_seen_ret;
extern "C" void nevr_test_call_return();
extern "C" void* nevr_drone_dispatch_original;
extern "C" void nevr_drone_dispatch_stub();

// The caller's frame: five callee-saved pushes and a 32-byte shadow area keep the stack 16-byte aligned at the call.
// kCallerPushes and kCallerShadowBytes are the numbers in the asm below; the static_assert ties them together.
constexpr std::size_t kCallerPushes = 5;
constexpr std::size_t kCallerShadowBytes = 32;
constexpr std::size_t kStackSlot = 8;      // a push
constexpr std::size_t kStackAlign = 16;    // the call's alignment
// The entry to nevr_test_call_stub leaves rsp 8 mod 16 (the return address); after the pushes and the shadow area
// it must be 0 mod 16 at the call.
static_assert((kCallerPushes * kStackSlot + kCallerShadowBytes + kStackSlot) % kStackAlign == 0,
              "nevr_test_call_stub pushes and shadow space keep the call 16-byte aligned");
// kFakeResult is the immediate the fake trampoline returns (mov eax, 119 in the asm above).
static_assert(kFakeResult == 119, "the fake trampoline's immediate");

asm(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl nevr_test_call_stub\n"
    "nevr_test_call_stub:\n"
    "  push rbx\n"
    "  push rsi\n"
    "  push rdi\n"
    "  push r12\n"
    "  push r13\n"
    "  sub rsp, 32\n"
    "  mov r10, qword ptr [rip + g_test_in_r10]\n"
    "  mov r11, qword ptr [rip + g_test_in_r11]\n"
    "  call nevr_drone_dispatch_stub\n"
    "nevr_test_call_return:\n"
    "  add rsp, 32\n"
    "  pop r13\n"
    "  pop r12\n"
    "  pop rdi\n"
    "  pop rsi\n"
    "  pop rbx\n"
    "  ret\n"
    ".globl nevr_test_fake_original\n"
    "nevr_test_fake_original:\n"
    "  mov qword ptr [rip + g_test_seen_rcx], rcx\n"
    "  mov qword ptr [rip + g_test_seen_rdx], rdx\n"
    "  mov qword ptr [rip + g_test_seen_r10], r10\n"
    "  mov qword ptr [rip + g_test_seen_r11], r11\n"
    "  mov rax, qword ptr [rsp]\n"
    "  mov qword ptr [rip + g_test_seen_ret], rax\n"
    "  mov eax, 119\n"
    "  ret\n"
    ".att_syntax prefix\n");

extern "C" {
std::uint64_t g_test_in_r10 = 0;
std::uint64_t g_test_in_r11 = 0;
std::uint64_t g_test_seen_rcx = 0;
std::uint64_t g_test_seen_rdx = 0;
std::uint64_t g_test_seen_r10 = 0;
std::uint64_t g_test_seen_r11 = 0;
std::uint64_t g_test_seen_ret = 0;
void nevr_test_fake_original();
}

TEST(DroneScript, DispatchBytesAreTheStockOnes) {
    // revault, echovr project, 221a0a3a2e56d247.dll at 0x180005D74: 48 8b 41 10 ba 48 46 5c ca
    const std::uint8_t stock[] = {0x48, 0x8b, 0x41, 0x10, 0xba, 0x48, 0x46, 0x5c, 0xca};
    ASSERT_EQ(sizeof stock, sizeof drone::kDispatchBytes);
    EXPECT_EQ(0, std::memcmp(stock, drone::kDispatchBytes, sizeof stock));
    EXPECT_EQ(kStockDispatchRva, drone::kDispatchRva);
}

TEST(DroneScript, TokenFieldsAreDroneFixsOffsets) {
    // dronefix.cpp reads the context at +0x20 and the stubs at +0x28 and +0x30 of DEATH_TOKEN_RVA
    EXPECT_EQ(kStockTokenRva, drone::kTokenRva);
    EXPECT_EQ(kStockTokenCtxOffset, drone::kTokenLastCtx);
    EXPECT_EQ(kStockTokenSpawnOffset, drone::kTokenSpawnStub);
    EXPECT_EQ(kStockTokenSoundOffset, drone::kTokenSoundStub);
    EXPECT_EQ(kStockTokenCountOffset, drone::kTokenNodesRun);
}

TEST(DroneScript, RecordDispatchStoresTheContextAndCountsTheNode) {
    std::uint8_t token[kTokenBytes];
    std::memset(token, kFill, sizeof token);
    std::memcpy(token + drone::kTokenNodesRun, &kZeroCount, kCountBytes);  // the node count starts at 0
    int ctx_storage = 0;
    drone::RecordDispatch(token, &ctx_storage);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&ctx_storage), static_cast<std::uintptr_t>(U64(token + drone::kTokenLastCtx)));
    EXPECT_EQ(kFirstCount, U32(token + drone::kTokenNodesRun));
    drone::RecordDispatch(token, &ctx_storage);
    EXPECT_EQ(kSecondCount, U32(token + drone::kTokenNodesRun));
}

TEST(DroneScript, RecordDispatchTouchesNothingElse) {
    std::uint8_t token[kTokenBytes];
    std::memset(token, kFill, sizeof token);
    int ctx_storage = 0;
    drone::RecordDispatch(token, &ctx_storage);
    for (std::size_t i = 0; i < sizeof token; ++i) {
        const bool count = i >= drone::kTokenNodesRun && i < drone::kTokenNodesRun + kCountBytes;
        const bool ctx = i >= drone::kTokenLastCtx && i < drone::kTokenLastCtx + kCtxBytes;
        if (!count && !ctx) {
            EXPECT_EQ(kFill, token[i]) << "byte " << i;
        }
    }
}

TEST(DroneScript, SpawnNodeAfterTheStockConstructorHasTheStubsFields) {
    void* code = Executable(pins::kSpawnCtorPin, pins::kSpawnCtorPinLen);
    ASSERT_NE(nullptr, code);
    alignas(16) std::uint8_t node[drone::kNodeBufferBytes];
    std::memset(node, kPoison, sizeof node);
    reinterpret_cast<NodeCtor>(code)(node);
    drone::BuildSpawnNode(node, drone::kDeathLife, drone::kDeathScale, drone::kDeathDuration);

    EXPECT_EQ(drone::kDeathAsset, U64(node + drone::kSpawnAsset));
    EXPECT_EQ(1u, node[drone::kSpawnDurationOn]);
    EXPECT_EQ(drone::kDeathDuration, F32(node + drone::kSpawnDuration));
    EXPECT_EQ(1u, node[drone::kSpawnKillAtEnd]);
    EXPECT_EQ(drone::kSelfSymbol, U64(node + drone::kSpawnActor));
    EXPECT_EQ(kExpectedRefPoint, U16(node + drone::kSpawnRefPoint));
    EXPECT_EQ(1.0f, F32(node + drone::kSpawnEmission));
    EXPECT_EQ(drone::kDeathLife, F32(node + drone::kSpawnLife));
    EXPECT_EQ(1.0f, F32(node + drone::kSpawnAlpha));
    EXPECT_EQ(1.0f, F32(node + drone::kSpawnColorR));
    EXPECT_EQ(1.0f, F32(node + drone::kSpawnColorG));
    EXPECT_EQ(1.0f, F32(node + drone::kSpawnColorB));
    for (std::uint32_t off : drone::kSpawnParticle) EXPECT_EQ(drone::kDeathScale, F32(node + off));
    for (std::uint32_t off : drone::kSpawnScale) EXPECT_EQ(drone::kDeathScale, F32(node + off));
    for (std::size_t k = 0; k < kRingEntries; ++k) {
        EXPECT_EQ(0u, U32(node + drone::kSpawnRing + kRingStride * static_cast<std::uint32_t>(k)));
    }
    VirtualFree(code, 0, MEM_RELEASE);
}

TEST(DroneScript, SoundNodeAfterTheStockConstructorHasTheStubsFields) {
    void* code = Executable(pins::kSoundCtorPin, pins::kSoundCtorPinLen);
    ASSERT_NE(nullptr, code);
    alignas(16) std::uint8_t node[drone::kNodeBufferBytes];
    std::memset(node, kPoison, sizeof node);
    reinterpret_cast<NodeCtor>(code)(node);
    drone::BuildSoundNode(node, drone::kPointSoundEvent);

    EXPECT_EQ(drone::kSelfSymbol, U64(node + drone::kSoundActor));
    EXPECT_EQ(drone::kPointSoundEvent, U32(node + drone::kSoundEvent));
    VirtualFree(code, 0, MEM_RELEASE);
}

TEST(DroneScript, PinnedBytesMatchExactlyOrNot) {
    std::uint8_t a[] = {1, 2, 3, 4};
    std::uint8_t pin[] = {1, 2, 3, 4};
    EXPECT_TRUE(drone::PinnedBytesMatch(a, pin, sizeof a));
    a[2] = 9;
    EXPECT_FALSE(drone::PinnedBytesMatch(a, pin, sizeof a));
}

namespace {
// A detonator image with its four pinned functions at their stock RVAs, zero elsewhere.
std::vector<std::uint8_t> DetonatorImage() {
    std::vector<std::uint8_t> img(kFakeImageBytes, 0);
    std::memcpy(img.data() + drone::kSpawnCtorRva, pins::kSpawnCtorPin, pins::kSpawnCtorPinLen);
    std::memcpy(img.data() + drone::kSpawnExecRva, pins::kSpawnExecPin, pins::kSpawnExecPinLen);
    std::memcpy(img.data() + drone::kSoundCtorRva, pins::kSoundCtorPin, pins::kSoundCtorPinLen);
    std::memcpy(img.data() + drone::kSoundExecRva, pins::kSoundExecPin, pins::kSoundExecPinLen);
    return img;
}
}  // namespace

TEST(DroneScript, DetonatorPinsHoldOnlyForTheStockBytes) {
    std::vector<std::uint8_t> img = DetonatorImage();
    EXPECT_TRUE(drone::DetonatorPinned(img.data()));
    EXPECT_FALSE(drone::DetonatorPinned(nullptr));
    img[drone::kSpawnExecRva + kFlipOffset] ^= kFlipMask;  // one byte of the spawn Execute
    EXPECT_FALSE(drone::DetonatorPinned(img.data()));
}

TEST(DroneScript, ContextEnabledReadsTheDisabledByte) {
    std::uint8_t ctx[drone::kNodeBufferBytes] = {};
    EXPECT_TRUE(drone::ContextEnabled(ctx));
    ctx[drone::kCtxDisabled] = 1;
    EXPECT_FALSE(drone::ContextEnabled(ctx));
}

TEST(DroneScript, DispatchWrapperRecordsAndHandsTheStockDispatcherItsRegisters) {
    std::uint8_t token[kTokenBytes];
    std::memset(token, 0, sizeof token);
    drone::SetTokenForTest(token);
    nevr_drone_dispatch_original = reinterpret_cast<void*>(&nevr_test_fake_original);
    int ctx_storage = 0;
    int arg2_storage = 0;
    g_test_in_r10 = kCallerR10;
    g_test_in_r11 = kCallerR11;
    const std::int64_t result = nevr_test_call_stub(&ctx_storage, &arg2_storage);

    const auto ctx_value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&ctx_storage));
    EXPECT_EQ(ctx_value, g_test_seen_rcx);
    EXPECT_EQ(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&arg2_storage)), g_test_seen_rdx);
    EXPECT_EQ(g_test_in_r10, g_test_seen_r10);
    EXPECT_EQ(g_test_in_r11, g_test_seen_r11);
    // The trampoline is entered by a jump, so its return address is the caller's, not a stub's.
    EXPECT_EQ(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&nevr_test_call_return)), g_test_seen_ret);
    EXPECT_EQ(kFakeResult, result);
    EXPECT_EQ(ctx_value, static_cast<std::uint64_t>(U64(token + drone::kTokenLastCtx)));
    EXPECT_EQ(kFirstCount, U32(token + drone::kTokenNodesRun));
    drone::SetTokenForTest(nullptr);
}
