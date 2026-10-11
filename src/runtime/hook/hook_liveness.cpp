#include "runtime/hook/hook_liveness.h"

#include <windows.h>

#include "core/logging.h"
#include "runtime/lifecycle/cli.h"  // g_isServer

namespace {

struct Entry {
  const char* name;
  const char* expected;  // where it is expected to run — states the claim
  // True only for a hook the binary structurally never enters on a dedicated
  // server (confirmed via N86 and this file's own header comment). Checking
  // it every periodic report for the life of a server process is testing an
  // invariant, not a live condition — see kPrecisionSleepWait's row below.
  bool guaranteedAbsentOnServer;
};

// Order MUST match nevr_hook_liveness::Id.
const Entry kEntries[] = {
    {"GetTimeMicroseconds", "all modes", false},
    {"CTimer_GetMilliSeconds", "all modes", false},
    {"CPrecisionSleep::Wait", "CLIENT ONLY — never runs on a server", true},
    {"EndMultiplayer", "session teardown", false},
    {"CSpinWait::WaitForValue", "under contention", false},
    {"HTTPListenerBringup", "server bringup", false},
    {"CBroadcaster::Listen", "registration + gameplay", false},
    {"CBroadcaster::ReceiveLocalEvent", "message dispatch", false},
};

// kEntries is sized by its INITIALIZER, not by kCount. That is load-bearing: if
// it were declared [kCount], adding an Id would silently grow the array with a
// zero row and this assert would compare kCount to itself — tautologically true,
// exactly the N65 "EXPECT_EQ(5,5)" defect. Written this way the two sides are
// independent facts, so adding an Id without a row is a COMPILE error rather
// than a runtime out-of-bounds read that prints "name=(null)" (measured).
static_assert(sizeof(kEntries) / sizeof(kEntries[0]) == nevr_hook_liveness::kCount,
              "kEntries must have exactly one row per nevr_hook_liveness::Id — add the "
              "row when you add the id");

volatile LONG g_counts[nevr_hook_liveness::kCount] = {0};

// Edge-triggered reporting state. A hook's line only prints when its entry
// count (and therefore its entered/not-entered status) has actually changed
// since the last report — not on every ~30s periodic tick forever. Fixed
// array, no heap, no lock: Report() runs from a single caller (tick.cpp's
// per-frame dispatcher), so plain statics are enough — no InterlockedXxx
// needed here (unlike g_counts, which Mark() touches from many hook threads).
bool g_everReported[nevr_hook_liveness::kCount] = {false};
LONG g_lastReportedCount[nevr_hook_liveness::kCount] = {0};

// kPrecisionSleepWait server-mode notice: logged once, the first time
// Report() runs on a server, instead of re-checking a structural invariant
// every periodic report for the rest of the process lifetime.
bool g_serverAbsenceNoticeLogged = false;

}  // namespace

namespace nevr_hook_liveness {

void Mark(Id id) {
  if (id < 0 || id >= kCount) return;
  InterlockedIncrement(&g_counts[id]);
}

int NeverEnteredCount() {
  int n = 0;
  for (int i = 0; i < kCount; i++) {
    if (g_counts[i] == 0) n++;
  }
  return n;
}

void Report(const char* context) {
  for (int i = 0; i < kCount; i++) {
    if (kEntries[i].name == nullptr) continue;  // belt-and-braces

    // D — kPrecisionSleepWait is structurally guaranteed never entered on a
    // dedicated server (N86; see this file's header comment and tick.cpp's).
    // That is an invariant, not a live check: note it once and stop tracking
    // it for the rest of this server run, instead of re-warning every ~30s.
    if (kEntries[i].guaranteedAbsentOnServer && g_isServer) {
      if (!g_serverAbsenceNoticeLogged) {
        g_serverAbsenceNoticeLogged = true;
        Log(EchoVR::LogLevel::Info,
            "[NEVR.PATCH] hook_liveness name=%s skipped=server_mode_guaranteed_absent "
            "(client-only hook) — not tracked further this run",
            kEntries[i].name);
      }
      continue;
    }

    const LONG c = g_counts[i];

    // B — edge-triggered: only emit a line when this hook's state actually
    // changed since the last report. The first report always fires (keeps
    // the ~30s-in initial proof-of-life visible); after that, a hook sitting
    // unchanged — including one that has already proven itself alive and
    // simply isn't firing again this interval — produces no line at all.
    if (g_everReported[i] && g_lastReportedCount[i] == c) continue;
    g_everReported[i] = true;
    g_lastReportedCount[i] = c;

    // WARNING for never-entered: an installed hook that has never run is either
    // mode-specific (fine, and the `expected` field says so) or dead wiring
    // (N86). Either way it is a claim that wants checking, so it must not be
    // filtered out as routine INFO noise.
    Log(c == 0 ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,
        "[NEVR.PATCH] hook_liveness name=%s entries=%ld entered=%s expected=%s at=%s",
        kEntries[i].name, c, c ? "yes" : "NO", kEntries[i].expected,
        context ? context : "(unknown)");
  }
}

}  // namespace nevr_hook_liveness
