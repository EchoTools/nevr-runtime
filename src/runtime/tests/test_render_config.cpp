// #1139, docs/adr/0022 "PCVR": the render_config model against a fake game image. No Windows call, no hook: the
// memory the line reads is a table of byte regions at game addresses, the clock is a number, and the two values that
// are not game memory are lambdas. The hooks and the log call are tested in test_render_config_hook.cpp.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/patch/render_config.h"

namespace {

using namespace nevr_render_config;

constexpr std::uintptr_t kBase = 0x10000000ULL;  // where the fake image "is loaded"

/// A fake process: byte regions at addresses. A read succeeds only when it lies inside one region.
class FakeImage final : public MemoryReader {
 public:
  void Region(std::uintptr_t address, std::size_t length) { regions_.push_back({address, std::vector<unsigned char>(length)}); }
  void Poke(std::uintptr_t address, const void* data, std::size_t length) {
    for (auto& r : regions_) {
      if (address >= r.start && address + length <= r.start + r.bytes.size()) {
        std::memcpy(r.bytes.data() + (address - r.start), data, length);
        return;
      }
    }
    FAIL() << "poke outside every region";
  }
  template <typename T>
  void PokeValue(std::uintptr_t address, T value) {
    Poke(address, &value, sizeof(value));
  }
  void PokeText(std::uintptr_t address, const std::string& text) { Poke(address, text.c_str(), text.size() + 1); }
  void DropRegionAt(std::uintptr_t address) {
    for (auto it = regions_.begin(); it != regions_.end(); ++it) {
      if (address >= it->start && address < it->start + it->bytes.size()) {
        regions_.erase(it);
        return;
      }
    }
  }
  bool Read(std::uintptr_t address, void* out, std::size_t length) const override {
    for (const auto& r : regions_) {
      if (address >= r.start && address + length <= r.start + r.bytes.size()) {
        std::memcpy(out, r.bytes.data() + (address - r.start), length);
        return true;
      }
    }
    return false;
  }

 private:
  struct Block {
    std::uintptr_t start;
    std::vector<unsigned char> bytes;
  };
  std::vector<Block> regions_;
};

std::uintptr_t At(std::uint64_t va) { return Resolve(kBase, va); }

/// The settings struct lives outside the image, as the game's does (on the game object).
constexpr std::uintptr_t kSettings = 0x30000000ULL;
constexpr std::uintptr_t kOtherSettings = 0x30001000ULL;
constexpr std::uintptr_t kBootReturn = 0x40000100ULL;
constexpr std::uintptr_t kMenuReturn = 0x40000200ULL;

struct World {
  FakeImage image;
  SettingsObserver observer;

  World() {
    image.Region(At(kGpuNameVa), 0x90);  // name buffer, vram at +0x80, vendor at +0x88
    image.Region(At(kHmdDescVa), kHmdDescBytes);
    image.Region(At(kRenderTargetVa), 8);
    image.Region(At(kResolutionScaleVa), 16);
    image.Region(kSettings, kSettingsBytes);
    image.Region(kOtherSettings, kSettingsBytes);

    image.PokeText(At(kGpuNameVa), "NVIDIA GeForce GTX 970");
    image.PokeValue<std::uint64_t>(At(kGpuVramVa), 4ULL << 30);  // 4096 MB
    image.PokeValue<std::uint32_t>(At(kGpuVendorVa), 1);
    image.PokeValue<std::int32_t>(At(kHmdDescVa) + kHmdTypeOffset, 16);
    image.PokeText(At(kHmdDescVa) + kHmdProductNameOffset, "Oculus Rift S");
    image.PokeValue<std::int32_t>(At(kHmdDescVa) + kHmdResolutionOffset, 1280);
    image.PokeValue<std::int32_t>(At(kHmdDescVa) + kHmdResolutionOffset + 4, 1440);
    image.PokeValue<float>(At(kHmdDescVa) + kHmdRefreshOffset, 80.0F);
    SetRenderTarget(1648, 1776);
    image.PokeValue<float>(At(kResolutionScaleVa), 1.3F);
    image.PokeValue<float>(At(kResolutionScaleVa) + 4, 1.3F);
    WriteSettings(kSettings, /*msaa=*/1, /*quality=*/3);
  }

  void SetRenderTarget(std::int32_t w, std::int32_t h) {
    const std::uint64_t packed = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(h)) << 32) |
                                 static_cast<std::uint32_t>(w);
    image.PokeValue<std::uint64_t>(At(kRenderTargetVa), packed);
  }

  /// flags: fullscreen(1) + TAA(4) set, adaptive resolution(2) and multires(8) clear.
  void WriteSettings(std::uintptr_t address, std::int32_t msaa, std::int32_t quality) {
    image.PokeValue<std::uint32_t>(address + kSettingsFlagsOffset, 1U | 4U);
    image.PokeValue<std::uint32_t>(address + kSettingsDisplayOffset, 0);
    image.PokeValue<float>(address + kSettingsResolutionScaleOffset, 1.0F);
    image.PokeValue<float>(address + kSettingsAdaptiveMinOffset, 45.0F);
    image.PokeValue<float>(address + kSettingsAdaptiveMaxOffset, 90.0F);
    image.PokeValue<std::int32_t>(address + kSettingsAdaptiveTargetOffset, 80);
    image.PokeValue<std::int32_t>(address + kSettingsMsaaOffset, msaa);
    image.PokeValue<std::int32_t>(address + kSettingsQualityLevelOffset, quality);
  }

  Externals Ext(int* driverCalls = nullptr, int* versionCalls = nullptr) {
    Externals e;
    e.gpuDriver = [driverCalls](const std::string& gpu) {
      if (driverCalls != nullptr) ++*driverCalls;
      return gpu == "NVIDIA GeForce GTX 970" ? std::string("31.0.15.3623") : std::string("unknown");
    };
    e.runtimeVersion = [versionCalls]() {
      if (versionCalls != nullptr) ++*versionCalls;
      return std::string("1.43.0");
    };
    return e;
  }
};

// The line a boot-time Apply with the selector having run produces, first frame.
const char* const kFirstFrameLine =
    "[NEVR.RENDER] render_config reason=first_frame changed=- platform=pcvr gpu=\"NVIDIA GeForce GTX 970\" gpu_vendor=1 "
    "vram_mb=4096 gpu_driver=31.0.15.3623 headset=\"Oculus Rift S\" hmd_type=16 runtime=libovr "
    "runtime_version=\"1.43.0\" refresh_hz=80 hmd_w=1280 hmd_h=1440 render_w=1648 render_h=1776 resolution_scale=1.3 "
    "settings_source=auto_selected quality_level=3 msaa=1 settings_resolution_scale=1 adaptive_res=false taa=true "
    "multires=false fullscreen=true adaptive_min=45 adaptive_max=90 adaptive_target=80 display=0";

void BootApplyWithSelector(World& w) {
  w.observer.OnSelector(kSettings);
  w.observer.OnApply(w.image, kSettings, kBootReturn, kBootReturn);
}

TEST(RenderConfigCollector, TheFirstPassWritesTheFirstFrameLineWithEveryFieldFromTheImage) {
  World w;
  BootApplyWithSelector(w);
  Collector c(w.image, kBase, w.observer, w.Ext());
  EXPECT_EQ(c.Poll(0), kFirstFrameLine);
}

TEST(RenderConfigCollector, ASecondPassWithNothingChangedWritesNothing) {
  World w;
  BootApplyWithSelector(w);
  Collector c(w.image, kBase, w.observer, w.Ext());
  ASSERT_FALSE(c.Poll(0).empty());
  EXPECT_EQ(c.Poll(1000), "");
  EXPECT_EQ(c.Poll(2500), "");
  EXPECT_EQ(c.Poll(60000), "");
}

TEST(RenderConfigCollector, ThePassesAreAtLeastASecondApart) {
  World w;
  BootApplyWithSelector(w);
  Collector c(w.image, kBase, w.observer, w.Ext());
  ASSERT_FALSE(c.Poll(5000).empty());
  w.image.PokeValue<float>(At(kHmdDescVa) + kHmdRefreshOffset, 90.0F);
  EXPECT_EQ(c.Poll(5999), "") << "a change inside the second must wait for the next pass";
  const std::string line = c.Poll(6000);
  EXPECT_NE(line.find("reason=change changed=refresh_hz platform=pcvr"), std::string::npos) << line;
  EXPECT_NE(line.find("refresh_hz=90 "), std::string::npos) << line;
}

TEST(RenderConfigCollector, ANewMsaaFromTheMenuIsOneChangeLineNamingMsaaAndTheSource) {
  World w;
  BootApplyWithSelector(w);
  Collector c(w.image, kBase, w.observer, w.Ext());
  ASSERT_FALSE(c.Poll(0).empty());
  w.WriteSettings(kSettings, /*msaa=*/2, /*quality=*/3);
  w.observer.OnApply(w.image, kSettings, kMenuReturn, kBootReturn);
  const std::string line = c.Poll(1000);
  EXPECT_NE(line.find("reason=change changed=settings_source,msaa platform=pcvr"), std::string::npos) << line;
  EXPECT_NE(line.find("settings_source=menu_or_json"), std::string::npos) << line;
  EXPECT_NE(line.find(" msaa=2 "), std::string::npos) << line;
  EXPECT_EQ(c.Poll(2000), "") << "the same configuration is not written twice";
}

TEST(RenderConfigCollector, AChangedRefreshRateInTheImageIsOneChangeLine) {
  World w;
  BootApplyWithSelector(w);
  Collector c(w.image, kBase, w.observer, w.Ext());
  ASSERT_FALSE(c.Poll(0).empty());
  w.image.PokeValue<float>(At(kHmdDescVa) + kHmdRefreshOffset, 72.0F);
  const std::string line = c.Poll(1000);
  EXPECT_NE(line.find("reason=change changed=refresh_hz "), std::string::npos) << line;
  EXPECT_NE(line.find(" refresh_hz=72 "), std::string::npos) << line;
}

TEST(RenderConfigCollector, ANewRenderTargetIsAChangeNamingBothSides) {
  World w;
  BootApplyWithSelector(w);
  Collector c(w.image, kBase, w.observer, w.Ext());
  ASSERT_FALSE(c.Poll(0).empty());
  w.SetRenderTarget(2000, 2100);
  const std::string line = c.Poll(1000);
  EXPECT_NE(line.find("changed=render_w,render_h "), std::string::npos) << line;
  EXPECT_NE(line.find("render_w=2000 render_h=2100"), std::string::npos) << line;
}

TEST(RenderConfigCollector, WithoutARenderTargetTheLineWaitsFiveSecondsThenIsWrittenAnyway) {
  World w;
  BootApplyWithSelector(w);
  w.image.PokeValue<std::uint64_t>(At(kRenderTargetVa), 0);
  Collector c(w.image, kBase, w.observer, w.Ext());
  EXPECT_EQ(c.Poll(10000), "") << "no render target yet: wait";
  EXPECT_EQ(c.Poll(14000), "");
  EXPECT_EQ(c.Poll(14999), "");
  const std::string line = c.Poll(15000);
  EXPECT_NE(line.find("reason=first_frame changed=- platform=pcvr"), std::string::npos) << line;
  EXPECT_NE(line.find(" render_target=unavailable "), std::string::npos) << line;
  EXPECT_EQ(line.find("render_w="), std::string::npos) << line;
  EXPECT_EQ(c.Poll(16000), "") << "one fallback line, not one a second";

  w.SetRenderTarget(1648, 1776);
  const std::string later = c.Poll(17000);
  EXPECT_NE(later.find("reason=change changed=render_w,render_h,render_target "), std::string::npos) << later;
}

TEST(RenderConfigCollector, ARenderTargetThatAppearsInsideTheGraceWritesTheNormalFirstFrameLine) {
  World w;
  BootApplyWithSelector(w);
  w.image.PokeValue<std::uint64_t>(At(kRenderTargetVa), 0);
  Collector c(w.image, kBase, w.observer, w.Ext());
  ASSERT_EQ(c.Poll(0), "");
  w.SetRenderTarget(1648, 1776);
  EXPECT_EQ(c.Poll(1000), kFirstFrameLine);
}

TEST(RenderConfigCollector, UnreadableMemoryReadsUnavailableAndTheSettingsBeforeAnyApplyReadUnknown) {
  World w;  // no Apply seen
  w.image.DropRegionAt(At(kGpuNameVa));
  w.image.DropRegionAt(At(kHmdDescVa));
  Collector c(w.image, kBase, w.observer, w.Ext());
  const std::string line = c.Poll(0);
  EXPECT_NE(line.find(" gpu=unavailable gpu_vendor=unavailable vram_mb=unavailable "), std::string::npos) << line;
  EXPECT_NE(line.find(" headset=unavailable hmd_type=unavailable "), std::string::npos) << line;
  EXPECT_NE(line.find(" refresh_hz=unavailable hmd_w=unavailable hmd_h=unavailable "), std::string::npos) << line;
  EXPECT_NE(line.find(" settings_source=unknown quality_level=unavailable msaa=unavailable "), std::string::npos) << line;
}

TEST(RenderConfigCollector, ADescriptorWithNoProductNameReadsTheWholeHeadsetGroupUnavailable) {
  World w;
  BootApplyWithSelector(w);
  w.image.PokeText(At(kHmdDescVa) + kHmdProductNameOffset, "");  // the descriptor before VR starts: all zero
  Collector c(w.image, kBase, w.observer, w.Ext());
  const std::string line = c.Poll(0);
  EXPECT_NE(line.find(" headset=unavailable hmd_type=unavailable "), std::string::npos) << line;
  EXPECT_NE(line.find(" refresh_hz=unavailable hmd_w=unavailable hmd_h=unavailable "), std::string::npos) << line;
}

TEST(RenderConfigCollector, TheDriverAndTheRuntimeVersionAreAskedForOnceAtTheFirstLineOnly) {
  World w;
  BootApplyWithSelector(w);
  int driverCalls = 0;
  int versionCalls = 0;
  Collector c(w.image, kBase, w.observer, w.Ext(&driverCalls, &versionCalls));
  w.image.PokeValue<std::uint64_t>(At(kRenderTargetVa), 0);
  EXPECT_EQ(c.Poll(0), "");
  EXPECT_EQ(c.Poll(1000), "");
  EXPECT_EQ(driverCalls + versionCalls, 0) << "the DXGI walk must not run on a pass that writes nothing";
  w.SetRenderTarget(1648, 1776);
  ASSERT_FALSE(c.Poll(2000).empty());
  EXPECT_EQ(driverCalls, 1);
  EXPECT_EQ(versionCalls, 1);
  w.image.PokeValue<float>(At(kHmdDescVa) + kHmdRefreshOffset, 90.0F);
  const std::string change = c.Poll(3000);
  ASSERT_FALSE(change.empty());
  EXPECT_EQ(driverCalls, 1);
  EXPECT_EQ(versionCalls, 1);
  EXPECT_NE(change.find("gpu_driver=31.0.15.3623"), std::string::npos) << change;
  EXPECT_NE(change.find("runtime_version=\"1.43.0\""), std::string::npos) << change;
}

TEST(RenderConfigCollector, ADriverQueryThatFailsReadsUnknown) {
  World w;
  BootApplyWithSelector(w);
  Externals e;
  e.gpuDriver = [](const std::string&) { return std::string("unknown"); };
  e.runtimeVersion = []() { return std::string(); };
  Collector c(w.image, kBase, w.observer, e);
  const std::string line = c.Poll(0);
  EXPECT_NE(line.find(" gpu_driver=unknown "), std::string::npos) << line;
  EXPECT_NE(line.find(" runtime_version=\"\" "), std::string::npos) << line;
}

TEST(RenderConfigFormat, QuotesBackslashesAndControlBytesNeverReachTheLine) {
  Snapshot s;
  s.gpu = {true, "Evil \"GPU\"\\name\r\nsecond\tline\x01"};
  s.headset = {true, "  Rift\"S  "};
  s.runtimeVersion = "1.43\n.0\"x";
  s.gpuDriver = "31.0 15\n";
  const std::string line = FormatLine(s, Reason::Change, {"gpu"});
  EXPECT_EQ(line.find('\n'), std::string::npos);
  EXPECT_EQ(line.find('\r'), std::string::npos);
  EXPECT_EQ(line.find('\t'), std::string::npos);
  EXPECT_EQ(line.find('\x01'), std::string::npos);
  EXPECT_NE(line.find("gpu=\"Evil 'GPU'/name  second line\""), std::string::npos) << line;
  EXPECT_NE(line.find("headset=\"Rift'S\""), std::string::npos) << line;
  EXPECT_NE(line.find("runtime_version=\"1.43 .0'x\""), std::string::npos) << line;
  EXPECT_NE(line.find(" gpu_driver=31.0_15 "), std::string::npos) << line;
  // Every double quote left is one of a field's own pair: an even count.
  EXPECT_EQ(std::count(line.begin(), line.end(), '"') % 2, 0);
}

TEST(RenderConfigFormat, CleanTextReplacesWhatCannotSitInsideQuotes) {
  EXPECT_EQ(CleanText("a\"b\\c"), "a'b/c");
  EXPECT_EQ(CleanText("tab\there"), "tab here");
  EXPECT_EQ(CleanText(std::string("nul\0x", 5)), "nul x");
  EXPECT_EQ(CleanText("caf\xC3\xA9"), "caf??");
  EXPECT_EQ(CleanText("  padded  "), "padded");
  EXPECT_EQ(CleanText("   "), "");
}

TEST(RenderConfigFormat, ChangedIsADashWhenNothingChangedAndAListInLineOrderOtherwise) {
  Snapshot s;
  EXPECT_NE(FormatLine(s, Reason::FirstFrame, {}).find(" changed=- platform=pcvr "), std::string::npos);
  EXPECT_NE(FormatLine(s, Reason::Change, {"msaa", "taa"}).find(" changed=msaa,taa platform=pcvr "), std::string::npos);
  EXPECT_TRUE(ChangedFields(s, s).empty());
}

TEST(RenderConfigSettings, EachFieldIsReadFromItsOffset) {
  World w;
  w.image.PokeValue<std::uint32_t>(kOtherSettings + kSettingsFlagsOffset, 0xAU);  // adaptive + multires
  w.image.PokeValue<std::uint32_t>(kOtherSettings + kSettingsDisplayOffset, 7);
  w.image.PokeValue<float>(kOtherSettings + kSettingsResolutionScaleOffset, 1.5F);
  w.image.PokeValue<float>(kOtherSettings + kSettingsAdaptiveMinOffset, 30.0F);
  w.image.PokeValue<float>(kOtherSettings + kSettingsAdaptiveMaxOffset, 120.0F);
  w.image.PokeValue<std::int32_t>(kOtherSettings + kSettingsAdaptiveTargetOffset, 72);
  w.image.PokeValue<std::int32_t>(kOtherSettings + kSettingsMsaaOffset, 3);
  w.image.PokeValue<std::int32_t>(kOtherSettings + kSettingsQualityLevelOffset, 2);
  GfxSettings g;
  ASSERT_TRUE(ReadGfxSettings(w.image, kOtherSettings, &g));
  EXPECT_EQ(g.flags, 0xAU);
  EXPECT_EQ(g.display, 7U);
  EXPECT_EQ(g.resolutionScale, 1.5F);
  EXPECT_EQ(g.adaptiveMin, 30.0F);
  EXPECT_EQ(g.adaptiveMax, 120.0F);
  EXPECT_EQ(g.adaptiveTarget, 72);
  EXPECT_EQ(g.msaa, 3);
  EXPECT_EQ(g.qualityLevel, 2);
  EXPECT_FALSE(ReadGfxSettings(w.image, 0x50000000ULL, &g));
}

TEST(RenderConfigSettings, TheFlagBitsAreTheOnesTheGameLogs) {
  World w;
  w.image.PokeValue<std::uint32_t>(kSettings + kSettingsFlagsOffset, kFlagAdaptiveResolution | kFlagMultiRes);
  BootApplyWithSelector(w);
  Collector c(w.image, kBase, w.observer, w.Ext());
  const std::string line = c.Poll(0);
  EXPECT_NE(line.find(" adaptive_res=true taa=false multires=true fullscreen=false "), std::string::npos) << line;
}

TEST(RenderConfigSettingsSource, TheBootCallOnTheStructTheSelectorFilledIsAutoSelected) {
  World w;
  w.observer.OnSelector(kSettings);
  w.observer.OnApply(w.image, kSettings, kBootReturn, kBootReturn);
  EXPECT_EQ(w.observer.Snapshot().source, SettingsSource::AutoSelected);
  EXPECT_TRUE(w.observer.Snapshot().seen);
}

TEST(RenderConfigSettingsSource, TheBootCallWithNoSelectorRunIsSavedOrDefault) {
  World w;
  w.observer.OnApply(w.image, kSettings, kBootReturn, kBootReturn);
  EXPECT_EQ(w.observer.Snapshot().source, SettingsSource::SavedOrDefault);
}

TEST(RenderConfigSettingsSource, ASelectorRunOnAnotherStructDoesNotMakeTheBootCallAutoSelected) {
  // The boot code runs the selector on game+0x8dd0 or game+0x8e34 and applies game+0x8dd0.
  World w;
  w.observer.OnSelector(kOtherSettings);
  w.observer.OnApply(w.image, kSettings, kBootReturn, kBootReturn);
  EXPECT_EQ(w.observer.Snapshot().source, SettingsSource::SavedOrDefault);
}

TEST(RenderConfigSettingsSource, AnyOtherCallerIsMenuOrJson) {
  World w;
  w.observer.OnSelector(kSettings);
  w.observer.OnApply(w.image, kSettings, kMenuReturn, kBootReturn);
  EXPECT_EQ(w.observer.Snapshot().source, SettingsSource::MenuOrJson);
}

TEST(RenderConfigSettingsSource, ASettingsStructThatCannotBeReadIsCountedAndChangesNothing) {
  World w;
  w.observer.OnApply(w.image, kSettings, kBootReturn, kBootReturn);
  const auto before = w.observer.Snapshot();
  w.observer.OnApply(w.image, 0x50000000ULL, kMenuReturn, kBootReturn);
  const auto after = w.observer.Snapshot();
  EXPECT_EQ(after.readFailures, 1U);
  EXPECT_EQ(after.applyCalls, 2U);
  EXPECT_EQ(after.source, before.source);
  EXPECT_EQ(after.settings.msaa, before.settings.msaa);

  SettingsObserver fresh;  // nothing read yet: a failed read must not make the settings look seen
  fresh.OnApply(w.image, 0x50000000ULL, kMenuReturn, kBootReturn);
  EXPECT_FALSE(fresh.Snapshot().seen);
  EXPECT_EQ(fresh.Snapshot().source, SettingsSource::Unknown);
}

}  // namespace
