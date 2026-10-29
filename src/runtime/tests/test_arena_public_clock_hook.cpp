// #982: the arena_public_clock hook on the real stock script DLL, under MinHook: the site classifies as shipped,
// a pass hooks it, a second pass leaves it, a call through the hooked bind function reaches the detour (the decision
// is applied to the state before the original runs), and a release and reload of the DLL is hooked again.
//
// Needs the stock script DLLs: NEVR_SCRIPT_DLL_DIR names the directory that holds 08317e89cc3ae611.dll and
// fc4cbaae440e658c.dll (a read-only install's bin/win10/scripts). The test loads them and writes nothing to disk.
// Without the variable the tests skip and say so.
//
// #1113: the tests of ArenaPublicClockTrampoline need no script DLL. They stand in for the two MinHook calls and
// check that a caller who reaches the detour never finds a null or a freed trampoline: while a hook is attached,
// when the attach fails, and after the old hook is removed.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <MinHook.h>

#include <vector>

#include "core/hook_lifecycle.h"
#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/lifecycle/arena_round_time.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/patch/arena_public_clock.h"
#include "runtime/patch/arena_public_clock_rules.h"

BOOL g_isServer = TRUE;  // the runtime defines it in cli.cpp; the detour reads it

namespace {

using namespace nevr_arena_public_clock;

struct Recorded {
  int calls{0};
  void* state{nullptr};
  void* node{nullptr};
  std::uint8_t valueSeen{0xFF};
};
Recorded g_recorded;

std::int64_t Recorder(void* state, void* node) {
  ++g_recorded.calls;
  g_recorded.state = state;
  g_recorded.node = node;
  g_recorded.valueSeen = static_cast<std::uint8_t*>(state)[kStateValueOffset];
  return 7;
}

std::wstring ScriptPath(const char* dll) {
  const char* dir = std::getenv("NEVR_SCRIPT_DLL_DIR");
  if (dir == nullptr || *dir == '\0') return L"";
  std::string path = dir;
  if (path[0] == '/') path = "Z:" + path;  // Wine's view of the Linux path
  for (char& c : path) if (c == '/') c = '\\';
  path += std::string("\\") + dll;
  return std::wstring(path.begin(), path.end());
}

class ArenaPublicClockHook : public ::testing::Test {
 protected:
  void SetUp() override {
    if (ScriptPath(kSites[0].dll).empty()) GTEST_SKIP() << "NEVR_SCRIPT_DLL_DIR is not set";
    ASSERT_TRUE(Hooking::Initialize());
    g_recorded = Recorded{};
  }
  static HMODULE Load(std::size_t site) { return LoadLibraryW(ScriptPath(kSites[site].dll).c_str()); }
  static std::uint8_t* Bind(HMODULE module, std::size_t site) {
    return reinterpret_cast<std::uint8_t*>(module) + kSites[site].bindRva;
  }
  static std::array<std::uint8_t, 0x38> State(std::uint8_t value) {
    std::array<std::uint8_t, 0x38> state{};
    const std::uint64_t id = kPublicMatchFlag;
    std::memcpy(state.data() + kStateNameOffset, &id, sizeof(id));
    state[kStateValueOffset] = value;
    return state;
  }
  // Calls the (hooked) bind function the way a script node does.
  static void CallBind(HMODULE module, std::size_t site, std::array<std::uint8_t, 0x38>& state) {
    auto fn = reinterpret_cast<BindFn>(Bind(module, site));
    EXPECT_EQ(fn(state.data(), reinterpret_cast<void*>(0x1234)), 7);
  }
};

TEST_F(ArenaPublicClockHook, ThePassHooksEachLoadedScriptAndASecondPassLeavesIt) {
  for (std::size_t site = 0; site < kSiteCount; ++site) {
    HMODULE module = Load(site);
    ASSERT_NE(module, nullptr) << kSites[site].dll << " error=" << GetLastError();
    const auto* image = reinterpret_cast<const std::uint8_t*>(module);
    EXPECT_EQ(ClassifySite(image, 0x100000, kSites[site]), SiteState::Unhooked) << kSites[site].dll;

    RefreshForTest();
    EXPECT_EQ(Bind(module, site)[0], 0xE9) << kSites[site].dll << " not hooked";
    EXPECT_EQ(ClassifySite(image, 0x100000, kSites[site]), SiteState::Hooked);
    const BindFn first = OriginalForTest(site);
    EXPECT_NE(first, nullptr);

    RefreshForTest();  // idempotent
    EXPECT_EQ(OriginalForTest(site), first) << "a second pass hooked again";
  }
}

TEST_F(ArenaPublicClockHook, AHookedCallForcesThePublicFlagFalseBeforeTheOriginalRuns) {
  nevr_arena_round_time::SetSessionGameType("echo_arena");
  for (std::size_t site = 0; site < kSiteCount; ++site) {
    HMODULE module = Load(site);
    ASSERT_NE(module, nullptr);
    RefreshForTest();
    SetOriginalForTest(site, &Recorder);
    auto state = State(1);
    CallBind(module, site, state);
    EXPECT_EQ(g_recorded.calls, 1) << kSites[site].dll;
    EXPECT_EQ(g_recorded.valueSeen, 0) << "the original saw the unforced value";
    EXPECT_EQ(g_recorded.node, reinterpret_cast<void*>(0x1234));
    g_recorded = Recorded{};
  }
}

TEST_F(ArenaPublicClockHook, PrivateArenaAndOtherFlagsReachTheOriginalUnchanged) {
  HMODULE module = Load(0);
  ASSERT_NE(module, nullptr);
  RefreshForTest();
  SetOriginalForTest(0, &Recorder);

  nevr_arena_round_time::SetSessionGameType("echo_arena_private");
  auto privateState = State(1);
  CallBind(module, 0, privateState);
  EXPECT_EQ(g_recorded.valueSeen, 1);

  nevr_arena_round_time::SetSessionGameType("echo_arena");
  auto otherFlag = State(1);
  const std::uint64_t other = kPublicMatchFlag ^ 0x10;
  std::memcpy(otherFlag.data() + kStateNameOffset, &other, sizeof(other));
  CallBind(module, 0, otherFlag);
  EXPECT_EQ(g_recorded.valueSeen, 1);
}

TEST_F(ArenaPublicClockHook, AReloadedScriptIsHookedAgain) {
  HMODULE first = Load(0);
  ASSERT_NE(first, nullptr);
  RefreshForTest();
  ASSERT_EQ(Bind(first, 0)[0], 0xE9);

  // Earlier tests in this process loaded the script too: release every reference, as the engine does when its
  // script resource is released, until the image is gone.
  const std::wstring path = ScriptPath(kSites[0].dll);
  while (GetModuleHandleW(path.substr(path.rfind(L'\\') + 1).c_str()) != nullptr) ASSERT_TRUE(FreeLibrary(first));
  HMODULE second = Load(0);
  ASSERT_NE(second, nullptr);
  const auto* image = reinterpret_cast<const std::uint8_t*>(second);
  EXPECT_EQ(ClassifySite(image, 0x100000, kSites[0]), SiteState::Unhooked) << "the reload is a fresh image";

  RefreshForTest();
  EXPECT_EQ(Bind(second, 0)[0], 0xE9) << "the reloaded script was not hooked again";
  nevr_arena_round_time::SetSessionGameType("echo_arena");
  SetOriginalForTest(0, &Recorder);
  auto state = State(1);
  CallBind(second, 0, state);
  EXPECT_EQ(g_recorded.valueSeen, 0);
}

// ---- #1113 ----------------------------------------------------------------------------------------------------

// What the stand-ins record. `Game*` is the game's own bind function (the hook target), `Trampoline` the copy MinHook
// would have made; `freed` models MH_RemoveHook freeing the trampoline.
struct Probe {
  int gameACalls{0};
  int gameBCalls{0};
  int trampolineCalls{0};
  int freedTrampolineCalls{0};
  bool freed{false};
  // attach
  void* attachEntry{nullptr};     // what *original held when attach was called: the target, as Hooking::Attach reads it
  bool attachSucceeds{true};
  bool callerBeforeCreate{false};  // a caller arrives at the start of the attach, before the trampoline exists
  bool callerAtEnable{false};      // a caller arrives as the hook goes live
  std::vector<std::int64_t> results;  // what each of those callers got back
  // remove
  std::vector<void*> removed;
  int removeStatus{MH_OK};
};
Probe g_probe;

std::int64_t GameA(void*, void*) { ++g_probe.gameACalls; return 11; }
std::int64_t GameB(void*, void*) { ++g_probe.gameBCalls; return 12; }
std::int64_t Trampoline(void*, void*) {
  if (g_probe.freed) ++g_probe.freedTrampolineCalls;
  ++g_probe.trampolineCalls;
  return 7;
}

// The caller: a script node reaching the bind function through the detour, with a flag store it does not care about.
void CallDetour(std::size_t site) {
  std::array<std::uint8_t, 0x38> state{};
  const std::uint64_t other = kPublicMatchFlag ^ 0x10;
  std::memcpy(state.data() + kStateNameOffset, &other, sizeof(other));
  g_probe.results.push_back(DetourForTest(site)(state.data(), nullptr));
}

// Hooking::Attach over a MinHook that can be told to fail: the same create / publish / enable order and rollback
// (core/hook_lifecycle.h), with the target read from *original.
bool ProbeAttach(void** original, void* /*detour*/) {
  g_probe.attachEntry = *original;
  if (g_probe.callerBeforeCreate) CallDetour(0);
  g_probe.freed = false;
  const nevr::hook::AttachStage stage = nevr::hook::AttachPublished(
      original,
      [&](void** trampoline) {
        *trampoline = reinterpret_cast<void*>(&Trampoline);
        return true;
      },
      [&] {
        if (g_probe.callerAtEnable) CallDetour(0);
        return g_probe.attachSucceeds;
      },
      [] {});
  return stage == nevr::hook::AttachStage::kAttached;
}

int ProbeRemove(void* target) {
  g_probe.removed.push_back(target);
  if (g_probe.removeStatus == MH_OK) g_probe.freed = true;  // the old trampoline is gone
  return g_probe.removeStatus;
}

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }

bool Logged(const char* needle) {
  for (const std::string& line : g_logged) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

class ArenaPublicClockTrampoline : public ::testing::Test {
 protected:
  void SetUp() override {
    g_probe = Probe{};
    g_logged.clear();
    ResetForTest();
    SetBackendForTest(&ProbeAttach, &ProbeRemove);
    SetBootLogSink(&CaptureLog);
  }
  void TearDown() override {
    SetBootLogSink(nullptr);
    ResetForTest();
  }
};

TEST_F(ArenaPublicClockTrampoline, ACallerThatArrivesAsTheHookGoesLiveReachesTheTrampoline) {
  g_probe.callerAtEnable = true;
  ASSERT_TRUE(HookSiteForTest(0, reinterpret_cast<void*>(&GameA)));
  EXPECT_EQ(g_probe.attachEntry, reinterpret_cast<void*>(&GameA)) << "attach was not handed the target to hook";
  ASSERT_EQ(g_probe.results.size(), 1u);
  EXPECT_EQ(g_probe.results[0], 7) << "the caller at enable did not reach the trampoline";
  EXPECT_EQ(g_probe.trampolineCalls, 1);
  EXPECT_EQ(g_probe.gameACalls, 0);

  CallDetour(0);
  EXPECT_EQ(g_probe.results.back(), 7);
  EXPECT_EQ(OriginalForTest(0), &Trampoline);
}

TEST_F(ArenaPublicClockTrampoline, AFailedAttachLeavesTheDetourCallingTheGamesFunction) {
  g_probe.callerAtEnable = true;
  g_probe.attachSucceeds = false;
  EXPECT_FALSE(HookSiteForTest(0, reinterpret_cast<void*>(&GameA)));
  ASSERT_EQ(g_probe.results.size(), 1u);
  EXPECT_EQ(g_probe.results[0], 7);  // it arrived while the trampoline was published

  CallDetour(0);  // the attach rolled back: the hook is gone, the game's function is whole
  EXPECT_EQ(g_probe.results.back(), 11) << "after a failed attach the detour did not reach the game's function";
  EXPECT_EQ(g_probe.gameACalls, 1);
  EXPECT_EQ(g_probe.trampolineCalls, 1) << "the rolled-back trampoline was called";
}

TEST_F(ArenaPublicClockTrampoline, ARemovedHooksTrampolineIsNeverCalled) {
  ASSERT_TRUE(HookSiteForTest(0, reinterpret_cast<void*>(&GameA)));
  ASSERT_EQ(OriginalForTest(0), &Trampoline);

  // The script is released and mapped again: the old hook is removed (its trampoline freed), then the new image is
  // hooked. A caller that reaches the detour between the two must reach a real function.
  g_probe.callerBeforeCreate = true;
  ASSERT_TRUE(HookSiteForTest(0, reinterpret_cast<void*>(&GameB)));
  ASSERT_EQ(g_probe.removed.size(), 1u);
  EXPECT_EQ(g_probe.removed[0], reinterpret_cast<void*>(&GameA));
  ASSERT_EQ(g_probe.results.size(), 1u);
  EXPECT_EQ(g_probe.freedTrampolineCalls, 0) << "the detour called a freed trampoline";
  EXPECT_EQ(g_probe.results[0], 12) << "between the removal and the new attach the detour did not reach a real function";
  EXPECT_EQ(g_probe.gameBCalls, 1);
}

TEST_F(ArenaPublicClockTrampoline, ANotCreatedAnswerIsTheHookAlreadyGone) {
  ASSERT_TRUE(HookSiteForTest(0, reinterpret_cast<void*>(&GameA)));
  g_probe.removeStatus = MH_ERROR_NOT_CREATED;
  ASSERT_TRUE(HookSiteForTest(0, reinterpret_cast<void*>(&GameB)));
  EXPECT_FALSE(Logged("remove")) << "an absent hook was reported as a failed removal";
  CallDetour(0);
  EXPECT_EQ(g_probe.results.back(), 7);
}

TEST_F(ArenaPublicClockTrampoline, AFailedRemovalIsLoggedAndTheOldHookIsRemovedAgainOnTheNextPass) {
  ASSERT_TRUE(HookSiteForTest(0, reinterpret_cast<void*>(&GameA)));

  g_probe.removeStatus = MH_ERROR_MEMORY_PROTECT;
  g_probe.attachSucceeds = false;
  EXPECT_FALSE(HookSiteForTest(0, reinterpret_cast<void*>(&GameB)));
  EXPECT_TRUE(Logged("MH_ERROR_MEMORY_PROTECT")) << "the failed removal was not logged with its status";
  EXPECT_EQ(g_probe.freedTrampolineCalls, 0);

  // The old hook is still in MinHook's table: the next pass takes it out again before hooking.
  g_probe.removeStatus = MH_OK;
  g_probe.attachSucceeds = true;
  ASSERT_TRUE(HookSiteForTest(0, reinterpret_cast<void*>(&GameB)));
  ASSERT_EQ(g_probe.removed.size(), 2u);
  EXPECT_EQ(g_probe.removed[1], reinterpret_cast<void*>(&GameA)) << "the old hook was forgotten after a failed removal";
}

}  // namespace
