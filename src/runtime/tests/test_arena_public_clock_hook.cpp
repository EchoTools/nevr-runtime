// #982: the arena_public_clock hook on the real stock script DLL, under MinHook: the site classifies as shipped,
// a pass hooks it, a second pass leaves it, a call through the hooked bind function reaches the detour (the decision
// is applied to the state before the original runs), and a release and reload of the DLL is hooked again.
//
// Needs the stock script DLLs: NEVR_SCRIPT_DLL_DIR names the directory that holds 08317e89cc3ae611.dll and
// fc4cbaae440e658c.dll (a read-only install's bin/win10/scripts). The test loads them and writes nothing to disk.
// Without the variable the tests skip and say so.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/hooking.h"
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

}  // namespace
