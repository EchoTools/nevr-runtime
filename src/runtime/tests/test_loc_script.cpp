// #2242 (H-LOC): the translocator's effect (runtime/combat/loc_script.h). The gates are pure, so the tests drive them on
// buffers: the token block (count, expiry, shown), the use counter, the teleport node's state and the disabled byte.
// The detonator guard is tested with a fake detonator image that holds the pinned bytes at its four RVAs; nothing in the
// image is executed. The two detours are not called here (they need the game's script to run); their wiring is in
// combat/loc_script.cpp and is checked by the byte pins below.
//
// NOT shown by these tests: the translocator's effect in a match, the teleport counter reaching the other clients, and the
// dispatcher hook installed in a run (no live run here). The LOC sound is not built (PLAN-v7 row 13).

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "runtime/combat/drone_script.h"
#include "runtime/combat/drone_script_pins.h"
#include "runtime/combat/loc_script.h"
#include "runtime/hook/script_hooks.h"

namespace loc = nevr_combat::loc_script;
namespace drone = nevr_combat::drone_script;
namespace pins = nevr_combat::drone_script::pins;

namespace {

// The values the tests pin, named so each literal says what it is. Each is checked against the stock build or the EC page
// by the tests that use it (the comments in loc_script.h name the sources).
constexpr std::uint64_t kExpectedScript = 0x9398a165c004a9c0ULL;  // the translocator script, 9398a165c004a9c0.dll
constexpr std::uint32_t kExpectedTeleportRva = 0x7040;           // FUN_180007040
constexpr std::uint32_t kExpectedDispatchRva = 0x5910;           // execute_single_node
constexpr std::uint32_t kExpectedTokenRva = 0x2104D20;           // the EC page's LOC token block
constexpr std::uint32_t kExpectedCounterRva = 0x2104D40;         // the EC page's LOC use counter
constexpr std::uint64_t kExpectedLocAsset = 0x4CE12AE42C1B6269ULL;  // pfx_wpn_combat_stun_field_pulse
constexpr float kExpectedScale = 2.0f;                           // the effect's size
constexpr float kExpectedLife = 2.5f;                            // the particle life
constexpr float kExpectedDuration = 2.5f;                        // seconds before the effect is killed
constexpr std::uint8_t kStockTeleportBytes[] = {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x08, 0x57};
constexpr std::uint8_t kStockDispatchBytes[] = {0x48, 0x8b, 0x41, 0x10, 0xba, 0x35, 0xee, 0x39, 0xe0};

constexpr std::size_t kTokenBytes = 0x20;           // the token block the gate tests write into
constexpr std::size_t kNodeBytes = 0x100;           // a node context: state at +0x18, disabled at +0xDB
constexpr std::size_t kFakeImageBytes = 0x4000;     // a detonator image big enough for its four RVAs
constexpr std::uint8_t kPoison = 0xCC;              // what a corrupted detonator byte holds
constexpr std::uint32_t kPoisonOffset = 0x10;       // a byte inside the spawn constructor
constexpr std::int32_t kNoTokens = 0;               // no token held
constexpr std::int32_t kNegativeCount = -1;         // a count below zero is not a token
constexpr std::int32_t kOneToken = 1;               // one token held
constexpr std::int32_t kTwoTokens = 2;              // two tokens held
constexpr std::uint64_t kExpiry = 1000;             // the token's expiry, interrupt time (100 ns)
constexpr std::uint64_t kBeforeExpiry = 999;        // one unit before it: still held
constexpr std::uint64_t kAtExpiry = 1000;           // at it: not held (the test is now < expiry)
constexpr std::uint64_t kEarly = 0;                 // a clock before any expiry
constexpr std::uint64_t kStateFirstRun = 0;         // the teleport node's state on its first run
constexpr std::uint64_t kStateRan = 1;              // the state after the node has run
constexpr std::uint8_t kEnabled = 0;                // the disabled byte when the node runs
constexpr std::uint8_t kDisabled = 1;               // the disabled byte when the node aborts
constexpr std::int32_t kCounterStart = 0;           // the use counter before a teleport
constexpr std::int32_t kCounterAfterOne = 1;        // after one teleport
constexpr std::int32_t kNoShown = 0;                // the shown count before a token is taken
constexpr std::int32_t kShownAfterOne = 1;          // after one token is taken
constexpr std::int32_t kShownAfterTwo = 2;          // after two
constexpr std::int32_t kCountEmpty = 0;             // the count after the held tokens are all taken
constexpr std::uint8_t kSentinel = 0xA5;            // fills the spawn fields the test checks were written

std::uint64_t U64(const std::uint8_t* at) {
    std::uint64_t v = 0;
    std::memcpy(&v, at, sizeof v);
    return v;
}
std::int32_t I32(const std::uint8_t* at) {
    std::int32_t v = 0;
    std::memcpy(&v, at, sizeof v);
    return v;
}
float F32(const std::uint8_t* at) {
    float v = 0;
    std::memcpy(&v, at, sizeof v);
    return v;
}
void Put32(std::uint8_t* at, std::int32_t v) { std::memcpy(at, &v, sizeof v); }
void Put64(std::uint8_t* at, std::uint64_t v) { std::memcpy(at, &v, sizeof v); }

// A token block with `count` tokens, `expiry` and nothing shown.
std::vector<std::uint8_t> Token(std::int32_t count, std::uint64_t expiry) {
    std::vector<std::uint8_t> token(kTokenBytes, 0);
    Put32(token.data() + loc::kTokenCount, count);
    Put64(token.data() + loc::kTokenExpiry, expiry);
    return token;
}

// A fake detonator image with the four pinned functions at their RVAs.
std::vector<std::uint8_t> DetonatorImage() {
    std::vector<std::uint8_t> image(kFakeImageBytes, 0);
    std::memcpy(image.data() + drone::kSpawnCtorRva, pins::kSpawnCtorPin, pins::kSpawnCtorPinLen);
    std::memcpy(image.data() + drone::kSpawnExecRva, pins::kSpawnExecPin, pins::kSpawnExecPinLen);
    std::memcpy(image.data() + drone::kSoundCtorRva, pins::kSoundCtorPin, pins::kSoundCtorPinLen);
    std::memcpy(image.data() + drone::kSoundExecRva, pins::kSoundExecPin, pins::kSoundExecPinLen);
    return image;
}

// A node context: enabled, with the given state.
std::vector<std::uint8_t> Node(std::uint64_t state, std::uint8_t disabled) {
    std::vector<std::uint8_t> node(kNodeBytes, 0);
    Put64(node.data() + loc::kNodeState, state);
    node[loc::kNodeDisabled] = disabled;
    return node;
}

class LocScriptTest : public ::testing::Test {
 protected:
    void SetUp() override { nevr_script_hooks::ResetForTest(); }
    // ResetForTest drops every recorded module, so no base pointer outlives the image it names.
    void TearDown() override { nevr_script_hooks::ResetForTest(); }
    // Records `image` as the loaded detonator (the table keeps the base only as a key; no rows, so nothing is installed).
    static void LoadDetonator(std::vector<std::uint8_t>& image) {
        nevr_script_hooks::OnLoaded(drone::kDetonatorScript, image.data());
    }
};

}  // namespace

// The bytes and fields the hooks are written against. A change to any of them is a change to the stock script or the
// EC page, and must be reviewed against the disassembly.
TEST(LocScriptPins, ScriptAndRvasAreTheStockOnes) {
    EXPECT_EQ(loc::kScript, kExpectedScript);
    EXPECT_EQ(loc::kTeleportRva, kExpectedTeleportRva);
    EXPECT_EQ(loc::kDispatchRva, kExpectedDispatchRva);
    EXPECT_EQ(loc::kTokenRva, kExpectedTokenRva);
    EXPECT_EQ(loc::kCounterRva, kExpectedCounterRva);
}

TEST(LocScriptPins, StockBytesAreTheOnesTheHooksReplace) {
    ASSERT_EQ(sizeof loc::kTeleportBytes, sizeof kStockTeleportBytes);
    ASSERT_EQ(sizeof loc::kDispatchBytes, sizeof kStockDispatchBytes);
    EXPECT_EQ(std::memcmp(loc::kTeleportBytes, kStockTeleportBytes, sizeof kStockTeleportBytes), 0);
    EXPECT_EQ(std::memcmp(loc::kDispatchBytes, kStockDispatchBytes, sizeof kStockDispatchBytes), 0);
}

TEST(LocScriptPins, EffectIsTheStunFieldPulseWithTheDefaultSizes) {
    EXPECT_EQ(loc::kLocFxAsset, kExpectedLocAsset);
    EXPECT_FLOAT_EQ(loc::kLocSpawn.scale, kExpectedScale);
    EXPECT_FLOAT_EQ(loc::kLocSpawn.life, kExpectedLife);
    EXPECT_FLOAT_EQ(loc::kLocSpawn.duration, kExpectedDuration);
    EXPECT_EQ(loc::kLocSpawn.asset, loc::kLocFxAsset);
}

// The count, expiry and shown fields: a held token is a count above 0 and a clock before the expiry.
TEST(LocScriptToken, HeldBeforeExpiryAndRefusedAtOrAfterIt) {
    auto token = Token(kOneToken, kExpiry);
    EXPECT_TRUE(loc::TokenHeld(token.data(), kBeforeExpiry));
    EXPECT_FALSE(loc::TokenHeld(token.data(), kAtExpiry));
}

TEST(LocScriptToken, NoTokenIsNotHeld) {
    auto none = Token(kNoTokens, kExpiry);
    auto negative = Token(kNegativeCount, kExpiry);
    EXPECT_FALSE(loc::TokenHeld(none.data(), kEarly));
    EXPECT_FALSE(loc::TokenHeld(negative.data(), kEarly));
    EXPECT_FALSE(loc::TokenHeld(nullptr, kEarly));
}

// Taking a token is once per token: the count goes down by one and the shown count up by one, and a second take of the
// same token does not happen while the count is empty.
TEST(LocScriptToken, TakeOnceAndShownGoesUp) {
    auto token = Token(kOneToken, kExpiry);
    ASSERT_TRUE(loc::TakeToken(token.data(), kEarly));
    EXPECT_EQ(I32(token.data() + loc::kTokenCount), kCountEmpty);
    EXPECT_EQ(I32(token.data() + loc::kTokenShown), kShownAfterOne);
    EXPECT_FALSE(loc::TakeToken(token.data(), kEarly));
    EXPECT_EQ(I32(token.data() + loc::kTokenCount), kCountEmpty);
    EXPECT_EQ(I32(token.data() + loc::kTokenShown), kShownAfterOne);
}

TEST(LocScriptToken, TwoTokensTakeTwoEffects) {
    auto token = Token(kTwoTokens, kExpiry);
    EXPECT_TRUE(loc::TakeToken(token.data(), kEarly));
    EXPECT_TRUE(loc::TakeToken(token.data(), kEarly));
    EXPECT_FALSE(loc::TakeToken(token.data(), kEarly));
    EXPECT_EQ(I32(token.data() + loc::kTokenCount), kCountEmpty);
    EXPECT_EQ(I32(token.data() + loc::kTokenShown), kShownAfterTwo);
}

// A token past its expiry is refused, and the refusal leaves the count and the shown count alone.
TEST(LocScriptToken, ExpiredTokenIsRefusedAndLeftAlone) {
    auto token = Token(kOneToken, kExpiry);
    EXPECT_FALSE(loc::TakeToken(token.data(), kAtExpiry));
    EXPECT_EQ(I32(token.data() + loc::kTokenCount), kOneToken);
    EXPECT_EQ(I32(token.data() + loc::kTokenShown), kNoShown);
}

TEST(LocScriptToken, NullTokenTakesNothing) {
    EXPECT_FALSE(loc::TakeToken(nullptr, kEarly));
}

// The teleport: the effect runs on the node's first run, on an enabled node only.
TEST(LocScriptTeleport, DueOnFirstRunOfAnEnabledNode) {
    auto node = Node(kStateFirstRun, kEnabled);
    EXPECT_TRUE(loc::TeleportEffectDue(node.data()));
}

TEST(LocScriptTeleport, NotDueAfterTheFirstRunOrWhenDisabledOrNull) {
    auto ran = Node(kStateRan, kEnabled);
    auto disabled = Node(kStateFirstRun, kDisabled);
    EXPECT_FALSE(loc::TeleportEffectDue(ran.data()));
    EXPECT_FALSE(loc::TeleportEffectDue(disabled.data()));
    EXPECT_FALSE(loc::TeleportEffectDue(nullptr));
}

// The counter: one teleport on this machine adds one, which VeilNet's watcher sends to the other clients.
TEST(LocScriptCounter, BumpAddsOneForOneTeleport) {
    std::int32_t counter = kCounterStart;
    loc::BumpTeleportCount(&counter);
    EXPECT_EQ(counter, kCounterAfterOne);
}

// The guard: the effect runs only with a token held, a pinned detonator loaded and an enabled node. A token is taken only
// when the effect runs, so a refused effect leaves the token for the next node.
TEST_F(LocScriptTest, ClaimRefusesWithoutADetonatorAndKeepsTheToken) {
    auto token = Token(kOneToken, kExpiry);
    auto node = Node(kStateFirstRun, kEnabled);
    EXPECT_FALSE(loc::ClaimEffect(node.data(), token.data(), kEarly));
    EXPECT_EQ(I32(token.data() + loc::kTokenCount), kOneToken);
    EXPECT_EQ(I32(token.data() + loc::kTokenShown), kNoShown);
}

TEST_F(LocScriptTest, ClaimRefusesAnUnpinnedDetonatorAndKeepsTheToken) {
    auto image = DetonatorImage();
    image[drone::kSpawnCtorRva + kPoisonOffset] = kPoison;
    LoadDetonator(image);
    auto token = Token(kOneToken, kExpiry);
    auto node = Node(kStateFirstRun, kEnabled);
    EXPECT_FALSE(loc::ClaimEffect(node.data(), token.data(), kEarly));
    EXPECT_EQ(I32(token.data() + loc::kTokenCount), kOneToken);
}

TEST_F(LocScriptTest, ClaimRefusesADisabledContextAndKeepsTheToken) {
    auto image = DetonatorImage();
    LoadDetonator(image);
    auto token = Token(kOneToken, kExpiry);
    auto node = Node(kStateFirstRun, kDisabled);
    EXPECT_FALSE(loc::ClaimEffect(node.data(), token.data(), kEarly));
    EXPECT_EQ(I32(token.data() + loc::kTokenCount), kOneToken);
}

TEST_F(LocScriptTest, ClaimRefusesWithNoTokenHeld) {
    auto image = DetonatorImage();
    LoadDetonator(image);
    auto token = Token(kNoTokens, kExpiry);
    auto node = Node(kStateFirstRun, kEnabled);
    EXPECT_FALSE(loc::ClaimEffect(node.data(), token.data(), kEarly));
    EXPECT_EQ(I32(token.data() + loc::kTokenShown), kNoShown);
}

TEST_F(LocScriptTest, ClaimTakesTheTokenWhenThePinnedDetonatorAndTheNodeAreReady) {
    auto image = DetonatorImage();
    LoadDetonator(image);
    auto token = Token(kOneToken, kExpiry);
    auto node = Node(kStateFirstRun, kEnabled);
    EXPECT_TRUE(loc::ClaimEffect(node.data(), token.data(), kEarly));
    EXPECT_EQ(I32(token.data() + loc::kTokenCount), kCountEmpty);
    EXPECT_EQ(I32(token.data() + loc::kTokenShown), kShownAfterOne);
}

// The spawn the translocator uses carries its own asset, size and life (the stock spawn stub's fields, drone_script.h).
TEST(LocScriptSpawn, BuildSpawnNodeWritesTheLocEffect) {
    std::vector<std::uint8_t> node(drone::kNodeBufferBytes, kSentinel);
    drone::BuildSpawnNode(node.data(), loc::kLocSpawn);
    EXPECT_EQ(U64(node.data() + drone::kSpawnAsset), loc::kLocFxAsset);
    EXPECT_FLOAT_EQ(F32(node.data() + drone::kSpawnLife), loc::kLocFxLife);
    EXPECT_FLOAT_EQ(F32(node.data() + drone::kSpawnDuration), loc::kLocFxDuration);
    EXPECT_FLOAT_EQ(F32(node.data() + drone::kSpawnParticle[0]), loc::kLocFxScale);
}
