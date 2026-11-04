// Host test for the controller type and raw pose log (#1986): the real handlers (src/quest/sentinel/hand_pose_log_hook.cpp), built
// without exceptions as the sentinel builds it, driven through the two thunks' entries with stand-ins for libr15's
// vrapi_EnumerateInputDevices, vrapi_GetInputTrackingState and vrapi_GetInputDeviceCapabilities. No GOT slot is touched:
// got_pinned_test.cpp resolves the two pinned slots in the real library.
//
// What it pins: only type-4 devices enter the table (eight at most); the capabilities are asked once per device; both handlers return
// the runtime's result and leave every byte it wrote as it wrote it while Controller Hand Flip is Off (the default); the first
// tracked orientation of a table device is the one logged, raw; each log line is written once, with the golden fields.
//
// And the interim Controller Hand Flip roll (KNOWN-WRONG #2043, hand_flip.h): with the toggle On, only a table device (type 4) has
// its orientation rolled, by q' = (y, -x, w, -z), and only those four floats change; hand tracking, the headset and an id never
// enumerated are untouched; both calls of a frame are rolled; a failed call is not; the toggle takes effect on the next call.
//
// Built and run by `just test-quest-hooks`.

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "hand_flip.h"
#include "hand_pose_log_hook.h"
#include "hook_log.h"
#include "quest/tests/test_check.h"

namespace {

namespace hpl = nevr_quest::hand_pose_log;
namespace hf = nevr_quest::hand_flip;
namespace is = nevr_quest::input_seam;
using sentinel::pinned::OvrMobileOpaque;

hpl::EnumerateThunk::Fn EnumerateEntry() { return reinterpret_cast<hpl::EnumerateThunk::Fn>(hpl::EnumerateThunk::EntryAddress()); }
hpl::TrackingThunk::Fn TrackingEntry() { return reinterpret_cast<hpl::TrackingThunk::Fn>(hpl::TrackingThunk::EntryAddress()); }

constexpr std::uint32_t kLeft = 0x20000002;
constexpr std::uint32_t kRight = 0x20000003;
constexpr std::uint32_t kHandTracking = 0x20000007;
constexpr std::uint32_t kHmd = 0x10000001;

// ---- the runtime's side -------------------------------------------------------------------------------------------------

struct StandInDevice {
  std::uint32_t type;
  std::uint32_t id;
};
std::vector<StandInDevice> g_devices;

// vrapi_EnumerateInputDevices: a failure past the last device, as the game's loop expects.
int StandInEnumerate(OvrMobileOpaque*, std::uint32_t index, void* header) {
  if (index >= g_devices.size()) return -1;
  hpl::CapabilityHeader h{g_devices[index].type, g_devices[index].id};
  std::memcpy(header, &h, sizeof(h));
  return 0;
}

// vrapi_GetInputDeviceCapabilities: a controller's words, by device.
int g_capsCalls = 0;
int g_capsRc = 0;
int StandInCaps(void*, is::VrapiTrackedRemoteCaps* caps) {
  ++g_capsCalls;
  if (g_capsRc != 0) return g_capsRc;
  caps->controllerCapabilities = caps->deviceId == kLeft ? 0x00004004U : 0x00004008U;  // ModelOculusTouch | LeftHand / RightHand
  caps->buttonCapabilities = 0x11;
  caps->touchCapabilities = 0x22;
  caps->reserved4 = 0x33;
  caps->reserved5 = 0x44;
  return 0;
}

int StandInProperty(const char* name, char* out) noexcept {
  if (std::strcmp(name, "ro.product.model") != 0) return 0;
  std::strcpy(out, "Quest 2");
  return 7;
}

// vrapi_GetInputTrackingState: writes the 104 bytes ovrTracking is, from g_tracking.
unsigned char g_tracking[hpl::kTrackingBytes];
int g_trackingRc = 0;
int StandInTracking(OvrMobileOpaque*, std::uint32_t, double, void* tracking) {
  if (g_trackingRc < 0) return g_trackingRc;
  std::memcpy(tracking, g_tracking, sizeof(g_tracking));
  return g_trackingRc;
}

void SetTracking(std::uint32_t status, float x, float y, float z, float w) {
  for (std::size_t i = 0; i < sizeof(g_tracking); ++i) g_tracking[i] = static_cast<unsigned char>(0xA0 + i);  // bytes nobody reads
  hpl::TrackingPrefix prefix{status, 0, {x, y, z, w}};
  std::memcpy(g_tracking, &prefix, sizeof(prefix));
}

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel /*level*/, const char* line) { g_lines.emplace_back(line); }

void Reset() {
  g_devices.clear();
  g_capsCalls = 0;
  g_capsRc = 0;
  g_trackingRc = 0;
  g_lines.clear();
  SetTracking(0x0f, 0.5F, -0.5F, 0.25F, 0.75F);
  hpl::ResetForTest();
  hpl::EnumerateThunk::Reset();
  hpl::TrackingThunk::Reset();
  *hpl::EnumerateThunk::OriginalOut() = reinterpret_cast<void*>(&StandInEnumerate);
  *hpl::TrackingThunk::OriginalOut() = reinterpret_cast<void*>(&StandInTracking);
}

void Arm() { hpl::ArmForTest(&StandInCaps, &StandInProperty); }

// The game's loop: every index until the runtime says no more.
void EnumerateAll() {
  alignas(8) unsigned char header[sizeof(hpl::CapabilityHeader)];
  for (std::uint32_t index = 0; EnumerateEntry()(nullptr, index, header) >= 0; ++index) {
  }
}

// A line from the event on, without the time stamp and level in front of it.
std::string Golden(const std::string& line) {
  const std::size_t at = line.find("\"event\"");
  return at == std::string::npos ? line : line.substr(at);
}

const std::string* LineOf(const char* event) {
  for (const std::string& line : g_lines) {
    if (line.find(std::string("\"event\":\"") + event + "\"") != std::string::npos) return &line;
  }
  return nullptr;
}

int CountOf(const char* event) {
  int n = 0;
  for (const std::string& line : g_lines) n += line.find(std::string("\"event\":\"") + event + "\"") != std::string::npos ? 1 : 0;
  return n;
}

// ---- tests --------------------------------------------------------------------------------------------------------------

// Without the hook armed both entries are the runtime's calls, and nothing is recorded.
void WithoutTheHookNothingIsRecorded() {
  Reset();
  g_devices = {{4, kLeft}};
  EnumerateAll();
  alignas(8) unsigned char tracking[hpl::kTrackingBytes];
  QCHECK(TrackingEntry()(nullptr, kLeft, 1.0, tracking) == 0);
  QCHECK(hpl::DeviceCountForTest() == 0);
  QCHECK(g_capsCalls == 0);
  QCHECK(hpl::PollOnceForTest() == 0);
}

// The runtime's result and header come back exactly as written, for a device, for a device of another type and for the end of the list.
void EnumerateReturnsTheRuntimesResultAndBytes() {
  Reset();
  Arm();
  g_devices = {{4, kLeft}, {8, kHandTracking}, {1, kHmd}};
  for (std::uint32_t index = 0; index < 4; ++index) {
    alignas(8) unsigned char expected[sizeof(hpl::CapabilityHeader)];
    alignas(8) unsigned char got[sizeof(hpl::CapabilityHeader)];
    std::memset(expected, 0x5A, sizeof(expected));
    std::memset(got, 0x5A, sizeof(got));
    const int want = StandInEnumerate(nullptr, index, expected);
    const int rc = EnumerateEntry()(nullptr, index, got);
    QCHECK(rc == want);
    QCHECK(std::memcmp(expected, got, sizeof(got)) == 0);
  }
}

// Only a TrackedRemote enters; a hand-tracking device, the headset and a failed call do not.
void OnlyTypeFourEntersTheTable() {
  Reset();
  Arm();
  g_devices = {{1, kHmd}, {4, kLeft}, {4, kRight}, {8, kHandTracking}};
  EnumerateAll();
  QCHECK(hpl::DeviceCountForTest() == 2);
  QCHECK(g_capsCalls == 2);  // no capabilities were asked for the others
}

// The capabilities are asked once per device, however often the game enumerates.
void CapabilitiesAreAskedOncePerDevice() {
  Reset();
  Arm();
  g_devices = {{4, kLeft}, {4, kRight}};
  for (int poll = 0; poll < 5; ++poll) EnumerateAll();
  QCHECK(g_capsCalls == 2);
  QCHECK(hpl::DeviceCountForTest() == 2);
}

// A ninth controller is not recorded and nothing else happens.
void ANinthDeviceIsIgnored() {
  Reset();
  Arm();
  for (std::uint32_t i = 0; i < 9; ++i) g_devices.push_back({4, 0x30000000U + i});
  EnumerateAll();
  QCHECK(hpl::DeviceCountForTest() == hpl::kDeviceSlots);
  QCHECK(g_capsCalls == hpl::kDeviceSlots);
}

// The tracking result and the 104 bytes come back as the runtime wrote them: for a table device, for an unknown id and for a failure.
void TrackingReturnsTheRuntimesResultAndBytes() {
  Reset();
  Arm();
  g_devices = {{4, kLeft}};
  EnumerateAll();
  for (const std::uint32_t id : {kLeft, kRight, kHandTracking}) {
    alignas(8) unsigned char expected[hpl::kTrackingBytes];
    alignas(8) unsigned char got[hpl::kTrackingBytes];
    std::memset(expected, 0x77, sizeof(expected));
    std::memset(got, 0x77, sizeof(got));
    const int want = StandInTracking(nullptr, id, 1.0, expected);
    const int rc = TrackingEntry()(nullptr, id, 1.0, got);
    QCHECK(rc == want);
    QCHECK(std::memcmp(expected, got, sizeof(got)) == 0);
  }
  g_trackingRc = -3;  // a failure: the result goes back, the buffer is the caller's, and nothing is recorded
  alignas(8) unsigned char untouched[hpl::kTrackingBytes];
  std::memset(untouched, 0x77, sizeof(untouched));
  QCHECK(TrackingEntry()(nullptr, kLeft, 1.0, untouched) == -3);
  for (unsigned char byte : untouched) QCHECK(byte == 0x77);
}

// The device line, golden, for the left and right controllers; the hand is the LeftHand / RightHand bit of the capabilities.
void TheDeviceLineIsGolden() {
  Reset();
  Arm();
  sentinel::SetLogSink(&CaptureLine);
  g_devices = {{4, kLeft}, {4, kRight}};
  EnumerateAll();
  QCHECK(hpl::PollOnceForTest() == 2);
  sentinel::SetLogSink(nullptr);
  QCHECK(CountOf("hand_flip_device") == 2);
  const std::string left =
      "\"event\":\"hand_flip_device\",\"device\":536870914,\"type\":4,\"caps_read\":\"yes\",\"caps\":\"0x00004004\","
      "\"button_caps\":\"0x00000011\",\"touch_caps\":\"0x00000022\",\"reserved4\":\"0x00000033\",\"reserved5\":\"0x00000044\","
      "\"hand\":\"left\",\"model\":\"Quest 2\",\"flip\":0,\"by\":\"default\"}";
  const std::string right =
      "\"event\":\"hand_flip_device\",\"device\":536870915,\"type\":4,\"caps_read\":\"yes\",\"caps\":\"0x00004008\","
      "\"button_caps\":\"0x00000011\",\"touch_caps\":\"0x00000022\",\"reserved4\":\"0x00000033\",\"reserved5\":\"0x00000044\","
      "\"hand\":\"right\",\"model\":\"Quest 2\",\"flip\":0,\"by\":\"default\"}";
  QCHECK(g_lines.size() == 2);
  if (g_lines.size() == 2) {
    QCHECK(Golden(g_lines[0]) == left);
    QCHECK(Golden(g_lines[1]) == right);
  }
}

// When the capabilities cannot be had (the call fails, or libr15's function is not there) the line says so, with zero words and an unknown hand.
void ADeviceWithNoCapabilitiesIsLoggedAsSuch() {
  for (const bool noFunction : {false, true}) {
    Reset();
    if (noFunction) {
      hpl::ArmForTest(nullptr, &StandInProperty);
    } else {
      Arm();
      g_capsRc = -2;
    }
    sentinel::SetLogSink(&CaptureLine);
    g_devices = {{4, kLeft}};
    EnumerateAll();
    QCHECK(hpl::PollOnceForTest() == 1);
    sentinel::SetLogSink(nullptr);
    QCHECK(g_lines.size() == 1);
    if (!g_lines.empty()) {
      QCHECK(Golden(g_lines[0]) ==
             "\"event\":\"hand_flip_device\",\"device\":536870914,\"type\":4,\"caps_read\":\"no\",\"caps\":\"0x00000000\","
             "\"button_caps\":\"0x00000000\",\"touch_caps\":\"0x00000000\",\"reserved4\":\"0x00000000\","
             "\"reserved5\":\"0x00000000\",\"hand\":\"unknown\",\"model\":\"Quest 2\",\"flip\":0,\"by\":\"default\"}");
    }
  }
}

// The first result whose Status says the orientation is tracked is the one logged; later poses change nothing; the line is written once.
void ThePoseLineIsTheFirstTrackedOrientationAndWrittenOnce() {
  Reset();
  Arm();
  sentinel::SetLogSink(&CaptureLine);
  g_devices = {{4, kLeft}};
  EnumerateAll();
  alignas(8) unsigned char tracking[hpl::kTrackingBytes];

  SetTracking(0x00, 9.0F, 9.0F, 9.0F, 9.0F);  // not tracked yet: not recorded
  QCHECK(TrackingEntry()(nullptr, kLeft, 1.0, tracking) == 0);
  QCHECK(hpl::PollOnceForTest() == 2);  // the device line, and the toggle's start line (the first tracking call resolved it)
  QCHECK(CountOf("hand_flip_pose") == 0);

  SetTracking(0x0f, 0.5F, -0.5F, 0.25F, 0.75F);
  QCHECK(TrackingEntry()(nullptr, kLeft, 1.0, tracking) == 0);
  SetTracking(0x0f, 1.0F, 2.0F, 3.0F, 4.0F);  // a later pose of the same device
  QCHECK(TrackingEntry()(nullptr, kLeft, 1.0, tracking) == 0);
  QCHECK(hpl::PollOnceForTest() == 1);
  QCHECK(hpl::PollOnceForTest() == 0);  // once per device
  sentinel::SetLogSink(nullptr);

  QCHECK(CountOf("hand_flip_pose") == 1);
  const std::string* pose = LineOf("hand_flip_pose");
  QCHECK(pose != nullptr);
  if (pose != nullptr) {
    QCHECK(Golden(*pose) ==
           "\"event\":\"hand_flip_pose\",\"device\":536870914,\"hand\":\"left\",\"status\":\"0x0000000f\",\"qx\":\"0.500000\","
           "\"qy\":\"-0.500000\",\"qz\":\"0.250000\",\"qw\":\"0.750000\"}");
  }
}

// A pose for a device the table does not hold (a hand-tracking id, an id never enumerated) is never logged.
void APoseOfAnotherDeviceIsNotLogged() {
  Reset();
  Arm();
  sentinel::SetLogSink(&CaptureLine);
  g_devices = {{4, kLeft}, {8, kHandTracking}};
  EnumerateAll();
  alignas(8) unsigned char tracking[hpl::kTrackingBytes];
  QCHECK(TrackingEntry()(nullptr, kHandTracking, 1.0, tracking) == 0);
  QCHECK(TrackingEntry()(nullptr, kRight, 1.0, tracking) == 0);
  QCHECK(hpl::PollOnceForTest() == 1);  // the left controller's device line, no pose line
  sentinel::SetLogSink(nullptr);
  QCHECK(CountOf("hand_flip_pose") == 0);
}

// Nothing here writes anything but its table: with the hook disarmed after use the entries are the runtime's calls again and a
// poll finds what was recorded and nothing new.
void DisarmingTheHookStopsTheRecording() {
  Reset();
  Arm();
  g_devices = {{4, kLeft}};
  EnumerateAll();
  hpl::EnumerateThunk::Disarm();
  hpl::TrackingThunk::Disarm();
  g_devices.push_back({4, kRight});
  EnumerateAll();
  QCHECK(hpl::DeviceCountForTest() == 1);
  alignas(8) unsigned char tracking[hpl::kTrackingBytes];
  QCHECK(TrackingEntry()(nullptr, kLeft, 1.0, tracking) == 0);
  sentinel::SetLogSink(&CaptureLine);
  QCHECK(hpl::PollOnceForTest() == 1);  // the device line of the first device; no pose was recorded after the disarm
  sentinel::SetLogSink(nullptr);
  QCHECK(CountOf("hand_flip_pose") == 0);
}

// ---- the interim roll (KNOWN-WRONG #2043) ---------------------------------------------------------------------------------

// The 104 bytes after a call for `id`, from the runtime's standard tracking buffer (g_tracking).
struct Result {
  int rc;
  unsigned char bytes[hpl::kTrackingBytes];
};
Result Track(std::uint32_t id) {
  Result r;
  std::memset(r.bytes, 0x5A, sizeof(r.bytes));
  r.rc = TrackingEntry()(nullptr, id, 1.0, r.bytes);
  return r;
}
float OrientationOf(const unsigned char* bytes, int i) {
  float v;
  std::memcpy(&v, bytes + offsetof(hpl::TrackingPrefix, orientation) + i * sizeof(float), sizeof(v));
  return v;
}

// Off is the default, and with it every byte the runtime wrote comes back as written (the measurement log's rule).
void WithTheToggleOffNothingIsChanged() {
  Reset();
  Arm();
  g_devices = {{4, kLeft}, {4, kRight}};
  EnumerateAll();
  QCHECK(!hf::Enabled());
  for (const std::uint32_t id : {kLeft, kRight}) {
    const Result r = Track(id);
    QCHECK(r.rc == 0 && std::memcmp(r.bytes, g_tracking, sizeof(g_tracking)) == 0);
  }
}

// On: a table device's four orientation floats are (y, -x, w, -z) of what the runtime wrote, and every other byte is as written.
void WithTheToggleOnOnlyTheFourOrientationFloatsChange() {
  Reset();
  Arm();
  g_devices = {{4, kLeft}, {4, kRight}};
  EnumerateAll();
  hf::SetFromMenu(1);
  for (const std::uint32_t id : {kLeft, kRight}) {
    const Result r = Track(id);
    QCHECK(r.rc == 0);
    // SetTracking(0x0f, 0.5, -0.5, 0.25, 0.75) is what the runtime wrote.
    QCHECK(OrientationOf(r.bytes, 0) == -0.5F && OrientationOf(r.bytes, 1) == -0.5F);
    QCHECK(OrientationOf(r.bytes, 2) == 0.75F && OrientationOf(r.bytes, 3) == -0.25F);
    const std::size_t from = offsetof(hpl::TrackingPrefix, orientation);
    QCHECK(std::memcmp(r.bytes, g_tracking, from) == 0);                                  // Status and padding
    QCHECK(std::memcmp(r.bytes + from + 16, g_tracking + from + 16, sizeof(g_tracking) - from - 16) == 0);  // pose, velocities, times
  }
}

// Only a type-4 device in the table is rolled: hand tracking, the headset and an id never enumerated are not.
void OnlyTypeFourIsRolled() {
  Reset();
  Arm();
  g_devices = {{1, kHmd}, {4, kLeft}, {8, kHandTracking}};
  EnumerateAll();
  hf::SetFromMenu(1);
  const Result left = Track(kLeft);
  QCHECK(OrientationOf(left.bytes, 0) == -0.5F);  // rolled
  for (const std::uint32_t id : {kHandTracking, kHmd, kRight}) {
    const Result r = Track(id);
    QCHECK(r.rc == 0 && std::memcmp(r.bytes, g_tracking, sizeof(g_tracking)) == 0);
  }
}

// The game polls twice per device in a frame (predicted, then current): each of the two results is rolled once, and neither is
// rolled twice.
void BothPollsOfAFrameAreRolled() {
  Reset();
  Arm();
  g_devices = {{4, kLeft}};
  EnumerateAll();
  hf::SetFromMenu(1);
  const Result predicted = Track(kLeft);
  const Result current = Track(kLeft);
  QCHECK(std::memcmp(predicted.bytes, current.bytes, sizeof(current.bytes)) == 0);
  QCHECK(OrientationOf(predicted.bytes, 0) == -0.5F && OrientationOf(predicted.bytes, 3) == -0.25F);
  QCHECK(OrientationOf(current.bytes, 0) == -0.5F && OrientationOf(current.bytes, 3) == -0.25F);
}

// The toggle takes effect on the next call, either way, with no restart and no re-enumeration.
void TheToggleTakesEffectOnTheNextCall() {
  Reset();
  Arm();
  g_devices = {{4, kLeft}};
  EnumerateAll();
  QCHECK(OrientationOf(Track(kLeft).bytes, 0) == 0.5F);
  hf::SetFromMenu(1);
  QCHECK(OrientationOf(Track(kLeft).bytes, 0) == -0.5F);
  hf::SetFromMenu(0);
  QCHECK(OrientationOf(Track(kLeft).bytes, 0) == 0.5F);
}

// A failed call is not touched, with the toggle On.
void AFailedCallIsNotRolled() {
  Reset();
  Arm();
  g_devices = {{4, kLeft}};
  EnumerateAll();
  hf::SetFromMenu(1);
  g_trackingRc = -3;
  alignas(8) unsigned char untouched[hpl::kTrackingBytes];
  std::memset(untouched, 0x77, sizeof(untouched));
  QCHECK(TrackingEntry()(nullptr, kLeft, 1.0, untouched) == -3);
  for (unsigned char byte : untouched) QCHECK(byte == 0x77);
}

// The pose line is the raw orientation, taken before the roll, so a log from a headset with the toggle On still shows what VrApi
// reported.
void ThePoseLineStaysRawWithTheToggleOn() {
  Reset();
  Arm();
  sentinel::SetLogSink(&CaptureLine);
  g_devices = {{4, kLeft}};
  EnumerateAll();
  hf::SetFromMenu(1);
  QCHECK(OrientationOf(Track(kLeft).bytes, 0) == -0.5F);
  hpl::PollOnceForTest();
  sentinel::SetLogSink(nullptr);
  const std::string* pose = LineOf("hand_flip_pose");
  QCHECK(pose != nullptr);
  if (pose != nullptr) {
    QCHECK(Golden(*pose) ==
           "\"event\":\"hand_flip_pose\",\"device\":536870914,\"hand\":\"left\",\"status\":\"0x0000000f\",\"qx\":\"0.500000\","
           "\"qy\":\"-0.500000\",\"qz\":\"0.250000\",\"qw\":\"0.750000\"}");
  }
}

// The device line says whether the toggle is on and what set it.
void TheDeviceLineSaysWhetherTheToggleIsOnAndWhatSetIt() {
  Reset();
  Arm();
  sentinel::SetLogSink(&CaptureLine);
  g_devices = {{4, kLeft}};
  EnumerateAll();
  hf::SetFromMenu(1);
  hpl::PollOnceForTest();
  sentinel::SetLogSink(nullptr);
  const std::string* device = LineOf("hand_flip_device");
  QCHECK(device != nullptr);
  if (device != nullptr) {
    QCHECK(device->find("\"model\":\"Quest 2\",\"flip\":1,\"by\":\"menu\"}") != std::string::npos);
  }
}

// The start line is the poller's, once, golden, after the first tracking call resolved it to the default.
void TheStartLineIsWrittenOnceByThePoller() {
  Reset();
  Arm();
  sentinel::SetLogSink(&CaptureLine);
  g_devices = {{4, kLeft}};
  EnumerateAll();
  Track(kLeft);
  hpl::PollOnceForTest();
  hpl::PollOnceForTest();
  sentinel::SetLogSink(nullptr);
  QCHECK(CountOf("hand_flip_start") == 1);
  const std::string* start = LineOf("hand_flip_start");
  QCHECK(start != nullptr);
  if (start != nullptr) {
    QCHECK(Golden(*start) ==
           "\"event\":\"hand_flip_start\",\"source\":\"default\",\"flip\":0,\"unversioned_marker_ignored\":0,"
           "\"consequence\":\"this start is not saved; only a change in the menu is remembered for the next launch\"}");
  }
}

// A result whose Status does not say the orientation is tracked holds no orientation to turn: it comes back as written with the toggle
// On, and the same call with the bit set is rolled.
void AnUntrackedPoseIsNotRolled() {
  Reset();
  Arm();
  g_devices = {{4, kLeft}};
  EnumerateAll();
  hf::SetFromMenu(1);
  SetTracking(0x0, 0.5F, -0.5F, 0.25F, 0.75F);
  const Result untracked = Track(kLeft);
  QCHECK(untracked.rc == 0 && std::memcmp(untracked.bytes, g_tracking, sizeof(g_tracking)) == 0);
  SetTracking(hpl::kStatusOrientationTracked, 0.5F, -0.5F, 0.25F, 0.75F);
  const Result tracked = Track(kLeft);
  QCHECK(OrientationOf(tracked.bytes, 0) == -0.5F && OrientationOf(tracked.bytes, 2) == 0.75F);
}

// The table holds eight and never frees: a ninth type-4 id is not recorded, not rolled, and said once by the poller with its id.
void ANinthIdIsNotRolledAndSaidOnce() {
  Reset();
  Arm();
  sentinel::SetLogSink(&CaptureLine);
  for (std::uint32_t i = 0; i < 9; ++i) g_devices.push_back({4, 0x30000000U + i});
  EnumerateAll();
  EnumerateAll();  // the game enumerates every frame: still one line
  hf::SetFromMenu(1);
  const Result ninth = Track(0x30000008U);
  QCHECK(ninth.rc == 0 && std::memcmp(ninth.bytes, g_tracking, sizeof(g_tracking)) == 0);  // not in the table: not rolled
  const Result first = Track(0x30000000U);
  QCHECK(OrientationOf(first.bytes, 0) == -0.5F);  // a table device still is
  hpl::PollOnceForTest();
  hpl::PollOnceForTest();
  sentinel::SetLogSink(nullptr);
  QCHECK(CountOf("hand_flip_table_full") == 1);
  const std::string* line = LineOf("hand_flip_table_full");
  QCHECK(line != nullptr);
  if (line != nullptr) {
    QCHECK(Golden(*line) ==
           "\"event\":\"hand_flip_table_full\",\"slots\":8,\"device\":805306376,"
           "\"consequence\":\"this controller's pose is not logged or rolled; the device table holds eight and never frees\"}");
    QCHECK(line->find("\"level\":\"warn\"") != std::string::npos);
  }
}

std::atomic<int> g_seamEntered{0};
std::atomic<int> g_seamReleased{0};
void BlockingSeam() noexcept {
  g_seamEntered.store(1);
  for (int i = 0; i < 10000 && g_seamReleased.load() == 0; ++i) {
    timespec nap{0, 1000000};
    nanosleep(&nap, nullptr);
  }
}
void* StartThread(void*) {
  hf::StartFromDirectory(nullptr);
  return nullptr;
}

// The per-frame path never waits for another thread's start: while one is mid-resolve a tracking call comes back at once, as written
// (the flag reads Off for that frame), though the wait a settings delivery would make is set long.
void TheTrackingPathNeverWaitsForAnotherThreadsStart() {
  Reset();
  Arm();
  g_devices = {{4, kLeft}};
  EnumerateAll();
  g_seamEntered.store(0);
  g_seamReleased.store(0);
  hf::g_startSeam.store(&BlockingSeam);
  hf::g_startWaitMs.store(5000);
  pthread_t starter;
  QCHECK(pthread_create(&starter, nullptr, &StartThread, nullptr) == 0);
  for (int i = 0; i < 5000 && g_seamEntered.load() == 0; ++i) {
    timespec nap{0, 1000000};
    nanosleep(&nap, nullptr);
  }
  QCHECK(g_seamEntered.load() == 1 && !hf::Started());
  timespec before, after;
  clock_gettime(CLOCK_MONOTONIC, &before);
  const Result r = Track(kLeft);
  clock_gettime(CLOCK_MONOTONIC, &after);
  const long elapsedMs = (after.tv_sec - before.tv_sec) * 1000L + (after.tv_nsec - before.tv_nsec) / 1000000L;
  QCHECK(r.rc == 0 && std::memcmp(r.bytes, g_tracking, sizeof(g_tracking)) == 0);
  QCHECK(elapsedMs < 1000);
  g_seamReleased.store(1);
  pthread_join(starter, nullptr);
  hf::g_startSeam.store(nullptr);
}

// The row is offered only when the hooks that roll, save and log it are installed: an install that is refused says so, and the
// row is not offered (the texture hook's menu then has none).
void ARefusedInstallLeavesTheRowNotOffered() {
  Reset();
  QCHECK(hf::Offered());  // a test starts offered
  sentinel::SetLogSink(&CaptureLine);
  const sentinel::GotTarget missing{"libnevr_no_such_library.so", "vrapi_EnumerateInputDevices", sentinel::RelocKind::kJumpSlot};
  QCHECK(!hpl::InstallAt(missing, missing, nullptr));
  sentinel::SetLogSink(nullptr);
  QCHECK(!hf::Offered());
  QCHECK(CountOf("hand_pose_log_install") == 1);
}

// The two slots the hooks are installed in are the pinned ones (got_pinned_test resolves them in the real library): the roll
// rides on the existing tracking hook and adds no slot.
void TheRollAddsNoSlot() {
  QCHECK(sentinel::pinned::LibR15EnumerateInputDevices().slotVaddr == std::optional<std::uint64_t>(0x36d5dd0ULL));
  QCHECK(sentinel::pinned::LibR15GetInputTrackingState().slotVaddr == std::optional<std::uint64_t>(0x36dc280ULL));
}

}  // namespace

int main() {
  WithoutTheHookNothingIsRecorded();
  EnumerateReturnsTheRuntimesResultAndBytes();
  OnlyTypeFourEntersTheTable();
  CapabilitiesAreAskedOncePerDevice();
  ANinthDeviceIsIgnored();
  TrackingReturnsTheRuntimesResultAndBytes();
  TheDeviceLineIsGolden();
  ADeviceWithNoCapabilitiesIsLoggedAsSuch();
  ThePoseLineIsTheFirstTrackedOrientationAndWrittenOnce();
  APoseOfAnotherDeviceIsNotLogged();
  DisarmingTheHookStopsTheRecording();
  WithTheToggleOffNothingIsChanged();
  WithTheToggleOnOnlyTheFourOrientationFloatsChange();
  OnlyTypeFourIsRolled();
  BothPollsOfAFrameAreRolled();
  TheToggleTakesEffectOnTheNextCall();
  AFailedCallIsNotRolled();
  ThePoseLineStaysRawWithTheToggleOn();
  TheDeviceLineSaysWhetherTheToggleIsOnAndWhatSetIt();
  TheStartLineIsWrittenOnceByThePoller();
  TheRollAddsNoSlot();
  AnUntrackedPoseIsNotRolled();
  ANinthIdIsNotRolledAndSaidOnce();
  TheTrackingPathNeverWaitsForAnotherThreadsStart();
  ARefusedInstallLeavesTheRowNotOffered();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "hand_pose_log_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("hand_pose_log_hook_test: all checks pass\n");
  return 0;
}
