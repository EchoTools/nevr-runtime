// #600: the RULES page's time rows, in Combat, set the capture point's times (runtime/combat/capture_time.h): the
// decisions, and the nine hooks and two pins installed through the script hook table under MinHook on a fake
// game-mode script.
//
// Each fake function is its row's pinned bytes, the stock code from its start through its last +0xA8 access, so the
// row handlers' own arithmetic and +0xDB check run as shipped. Then comes a tail this test writes in the stock
// function's frame:
//  - seconds down and up: `add rsp, 0x58; ret` (the stock tail);
//  - minutes down and up: `add rsp, 0x20; pop rbx; ret` (the stock tail); each calls its helper, pinned, whose tail
//    here adds 60 to the value it read and returns it as a float (the stock helper computes the next minutes value);
//  - the five readers: `mov eax, ecx; add rsp, 0xA8; ret`, so each returns the int it read (the stock ones publish it).
// The name string the pinned code copies is laid out at 0x2FA09. The node, its context and the globals are test
// buffers with nonzero bytes around every field the hooks read or write.
//
// NOT shown by these tests: the shipped script's bytes in a run, and a podium press in a Combat match.

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
#include "runtime/combat/capture_time.h"
#include "runtime/combat/rules_state.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace ct = nevr_combat::capture_time;
namespace rules = nevr_combat::rules;
namespace sh = nevr_script_hooks;

constexpr std::uint32_t kNameRva = 0x2FA09;
constexpr std::size_t kImage = 0x30000;
constexpr std::uint32_t kSecondsFF50 = 0xFF50, kSeconds10A20 = 0x10A20, kMinutesFA60 = 0xFA60, kMinutes14FB0 = 0x14FB0;
constexpr std::uint32_t kReaders[] = {0x16160, 0x13E00, 0x12860, 0x11F90, 0x16050};

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

void Emit(std::vector<std::uint8_t>& v, std::initializer_list<std::uint8_t> b) { v.insert(v.end(), b); }
float Float(const std::uint8_t* at) {
  float f = 0;
  std::memcpy(&f, at, sizeof f);
  return f;
}
void SetFloat(std::uint8_t* at, float f) { std::memcpy(at, &f, sizeof f); }

TEST(CaptureTimeDecision, TheValueIsTOrTheMapsOwnAndTheCaptureTimeIsTMod60) {
  std::uint8_t box[0x40];
  std::memset(box, 0xa5, sizeof box);
  SetFloat(box + rules::kRoundTotal, 0.0f);
  const rules::RulesState r(box);
  EXPECT_FALSE(r.RoundTotalSet());
  EXPECT_EQ(ct::PageValue(r), 2704.0f) << "45 * 60 + 4 while unset";
  SetFloat(box + rules::kRoundTotal, -5.0f);
  EXPECT_FALSE(r.RoundTotalSet()) << "a negative float's bits are a negative int32";
  EXPECT_EQ(ct::PageValue(r), 2704.0f);
  SetFloat(box + rules::kRoundTotal, 125.0f);
  EXPECT_TRUE(r.RoundTotalSet());
  EXPECT_EQ(ct::PageValue(r), 125.0f);
  EXPECT_EQ(ct::CaptureOf(125.0f), 5.0f);
  EXPECT_EQ(ct::CaptureOf(125.9f), 5.0f) << "cvttss2si truncates";
  EXPECT_EQ(ct::CaptureOf(120.0f), 0.0f);
  EXPECT_EQ(ct::CaptureOf(-61.0f), -1.0f) << "idiv's remainder takes the dividend's sign";
  r.SetRoundTime(7.0f, 8.0f);
  EXPECT_EQ(Float(box + rules::kRoundTotal), 7.0f);
  EXPECT_EQ(Float(box + rules::kCaptureTime), 8.0f);
  EXPECT_EQ(box[rules::kCaptureTime + 4], 0xa5);
  EXPECT_EQ(rules::kCaptureTime, 0x0Cu) << "VeilNet's CAPTURE_SLOT";
  EXPECT_FALSE(rules::RulesState(nullptr).RoundTotalSet());
}

TEST(CaptureTimeTable, NineRowsEachEndingOnTheirLastPageAccess) {
  ASSERT_EQ(ct::kPageCount, 9u);
  for (const ct::Page& p : ct::kPages) {
    std::uint32_t disp = 0;
    std::memcpy(&disp, p.expected + p.expected_len - 4, sizeof disp);
    EXPECT_EQ(disp, ct::kPageTime) << p.name;
  }
  EXPECT_TRUE(ct::kPages[0].writes && ct::kPages[1].writes && ct::kPages[2].writes && ct::kPages[3].writes);
  for (std::size_t i = 4; i < ct::kPageCount; ++i) EXPECT_FALSE(ct::kPages[i].writes) << ct::kPages[i].name;
}

class CaptureTimeInstalled : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    ct::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    std::memset(box_, 0xa5, sizeof box_);
    SetFloat(box_ + rules::kRoundTotal, 0.0f);
    SetFloat(box_ + rules::kCaptureTime, 0.0f);
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);
    for (const ct::Page& p : ct::kPages) {
      std::vector<std::uint8_t> v(p.expected, p.expected + p.expected_len);
      v.reserve(v.size() + 16);
      if (p.rva == kSecondsFF50 || p.rva == kSeconds10A20) {
        Emit(v, {0x48, 0x83, 0xc4, 0x58, 0xc3});
      } else if (p.rva == kMinutesFA60 || p.rva == kMinutes14FB0) {
        Emit(v, {0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3});
      } else {
        Emit(v, {0x89, 0xc8, 0x48, 0x81, 0xc4, 0xa8, 0x00, 0x00, 0x00, 0xc3});
      }
      std::memcpy(base_ + p.rva, v.data(), v.size());
    }
    for (const auto& [rva, pin] : {std::pair{0x23240u, ct::kMinutesHelper23240}, std::pair{0x28930u, ct::kMinutesHelper28930}}) {
      std::vector<std::uint8_t> v(pin, pin + sizeof ct::kMinutesHelper23240);
      Emit(v, {0x41, 0x83, 0xc1, 0x3c, 0xf3, 0x41, 0x0f, 0x2a, 0xc1, 0x48, 0x83, 0xc4, 0x58, 0xc3});  // r9d += 60
      std::memcpy(base_ + rva, v.data(), v.size());
    }
    std::memcpy(base_ + kNameRva, "clock", 6);
    std::memset(node_, 0xa5, sizeof node_);
    std::memset(context_, 0xa5, sizeof context_);
    std::memset(globals_, 0xa5, sizeof globals_);
    std::uint8_t* context = context_;
    std::memcpy(node_, &context, sizeof context);
    node_[0xDB] = 0;
    std::uint8_t* globals = globals_;
    std::memcpy(context_ + ct::kContextGlobals, &globals, sizeof globals);
    SetFloat(globals_ + ct::kPageTime, 600.0f);  // the match clock
  }
  void TearDown() override {
    for (const ct::Page& p : ct::kPages) MH_RemoveHook(base_ + p.rva);
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    ct::ResetForTest();
    SetBootLogSink(nullptr);
  }
  std::uint64_t Call(std::uint32_t rva) { return reinterpret_cast<std::uint64_t (*)(void*)>(base_ + rva)(node_); }
  float Clock() const { return Float(globals_ + ct::kPageTime); }
  float T() const { return Float(box_ + rules::kRoundTotal); }
  float Capture() const { return Float(box_ + rules::kCaptureTime); }
  void Load() {
    ct::Register(box_);
    sh::OnLoaded(ct::kScript, base_);
  }
  std::uint8_t* base_ = nullptr;
  std::uint8_t box_[0x40];
  std::uint8_t node_[0x100];
  std::uint8_t context_[0x80];
  std::uint8_t globals_[0xC0];
};

TEST_F(CaptureTimeInstalled, TheReadersSeeTAndTheClockIsUntouched) {
  Load();
  EXPECT_EQ(Count("script hooks installed script=6fa8cc9cd721a563"), 1);
  EXPECT_EQ(Count("hooks=9 slots=0 pins=2"), 1);
  for (std::uint32_t rva : kReaders) {
    SCOPED_TRACE(rva);
    EXPECT_EQ(Call(rva), 2704u) << "unset: the maps' own 45:04";
    EXPECT_EQ(Clock(), 600.0f) << "the match clock is put back";
  }
  SetFloat(box_ + rules::kRoundTotal, 125.0f);
  for (std::uint32_t rva : kReaders) EXPECT_EQ(Call(rva), 125u);
  EXPECT_EQ(T(), 125.0f) << "a reader writes no T";
  // A reader that returns 0 (as a stock one can: its setter's answer) writes no T either, even while T is unset.
  std::uint8_t* tail = base_ + 0x16160 + ct::kPages[4].expected_len;
  ASSERT_EQ(tail[0], 0x89);
  tail[0] = 0x31;  // mov eax, ecx -> xor eax, eax
  tail[1] = 0xc0;
  SetFloat(box_ + rules::kRoundTotal, 0.0f);
  EXPECT_EQ(Call(0x16160), 0u);
  EXPECT_EQ(T(), 0.0f) << "still unset";
  EXPECT_EQ(globals_[ct::kPageTime + 4], 0xa5);
  EXPECT_EQ(globals_[ct::kPageTime - 1], 0xa5);
}

TEST_F(CaptureTimeInstalled, TheRowHandlersWriteTAndTheCaptureTime) {
  // What each stock handler writes for 125 (2:05) and for 2704 (45:04), run before the hooks are in: the hooked
  // handler must write the same to T, and leave the clock alone.
  const std::uint32_t handlers[] = {kSecondsFF50, kSeconds10A20, kMinutesFA60, kMinutes14FB0};
  float from125[4], from2704[4];
  for (int i = 0; i < 4; ++i) {
    SetFloat(globals_ + ct::kPageTime, 125.0f);
    ASSERT_EQ(Call(handlers[i]), 0u);
    from125[i] = Clock();
    SetFloat(globals_ + ct::kPageTime, 2704.0f);
    Call(handlers[i]);
    from2704[i] = Clock();
  }
  EXPECT_EQ(from125[0], 126.0f) << "0xFF50 raises the seconds";
  EXPECT_EQ(from125[3], 185.0f) << "the test's helper adds 60";
  SetFloat(globals_ + ct::kPageTime, 600.0f);
  Load();
  for (int i = 0; i < 4; ++i) {
    SCOPED_TRACE(i);
    SetFloat(box_ + rules::kRoundTotal, 125.0f);
    EXPECT_EQ(Call(handlers[i]), 0u);
    EXPECT_EQ(T(), from125[i]) << "the stock arithmetic, on T";
    EXPECT_EQ(Capture(), static_cast<float>(static_cast<int>(from125[i]) % 60));
    EXPECT_EQ(Clock(), 600.0f) << "the match clock is never touched";
    SetFloat(box_ + rules::kRoundTotal, 0.0f);
    EXPECT_EQ(Call(handlers[i]), 0u);
    EXPECT_EQ(T(), from2704[i]) << "unset: from the maps' own 45:04";
    EXPECT_EQ(Clock(), 600.0f);
  }
  // +0xDB set: the handler writes nothing and returns -1, so T stands.
  SetFloat(box_ + rules::kRoundTotal, 125.0f);
  SetFloat(box_ + rules::kCaptureTime, 9.0f);
  node_[0xDB] = 1;
  for (std::uint32_t h : handlers) {
    EXPECT_EQ(Call(h), ~std::uint64_t{0});
    EXPECT_EQ(T(), 125.0f);
    EXPECT_EQ(Capture(), 9.0f);
    EXPECT_EQ(Clock(), 600.0f);
  }
  node_[0xDB] = 0;
}

TEST_F(CaptureTimeInstalled, EachPressThatChangesTIsLoggedOnce) {
  Load();
  SetFloat(box_ + rules::kRoundTotal, 125.0f);
  for (std::uint32_t rva : kReaders) Call(rva);
  EXPECT_EQ(Count("capture time: T changed"), 0) << "a reader logs nothing";
  EXPECT_EQ(Call(kSecondsFF50), 0u);
  EXPECT_EQ(Count("capture time: T changed from=125 to=126 capture=6 script=6fa8cc9cd721a563"), 1);
  node_[0xDB] = 1;
  EXPECT_EQ(Call(kSecondsFF50), ~std::uint64_t{0});
  node_[0xDB] = 0;
  EXPECT_EQ(Count("capture time: T changed"), 1) << "+0xDB set: nothing written, nothing logged";
  SetFloat(box_ + rules::kRoundTotal, 0.0f);
  EXPECT_EQ(Call(kSecondsFF50), 0u);
  EXPECT_EQ(Count("capture time: T changed from=2704 to=2705 capture=5"), 1) << "unset: from the maps' own 45:04";
  EXPECT_EQ(Count("capture time: T changed"), 2);
}

TEST_F(CaptureTimeInstalled, StockAfterTheFree) {
  Load();
  sh::OnUnloading(ct::kScript, base_);
  SetFloat(box_ + rules::kRoundTotal, 125.0f);
  EXPECT_EQ(Call(0x16160), 600u) << "the stock reader reads the clock";
  EXPECT_EQ(Call(kSecondsFF50), 0u);
  EXPECT_EQ(Clock(), 601.0f) << "the stock handler writes the clock (10:00 -> 10:01)";
  EXPECT_EQ(T(), 125.0f);
}

TEST_F(CaptureTimeInstalled, AHelperWhoseBytesDifferRefusesTheScript) {
  base_[0x23274 + 5] = 0xB0;  // the helper reads +0xB0, not the page value
  Load();
  EXPECT_EQ(Count("script hooks refused script=6fa8cc9cd721a563"), 1);
  SetFloat(box_ + rules::kRoundTotal, 125.0f);
  EXPECT_EQ(Call(0x16160), 600u) << "the whole script runs stock";
}

TEST_F(CaptureTimeInstalled, RegisteringTwiceAddsTheRowsOnce) {
  ct::Register(box_);
  ct::Register(box_);
  sh::OnLoaded(ct::kScript, base_);
  EXPECT_EQ(Count("capture time fix registered"), 1);
  EXPECT_EQ(Count("hooks=9 slots=0 pins=2"), 1);
}

}  // namespace
