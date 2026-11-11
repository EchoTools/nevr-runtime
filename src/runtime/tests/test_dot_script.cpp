// #600: the DOT FIELD script's hooks (runtime/combat/dot_script.h): the decisions, and both hooks installed through the
// script hook table under MinHook on a fake DOT script.
//
// The fake script has the real first bytes of PlaySound's Execute (0x3D10) and of next_node (0x10F30) at their RVAs and
// short bodies written here: Execute returns the dword at node+0x28 (the event it would play) and marks its context, read as the script's
// builders write it (a dword), with nonzero bytes on both sides of it in the node; next_node returns 0x9507DC91 for node
// 0x393F4ADA and 0x77 for any other.
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
  }
  void TearDown() override {
    MH_RemoveHook(base_ + ds::kPlaySoundExecuteRva);
    MH_RemoveHook(base_ + ds::kNextNodeRva);
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
  EXPECT_EQ(Count("hooks=2 slots=0"), 1);
}

}  // namespace
