// #1774 P3: the headless replacement of ApplyRuntimeGfxSettings, and its coexistence with render_config_hook on a headless
// run, under MinHook against a fake game image.
//
// The fake image is one RWX allocation the size of echovr.exe's data range. ApplyRuntimeGfxSettings and the GPU
// auto-selector sit at their real RVAs inside it, built from the production prologue bytes followed by an epilogue that
// returns a known value, so an original that runs is visible in the result. Everything runs through the production code:
// PatchDetour, the detours, the liveness counter and the render_config observer. Only the game is stood in for.
//
// NOT shown by these tests: that the real echovr.exe has these bytes at these addresses on a live headless run.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <MinHook.h>

#include "core/globals.h"
#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/hook_liveness.h"
#include "runtime/patch/headless_gfx_settings.h"
#include "runtime/patch/render_config.h"
#include "runtime/patch/render_config_hook.h"

// hook_liveness.cpp reads the server flag; the runtime defines it in cli.cpp.
BOOL g_isServer = FALSE;

namespace {

constexpr std::size_t kImageBytes = 0x2200000;  // covers the highest address the detours touch (0x140c31e00 is the selector)
constexpr std::uint64_t kApplyResult = 0x1234abcdULL;
constexpr std::uint64_t kSelectorResult = 0x5678ULL;

// The selector's prologue as render_config_hook.cpp validates it (the apply's comes from render_config.h).
constexpr std::array<std::uint8_t, 26> kSelectorPrologue = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24,
                                                             0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83,
                                                             0xec, 0x20, 0x48, 0x8b, 0xea, 0x48, 0x8b, 0xd9};
// Epilogues that restore what the prologues saved and return a known value.
constexpr std::array<std::uint8_t, 29> kApplyEpilogue = {
    0xb8, 0xcd, 0xab, 0x34, 0x12,  // mov eax, 0x1234abcd
    0x48, 0x89, 0xec,              // mov rsp, rbp
    0x41, 0x5f,                    // pop r15
    0x41, 0x5e,                    // pop r14
    0x5d,                          // pop rbp
    0x48, 0x8b, 0x5c, 0x24, 0x10,  // mov rbx, [rsp+0x10]
    0x48, 0x8b, 0x74, 0x24, 0x18,  // mov rsi, [rsp+0x18]
    0x48, 0x8b, 0x7c, 0x24, 0x20,  // mov rdi, [rsp+0x20]
    0xc3};                         // ret
constexpr std::array<std::uint8_t, 26> kSelectorEpilogue = {
    0xb8, 0x78, 0x56, 0x00, 0x00,  // mov eax, 0x5678
    0x48, 0x83, 0xc4, 0x20,        // add rsp, 0x20
    0x5f,                          // pop rdi
    0x48, 0x8b, 0x5c, 0x24, 0x08,  // mov rbx, [rsp+8]
    0x48, 0x8b, 0x6c, 0x24, 0x10,  // mov rbp, [rsp+0x10]
    0x48, 0x8b, 0x74, 0x24, 0x18,  // mov rsi, [rsp+0x18]
    0xc3};

std::vector<std::string> g_logged;
std::vector<EchoVR::LogLevel> g_levels;
void CaptureLog(EchoVR::LogLevel level, const CHAR* message) {
  g_levels.push_back(level);
  g_logged.push_back(message);
}

int CountContaining(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) {
    if (line.find(needle) != std::string::npos) ++n;
  }
  return n;
}

std::string LineContaining(const char* needle) {
  for (const std::string& line : g_logged) {
    if (line.find(needle) != std::string::npos) return line;
  }
  return "";
}

using ApplyFn = std::uint64_t (*)(void*, void*, int);
using SelectorFn = std::uint64_t (*)(void*, void*);

volatile int g_marker = 0;

// Two call sites with distinct return addresses, as the game has at boot (FUN_1401090c0) and in the menu (fcn.140a26be0).
// The volatile result stops a tail call, which would hand the hook the wrapper's own caller.
__attribute__((noinline)) std::uint64_t CallApplyFromBoot(ApplyFn fn, void* settings) {
  g_marker = 1;
  volatile std::uint64_t result = fn(settings, nullptr, 0);
  return result;
}
__attribute__((noinline)) std::uint64_t CallApplyFromMenu(ApplyFn fn, void* settings) {
  g_marker = 2;
  volatile std::uint64_t result = fn(settings, nullptr, 1);
  return result;
}
__attribute__((noinline)) std::uint64_t CallSelector(SelectorFn fn, void* settings) {
  g_marker = 3;
  volatile std::uint64_t result = fn(settings, nullptr);
  return result;
}

class HeadlessGfxSettings : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    nevr_headless_gfx_settings::ResetForTest();
    nevr_render_config_hook::ResetForTest();
    g_logged.clear();
    g_levels.clear();
    SetBootLogSink(&CaptureLog);

    image_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(image_, nullptr) << "VirtualAlloc error=" << GetLastError();
    base_ = reinterpret_cast<std::uintptr_t>(image_);

    Put(nevr_render_config::kApplyVa, nevr_render_config::kApplyPrologue.data(), nevr_render_config::kApplyPrologue.size());
    Put(nevr_render_config::kApplyVa + nevr_render_config::kApplyPrologue.size(), kApplyEpilogue.data(), kApplyEpilogue.size());
    Put(nevr_render_config::kSelectorVa, kSelectorPrologue.data(), kSelectorPrologue.size());
    Put(nevr_render_config::kSelectorVa + kSelectorPrologue.size(), kSelectorEpilogue.data(), kSelectorEpilogue.size());
  }

  void TearDown() override {
    nevr_headless_gfx_settings::ResetForTest();
    nevr_render_config_hook::ResetForTest();
    // Hooking::Detach only disables a hook and MinHook keeps its entry for the address; remove both so the next test's
    // allocation, which can land on the same address, installs afresh.
    if (image_ != nullptr) {
      MH_RemoveHook(At(nevr_render_config::kApplyVa));
      MH_RemoveHook(At(nevr_render_config::kSelectorVa));
    }
    SetBootLogSink(nullptr);
    if (image_ != nullptr) VirtualFree(image_, 0, MEM_RELEASE);
    image_ = nullptr;
  }

  std::uint8_t* At(std::uint64_t va) { return image_ + (va - nevr_render_config::kImageBase); }
  void Put(std::uint64_t va, const void* data, std::size_t length) { std::memcpy(At(va), data, length); }
  ApplyFn ApplyEntry() { return reinterpret_cast<ApplyFn>(At(nevr_render_config::kApplyVa)); }
  SelectorFn SelectorEntry() { return reinterpret_cast<SelectorFn>(At(nevr_render_config::kSelectorVa)); }

  std::uint8_t* image_ = nullptr;
  std::uintptr_t base_ = 0;
};

// ---- the headless detour ----

// The liveness counters are process-wide and have no reset, so each test reads the count before and after its calls.
long GfxEntries() { return nevr_hook_liveness::Entries(nevr_hook_liveness::kApplyGfxSettings); }

TEST_F(HeadlessGfxSettings, TheDetourReturnsZeroAndNeverEntersTheOriginalFromEitherCaller) {
  ASSERT_TRUE(nevr_headless_gfx_settings::Install(base_));
  EXPECT_EQ(At(nevr_render_config::kApplyVa)[0], 0xE9) << "the apply was not detoured";
  const long before = GfxEntries();
  EXPECT_EQ(CallApplyFromBoot(ApplyEntry(), nullptr), 0U) << "the boot call returned the original's result";
  EXPECT_EQ(CallApplyFromMenu(ApplyEntry(), nullptr), 0U) << "the menu call returned the original's result";
  EXPECT_EQ(GfxEntries() - before, 2L);
  EXPECT_EQ(CountContaining("[NEVR.HEADLESS] graphics settings apply skipped calls=1 "), 1);
  EXPECT_EQ(CountContaining("[NEVR.HEADLESS] graphics settings apply skipped calls=2 "), 1);
}

TEST_F(HeadlessGfxSettings, WithoutTheDetourTheOriginalRuns) {
  // The baseline the detour replaces: the game's apply runs to its epilogue and returns its value.
  const long before = GfxEntries();
  EXPECT_EQ(CallApplyFromBoot(ApplyEntry(), nullptr), kApplyResult);
  EXPECT_EQ(GfxEntries() - before, 0L);
}

TEST_F(HeadlessGfxSettings, AMismatchedPrologueInstallsNothingAndNamesTheBytes) {
  At(nevr_render_config::kApplyVa)[3] ^= 0xFF;  // the 0x24 of `mov [rsp+0x10],rbx`
  EXPECT_FALSE(nevr_headless_gfx_settings::Install(base_));
  EXPECT_FALSE(nevr_headless_gfx_settings::ActiveForTest());
  EXPECT_EQ(At(nevr_render_config::kApplyVa)[0], nevr_render_config::kApplyPrologue[0]) << "a hook went in on a mismatch";
  const std::string warning = LineContaining("reason=prologue_mismatch");
  EXPECT_NE(warning.find("name=ApplyRuntimeGfxSettings va=0x140c31870"), std::string::npos) << warning;
  EXPECT_NE(warning.find("expected=48895c2410488974"), std::string::npos) << warning;
  EXPECT_NE(warning.find("actual=48895cdb10488974"), std::string::npos) << warning;
  for (std::size_t i = 0; i < g_logged.size(); ++i) {
    if (g_logged[i].find("prologue_mismatch") != std::string::npos) EXPECT_EQ(g_levels[i], EchoVR::LogLevel::Warning);
  }
}

TEST_F(HeadlessGfxSettings, TheSecondInstallIsANoOpThatStillReportsInstalled) {
  ASSERT_TRUE(nevr_headless_gfx_settings::Install(base_));
  EXPECT_TRUE(nevr_headless_gfx_settings::Install(base_));
  EXPECT_EQ(CountContaining("graphics settings hook installed"), 1);
}

// ---- the liveness report: the row is named only on a headless run ----

TEST_F(HeadlessGfxSettings, TheLivenessReportNamesTheDetourOnlyOnAHeadlessRun) {
  const BOOL wasHeadless = g_isHeadless;
  g_isHeadless = FALSE;
  nevr_hook_liveness::Report("test");
  EXPECT_EQ(CountContaining("hook_liveness name=ApplyRuntimeGfxSettings"), 0) << "a client run reports a hook it never installs";
  EXPECT_GT(CountContaining("hook_liveness name=CTimer_GetMilliSeconds"), 0) << "Report wrote no rows";
  g_isHeadless = wasHeadless;
}

// ---- the render_config observer on the same run ----

TEST_F(HeadlessGfxSettings, OnAHeadlessRunTheRenderConfigObserverLeavesTheApplyToTheDetourAndKeepsTheSelector) {
  // PatchEnableHeadless runs before render_config's Install (boot.cpp), so the detour is in place first.
  ASSERT_TRUE(nevr_headless_gfx_settings::Install(base_));
  EXPECT_TRUE(nevr_render_config_hook::Install(base_, /*isServer=*/false, /*headless=*/true));
  EXPECT_EQ(At(nevr_render_config::kSelectorVa)[0], 0xE9) << "the GPU-score selector (#1419) was not detoured on headless";
  EXPECT_EQ(CountContaining("[NEVR.RENDER] render_config hooks apply=skipped_headless selector=installed"), 1);
  for (std::size_t i = 0; i < g_logged.size(); ++i) {
    EXPECT_NE(g_levels[i], EchoVR::LogLevel::Warning) << "a Warning on the headless install: " << g_logged[i];
  }
  // The apply still goes to the headless detour, not to a second observer: 0 back, and the headless counter moves.
  const long before = GfxEntries();
  EXPECT_EQ(CallApplyFromMenu(ApplyEntry(), nullptr), 0U);
  EXPECT_EQ(GfxEntries() - before, 1L);
  EXPECT_EQ(CallSelector(SelectorEntry(), nullptr), kSelectorResult);
}

}  // namespace
