#pragma once

// The session map pin (docs/adr/0047, CDN_DESIGN §6.5, F3): both ends of a session run the same version of a channel map.
// The server writes the mounted map's pin into the session's owner JSON as `nevr_map_pin`; a client whose channel lists
// the session's level compares it with its own copy and leaves on a mismatch, with a notice saying why. The login carries
// the staged and mounted maps as `nevr_maps`. With no map staged or mounted, nothing here changes the game or the login.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nevr_map_session_pin {

// level = rad_hash(id): the owner-JSON name on both platforms (Quest appends _lowspec only at its own load).
struct MapPin {
  std::string id, version, index_sha256, platform, source_sha256, title;
  std::uint64_t level = 0;
};

// rad_hash("<id>@<version>@<first 16 hex of source_sha256>"). Never 0 for a published map (E2 pin_zero).
std::uint64_t PinValue(const MapPin& pin);

// Boot thread, once, from PC5b after BootMount.
struct ChannelView {
  std::vector<std::pair<std::uint64_t, std::string>> listed;     // level -> id, every map listed for this platform
  std::vector<std::pair<std::string, std::string>> local;        // (id, local .kfclvl path) skipped as local_map
  std::vector<std::string> subscribed;
  std::vector<std::pair<std::string, std::string>> not_mounted;  // (id, token)
  std::vector<MapPin> staged;                                    // registered at boot, mount not yet run
};
void SetChannelView(ChannelView view);

// Game thread, once, from PC5b's OnMount: the maps whose set mounted.
void SetMountedPins(std::vector<MapPin> pins);
std::vector<MapPin> MountedPins();  // any thread; a copy taken under a mutex

using FetchRequestFn = void (*)(const std::string& id);
void SetFetchRequest(FetchRequestFn fn);  // default none: no request is made

// Game thread: the owner-JSON "level" (getter +0x60D3D0, default 0) when a lobby exists and it is non-zero, else
// EchoVR::ReadMainLevelSymbol(g_pGame, kPcMainLevelSymbolOffset). Also PC8's main-level source.
std::uint64_t SessionLevel();

// Pure, over the state above (tests drive them with no game).
std::uint64_t ServerPinFor(std::uint64_t level);  // 0 unless `level` is a mounted map's

enum class ClientAction { kNone, kUnpinned, kLocal, kLeave };
struct ClientDecision {
  ClientAction action = ClientAction::kNone;
  std::string id, reason, notice_line;
  bool request_fetch = false;
};
ClientDecision DecideClient(std::uint64_t level, std::uint64_t pin);  // pin 0 = absent; the §6.5 table
constexpr std::size_t kNoticeLineMax = 63;

// Game thread, from PC5b's Tick. Server: sets nevr_map_pin on change only. Client: DecideClient once per
// (lobby, level, pin); a kLeave writes the notice and calls ScheduleNow, retried per tick for 10 s.
void Tick();

// LoginEntriesJson, RegistrationEntries: any thread; the same mutex as MountedPins.
std::string LoginEntriesJson();                  // "[]" when nothing is staged or mounted
std::vector<std::string> RegistrationEntries();  // one JSON object each; for the OUT-OF-REPO field; not sent in v1

}  // namespace nevr_map_session_pin
