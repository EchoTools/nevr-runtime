#include "hand_pose_log_hook.h"

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "hand_flip.h"
#include "hook_install.h"
#include "hook_log.h"

namespace nevr_quest::hand_pose_log {

namespace {

namespace pin = sentinel::pinned;

sentinel::GotHook g_enumerateHook, g_trackingHook;
std::atomic<input_seam::CapsFn> g_caps{nullptr};
std::atomic<PropertyReader> g_property{nullptr};

// ---- the device table ---------------------------------------------------------------------------------------------------
//
// Written by the game's thread inside the two handlers, read by the poller. An entry is a seqlock: the writer makes `seq` odd,
// stores the words, makes it even; the reader takes a copy and keeps it only when `seq` was even and did not move. Every word
// is its own atomic, so a torn read is a retry and never a data race. A writer that finds `seq` odd (it cannot, the game polls
// from one thread) skips: the next poll offers the same data again.

constexpr std::uint32_t kUsed = 1;
constexpr std::uint32_t kCapsAsked = 2;  // the capabilities were asked (kCapsRead says whether the call succeeded)
constexpr std::uint32_t kCapsRead = 4;
constexpr std::uint32_t kPoseSeen = 8;
constexpr int kCapWords = 5;   // ControllerCapabilities, ButtonCapabilities, TouchCapabilities, Reserved4, Reserved5
constexpr int kPoseWords = 5;  // Status, then the orientation x, y, z, w as bit patterns

struct Slot {
  std::atomic<std::uint32_t> seq;
  std::atomic<std::uint32_t> id;
  std::atomic<std::uint32_t> state;
  std::atomic<std::uint32_t> caps[kCapWords];
  std::atomic<std::uint32_t> pose[kPoseWords];
};
Slot g_slots[kDeviceSlots];  // zero-initialised static storage: no constructor runs at load
// KNOWN-WRONG #2043 Enumerations that found a type-4 id with no free slot (the table never frees), and the first such id. The
// poller says it once (hand_flip_table_full): that controller's pose is not rolled.
std::atomic<std::uint64_t> g_tableFullCount{0};
std::atomic<std::uint32_t> g_tableFullFirstId{0};
bool g_tableFullLogged = false;  // the poller pass only

bool BeginWrite(Slot& slot, std::uint32_t* token) noexcept {
  std::uint32_t seq = slot.seq.load(std::memory_order_relaxed);
  if ((seq & 1U) != 0) return false;
  if (!slot.seq.compare_exchange_strong(seq, seq + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) return false;
  *token = seq;
  return true;
}
void EndWrite(Slot& slot, std::uint32_t token) noexcept { slot.seq.store(token + 2, std::memory_order_release); }

// The entry for `id` (type 4 only ever enters), or null.
Slot* Find(std::uint32_t id) noexcept {
  for (Slot& slot : g_slots) {
    if ((slot.state.load(std::memory_order_acquire) & kUsed) != 0 && slot.id.load(std::memory_order_relaxed) == id) return &slot;
  }
  return nullptr;
}

Slot* FirstFree() noexcept {
  for (Slot& slot : g_slots) {
    if ((slot.state.load(std::memory_order_acquire) & kUsed) == 0) return &slot;
  }
  return nullptr;
}

std::uint32_t Bits(float value) noexcept {
  std::uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
float FromBits(std::uint32_t bits) noexcept {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// ---- the two handlers ---------------------------------------------------------------------------------------------------

// vrapi_EnumerateInputDevices: the original first; its result and the header it filled go back untouched. A type-4 device not
// yet in the table is added, with its capabilities read once through libr15's own function (the call PollTracking makes next,
// on this thread, with this ovr). Never logs, allocates or throws: it is live across the call into the runtime and the
// capabilities call (callback_thunk.h rule 1).
int EnumerateHandler(EnumerateThunk::Fn original, pin::OvrMobileOpaque* ovr, std::uint32_t index, void* header) noexcept {
  const int rc = original(ovr, index, header);
  if (rc < 0 || header == nullptr) return rc;
  CapabilityHeader h;
  std::memcpy(&h, header, sizeof(h));
  if (h.type != input_seam::kControllerTypeTrackedRemote) return rc;
  if (Find(h.deviceId) != nullptr) return rc;
  Slot* const slot = FirstFree();
  if (slot == nullptr) {  // a ninth device is not recorded, and so is not rolled
    if (g_tableFullCount.fetch_add(1, std::memory_order_relaxed) == 0) g_tableFullFirstId.store(h.deviceId, std::memory_order_relaxed);
    return rc;
  }

  input_seam::VrapiTrackedRemoteCaps caps{};
  caps.type = input_seam::kControllerTypeTrackedRemote;
  caps.deviceId = h.deviceId;
  const input_seam::CapsFn fn = g_caps.load(std::memory_order_acquire);
  const bool read = fn != nullptr && fn(ovr, &caps) == 0;

  std::uint32_t token = 0;
  if (!BeginWrite(*slot, &token)) return rc;
  slot->id.store(h.deviceId, std::memory_order_relaxed);
  slot->caps[0].store(read ? caps.controllerCapabilities : 0, std::memory_order_relaxed);
  slot->caps[1].store(read ? caps.buttonCapabilities : 0, std::memory_order_relaxed);
  slot->caps[2].store(read ? caps.touchCapabilities : 0, std::memory_order_relaxed);
  slot->caps[3].store(read ? caps.reserved4 : 0, std::memory_order_relaxed);
  slot->caps[4].store(read ? caps.reserved5 : 0, std::memory_order_relaxed);
  slot->state.store(kUsed | kCapsAsked | (read ? kCapsRead : 0), std::memory_order_release);
  EndWrite(*slot, token);
  return rc;
}

// Stores the raw orientation of a table device once, from its first result whose Status says the orientation is tracked. The
// measurement log reads it, so it is taken before the roll below.
void RecordFirstPose(Slot* slot, const void* tracking) noexcept {
  if ((slot->state.load(std::memory_order_acquire) & kPoseSeen) != 0) return;
  TrackingPrefix prefix;
  std::memcpy(&prefix, tracking, sizeof(prefix));
  if ((prefix.status & kStatusOrientationTracked) == 0) return;
  std::uint32_t token = 0;
  if (!BeginWrite(*slot, &token)) return;
  slot->pose[0].store(prefix.status, std::memory_order_relaxed);
  for (int i = 0; i < 4; ++i) slot->pose[1 + i].store(Bits(prefix.orientation[i]), std::memory_order_relaxed);
  slot->state.store(slot->state.load(std::memory_order_relaxed) | kPoseSeen, std::memory_order_release);
  EndWrite(*slot, token);
}

// KNOWN-WRONG #2043 The interim roll (hand_flip.h): the orientation of a table device (type 4 only) is post-multiplied by a 180
// degree roll about its local Z axis, in place, when the result's Status says the orientation is tracked (an untracked result holds
// no orientation to turn). The four orientation floats are the only bytes written.
void RollOrientation(void* tracking) noexcept {
  TrackingPrefix prefix;
  std::memcpy(&prefix, tracking, sizeof(prefix));
  if ((prefix.status & kStatusOrientationTracked) == 0) return;
  float orientation[4];
  std::memcpy(orientation, static_cast<unsigned char*>(tracking) + offsetof(TrackingPrefix, orientation), sizeof(orientation));
  hand_flip::Roll(orientation);
  std::memcpy(static_cast<unsigned char*>(tracking) + offsetof(TrackingPrefix, orientation), orientation, sizeof(orientation));
}

// vrapi_GetInputTrackingState: the original first; its result goes back as it came. The first result of a table device whose
// Status says the orientation is tracked gives its raw orientation, stored once. KNOWN-WRONG #2043 Then, for a table device (type
// 4: a hand-tracking or unknown id is never in the table) and only while Controller Hand Flip is On, the orientation is rolled;
// every other byte of the result stays as the runtime wrote it. Each call of a frame (the game polls twice per device) is rolled.
// Same rule as above: never logs, allocates or throws.
int TrackingHandler(TrackingThunk::Fn original, pin::OvrMobileOpaque* ovr, std::uint32_t deviceId, double absTime,
                    void* tracking) noexcept {
  const int rc = original(ovr, deviceId, absTime, tracking);
  if (rc < 0 || tracking == nullptr) return rc;
  Slot* const slot = Find(deviceId);
  if (slot == nullptr) return rc;
  RecordFirstPose(slot, tracking);
  if (!hand_flip::Started()) hand_flip::EnsureStarted();  // resolves it if nobody has; never waits for another thread's resolve
  if (hand_flip::Started() && hand_flip::Enabled()) RollOrientation(tracking);  // not started: the flag reads Off for this frame
  return rc;
}

NEVR_HOOK_RECORD(kEnumerateRecord, EnumerateThunk, &EnumerateHandler);
NEVR_HOOK_RECORD(kTrackingRecord, TrackingThunk, &TrackingHandler);

// ---- the poller: the only place this feature logs -----------------------------------------------------------------------

struct Snapshot {
  std::uint32_t id;
  std::uint32_t state;
  std::uint32_t caps[kCapWords];
  std::uint32_t pose[kPoseWords];
};

// A consistent copy of an entry, or false (being written: the next pass takes it).
bool Take(const Slot& slot, Snapshot* out) noexcept {
  const std::uint32_t before = slot.seq.load(std::memory_order_acquire);
  if ((before & 1U) != 0) return false;
  out->id = slot.id.load(std::memory_order_relaxed);
  out->state = slot.state.load(std::memory_order_relaxed);
  for (int i = 0; i < kCapWords; ++i) out->caps[i] = slot.caps[i].load(std::memory_order_relaxed);
  for (int i = 0; i < kPoseWords; ++i) out->pose[i] = slot.pose[i].load(std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_acquire);
  return slot.seq.load(std::memory_order_relaxed) == before;
}

bool g_loggedDevice[kDeviceSlots];  // touched by the poller pass only (PollMutex)
bool g_loggedPose[kDeviceSlots];
char g_model[92];
bool g_modelRead = false;

#if defined(__ANDROID__)
int AndroidProperty(const char* name, char* out) noexcept { return __system_property_get(name, out); }
#endif

void ReadModel() noexcept {
  if (g_modelRead) return;
  g_modelRead = true;
  std::snprintf(g_model, sizeof(g_model), "%s", "unknown");
  const PropertyReader property = g_property.load(std::memory_order_acquire);
  if (property == nullptr) return;
  char value[92] = {};
  if (property("ro.product.model", value) > 0) std::snprintf(g_model, sizeof(g_model), "%s", value);
}

const char* HandName(const Snapshot& s) noexcept {
  if ((s.state & kCapsRead) == 0) return "unknown";
  const std::uint32_t hand = input_seam::HandFromCaps(s.caps[0]);
  return hand == NEVR_QUEST_HAND_LEFT ? "left" : (hand == NEVR_QUEST_HAND_RIGHT ? "right" : "unknown");
}

int PollLocked() noexcept {
  int lines = 0;
  for (int i = 0; i < kDeviceSlots; ++i) {
    Snapshot s;
    if (!Take(g_slots[i], &s) || (s.state & kUsed) == 0) continue;
    if (!g_loggedDevice[i] && (s.state & kCapsAsked) != 0) {
      g_loggedDevice[i] = true;
      ReadModel();
      char caps[5][11];
      for (int w = 0; w < kCapWords; ++w) std::snprintf(caps[w], sizeof(caps[w]), "0x%08x", static_cast<unsigned>(s.caps[w]));
      sentinel::LogFields(sentinel::LogLevel::kInfo, "hand_flip_device",
                          {{"device", s.id},
                           {"type", input_seam::kControllerTypeTrackedRemote},
                           {"caps_read", (s.state & kCapsRead) != 0 ? "yes" : "no"},
                           {"caps", caps[0]},
                           {"button_caps", caps[1]},
                           {"touch_caps", caps[2]},
                           {"reserved4", caps[3]},
                           {"reserved5", caps[4]},
                           {"hand", HandName(s)},
                           {"model", g_model},
                           // KNOWN-WRONG #2043 the toggle's flag and what set it (hand_flip.h) leave this line with the toggle.
                           {"flip", hand_flip::Current()},
                           {"by", hand_flip::ByName(hand_flip::CurrentBy())}});
      ++lines;
    }
    if (!g_loggedPose[i] && (s.state & kPoseSeen) != 0) {
      g_loggedPose[i] = true;
      char status[11];
      std::snprintf(status, sizeof(status), "0x%08x", static_cast<unsigned>(s.pose[0]));
      char q[4][48];  // a float prints at most 47 characters with six decimals
      for (int w = 0; w < 4; ++w) std::snprintf(q[w], sizeof(q[w]), "%.6f", static_cast<double>(FromBits(s.pose[1 + w])));
      sentinel::LogFields(sentinel::LogLevel::kInfo, "hand_flip_pose",
                          {{"device", s.id},
                           {"hand", HandName(s)},
                           {"status", status},
                           {"qx", q[0]},
                           {"qy", q[1]},
                           {"qz", q[2]},
                           {"qw", q[3]}});
      ++lines;
    }
  }
  if (!g_tableFullLogged && g_tableFullCount.load(std::memory_order_relaxed) != 0) {
    g_tableFullLogged = true;  // KNOWN-WRONG #2043 the table is the interim's and never frees
    sentinel::LogFields(sentinel::LogLevel::kWarn, "hand_flip_table_full",
                        {{"slots", kDeviceSlots},
                         {"device", g_tableFullFirstId.load(std::memory_order_relaxed)},
                         {"consequence", "this controller's pose is not logged or rolled; the device table holds eight and never frees"}});
    ++lines;
  }
  lines += hand_flip::Poll();  // KNOWN-WRONG #2043 the toggle's start, carrier and save lines
  return lines;
}

pthread_mutex_t g_pollMutex = PTHREAD_MUTEX_INITIALIZER;  // one pass at a time (the thread and a test)
pthread_mutex_t g_wait = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond;
bool g_condReady = false;
bool g_running = false;
pthread_t g_thread;
constexpr unsigned kPeriodMs = 500;

int PollOnce() noexcept {
  pthread_mutex_lock(&g_pollMutex);
  const int lines = PollLocked();
  pthread_mutex_unlock(&g_pollMutex);
  return lines;
}

// The poller has no stop: it ends with the process, like the texture poller (texture_menu_hook.cpp) and the reporter thread
// (hook_report.h, "Teardown"); the sentinel is never unloaded.
void* PollerMain(void*) {
  pthread_mutex_lock(&g_wait);
  for (;;) {
    timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += static_cast<long>(kPeriodMs) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_nsec -= 1000000000L;
      ++deadline.tv_sec;
    }
    int rc = 0;
    while (rc == 0) rc = pthread_cond_timedwait(&g_cond, &g_wait, &deadline);  // nothing signals it: it ends at the deadline
    pthread_mutex_unlock(&g_wait);
    PollOnce();
    pthread_mutex_lock(&g_wait);
  }
  return nullptr;
}

bool StartPoller() noexcept {
  if (g_running) return true;
  if (!g_condReady) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cond, &attr);
    pthread_condattr_destroy(&attr);
    g_condReady = true;
  }
  const int rc = pthread_create(&g_thread, nullptr, &PollerMain, nullptr);
  if (rc == 0) {
    pthread_setname_np(g_thread, "nevr-handpose");
    g_running = true;
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "hand_pose_log_install", {{"result", "poller_not_started"}, {"errno", rc}});
  }
  return rc == 0;
}

void Arm(input_seam::CapsFn caps, PropertyReader property) noexcept {
  g_caps.store(caps, std::memory_order_release);
  g_property.store(property, std::memory_order_release);
  EnumerateThunk::Arm(kEnumerateRecord);
  TrackingThunk::Arm(kTrackingRecord);
}

// The function libr15's vrapi_GetInputDeviceCapabilities slot holds. The slot is read, never written (here or by the input
// seam, which reads it the same way), so the order of the two readers does not matter. Null when libr15 is not the patched
// build or the slot does not resolve.
input_seam::CapsFn ResolveGameCaps() noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(pin::kLibR15, &image)) return nullptr;
  char id[80] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id)) || std::strcmp(id, pin::LibR15PatchedBuildId()) != 0) return nullptr;
  const sentinel::SlotResolution slot = sentinel::ResolveSlot(image, pin::LibR15GetInputDeviceCapabilities(), sentinel::kNativeRelocs);
  if (slot.status != sentinel::GotStatus::kOk || slot.slot == nullptr) return nullptr;
  return reinterpret_cast<input_seam::CapsFn>(__atomic_load_n(slot.slot, __ATOMIC_ACQUIRE));
}

}  // namespace

bool InstallAt(const sentinel::GotTarget& enumerate, const sentinel::GotTarget& tracking, input_seam::CapsFn caps) noexcept {
#if defined(__ANDROID__)
  const PropertyReader property = &AndroidProperty;
#else
  const PropertyReader property = nullptr;
#endif
  Arm(caps, property);
  const sentinel::GotStatus e = sentinel::InstallThunk<EnumerateThunk>(g_enumerateHook, enumerate);
  const sentinel::GotStatus t = e == sentinel::GotStatus::kOk ? sentinel::InstallThunk<TrackingThunk>(g_trackingHook, tracking)
                                                               : sentinel::GotStatus::kNotInstalled;
  const bool all = e == sentinel::GotStatus::kOk && t == sentinel::GotStatus::kOk;
  sentinel::LogFields(all ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "hand_pose_log_install",
                      {{"result", all ? "installed" : (e == sentinel::GotStatus::kOk ? "partial" : "refused")},
                       {"enumerate_input_devices", sentinel::GotStatusName(e)},
                       {"get_input_tracking_state", sentinel::GotStatusName(t)},
                       {"caps_function", caps != nullptr ? "found" : "missing"},
                       {"consequence", "a log of each controller's type and raw pose; a pose is rolled only while Controller Hand Flip is On"}});
  const bool polling = e == sentinel::GotStatus::kOk && StartPoller();
  // KNOWN-WRONG #2043 The toggle's row is offered only when both hooks and the poller that saves and logs are in.
  hand_flip::SetOffered(all && polling);
  return all;
}

bool Install() noexcept {
  return InstallAt(pin::LibR15EnumerateInputDevices(), pin::LibR15GetInputTrackingState(), ResolveGameCaps());
}

void ArmForTest(input_seam::CapsFn caps, PropertyReader property) noexcept { Arm(caps, property); }

void ResetForTest() noexcept {
  for (Slot& slot : g_slots) {
    slot.seq.store(0, std::memory_order_relaxed);
    slot.id.store(0, std::memory_order_relaxed);
    slot.state.store(0, std::memory_order_relaxed);
    for (auto& word : slot.caps) word.store(0, std::memory_order_relaxed);
    for (auto& word : slot.pose) word.store(0, std::memory_order_relaxed);
  }
  for (bool& logged : g_loggedDevice) logged = false;
  for (bool& logged : g_loggedPose) logged = false;
  g_modelRead = false;
  g_model[0] = '\0';
  g_tableFullCount.store(0);
  g_tableFullFirstId.store(0);
  g_tableFullLogged = false;
  hand_flip::ResetForTest();
  g_caps.store(nullptr, std::memory_order_relaxed);
  g_property.store(nullptr, std::memory_order_relaxed);
}

int PollOnceForTest() noexcept { return PollOnce(); }

int DeviceCountForTest() noexcept {
  int count = 0;
  for (const Slot& slot : g_slots) {
    if ((slot.state.load(std::memory_order_acquire) & kUsed) != 0) ++count;
  }
  return count;
}

}  // namespace nevr_quest::hand_pose_log
