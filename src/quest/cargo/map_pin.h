/* PC9's client half on the Quest (CDN_DESIGN Rev 3 §6.5, §10.4 Q2): the session map pin check.
 *
 * The pin is PC9's (runtime/content/map_session_pin.cpp PinValue): rad_hash("<id>@<version>@<source_sha256 first 16>"). It names
 * the map document, not the platform package, so a Quest client with the map's Quest package and a PC server with its PC package
 * agree (E2 refuses a version whose two platforms disagree on source_sha256).
 *
 * The session level is keyed by rad_hash(<id>), the owner-JSON name a PC server publishes, never by rad_hash(<id>_lowspec). The
 * Quest's own main level is the `_lowspec` one (CR15Game::LoadLevelName appends it), so the view lists both names for each map.
 *
 * The Quest has no LOCAL source and is never a server: Decide gives none, map_unpinned or a leave, by PC9's table otherwise.
 * TickWith is PC9's client tick over GameAccess; map_pin_game.cpp is its libr15 half, run once per CR15NetGame::Update by the
 * frame dispatcher, and installed only when the maps service starts (no maps config: nothing is installed).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/map_run.h"

namespace nevr_quest::maps::pin {

struct Pin {
  std::string id, version, source_sha256;
  std::string index_sha256, platform;  // for the login entry only
  std::uint64_t level = 0;             // rad_hash(<id>), as the PC's MountedMap.level
};

std::uint64_t PinValue(const Pin& pin);

// rad_hash(<id>) and rad_hash(<id>_lowspec): the two names one map's level goes by in a Quest session.
std::uint64_t SessionLevelOf(const std::string& id);
std::uint64_t LowspecLevelOf(const std::string& id);

struct View {
  std::vector<std::pair<std::uint64_t, std::string>> listed;     // level -> id, both names of every listed map
  std::vector<std::string> subscribed;
  std::vector<std::pair<std::string, std::string>> not_mounted;  // (id, token)
  std::vector<Pin> staged;                                       // registered at boot, mount not yet run
};

// The view of this run from the boot mount, as PC5b builds PC9's ChannelView on the PC.
View ViewFromBoot(const nevr_maps::BootReport& boot, const std::vector<std::string>& subscribed);

void SetView(View view);
void SetMounted(std::vector<Pin> pins);
std::vector<Pin> Mounted();  // any thread; a copy taken under the mutex

enum class Action { kNone, kUnpinned, kLeave };

struct Decision {
  Action action = Action::kNone;
  std::string id, reason;  // reason: map_unpinned, not_mounted, version or not_subscribed
  bool request_fetch = false;
};

// `level` is the session's level, `pin` the owner JSON's nevr_map_pin (0: absent). PC9's §6.5 table without its LOCAL row.
Decision Decide(std::uint64_t level, std::uint64_t pin);

// PC9's notice line for a leave, at most kNoticeBytes: the longest field is cut first and a `~` marks the cut.
// `local_version` is the version this headset holds of the map ("" when none).
inline constexpr std::size_t kNoticeBytes = 63;
inline constexpr const char* kNoticeTitle = "Map version mismatch";
std::string NoticeFor(const Decision& decision, const std::string& local_version);

// The login's `nevr_maps` entries (PC9 LoginEntriesJson): the mounted maps, then the staged maps not yet mounted.
struct LoginEntry {
  Pin pin;
  bool mounted = false;
};
std::vector<LoginEntry> LoginEntries();

// The game half of the client tick behind an interface, so the state machine runs in a host test.
class GameAccess {
 public:
  virtual ~GameAccess() = default;
  virtual bool Callable() = 0;          // false: this libr15 is not the pinned build; nothing else is called
  virtual std::uint64_t Lobby() = 0;    // the CNSLobby, 0 when there is none
  virtual std::int32_t NetState() = 0;  // CR15NetGame::EState
  virtual std::uint64_t OwnerSymbol(std::uint64_t lobby, const char* key) = 0;  // 0 when absent
  virtual std::uint64_t MainLevel() = 0;                                        // 0 when the game holds no level
  virtual void WriteNotice(const char* title, const char* line) = 0;
  virtual bool ScheduleLeave() = 0;  // CR15NetGame::ScheduleReturnToLobby
  virtual std::uint64_t NowMs() = 0;
  // One `map_skew` line: reason map_unpinned, version, not_subscribed, not_mounted or leave_failed.
  virtual void LogSkew(const char* reason, const std::string& id, const std::string& notice) = 0;
};

inline constexpr const char* kOwnerPinKey = "nevr_map_pin";
inline constexpr const char* kOwnerLevelKey = "level";
inline constexpr std::uint64_t kLeaveRetryMs = 10000;
inline constexpr std::int32_t kFirstSessionState = 6;  // server loading
inline constexpr std::int32_t kLastSessionState = 9;   // in game

// The owner JSON's level when present, else the main level.
std::uint64_t SessionLevelWith(GameAccess& game, std::uint64_t lobby);

// PC9's client tick: in net states 6 to 9 each new (lobby, level, pin) is decided once; a leave writes the notice, asks for
// the fetch when the decision says so, and is retried every tick for kLeaveRetryMs. Game thread only.
void TickWith(GameAccess& game);

// Called with a map id when a leave asks for its fetch (null: none). Game thread.
void SetFetchRequest(void (*request)(const std::string& id));

void ResetForTest();

}  // namespace nevr_quest::maps::pin
