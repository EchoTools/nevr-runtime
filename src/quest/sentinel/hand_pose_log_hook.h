/* The controller type and raw pose log (#1986, measurement only).
 *
 * Why. Players on a Quest 2 report hands rolled backwards with Touch Pro / Touch Plus controllers. libr15 has no per-controller
 * logic (pinned_targets.h, the VrApi input block): CVR::PollTracking (FUN 0x1a71808) enumerates the input devices, keeps type 4
 * (TrackedRemote), takes the hand from the LeftHand bit of the capabilities, and copies the pose VrApi returns, unchanged, into
 * the hand's slot. If a controller comes out rolled, the roll is in the pose the headset's VrApi reports for it. No documented
 * capability tells a Touch Pro from a Touch, so nothing here can decide anything; this file only records what each controller
 * reports, so one log from a Touch and one from a Touch Pro on the same headset can be compared.
 *
 * What it reads, and changes nothing of. Two GOT hooks on libr15's own slots (pinned_targets.h), original first:
 *
 *   vrapi_EnumerateInputDevices   the {Type, DeviceID} header the game is given. A device of type 4 enters the device table
 *                                 (8 entries; any other type, a failed call and a ninth device are ignored). The first time
 *                                 a type-4 device is seen, the handler asks the same VrApi, on the game's thread, for that
 *                                 device's capabilities through the function libr15's own slot holds (the call the game makes
 *                                 right after, never a hook), and keeps the five words.
 *   vrapi_GetInputTrackingState   after the original returns success, the first result of a table device whose Status has the
 *                                 orientation-tracked bit gives its raw orientation quaternion, stored once.
 *
 * The result of both calls and every byte they wrote are returned exactly as the runtime wrote them. No pose is changed, no
 * menu row or setting exists, no flag turns anything on.
 *
 * Log lines, written by a poller thread (`nevr-handpose`, twice a second), never on the game's call path (callback_thunk.h
 * rule 1). Each is written once per device per session, Info:
 *   {"event":"hand_flip_device","device":<id>,"type":4,"caps_read":"yes|no","caps":"0x%08x","button_caps":"0x%08x",
 *    "touch_caps":"0x%08x","reserved4":"0x%08x","reserved5":"0x%08x","hand":"left|right|unknown","model":"<ro.product.model>"}
 *   {"event":"hand_flip_pose","device":<id>,"hand":"left|right|unknown","status":"0x%08x","qx":"%.6f","qy":..,"qz":..,"qw":..}
 * `caps` is ControllerCapabilities; `hand` is the LeftHand / RightHand bit (input_seam.h HandFromCaps); a floating-point value
 * is a string with six decimals.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "pinned_targets.h"
#include "quest/plugin/input_seam.h"

namespace nevr_quest::hand_pose_log {

using EnumerateThunk = sentinel::pinned::LibR15EnumerateInputDevicesThunk;
using TrackingThunk = sentinel::pinned::LibR15GetInputTrackingStateThunk;

// ovrInputCapabilityHeader (VrApi_Input.h): the Type and DeviceID the game reads at the start of PollTracking's loop.
struct CapabilityHeader {
  std::uint32_t type;
  std::uint32_t deviceId;
};
static_assert(sizeof(CapabilityHeader) == 8, "ovrInputCapabilityHeader");

// The part of ovrTracking (VrApi_Types.h, 104 bytes) this reads: Status, padding, then the orientation of HeadPose.Pose.
struct TrackingPrefix {
  std::uint32_t status;
  std::uint32_t padding;
  float orientation[4];  // x, y, z, w
};
static_assert(offsetof(TrackingPrefix, orientation) == 8, "ovrTracking: HeadPose starts at 8");
inline constexpr std::size_t kTrackingBytes = 104;
inline constexpr std::uint32_t kStatusOrientationTracked = 1;  // VRAPI_TRACKING_STATUS_ORIENTATION_TRACKED

inline constexpr int kDeviceSlots = 8;

// Where the development property is read: copies the value into `out` (at least 92 bytes) and returns its length, or 0.
using PropertyReader = int (*)(const char* name, char* out) noexcept;

// Installs the two hooks into libr15's slots and starts the poller. True when both slots hold the thunks. Logs one
// {"event":"hand_pose_log_install",...} line. The capabilities function is read from libr15's own slot; without it the device
// line says caps_read=no.
bool Install() noexcept;

// The install on any two targets with the capabilities function given (the host test installs it in a fixture library).
bool InstallAt(const sentinel::GotTarget& enumerate, const sentinel::GotTarget& tracking, input_seam::CapsFn caps) noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// Arms both handlers without touching a GOT slot. ResetForTest forgets the table and the logged flags (not the thunks' own
// state: reset those through their Reset()).
void ArmForTest(input_seam::CapsFn caps, PropertyReader property) noexcept;
void ResetForTest() noexcept;

// One poller pass, as the thread runs it. Returns the number of lines written.
int PollOnceForTest() noexcept;

// Devices in the table (type 4 only).
int DeviceCountForTest() noexcept;

}  // namespace nevr_quest::hand_pose_log
