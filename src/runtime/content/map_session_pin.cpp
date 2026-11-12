#include "runtime/content/map_session_pin.h"

#include <cstdio>
#include <mutex>

#include <nlohmann/json.hpp>

#include "core/logging.h"
#include "nevr/symbol_hash.h"
#include "runtime/content/map_session_pin_seam.h"

namespace nevr_map_session_pin {

namespace {

std::mutex g_mutex;
ChannelView g_view;
std::vector<MapPin> g_mounted;
FetchRequestFn g_fetch = nullptr;

// Game thread only (Tick).
struct TickState {
  std::uint64_t server_set = 0;  // the pin nevr wrote into this lobby's owner JSON; the only value it clears
  bool have_key = false;
  std::uint64_t key_lobby = 0, key_level = 0, key_pin = 0;
  bool local_warned = false;
  bool leave_pending = false;
  std::uint64_t leave_deadline = 0;
  std::string leave_id;
};
TickState g_tick;

std::string Hex(std::uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
  return text;
}

const MapPin* PinById(const std::vector<MapPin>& pins, const std::string& id) {
  for (const MapPin& pin : pins)
    if (pin.id == id) return &pin;
  return nullptr;
}

const std::string* SecondOf(const std::vector<std::pair<std::string, std::string>>& rows, const std::string& id) {
  for (const auto& row : rows)
    if (row.first == id) return &row.second;
  return nullptr;
}

bool Contains(const std::vector<std::string>& names, const std::string& id) {
  for (const std::string& name : names)
    if (name == id) return true;
  return false;
}

// Drops a cut-off UTF-8 sequence from the end, so a cut never leaves half a character on the game's error page.
void PopPartialUtf8(std::string& text) {
  while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
  if (!text.empty() && static_cast<unsigned char>(text.back()) >= 0xC0) text.pop_back();
}

// WriteErrorBlock keeps 63 bytes of a line. The fields (an id, a version, a token) are cut, longest first, and marked
// with '~'; the fixed words stay whole.
struct Part {
  std::string text;
  bool field = false;
};

std::string FitLine(std::vector<Part> parts) {
  auto total = [&parts] {
    std::size_t n = 0;
    for (const Part& part : parts) n += part.text.size();
    return n;
  };
  while (total() > kNoticeLineMax) {
    Part* longest = nullptr;
    for (Part& part : parts)
      if (part.field && part.text.size() >= 2 && (longest == nullptr || part.text.size() > longest->text.size()))
        longest = &part;
    if (longest == nullptr) break;
    std::string& text = longest->text;
    text.resize(text.size() - 2);
    PopPartialUtf8(text);
    text += '~';
  }
  std::string line;
  for (const Part& part : parts) line += part.text;
  if (line.size() > kNoticeLineMax) {
    line.resize(kNoticeLineMax);
    PopPartialUtf8(line);
  }
  return line;
}

nlohmann::json EntryJson(const MapPin& pin) {
  return {{"level", Hex(pin.level)},          {"id", pin.id},
          {"version", pin.version},           {"index_sha256", pin.index_sha256},
          {"source_sha256", pin.source_sha256}, {"platform", pin.platform}};
}

// `replace`: ids and versions come from a channel; a login or a log must not fail over them.
std::string Dump(const nlohmann::json& value) { return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace); }

void ResetClientKey() {
  g_tick.have_key = false;
  g_tick.local_warned = false;
}

void TryLeave(GameAccess& game, std::uint64_t now) {
  if (game.ScheduleLeave()) {
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_skew leave scheduled id=%s", g_tick.leave_id.c_str());
    g_tick.leave_pending = false;
    return;
  }
  if (!g_tick.leave_pending) {
    g_tick.leave_pending = true;
    g_tick.leave_deadline = now + kLeaveRetryMs;
  } else if (now >= g_tick.leave_deadline) {
    g_tick.leave_pending = false;
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MAPS] map_skew reason=leave_failed id=%s: the return to lobby could not be scheduled for %llu ms; this "
        "client stays in a session whose map version differs",
        g_tick.leave_id.c_str(), static_cast<unsigned long long>(kLeaveRetryMs));
  }
}

void ServerTick(GameAccess& game, std::uint64_t lobby) {
  if (lobby == 0) {
    g_tick.server_set = 0;
    return;
  }
  const std::uint64_t level = SessionLevelOf(game);
  const std::uint64_t desired = ServerPinFor(level);
  const std::uint64_t current = game.OwnerSymbol(lobby, kOwnerPinKey);
  if (current == desired) return;
  if (desired != 0) {
    game.SetOwnerSymbol(lobby, kOwnerPinKey, desired);
    g_tick.server_set = desired;
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] session pin set level=%s pin=%s", Hex(level).c_str(), Hex(desired).c_str());
  } else if (g_tick.server_set != 0 && current == g_tick.server_set) {
    game.SetOwnerSymbol(lobby, kOwnerPinKey, 0);
    g_tick.server_set = 0;
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] session pin cleared level=%s", Hex(level).c_str());
  }
}

void ClientTick(GameAccess& game, std::uint64_t lobby, std::uint64_t now) {
  const std::int32_t state = game.NetState();
  // ServerLoading (6) to InGame (9): the client is in a session. Leaving one resets the decision, so a later session
  // with the same level and pin is decided again.
  const bool inSession = state >= 6 && state <= 9;
  if (lobby == 0 || !inSession) {
    ResetClientKey();
    g_tick.leave_pending = false;
    return;
  }
  if (g_tick.leave_pending) TryLeave(game, now);

  const std::uint64_t level = SessionLevelOf(game);
  const std::uint64_t pin = game.OwnerSymbol(lobby, kOwnerPinKey);
  if (g_tick.have_key && g_tick.key_lobby == lobby && g_tick.key_level == level && g_tick.key_pin == pin) return;
  g_tick.have_key = true;
  g_tick.key_lobby = lobby;
  g_tick.key_level = level;
  g_tick.key_pin = pin;

  const ClientDecision decision = DecideClient(level, pin);
  switch (decision.action) {
    case ClientAction::kNone:
      return;
    case ClientAction::kUnpinned:
      Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_unpinned id=%s level=%s: the server states no map version",
          decision.id.c_str(), Hex(level).c_str());
      return;
    case ClientAction::kLocal: {
      if (g_tick.local_warned) return;
      g_tick.local_warned = true;
      std::string path;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (const std::string* local = SecondOf(g_view.local, decision.id)) path = *local;
      }
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.MAPS] map_skew reason=local_map id=%s: local %s overrides channel map %s; the server runs another "
          "version (pin %s). This match may not match the server. Remove the file to use the channel copy.",
          decision.id.c_str(), path.c_str(), decision.id.c_str(), Hex(pin).c_str());
      return;
    }
    case ClientAction::kLeave:
      break;
  }

  Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_skew reason=%s id=%s level=%s pin=%s fetch=%d notice=\"%s\"",
      decision.reason.c_str(), decision.id.c_str(), Hex(level).c_str(), Hex(pin).c_str(), decision.request_fetch ? 1 : 0,
      decision.notice_line.c_str());
  if (!game.WriteNotice(kNoticeTitle, decision.notice_line.c_str()))
    Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_skew notice not written id=%s", decision.id.c_str());
  if (decision.request_fetch) {
    FetchRequestFn fetch = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      fetch = g_fetch;
    }
    if (fetch != nullptr) fetch(decision.id);
  }
  // A leave already retrying stands, whatever the level does meanwhile.
  if (g_tick.leave_pending) return;
  g_tick.leave_id = decision.id;
  TryLeave(game, now);
}

}  // namespace

std::uint64_t PinValue(const MapPin& pin) {
  const std::string identity = pin.id + "@" + pin.version + "@" + pin.source_sha256.substr(0, 16);
  return EchoVR::CSymbol64Hash(identity.c_str());
}

void SetChannelView(ChannelView view) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_view = std::move(view);
}

void SetMountedPins(std::vector<MapPin> pins) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_mounted = std::move(pins);
}

std::vector<MapPin> MountedPins() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_mounted;
}

void SetFetchRequest(FetchRequestFn fn) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_fetch = fn;
}

std::uint64_t ServerPinFor(std::uint64_t level) {
  if (level == 0) return 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const MapPin& pin : g_mounted)
    if (pin.level == level) return PinValue(pin);
  return 0;
}

ClientDecision DecideClient(std::uint64_t level, std::uint64_t pin) {
  ClientDecision decision;
  if (level == 0) return decision;
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::string* listedId = nullptr;
  for (const auto& row : g_view.listed)
    if (row.first == level) listedId = &row.second;
  if (listedId == nullptr) return decision;
  const std::string& id = *listedId;
  decision.id = id;
  if (pin == 0) {
    decision.action = ClientAction::kUnpinned;
    decision.reason = "map_unpinned";
    return decision;
  }
  // A staged map has not mounted yet (the login and the lobby can come before the first resource request) but will.
  const MapPin* mounted = PinById(g_mounted, id);
  const MapPin* staged = PinById(g_view.staged, id);
  if ((mounted != nullptr && PinValue(*mounted) == pin) || (staged != nullptr && PinValue(*staged) == pin)) return decision;

  if (SecondOf(g_view.local, id) != nullptr) {
    decision.action = ClientAction::kLocal;
    decision.reason = "local_map";
    return decision;
  }
  decision.action = ClientAction::kLeave;
  // The pin is a hash, so the client cannot name the server's version; it names its own when it has one.
  if (const std::string* token = SecondOf(g_view.not_mounted, id)) {
    decision.reason = "not_mounted";
    decision.notice_line = FitLine({{"Map "}, {id, true}, {" did not load: "}, {*token, true}, {". See the nevr log."}});
    decision.request_fetch = *token == "stock_changed";
    return decision;
  }
  if (Contains(g_view.subscribed, id)) {
    decision.reason = "version";
    decision.request_fetch = true;
    const MapPin* have = mounted != nullptr ? mounted : staged;
    // An if, not a ?: over two braced lists: GCC 15 reads the ?: as keeping the list past the call (-Wdangling-pointer).
    if (have != nullptr) {
      decision.notice_line =
          FitLine({{"Needs "}, {id, true}, {" (not your "}, {have->version, true}, {"); updating for next launch."}});
    } else {
      decision.notice_line = FitLine({{"Needs map "}, {id, true}, {"; updating for your next launch."}});
    }
    return decision;
  }
  decision.reason = "not_subscribed";
  decision.notice_line = FitLine({{"Needs map "}, {id, true}, {". Add it to maps.subscribed."}});
  return decision;
}

std::uint64_t SessionLevelOf(GameAccess& game) {
  if (game.Callable()) {
    const std::uint64_t lobby = game.Lobby();
    if (lobby != 0) {
      const std::uint64_t level = game.OwnerSymbol(lobby, kOwnerLevelKey);
      if (level != 0) return level;
    }
  }
  return game.MainLevel();
}

void TickWith(GameAccess& game) {
  if (!game.Callable()) return;
  const std::uint64_t lobby = game.Lobby();
  if (game.IsServer())
    ServerTick(game, lobby);
  else
    ClientTick(game, lobby, game.NowMs());
}

std::string LoginEntriesJson() {
  nlohmann::json entries = nlohmann::json::array();
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const MapPin& pin : g_mounted) {
    nlohmann::json entry = EntryJson(pin);
    entry["mounted"] = true;
    entries.push_back(std::move(entry));
  }
  for (const MapPin& pin : g_view.staged) {
    if (PinById(g_mounted, pin.id) != nullptr) continue;
    nlohmann::json entry = EntryJson(pin);
    entry["mounted"] = false;
    entries.push_back(std::move(entry));
  }
  return Dump(entries);
}

std::vector<std::string> RegistrationEntries() {
  std::vector<std::string> entries;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const MapPin& pin : g_mounted) entries.push_back(Dump(EntryJson(pin)));
  // A staged map's set is registered at boot, so the server already reports its level (MountedLevelNames).
  for (const MapPin& pin : g_view.staged)
    if (PinById(g_mounted, pin.id) == nullptr) entries.push_back(Dump(EntryJson(pin)));
  return entries;
}

void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_view = ChannelView();
  g_mounted.clear();
  g_fetch = nullptr;
  g_tick = TickState();
}

}  // namespace nevr_map_session_pin
