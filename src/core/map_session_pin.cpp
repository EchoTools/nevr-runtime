#include "core/map_session_pin.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>

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

std::mutex& g_mutex() {
  static std::mutex instance;  // function-local: no load-time initializer in the sentinel (tools/check_quest_static_init.sh)
  return instance;
}
ChannelView& g_view() {
  static ChannelView instance;  // function-local: no load-time initializer in the sentinel (tools/check_quest_static_init.sh)
  return instance;
}
std::vector<MapPin>& g_mounted() {
  static std::vector<MapPin> instance;  // function-local: no load-time initializer in the sentinel (tools/check_quest_static_init.sh)
  return instance;
}
FetchRequestFn g_fetch = nullptr;

// The cargo inputs (SetCargoServerPin, SetCargoDecide): written under g_mutex(), copied under it by TickWith.
struct CargoInputs {
  std::optional<std::uint64_t> server_pin;
  std::shared_ptr<const CargoDecideFn> decide;
};
CargoInputs& g_cargo() {
  static CargoInputs instance;  // function-local: no load-time initializer in the sentinel (tools/check_quest_static_init.sh)
  return instance;
}

// The tick's state: game thread only (TickWith), and ResetForTest.
struct TickState {
  std::uint64_t server_set = 0;  // the pin this server wrote into the lobby's owner JSON; the only value it clears
  bool have_key = false;
  std::uint64_t key_lobby = 0, key_level = 0, key_pin = 0, key_cargo = 0;
  bool local_warned = false;
  bool leave_pending = false;
  std::uint64_t leave_deadline = 0;
  std::string leave_id;
};
TickState& g_tick() {
  static TickState instance;  // function-local: no load-time initializer in the sentinel (tools/check_quest_static_init.sh)
  return instance;
}

// The client's join hold: armed by the level-load detour (HoldLoadBegin, game thread), ended by the tick, told by the worker
// (NoteFetchFailed, NoteHoldProgress) and the mount observer (NoteMapsMounted); all under g_mutex.
struct JoinHoldState {
  bool armed = false;
  bool cached = false;  // the map is on disk and only needs its mount: the notices say Loading, not Downloading
  std::string id, failed;  // failed: the consequence of a failed fetch of `id`
  std::uint64_t lobby = 0, level = 0, pin = 0, armed_ms = 0;
  nevr_maps::HoldPlan plan;
  bool spent = false;  // a hold for this (lobby, level, pin) ended in a leave: the game's retries of the load stay skipped
  std::uint64_t spent_lobby = 0, spent_level = 0, spent_pin = 0;
};
JoinHoldState& g_hold() {
  static JoinHoldState instance;  // function-local: no load-time initializer in the sentinel (tools/check_quest_static_init.sh)
  return instance;
}
std::vector<std::string>& g_mounted_ids() {
  static std::vector<std::string> instance;  // the map ids whose registry mount completed since their hold armed
  return instance;
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

// Drops a cut-off UTF-8 sequence from the end, so a cut never leaves half a character on the game's error page.
void PopPartialUtf8(std::string& text) {
  while (!text.empty() && (static_cast<unsigned char>(text.back()) & kUtf8ContinuationMask) == kUtf8ContinuationBits)
    text.pop_back();
  if (!text.empty() && static_cast<unsigned char>(text.back()) >= kUtf8LeadMin) text.pop_back();
}

}  // namespace

// The notice line, at most kNoticeLineMax bytes: the longest field is cut first, two bytes at a time, and marked with '~'.
// `Part` is declared in map_session_pin.h, so the cargo pin's lines (core/cargo_session_pin.h) are cut by this same rule.
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

namespace {

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
  g_tick().have_key = false;
  g_tick().local_warned = false;
}

// Asks the game to return to the lobby. A refusal is retried each tick for kLeaveRetryMs, then reported once.
void TryLeave(GameAccess& game, std::uint64_t now) {
  if (game.ScheduleLeave()) {
    PinEvent event;
    event.kind = PinEventKind::kLeaveScheduled;
    event.id = g_tick().leave_id;
    game.Event(event);
    g_tick().leave_pending = false;
    return;
  }
  if (!g_tick().leave_pending) {
    g_tick().leave_pending = true;
    g_tick().leave_deadline = now + kLeaveRetryMs;
  } else if (now >= g_tick().leave_deadline) {
    g_tick().leave_pending = false;
    PinEvent event;
    event.kind = PinEventKind::kLeaveFailed;
    event.id = g_tick().leave_id;
    game.Event(event);
  }
}

// The map pin's server write: nevr_map_pin is set on change and cleared only when this server set it.
void ServerMapTick(GameAccess& game, std::uint64_t lobby) {
  const std::uint64_t level = SessionLevelWith(game);
  const std::uint64_t desired = ServerPinFor(level);
  const std::uint64_t current = game.OwnerSymbol(lobby, kOwnerPinKey);
  if (current == desired) return;
  PinEvent event;
  event.level = level;
  event.pin = desired;
  if (desired != 0) {
    game.SetOwnerSymbol(lobby, kOwnerPinKey, desired);
    g_tick().server_set = desired;
    event.kind = PinEventKind::kServerSet;
    game.Event(event);
  } else if (g_tick().server_set != 0 && current == g_tick().server_set) {
    game.SetOwnerSymbol(lobby, kOwnerPinKey, 0);
    g_tick().server_set = 0;
    event.kind = PinEventKind::kServerCleared;
    game.Event(event);
  }
}

// The cargo pin's server write (PLAN-v7 5.5 "Server"): B on change, whatever the map pin does, so a server with no map still states its set.
void ServerCargoTick(GameAccess& game, std::uint64_t lobby, std::uint64_t pin) {
  if (game.OwnerSymbol(lobby, kOwnerCargoPinKey) == pin) return;
  game.SetOwnerSymbol(lobby, kOwnerCargoPinKey, pin);
  PinEvent event;
  event.kind = PinEventKind::kCargoServerSet;
  event.pin = pin;
  game.Event(event);
}

void ServerTick(GameAccess& game, std::uint64_t lobby, const std::optional<std::uint64_t>& cargo_pin) {
  if (lobby == 0) {
    g_tick().server_set = 0;
    return;
  }
  ServerMapTick(game, lobby);
  if (cargo_pin) ServerCargoTick(game, lobby, *cargo_pin);
}

void AskFetch(const std::string& id) {
  FetchRequestFn fetch = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex());
    fetch = g_fetch;
  }
  if (fetch != nullptr) fetch(id);
}

// Writes the notice and the leave once for both inputs: the map's notice when the map leaves, else the cargo's (PLAN-v7 5.5).
void ClientLeaveOnce(GameAccess& game, std::uint64_t now, std::uint64_t level, std::uint64_t pin, std::uint64_t cargo_pin,
                     const ClientDecision& decision, const CargoOutcome& cargo, bool map_leaves) {
  std::string title;
  std::string line;
  if (map_leaves) {
    PinEvent event;
    event.kind = PinEventKind::kSkew;
    event.level = level;
    event.pin = pin;
    event.id = decision.id;
    event.reason = decision.reason;
    event.notice = decision.notice_line;
    event.fetch = decision.request_fetch;
    game.Event(event);
    title = kNoticeTitle;
    line = decision.notice_line;
  } else {
    PinEvent event;
    event.kind = PinEventKind::kCargoSkew;
    event.level = level;
    event.pin = cargo_pin;
    event.id = cargo.id;
    event.reason = cargo.reason;
    event.notice = cargo.line;
    event.local = cargo.local;
    event.manifest_known = cargo.manifest_known;
    event.manifest = cargo.manifest;
    game.Event(event);
    title = cargo.title;
    line = cargo.line;
  }
  if (!game.WriteNotice(title.c_str(), line.c_str())) {
    PinEvent refused;
    refused.kind = PinEventKind::kNoticeNotWritten;
    refused.id = map_leaves ? decision.id : cargo.id;
    game.Event(refused);
  }
  if (map_leaves && decision.request_fetch) AskFetch(decision.id);
  // A leave already retrying stands, whatever the level does meanwhile.
  if (g_tick().leave_pending) return;
  g_tick().leave_id = map_leaves ? decision.id : cargo.id;
  TryLeave(game, now);
}

// The two session keys the hold compares: the owner JSON's level and the map pin.
bool HoldSpentFor(std::uint64_t lobby, std::uint64_t level, std::uint64_t pin) {
  std::lock_guard<std::mutex> lock(g_mutex());
  const JoinHoldState& hold = g_hold();
  return hold.spent && hold.spent_lobby == lobby && hold.spent_level == level && hold.spent_pin == pin;
}

// Ends a hold in the one leave: the notice and the schedule are ClientLeaveOnce's, the reason is the hold's. The hold's key is kept
// so the game's retries of the load stay skipped while the leave runs.
void HoldLeave(GameAccess& game, std::uint64_t now, const JoinHoldState& hold, const ClientDecision& decision) {
  {
    std::lock_guard<std::mutex> lock(g_mutex());
    g_hold().armed = false;
    g_hold().spent = true;
    g_hold().spent_lobby = hold.lobby;
    g_hold().spent_level = hold.level;
    g_hold().spent_pin = hold.pin;
  }
  ClientLeaveOnce(game, now, hold.level, hold.pin, 0, decision, CargoOutcome(), true);
}

ClientDecision HoldLeaveDecision(const std::string& id, const char* reason, std::string notice) {
  ClientDecision decision;
  decision.action = ClientAction::kLeave;
  decision.id = id;
  decision.reason = reason;
  decision.notice_line = std::move(notice);
  return decision;
}

// The hold's step, each tick and in any game state. True while a load is held for this session (the pin's own decision then waits).
bool HoldStep(GameAccess& game, std::uint64_t lobby, std::uint64_t now) {
  JoinHoldState hold;
  bool mounted = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex());
    if (!g_hold().armed) return false;
    hold = g_hold();
    for (const std::string& id : g_mounted_ids()) mounted = mounted || id == hold.id;
  }
  const std::uint64_t level = lobby != 0 ? game.OwnerSymbol(lobby, kOwnerLevelKey) : 0;
  const std::uint64_t pin = lobby != 0 ? game.OwnerSymbol(lobby, kOwnerPinKey) : 0;
  PinEvent event;
  event.level = hold.level;
  event.pin = hold.pin;
  event.id = hold.id;
  event.waited_ms = now >= hold.armed_ms ? now - hold.armed_ms : 0;
  if (lobby != hold.lobby || level != hold.level || pin != hold.pin) {
    {
      std::lock_guard<std::mutex> lock(g_mutex());
      g_hold().armed = false;
    }
    event.kind = PinEventKind::kHoldDropped;
    event.reason = "session_changed";
    game.Event(event);
    return false;
  }
  const ClientDecision decision = DecideClient(hold.level, hold.pin);
  // An unpinned session's cached map still waiting for its mount (not_asked) keeps the hold; once mounted the decision is a plain stay.
  const bool unpinnedWaiting = decision.action == ClientAction::kUnpinned && decision.holdable;
  if ((mounted && decision.action == ClientAction::kNone) ||
      (decision.action != ClientAction::kNone && decision.action != ClientAction::kLeave && !unpinnedWaiting)) {
    {
      std::lock_guard<std::mutex> lock(g_mutex());
      g_hold().armed = false;
    }
    event.kind = PinEventKind::kHoldReleased;
    game.Event(event);
    if (!game.ReplayLoad()) {
      HoldLeave(game, now, hold,
                HoldLeaveDecision(hold.id, "replay_failed", FitLine({{"Map "}, {hold.id, true}, {" could not be loaded. Join again."}})));
    }
    return false;
  }
  if (!hold.failed.empty()) {
    HoldLeave(game, now, hold,
              HoldLeaveDecision(hold.id, "fetch_failed",
                                FitLine({{"Map "}, {hold.id, true}, {hold.cached ? " could not be loaded. See the nevr log." : " could not be downloaded. See the nevr log."}})));
    return false;
  }
  if (now >= hold.armed_ms + nevr_maps::MapHoldTimeoutMs(hold.plan)) {
    HoldLeave(game, now, hold,
              HoldLeaveDecision(hold.id, "hold_timeout",
                                FitLine({{hold.cached ? "Loading " : "Downloading "}, {hold.id, true}, {" took too long; join again later."}})));
    return false;
  }
  // Another version mounted (or the map cannot be had): no wait cures it, so the one leave takes it with its own notice.
  if (decision.action == ClientAction::kLeave && !decision.holdable) {
    HoldLeave(game, now, hold, decision);
    return false;
  }
  return true;
}

void ClientTick(GameAccess& game, std::uint64_t lobby, std::uint64_t now, const CargoDecideFn* cargo, bool held) {
  const std::int32_t state = game.NetState();
  // A client in a session: the decision is made again for each new session (a lobby, a level or a pin that changed).
  const bool inSession = state >= kFirstSessionState && state <= kLastSessionState;
  if (lobby == 0 || !inSession) {
    ResetClientKey();
    g_tick().leave_pending = false;
    return;
  }
  if (g_tick().leave_pending) TryLeave(game, now);

  const std::uint64_t level = SessionLevelWith(game);
  const std::uint64_t pin = game.OwnerSymbol(lobby, kOwnerPinKey);
  // A load held for this session leaves, or loads, by the hold alone; one the hold already took to a leave is not decided again.
  if (held || HoldSpentFor(lobby, level, pin)) return;
  // The cargo pin is read each frame in a session only when a client decides it (F8): a change the server writes after the first
  // read changes the key, so the decision is made again.
  const std::uint64_t cargo_pin = cargo != nullptr ? game.OwnerSymbol(lobby, kOwnerCargoPinKey) : 0;
  if (g_tick().have_key && g_tick().key_lobby == lobby && g_tick().key_level == level && g_tick().key_pin == pin &&
      g_tick().key_cargo == cargo_pin)
    return;
  g_tick().have_key = true;
  g_tick().key_lobby = lobby;
  g_tick().key_level = level;
  g_tick().key_pin = pin;
  g_tick().key_cargo = cargo_pin;

  const ClientDecision decision = DecideClient(level, pin);
  CargoOutcome outcome;
  if (cargo != nullptr) outcome = (*cargo)(level, cargo_pin);
  const bool map_leaves = decision.action == ClientAction::kLeave;
  if (cargo != nullptr) {
    PinEvent record;
    record.kind = PinEventKind::kSessionDecision;
    record.level = level;
    record.pin = cargo_pin;
    record.reason = decision.reason.empty() ? "none" : decision.reason;
    record.cargo_reason = outcome.reason.empty() ? "none" : outcome.reason;
    record.leave = map_leaves ? "map" : outcome.leave ? "cargo" : "none";
    game.Event(record);
  }

  PinEvent event;
  event.level = level;
  event.pin = pin;
  event.id = decision.id;
  event.reason = decision.reason;
  switch (decision.action) {
    case ClientAction::kNone:
      break;
    case ClientAction::kUnpinned:
      event.kind = PinEventKind::kUnpinned;
      game.Event(event);
      // A load the hold could not take (a platform that cannot hold it) still asks for the cached map's mount: it plays from the next load.
      if (decision.request_fetch) AskFetch(decision.id);
      break;
    case ClientAction::kLocal:
      if (g_tick().local_warned) break;
      g_tick().local_warned = true;
      event.kind = PinEventKind::kLocalMap;
      event.local_path = decision.local_path;
      game.Event(event);
      break;
    case ClientAction::kLeave:
      break;
  }
  if (!map_leaves && !outcome.leave) return;
  ClientLeaveOnce(game, now, level, pin, cargo_pin, decision, outcome, map_leaves);
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
  std::lock_guard<std::mutex> lock(g_mutex());
  g_view() = std::move(view);
}

void SetMountedPins(std::vector<MapPin> pins) {
  std::lock_guard<std::mutex> lock(g_mutex());
  g_mounted() = std::move(pins);
}

std::vector<MapPin> MountedPins() {
  std::lock_guard<std::mutex> lock(g_mutex());
  return g_mounted();
}

bool CachedMapAwaitsMount(const std::string& id) {
  std::lock_guard<std::mutex> lock(g_mutex());
  const std::string* token = SecondOf(g_view().not_mounted, id);
  return token != nullptr && *token == nevr_maps::kNotAskedToken;
}

void SetFetchRequest(FetchRequestFn fn) {
  std::lock_guard<std::mutex> lock(g_mutex());
  g_fetch = fn;
}

std::uint64_t ServerPinFor(std::uint64_t level) {
  if (level == 0) return 0;
  std::lock_guard<std::mutex> lock(g_mutex());
  for (const MapPin& pin : g_mounted())
    if (pin.level == level) return PinValue(pin);
  return 0;
}

ClientDecision DecideClient(std::uint64_t level, std::uint64_t pin) {
  ClientDecision decision;
  if (level == 0) return decision;
  std::lock_guard<std::mutex> lock(g_mutex());
  const std::string* listedId = nullptr;
  for (const auto& row : g_view().listed)
    if (row.first == level) listedId = &row.second;
  if (listedId == nullptr) return decision;
  const std::string& id = *listedId;
  decision.id = id;
  if (pin == 0) {
    decision.action = ClientAction::kUnpinned;
    decision.reason = "map_unpinned";
    // The server states no version, so the client never leaves for one; but a session whose level is a map this client lists and holds
    // on disk (not_asked) runs that map, and nothing mounts a cached map at boot: the session asks for its mount, and the load waits for
    // it as it does for a pinned one (HoldLoadBegin), so the cached map is played as it was before the mount moved to the request.
    const std::string* unpinnedToken = SecondOf(g_view().not_mounted, id);
    if (unpinnedToken != nullptr && *unpinnedToken == nevr_maps::kNotAskedToken) {
      decision.request_fetch = true;
      decision.holdable = true;
      decision.cached = true;
      decision.notice_line = FitLine({{"Loading "}, {id, true}, {"; join again when it is ready."}});
    }
    return decision;
  }
  // A staged map has not mounted yet (the login and the lobby can come before the first resource request) but will.
  const MapPin* mounted = PinById(g_mounted(), id);
  const MapPin* staged = PinById(g_view().staged, id);
  if ((mounted != nullptr && PinValue(*mounted) == pin) || (staged != nullptr && PinValue(*staged) == pin)) return decision;

  if (const std::string* local = SecondOf(g_view().local, id)) {
    decision.action = ClientAction::kLocal;
    decision.reason = "local_map";
    decision.local_path = *local;
    return decision;
  }
  decision.action = ClientAction::kLeave;
  // The pin is a hash, so the client cannot name the server's version; it names its own when it has one.
  if (const std::string* token = SecondOf(g_view().not_mounted, id)) {
    decision.reason = "not_mounted";
    // Not held, or held at a version the channel no longer lists: the request downloads the listed version, which mounts during this
    // run (ADR 0048 decision 16). A stale copy is never played in its place.
    if (*token == nevr_maps::kNotCachedToken || *token == nevr_maps::kStaleToken) {
      decision.request_fetch = true;
      decision.holdable = true;
      decision.notice_line = FitLine({{"Downloading "}, {id, true}, {"; join again when it is ready."}});
      return decision;
    }
    // A cached map at the listed version that no session has asked for (every cached map of an on-demand boot): the request checks the
    // manifest and mounts it in this run, so nothing downloads and the join waits only for the mount.
    if (*token == nevr_maps::kNotAskedToken) {
      decision.request_fetch = true;
      decision.holdable = true;
      decision.cached = true;
      decision.notice_line = FitLine({{"Loading "}, {id, true}, {"; join again when it is ready."}});
      return decision;
    }
    // A map this run already mounts at another version: the stored one applies at the next start, and the mounted one stays until then.
    // The internal token is never shown to the player.
    if (*token == nevr_maps::kMountedOtherVersionToken) {
      decision.notice_line = FitLine({{"Map "}, {id, true}, {": the update "}, {kAppliesAtNextStartClause}, {"."}});
      return decision;
    }
    decision.notice_line = FitLine({{"Map "}, {id, true}, {" did not load: "}, {*token, true}, {". See the nevr log."}});
    decision.request_fetch = *token == "stock_changed";
    return decision;
  }
  decision.reason = "version";
  decision.request_fetch = true;
  decision.holdable = mounted == nullptr && staged == nullptr;
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

void SetCargoServerPin(std::uint64_t pin) {
  std::lock_guard<std::mutex> lock(g_mutex());
  g_cargo().server_pin = pin;
}

void SetCargoDecide(CargoDecideFn decide) {
  std::lock_guard<std::mutex> lock(g_mutex());
  g_cargo().decide = decide ? std::make_shared<const CargoDecideFn>(std::move(decide)) : nullptr;
}

void TickWith(GameAccess& game) {
  // The cargo inputs are copied under the mutex: the decision is a shared pointer, so a frame copies no function.
  std::optional<std::uint64_t> cargo_pin;
  std::shared_ptr<const CargoDecideFn> cargo;
  {
    // Nothing listed, nothing mounted, nothing to clear, no leave pending and no cargo input: no decision and no write can follow,
    // so the game is not called at all, not even to prove its own bytes (Callable reads them and logs a mismatch). The tick runs
    // every frame once it is wired, so a run with no maps and no cargo input does nothing here.
    std::lock_guard<std::mutex> lock(g_mutex());
    cargo_pin = g_cargo().server_pin;
    cargo = g_cargo().decide;
    if (g_view().listed.empty() && g_mounted().empty() && g_tick().server_set == 0 && !g_tick().leave_pending && !cargo_pin &&
        !cargo && !g_hold().armed)
      return;
  }
  if (!game.Callable()) return;
  const std::uint64_t lobby = game.Lobby();
  if (game.IsServer())
    ServerTick(game, lobby, cargo_pin);
  else {
    const std::uint64_t now = game.NowMs();
    const bool held = HoldStep(game, lobby, now);
    ClientTick(game, lobby, now, cargo.get(), held);
  }
}

bool HoldLoadBegin(GameAccess& game) {
  if (!game.Callable() || game.IsServer() || !game.CanHoldLoad()) return false;
  const std::uint64_t lobby = game.Lobby();
  if (lobby == 0) return false;
  const std::uint64_t level = game.OwnerSymbol(lobby, kOwnerLevelKey);
  const std::uint64_t pin = game.OwnerSymbol(lobby, kOwnerPinKey);
  if (level == 0) return false;
  {
    std::lock_guard<std::mutex> lock(g_mutex());
    JoinHoldState& hold = g_hold();
    // The game calls the load again each frame it stays in state 6: a hold that stands, or ended in a leave, keeps it skipped.
    if (hold.armed && hold.lobby == lobby && hold.level == level && hold.pin == pin) return true;
    if (hold.spent && hold.spent_lobby == lobby && hold.spent_level == level && hold.spent_pin == pin) return true;
    hold.spent = false;
  }
  const ClientDecision decision = DecideClient(level, pin);
  const bool unpinnedCached = decision.action == ClientAction::kUnpinned && decision.holdable;
  if ((decision.action != ClientAction::kLeave && !unpinnedCached) || !decision.holdable) return false;
  const std::uint64_t now = game.NowMs();
  {
    std::lock_guard<std::mutex> lock(g_mutex());
    JoinHoldState& hold = g_hold();
    hold = JoinHoldState();
    hold.armed = true;
    hold.id = decision.id;
    hold.cached = decision.cached;
    hold.lobby = lobby;
    hold.level = level;
    hold.pin = pin;
    hold.armed_ms = now;
    std::vector<std::string>& mounted = g_mounted_ids();
    mounted.erase(std::remove(mounted.begin(), mounted.end(), decision.id), mounted.end());  // only a mount after the arm counts
  }
  PinEvent event;
  event.kind = PinEventKind::kHoldArmed;
  event.level = level;
  event.pin = pin;
  event.id = decision.id;
  event.fetch = true;
  event.timeout_ms = nevr_maps::MapHoldTimeoutMs(nevr_maps::HoldPlan());
  event.notice = FitLine({{decision.cached ? "Loading " : "Downloading "}, {decision.id, true}, {"; the join continues when it is ready."}});
  game.Event(event);
  AskFetch(decision.id);
  return true;
}

void NoteMapsMounted(const std::vector<std::string>& ids) {
  std::lock_guard<std::mutex> lock(g_mutex());
  for (const std::string& id : ids) g_mounted_ids().push_back(id);
}

void NoteFetchFailed(const std::string& id, const std::string& consequence) {
  std::lock_guard<std::mutex> lock(g_mutex());
  if (g_hold().armed && g_hold().id == id) g_hold().failed = consequence.empty() ? std::string("failed") : consequence;
}

void NoteHoldProgress(const nevr_maps::HoldPlan& plan) {
  std::lock_guard<std::mutex> lock(g_mutex());
  if (g_hold().armed) g_hold().plan = plan;
}

nlohmann::json LoginEntries() {
  nlohmann::json entries = nlohmann::json::array();
  std::lock_guard<std::mutex> lock(g_mutex());
  for (const MapPin& pin : g_mounted()) {
    nlohmann::json entry = EntryJson(pin);
    entry["mounted"] = true;
    entries.push_back(std::move(entry));
  }
  for (const MapPin& pin : g_view().staged) {
    if (PinById(g_mounted(), pin.id) != nullptr) continue;
    nlohmann::json entry = EntryJson(pin);
    entry["mounted"] = false;
    entries.push_back(std::move(entry));
  }
  return entries;
}

std::vector<std::string> RegistrationEntries() {
  std::vector<std::string> entries;
  std::lock_guard<std::mutex> lock(g_mutex());
  for (const MapPin& pin : g_mounted()) entries.push_back(EntryJson(pin).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
  // A staged map's set is registered at boot, so the server already reports its level (MountedLevelNames).
  for (const MapPin& pin : g_view().staged)
    if (PinById(g_mounted(), pin.id) == nullptr)
      entries.push_back(EntryJson(pin).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
  return entries;
}

void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_mutex());
  g_view() = ChannelView();
  g_mounted().clear();
  g_fetch = nullptr;
  g_cargo() = CargoInputs();
  g_tick() = TickState();
  g_hold() = JoinHoldState();
  g_mounted_ids().clear();
}

}  // namespace nevr_map_session_pin
