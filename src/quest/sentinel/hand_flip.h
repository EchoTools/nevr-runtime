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
 * The menu row. texture_menu.cpp adds "Controller Hand Flip" (STRING ARRAY, Off / On) under Texture Resolution on the GRAPHICS
 * page. The row is bound to the setting `graphics|fullscreen`, which the game stores in SR15GfxSettings flags bit 0
 * (kCarrierBit): a Quest has no fullscreen mode, and the consumer audit (docs/design/2026-11-02-quest-hand-flip-toggle.md,
 * "The interim in the sentinel") found nothing in libr15 that reads the bit except a log line in ApplyRuntimeGfxSettings, the
 * JSON round trip of the settings file, and the script expression CR15GfxSettingsExpression, which copies the struct and exposes
 * the bit as a boolean output. texture_menu_hook.cpp's HandleApply reads the bit on every delivery: the first delivery is
 * set to the stored choice (or the default), a later delivery with another bit is the player's choice (SetFromMenu).
 *
 * The persisted choice. Its own file beside the texture marker, {"version":1,"hand_flip":0|1} (kFileName), written only by a
 * menu change (a derived or default value is never saved). A file without the version, with another version, or with a value
 * other than 0 or 1 is ignored and the ignore is logged (hand_flip_start unversioned_marker_ignored=1). It is read once, by
 * StartFromDirectory, before the first tracking call can use the flag: the texture hook's start resolver calls it on the game's
 * first eye-size query, and the tracking handler calls EnsureStarted() when that has not happened.
 *
 * Nothing on the game's call path logs. The poller thread of hand_pose_log_hook.cpp calls Poll(), which writes
 *   {"event":"hand_flip_start","source":"player_marker|default","flip":0|1,"unversioned_marker_ignored":0|1,"consequence":...}
 * once, and, after a change in the menu,
 *   {"event":"hand_flip_saved","result":"written|write_failed|no_directory","flip":0|1,"errno":N,"consequence":...}
 * The existing hand_flip_device line carries `flip` (the flag when it is written) and `by` (default, player_marker or menu).
 * There is no per-frame logging.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h. State is constant-initialised
 * inline variables: no static initialiser runs at load (tools/check_quest_static_init.sh).
 */
#pragma once

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
inline constexpr const char* kMenuLabels[kMenuValueCount] = {"Off", "On"};

// The persisted choice: a file of our own beside the texture marker (RAD/LoneEcho/ under the game's local app data directory).
inline constexpr char kFileName[] = "nevr_hand_flip.json";
inline constexpr int kFileVersion = 1;

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
inline std::atomic<int> g_startState{0};         // 0 not started, 1 being resolved, 2 resolved
inline std::atomic<int> g_startSource{kByDefault};  // kByPlayerMarker when the file gave the start, else kByDefault
inline std::atomic<int> g_startFlip{0};          // the flag the start resolved to (the menu may have changed g_flip since)
inline std::atomic<bool> g_ignored{false};       // the file was there and was not ours to read
inline std::atomic<int> g_pendingSave{-1};       // the menu's last choice the poller has not yet written (-1: none)
inline std::atomic<bool> g_pathReady{false};
inline char g_path[704];                         // the file's path, written once before g_pathReady
inline std::atomic<void (*)() noexcept> g_provider{nullptr};  // what resolves the start from the game's directory
inline bool g_startLogged = false;               // the poller pass only (the poll mutex of hand_pose_log_hook.cpp)

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
  if (directory != nullptr && directory[0] != '\0') {
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
  g_flip.store(flip, std::memory_order_relaxed);
  g_by.store(source, std::memory_order_relaxed);
  g_startSource.store(source, std::memory_order_relaxed);
  g_startFlip.store(flip, std::memory_order_relaxed);
  g_startState.store(2, std::memory_order_release);
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

// A change in the menu: the flag takes the player's choice at once (the tracking handler reads it on its next call) and the choice
// is queued for the poller to write. The start is resolved first, so it cannot overwrite the choice afterwards.
inline void SetFromMenu(int flip) noexcept {
  EnsureStarted();  // a later start would overwrite the choice
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
  const char* const consequence = "this choice is not remembered at the next launch";
  if (!g_pathReady.load(std::memory_order_acquire)) {
    sentinel::LogFields(sentinel::LogLevel::kError, "hand_flip_saved",
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
    sentinel::LogFields(sentinel::LogLevel::kError, "hand_flip_saved",
                        {{"result", "write_failed"}, {"flip", flip}, {"errno", error}, {"consequence", consequence}});
    return 1;
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, "hand_flip_saved",
                      {{"result", "written"},
                       {"flip", flip},
                       {"errno", 0},
                       {"consequence", "the next launch starts with this choice"}});
  return 1;
}

// One poller pass: the start line once it is resolved, then a pending save. Returns the number of lines logged.
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
  lines += WritePending();
  return lines;
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
  g_pathReady.store(false);
  g_path[0] = '\0';
  g_provider.store(nullptr);
  g_startLogged = false;
}

}  // namespace nevr_quest::hand_flip
