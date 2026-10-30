// Host test for the CREATE GAME / JOIN VIA CODE observer on libr15's MetricLog (ADR 0017, #966): the real observer
// (src/quest/invite/wizard_metric_hook.cpp), built without exceptions as the sentinel builds it, driven through the thunk's
// entry against a stand-in for the game's MetricLog. The stand-in original records its arguments. No GOT slot in a real
// libr15 is touched here: got_pinned_test.cpp resolves the slot and the prologue in the store file, and
// tests/quest TestHookFramesCarryNoPersonality covers the frames of the built artifact.
//
// Built and run by `just test-quest-hooks`.

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "got_hook.h"
#include "hook_log.h"
#include "pinned_targets.h"
#include "quest/invite/wizard_metric_hook.h"
#include "quest/tests/test_check.h"
#include "runtime/patch/invite_wizard.h"

namespace {

namespace wm = nevr_quest::wizard_metric;
using Thunk = sentinel::pinned::LibR15MetricLogThunk;

// ---- the log ---------------------------------------------------------------------------------------

std::mutex g_logMutex;
std::vector<std::string> g_lines;

void Sink(sentinel::LogLevel, const char* line) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_lines.emplace_back(line);
}

std::vector<std::string> Lines(const char* event) {
  const std::string needle = std::string("\"event\":\"") + event + "\"";
  std::vector<std::string> out;
  std::lock_guard<std::mutex> lock(g_logMutex);
  for (const std::string& l : g_lines) {
    if (l.find(needle) != std::string::npos) out.push_back(l);
  }
  return out;
}

std::vector<std::string> AllLines() {
  std::lock_guard<std::mutex> lock(g_logMutex);
  return g_lines;
}

std::string Field(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t at = line.find(needle);
  if (at == std::string::npos) return "<absent>";
  const std::size_t begin = at + needle.size();
  const std::size_t end = line.find_first_of(",}", begin);
  return line.substr(begin, end - begin);
}

// ---- the stand-in original (the game's MetricLog) ----------------------------------------------------

struct Original {
  int calls = 0;
  const char* json = nullptr;
  unsigned long long length = 0;
};
Original g_original;

void FakeOriginal(const char* json, unsigned long long length) {
  ++g_original.calls;
  g_original.json = json;
  g_original.length = length;
}

void Reset() {
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_lines.clear();
  }
  g_original = Original();
  wm::ResetForTest();
  Thunk::Reset();
  *Thunk::OriginalOut() = reinterpret_cast<void*>(&FakeOriginal);
  wm::ArmForTest();
}

// The call the game makes: through the thunk's entry.
__attribute__((noinline)) void Call(const char* json, unsigned long long length) {
  const Thunk::Fn entry = reinterpret_cast<Thunk::Fn>(Thunk::EntryAddress());
  entry(json, length);
  asm volatile("" ::: "memory");
}

// ---- the lines the wizard sends -----------------------------------------------------------------------

const std::string kCreate =
    R"({"panel_type":"deep_linking","panel_name":"create_game_review_settings","event_name":"create_invite",)"
    R"("Server_region":"US WEST","Expires_after":"3 HOURS"})";
const std::string kJoin =
    R"({"panel_type":"deep_linking","panel_name":"join_via_code","event_name":"join","join_code":"k7q-x2m9"})";
// The code kJoin types, as the normaliser makes it: it must never be in a log line.
constexpr const char* kCodeUpper = "K7QX2M9";

// ---- tests -----------------------------------------------------------------------------------------

void AnUnarmedThunkPassesThroughAndObservesNothing() {
  Reset();
  Thunk::Disarm();
  Call(kJoin.c_str(), kJoin.size());
  QCHECK(g_original.calls == 1 && g_original.json == kJoin.c_str() && g_original.length == kJoin.size());
  QCHECK(wm::CurrentTotals().calls == 0 && wm::CurrentTotals().copies == 0);
  QCHECK(AllLines().empty());
}

void ACreateClickIsLoggedAndHeldBecauseNoChoicesAreSeen() {
  Reset();
  Call(kCreate.c_str(), kCreate.size());
  QCHECK(g_original.calls == 1 && g_original.json == kCreate.c_str() && g_original.length == kCreate.size());
  const std::vector<std::string> clicks = Lines("invite_click");
  QCHECK(clicks.size() == 1);
  if (clicks.size() == 1) {
    QCHECK(Field(clicks[0], "click") == "\"create_invite\"");
    QCHECK(Field(clicks[0], "panel") == "\"deep_linking\"");
    QCHECK(Field(clicks[0], "tag") == "\"NEVR.INVITE\"");
  }
  const std::vector<std::string> held = Lines("invite_request_not_sent");
  QCHECK(held.size() == 1);
  if (held.size() == 1) {
    QCHECK(Field(held[0], "reason") == "\"incomplete\"");
    QCHECK(Field(held[0], "op") == "\"create\"");
    QCHECK(Field(held[0], "code_len") == "0");
  }
  QCHECK(wm::CurrentTotals().wizard == 1 && wm::CurrentTotals().submits == 0);
}

void AJoinWithACodeIsLoggedAndHeldWithoutSubmit() {
  Reset();
  Call(kJoin.c_str(), kJoin.size());
  QCHECK(g_original.calls == 1 && g_original.json == kJoin.c_str() && g_original.length == kJoin.size());
  const std::vector<std::string> clicks = Lines("invite_click");
  QCHECK(clicks.size() == 1);
  if (clicks.size() == 1) QCHECK(Field(clicks[0], "click") == "\"join_via_code\"");
  // "k7q-x2m9" normalises to seven symbols: the length is logged, the code is not.
  const std::vector<std::string> held = Lines("invite_request_not_sent");
  QCHECK(held.size() == 1);
  if (held.size() == 1) {
    QCHECK(Field(held[0], "reason") == "\"observe_only\"");
    QCHECK(Field(held[0], "op") == "\"resolve\"");
    QCHECK(Field(held[0], "code_len") == "7");
  }
  QCHECK(wm::CurrentTotals().wizard == 1 && wm::CurrentTotals().submits == 0);
}

void AnUnusableJoinCodeSaysWhyAndNothingIsSent() {
  struct Case {
    const char* what;
    std::string line;
    const char* reason;
  };
  const std::string prefix = R"({"panel_type":"deep_linking","panel_name":"join_via_code","event_name":"join","join_code":")";
  const Case cases[] = {
      {"the game's placeholder", prefix + R"(<invalid>"})", "\"empty\""},
      {"an empty code", prefix + R"("})", "\"empty\""},
      {"a symbol outside the alphabet", prefix + R"(K7QX2M0"})", "\"malformed\""},
      {"sixty-four symbols", prefix + std::string(64, 'A') + R"("})", "\"too_long\""},
  };
  for (const Case& c : cases) {
    Reset();
    Call(c.line.c_str(), c.line.size());
    const std::vector<std::string> held = Lines("invite_request_not_sent");
    if (held.size() != 1 || Field(held[0], "reason") != c.reason) {
      std::fprintf(stderr, "case '%s': expected one not-sent line with reason %s\n", c.what, c.reason);
      QCHECK(false);
    }
    QCHECK(wm::CurrentTotals().submits == 0);
  }
}

void ANonWizardLineLogsNothing() {
  Reset();
  const std::string otherPanel =
      R"({"panel_type":"main_menu","panel_name":"join_via_code","event_name":"join","join_code":"K7QX2M9"})";
  Call(otherPanel.c_str(), otherPanel.size());
  const std::string otherName = R"({"panel_type":"deep_linking","panel_name":"settings","event_name":"open"})";
  Call(otherName.c_str(), otherName.size());
  QCHECK(AllLines().empty());
  QCHECK(wm::CurrentTotals().calls == 2 && wm::CurrentTotals().copies == 2 && wm::CurrentTotals().wizard == 0);
  QCHECK(g_original.calls == 2);
}

void AnEscapedPanelNameIsLeftToTheSharedPrefilter() {
  // The shared prefilter looks for the literal text deep_linking before any parse, so an escaped spelling is not a wizard
  // line for this observer (the PC observer does the same: invite_wizard.h, MayBeWizardMetric).
  Reset();
  // The text holds a backslash-u escape where the word's underscore would be, so the literal deep_linking is absent.
  // Built from a non-raw literal so the backslash is one the test wrote, not one a raw literal kept by accident.
  const std::string escaped = std::string(R"({"panel_type":"deep)") + "\\u005f" +
                              R"(linking","panel_name":"join_via_code","event_name":"join","join_code":"K7QX2M9"})";
  QCHECK(escaped.find("deep_linking") == std::string::npos);
  QCHECK(escaped.find("deep\\u005flinking") != std::string::npos);
  Call(escaped.c_str(), escaped.size());
  QCHECK(AllLines().empty());
  QCHECK(wm::CurrentTotals().wizard == 0 && wm::CurrentTotals().submits == 0);
  QCHECK(g_original.calls == 1);
}

void NullZeroAndOversizeLinesAreNeverCopied() {
  Reset();
  Call(nullptr, 10);
  Call(kJoin.c_str(), 0);
  std::vector<char> big(nevr_invite::kMetricMaxBytes + 1, ' ');
  Call(big.data(), nevr_invite::kMetricMaxBytes + 1);
  QCHECK(wm::CurrentTotals().calls == 3 && wm::CurrentTotals().copies == 0);
  QCHECK(AllLines().empty());
  QCHECK(g_original.calls == 3);
  QCHECK(g_original.json == big.data() && g_original.length == nevr_invite::kMetricMaxBytes + 1);
}

void AnUnreadableLineIsWarnedAndStillPassedOn() {
  Reset();
  const std::size_t pageSize = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  void* page = mmap(nullptr, pageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  QCHECK(page != MAP_FAILED);
  if (page == MAP_FAILED) return;
  const char* unreadable = static_cast<const char*>(page);
  Call(unreadable, 64);
  QCHECK(g_original.calls == 1 && g_original.json == unreadable && g_original.length == 64);
  const std::vector<std::string> skips = Lines("invite_observer_skip");
  QCHECK(skips.size() == 1);
  if (skips.size() == 1) {
    QCHECK(Field(skips[0], "reason") == "\"copy_failed\"");
    QCHECK(Field(skips[0], "bytes") == "64");
  }
  QCHECK(wm::CurrentTotals().copies == 1 && wm::CurrentTotals().wizard == 0);
  munmap(page, pageSize);
}

void CopyGameBytesCopiesWithinItsCapacityAndNoFurther() {
  const char source[] = "abcdef";
  char destination[16] = {};
  QCHECK(wm::CopyGameBytes(source, 6, destination, sizeof(destination)));
  QCHECK(std::memcmp(destination, source, 6) == 0);

  // More than the capacity is refused, and nothing past the capacity is written.
  char wide[80];
  std::memset(wide, 'x', sizeof(wide));
  char out[80];
  std::memset(out, 0xAB, sizeof(out));
  QCHECK(!wm::CopyGameBytes(wide, sizeof(wide), out, 64));
  bool untouched = true;
  for (std::size_t i = 64; i < sizeof(out); ++i) {
    if (out[i] != static_cast<char>(0xAB)) untouched = false;
  }
  QCHECK(untouched);

  // An unreadable source is refused.
  const std::size_t pageSize = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  void* page = mmap(nullptr, pageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  QCHECK(page != MAP_FAILED);
  if (page != MAP_FAILED) {
    QCHECK(!wm::CopyGameBytes(static_cast<const char*>(page), 16, destination, sizeof(destination)));
    munmap(page, pageSize);
  }
}

void TheSubmitHandOffStaysOffWhileTheSwitchIsOff() {
  Reset();
  for (int i = 0; i < 3; ++i) Call(kJoin.c_str(), kJoin.size());
  QCHECK(wm::CurrentTotals().wizard == 3 && wm::CurrentTotals().submits == 0);
  QCHECK(Lines("invite_request_not_sent").size() == 3);
  QCHECK(g_original.calls == 3);
}

void NoLineCarriesTheCodeAndEveryLineIsTagged() {
  Reset();
  Call(kJoin.c_str(), kJoin.size());
  Call(kCreate.c_str(), kCreate.size());
  const std::vector<std::string> lines = AllLines();
  QCHECK(!lines.empty());
  for (const std::string& line : lines) {
    QCHECK(line.find(kCodeUpper) == std::string::npos);
    QCHECK(line.find("k7q-x2m9") == std::string::npos);
    QCHECK(line.find("k7qx2m9") == std::string::npos);
    QCHECK(line.find("\"tag\":\"NEVR.INVITE\"") != std::string::npos);
  }
}

// The PC-side shape of the pinned bytes is what the observer checks before it installs.
void ThePrologueCheckAcceptsTheStoreBytesAndNoOthers() {
  unsigned char code[16];
  std::memcpy(code, sentinel::pinned::kMetricLogCode, sizeof(code));
  QCHECK(wm::PrologueMatches(code));
  for (std::size_t i = 0; i < sizeof(code); ++i) {
    unsigned char changed[16];
    std::memcpy(changed, code, sizeof(code));
    changed[i] = static_cast<unsigned char>(changed[i] ^ 0x01);
    QCHECK(!wm::PrologueMatches(changed));
  }
}

// Every Skip has the token the not-sent line names. Some (NoLevel, NotConfigured, NoToken, Busy, Stopped) cannot come out of
// this observer's PlanRequest call, so only this test pins them: a swapped token is caught here and nowhere else.
void EverySkipHasItsPinnedToken() {
  struct Case {
    nevr_invite::Skip skip;
    const char* token;
  };
  const Case cases[] = {
      {nevr_invite::Skip::None, "none"},
      {nevr_invite::Skip::Empty, "empty"},
      {nevr_invite::Skip::Malformed, "malformed"},
      {nevr_invite::Skip::TooLong, "too_long"},
      {nevr_invite::Skip::Incomplete, "incomplete"},
      {nevr_invite::Skip::NoLevel, "no_level"},
      {nevr_invite::Skip::NotConfigured, "not_configured"},
      {nevr_invite::Skip::NoToken, "no_token"},
      {nevr_invite::Skip::Busy, "busy"},
      {nevr_invite::Skip::Stopped, "stopped"},
  };
  for (const Case& c : cases) {
    const char* got = wm::SkipToken(c.skip);
    if (std::strcmp(got, c.token) != 0) {
      std::fprintf(stderr, "SkipToken: expected %s, got %s\n", c.token, got);
      QCHECK(false);
    }
  }
}

// An image whose MetricLog prologue lives in g_fakePrologue: the observer reads its prologue at base + kMetricLogVaddr,
// so the base is that address less the pinned offset. No program headers: the GOT step finds no slot and refuses.
unsigned char g_fakePrologue[16];

bool FakeLookup(const char* /*module*/, sentinel::ElfImage* out) {
  out->base = reinterpret_cast<std::uintptr_t>(g_fakePrologue) - sentinel::pinned::kMetricLogVaddr;
  out->phdr = nullptr;
  out->phnum = 0;
  return true;
}

void InstallNotPatchedRefusesAndLogsIt() {
  Reset();
  QCHECK(!wm::Install());  // the process has no libr15 loaded
  const std::vector<std::string> off = Lines("invite_observer_off");
  QCHECK(off.size() == 1);
  if (off.size() == 1) QCHECK(Field(off[0], "reason") == "\"not_patched\"");
  QCHECK(Lines("invite_observer_installed").empty());
  QCHECK(wm::CurrentTotals().calls == 0 && wm::CurrentTotals().copies == 0);
}

void InstallWithAWrongPrologueRefusesBeforeTheSlotAndArmsNothing() {
  unsigned char good[16];
  std::memcpy(good, sentinel::pinned::kMetricLogCode, sizeof(good));
  std::memcpy(g_fakePrologue, good, sizeof(good));
  g_fakePrologue[0] = static_cast<unsigned char>(g_fakePrologue[0] ^ 0x01);
  Reset();
  Thunk::Disarm();
  QCHECK(!wm::Install(&FakeLookup));
  const std::vector<std::string> off = Lines("invite_observer_off");
  QCHECK(off.size() == 1);
  if (off.size() == 1) {
    QCHECK(Field(off[0], "reason") == "\"prologue_mismatch\"");
    QCHECK(Field(off[0], "va") == "\"0x000000000120cc50\"");
    QCHECK(Field(off[0], "expected") == "\"" + nevr_invite::HexBytes(good, sizeof(good)) + "\"");
    QCHECK(Field(off[0], "actual") == "\"" + nevr_invite::HexBytes(g_fakePrologue, sizeof(g_fakePrologue)) + "\"");
  }
  QCHECK(Lines("invite_observer_installed").empty());
  // Still disarmed: a call from the game reaches the original and nothing else.
  Call(kJoin.c_str(), kJoin.size());
  QCHECK(wm::CurrentTotals().calls == 0 && g_original.calls == 1);
}

void InstallWithTheStockPrologueTriesTheSlotAndSaysWhyItRefused() {
  std::memcpy(g_fakePrologue, sentinel::pinned::kMetricLogCode, sizeof(g_fakePrologue));
  Reset();
  Thunk::Disarm();
  QCHECK(!wm::Install(&FakeLookup));  // the fake image has no slot to write
  const std::vector<std::string> off = Lines("invite_observer_off");
  QCHECK(off.size() == 1);
  if (off.size() == 1) {
    const std::string reason = Field(off[0], "reason");
    QCHECK(reason != "\"prologue_mismatch\"" && reason != "\"not_patched\"" && reason != "<absent>");
    QCHECK(Field(off[0], "name") == "\"MetricLog\"");
  }
  QCHECK(Lines("invite_observer_installed").empty());
}

}  // namespace

int main() {
  sentinel::SetLogSink(&Sink);
  AnUnarmedThunkPassesThroughAndObservesNothing();
  ACreateClickIsLoggedAndHeldBecauseNoChoicesAreSeen();
  AJoinWithACodeIsLoggedAndHeldWithoutSubmit();
  AnUnusableJoinCodeSaysWhyAndNothingIsSent();
  ANonWizardLineLogsNothing();
  AnEscapedPanelNameIsLeftToTheSharedPrefilter();
  NullZeroAndOversizeLinesAreNeverCopied();
  AnUnreadableLineIsWarnedAndStillPassedOn();
  CopyGameBytesCopiesWithinItsCapacityAndNoFurther();
  TheSubmitHandOffStaysOffWhileTheSwitchIsOff();
  NoLineCarriesTheCodeAndEveryLineIsTagged();
  ThePrologueCheckAcceptsTheStoreBytesAndNoOthers();
  EverySkipHasItsPinnedToken();
  InstallNotPatchedRefusesAndLogsIt();
  InstallWithAWrongPrologueRefusesBeforeTheSlotAndArmsNothing();
  InstallWithTheStockPrologueTriesTheSlotAndSaysWhyItRefused();
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "wizard_metric_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("wizard_metric_hook_test: all checks passed\n");
  return 0;
}
