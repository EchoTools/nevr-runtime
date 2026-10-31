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
 * proves the one `bl` to the PLT stub). The handler never calls the game's function: it measures the headset's number
 * itself, from ro.build.version.incremental, with the same parse the game makes (ParseOsBuild), and answers 0, "no
 * number", on every call. Initialize then goes on to its next check exactly as for a headset whose property is not a
 * number, so the answer is the same whether the stock or the patched function sits behind the slot. The game's variable
 * is never written. libpnsovr has its own copy of the function (the login's system info) and is not touched.
 *
 * Lines (sentinel log):
 *   install  {"event":"os_build_gate_install","result":"installed|refused","status":...,"floor":<n>,"floor_source":...,
 *            "original_body":...}
 *            floor_source is "pinned" when the floor was read from libr15 after its build ID matched, else why not
 *            (floor 0: the line in the handler then cannot say whether the game would have refused). original_body is
 *            what the loaded libr15's OSBuildVersion starts with: "queries_property", "returns_zero", "other_code", or
 *            why it was not read ("no_build_id", "build_id_mismatch", "module_not_loaded").
 *   call     {"event":"os_build_gate","answer":"no_number","os_build_read":0|1,"os_build":<n>,"floor":<n>,
 *            "game_would_refuse":0|1,"reason":...}, once per process (Initialize runs once), written on the first call.
 *            reason is ReadingName() of the property: "number", "no_property", "too_long" or "not_a_number".
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "pinned_targets.h"

namespace nevr_quest::os_build_gate {

using Thunk = sentinel::pinned::LibR15OSBuildVersionThunk;

// How the property was read. kNumber is the only reading the game could have refused on.
enum class Reading { kNumber, kNoProperty, kTooLong, kNotANumber };
const char* ReadingName(Reading reading) noexcept;  // "number", "no_property", "too_long", "not_a_number"

// The game's parse of ro.build.version.incremental (CSysInfo::OSBuildVersion, IsUInt64 and ToUInt64): `length` is what the
// property getter returned, `value` its text. On kNumber `*out` is the number (wrapped to 64 bits, as the game wraps it).
Reading ParseOsBuild(const char* value, int length, std::uint64_t* out) noexcept;

// The property getter the hook reads through: the contract of __system_property_get (the length written, or 0).
using PropertyReader = int (*)(const char* name, char* value);

// Test support: the reader the hook uses from now on; nullptr restores the default (__system_property_get on Android,
// 0 elsewhere).
void SetPropertyReaderForTest(PropertyReader reader) noexcept;

// What the handler measured on its first call.
struct Seen {
  unsigned long long calls;  // calls through the hook
  bool read;                 // the property parsed as a number on the first call
  std::uint64_t os_build;    // the number it read (0 when it read none)
  std::uint64_t floor;       // the floor the install read (0: unknown)
  bool would_refuse;         // read and os_build < floor, with the floor known
  const char* reason;        // ReadingName() of the first call's reading; "unmeasured" before it
};
Seen CurrentSeen() noexcept;

// Installs the hook into libr15's OSBuildVersion slot after reading the floor and the original's first instruction from
// the loaded libr15. True when the slot holds the thunk. A refused install is logged by GotHook and by the hook's own
// line, and leaves the game's call intact.
bool Install() noexcept;

// The floor read from `image` (a libr15 image): the constant at the pinned address when the image's build ID is the
// pinned one, else 0. `*source` names the outcome ("pinned", "no_build_id", "build_id_mismatch").
std::uint64_t ReadPinnedFloorFrom(const sentinel::ElfImage& image, const char** source) noexcept;

// The class of the original OSBuildVersion's first instruction word in `image`, read and never written: "queries_property"
// (the stock body), "returns_zero" (the body the community patch writes), "other_code", or, when the image is not the
// pinned build, "no_build_id" or "build_id_mismatch". The same string is written to `*kind`.
const char* ReadOriginalBodyFrom(const sentinel::ElfImage& image, const char** kind) noexcept;

// The install on any target with `floor` as the floor and `originalBody` as the classification Install read, the images
// found through `lookup` (the host test installs it in a fixture library). Install() passes the default lookup.
bool InstallAt(const sentinel::GotTarget& target, std::uint64_t floor, const char* floorSource, const char* originalBody,
               sentinel::ImageLookup lookup = sentinel::FindLoadedImage) noexcept;

// Test support: arms the handler without touching a GOT slot, with `floor` as the floor. ResetForTest clears the
// one-shot and the counts.
void ArmForTest(std::uint64_t floor) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::os_build_gate
