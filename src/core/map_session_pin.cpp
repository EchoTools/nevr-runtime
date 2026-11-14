#include "core/map_session_pin.h"

#include <cstring>
#include <mutex>

#include "core/map_format.h"
#include "nevr/symbol_hash.h"

namespace nevr_map_session_pin {

// The consequence a record states for a pin that is off (the words both platforms write).
constexpr const char* kPinOffConsequence = "a mismatched server is not left";

namespace {

// The source hash prefix the identity uses (ADR 0048 decision 9): 16 hex characters.
constexpr std::size_t kSourcePrefixChars = 16;
// A cut field keeps its text but two bytes shorter, plus one '~' that marks the cut.
constexpr std::size_t kCutBytes = 2;
constexpr char kCutMark = '~';
// A UTF-8 continuation byte is 10xxxxxx; a lead byte of a two-or-more-byte sequence is at least 11000000.
constexpr unsigned char kUtf8ContinuationMask = 0xC0;
constexpr unsigned char kUtf8ContinuationBits = 0x80;
constexpr unsigned char kUtf8LeadMin = 0xC0;

std::mutex g_mutex;
ChannelView g_view;
std::vector<MapPin> g_mounted;
FetchRequestFn g_fetch = nullptr;

// The tick's state: game thread only (TickWith), and ResetForTest.
struct TickState {
  std::uint64_t server_set = 0;  // the pin this server wrote into the lobby's owner JSON; the only value it clears
  bool have_key = false;
  std::uint64_t key_lobby = 0, key_level = 0, key_pin = 0;
  bool local_warned = false;
  bool leave_pending = false;
  std::uint64_t leave_deadline = 0;
  std::string leave_id;
};
TickState g_tick;

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

// Drops a cut-off UTF-8 sequence from the end, so a cut never leaves half a character on the game's error page.
void PopPartialUtf8(std::string& text) {
  while (!text.empty() && (static_cast<unsigned char>(text.back()) & kUtf8ContinuationMask) == kUtf8ContinuationBits)
    text.pop_back();
  if (!text.empty() && static_cast<unsigned char>(text.back()) >= kUtf8LeadMin) text.pop_back();
}

// One piece of a notice line. A field (an id, a version, a token) is cut when the line is too long; the fixed words stay whole.
struct Part {
  std::string text;
  bool field = false;
};

// The notice line, at most kNoticeLineMax bytes: the longest field is cut first, two bytes at a time, and marked with '~'.
std::string FitLine(std::vector<Part> parts) {
  auto total = [&parts] {
    std::size_t n = 0;
    for (const Part& part : parts) n += part.text.size();
    return n;
  };
  while (total() > kNoticeLineMax) {
    Part* longest = nullptr;
    for (Part& part : parts)
      if (part.field && part.text.size() >= kCutBytes && (longest == nullptr || part.text.size() > longest->text.size()))
        longest = &part;
    if (longest == nullptr) break;
    std::string& text = longest->text;
    text.resize(text.size() - kCutBytes);
    PopPartialUtf8(text);
    text += kCutMark;
  }
  std::string line;
  for (const Part& part : parts) line += part.text;
  if (line.size() > kNoticeLineMax) {
    line.resize(kNoticeLineMax);
    PopPartialUtf8(line);
  }
  return line;
}

constexpr const char* kHexDigits = "0123456789abcdef";
constexpr int kHexDigitBits = 4;
constexpr unsigned kHexDigitMask = 0xF;

nlohmann::json EntryJson(const MapPin& pin) {
  return {{"level", HexText(pin.level)},
          {"id", pin.id},
          {"version", pin.version},
          {"index_sha256", pin.index_sha256},
          {"source_sha256", pin.source_sha256},
          {"platform", pin.platform}};
}

void ResetClientKey() {
  g_tick.have_key = false;
  g_tick.local_warned = false;
}

// Asks the game to return to the lobby. A refusal is retried each tick for kLeaveRetryMs, then reported once.
void TryLeave(GameAccess& game, std::uint64_t now) {
  if (game.ScheduleLeave()) {
    PinEvent event;
    event.kind = PinEventKind::kLeaveScheduled;
    event.id = g_tick.leave_id;
    game.Event(event);
    g_tick.leave_pending = false;
    return;
  }
  if (!g_tick.leave_pending) {
    g_tick.leave_pending = true;
    g_tick.leave_deadline = now + kLeaveRetryMs;
  } else if (now >= g_tick.leave_deadline) {
    g_tick.leave_pending = false;
    PinEvent event;
    event.kind = PinEventKind::kLeaveFailed;
    event.id = g_tick.leave_id;
    game.Event(event);
  }
}

void ServerTick(GameAccess& game, std::uint64_t lobby) {
  if (lobby == 0) {
    g_tick.server_set = 0;
    return;
  }
  const std::uint64_t level = SessionLevelWith(game);
  const std::uint64_t desired = ServerPinFor(level);
  const std::uint64_t current = game.OwnerSymbol(lobby, kOwnerPinKey);
  if (current == desired) return;
  PinEvent event;
  event.level = level;
  event.pin = desired;
  if (desired != 0) {
    game.SetOwnerSymbol(lobby, kOwnerPinKey, desired);
    g_tick.server_set = desired;
    event.kind = PinEventKind::kServerSet;
    game.Event(event);
  } else if (g_tick.server_set != 0 && current == g_tick.server_set) {
    game.SetOwnerSymbol(lobby, kOwnerPinKey, 0);
    g_tick.server_set = 0;
    event.kind = PinEventKind::kServerCleared;
    game.Event(event);
  }
}

void ClientTick(GameAccess& game, std::uint64_t lobby, std::uint64_t now) {
  const std::int32_t state = game.NetState();
  // A client in a session: the decision is made again for each new session (a lobby, a level or a pin that changed).
  const bool inSession = state >= kFirstSessionState && state <= kLastSessionState;
  if (lobby == 0 || !inSession) {
    ResetClientKey();
    g_tick.leave_pending = false;
    return;
  }
  if (g_tick.leave_pending) TryLeave(game, now);

  const std::uint64_t level = SessionLevelWith(game);
  const std::uint64_t pin = game.OwnerSymbol(lobby, kOwnerPinKey);
  if (g_tick.have_key && g_tick.key_lobby == lobby && g_tick.key_level == level && g_tick.key_pin == pin) return;
  g_tick.have_key = true;
  g_tick.key_lobby = lobby;
  g_tick.key_level = level;
  g_tick.key_pin = pin;

  const ClientDecision decision = DecideClient(level, pin);
  PinEvent event;
  event.level = level;
  event.pin = pin;
  event.id = decision.id;
  event.reason = decision.reason;
  switch (decision.action) {
    case ClientAction::kNone:
      return;
    case ClientAction::kUnpinned:
      event.kind = PinEventKind::kUnpinned;
      game.Event(event);
      return;
    case ClientAction::kLocal:
      if (g_tick.local_warned) return;
      g_tick.local_warned = true;
      event.kind = PinEventKind::kLocalMap;
      event.local_path = decision.local_path;
      game.Event(event);
      return;
    case ClientAction::kLeave:
      break;
  }

  event.kind = PinEventKind::kSkew;
  event.notice = decision.notice_line;
  event.fetch = decision.request_fetch;
  game.Event(event);
  if (!game.WriteNotice(kNoticeTitle, decision.notice_line.c_str())) {
    PinEvent refused;
    refused.kind = PinEventKind::kNoticeNotWritten;
    refused.id = decision.id;
    game.Event(refused);
  }
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
  const std::string identity = pin.id + "@" + pin.version + "@" + pin.source_sha256.substr(0, kSourcePrefixChars);
  return EchoVR::CSymbol64Hash(identity.c_str());
}

std::string HexText(std::uint64_t value) {
  // "0x" and sixteen hex digits: 2 + 16 bytes.
  constexpr std::size_t kHexDigitsPerU64 = 16;
  constexpr std::size_t kHexTextBytes = 2 + kHexDigitsPerU64;
  std::string text = "0x";
  text.reserve(kHexTextBytes);
  for (std::size_t i = 0; i < kHexDigitsPerU64; ++i) {
    const int shift = static_cast<int>((kHexDigitsPerU64 - 1 - i) * kHexDigitBits);
    text += kHexDigits[(value >> shift) & kHexDigitMask];
  }
  return text;
}

std::string HexBytesText(const std::uint8_t* bytes, std::size_t n) {
  constexpr std::size_t kHexDigitsPerByte = 2;
  std::string text;
  text.reserve(n * kHexDigitsPerByte);
  for (std::size_t i = 0; i < n; ++i) {
    text += kHexDigits[(bytes[i] >> kHexDigitBits) & kHexDigitMask];
    text += kHexDigits[bytes[i] & kHexDigitMask];
  }
  return text;
}

PinFields PinOffFields(const char* reason, const char* name, std::uint64_t va, const std::uint8_t* expected,
                       const std::uint8_t* actual, std::size_t n) {
  PinFields fields;
  fields.push_back({"status", "off"});
  fields.push_back({"reason", reason});
  if (name != nullptr) {
    fields.push_back({"name", name});
    fields.push_back({"va", HexText(va)});
    fields.push_back({"expected", HexBytesText(expected, n)});
    fields.push_back({"actual", HexBytesText(actual, n)});
  }
  fields.push_back({"consequence", kPinOffConsequence});
  return fields;
}

bool SameBytesExact(const std::uint8_t* at, const std::uint8_t* expected, std::size_t n) {
  return std::memcmp(at, expected, n) == 0;
}

bool CheckGameBytes(const std::uint8_t* at, std::uint64_t va, const std::uint8_t* expected, std::size_t n, SameBytesFn same,
                    const char* reason, const char* name, PinFields& refusal) {
  if (same(at, expected, n)) return true;
  refusal = PinOffFields(reason, name, va, expected, at, n);
  return false;
}

std::string FormatFields(const PinFields& fields) {
  std::string line;
  for (const PinField& field : fields) {
    if (!line.empty()) line += ' ';
    const bool quoted = field.second.find(' ') != std::string::npos;
    line += field.first + "=" + (quoted ? "\"" + field.second + "\"" : field.second);
  }
  return line;
}

std::uint64_t SessionLevelOf(const std::string& id) { return EchoVR::CSymbol64Hash(id.c_str()); }

std::uint64_t LowspecLevelOf(const std::string& id) {
  return EchoVR::CSymbol64Hash((id + nevr_maps::kQuestLevelSuffix).c_str());
}

ChannelView ViewFromMount(const nevr_maps::MountReport& report) {
  ChannelView view;
  for (const auto& row : report.listed) {
    view.listed.emplace_back(SessionLevelOf(row.second), row.second);
    view.listed.emplace_back(LowspecLevelOf(row.second), row.second);
  }
  view.not_mounted = report.not_mounted;
  for (const nevr_maps::MountedMap& m : report.staged)
    view.staged.push_back({m.id, m.version, m.index_sha256, m.platform, m.source_sha256, m.title, m.level});
  return view;
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

  if (const std::string* local = SecondOf(g_view.local, id)) {
    decision.action = ClientAction::kLocal;
    decision.reason = "local_map";
    decision.local_path = *local;
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
  decision.reason = "version";
  decision.request_fetch = true;
  const MapPin* have = mounted != nullptr ? mounted : staged;
  // An if, not a ?: over two braced lists: GCC 15 reads the ?: as keeping the list past the call (-Wdangling-pointer).
  if (have != nullptr) {
    decision.notice_line = FitLine({{"Needs "}, {id, true}, {" (not your "}, {have->version, true}, {"); updating for next launch."}});
  } else {
    decision.notice_line = FitLine({{"Needs map "}, {id, true}, {"; updating for your next launch."}});
  }
  return decision;
}

std::uint64_t SessionLevelWith(GameAccess& game) {
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
  {
    // Nothing listed, nothing mounted, nothing to clear and no leave pending: no decision and no write can follow, so the
    // game is not called (the tick runs every frame once it is wired).
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_view.listed.empty() && g_mounted.empty() && g_tick.server_set == 0 && !g_tick.leave_pending) return;
  }
  const std::uint64_t lobby = game.Lobby();
  if (game.IsServer())
    ServerTick(game, lobby);
  else
    ClientTick(game, lobby, game.NowMs());
}

nlohmann::json LoginEntries() {
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
  return entries;
}

std::vector<std::string> RegistrationEntries() {
  std::vector<std::string> entries;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const MapPin& pin : g_mounted) entries.push_back(EntryJson(pin).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
  // A staged map's set is registered at boot, so the server already reports its level (MountedLevelNames).
  for (const MapPin& pin : g_view.staged)
    if (PinById(g_mounted, pin.id) == nullptr)
      entries.push_back(EntryJson(pin).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
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
