// #600: the DOT FIELD script's hooks (runtime/combat/dot_script.h): the decisions, and both hooks installed through the
// script hook table under MinHook on a fake DOT script.
//
// The fake script has the real first bytes of PlaySound's Execute (0x3D10) and of next_node (0x10F30) at their RVAs and
// short bodies written here: Execute returns the dword at node+0x28 (the event it would play) and marks its context, read as the script's
// builders write it (a dword), with nonzero bytes on both sides of it in the node; next_node returns 0x9507DC91 for node
// 0x393F4ADA and 0x77 for any other.
//
// The scene-set half (#600, PLAN-v7 H-DOTF): the three scene-set nodes (FUN_18000CFE0, FUN_18000DA00, FUN_18000AC90) are
// rebuilt as N rows. Each fake node has its real 16-byte prologue and a `ret`; the helper at RVA 0x4270 has its real
// prologue and forwards to a test function, so the block each node passes is read back.
//
// NOT shown by these tests: the shipped script's bytes in a run (the PR's live run), and the sounds heard in a match.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <MinHook.h>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/combat/dot_script.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace ds = nevr_combat::dot_script;
namespace sh = nevr_script_hooks;

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

TEST(DotScriptDecision, OnlyTheDotsTwoEventsAndItsSetupNodeChange) {
  EXPECT_EQ(ds::Event(0xF7C92B80), 0x5B47A1CDu) << "play_grenade_stun_active -> play_grenade_stun_activate";
  EXPECT_EQ(ds::Event(0xC9CD3996), 0xBDE3209Du) << "stop_grenade_stun_active -> stop_grenade_stun";
  EXPECT_EQ(ds::Event(0x12345678), 0x12345678u);
  EXPECT_EQ(ds::NextNode(0x393f4ada, 0x9507dc91), 0x013166c6u);
  EXPECT_EQ(ds::NextNode(0x393f4ada, 0xffffffffffffffffULL), 0xffffffffffffffffULL) << "another port of the setup node";
  EXPECT_EQ(ds::NextNode(0x12345678, 0x9507dc91), 0x9507dc91u) << "another node";
  EXPECT_EQ(ds::NextNode(0x393f4ada, 0x19507dc91ULL), 0x19507dc91ULL) << "the whole result, not its low dword";
}

TEST(DotScriptDecision, TheFieldHideNodeContinuesToTheSoundStop) {
  EXPECT_EQ(ds::NextNode(ds::kHideNode, ds::kHideStockNext), ds::kSoundStop)
      << "the hide node (ARC_SCENE off) continues to the sound stop";
}

TEST(DotScriptDecision, TheHideRuleMatchesOnlyItsNodeAndStockResult) {
  constexpr std::uint64_t kUnrelatedNext = 0x12345;
  constexpr std::uint64_t kUnrelatedNode = 0x12345678ULL;
  EXPECT_EQ(ds::NextNode(ds::kHideNode, kUnrelatedNext), kUnrelatedNext) << "another result of the hide node is the game's";
  EXPECT_EQ(ds::NextNode(kUnrelatedNode, ds::kHideStockNext), ds::kHideStockNext)
      << "another node with the hide node's stock result is the game's";
}

// The field sound's node as the shipped script keys it, from an independent source: fix_dot_field.py LINKS[0] (site
// 0x12B4F) matches its stock result 0x1F2A71917, and the node it is reached from is `cmp rcx, 0xF2A71917` at 0x180012B01
// (PLAN-v7 line 1105, "H-DOTF adds (f2a71917 -> 0x9507DC91)"). The header's kSoundNode must equal it.
constexpr std::uint64_t kFieldSoundNodeInTheGame = 0xf2a71917ULL;

TEST(DotScriptDecision, TheFieldSoundLeadsToTheShowNodeAndTheShowNodeEnds) {
  EXPECT_EQ(ds::kSoundNode, kFieldSoundNodeInTheGame) << "the sound's node is 0xF2A71917 (PLAN-v7 line 1105), not f667d292";
  EXPECT_EQ(ds::NextNode(kFieldSoundNodeInTheGame, ds::kSoundStartNext), ds::kShowNode)
      << "the sound's start, keyed on the game's node, then the show node";
  EXPECT_EQ(ds::NextNode(ds::kSoundNode, ds::kSoundStartNext), ds::kShowNode) << "the sound's start, then the show node";
  EXPECT_EQ(ds::NextNode(ds::kShowNode, ds::kShowStockNext), ds::kChainEnd) << "the show node's stock end is -1";
  EXPECT_EQ(ds::NextNode(ds::kSoundNode, 0x12345), 0x12345u) << "another result of the sound node is the game's";
  EXPECT_EQ(ds::NextNode(ds::kShowNode, 0x12345), 0x12345u) << "another result of the show node is the game's";
  EXPECT_EQ(ds::NextNode(0x12345678, ds::kSoundStartNext), ds::kSoundStartNext)
      << "another node with the sound's result is the game's";
  EXPECT_EQ(ds::NextNode(0x12345678, ds::kShowStockNext), ds::kShowStockNext)
      << "another node with the show node's result is the game's";
}

TEST(DotScriptDecision, TheSceneSetBlockHoldsTheFieldPairAndTheFlag) {
  std::uint8_t context[0x100];
  for (std::size_t i = 0; i < sizeof context; ++i) context[i] = static_cast<std::uint8_t>(i);
  std::uint8_t block[ds::kBlockSize];
  std::memset(block, 0xee, sizeof block);
  ds::FillSceneSetBlock(block, context, ds::kArcScene, 1);
  std::uint64_t word = 0;
  std::memcpy(&word, block + 0x00, 8);
  EXPECT_EQ(word, 0u) << "the first word is zero";
  std::memcpy(&word, block + 0x08, 8);
  EXPECT_EQ(word, 0xb7b6b5b4b3b2b1b0ULL) << "the context's bytes at +0xB0 (the byte at i is i)";
  std::memcpy(&word, block + 0x10, 8);
  EXPECT_EQ(word, 0xbfbebdbcbbbab9b8ULL) << "the context's bytes at +0xB8";
  std::memcpy(&word, block + 0x18, 8);
  EXPECT_EQ(word, 0xc7c6c5c4c3c2c1c0ULL) << "the context's bytes at +0xC0";
  std::memcpy(&word, block + ds::kBlockComponent, 8);
  EXPECT_EQ(word, ds::kFieldComponent) << "the FIELD component";
  std::memcpy(&word, block + ds::kBlockScene, 8);
  EXPECT_EQ(word, ds::kArcScene) << "the scene is ARC_SCENE";
  EXPECT_EQ(block[ds::kBlockFlag], 1) << "the flag is the word's low byte";
  EXPECT_EQ(block[ds::kBlockFlag + 1], 0) << "and its high byte is zero";
  std::uint32_t tail = 0;
  std::memcpy(&tail, block + ds::kBlockTail, 4);
  EXPECT_EQ(tail, 0xFFu);
}

// What the helper was given: the block (copied as the game passes it) and the node.
struct HelperCall {
  std::uint8_t block[ds::kBlockSize] = {};
  std::uintptr_t node = 0;
  int calls = 0;
};
HelperCall g_helper;
std::uint64_t g_last_node = 0;

// The helper's result, distinct from 0 so a node that drops it (returns 0 or garbage) is seen.
constexpr std::uint64_t kHelperResult = 0x5a5a1234fedcba98ULL;

std::uint64_t FakeHelper(void* block, void* node) {
  std::memcpy(g_helper.block, block, sizeof g_helper.block);
  g_helper.node = reinterpret_cast<std::uintptr_t>(node);
  ++g_helper.calls;
  return kHelperResult;
}

class DotScriptInstalled : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    ds::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x12000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);
    std::vector<std::uint8_t> exec(ds::kPlaySoundPrologue, ds::kPlaySoundPrologue + sizeof ds::kPlaySoundPrologue);
    exec.insert(exec.end(), {0x8b, 0x41, 0x28,               // mov eax, dword [rcx+0x28]
                             0xc6, 0x42, 0x1f, 0x01,         // mov byte [rdx+0x1f], 1: the game's Execute ran
                             0x48, 0x83, 0xc4, 0x20, 0x5f, 0xc3});  // add rsp, 0x20; pop rdi; ret
    std::memcpy(base_ + ds::kPlaySoundExecuteRva, exec.data(), exec.size());
    std::vector<std::uint8_t> next(ds::kNextNodePrologue, ds::kNextNodePrologue + sizeof ds::kNextNodePrologue);
    next.insert(next.end(), {0x00, 0x00, 0x00, 0x00,                 // (jg +0)
                             0x48, 0x81, 0xf9, 0xda, 0x4a, 0x3f, 0x39,  // cmp rcx, 0x393f4ada
                             0x75, 0x06,                               // jne +6
                             0xb8, 0x91, 0xdc, 0x07, 0x95, 0xc3,       // mov eax, 0x9507dc91; ret
                             0xb8, 0x77, 0x00, 0x00, 0x00, 0xc3});     // mov eax, 0x77; ret
    std::memcpy(base_ + ds::kNextNodeRva, next.data(), next.size());
    WriteBody(ds::kShowSceneSetRva, ds::kShowSceneSetPrologue, sizeof ds::kShowSceneSetPrologue);
    WriteBody(ds::kHideSceneSetRva, ds::kHideSceneSetPrologue, sizeof ds::kHideSceneSetPrologue);
    WriteBody(ds::kBubbleSceneSetRva, ds::kBubbleSceneSetPrologue, sizeof ds::kBubbleSceneSetPrologue);
    // The helper: its real prologue, then a call of FakeHelper (the block and the node in rcx and rdx, as the game
    // passes them), then the prologue's epilogue.
    std::vector<std::uint8_t> helper(ds::kReadContextPrologue, ds::kReadContextPrologue + sizeof ds::kReadContextPrologue);
    const std::uint64_t target = reinterpret_cast<std::uintptr_t>(&::FakeHelper);
    helper.insert(helper.end(), {0x48, 0xb8});                      // mov rax, imm64
    for (int i = 0; i < 8; ++i) helper.push_back(static_cast<std::uint8_t>(target >> (8 * i)));
    helper.insert(helper.end(), {0xff, 0xd0,                        // call rax
                                 0x48, 0x83, 0xc4, 0x20, 0x5f, 0xc3});  // add rsp, 0x20; pop rdi; ret
    std::memcpy(base_ + ds::kReadContextVariablesRva, helper.data(), helper.size());
    g_helper = HelperCall{};
    g_last_node = 0;
  }
  void TearDown() override {
    MH_RemoveHook(base_ + ds::kPlaySoundExecuteRva);
    MH_RemoveHook(base_ + ds::kNextNodeRva);
    MH_RemoveHook(base_ + ds::kShowSceneSetRva);
    MH_RemoveHook(base_ + ds::kHideSceneSetRva);
    MH_RemoveHook(base_ + ds::kBubbleSceneSetRva);
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    ds::ResetForTest();
    SetBootLogSink(nullptr);
  }
  // A PlaySound node whose event is `event`, with nonzero bytes on both sides of the dword.
  std::uint64_t Play(std::uint32_t event, std::uint8_t* node) {
    std::memset(node, 0xa5, 0x40);
    std::memcpy(node + ds::kEventOffset, &event, sizeof event);
    std::uint8_t ctx[0x20] = {};  // +0x18 is 0: the script's "not aborting"
    const std::uint64_t r = reinterpret_cast<std::uint64_t (*)(void*, void*)>(base_ + ds::kPlaySoundExecuteRva)(node, ctx);
    EXPECT_EQ(ctx[0x1f], 1) << "the game's Execute ran";
    return r;
  }
  std::uint64_t Next(std::uint64_t node, std::uint64_t port) {
    return reinterpret_cast<std::uint64_t (*)(std::uint64_t, std::uint64_t)>(base_ + ds::kNextNodeRva)(node, port);
  }
  // A fake node: its real 16-byte prologue, then the rest of the store it begins (`mov dword [rsp+0x30], 0`) and a `ret`,
  // so the bytes decode. The stock body is never run by these tests (the rebuilt node replaces it).
  void WriteBody(std::uint32_t rva, const std::uint8_t* prologue, std::size_t len) {
    std::vector<std::uint8_t> code(prologue, prologue + len);
    code.insert(code.end(), {0x44, 0x24, 0x30, 0x00, 0x00, 0x00, 0x00, 0xc3});
    std::memcpy(base_ + rva, code.data(), code.size());
  }
  // Runs `fn` on a fake node whose object holds a context with 0x1111, 0x2222 and 0x3333 at +0xB0, +0xB8 and +0xC0;
  // returns what the node returned.
  std::uint64_t RunNodeWith(std::uint64_t (*fn)(std::uint64_t*)) {
    std::uint8_t context[0x100] = {};
    const std::uint64_t values[3] = {0x1111, 0x2222, 0x3333};
    std::memcpy(context + ds::kContextFirst, values, sizeof values);
    std::uint8_t object[0x80] = {};
    const std::uint8_t* context_ptr = context;
    std::memcpy(object + ds::kContextPointer, &context_ptr, sizeof context_ptr);
    const std::uint8_t* object_ptr = object;
    std::uint64_t node[1] = {reinterpret_cast<std::uint64_t>(object_ptr)};
    g_last_node = reinterpret_cast<std::uintptr_t>(node);
    return fn(node);
  }
  // Runs the installed node at `rva`.
  std::uint64_t RunNode(std::uint32_t rva) {
    return RunNodeWith(reinterpret_cast<std::uint64_t (*)(std::uint64_t*)>(base_ + rva));
  }

  std::uint8_t* base_ = nullptr;
};

TEST_F(DotScriptInstalled, TheFieldPlaysTheStunsSoundsAndSkipsTheSceneSets) {
  std::uint8_t node[0x40];
  EXPECT_EQ(Play(ds::kFieldOnEvent, node), ds::kFieldOnEvent) << "stock";
  EXPECT_EQ(Next(ds::kSetupNode, 0), ds::kFirstSceneSetNode) << "stock";
  ds::Register();
  sh::OnLoaded(ds::kScript, base_);
  EXPECT_EQ(Count("script hooks installed script=97d0806e6ea89130"), 1);
  EXPECT_EQ(Play(ds::kFieldOnEvent, node), ds::kStunActivate);
  EXPECT_EQ(node[ds::kEventOffset - 1], 0xa5) << "the byte before the event is untouched";
  EXPECT_EQ(node[ds::kEventOffset + 4], 0xa5) << "the byte after the event is untouched: it is a dword";
  EXPECT_EQ(Play(ds::kFieldStopEvent, node), ds::kStunStop);
  EXPECT_EQ(Play(0x11223344, node), 0x11223344u) << "another event plays as it is";
  EXPECT_EQ(Next(ds::kSetupNode, 0), ds::kAfterSceneSetNodes);
  EXPECT_EQ(Next(0x55, 0), 0x77u) << "another node's next is the game's";
  sh::OnUnloading(ds::kScript, base_);
  EXPECT_EQ(Play(ds::kFieldOnEvent, node), ds::kFieldOnEvent);
  EXPECT_EQ(Next(ds::kSetupNode, 0), ds::kFirstSceneSetNode);
}

TEST_F(DotScriptInstalled, RegisteringTwiceAddsTheRowsOnce) {
  ds::Register();
  ds::Register();
  sh::OnLoaded(ds::kScript, base_);
  EXPECT_EQ(Count("DOT FIELD fix registered"), 1);
  EXPECT_EQ(Count("hooks=5 slots=0 pins=1"), 1);
  EXPECT_EQ(Count("DOT FIELD fix registered script=97d0806e6ea89130 hooks=5 pins=1 "), 1)
      << "the DOT line's counts are computed from the row table: five detours, one pin";
}

TEST_F(DotScriptInstalled, TheSceneSetNodesLoadTheBubbleScene) {
  ds::Register();
  sh::OnLoaded(ds::kScript, base_);
  EXPECT_EQ(Count("hooks=5 slots=0 pins=1"), 1) << "the three nodes and the two stock rows are installed, the helper pinned";

  EXPECT_EQ(RunNode(ds::kShowSceneSetRva), kHelperResult) << "show returns the helper's result, as the stock node does";
  EXPECT_EQ(g_helper.calls, 1);
  EXPECT_EQ(g_last_node, g_helper.node) << "the helper gets the node the game passed";
  std::uint64_t word = 0;
  std::memcpy(&word, g_helper.block + ds::kBlockScene, 8);
  EXPECT_EQ(word, ds::kArcScene) << "show: ARC_SCENE";
  std::memcpy(&word, g_helper.block + ds::kBlockComponent, 8);
  EXPECT_EQ(word, ds::kFieldComponent);
  EXPECT_EQ(g_helper.block[ds::kBlockFlag], 1) << "show: flag on";
  std::memcpy(&word, g_helper.block + 0x08, 8);
  EXPECT_EQ(word, 0x1111u) << "the context's values come from the node's object";

  EXPECT_EQ(RunNode(ds::kHideSceneSetRva), kHelperResult) << "hide returns the helper's result";
  EXPECT_EQ(g_helper.calls, 2);
  std::memcpy(&word, g_helper.block + ds::kBlockScene, 8);
  EXPECT_EQ(word, ds::kArcScene) << "hide: the same scene, switched off";
  EXPECT_EQ(g_helper.block[ds::kBlockFlag], 0) << "hide: flag off";

  EXPECT_EQ(RunNode(ds::kBubbleSceneSetRva), kHelperResult) << "e94b7b9c returns the helper's result";
  EXPECT_EQ(g_helper.calls, 3);
  std::memcpy(&word, g_helper.block + ds::kBlockScene, 8);
  EXPECT_EQ(word, ds::kArcScene) << "e94b7b9c: stores ARC_SCENE (not the stock 678a.. pair)";
  EXPECT_EQ(g_helper.block[ds::kBlockFlag], 0) << "e94b7b9c: flag 0, as in his build (X2)";

  sh::OnUnloading(ds::kScript, base_);
  EXPECT_EQ(std::memcmp(base_ + ds::kShowSceneSetRva, ds::kShowSceneSetPrologue, sizeof ds::kShowSceneSetPrologue), 0)
      << "after unloading, the show node's stock bytes are back (its first 16 bytes are the prologue again)";
}

TEST_F(DotScriptInstalled, ARebuiltNodeWithoutItsModuleReturnsZeroAndCallsNothing) {
  ds::Register();
  sh::OnLoaded(ds::kScript, base_);
  sh::OnUnloading(ds::kScript, base_);  // the module is gone: LoadedBase is null
  EXPECT_EQ(RunNodeWith([](std::uint64_t* node) -> std::uint64_t { return ds::SceneSetNode(node, 1); }), 0u)
      << "no base: the node returns the game's success value 0, never an unset rax";
  EXPECT_EQ(g_helper.calls, 0) << "no base: the helper is not called";
}

TEST_F(DotScriptInstalled, AChangedHelperRefusesTheWholeScript) {
  base_[ds::kReadContextVariablesRva + 1] ^= 0xff;  // a byte of the helper's prologue differs
  ds::Register();
  sh::OnLoaded(ds::kScript, base_);
  EXPECT_EQ(Count("script hooks refused script=97d0806e6ea89130"), 1) << "one pin mismatch refuses every row";
  EXPECT_EQ(Count("script hooks installed"), 0);
  EXPECT_EQ(g_helper.calls, 0) << "the script runs stock: no rebuilt node is installed";
  EXPECT_EQ(std::memcmp(base_ + ds::kShowSceneSetRva, ds::kShowSceneSetPrologue, sizeof ds::kShowSceneSetPrologue), 0);
}

}  // namespace
