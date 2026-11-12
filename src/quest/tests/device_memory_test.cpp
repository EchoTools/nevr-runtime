// Host test for the one memory reader (src/core/device_memory.*): the text of /proc/meminfo parses to bytes, a line that is missing or
// malformed is a refusal and not a guess, the share arithmetic every memory-based limit uses, and the real read on this machine.
//
// Built and run by `just test-quest-data`.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/device_memory.h"
#include "quest/tests/test_check.h"

namespace {

namespace nd = nevr_device;

void TheMemAvailableLineIsReadInBytes() {
  std::uint64_t bytes = 0;
  QCHECK(nd::ParseMemAvailable("MemTotal:       16384 kB\nMemFree:         100 kB\nMemAvailable:   2048 kB\nBuffers: 5 kB\n", &bytes));
  QCHECK(bytes == 2048ull * 1024);
  QCHECK(nd::ParseMemAvailable("MemAvailable:0 kB\n", &bytes) && bytes == 0);
  QCHECK(nd::ParseMemAvailable("MemAvailable: 6291456 kB", &bytes) && bytes == 6144ull * 1024 * 1024);  // the last line, no newline
}

void ALineThatIsMissingOrMalformedIsRefused() {
  std::uint64_t bytes = 77;
  QCHECK(!nd::ParseMemAvailable("MemTotal: 100 kB\nMemFree: 5 kB\n", &bytes));
  QCHECK(!nd::ParseMemAvailable("", &bytes));
  QCHECK(!nd::ParseMemAvailable(nullptr, &bytes));
  QCHECK(!nd::ParseMemAvailable("MemAvailable: lots kB\n", &bytes));
  QCHECK(!nd::ParseMemAvailable("MemAvailable: 12 MB\n", &bytes));  // the unit is kB
  QCHECK(!nd::ParseMemAvailable("MemAvailable: 18446744073709551615 kB\n", &bytes));  // times 1024 does not fit 64 bits
  QCHECK(bytes == 77);  // a refusal leaves the caller's value alone
}

void TheShareIsTheWholeOverTheDivisor() {
  QCHECK(nd::Share(1000, 8) == 125);
  QCHECK(nd::Share(7, 8) == 0);
  QCHECK(nd::Share(1000, 0) == 0);
  QCHECK(nd::Share(2 * nd::Share(1000, 8), 1) == 250);
}

void TheRealReadGivesAPositiveNumberOnThisMachine() {
  std::uint64_t bytes = 0;
  QCHECK(nd::MemoryAvailable(&bytes));
  QCHECK(bytes > 0);
  std::printf("device_memory_test: this machine has %llu bytes available\n", static_cast<unsigned long long>(bytes));
}

}  // namespace

int main() {
  TheMemAvailableLineIsReadInBytes();
  ALineThatIsMissingOrMalformedIsRefused();
  TheShareIsTheWholeOverTheDivisor();
  TheRealReadGivesAPositiveNumberOnThisMachine();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "device_memory_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("device_memory_test: all checks passed\n");
  return 0;
}
