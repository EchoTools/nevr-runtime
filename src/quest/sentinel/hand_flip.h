/* The Controller Hand Flip toggle (#1986): the interim roll, its flag, its persisted choice and its log lines.
 *
 * KNOWN-WRONG #2043 This whole header is the interim. A controller fix belongs to the VrApi-on-OpenXR layer (ADR 0033 rules 1 and
 * 3, docs/design/2026-11-02-quest-hand-flip-toggle.md); it is here, above the VrApi boundary, only for the players who need it
 * before the layer carries it, and it is dropped in the change that moves the roll into the layer (#2043).
 *
 * What it does. Players on a Quest 2 with Touch Pro or Touch Plus controllers report hands rolled backwards. While the player has
 * Controller Hand Flip On, the tracking handler (hand_pose_log_hook.cpp) post-multiplies the orientation of every type-4
 * (TrackedRemote) device by a 180 degree roll about the controller's local Z axis: q' = q * (0, 0, 1, 0) = (y, -x, w, -z) for
 * q = (x, y, z, w). Position, velocities, accelerations, status and times stay as the runtime wrote them. Hand tracking and
 * unknown device ids are never touched. Default Off: nothing here chooses by controller (the sentinel does not read the
 * interaction profile, ADR 0033 C1).
 *
 * The menu row. texture_menu.cpp adds "Controller Hand Flip" under Texture Resolution on the NEVR page (texture_quality.h
 * kMenuPageName). It is a NAMED FLAG row: a boolean the game shows by the row's `truestr` and `falsestr` and writes back as a JSON
 * boolean (a STRING ARRAY row writes its index, which the game's json-path writer refuses to put in a boolean key: "cannot change
 * type of [graphics|fullscreen] to integer"). The row is bound to the setting `graphics|fullscreen`, which the game stores in
 * SR15GfxSettings flags bit 0
 * (kCarrierBit): a Quest has no fullscreen mode, and the consumer audit (docs/design/2026-11-02-quest-hand-flip-toggle.md,
 * "The interim in the sentinel") found nothing in libr15 that reads the bit except a log line in ApplyRuntimeGfxSettings, the
 * JSON round trip of the settings file, and the script expression CR15GfxSettingsExpression, which copies the struct and exposes
 * the bit as a boolean output.
 *
 * The polarity. The game's own value of the bit is 1 (the v6.16.4 headset log prints `Fullscreen: Enabled` at every start), so
 * Off is the bit set (the row's true, `truestr` "Off"), which is the game's value, and On is the bit cleared (false, `falsestr`
 * "On"). A player who never turns the toggle on therefore
 * leaves the game's setting as the game holds it (nothing is changed, nothing differs when it is saved). texture_menu_hook.cpp's
 * HandleApply reads the bit on every delivery: the first delivery is set to the stored choice (or the default, Off), a later
 * delivery with another bit is the player's choice (SetFromMenu with the choice the bit now means). BitFor and FlipFor are the one
 * place the inversion is written; the hook hands the menu a choice as MenuIndexFor (index 0 is "On"), and the row's `value` for that
 * index is MenuValueFor (true for Off).
 *
 * The persisted choice. Its own file beside the game's settings file (RAD/LoneEcho/), {"version":1,"hand_flip":0|1} (kFileName), written only by a
 * menu change (a derived or default value is never saved). A file without the version, with another version, or with a value
 * other than 0 or 1 is ignored and the ignore is logged (hand_flip_start unversioned_marker_ignored=1). It is read once, by
 * StartFromDirectory, before the first tracking call can use the flag: the texture hook's start resolver calls it on the game's
 * first eye-size query, and the tracking handler calls EnsureStarted() when that has not happened.
 *
 * Offered. The row exists only when the pieces that make it mean something are installed: the two hand pose hooks (the roll) and the
 * poller that saves and logs (hand_pose_log_hook.cpp InstallAt calls SetOffered). When it is not offered the menu has no row and the
 * stock hiding entry of the setting stays, the saved choice is not read (the roll stays Off), the bit is never written, and the
 * texture poller says so once (hand_flip_row).
 *
 * The start and who may trust it. The start is resolved by whichever thread asks first (the texture hook's eye-size query, the
 * tracking handler); a thread that finds it being resolved by another returns at once. The tracking handler is the per-frame path
 * and never waits: until the start is resolved the flag reads Off for that frame. The settings delivery and the menu change
 * (AwaitStart) wait, on a condition variable with a timeout, until the start is resolved before they read the flag, so a first
 * delivery cannot write the bit from a flag the start has not yet set. A wait that times out leaves the load to the next delivery.
 *
 * Nothing on the game's call path logs. The poller thread of hand_pose_log_hook.cpp calls Poll(), which writes
 *   {"event":"hand_flip_start","source":"player_marker|default","flip":0|1,"unversioned_marker_ignored":0|1,"consequence":...}
 * once, then, once, the write of the first delivery into the game's bit
 *   {"event":"hand_flip_carrier","before":0|1,"after":0|1,"consequence":...}
 * (before is the bit as the game held it, after the bit the toggle's start chose: with the default, Off, both are 1), and, after a
 * change in the menu,
 *   {"event":"hand_flip_saved","result":"written|write_failed|no_directory","flip":0|1,"errno":N,"consequence":...}
 * The existing hand_flip_device line carries `flip` (the flag when it is written) and `by` (default, player_marker or menu).
 * There is no per-frame logging.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h. State is constant-initialised
 * inline variables: no static initialiser runs at load (tools/check_quest_static_init.sh).
 */
#pragma once

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "hook_log.h"

namespace nevr_quest::hand_flip {

// The menu row: the setting it is bound to, the SR15GfxSettings flags word the setting lives in, its bit, and what the player reads.
inline constexpr const char* kMenuPath = "graphics|fullscreen";
inline constexpr std::size_t kCarrierOffset = 0x00;
inline constexpr std::uint32_t kCarrierBit = 1U;
inline constexpr const char* kMenuName = "Controller Hand Flip";
inline constexpr int kMenuValueCount = 2;
inline constexpr const char* kMenuLabels[kMenuValueCount] = {"On", "Off"};
inline constexpr const char* kMenuTrueLabel = "Off";  // the row's `truestr`: true is the bit set, the game's value
inline constexpr const char* kMenuFalseLabel = "On";  // the row's `falsestr`: false is the bit cleared, the roll on

// The persisted choice: a file of our own beside the game's settings file (RAD/LoneEcho/ under the game's local app data directory).
inline constexpr char kFileName[] = "nevr_hand_flip.json";
inline constexpr int kFileVersion = 1;

// The bit the game holds for a choice, the choice a bit means, and the menu index of a choice (the row lists "On" first, so On is
// index 0). The game's own value of the bit is 1 (Off); each is its own inverse.
inline constexpr int BitFor(int flip) noexcept { return flip != 0 ? 0 : 1; }
inline constexpr int FlipFor(int bit) noexcept { return bit != 0 ? 0 : 1; }
inline constexpr int MenuIndexFor(int flip) noexcept { return flip != 0 ? 0 : 1; }
// The NAMED FLAG row's `value` for a menu index: index 0 (On) is false, the bit cleared; any other index (Off) is true, the bit set.
inline constexpr bool MenuValueFor(int menuIndex) noexcept { return menuIndex != 0; }

// What set the flag: nothing (the default), the player's file at the start, or a change in the menu.
enum By : int { kByDefault = 0, kByPlayerMarker = 1, kByMenu = 2 };

inline const char* ByName(int by) noexcept {
  return by == kByPlayerMarker ? "player_marker" : (by == kByMenu ? "menu" : "default");
}

// ---- the roll ------------------------------------------------------------------------------------------------------------

// q = (x, y, z, w) -> q * (0, 0, 1, 0) = (y, -x, w, -z): 180 degrees about the local Z axis. Applied twice it gives -q, the same
// rotation.
inline void Roll(float q[4]) noexcept {
  const float x = q[0];
  const float y = q[1];
  const float z = q[2];
  const float w = q[3];
  q[0] = y;
  q[1] = -x;
  q[2] = w;
  q[3] = -z;
}

// ---- the file ------------------------------------------------------------------------------------------------------------

enum class FileStatus {
  kValid,    // a versioned file with hand_flip 0 or 1: `flip` is the player's choice
  kIgnored,  // a file that is not ours to read: no version, another version, or a value other than 0 or 1
  kAbsent,   // no hand_flip key
};

namespace detail {

// The whole number after the first `key` (a quoted JSON key, with its quotes) in `text`, or -1 when the key is absent, cut off by
// `size`, or not followed by a whole number. A negative number reads -2 (never a valid value), a huge one saturates at 1000.
// Allocation-free: it runs on the game's thread.
inline long WholeNumberAfterKey(const char* text, std::size_t size, const char* key, std::size_t keyLength) noexcept {
  if (text == nullptr) return -1;
  std::size_t at = 0;
  for (; at + keyLength <= size; ++at) {
    std::size_t i = 0;
    while (i < keyLength && text[at + i] == key[i]) ++i;
    if (i == keyLength) break;
  }
  if (at + keyLength > size) return -1;
  std::size_t i = at + keyLength;
  while (i < size && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
  if (i >= size || text[i] != ':') return -1;
  ++i;
  while (i < size && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
  bool negative = false;
  if (i < size && text[i] == '-') {
    negative = true;
    ++i;
  }
  if (i >= size || text[i] < '0' || text[i] > '9') return -1;
  long value = 0;
  while (i < size && text[i] >= '0' && text[i] <= '9') {
    if (value < 1000) value = value * 10 + (text[i] - '0');
    ++i;
  }
  if (i < size && (text[i] == '.' || text[i] == 'e' || text[i] == 'E')) return -1;  // not a whole number
  return negative ? -2 : value;
}

}  // namespace detail

// Reads the persisted choice from a file's text. Allocation-free.
inline FileStatus ParseFile(const char* text, std::size_t size, int* flip) noexcept {
  constexpr char kValueKey[] = "\"hand_flip\"";
  constexpr char kVersionKey[] = "\"version\"";
  const long value = detail::WholeNumberAfterKey(text, size, kValueKey, sizeof(kValueKey) - 1);
  if (value == -1) return FileStatus::kAbsent;
  const long version = detail::WholeNumberAfterKey(text, size, kVersionKey, sizeof(kVersionKey) - 1);
  if (version != kFileVersion || (value != 0 && value != 1)) return FileStatus::kIgnored;
  *flip = static_cast<int>(value);
  return FileStatus::kValid;
}

// The text the file holds for a choice: {"version":1,"hand_flip":N}. Returns the length written (0 when `capacity` is too small).
inline std::size_t FileText(int flip, char* out, std::size_t capacity) noexcept {
  const int n = std::snprintf(out, capacity, "{\"version\":%d,\"hand_flip\":%d}", kFileVersion, flip != 0 ? 1 : 0);
  return n > 0 && static_cast<std::size_t>(n) < capacity ? static_cast<std::size_t>(n) : 0;
}

// ---- the state -----------------------------------------------------------------------------------------------------------

inline std::atomic<int> g_flip{0};               // the flag the tracking handler reads on every call
inline std::atomic<int> g_by{kByDefault};        // what set it
inline std::atomic<bool> g_offered{false};       // the hooks that roll, save and log are installed (SetOffered); false: no row
inline std::atomic<int> g_startState{0};         // 0 not started, 1 being resolved, 2 resolved
inline std::atomic<int> g_startSource{kByDefault};  // kByPlayerMarker when the file gave the start, else kByDefault
inline std::atomic<int> g_startFlip{0};          // the flag the start resolved to (the menu may have changed g_flip since)
inline std::atomic<bool> g_ignored{false};       // the file was there and was not ours to read
inline std::atomic<int> g_pendingSave{-1};       // the menu's last choice the poller has not yet written (-1: none)
inline std::atomic<int> g_carrierBefore{-1};     // the bit as the game held it at the first delivery (-1: no delivery yet)
inline std::atomic<int> g_carrierAfter{-1};      // the bit after the first delivery set it to the start's choice
inline std::atomic<bool> g_pathReady{false};
inline char g_path[704];                         // the file's path, written once before g_pathReady
inline std::atomic<void (*)() noexcept> g_provider{nullptr};  // what resolves the start from the game's directory
inline std::atomic<int> g_failedFlip{-1};        // the choice whose save last failed (-1: none), so a retry does not repeat the line
inline std::atomic<std::uint64_t> g_startTimeouts{0};  // waits for the start that gave up
inline std::atomic<int> g_startWaitMs{2000};     // how long the delivery waits for another thread's start to resolve
inline std::atomic<int> g_startWaiters{0};       // threads inside the wait (for the test)
inline std::atomic<void (*)() noexcept> g_startSeam{nullptr};  // test only: runs inside the start, after the file is read, before it stores
inline pthread_mutex_t g_startMutex = PTHREAD_MUTEX_INITIALIZER;  // constant-initialised: no static initialiser (check_quest_static_init)
inline pthread_cond_t g_startCond = PTHREAD_COND_INITIALIZER;
inline bool g_startLogged = false;               // the poller pass only (the poll mutex of hand_pose_log_hook.cpp)
inline bool g_timeoutLogged = false;             // likewise
inline bool g_rowLogged = false;                 // the texture poller's pass only
inline bool g_carrierLogged = false;             // likewise

// The row is offered only while the hooks that give it a meaning are installed (hand_pose_log::InstallAt). Until they say so it is not.
inline bool Offered() noexcept { return g_offered.load(std::memory_order_acquire); }
inline void SetOffered(bool offered) noexcept { g_offered.store(offered, std::memory_order_release); }

inline bool Enabled() noexcept { return g_flip.load(std::memory_order_relaxed) != 0; }
inline int Current() noexcept { return g_flip.load(std::memory_order_relaxed); }
inline bool Started() noexcept { return g_startState.load(std::memory_order_acquire) == 2; }
inline int CurrentBy() noexcept { return g_by.load(std::memory_order_relaxed); }

// Resolves the start once, from the game's local app data directory (`directory`, nullptr: none, which is the default). The first
// caller wins; a caller that finds it resolved or being resolved returns at once. Runs inside the game's call: no logging.
inline void StartFromDirectory(const char* directory) noexcept {
  int expected = 0;
  if (!g_startState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
  int flip = 0;
  int source = kByDefault;
  if (directory != nullptr && directory[0] != '\0' && Offered()) {  // not offered: the saved choice is not read, the roll stays Off
    static_assert(512 + sizeof("/RAD/LoneEcho/") + sizeof(kFileName) <= sizeof(g_path), "hand flip path buffer fits the directory");
    std::snprintf(g_path, sizeof(g_path), "%.512s/RAD/LoneEcho/%s", directory, kFileName);
    g_pathReady.store(true, std::memory_order_release);
    std::FILE* const file = std::fopen(g_path, "r");
    if (file != nullptr) {
      char buffer[256];
      const std::size_t size = std::fread(buffer, 1, sizeof(buffer), file);
      std::fclose(file);
      int fromFile = 0;
      const FileStatus status = ParseFile(buffer, size, &fromFile);
      if (status == FileStatus::kValid) {
        flip = fromFile;
        source = kByPlayerMarker;
      } else if (status == FileStatus::kIgnored) {
        g_ignored.store(true, std::memory_order_release);
      }
    }
  }
  void (*const seam)() noexcept = g_startSeam.load(std::memory_order_acquire);
  if (seam != nullptr) seam();
  g_flip.store(flip, std::memory_order_relaxed);
  g_by.store(source, std::memory_order_relaxed);
  g_startSource.store(source, std::memory_order_relaxed);
  g_startFlip.store(flip, std::memory_order_relaxed);
  pthread_mutex_lock(&g_startMutex);
  g_startState.store(2, std::memory_order_release);
  pthread_cond_broadcast(&g_startCond);
  pthread_mutex_unlock(&g_startMutex);
}

// What resolves the start when the tracking handler needs it first (the texture hook registers its directory lookup here).
inline void SetStartProvider(void (*provider)() noexcept) noexcept { g_provider.store(provider, std::memory_order_release); }

// Makes sure the start has been resolved: through the provider, or, with none, to the default. A second caller while another
// thread resolves it returns with the flag still Off.
inline void EnsureStarted() noexcept {
  if (g_startState.load(std::memory_order_acquire) != 0) return;
  void (*const provider)() noexcept = g_provider.load(std::memory_order_acquire);
  if (provider != nullptr) {
    provider();
  } else {
    StartFromDirectory(nullptr);
  }
}

// The first delivery's write into the game's bit, kept for the poller to log once: the bit as the game held it and the bit it was
// set to. Runs inside the game's call: no logging.
inline void RecordCarrier(int before, int after) noexcept {
  g_carrierBefore.store(before != 0 ? 1 : 0, std::memory_order_relaxed);
  g_carrierAfter.store(after != 0 ? 1 : 0, std::memory_order_release);
}

// Resolves the start if nobody has, else waits for the thread that is resolving it (a condition variable, no spin) for at most
// g_startWaitMs. True when the start is resolved. For the settings delivery and the menu change only: the per-frame tracking path
// never calls it.
inline bool AwaitStart() noexcept {
  EnsureStarted();
  if (Started()) return true;
  timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);  // PTHREAD_COND_INITIALIZER waits on the realtime clock
  const long waitMs = g_startWaitMs.load(std::memory_order_relaxed);
  deadline.tv_sec += waitMs / 1000;
  deadline.tv_nsec += (waitMs % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_nsec -= 1000000000L;
    ++deadline.tv_sec;
  }
  pthread_mutex_lock(&g_startMutex);
  g_startWaiters.fetch_add(1, std::memory_order_acq_rel);
  int rc = 0;
  while (g_startState.load(std::memory_order_acquire) != 2 && rc == 0) rc = pthread_cond_timedwait(&g_startCond, &g_startMutex, &deadline);
  g_startWaiters.fetch_sub(1, std::memory_order_acq_rel);
  pthread_mutex_unlock(&g_startMutex);
  if (Started()) return true;
  g_startTimeouts.fetch_add(1, std::memory_order_relaxed);
  return false;
}

// A change in the menu: the flag takes the player's choice at once (the tracking handler reads it on its next call) and the choice
// is queued for the poller to write. The start is resolved first, so it cannot overwrite the choice afterwards.
inline void SetFromMenu(int flip) noexcept {
  if (!AwaitStart()) return;  // a later start would overwrite the choice; the delivery that calls this has awaited it already
  const int value = flip != 0 ? 1 : 0;
  g_flip.store(value, std::memory_order_relaxed);
  g_by.store(kByMenu, std::memory_order_relaxed);
  g_pendingSave.store(value, std::memory_order_release);
}

// ---- the poller's half: the only place this feature logs ----------------------------------------------------------------

// Writes the menu's last choice to the file through a temporary file and a rename, so a reader never sees half of it.
// Returns the number of lines logged.
inline int WritePending() noexcept {
  const int flip = g_pendingSave.exchange(-1, std::memory_order_acq_rel);
  if (flip < 0) return 0;
  const char* const consequence =
      "this choice is not remembered; the previous choice comes back at the next launch";
  if (!g_pathReady.load(std::memory_order_acquire)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "hand_flip_saved",
                        {{"result", "no_directory"}, {"flip", flip}, {"errno", 0}, {"consequence", consequence}});
    return 1;
  }
  char temporary[sizeof(g_path) + 8];
  std::snprintf(temporary, sizeof(temporary), "%s.tmp", g_path);
  char text[64];
  const std::size_t length = FileText(flip, text, sizeof(text));
  int error = 0;
  std::FILE* const file = std::fopen(temporary, "w");
  if (file == nullptr) {
    error = errno;
  } else {
    const bool wrote = std::fwrite(text, 1, length, file) == length;
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !closed) error = errno != 0 ? errno : EIO;
  }
  if (error == 0 && std::rename(temporary, g_path) != 0) error = errno;
  if (error != 0) {
    std::remove(temporary);
    // Kept for the next poll unless the player has chosen again since; the line is written for the first failure of a choice only.
    int none = -1;
    g_pendingSave.compare_exchange_strong(none, flip, std::memory_order_acq_rel);
    if (g_failedFlip.exchange(flip, std::memory_order_acq_rel) == flip) return 0;
    sentinel::LogFields(sentinel::LogLevel::kWarn, "hand_flip_saved",
                        {{"result", "write_failed"},
                         {"flip", flip},
                         {"errno", error},
                         {"consequence", "this choice is not remembered yet; the previous choice comes back at the next launch "
                                         "unless a later attempt (one every poll) writes it"}});
    return 1;
  }
  g_failedFlip.store(-1, std::memory_order_release);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "hand_flip_saved",
                      {{"result", "written"},
                       {"flip", flip},
                       {"errno", 0},
                       {"consequence", "the next launch starts with this choice"}});
  return 1;
}

// One poller pass: the start line once it is resolved, the carrier line once the first delivery has written, then a pending save. Returns the number of lines logged.
inline int Poll() noexcept {
  int lines = 0;
  if (!g_startLogged && g_startState.load(std::memory_order_acquire) == 2) {
    g_startLogged = true;
    sentinel::LogFields(sentinel::LogLevel::kInfo, "hand_flip_start",
                        {{"source", ByName(g_startSource.load(std::memory_order_relaxed))},
                         {"flip", g_startFlip.load(std::memory_order_relaxed)},
                         {"unversioned_marker_ignored", g_ignored.load(std::memory_order_acquire) ? 1 : 0},
                         {"consequence", "this start is not saved; only a change in the menu is remembered for the next launch"}});
    ++lines;
  }
  const int after = g_carrierAfter.load(std::memory_order_acquire);
  if (!g_carrierLogged && after >= 0) {
    g_carrierLogged = true;
    sentinel::LogFields(sentinel::LogLevel::kInfo, "hand_flip_carrier",
                        {{"before", g_carrierBefore.load(std::memory_order_relaxed)},
                         {"after", after},
                         {"consequence", "the game's fullscreen setting bit carries the toggle: 1 is Off (the game's own value), 0 is On"}});
    ++lines;
  }
  if (!g_timeoutLogged && g_startTimeouts.load(std::memory_order_relaxed) != 0) {
    g_timeoutLogged = true;
    sentinel::LogFields(sentinel::LogLevel::kWarn, "hand_flip_start_wait",
                        {{"result", "timed_out"},
                         {"wait_ms", g_startWaitMs.load(std::memory_order_relaxed)},
                         {"consequence", "a settings delivery did not wait longer for the start; the toggle's load waits for the next delivery"}});
    ++lines;
  }
  lines += WritePending();
  return lines;
}

// The texture poller's half: when the row is not offered, says so once (the hand pose poller, which would say it otherwise, is
// not running then). Returns the number of lines logged.
inline int PollRow() noexcept {
  if (g_rowLogged || Offered() || g_startState.load(std::memory_order_acquire) != 2) return 0;
  g_rowLogged = true;
  sentinel::LogFields(sentinel::LogLevel::kWarn, "hand_flip_row",
                      {{"offered", 0},
                       {"reason", "the hand pose hooks or their poller are not installed"},
                       {"consequence", "the Controller Hand Flip row is not in the menu, a saved choice is ignored and the roll stays Off"}});
  return 1;
}

// ---- test support ------------------------------------------------------------------------------------------------------

inline void ResetForTest() noexcept {
  g_flip.store(0);
  g_by.store(kByDefault);
  g_startState.store(0);
  g_startSource.store(kByDefault);
  g_startFlip.store(0);
  g_ignored.store(false);
  g_pendingSave.store(-1);
  g_carrierBefore.store(-1);
  g_carrierAfter.store(-1);
  g_pathReady.store(false);
  g_path[0] = '\0';
  g_provider.store(nullptr);
  g_startLogged = false;
  g_carrierLogged = false;
  g_timeoutLogged = false;
  g_rowLogged = false;
  g_failedFlip.store(-1);
  g_startTimeouts.store(0);
  g_startWaitMs.store(2000);
  g_startSeam.store(nullptr);
  g_offered.store(true);  // a test starts with the row offered; it says otherwise when it tests the other case
}

}  // namespace nevr_quest::hand_flip
