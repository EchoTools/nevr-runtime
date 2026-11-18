// #2242 (H-POD-a): the podium terminal's rules page 2 (runtime/combat/podium_pages.h). The page logic is pure and is driven
// directly. The six rows are installed through the script hook table (runtime/hook/script_hooks.h) on a fake terminal image
// (one RWX allocation holding each row's stock bytes at its RVA, and the stock 20-byte MenuPage body), and the installed
// detours are called through their entry points, so MinHook's trampolines and the table's prologue check run as in a game.
//
// NOT shown by these tests: that the shipped echovr.exe's terminal runs these rows (no live run here); the stock bodies the
// tests run are the fake image's bytes, which are the pins, not the game's code; the enable checks' stock path (MenuPage
// other than 1 calls the stock body, which a fake image cannot run); the start button in a match; the press chain that reads
// the press flag (a later piece).

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <MinHook.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/combat/podium_pages.h"
#include "runtime/combat/rules_state.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace pp = nevr_combat::podium_pages;
namespace rules = nevr_combat::rules;
namespace sh = nevr_script_hooks;

// The sizes the fake memory uses. Every RVA of a row is below kImageBytes.
constexpr std::size_t kImageBytes = 0x40000;
constexpr std::size_t kCtxBytes = 0x200;   // the node context: [0] node pointer, next node at +10h
constexpr std::size_t kNodeBytes = 0x100;  // the script node: the data pointer at +68h
constexpr std::size_t kDataBytes = 0x400;  // the script data: MenuPage at +1A8h
constexpr std::size_t kMailboxBytes = 0x40;  // the mailbox: the payload map byte at +30h
constexpr std::size_t kPointerBytes = sizeof(std::uint64_t);
constexpr std::size_t kMenuPageBytes = sizeof(std::int32_t);
constexpr std::size_t kNextBytes = sizeof(std::uint64_t);

// The stock values the tests pin (fix_rules_pages.py, cited at each).
constexpr std::uint64_t kExpectedScript = 0xd4653fc2065e8184ULL;  // the podium terminal's script
constexpr std::uint32_t kExpectedRestartRva = 0x1A1C0;
constexpr std::uint32_t kExpectedMenu1Rva = 0x209C0;
constexpr std::uint32_t kExpectedRulesRva = 0x11310;
constexpr std::uint32_t kExpectedEnableRva = 0x23810;
constexpr std::uint32_t kExpectedMatchRva = 0x209E0;
constexpr std::uint32_t kExpectedAlignRva = 0x35720;
constexpr std::uint32_t kExpectedDataPointer = 0x68;
constexpr std::uint32_t kExpectedMenuPage = 0x1A8;
constexpr std::uint32_t kExpectedNodeNext = 0x10;
constexpr std::uint32_t kExpectedPayloadMap = 0x30;  // the mailbox's payload map (rules::kPayloadMap)
constexpr std::uint64_t kExpectedExecTrue = 0x56CCD1BD99A59D3BULL;
constexpr std::uint64_t kExpectedHideFrom = 0xF8953074ULL;
constexpr std::uint64_t kExpectedShowFrom = 0x9A09FD90ULL;
constexpr std::uint64_t kExpectedAlign = 1;
constexpr std::size_t kExpectedMenu1Size = 20;  // the whole stock body (fix_rules_pages.py MENU1_BODY)
constexpr std::size_t kExpectedRowCount = 6;
constexpr std::uint8_t kPayloadOff = 0;  // a payload map is 1; a plain page is 0
constexpr std::uint8_t kPayloadOn = 1;
constexpr std::int32_t kMenuPageNone = 0;
constexpr std::int32_t kMenuPageOther = 2;  // any page but the rules page
constexpr std::int32_t kMenuPageMinusOne = -1;
constexpr std::uint8_t kPageOne = pp::kPageOne;
constexpr std::uint8_t kPageTwo = pp::kPageTwo;
constexpr std::uint8_t kPressBackValue = pp::kPressBack;
constexpr std::uint8_t kPressRulesValue = pp::kPressRules;
constexpr std::int32_t kRulesPage = pp::kMenuPageRules;
constexpr std::size_t kCorruptOffset = 2;   // the RESTART row's third byte is changed for the refusal test
constexpr std::uint8_t kCorruptValue = 0x00;
// The RULES row's stock tail after its 6-byte prologue: add rsp, 58h; ret. Lets the stock call run on the fake image.
constexpr std::uint8_t kRulesTailBytes[] = {0x48, 0x83, 0xc4, 0x58, 0xc3};

// The stock bytes: each row's prologue (kRestartBytes ... kAlignCheckBytes) and the stock MenuPage body (kMenu1Bytes).
using NodeFn = std::uint64_t (*)(void*);

// The fake image's RVAs, each with its bytes; a row whose RVA is at the image's end would read past it, so the image is
// kImageBytes and the largest RVA plus its row is well inside.
struct RowBytes {
  std::uint32_t rva;
  const std::uint8_t* bytes;
  std::size_t size;
};
constexpr RowBytes kRows[] = {
    {kExpectedRestartRva, pp::kRestartBytes, sizeof pp::kRestartBytes},
    {kExpectedMenu1Rva, pp::kMenu1Bytes, sizeof pp::kMenu1Bytes},
    {kExpectedRulesRva, pp::kRulesCheckBytes, sizeof pp::kRulesCheckBytes},
    {kExpectedEnableRva, pp::kEnableCheckBytes, sizeof pp::kEnableCheckBytes},
    {kExpectedMatchRva, pp::kMatchCheckBytes, sizeof pp::kMatchCheckBytes},
    {kExpectedAlignRva, pp::kAlignCheckBytes, sizeof pp::kAlignCheckBytes},
};

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

// A fake script terminal: an RWX image with the six rows' bytes at their RVAs, and a node context whose pointers lead to
// the data (MenuPage) and the mailbox (payload) the detours read.
struct FakeTerminal {
  std::uint8_t* base = nullptr;
  std::uint8_t ctx[kCtxBytes] = {};
  std::uint8_t node[kNodeBytes] = {};
  std::uint8_t data[kDataBytes] = {};
  volatile std::uint8_t mailbox[kMailboxBytes] = {};

  FakeTerminal() {
    base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    for (const RowBytes& row : kRows) std::memcpy(base + row.rva, row.bytes, row.size);
    std::memcpy(base + kExpectedRulesRva + sizeof pp::kRulesCheckBytes, kRulesTailBytes, sizeof kRulesTailBytes);
    // The stock MenuPage body reads [ctx] and [node + 68h] (rcx = data), then writes data + 1A8h (fix_rules_pages.py MENU1_BODY).
    std::uintptr_t nodeAddress = reinterpret_cast<std::uintptr_t>(node);
    std::uintptr_t dataAddress = reinterpret_cast<std::uintptr_t>(data);
    std::memcpy(ctx, &nodeAddress, kPointerBytes);
    std::memcpy(node + kExpectedDataPointer, &dataAddress, kPointerBytes);
    SetMenuPage(kRulesPage);
    SetPayload(kPayloadOff);
  }
  // The table forgets its rows but does not take installed detours back (ResetForTest); a detour left on a freed image
  // would be called by the next test's image, so each row's hook is removed first (a row that was never installed reports
  // not-created, which is not a failure here).
  ~FakeTerminal() {
    for (const RowBytes& row : kRows) MH_RemoveHook(base + row.rva);
    VirtualFree(base, 0, MEM_RELEASE);
  }

  void SetMenuPage(std::int32_t page) { std::memcpy(data + kExpectedMenuPage, &page, kMenuPageBytes); }
  void SetPayload(std::uint8_t payload) { mailbox[rules::kPayloadMap] = payload; }
  std::uint64_t Next() const {
    std::uint64_t next = 0;
    std::memcpy(&next, ctx + kExpectedNodeNext, kNextBytes);
    return next;
  }
  void* Ctx() { return ctx; }
  std::uint8_t* At(std::uint32_t rva) const { return base + rva; }
  NodeFn Entry(std::uint32_t rva) const { return reinterpret_cast<NodeFn>(base + rva); }
};

// Registers the six rows with the table as Register would, and loads the fake terminal.
void Install(FakeTerminal& t) {
  sh::Hook rows[pp::kRowCount];
  pp::Rows(rows);
  sh::Register(rows, pp::kRowCount);
  pp::SetMailboxForTest(t.mailbox);
  sh::OnLoaded(kExpectedScript, t.base);
}

class PodiumPages : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    pp::ResetForTest();
    g_logged.clear();
    g_levels.clear();
    SetBootLogSink(&CaptureLog);
  }
  void TearDown() override {
    sh::ResetForTest();
    pp::ResetForTest();
    SetBootLogSink(nullptr);
  }
};

// --- the pins ----------------------------------------------------------------------------------------------------------

// The MENU1 body, written from Cat's record (fix_rules_pages.py MENU1_BODY, :107), not from the module's constant. The
// module's bytes are checked against this literal, so a transcription error in kMenu1Bytes fails here.
TEST_F(PodiumPages, MenuOneBodyIsCatsRecordedBytes) {
  constexpr std::uint8_t kCatsMenu1Body[] = {0x48, 0x8b, 0x01, 0x48, 0x8b, 0x48, 0x68, 0x33, 0xc0, 0xc7, 0x81,
                                             0xa8, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc3};
  ASSERT_EQ(sizeof pp::kMenu1Bytes, sizeof kCatsMenu1Body);
  EXPECT_EQ(std::memcmp(pp::kMenu1Bytes, kCatsMenu1Body, sizeof kCatsMenu1Body), 0);
}

TEST_F(PodiumPages, PinsAreTheStockScriptRvasSizesAndValues) {
  EXPECT_EQ(pp::kScript, kExpectedScript);
  EXPECT_EQ(pp::kRestartRva, kExpectedRestartRva);
  EXPECT_EQ(pp::kMenu1Rva, kExpectedMenu1Rva);
  EXPECT_EQ(pp::kRulesCheckRva, kExpectedRulesRva);
  EXPECT_EQ(pp::kEnableCheckRva, kExpectedEnableRva);
  EXPECT_EQ(pp::kMatchCheckRva, kExpectedMatchRva);
  EXPECT_EQ(pp::kAlignCheckRva, kExpectedAlignRva);
  EXPECT_EQ(pp::kDataPointer, kExpectedDataPointer);
  EXPECT_EQ(pp::kMenuPage, kExpectedMenuPage);
  EXPECT_EQ(pp::kNodeNext, kExpectedNodeNext);
  EXPECT_EQ(rules::kPayloadMap, kExpectedPayloadMap);
  EXPECT_EQ(pp::kExecTrue, kExpectedExecTrue);
  EXPECT_EQ(pp::kHideFrom, kExpectedHideFrom);
  EXPECT_EQ(pp::kShowFrom, kExpectedShowFrom);
  EXPECT_EQ(pp::kAlignResult, kExpectedAlign);
  EXPECT_EQ(pp::kRowCount, kExpectedRowCount);
  EXPECT_EQ(sizeof pp::kMenu1Bytes, kExpectedMenu1Size);
}

TEST_F(PodiumPages, PlanNamesSixRowsAtTheSixRvasWithTheirBytes) {
  sh::Hook rows[pp::kRowCount];
  pp::Rows(rows);
  for (std::size_t i = 0; i < pp::kRowCount; ++i) {
    EXPECT_EQ(rows[i].script, kExpectedScript);
    EXPECT_EQ(rows[i].rva, kRows[i].rva);
    EXPECT_EQ(rows[i].expected_len, kRows[i].size);
    EXPECT_NE(rows[i].detour, nullptr);
    EXPECT_NE(rows[i].original, nullptr);
    EXPECT_NE(rows[i].name, nullptr);
  }
}

// --- the page logic (pure) ----------------------------------------------------------------------------------------------

TEST_F(PodiumPages, NextFromPageOneGoesToPageTwoAndBackToPageOne) {
  pp::PageState state;
  pp::MenuOneStarts(state, kPayloadOff);
  EXPECT_EQ(state.page, kPageOne);
  EXPECT_EQ(pp::RestartNext(state, kPayloadOff), kExpectedHideFrom);  // page 1 -> 2: the labels hide
  EXPECT_EQ(state.page, kPageTwo);
  EXPECT_EQ(pp::RestartNext(state, kPayloadOff), kExpectedShowFrom);  // page 2 -> 1: the labels show
  EXPECT_EQ(state.page, kPageOne);
}

TEST_F(PodiumPages, APayloadMapKeepsItsPageAndOnlyHides) {
  pp::PageState state;
  pp::MenuOneStarts(state, kPayloadOn);
  EXPECT_EQ(state.page, kPageTwo) << "a payload map opens on page 2, its only page";
  EXPECT_EQ(pp::RestartNext(state, kPayloadOn), kExpectedHideFrom);
  EXPECT_EQ(state.page, kPageTwo) << "NEXT keeps the page on a payload map";
}

TEST_F(PodiumPages, RulesAndBackSetThePageAndThePressFlag) {
  pp::PageState state;
  pp::RulesPress(state, /*backNow=*/false, kPayloadOn);
  EXPECT_EQ(state.page, kPageTwo);
  EXPECT_EQ(state.press, kPressRulesValue);
  pp::RulesPress(state, /*backNow=*/true, kPayloadOn);
  EXPECT_EQ(state.page, kPageOne) << "BACK goes to page 1 whatever the payload";
  EXPECT_EQ(state.press, kPressBackValue);
  pp::RulesPress(state, /*backNow=*/false, kPayloadOff);
  EXPECT_EQ(state.page, kPageOne);
  EXPECT_EQ(state.press, kPressRulesValue);
}

TEST_F(PodiumPages, MenuPageIsReadThroughTheNodeAndItsData) {
  FakeTerminal t;
  t.SetMenuPage(kRulesPage);
  EXPECT_EQ(pp::MenuPageOf(t.Ctx()), kRulesPage);
  t.SetMenuPage(kMenuPageNone);
  EXPECT_EQ(pp::MenuPageOf(t.Ctx()), kMenuPageNone);
  EXPECT_EQ(pp::MenuPageOf(nullptr), kMenuPageNone) << "a null context reads as not the rules page";
  std::uint64_t none = 0;
  std::memcpy(t.ctx, &none, kPointerBytes);
  EXPECT_EQ(pp::MenuPageOf(t.Ctx()), kMenuPageNone) << "a null node reads as not the rules page";
}

TEST_F(PodiumPages, EnableIsTrueOnlyOnTheRulesPage) {
  EXPECT_TRUE(pp::EnableShowsTrue(kRulesPage));
  EXPECT_FALSE(pp::EnableShowsTrue(kMenuPageNone));
  EXPECT_FALSE(pp::EnableShowsTrue(kMenuPageOther));
  EXPECT_FALSE(pp::EnableShowsTrue(kMenuPageMinusOne));
}

// --- the installed rows (the real table, MinHook, the fake image) ---------------------------------------------------------

TEST_F(PodiumPages, TheSixRowsInstallWhenTheImageMatchesAndTheDetoursRunThePageAsPageOneTwoOne) {
  FakeTerminal t;
  sh::Hook rows[pp::kRowCount];
  pp::Rows(rows);
  sh::Register(rows, pp::kRowCount);
  pp::SetMailboxForTest(t.mailbox);
  sh::OnLoaded(kExpectedScript, t.base);
  EXPECT_EQ(Count("script hooks installed script=d4653fc2065e8184 base=", EchoVR::LogLevel::Info), 1);
  EXPECT_EQ(Count("hooks=6 slots=0 pins=0", EchoVR::LogLevel::Info), 1);
  EXPECT_EQ(Count("hook not installed", EchoVR::LogLevel::Warning), 0);

  // The start button's checks: EXEC_TRUE on the rules page, without the stock body (the fake image cannot run it).
  EXPECT_EQ(t.Entry(kExpectedEnableRva)(t.Ctx()), kExpectedExecTrue);
  EXPECT_EQ(t.Entry(kExpectedMatchRva)(t.Ctx()), kExpectedExecTrue);
  // The alignment check returns 1 and never runs its stock body.
  EXPECT_EQ(t.Entry(kExpectedAlignRva)(nullptr), kExpectedAlign);

  // The page, from the start: MENU1 runs the stock body (MenuPage = 1 in the context) and starts on page 1.
  EXPECT_EQ(t.Entry(kExpectedMenu1Rva)(t.Ctx()), 0u);
  // NEXT: page 1 -> 2, the labels hide.
  EXPECT_EQ(t.Entry(kExpectedRestartRva)(t.Ctx()), 0u);
  EXPECT_EQ(t.Next(), kExpectedHideFrom);
  // NEXT again: page 2 -> 1, the labels show.
  EXPECT_EQ(t.Entry(kExpectedRestartRva)(t.Ctx()), 0u);
  EXPECT_EQ(t.Next(), kExpectedShowFrom);
}

TEST_F(PodiumPages, RulesEntryTakesPageTwoOnRulesAndPageOneOnBack) {
  FakeTerminal t;
  Install(t);
  // RULES (MenuPage is not 1) on a payload map: page 2.
  t.SetPayload(kPayloadOn);
  t.SetMenuPage(kMenuPageNone);
  t.Entry(kExpectedRulesRva)(t.Ctx());
  // NEXT on the rules page, now without a payload map: page 2 -> 1, the labels show.
  t.SetMenuPage(kRulesPage);
  t.SetPayload(kPayloadOff);
  t.Entry(kExpectedRestartRva)(t.Ctx());
  EXPECT_EQ(t.Next(), kExpectedShowFrom) << "RULES on a payload map left page 2";
  // BACK (MenuPage is 1) on a payload map: page 1, so the next NEXT goes to page 2.
  t.SetPayload(kPayloadOn);
  t.Entry(kExpectedRulesRva)(t.Ctx());
  t.SetPayload(kPayloadOff);
  t.SetMenuPage(kRulesPage);
  t.Entry(kExpectedRestartRva)(t.Ctx());
  EXPECT_EQ(t.Next(), kExpectedHideFrom) << "BACK left page 1 where NEXT goes to page 2";
}

TEST_F(PodiumPages, MenuOneOnAPayloadMapStartsOnPageTwo) {
  FakeTerminal t;
  Install(t);
  // The node that starts the rules page, on a payload map: the page is 2, so a NEXT with no payload map flips it to 1.
  t.SetPayload(kPayloadOn);
  EXPECT_EQ(t.Entry(kExpectedMenu1Rva)(t.Ctx()), 0u);
  t.SetPayload(kPayloadOff);
  t.Entry(kExpectedRestartRva)(t.Ctx());
  EXPECT_EQ(t.Next(), kExpectedShowFrom) << "MENU1 on a payload map did not start on page 2";
}

TEST_F(PodiumPages, AChangedByteRefusesTheWholeTerminalWithRuleSixFieldsAndWritesNothing) {
  FakeTerminal t;
  t.base[kExpectedRestartRva + kCorruptOffset] = kCorruptValue;  // the RESTART row's bytes are not the stock ones
  std::vector<std::uint8_t> before(t.base, t.base + kImageBytes);
  sh::Hook rows[pp::kRowCount];
  pp::Rows(rows);
  sh::Register(rows, pp::kRowCount);
  sh::OnLoaded(kExpectedScript, t.base);
  EXPECT_EQ(Count("hook not installed script=d4653fc2065e8184 name=\"podium RESTART", EchoVR::LogLevel::Warning), 1);
  EXPECT_EQ(Count("rva=0x1a1c0", EchoVR::LogLevel::Warning), 1);
  EXPECT_EQ(Count("expected=4881eca8040000 actual=488100a8040000", EchoVR::LogLevel::Warning), 1)
      << "the Rule 6 fields: the row's stock bytes and the bytes the image holds";
  EXPECT_EQ(Count("script hooks refused script=d4653fc2065e8184", EchoVR::LogLevel::Warning), 1);
  EXPECT_EQ(Count("script hooks installed", EchoVR::LogLevel::Info), 0);
  EXPECT_EQ(std::memcmp(before.data(), t.base, kImageBytes), 0) << "a refused terminal writes no byte of the image";
}

TEST_F(PodiumPages, RegisterLogsOnceWithItsConsequence) {
  pp::Register();
  pp::Register();  // a second call is a no-op
  EXPECT_EQ(Count("podium pages fix registered script=d4653fc2065e8184 hooks=6", EchoVR::LogLevel::Info), 1);
}

}  // namespace
