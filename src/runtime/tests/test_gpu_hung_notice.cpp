#include "runtime/log/gpu_hung_notice.h"

#include <gtest/gtest.h>

namespace {

using nevr_gpu_hung::Action;
using nevr_gpu_hung::Tracker;

constexpr char kHungLine[] =
    "[EVR] The D3D device has been removed with reason 'DXGI_ERROR_DEVICE_HUNG' (0x887A0006)";

TEST(GpuHungNotice, OnlyTheHungReasonMatches) {
  EXPECT_TRUE(nevr_gpu_hung::IsDeviceHungLine(kHungLine));
  EXPECT_FALSE(nevr_gpu_hung::IsDeviceHungLine(
      "[EVR] The D3D device has been removed with reason 'DXGI_ERROR_DEVICE_REMOVED' (0x887A0005)"));
  EXPECT_FALSE(nevr_gpu_hung::IsDeviceHungLine(
      "[EVR] The D3D device has been removed with reason 'DXGI_ERROR_DEVICE_RESET'"));
  EXPECT_FALSE(nevr_gpu_hung::IsDeviceHungLine("[EVR] DXGI_ERROR_DEVICE_HUNG mentioned in a settings key"));
  EXPECT_FALSE(nevr_gpu_hung::IsDeviceHungLine(""));
}

TEST(GpuHungNotice, AnUnrelatedLineDoesNothing) {
  Tracker tracker;
  const auto result = tracker.Observe("[EVR] Frame time 11ms", false, false);
  EXPECT_EQ(result.action, Action::None);
  EXPECT_FALSE(tracker.handled());
}

TEST(GpuHungNotice, AClientGetsTheRecordAndTheDialog) {
  Tracker tracker;
  tracker.Observe("[EVR] Video Card: NVIDIA GeForce RTX 2070 SUPER", false, false);
  tracker.Observe("[EVR] Driver Version: r610_85 (61088)", false, false);
  const auto result = tracker.Observe(kHungLine, false, false);
  EXPECT_EQ(result.action, Action::LogAndNotify);
  EXPECT_NE(result.log_line.find("gpu=\"NVIDIA GeForce RTX 2070 SUPER\""), std::string::npos) << result.log_line;
  EXPECT_NE(result.log_line.find("driver=\"r610_85 (61088)\""), std::string::npos) << result.log_line;
  EXPECT_NE(result.log_line.find("https://www.meta.com/help/quest/294035639100350/"), std::string::npos);
  EXPECT_NE(result.dialog_text.find("DXGI_ERROR_DEVICE_HUNG"), std::string::npos);
  EXPECT_NE(result.dialog_text.find("https://www.meta.com/help/quest/294035639100350/"), std::string::npos);
}

TEST(GpuHungNotice, TheCardAndDriverAreUnknownWhenTheGameHasNotNamedThemYet) {
  // Measured: the System Info dump arrives after the hung line, so the first line often has neither.
  Tracker tracker;
  const auto result = tracker.Observe(kHungLine, false, false);
  EXPECT_EQ(result.action, Action::LogAndNotify);
  EXPECT_NE(result.log_line.find("gpu=\"unknown\""), std::string::npos) << result.log_line;
  EXPECT_NE(result.log_line.find("driver=\"unknown\""), std::string::npos) << result.log_line;
}

TEST(GpuHungNotice, TheNoticeIsOnceOnlyPerRun) {
  Tracker tracker;
  EXPECT_EQ(tracker.Observe(kHungLine, false, false).action, Action::LogAndNotify);
  EXPECT_EQ(tracker.Observe(kHungLine, false, false).action, Action::None);
  EXPECT_EQ(tracker.Observe(kHungLine, false, false).action, Action::None);
}

TEST(GpuHungNotice, ADedicatedServerGetsTheRecordNeverTheDialog) {
  Tracker tracker;
  const auto result = tracker.Observe(kHungLine, /*isServer=*/true, /*isHeadless=*/true);
  EXPECT_EQ(result.action, Action::LogOnly);
  EXPECT_FALSE(result.log_line.empty());
}

TEST(GpuHungNotice, AHeadlessRunWithoutServerFlagIsAlsoLogOnly) {
  Tracker tracker;
  EXPECT_EQ(tracker.Observe(kHungLine, /*isServer=*/false, /*isHeadless=*/true).action, Action::LogOnly);
}

TEST(GpuHungNotice, LabelValuesAreTrimmed) {
  EXPECT_EQ(nevr_gpu_hung::ValueAfterLabel("[EVR] Video Card:   NVIDIA X  \r\n", "Video Card:"), "NVIDIA X");
  EXPECT_EQ(nevr_gpu_hung::ValueAfterLabel("[EVR] nothing here", "Video Card:"), "");
}

}  // namespace
