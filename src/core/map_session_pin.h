#pragma once

// The session map pin (docs/adr/0048 decision 9): players match only on the same map version. The server writes the pin of
// the map it mounted for the session's level into the session's owner JSON as `nevr_map_pin`; a client whose channel lists the
// session's level compares it with its own copy and leaves on a mismatch, with a notice saying why. The login carries the
// staged and mounted maps as `nevr_maps`, and the registration carries the same entries.
//
// This header and map_session_pin.cpp are the pure part, compiled on both platforms (src/core/maps.cmake): the pin value, the
// view, the decision table, the notice line, the login and registration entries, both level names of a map, and the tick over
// a GameAccess. Each platform keeps only its game half, which reads and writes the game through its own address table and
// formats the events the tick emits:
//   runtime/content/map_session_pin_game.cpp (PC): echovr.exe's owner-JSON calls (addresses.h), the error block, the return
//     to lobby, and the Log() lines. Platform reason: the PC's addresses and its log are the PC's.
//   quest/cargo/map_pin_game.cpp (Quest): libr15's owner-JSON call and return to lobby (pinned_targets.h), the frame
//     dispatcher's tick and the sentinel log. Platform reason: libr15 is patched and proven on the Quest, and the Quest has no Log().

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/map_run.h"

namespace nevr_map_session_pin {

// The owner-JSON names and the notice both platforms use.
inline constexpr const char* kOwnerPinKey = "nevr_map_pin";
inline constexpr const char* kOwnerLevelKey = "level";
inline constexpr const char* kNoticeTitle = "Map version mismatch";
inline constexpr std::size_t kNoticeLineMax = 63;  // the game's error block keeps 63 bytes of a line (WriteErrorBlock)
// A leave the game refuses is retried for this long (the session pin's retry window).
inline constexpr std::uint64_t kLeaveRetryMs = 10000;
// The game states of a client in a session: EchoVR::NetGameState ServerLoading (6) to InGame (9), src/abi/echovr.h:364-367.
// The values are written here and not included: echovr.h includes windows.h, which this pure part does not have on the Quest
// (src/quest/host_tests/groups/shared.cmake compiles it there). Outside them the client decides nothing.
inline constexpr std::int32_t kFirstSessionState = 6;
inline constexpr std::int32_t kLastSessionState = 9;

// One map as the session pin sees it: its id, version and the two hashes that tie the PC and Quest packages of one version.
struct MapPin {
  std::string id, version, index_sha256, platform, source_sha256, title;
  std::uint64_t level = 0;  // rad_hash(id): the owner-JSON name on both platforms (the Quest's own level is <id>_lowspec)
};

// rad_hash("<id>@<version>@<first 16 hex of source_sha256>"). Never 0 for a published map.
std::uint64_t PinValue(const MapPin& pin);

// The two names one map's level goes by in a session: rad_hash(<id>) (the owner-JSON name a PC server publishes) and
// rad_hash(<id> + nevr_maps::kQuestLevelSuffix) (the name the Quest's own main level carries: CR15Game::LoadLevelName appends it).
std::uint64_t SessionLevelOf(const std::string& id);
std::uint64_t LowspecLevelOf(const std::string& id);

// "0x" and sixteen hex digits: how the logs and the login spell a level or a pin. Both platforms format their records with it.
std::string HexText(std::uint64_t value);
// Lowercase hex of `n` bytes, two digits each, no separators: the expected and the actual bytes of a prologue check (logging.md
// Rule 6). Both platforms format their records with it.
std::string HexBytesText(const std::uint8_t* bytes, std::size_t n);

// The fields of a `map_pin status=off` record (logging.md [NEVR.MAPS]), as (key, value) text in the order they are written. With
// `name` null the record gives the reason only (a module or build check); with it, the record also names the function, its VA and
// the expected and the actual bytes (logging.md Rule 6). The consequence is the same words on both platforms.
using PinField = std::pair<std::string, std::string>;
using PinFields = std::vector<PinField>;
PinFields PinOffFields(const char* reason, const char* name, std::uint64_t va, const std::uint8_t* expected,
                       const std::uint8_t* actual, std::size_t n);

// True when the n bytes at `at` agree with `expected` (the caller's rule). A byte rule that accepts a hook's jmp is passed as `same`.
using SameBytesFn = bool (*)(const std::uint8_t* at, const std::uint8_t* expected, std::size_t n);
bool SameBytesExact(const std::uint8_t* at, const std::uint8_t* expected, std::size_t n);
// True when the bytes at `at` (the game function at virtual address `va`) agree under `same`. Otherwise `refusal` receives the
// record's fields for `reason` and `name`, and the function returns false.
bool CheckGameBytes(const std::uint8_t* at, std::uint64_t va, const std::uint8_t* expected, std::size_t n, SameBytesFn same,
                    const char* reason, const char* name, PinFields& refusal);

// The fields as the text one log line carries: `key=value` separated by spaces, a value with a space in double quotes.
std::string FormatFields(const PinFields& fields);

// What this run's channel holds, as the session pin reads it. Replaced whole by SetChannelView.
struct ChannelView {
  std::vector<std::pair<std::uint64_t, std::string>> listed;     // level -> id, both names of every map listed for this platform
  std::vector<std::pair<std::string, std::string>> local;        // (id, local map path): a local copy overrides the channel's (PC)
  std::vector<std::pair<std::string, std::string>> not_mounted;  // (id, token) for a map that did not mount
  std::vector<MapPin> staged;                                    // registered at boot, mount not yet run
};

// The view of this run from a mount report: both level names of every listed map, the staged maps, the not-mounted tokens.
ChannelView ViewFromMount(const nevr_maps::MountReport& report);

// Replace, under a mutex: each call replaces what the last one set (the boot thread and the game thread both call them).
void SetChannelView(ChannelView view);
void SetMountedPins(std::vector<MapPin> pins);
std::vector<MapPin> MountedPins();  // any thread; a copy taken under the same mutex

using FetchRequestFn = void (*)(const std::string& id);
void SetFetchRequest(FetchRequestFn fn);  // default none: no request is made

// The server's pin for a session at `level`: the pin of the mounted map with that level, or 0 when none is mounted for it.
std::uint64_t ServerPinFor(std::uint64_t level);

enum class ClientAction { kNone, kUnpinned, kLocal, kLeave };

struct ClientDecision {
  ClientAction action = ClientAction::kNone;
  std::string id, reason, notice_line, local_path;
  bool request_fetch = false;
};

// The decision table (ADR 0048 decision 9), over the view and the mounted and staged pins. `pin` is the owner JSON's
// nevr_map_pin, 0 when absent:
//   level 0, or a level no listed map has                      -> none
//   pin 0                                                       -> unpinned (the server states no version)
//   a mounted or staged pin of the same map equals `pin`        -> none
//   a local copy of the map (PC only)                           -> local (stays; no leave, no fetch)
//   the map has a not-mounted token                             -> leave (reason not_mounted); fetch only for stock_changed
//   otherwise                                                   -> leave (reason version); fetch
ClientDecision DecideClient(std::uint64_t level, std::uint64_t pin);

// What the platform does with a decision and the tick (see GameAccess). Each is one record the platform writes.
enum class PinEventKind { kServerSet, kServerCleared, kUnpinned, kLocalMap, kSkew, kLeaveScheduled, kLeaveFailed, kNoticeNotWritten };

struct PinEvent {
  PinEventKind kind = PinEventKind::kSkew;
  std::uint64_t level = 0, pin = 0;
  std::string id, reason, notice, local_path;
  bool fetch = false;
};

// The game half behind an interface, so the tick runs in a test with no game. Each platform implements it once.
class GameAccess {
 public:
  virtual ~GameAccess() = default;
  virtual bool Callable() = 0;          // false: the game's code is not the build this pin expects; nothing else is called
  virtual bool IsServer() = 0;          // a dedicated server writes the pin; a client decides it (the Quest is never a server)
  virtual std::uint64_t Lobby() = 0;    // the lobby, 0 when there is none
  virtual std::int32_t NetState() = 0;  // the net game's state
  virtual std::uint64_t OwnerSymbol(std::uint64_t lobby, const char* key) = 0;  // 0 when absent
  virtual void SetOwnerSymbol(std::uint64_t lobby, const char* key, std::uint64_t value) = 0;  // server only
  virtual std::uint64_t MainLevel() = 0;                                        // 0 when the game holds no level
  virtual bool WriteNotice(const char* title, const char* line) = 0;
  virtual bool ScheduleLeave() = 0;  // the return to lobby; false when it could not be scheduled now
  virtual std::uint64_t NowMs() = 0;
  virtual void Event(const PinEvent& event) = 0;  // every record the pin writes; the platform formats it
};

// The level a session is at: the owner JSON's `level` when a lobby exists and it is non-zero, else the main level.
std::uint64_t SessionLevelWith(GameAccess& game);

// The tick, on the game thread: a server sets nevr_map_pin on change only; a client decides once per (lobby, level, pin) in a
// session, and a leave writes the notice and is retried for kLeaveRetryMs. With no listed map and no mounted pin, it makes no
// game call at all.
void TickWith(GameAccess& game);

// The login's nevr_maps: the mounted maps, then the staged maps not yet mounted (each with "mounted"). An empty array when none.
nlohmann::json LoginEntries();

// The registration's entries (one JSON object each, without "mounted"): the mounted maps, then the staged maps not yet mounted.
// KNOWN-WRONG #TBD: nothing calls this, and the registration message has no field to carry it: GameServerRegistrationMessage
// (gen/cpp/gameservice/v1/gameservice.pb.h) has installed_levels, guilds and features only. The field is a schema change in the
// BSR proto, then `just proto` (ADR 0048 decision 9).
std::vector<std::string> RegistrationEntries();

// Forgets the view, the mounted pins, the fetch request and the tick state. For tests.
void ResetForTest();

}  // namespace nevr_map_session_pin
