#include "runtime/hook/hook_liveness.h"

#include <windows.h>

#include "core/logging.h"

namespace {

struct Entry {
  const char* name;
  const char* expected;  // where it is expected to run — states the claim
};

// Order MUST match HookLiveness::Id.
const Entry kEntries[] = {
    {"GetTimeMicroseconds", "all modes"},
    {"CTimer_GetMilliSeconds", "all modes"},
    {"CPrecisionSleep::Wait", "CLIENT ONLY — never runs on a server (N86)"},
    {"EndMultiplayer", "session teardown"},
    {"CSpinWait::WaitForValue", "under contention"},
    {"HTTPListenerBringup", "server bringup"},
    {"CBroadcaster::Listen", "registration + gameplay"},
    {"CBroadcaster::ReceiveLocalEvent", "message dispatch"},
};

// kEntries is sized by its INITIALIZER, not by kCount. That is load-bearing: if
// it were declared [kCount], adding an Id would silently grow the array with a
// zero row and this assert would compare kCount to itself — tautologically true,
// exactly the N65 "EXPECT_EQ(5,5)" defect. Written this way the two sides are
// independent facts, so adding an Id without a row is a COMPILE error rather
// than a runtime out-of-bounds read that prints "name=(null)" (measured).
static_assert(sizeof(kEntries) / sizeof(kEntries[0]) == HookLiveness::kCount,
              "kEntries must have exactly one row per HookLiveness::Id — add the "
              "row when you add the id");

volatile LONG g_counts[HookLiveness::kCount] = {0};

}  // namespace

namespace HookLiveness {

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
    const LONG c = g_counts[i];
    if (kEntries[i].name == nullptr) continue;  // belt-and-braces
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

}  // namespace HookLiveness
