// #1139, docs/adr/0022 "PCVR": the two observer hooks and the Poll entry, under MinHook, against a fake game image.
//
// The fake image is one RWX allocation the size of echovr.exe's data range. The two hooked functions sit at their real
// RVAs inside it, built from the real prologue bytes followed by a short epilogue that returns a known value; the game
// globals the line reads sit at their real RVAs too. Everything runs through the production code: PatchDetour,
// ProcessReader (ReadProcessMemory on this process), the detours and Poll. Only the game and the two process queries
// (DXGI, LibOVR) are stood in for.
//
// NOT shown by these tests: that the real echovr.exe has these bytes and addresses on a live process, and that the
// deferred bootstrap installs the hooks before the game's boot call (see the PR).

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <MinHook.h>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/patch/render_config_hook.h"

namespace {

using namespace nevr_render_config;
namespace hook = nevr_render_config_hook;

constexpr std::size_t kImageBytes = 0x2200000;  // covers the highest game global the line reads (0x1420c78b0)
constexpr std::uint64_t kApplyResult = 0x1234abcdULL;
constexpr std::uint64_t kSelectorResult = 0x5678ULL;

// The real prologues (kApplyPrologue from render_config.h, the selector's from render_config_hook.cpp), then an epilogue
// that restores what the prologue saved.
constexpr std::array<std::uint8_t, 26> kSelectorPrologue = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24,
                                                             0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83,
                                                             0xec, 0x20, 0x48, 0x8b, 0xea, 0x48, 0x8b, 0xd9};
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

int CountEmpty() {
  int n = 0;
  for (const std::string& line : g_logged) {
    if (line.empty()) ++n;
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

// Two call sites with different return addresses, standing in for the game's boot code and its menu code. The volatile
// result stops a tail call (which would hand the hook the wrapper's own caller), and the distinct marker stops the
// compiler merging the two bodies.
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

class RenderConfigHook : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    hook::ResetForTest();
    g_logged.clear();
    g_levels.clear();
    SetBootLogSink(&CaptureLog);

    image_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(image_, nullptr) << "VirtualAlloc error=" << GetLastError();
    base_ = reinterpret_cast<std::uintptr_t>(image_);

    Put(kApplyVa, kApplyPrologue.data(), kApplyPrologue.size());
    Put(kApplyVa + kApplyPrologue.size(), kApplyEpilogue.data(), kApplyEpilogue.size());
    Put(kSelectorVa, kSelectorPrologue.data(), kSelectorPrologue.size());
    Put(kSelectorVa + kSelectorPrologue.size(), kSelectorEpilogue.data(), kSelectorEpilogue.size());

    const char gpu[] = "NVIDIA GeForce GTX 970";
    Put(kGpuNameVa, gpu, sizeof(gpu));
    PutValue<std::uint64_t>(kGpuVramVa, 4ULL << 30);
    PutValue<std::uint32_t>(kGpuVendorVa, 1);
    PutValue<std::int32_t>(kHmdDescVa + kHmdTypeOffset, 16);
    const char headset[] = "Oculus Rift S";
    Put(kHmdDescVa + kHmdProductNameOffset, headset, sizeof(headset));
    PutValue<std::int32_t>(kHmdDescVa + kHmdResolutionOffset, 1280);
    PutValue<std::int32_t>(kHmdDescVa + kHmdResolutionOffset + 4, 1440);
    PutValue<float>(kHmdDescVa + kHmdRefreshOffset, 80.0F);
    PutValue<std::uint64_t>(kRenderTargetVa, (1776ULL << 32) | 1648ULL);
    PutValue<float>(kResolutionScaleVa, 1.3F);

    Externals fake;
    fake.gpuDriver = [](const std::string&) { return std::string("31.0.15.3623"); };
    fake.runtimeVersion = []() { return std::string("1.43.0"); };
    hook::SetExternalsForTest(fake);

    WriteSettings(settings_, /*msaa=*/1);
  }

  void TearDown() override {
    hook::ResetForTest();
    // Hooking::Detach only disables a hook; MinHook keeps its entry for the address, and the next test's allocation can
    // land on the same address. Remove the entries so each test installs afresh.
    if (image_ != nullptr) {
      MH_RemoveHook(At(kApplyVa));
      MH_RemoveHook(At(kSelectorVa));
    }
    SetBootLogSink(nullptr);
    if (image_ != nullptr) VirtualFree(image_, 0, MEM_RELEASE);
    image_ = nullptr;
  }

  std::uint8_t* At(std::uint64_t va) { return image_ + (va - kImageBase); }
  void Put(std::uint64_t va, const void* data, std::size_t length) { std::memcpy(At(va), data, length); }
  template <typename T>
  void PutValue(std::uint64_t va, T value) {
    Put(va, &value, sizeof(value));
  }
  ApplyFn ApplyEntry() { return reinterpret_cast<ApplyFn>(At(kApplyVa)); }
  SelectorFn SelectorEntry() { return reinterpret_cast<SelectorFn>(At(kSelectorVa)); }

  static void WriteSettings(std::array<std::uint8_t, kSettingsBytes>& settings, std::int32_t msaa) {
    settings.fill(0);
    const std::uint32_t flags = kFlagFullscreen | kFlagTaa;
    const float scale = 1.0F;
    const float adaptiveMin = 45.0F;
    const float adaptiveMax = 90.0F;
    const std::int32_t target = 80;
    const std::int32_t quality = 3;
    std::memcpy(settings.data() + kSettingsFlagsOffset, &flags, sizeof(flags));
    std::memcpy(settings.data() + kSettingsResolutionScaleOffset, &scale, sizeof(scale));
    std::memcpy(settings.data() + kSettingsAdaptiveMinOffset, &adaptiveMin, sizeof(adaptiveMin));
    std::memcpy(settings.data() + kSettingsAdaptiveMaxOffset, &adaptiveMax, sizeof(adaptiveMax));
    std::memcpy(settings.data() + kSettingsAdaptiveTargetOffset, &target, sizeof(target));
    std::memcpy(settings.data() + kSettingsMsaaOffset, &msaa, sizeof(msaa));
    std::memcpy(settings.data() + kSettingsQualityLevelOffset, &quality, sizeof(quality));
  }

  /// The changed= list of a change line.
  static std::string ChangedList(const std::string& line) {
    const std::string key = " changed=";
    const auto at = line.find(key);
    if (at == std::string::npos) return "";
    const auto start = at + key.size();
    return line.substr(start, line.find(' ', start) - start);
  }

  std::uint8_t* image_ = nullptr;
  std::uintptr_t base_ = 0;
  std::array<std::uint8_t, kSettingsBytes> settings_{};
};

// ---- (g) server mode installs nothing ----

TEST_F(RenderConfigHook, ServerModeInstallsNothingAndSaysSoOnce) {
  EXPECT_FALSE(hook::Install(base_, /*isServer=*/true, /*headless=*/false));
  EXPECT_FALSE(hook::Install(base_, /*isServer=*/true, /*headless=*/false));
  EXPECT_FALSE(hook::ActiveForTest());
  EXPECT_EQ(At(kApplyVa)[0], kApplyPrologue[0]) << "a server run wrote a hook";
  EXPECT_EQ(At(kSelectorVa)[0], kSelectorPrologue[0]);
  EXPECT_EQ(CountContaining("[NEVR.RENDER] render_config skipped reason=server_mode"), 1);
  hook::Poll(10'000'000);
  hook::Poll(20'000'000);
  EXPECT_EQ(CountContaining("reason=first_frame"), 0) << "a server run wrote the render_config line";
}

TEST_F(RenderConfigHook, PollBeforeInstallWritesNothing) {
  hook::Poll(1'000'000);
  EXPECT_TRUE(g_logged.empty());
}

// ---- Install ----

TEST_F(RenderConfigHook, AClientInstallsBothObserversAndSaysSo) {
  ASSERT_TRUE(hook::Install(base_, /*isServer=*/false, /*headless=*/false));
  EXPECT_TRUE(hook::ActiveForTest());
  EXPECT_EQ(At(kApplyVa)[0], 0xE9) << "apply not detoured";
  EXPECT_EQ(At(kSelectorVa)[0], 0xE9) << "selector not detoured";
  EXPECT_EQ(CountContaining("[NEVR.RENDER] render_config hooks apply=installed selector=installed"), 1);
  EXPECT_TRUE(hook::Install(base_, false, false)) << "a second Install is a no-op that still reports installed";
  EXPECT_EQ(CountContaining("render_config hooks"), 1);
}

TEST_F(RenderConfigHook, AMismatchedPrologueInstallsNeitherHookAndNamesTheCause) {
  At(kSelectorVa)[3] ^= 0xFF;  // one byte of the selector's prologue differs
  EXPECT_FALSE(hook::Install(base_, /*isServer=*/false, /*headless=*/false));
  EXPECT_EQ(At(kApplyVa)[0], kApplyPrologue[0]) << "the apply hook went in although the selector did not validate";
  EXPECT_EQ(At(kSelectorVa)[0], kSelectorPrologue[0]);
  const std::string warning = LineContaining("reason=prologue_mismatch");
  EXPECT_NE(warning.find("name=GfxSettingsAutoSelect va=0x140c31e00"), std::string::npos) << warning;
  EXPECT_NE(warning.find("expected=48895c240848896c actual=48895cdb0848896c"), std::string::npos) << warning;
  EXPECT_EQ(CountContaining("reason=prologue_mismatch"), 1) << "only the selector mismatched";
  for (std::size_t i = 0; i < g_logged.size(); ++i) {
    if (g_logged[i].find("prologue_mismatch") != std::string::npos) {
      EXPECT_EQ(g_levels[i], EchoVR::LogLevel::Warning);
    }
  }

  // The check still runs; the settings fields read unavailable.
  hook::Poll(1'000'000);
  const std::string line = LineContaining("reason=first_frame");
  EXPECT_NE(line.find(" msaa=unavailable "), std::string::npos) << line;
  EXPECT_NE(line.find(" settings_source=unknown "), std::string::npos) << line;
}

// ---- (f) the hooks pass the original's result through, and record the boot caller only for the boot return address ----

TEST_F(RenderConfigHook, TheApplyHookReturnsTheOriginalsResultUnchanged) {
  ASSERT_TRUE(hook::Install(base_, false, false));
  EXPECT_EQ(CallApplyFromMenu(ApplyEntry(), settings_.data()), kApplyResult);
  EXPECT_EQ(CallApplyFromBoot(ApplyEntry(), settings_.data()), kApplyResult);
  EXPECT_EQ(hook::ObservedForTest().applyCalls, 2U);
}

TEST_F(RenderConfigHook, TheSelectorHookReturnsTheOriginalsResultUnchanged) {
  ASSERT_TRUE(hook::Install(base_, false, false));
  EXPECT_EQ(CallSelector(SelectorEntry(), settings_.data()), kSelectorResult);
}

TEST_F(RenderConfigHook, OnlyTheBootReturnAddressCountsAsTheBootCall) {
  ASSERT_TRUE(hook::Install(base_, false, false));

  // Learn the boot wrapper's return address the way the hook sees it, then declare it the boot return address.
  ASSERT_EQ(CallApplyFromBoot(ApplyEntry(), settings_.data()), kApplyResult);
  const std::uintptr_t bootReturn = hook::LastApplyReturnForTest();
  ASSERT_NE(bootReturn, 0U);
  EXPECT_EQ(hook::ObservedForTest().source, SettingsSource::MenuOrJson) << "before it is declared, no caller is the boot call";

  hook::SetBootReturnForTest(bootReturn);
  ASSERT_EQ(CallApplyFromBoot(ApplyEntry(), settings_.data()), kApplyResult);
  EXPECT_EQ(hook::LastApplyReturnForTest(), bootReturn);
  EXPECT_EQ(hook::ObservedForTest().source, SettingsSource::SavedOrDefault) << "the boot call, no selector run";

  ASSERT_EQ(CallApplyFromMenu(ApplyEntry(), settings_.data()), kApplyResult);
  EXPECT_NE(hook::LastApplyReturnForTest(), bootReturn);
  EXPECT_EQ(hook::ObservedForTest().source, SettingsSource::MenuOrJson) << "a different return address is not the boot call";
}

TEST_F(RenderConfigHook, ASelectorRunOnTheSameStructBeforeTheBootCallIsAutoSelected) {
  ASSERT_TRUE(hook::Install(base_, false, false));
  ASSERT_EQ(CallApplyFromBoot(ApplyEntry(), settings_.data()), kApplyResult);
  hook::SetBootReturnForTest(hook::LastApplyReturnForTest());

  ASSERT_EQ(CallSelector(SelectorEntry(), settings_.data()), kSelectorResult);
  ASSERT_EQ(CallApplyFromBoot(ApplyEntry(), settings_.data()), kApplyResult);
  EXPECT_EQ(hook::ObservedForTest().source, SettingsSource::AutoSelected);
}

TEST_F(RenderConfigHook, AnApplyCallOnAnUnreadableStructReturnsTheResultAndIsCounted) {
  ASSERT_TRUE(hook::Install(base_, false, false));
  EXPECT_EQ(CallApplyFromMenu(ApplyEntry(), nullptr), kApplyResult);
  const auto seen = hook::ObservedForTest();
  EXPECT_EQ(seen.applyCalls, 1U);
  EXPECT_EQ(seen.readFailures, 1U);
  EXPECT_FALSE(seen.seen);
}

// ---- (#1774) a headless run: the apply is the headless detour's; the selector stays ----

TEST_F(RenderConfigHook, AHeadlessInstallSkipsOnlyTheApplyDetour) {
  EXPECT_TRUE(hook::Install(base_, /*isServer=*/false, /*headless=*/true));
  EXPECT_EQ(At(kApplyVa)[0], kApplyPrologue[0]) << "the apply was detoured on a headless run";
  EXPECT_EQ(At(kSelectorVa)[0], 0xE9) << "the selector was not detoured on a headless run";
  EXPECT_EQ(CountContaining("[NEVR.RENDER] render_config hooks apply=skipped_headless selector=installed"), 1);
  EXPECT_EQ(CountContaining("apply=installed"), 0);
}

// ---- The line through Poll and the log ----

TEST_F(RenderConfigHook, TheFirstFrameLineIsLoggedOnceAtInfoWithTheImagesValues) {
  ASSERT_TRUE(hook::Install(base_, false, false));
  ASSERT_EQ(CallApplyFromBoot(ApplyEntry(), settings_.data()), kApplyResult);
  hook::SetBootReturnForTest(hook::LastApplyReturnForTest());
  ASSERT_EQ(CallSelector(SelectorEntry(), settings_.data()), kSelectorResult);
  ASSERT_EQ(CallApplyFromBoot(ApplyEntry(), settings_.data()), kApplyResult);

  hook::Poll(1'000'000);
  hook::Poll(1'500'000);  // inside the second: not evaluated
  hook::Poll(2'000'000);
  hook::Poll(30'000'000);
  EXPECT_EQ(CountContaining("[NEVR.RENDER] render_config reason="), 1);
  EXPECT_EQ(CountEmpty(), 0) << "a pass with nothing to say wrote an empty log line";
  const std::string line = LineContaining("reason=first_frame");
  EXPECT_NE(line.find("changed=- platform=pcvr gpu=\"NVIDIA GeForce GTX 970\""), std::string::npos) << line;
  EXPECT_NE(line.find(" gpu_driver=31.0.15.3623 "), std::string::npos) << line;
  EXPECT_NE(line.find(" render_w=1648 render_h=1776 "), std::string::npos) << line;
  EXPECT_NE(line.find(" settings_source=auto_selected quality_level=3 msaa=1 "), std::string::npos) << line;
  for (std::size_t i = 0; i < g_logged.size(); ++i) {
    if (g_logged[i].find("render_config reason=") != std::string::npos) {
      EXPECT_EQ(g_levels[i], EchoVR::LogLevel::Info);
    }
  }
  // The trace follows the first line, once, naming what the observers saw.
  EXPECT_EQ(CountContaining("[NEVR.RENDER] render_config settings_trace "), 1);
  const std::string trace = LineContaining("settings_trace");
  EXPECT_NE(trace.find(" apply_calls=2 read_failures=0 "), std::string::npos) << trace;
  EXPECT_NE(trace.find(" last_apply_return=0x"), std::string::npos) << trace;
  EXPECT_NE(trace.find(" boot_return=0x"), std::string::npos) << trace;
}

TEST_F(RenderConfigHook, AMsaaChangeThroughTheApplyHookAndANewRefreshRateAreOneChangeLine) {
  ASSERT_TRUE(hook::Install(base_, false, false));
  ASSERT_EQ(CallApplyFromBoot(ApplyEntry(), settings_.data()), kApplyResult);
  hook::SetBootReturnForTest(hook::LastApplyReturnForTest());
  ASSERT_EQ(CallSelector(SelectorEntry(), settings_.data()), kSelectorResult);
  ASSERT_EQ(CallApplyFromBoot(ApplyEntry(), settings_.data()), kApplyResult);
  hook::Poll(1'000'000);
  ASSERT_EQ(CountContaining("reason=first_frame"), 1);

  WriteSettings(settings_, /*msaa=*/4);
  ASSERT_EQ(CallApplyFromMenu(ApplyEntry(), settings_.data()), kApplyResult);
  PutValue<float>(kHmdDescVa + kHmdRefreshOffset, 120.0F);

  hook::Poll(2'000'000);
  ASSERT_EQ(CountContaining("reason=change"), 1);
  const std::string line = LineContaining("reason=change");
  const std::string changed = ChangedList(line);
  EXPECT_NE(changed.find("msaa"), std::string::npos) << changed;
  EXPECT_NE(changed.find("settings_source"), std::string::npos) << changed;
  EXPECT_NE(changed.find("refresh_hz"), std::string::npos) << changed;
  EXPECT_NE(line.find(" msaa=4 "), std::string::npos) << line;
  EXPECT_NE(line.find(" settings_source=menu_or_json "), std::string::npos) << line;
  EXPECT_NE(line.find(" refresh_hz=120 "), std::string::npos) << line;

  hook::Poll(3'000'000);
  hook::Poll(4'000'000);
  EXPECT_EQ(CountContaining("reason=change"), 1) << "the same configuration was written again";
  EXPECT_EQ(CountContaining("settings_trace"), 1) << "the trace is written with the first line only, not with a change";
}

TEST_F(RenderConfigHook, TheTraceNamesTheLastApplyReturnAndTheBootReturnAsTwoAddresses) {
  ASSERT_TRUE(hook::Install(base_, false, false));
  ASSERT_EQ(CallApplyFromBoot(ApplyEntry(), settings_.data()), kApplyResult);
  const std::uintptr_t bootReturn = hook::LastApplyReturnForTest();
  hook::SetBootReturnForTest(bootReturn);
  ASSERT_EQ(CallApplyFromMenu(ApplyEntry(), settings_.data()), kApplyResult);
  const std::uintptr_t menuReturn = hook::LastApplyReturnForTest();
  ASSERT_NE(menuReturn, bootReturn);

  hook::Poll(1'000'000);
  const std::string trace = LineContaining("settings_trace");
  char want[64];
  std::snprintf(want, sizeof(want), " last_apply_return=0x%llx boot_return=0x%llx", static_cast<unsigned long long>(menuReturn),
                static_cast<unsigned long long>(bootReturn));
  EXPECT_NE(trace.find(want), std::string::npos) << trace << " wanted " << want;
}

TEST_F(RenderConfigHook, WithNoRenderTargetTheFirstLineComesAfterFiveSecondsWithTheTargetUnavailable) {
  PutValue<std::uint64_t>(kRenderTargetVa, 0);
  ASSERT_TRUE(hook::Install(base_, false, false));
  hook::Poll(1'000'000);
  hook::Poll(5'000'000);
  EXPECT_EQ(CountContaining("reason=first_frame"), 0);
  hook::Poll(6'000'000);
  const std::string line = LineContaining("reason=first_frame");
  EXPECT_NE(line.find(" render_target=unavailable "), std::string::npos) << line;
}

TEST_F(RenderConfigHook, TheProductionQueriesAnswerInsteadOfFailingTheLine) {
  hook::ResetForTest();  // puts the production DXGI and LibOVR queries back in place of SetUp's stand-ins
  ASSERT_TRUE(hook::Install(base_, false, false));
  hook::Poll(1'000'000);
  const std::string line = LineContaining("reason=first_frame");
  ASSERT_FALSE(line.empty()) << "the first-frame line was not written";
  EXPECT_NE(line.find(" gpu_driver="), std::string::npos) << line;
  EXPECT_NE(line.find(" runtime_version=\""), std::string::npos) << line;
  EXPECT_EQ(line.find('\n'), std::string::npos);
}

// ---- (#1419) the GPU score: a zero NVIDIA rating is filled before the selector reads it; any other value is kept ----

int g_queryCalls = 0;

// 3584 cores at 1,500,000 kHz: 10.752, the game's own formula (patch/gpu_score.h).
void FakeNvidiaQuery(nevr_gpu_score::NvidiaMeasure* out) {
  ++g_queryCalls;
  out->haveCores = true;
  out->cores = 3584;
  out->haveClockKhz = true;
  out->clockKhz = 1500000;
}

void FakeNvidiaAbsent(nevr_gpu_score::NvidiaMeasure* out) {
  ++g_queryCalls;
  out->failure = "nvapi_absent";
}

TEST_F(RenderConfigHook, AZeroNvidiaScoreIsFilledBeforeTheSelectorRunsAndLoggedOnce) {
  PutValue<double>(kGpuScoreVa, 0.0);
  g_queryCalls = 0;
  hook::SetNvidiaQueryForTest(&FakeNvidiaQuery);
  ASSERT_TRUE(hook::Install(base_, false, false));
  ASSERT_EQ(CallSelector(SelectorEntry(), settings_.data()), kSelectorResult);
  EXPECT_EQ(g_queryCalls, 1);
  double score = 0.0;
  std::memcpy(&score, At(kGpuScoreVa), sizeof(score));
  EXPECT_DOUBLE_EQ(score, 10.752);
  EXPECT_EQ(CountContaining("[NEVR.RENDER] gpu_score filled vendor=1 game_score=0 "), 1);
  EXPECT_EQ(CountContaining("reason=-"), 1);
}

TEST_F(RenderConfigHook, ANonZeroGameScoreIsKeptAndTheQueryIsNotAsked) {
  PutValue<double>(kGpuScoreVa, 6.0);
  g_queryCalls = 0;
  hook::SetNvidiaQueryForTest(&FakeNvidiaQuery);
  ASSERT_TRUE(hook::Install(base_, false, false));
  ASSERT_EQ(CallSelector(SelectorEntry(), settings_.data()), kSelectorResult);
  EXPECT_EQ(g_queryCalls, 0);
  double score = 0.0;
  std::memcpy(&score, At(kGpuScoreVa), sizeof(score));
  EXPECT_DOUBLE_EQ(score, 6.0);
  EXPECT_EQ(CountContaining("[NEVR.RENDER] gpu_score kept"), 1);
}

TEST_F(RenderConfigHook, AZeroScoreWithNoNvapiStaysZeroAndSaysWhy) {
  PutValue<double>(kGpuScoreVa, 0.0);
  g_queryCalls = 0;
  hook::SetNvidiaQueryForTest(&FakeNvidiaAbsent);
  ASSERT_TRUE(hook::Install(base_, false, false));
  ASSERT_EQ(CallSelector(SelectorEntry(), settings_.data()), kSelectorResult);
  EXPECT_EQ(g_queryCalls, 1);
  double score = 1.0;
  std::memcpy(&score, At(kGpuScoreVa), sizeof(score));
  EXPECT_DOUBLE_EQ(score, 0.0);
  EXPECT_EQ(CountContaining("[NEVR.RENDER] gpu_score unrated vendor=1 game_score=0 score=0 source=none reason=nvapi_absent"), 1);
  for (std::size_t i = 0; i < g_logged.size(); ++i) {
    if (g_logged[i].find("gpu_score unrated") != std::string::npos) {
      EXPECT_EQ(g_levels[i], EchoVR::LogLevel::Warning);
    }
  }
}

}  // namespace
