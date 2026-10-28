/* The game never refuses a headset for its OS build number (#945).
 *
 * What the game does (pinned libr15; addresses in pinned_targets.h, LibR15OSBuildVersion): CR15NetGame::Initialize asks
 * CSysInfo::OSBuildVersion for the number in ro.build.version.incremental. When it gets one and the number is below
 * NRadEngine::kMinOSBuildVersion (6023800249000000 in the pinned build), it switches the net game to "os needs update"
 * (state -100) and returns: no config request, no login, nothing after. A number it cannot read (not all digits, or
 * longer than 20 characters) skips the floor. The floor is a constant of the shipped build, so a headset whose OS numbers
 * its builds below it never gets past it; the community patch repo's "Quest S Version Patch" makes the function return 0
 * for that reason.
 *
 * One GOT hook, on libr15's OSBuildVersion slot. Initialize is its only caller (pinned_targets.h; got_pinned_test.cpp
 * proves the one `bl` to the PLT stub). The handler calls the original into a local of its own, so the game's variable is
 * never written, and answers 0, "no number": Initialize then goes on to its next check exactly as for a headset whose
 * property is not a number. libpnsovr has its own copy of the function (the login's system info) and is not touched.
 *
 * Lines (sentinel log):
 *   install  {"event":"os_build_gate_install","result":"installed|refused","status":...,"floor":<n>,"floor_source":...}
 *            floor_source is "pinned" when the floor was read from libr15 after its build ID matched, else why not
 *            (floor 0: the line in the handler then cannot say whether the game would have refused).
 *   call     {"event":"os_build_gate","answer":"no_number","os_build_read":0|1,"os_build":<n>,"floor":<n>,
 *            "game_would_refuse":0|1}, once per process (Initialize runs once), written after the original returned.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "pinned_targets.h"

namespace nevr_quest::os_build_gate {

using Thunk = sentinel::pinned::LibR15OSBuildVersionThunk;

// What the handler measured on its first call.
struct Seen {
  unsigned long long calls;  // calls through the hook
  bool read;                 // the original returned 1 on the first call
  std::uint64_t os_build;    // the number it read (0 when it read none)
  std::uint64_t floor;       // the floor the install read (0: unknown)
  bool would_refuse;         // read and os_build < floor, with the floor known
};
Seen CurrentSeen() noexcept;

// Installs the hook into libr15's OSBuildVersion slot after reading the floor from the loaded libr15. True when the slot
// holds the thunk. A refused install is logged by GotHook and by the hook's own line, and leaves the game's call intact.
bool Install() noexcept;

// The install on any target with `floor` as the floor (the host test installs it in a fixture library).
bool InstallAt(const sentinel::GotTarget& target, std::uint64_t floor, const char* floorSource) noexcept;

// Test support: arms the handler without touching a GOT slot, with `floor` as the floor. ResetForTest clears the
// one-shot and the counts.
void ArmForTest(std::uint64_t floor) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::os_build_gate
