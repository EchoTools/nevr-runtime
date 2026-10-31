// #1774: the loading-tip node hooks (patch/loading_tips.h) installed into a hand-assembled image under real MinHook.
//
// The image is one committed RWX page. Each "stock function" is the game's prologue bytes, then a body that counts
// its own call and returns, so a call that reaches the body is visible as a counter. A thunk in the same page calls a
// function with sentinel values in the callee-saved registers and stores them back, so the detour's register
// discipline is checked. No game binary is needed. The pages are not freed: MinHook keeps its trampolines in them.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/patch/loading_tips.h"

BOOL g_isServer = TRUE;  // hook_liveness.cpp reads it; the runtime defines it in cli.cpp

namespace {

using nevr_loading_tips::InstallSites;
using nevr_loading_tips::Site;

constexpr std::size_t kSiteCount = 3;
constexpr std::size_t kPageSize = 4096;
constexpr std::uint32_t kFnOffset[kSiteCount] = {0x100, 0x200, 0x300};
constexpr std::uint32_t kCounterOffset[kSiteCount] = {0x800, 0x804, 0x808};
constexpr std::uint32_t kThunkOffset = 0x400;
constexpr std::uint32_t kOutOffset = 0x900;

// The game's bytes. Pick and Select (0x140bd9670, 0x140be7c90) start with the same 16 bytes; ByIndex (0x140be6d10)
// pushes RBX as well, so its 17 bytes are the whole prologue and its first 16 are what the installer checks.
const std::vector<std::uint8_t> kPickFull = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
                                             0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
const std::vector<std::uint8_t> kByIndexFull = {0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41,
                                                0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
const std::array<std::uint8_t, 16> kPickChecked = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
                                                   0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
const std::array<std::uint8_t, 16> kByIndexChecked = {0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56,
                                                      0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41};
// Pops that undo the pushes above, in reverse order.
const std::vector<std::uint8_t> kPickPops = {0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D};
const std::vector<std::uint8_t> kByIndexPops = {0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C,
                                                0x5F, 0x5E, 0x5B, 0x5D};

// The sentinels the thunk leaves in RBX, RBP, RSI, RDI, R12, R13, R14, R15 before the call.
const std::array<std::uint64_t, 8> kSentinels = {0x1111111111111111ULL, 0x2222222222222222ULL,
                                                 0x3333333333333333ULL, 0x4444444444444444ULL,
                                                 0x5555555555555555ULL, 0x6666666666666666ULL,
                                                 0x7777777777777777ULL, 0x8888888888888888ULL};
const char* const kRegNames[8] = {"rbx", "rbp", "rsi", "rdi", "r12", "r13", "r14", "r15"};

void AppendImm64(std::vector<std::uint8_t>& code, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) code.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

void Append(std::vector<std::uint8_t>& code, std::initializer_list<std::uint8_t> bytes) {
  code.insert(code.end(), bytes.begin(), bytes.end());
}

// The body of a stock function: its prologue, `MOV RAX, counter; INC DWORD [RAX]`, the pops, RET.
void WriteStub(std::uint8_t* at, const std::vector<std::uint8_t>& prologue, const std::vector<std::uint8_t>& pops,
               std::uint64_t counter) {
  std::vector<std::uint8_t> code = prologue;
  Append(code, {0x48, 0xB8});
  AppendImm64(code, counter);
  Append(code, {0xFF, 0x00});
  code.insert(code.end(), pops.begin(), pops.end());
  code.push_back(0xC3);
  std::memcpy(at, code.data(), code.size());
}

// A thunk: saves RCX (the out array), loads the sentinels, the arguments and the target, calls it, then stores
// every callee-saved register into out[0..7]. Entered as void(uint64_t* out).
std::vector<std::uint8_t> ThunkCode(std::uint64_t target, std::uint64_t node, std::uint64_t data) {
  std::vector<std::uint8_t> code;
  Append(code, {0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});  // push rbx,rbp,rsi,rdi,r12-r15
  Append(code, {0x51});                                                                    // push rcx (out)
  Append(code, {0x48, 0x83, 0xEC, 0x20});                                                  // sub rsp,0x20
  // movabs rbx, rbp, rsi, rdi (REX.W) and r12-r15 (REX.W+B), each followed by its imm64.
  static const std::uint8_t kMovabs[8][2] = {{0x48, 0xBB}, {0x48, 0xBD}, {0x48, 0xBE}, {0x48, 0xBF},
                                             {0x49, 0xBC}, {0x49, 0xBD}, {0x49, 0xBE}, {0x49, 0xBF}};
  for (std::size_t r = 0; r < 8; ++r) {
    code.push_back(kMovabs[r][0]);
    code.push_back(kMovabs[r][1]);
    AppendImm64(code, kSentinels[r]);
  }
  Append(code, {0x48, 0xB9});  // mov rcx, node
  AppendImm64(code, node);
  Append(code, {0x48, 0xBA});  // mov rdx, data
  AppendImm64(code, data);
  Append(code, {0x48, 0xB8});  // mov rax, target
  AppendImm64(code, target);
  Append(code, {0xFF, 0xD0});                  // call rax
  Append(code, {0x48, 0x83, 0xC4, 0x20});      // add rsp,0x20
  Append(code, {0x59});                        // pop rcx (out)
  Append(code, {0x48, 0x89, 0x19});            // mov [rcx],rbx
  Append(code, {0x48, 0x89, 0x69, 0x08});      // mov [rcx+0x08],rbp
  Append(code, {0x48, 0x89, 0x71, 0x10});      // mov [rcx+0x10],rsi
  Append(code, {0x48, 0x89, 0x79, 0x18});      // mov [rcx+0x18],rdi
  Append(code, {0x4C, 0x89, 0x61, 0x20});      // mov [rcx+0x20],r12
  Append(code, {0x4C, 0x89, 0x69, 0x28});      // mov [rcx+0x28],r13
  Append(code, {0x4C, 0x89, 0x71, 0x30});      // mov [rcx+0x30],r14
  Append(code, {0x4C, 0x89, 0x79, 0x38});      // mov [rcx+0x38],r15
  Append(code, {0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, 0x5B});  // pops
  Append(code, {0xC3});                                                                    // ret
  return code;
}

// The image: the three stock functions with their counters at zero, and room for the thunk and its out array.
struct Image {
  std::uint8_t* page{nullptr};
  std::array<Site, kSiteCount> sites{};

  std::uint8_t* Fn(std::size_t i) const { return page + kFnOffset[i]; }
  std::uint64_t FnAddress(std::size_t i) const { return reinterpret_cast<std::uint64_t>(Fn(i)); }
  std::uint32_t Counter(std::size_t i) const {
    std::uint32_t value = 0;
    std::memcpy(&value, page + kCounterOffset[i], sizeof(value));
    return value;
  }
};

// The tests call the stock functions and the thunk, so each page is executable; a fresh page per test keeps the
// hooks of one test out of the next.
Image MakeImage() {
  Image img;
  img.page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kPageSize, MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_EXECUTE_READWRITE));
  if (img.page == nullptr) return img;
  const auto base = reinterpret_cast<std::uint64_t>(img.page);
  WriteStub(img.page + kFnOffset[0], kPickFull, kPickPops, base + kCounterOffset[0]);
  WriteStub(img.page + kFnOffset[1], kByIndexFull, kByIndexPops, base + kCounterOffset[1]);
  WriteStub(img.page + kFnOffset[2], kPickFull, kPickPops, base + kCounterOffset[2]);
  // The site rows: the RVA is the offset from the image base, which is the page here.
  img.sites = {{
      Site{"CR15PickLoadingTipNode::Enter", kFnOffset[0], kPickChecked},
      Site{"CR15SelectLoadingTipByIndexNode::Enter", kFnOffset[1], kByIndexChecked},
      Site{"CR15SelectLoadingTipNode::Enter", kFnOffset[2], kPickChecked},
  }};
  return img;
}

void CallSite(const Image& img, std::size_t i) {
  using StockFn = void (*)(void* node, void* data);
  reinterpret_cast<StockFn>(img.Fn(i))(reinterpret_cast<void*>(0x1000), nullptr);
}

struct Captured {
  EchoVR::LogLevel level;
  std::string text;
};
std::vector<Captured> g_logged;

void CaptureLog(EchoVR::LogLevel level, const CHAR* message) { g_logged.push_back(Captured{level, message}); }

bool Logged(EchoVR::LogLevel level, const std::string& needle) {
  for (const Captured& line : g_logged) {
    if (line.level == level && line.text.find(needle) != std::string::npos) return true;
  }
  return false;
}

class LoadingTips : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
  }
  void TearDown() override { SetBootLogSink(nullptr); }
};

TEST_F(LoadingTips, EachMatchingSiteIsHookedAndItsStockBodyNoLongerRuns) {
  Image img = MakeImage();
  ASSERT_NE(img.page, nullptr);
  ASSERT_EQ(InstallSites(reinterpret_cast<std::uintptr_t>(img.page), img.sites.data(), img.sites.size()), 3u);

  for (std::size_t i = 0; i < kSiteCount; ++i) CallSite(img, i);

  for (std::size_t i = 0; i < kSiteCount; ++i) {
    EXPECT_EQ(img.Counter(i), 0u) << img.sites[i].name << " ran its stock body after the hook";
    EXPECT_TRUE(Logged(EchoVR::LogLevel::Info,
                       std::string("loading tip node skipped name=") + img.sites[i].name + " count=1"))
        << img.sites[i].name << ": no first-hit line";
  }
}

TEST_F(LoadingTips, ADetourDoesNotCallTheOriginal) {
  Image img = MakeImage();
  ASSERT_NE(img.page, nullptr);
  ASSERT_EQ(InstallSites(reinterpret_cast<std::uintptr_t>(img.page), img.sites.data(), 1), 1u);
  CallSite(img, 0);
  CallSite(img, 0);
  CallSite(img, 1);  // the unhooked site runs its stock body
  EXPECT_EQ(img.Counter(0), 0u) << "the trampoline (the stock body) was called";
  EXPECT_EQ(img.Counter(1), 1u) << "an unhooked site was not left alone";
}

TEST_F(LoadingTips, ASiteWhoseBytesDifferIsRefusedAndTheOthersStillHook) {
  Image img = MakeImage();
  ASSERT_NE(img.page, nullptr);
  img.sites[1].prologue[0] = 0x49;  // the table expects bytes the game does not hold at this site

  ASSERT_EQ(InstallSites(reinterpret_cast<std::uintptr_t>(img.page), img.sites.data(), img.sites.size()), 2u);
  EXPECT_EQ(img.Fn(1)[0], 0x48) << "the refused site was written";

  for (std::size_t i = 0; i < kSiteCount; ++i) CallSite(img, i);
  EXPECT_EQ(img.Counter(0), 0u);
  EXPECT_EQ(img.Counter(1), 1u) << "the refused site did not run its stock body";
  EXPECT_EQ(img.Counter(2), 0u);
  EXPECT_TRUE(Logged(EchoVR::LogLevel::Warning,
                     "hook failed name=CR15SelectLoadingTipByIndexNode::Enter"))
      << "the refusal was not logged by name";
  EXPECT_TRUE(Logged(EchoVR::LogLevel::Warning, "reason=prologue_mismatch"));
  EXPECT_TRUE(Logged(EchoVR::LogLevel::Warning, "hooks=2/3 failed=1"));
}

TEST_F(LoadingTips, CalleeSavedRegistersAndTheStackSurviveEachDetour) {
  Image img = MakeImage();
  ASSERT_NE(img.page, nullptr);
  ASSERT_EQ(InstallSites(reinterpret_cast<std::uintptr_t>(img.page), img.sites.data(), img.sites.size()), 3u);

  using ThunkFn = void (*)(std::uint64_t* out);
  for (std::size_t i = 0; i < kSiteCount; ++i) {
    const std::vector<std::uint8_t> code = ThunkCode(img.FnAddress(i), 0x1000, 0x2000);
    std::memcpy(img.page + kThunkOffset, code.data(), code.size());
    auto* out = reinterpret_cast<std::uint64_t*>(img.page + kOutOffset);
    std::memset(out, 0, 8 * sizeof(std::uint64_t));

    reinterpret_cast<ThunkFn>(img.page + kThunkOffset)(out);  // returns only if the stack came back balanced

    for (std::size_t r = 0; r < 8; ++r) {
      EXPECT_EQ(out[r], kSentinels[r]) << img.sites[i].name << " changed " << kRegNames[r];
    }
  }
}

TEST_F(LoadingTips, TheAggregateLineIsAnInfoLineNamingAllThreeSites) {
  Image img = MakeImage();
  ASSERT_NE(img.page, nullptr);
  ASSERT_EQ(InstallSites(reinterpret_cast<std::uintptr_t>(img.page), img.sites.data(), img.sites.size()), 3u);
  EXPECT_TRUE(Logged(EchoVR::LogLevel::Info,
                     "Disabled loading tips system for server mode hooks=3/3 failed=0"));
  EXPECT_FALSE(Logged(EchoVR::LogLevel::Warning, "Disabled loading tips system"));
}

}  // namespace
