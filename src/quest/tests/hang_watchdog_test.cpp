// Host test for the frozen-load guard (#1737, G6A-045): the real module (src/quest/sentinel/hang_watchdog.cpp), built
// without exceptions as the sentinel builds it, driven through the two thunk entries with stand-ins for libr15's
// vrapi_EnterVrMode and vrapi_LeaveVrMode, with the watcher's look (PollOnce) called at chosen times instead of a thread,
// and the process end through the real process_end::EndNow with its exit replaced. No GOT slot is touched:
// got_pinned_test.cpp resolves the two pinned slots in the real library.
//
// Built and run by `just test-quest-hooks`.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hang_watchdog.h"
#include "hook_log.h"
#include "process_end_hook.h"
#include "quest/tests/test_check.h"

namespace {

namespace hw = nevr_quest::hang_watchdog;
namespace pe = nevr_quest::process_end;
using sentinel::pinned::OvrMobileOpaque;
using sentinel::pinned::OvrModeParmsOpaque;

constexpr std::int64_t kS = 1000000000LL;

std::vector<std::string> g_lines;
void Capture(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }
bool Logged(const char* needle) {
  for (const std::string& l : g_lines)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}
int CountLogged(const char* needle) {
  int n = 0;
  for (const std::string& l : g_lines)
    if (l.find(needle) != std::string::npos) ++n;
  return n;
}

// ---- stand-ins -------------------------------------------------------------------------------------------------------------

OvrMobileOpaque* g_session = reinterpret_cast<OvrMobileOpaque*>(0x1000);
OvrMobileOpaque* StandInEnter(const OvrModeParmsOpaque*) { return g_session; }
OvrMobileOpaque* StandInEnterFails(const OvrModeParmsOpaque*) { return nullptr; }
int g_leaveCalls = 0;
void StandInLeave(OvrMobileOpaque*) { ++g_leaveCalls; }

hw::EnterThunk::Fn Enter() { return reinterpret_cast<hw::EnterThunk::Fn>(hw::EnterThunk::EntryAddress()); }
hw::LeaveThunk::Fn Leave() { return reinterpret_cast<hw::LeaveThunk::Fn>(hw::LeaveThunk::EntryAddress()); }

int g_ends = 0;
std::string g_endReason;
void RecordEnd(const char* reason) noexcept {
  ++g_ends;
  g_endReason = reason;
}

sentinel::GotStatus g_statuses[2] = {sentinel::GotStatus::kOk, sentinel::GotStatus::kOk};
sentinel::GotStatus FakeInstall(int slot) noexcept { return g_statuses[slot]; }

std::string g_dir;
void WriteFile(const char* name, const char* text) {
  const std::string p = g_dir + "/" + name;
  FILE* f = std::fopen(p.c_str(), "w");
  if (f == nullptr) return;
  std::fputs(text, f);
  std::fclose(f);
}
void RemoveFile(const char* name) { std::remove((g_dir + "/" + name).c_str()); }

void Reset() {
  g_lines.clear();
  g_ends = 0;
  g_endReason.clear();
  g_leaveCalls = 0;
  g_statuses[0] = g_statuses[1] = sentinel::GotStatus::kOk;
  RemoveFile(hw::kOffSwitch);
  RemoveFile(hw::kSecondsFile);
  hw::ResetForTest();
  hw::EnterThunk::Reset();
  hw::LeaveThunk::Reset();
  *hw::EnterThunk::OriginalOut() = reinterpret_cast<void*>(&StandInEnter);
  *hw::LeaveThunk::OriginalOut() = reinterpret_cast<void*>(&StandInLeave);
}

bool InstallOn() { return hw::InstallVia(g_dir.c_str(), &FakeInstall, &RecordEnd, false, true); }

// ---- cases -----------------------------------------------------------------------------------------------------------------

// The case the guard exists for: in VR mode, frames, then none for 30 s -> one hangwd_exit line and the end.
void ThirtySecondsWithoutAFrameInVrEndsTheProcess() {
  Reset();
  QCHECK(InstallOn());
  QCHECK(Logged("\"event\":\"q15t_hangwd_install\"") && Logged("\"result\":\"on\"") && Logged("\"limit_s\":30"));
  QCHECK(Enter()(nullptr) == g_session);
  hw::Beat(100 * kS);
  hw::Beat(101 * kS);
  QCHECK(!hw::PollOnce(130 * kS));  // 29 s since the last frame: not yet
  QCHECK(g_ends == 0);
  QCHECK(hw::PollOnce(131 * kS));   // 30 s
  QCHECK(g_ends == 1);
  QCHECK(g_endReason == "hang_watchdog");
  QCHECK(Logged("\"event\":\"hangwd_exit\"") && Logged("\"gap_ms\":30000") && Logged("\"frames\":2") &&
         Logged("\"armed_ms\":1000") && Logged("\"reason\":\"no_frame_submitted_in_vr\""));
}

// A paused app submits no frames: after LeaveVrMode no gap ends it, however long.
void APauseIsNeverAHang() {
  Reset();
  QCHECK(InstallOn());
  Enter()(nullptr);
  hw::Beat(10 * kS);
  Leave()(g_session);
  QCHECK(g_leaveCalls == 1);  // the game's call still runs
  QCHECK(!hw::PollOnce(1000 * kS));
  QCHECK(g_ends == 0);
  QCHECK(Logged("\"state\":\"left\""));
}

// Before the first frame after EnterVrMode the guard is not armed (a load that has not drawn yet is the splash's business,
// not a frozen view): a resume re-arms on its first frame, and the gap counts from there.
void TheGuardArmsOnTheFirstFrameAfterEachEntry() {
  Reset();
  QCHECK(InstallOn());
  Enter()(nullptr);
  QCHECK(!hw::PollOnce(500 * kS));  // in VR, no frame yet
  hw::Beat(600 * kS);
  Leave()(g_session);
  Enter()(nullptr);                 // resume: disarmed until a frame
  QCHECK(!hw::PollOnce(700 * kS));
  hw::Beat(710 * kS);
  QCHECK(!hw::PollOnce(739 * kS));
  QCHECK(hw::PollOnce(740 * kS));
  QCHECK(g_ends == 1);
  QCHECK(Logged("\"armed_ms\":0"));
}

// An EnterVrMode that failed (null session) does not put the guard in VR mode.
void AFailedEntryIsNotVrMode() {
  Reset();
  *hw::EnterThunk::OriginalOut() = reinterpret_cast<void*>(&StandInEnterFails);
  QCHECK(InstallOn());
  QCHECK(Enter()(nullptr) == nullptr);
  hw::Beat(1 * kS);
  QCHECK(!hw::PollOnce(100 * kS));
  QCHECK(g_ends == 0);
}

// hangwd.off: nothing installed, no frame arms anything.
void TheOffSwitchInstallsNothing() {
  Reset();
  WriteFile(hw::kOffSwitch, "");
  QCHECK(!InstallOn());
  QCHECK(Logged("\"result\":\"off\"") && Logged("\"reason\":\"hangwd.off\""));
  hw::EnterVr();
  hw::Beat(1 * kS);
  QCHECK(!hw::PollOnce(1000 * kS));
  QCHECK(g_ends == 0);
}

// hangwd.sec in range sets the bound; out of range is ignored and said.
void TheSecondsFileSetsTheBound() {
  Reset();
  WriteFile(hw::kSecondsFile, " 45\n");
  QCHECK(InstallOn());
  QCHECK(Logged("\"limit_s\":45") && Logged("\"limit_source\":\"hangwd.sec\""));
  Enter()(nullptr);
  hw::Beat(0 + 1);
  QCHECK(!hw::PollOnce(44 * kS));
  QCHECK(hw::PollOnce(46 * kS));

  Reset();
  WriteFile(hw::kSecondsFile, "5");
  QCHECK(InstallOn());
  QCHECK(Logged("\"limit_s\":30") && Logged("\"limit_source\":\"hangwd.sec_ignored\""));
}

void ParseSecondsBounds() {
  unsigned v = 0;
  QCHECK(hw::ParseSeconds("10", &v) && v == 10);
  QCHECK(hw::ParseSeconds("900", &v) && v == 900);
  QCHECK(!hw::ParseSeconds("9", &v));
  QCHECK(!hw::ParseSeconds("901", &v));
  QCHECK(!hw::ParseSeconds("30s", &v));
  QCHECK(!hw::ParseSeconds("-30", &v));
  QCHECK(!hw::ParseSeconds("", &v));
  QCHECK(!hw::ParseSeconds(nullptr, &v));
}

// Either VR-mode hook refused: the guard cannot tell a pause from a hang, so it never arms.
void ARefusedVrModeHookLeavesTheGuardOff() {
  Reset();
  g_statuses[1] = sentinel::GotStatus::kOriginalMismatch;
  QCHECK(!InstallOn());
  QCHECK(Logged("\"result\":\"refused\"") && Logged("\"reason\":\"vr_mode_hook_refused\"") &&
         Logged("\"leave_status\":\"original_mismatch\""));
  Enter()(nullptr);
  hw::Beat(1 * kS);
  QCHECK(!hw::PollOnce(1000 * kS));
  QCHECK(g_ends == 0);
}

// No frame source (render_config's SubmitFrame2 hook refused): nothing hooked, the line says why.
void NoFrameSourceInstallsNothing() {
  Reset();
  QCHECK(!hw::InstallVia(g_dir.c_str(), &FakeInstall, &RecordEnd, false, false));
  QCHECK(Logged("\"reason\":\"frame_source_not_installed\""));
}

// The end goes through process_end::EndNow: its process_ending line after the guard's own, the before-exit callback,
// then exit code 0. One exit path for the running game, never a second one.
int g_exitCode = -1;
int g_flushes = 0;
void RecordExit(int code) noexcept { g_exitCode = code; }
void Flush() noexcept { ++g_flushes; }
void TheEndIsProcessEndsWithItsFlush() {
  Reset();
  pe::ResetForTest();
  pe::SetExitForTest(&RecordExit);
  pe::SetBeforeExit(&Flush);
  QCHECK(hw::InstallVia(g_dir.c_str(), &FakeInstall, &pe::EndNow, false, true));
  Enter()(nullptr);
  hw::Beat(1 * kS);
  QCHECK(hw::PollOnce(40 * kS));
  QCHECK(g_exitCode == 0);
  QCHECK(g_flushes == 1);
  QCHECK(CountLogged("\"event\":\"process_ending\"") == 1 && Logged("\"reason\":\"hang_watchdog\""));
  // the cause is on disk before the end: hangwd_exit precedes process_ending
  std::size_t cause = g_lines.size(), end = g_lines.size();
  for (std::size_t i = 0; i < g_lines.size(); ++i) {
    if (cause == g_lines.size() && g_lines[i].find("\"event\":\"hangwd_exit\"") != std::string::npos) cause = i;
    if (end == g_lines.size() && g_lines[i].find("\"event\":\"process_ending\"") != std::string::npos) end = i;
  }
  QCHECK(cause < end);
  pe::ResetForTest();
}

}  // namespace

int main() {
  char tmpl[] = "./hangwd_test_XXXXXX";
  const char* dir = mkdtemp(tmpl);
  if (dir == nullptr) {
    std::fprintf(stderr, "mkdtemp failed\n");
    return 2;
  }
  g_dir = dir;
  sentinel::SetLogSink(&Capture);
  hw::ArmForTest();

  ThirtySecondsWithoutAFrameInVrEndsTheProcess();
  APauseIsNeverAHang();
  TheGuardArmsOnTheFirstFrameAfterEachEntry();
  AFailedEntryIsNotVrMode();
  TheOffSwitchInstallsNothing();
  TheSecondsFileSetsTheBound();
  ParseSecondsBounds();
  ARefusedVrModeHookLeavesTheGuardOff();
  NoFrameSourceInstallsNothing();
  TheEndIsProcessEndsWithItsFlush();

  RemoveFile(hw::kOffSwitch);
  RemoveFile(hw::kSecondsFile);
  rmdir(dir);
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "hang_watchdog_test: %d failure(s)\n", quest_test::Failures());
    return 1;
  }
  std::printf("hang_watchdog_test: OK\n");
  return 0;
}
