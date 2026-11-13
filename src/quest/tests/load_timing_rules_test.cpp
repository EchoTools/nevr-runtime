// The load timing records on the Quest's host build: runtime/hook/load_timing_rules.h is the file the PC runtime compiles
// (src/runtime/tests/load_timing_rules_test.cpp covers it in full); this proves the Quest build sees the same records.

#include <cstdint>
#include <cstdio>
#include <string>

#include "runtime/hook/load_timing_rules.h"

namespace {

struct Clock {
  std::uint64_t mono = 5;
  std::uint64_t EpochMs() { return 99; }
  std::uint64_t MonotonicMs() { return mono; }
};

int Check(bool ok, const char* what) {
  if (!ok) std::fprintf(stderr, "load_timing_rules_test FAIL: %s\n", what);
  return ok ? 0 : 1;
}

}  // namespace

int main() {
  int failures = 0;
  failures += Check(nevr_load_timing::FormatLevelLoad("mpl_arena_a", 0xABULL, 12, 99, true) ==
                        "[NEVR.BOOT] level_load level=mpl_arena_a level_hash=0xab ms=12 start_ts_ms=99 first_load=true",
                    "level_load record");
  nevr_load_timing::BootTimer timer;
  std::string line;
  timer.MarkEngineReady(10);
  failures += Check(timer.MarkLoggedIn(30, &line) &&
                        line == "[NEVR.BOOT] boot_timing engine_ready_ms=10 logged_in_ms=30",
                    "boot_timing record");
  Clock clock;
  std::string record;
  nevr_load_timing::FirstLoad first;
  const int result = nevr_load_timing::TimeLevelLoad(
      clock, [&](const std::string& l) { record = l; }, first, 0xABULL, "x", [&] { clock.mono += 40; return 3; });
  failures += Check(result == 3 && record == "[NEVR.BOOT] level_load level=x level_hash=0xab ms=40 start_ts_ms=99 first_load=true",
                    "wrapper");
  std::string voidRecord;
  nevr_load_timing::FirstLoad firstVoid;
  nevr_load_timing::TimeLevelLoad(
      clock, [&](const std::string& l) { voidRecord = l; }, firstVoid, 0xABULL, "v", [&] { clock.mono += 1; });
  failures += Check(voidRecord == "[NEVR.BOOT] level_load level=v level_hash=0xab ms=1 start_ts_ms=99 first_load=true",
                    "void wrapper");
  if (failures == 0) std::puts("load_timing_rules_test: ok");
  return failures == 0 ? 0 : 1;
}
