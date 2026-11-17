// #2242 (H-BARR): the barrier copies' value sites (runtime/combat/barrier_copies.h), through the script hook table
// (runtime/hook/script_hooks.h) on a fake copy image. The image is one RWX allocation holding the stock bytes at each
// site, so OnLoaded runs the production path: the rows are checked, then the bytes are written by Apply and ProcessMemcpy.
//
// NOT shown by these tests: that the shipped copy's bytes are the ones the constants name (the copies are made on the
// device from the player's stock barrier, and no game bytes are in the repo), and the use counter, which the code site
// 0x64F1 bumps and is not ported (barrier_copies.h, "NOT ported").

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/logging.h"
#include "runtime/combat/barrier_copies.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace bc = nevr_combat::barrier_copies;
namespace sh = nevr_script_hooks;

std::vector<std::string> g_logged;
std::vector<EchoVR::LogLevel> g_levels;
void CaptureLog(EchoVR::LogLevel level, const CHAR* message) {
  g_levels.push_back(level);
  g_logged.push_back(message);
}
int Count(const char* needle, EchoVR::LogLevel level) {
  int n = 0;
  for (std::size_t i = 0; i < g_logged.size(); ++i) {
    if (g_levels[i] == level && g_logged[i].find(needle) != std::string::npos) ++n;
  }
  return n;
}

constexpr std::uint64_t kStockBarrier = 0xb87ba2039b049d5eULL;  // the energy barrier: never patched
constexpr std::size_t kImageSize = 0x8000;                      // the copy's sites run to 0x74F8

// The stock bytes at the three sites (veil_fx.py:452, :109) and the bytes a changed site holds.
constexpr std::uint8_t kParamStock[8] = {0x48, 0xc7, 0x45, 0xd7, 0xff, 0xff, 0xff, 0xff};
constexpr std::uint8_t kShowOnBytes[5] = {0xc6, 0x44, 0x24, 0x48, 0x01};
constexpr std::uint8_t kShowOffBytes[5] = {0xc6, 0x44, 0x24, 0x48, 0x00};
constexpr std::uint8_t kChangedShow[5] = {0xc6, 0x44, 0x24, 0x48, 0x02};
// The param symbol after the marker is written: the UMBRAL VEIL's "SMOK" and NEUTRON's "POIN", little-endian.
constexpr std::uint8_t kParamSmokeBytes[8] = {0x48, 0xc7, 0x45, 0xd7, 0x4b, 0x4f, 0x4d, 0x53};
constexpr std::uint8_t kParamPointBytes[8] = {0x48, 0xc7, 0x45, 0xd7, 0x50, 0x4f, 0x49, 0x4e};

// A copy image: committed, executable and writable, with the stock bytes at each site.
class FakeCopy {
 public:
  FakeCopy() {
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    std::memcpy(base_ + bc::kParamSymbolRva, kParamStock, sizeof kParamStock);
    std::memcpy(base_ + bc::kPrimingShowRvaA, kShowOnBytes, sizeof kShowOnBytes);
    std::memcpy(base_ + bc::kPrimingShowRvaB, kShowOnBytes, sizeof kShowOnBytes);
  }
  ~FakeCopy() { VirtualFree(base_, 0, MEM_RELEASE); }
  std::uint8_t* base() const { return base_; }
  std::uint8_t* At(std::uint32_t rva) const { return base_ + rva; }

 private:
  std::uint8_t* base_ = nullptr;
};

class BarrierCopies : public ::testing::Test {
 protected:
  void SetUp() override {
    sh::ResetForTest();
    bc::ResetForTest();
    g_logged.clear();
    g_levels.clear();
    SetBootLogSink(&CaptureLog);
    bc::Register();
  }
  void TearDown() override {
    sh::ResetForTest();
    bc::ResetForTest();
    SetBootLogSink(nullptr);
  }
};

TEST_F(BarrierCopies, RegistersOnceAndLogsTheRegistrationOnce) {
  bc::Register();  // a second call does nothing
  EXPECT_EQ(Count("barrier copies fix registered", EchoVR::LogLevel::Info), 1);
}

TEST_F(BarrierCopies, UmbralVeilCopyLoadsWithItsMarkerAndNoPrimingHologram) {
  FakeCopy copy;
  sh::OnLoaded(bc::kSmokeScript, copy.base());
  // "SMOK" (0x534D4F4B) as the little-endian imm32 after `mov qword [rbp-29h], imm32` (veil_fx.py:450-452).
  EXPECT_EQ(std::memcmp(copy.At(bc::kParamSymbolRva), kParamSmokeBytes, sizeof kParamSmokeBytes), 0);
  EXPECT_EQ(std::memcmp(copy.At(bc::kPrimingShowRvaA), kShowOffBytes, sizeof kShowOffBytes), 0);
  EXPECT_EQ(std::memcmp(copy.At(bc::kPrimingShowRvaB), kShowOffBytes, sizeof kShowOffBytes), 0);
  EXPECT_EQ(Count("script hooks installed", EchoVR::LogLevel::Info), 1);
  EXPECT_EQ(Count("patches=3", EchoVR::LogLevel::Info), 1);
  sh::OnUnloading(bc::kSmokeScript, copy.base());
}

TEST_F(BarrierCopies, NeutronCopyLoadsWithThePointMarker) {
  FakeCopy copy;
  sh::OnLoaded(bc::kPointScript, copy.base());
  // "POIN" (0x4E494F50), veil_fx.py:519.
  EXPECT_EQ(std::memcmp(copy.At(bc::kParamSymbolRva), kParamPointBytes, sizeof kParamPointBytes), 0);
  EXPECT_EQ(std::memcmp(copy.At(bc::kPrimingShowRvaA), kShowOffBytes, sizeof kShowOffBytes), 0);
  EXPECT_EQ(std::memcmp(copy.At(bc::kPrimingShowRvaB), kShowOffBytes, sizeof kShowOffBytes), 0);
  sh::OnUnloading(bc::kPointScript, copy.base());
}

TEST_F(BarrierCopies, TheStockBarrierIsNotPatched) {
  FakeCopy copy;
  sh::OnLoaded(kStockBarrier, copy.base());
  EXPECT_EQ(std::memcmp(copy.At(bc::kParamSymbolRva), kParamStock, sizeof kParamStock), 0);
  EXPECT_EQ(std::memcmp(copy.At(bc::kPrimingShowRvaA), kShowOnBytes, sizeof kShowOnBytes), 0);
  EXPECT_EQ(std::memcmp(copy.At(bc::kPrimingShowRvaB), kShowOnBytes, sizeof kShowOnBytes), 0);
  EXPECT_EQ(Count("patch not installed", EchoVR::LogLevel::Warning), 0);
  sh::OnUnloading(kStockBarrier, copy.base());
}

TEST_F(BarrierCopies, AChangedSiteRefusesEveryPatchAndWritesNothing) {
  FakeCopy copy;
  // One byte of the second priming site differs from what the patch was written against.
  std::memcpy(copy.At(bc::kPrimingShowRvaB), kChangedShow, sizeof kChangedShow);
  sh::OnLoaded(bc::kSmokeScript, copy.base());

  // Nothing is written: the marker and the first priming site keep their stock bytes, and the second keeps its own.
  EXPECT_EQ(std::memcmp(copy.At(bc::kParamSymbolRva), kParamStock, sizeof kParamStock), 0);
  EXPECT_EQ(std::memcmp(copy.At(bc::kPrimingShowRvaA), kShowOnBytes, sizeof kShowOnBytes), 0);
  EXPECT_EQ(std::memcmp(copy.At(bc::kPrimingShowRvaB), kChangedShow, sizeof kChangedShow), 0);

  // Rule 6: the refused site is named with its address and the bytes expected and found.
  ASSERT_EQ(Count("patch not installed", EchoVR::LogLevel::Warning), 1);
  EXPECT_EQ(Count("script hooks refused", EchoVR::LogLevel::Warning), 1);
  bool named = false;
  for (const std::string& line : g_logged) {
    if (line.find("patch not installed") == std::string::npos) continue;
    named = line.find("name=\"BARRIER COPY: UMBRAL VEIL priming hologram off (ShowModel at 0x74F8)\"") != std::string::npos &&
            line.find("expected=c644244801") != std::string::npos && line.find("actual=c644244802") != std::string::npos &&
            line.find("va=") != std::string::npos;
  }
  EXPECT_TRUE(named);
  EXPECT_EQ(Count("script hooks installed", EchoVR::LogLevel::Info), 0);
  sh::OnUnloading(bc::kSmokeScript, copy.base());
}

}  // namespace
