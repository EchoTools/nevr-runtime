#pragma once
// #906, docs/adr/0017-invite-codes-for-the-create-game-wizard.md: the client half of the invite codes for the game's
// CREATE GAME / JOIN VIA CODE wizard, as pure logic. What the wizard's click metric asks for, which request is sent
// and which is not, the [NEVR.INVITE] line each step writes, and what the service's answer means.
//
// No game, no socket, no hook and no clock in here: invite_wizard_hook.cpp feeds it a metric line, invite_worker.h posts
// the request, and invite_answer_text.h / invite_answer_apply.cpp write the answer back into the wizard (ADR 0017, "Order
// of work"). src/runtime/tests/test_invite_wizard.cpp pins everything below on the host.
//
// The metric field names follow the shape echovr-modkit (rad-links, MIT, by Dualgame) reports for the final PC build.
// No packet has been captured, so they are an input assumption (ADR 0017, "Not shown").
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace nevr_invite {

// A code is A-Z and 2-9 without the look-alikes 0 O 1 I L: players read it aloud and type it on a controller (ADR
// 0017, D1).
inline constexpr std::string_view kCodeAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
// The wizard's text field is a 64-byte container with a terminator, so no more than 63 symbols can be typed. The
// field's own limit is level data and has not been measured (ADR 0017, D1): this is the ceiling the container allows,
// not the length the field accepts.
inline constexpr std::size_t kCodeMaxChars = 63;
// A requested expiry is clamped to 7 days (ADR 0017, D1). The service applies its own maximum as well.
inline constexpr std::int64_t kExpiryMaxSeconds = 7 * 24 * 60 * 60;
// A metric line longer than this is not looked at: the wizard's lines are a few hundred bytes.
inline constexpr std::size_t kMetricMaxBytes = 4096;

// The bytes as two lower-case hex digits each, in order, with no separator: how a hook's Warning writes the prologue it
// expected and the bytes it found (docs/standards/logging.md, Rule 6).
inline std::string HexBytes(const std::uint8_t* bytes, std::size_t length) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; bytes != nullptr && i < length; ++i) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 0x0f]);
  }
  return out;
}

// ---- the wizard's click metric ----

enum class Kind { NotWizard, Create, Join };

struct Metric {
  Kind kind = Kind::NotWizard;
  std::string region;        // Create: Server_region
  std::string expires_text;  // Create: Expires_after, such as "3 HOURS"
  std::string join_code;     // Join: join_code as typed; "" when absent, not text, or the game's "<invalid>" mark
};

namespace detail {

inline const nlohmann::json* StringField(const nlohmann::json& object, const char* key) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_string()) return nullptr;
  return &*it;
}

inline std::string TextOr(const nlohmann::json& object, const char* key) {
  const nlohmann::json* field = StringField(object, key);
  return field == nullptr ? std::string() : field->get<std::string>();
}

// A click's own fields (the typed code, the create page's settings) are what the game puts in the metric's `event_detail`
// object; the panel and event names stay at the top. The top level wins when a key is in both.
inline std::string DetailTextOr(const nlohmann::json& object, const char* key) {
  if (const nlohmann::json* top = StringField(object, key)) return top->get<std::string>();
  const auto detail = object.find("event_detail");
  if (detail == object.end() || !detail->is_object()) return std::string();
  return TextOr(*detail, key);
}

inline bool IsEvent(const nlohmann::json& object, const char* panel_name, const char* event_name) {
  const nlohmann::json* panel = StringField(object, "panel_name");
  const nlohmann::json* event = StringField(object, "event_name");
  return panel != nullptr && event != nullptr && panel->get<std::string>() == panel_name &&
         event->get<std::string>() == event_name;
}

}  // namespace detail

// A cheap test run before any JSON parse: the submit sees every remote log line, and only a wizard line names this panel.
inline bool MayBeWizardMetric(std::string_view line) { return line.find("deep_linking") != std::string_view::npos; }

inline Metric ParseMetric(std::string_view line) {
  Metric metric;
  if (line.empty() || line.size() > kMetricMaxBytes) return metric;
  const nlohmann::json doc = nlohmann::json::parse(line.begin(), line.end(), nullptr, false);
  if (!doc.is_object()) return metric;
  if (detail::TextOr(doc, "panel_type") != "deep_linking") return metric;

  if (detail::IsEvent(doc, "create_game_review_settings", "create_invite")) {
    metric.kind = Kind::Create;
    metric.region = detail::DetailTextOr(doc, "Server_region");
    metric.expires_text = detail::DetailTextOr(doc, "Expires_after");
  } else if (detail::IsEvent(doc, "join_via_code", "join")) {
    metric.kind = Kind::Join;
    metric.join_code = detail::DetailTextOr(doc, "join_code");
    if (metric.join_code == "<invalid>") metric.join_code.clear();
  }
  return metric;
}

// What a wizard metric line holds, without its values: each top-level key with its kind and, for text, its length ("s5"; "invalid"
// for the game's "<invalid>" mark). A join click that carries no usable code is logged with this, so the next reader can see which
// key the game really sends the typed code under without a code, a name or a token reaching the log (docs/standards/logging.md, Rule 4).
inline std::string MetricShape(std::string_view line) {
  if (line.empty() || line.size() > kMetricMaxBytes) return "unreadable";
  const nlohmann::json doc = nlohmann::json::parse(line.begin(), line.end(), nullptr, false);
  if (!doc.is_object()) return "unreadable";
  std::string out;
  for (auto it = doc.begin(); it != doc.end(); ++it) {
    if (!out.empty()) out += ',';
    out += it.key() + '=';
    if (it->is_string()) {
      const std::string text = it->get<std::string>();
      out += text == "<invalid>" ? std::string("invalid") : "s" + std::to_string(text.size());
    } else if (it->is_number()) {
      out += "number";
    } else if (it->is_boolean()) {
      out += "bool";
    } else if (it->is_null()) {
      out += "null";
    } else if (it->is_object()) {
      out += "{" + MetricShape(it->dump()) + "}";  // one more level: a nested object's own keys and kinds
    } else {
      out += "array";
    }
  }
  return out;
}

// The share page's JOIN button (#1731). Its page script (`8c3244ce60942e17.dll`) sends one deep_linking click metric and
// nothing else: panel `create_game_summary`, event `join` (the script's strings at 0x180008438 and 0x180008470). The game has no
// function that joins on it, so a client that reads the click starts the join of the match it just created.
inline bool IsShareJoin(std::string_view line) {
  if (line.empty() || line.size() > kMetricMaxBytes) return false;
  const nlohmann::json doc = nlohmann::json::parse(line.begin(), line.end(), nullptr, false);
  return doc.is_object() && detail::TextOr(doc, "panel_type") == "deep_linking" &&
         detail::IsEvent(doc, "create_game_summary", "join");
}

inline std::string ShareJoinClickLine() {
  return "[NEVR.INVITE] wizard click seen event=join panel=create_game_summary";
}

// ---- the code ----

enum class CodeCheck { Ok, Empty, TooLong, Malformed };

struct CheckedCode {
  CodeCheck check = CodeCheck::Empty;
  std::string code;         // Ok only: upper case, no separators
  std::size_t length = 0;   // symbols left after the separators are dropped, whatever the verdict
};

// Spaces and hyphens are separators and are dropped; letters are upper-cased; every other byte must be in
// kCodeAlphabet. A symbol outside the alphabet wins over a code that is also too long.
inline CheckedCode CheckCode(std::string_view typed) {
  CheckedCode out;
  std::string code;
  for (const char raw : typed) {
    if (raw == ' ' || raw == '-') continue;
    const char symbol = (raw >= 'a' && raw <= 'z') ? static_cast<char>(raw - 'a' + 'A') : raw;
    code.push_back(symbol);
  }
  out.length = code.size();
  if (code.empty()) {
    out.check = CodeCheck::Empty;
    return out;
  }
  for (const char symbol : code) {
    if (kCodeAlphabet.find(symbol) == std::string_view::npos) {
      out.check = CodeCheck::Malformed;
      return out;
    }
  }
  if (code.size() > kCodeMaxChars) {
    out.check = CodeCheck::TooLong;
    return out;
  }
  out.check = CodeCheck::Ok;
  out.code = std::move(code);
  return out;
}

// ---- the expiry ----

// "3 HOURS" -> 10800. A value over the maximum is the maximum; text that is not "<whole number> <unit>" (a unit is
// MINUTE, HOUR or DAY, singular or plural, any case) is 0, which asks for nothing so the service's default applies.
inline std::int64_t ExpirySeconds(std::string_view text) {
  std::size_t at = 0;
  while (at < text.size() && text[at] == ' ') ++at;
  std::int64_t value = 0;
  while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
    if (value < 1000000000) value = value * 10 + (text[at] - '0');  // far past any clamp; cannot overflow
    ++at;
  }
  while (at < text.size() && text[at] == ' ') ++at;
  std::string unit;
  while (at < text.size() && text[at] != ' ') {
    const char c = text[at];
    unit.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c);
    ++at;
  }
  while (at < text.size() && text[at] == ' ') ++at;
  if (at != text.size()) return 0;

  std::int64_t unit_seconds = 0;
  if (unit == "MINUTE" || unit == "MINUTES") {
    unit_seconds = 60;
  } else if (unit == "HOUR" || unit == "HOURS") {
    unit_seconds = 60 * 60;
  } else if (unit == "DAY" || unit == "DAYS") {
    unit_seconds = 24 * 60 * 60;
  } else {
    return 0;
  }
  const std::int64_t seconds = value * unit_seconds;
  return seconds > kExpiryMaxSeconds ? kExpiryMaxSeconds : seconds;
}

// ---- the plan: which request is sent ----

// What the CREATE GAME cards chose. The metric carries only the review page's own fields, so the choices come from
// the component events the cards send (ADR 0017, "Client").
struct Choices {
  std::string experience;  // "arena", "combat", "lobby"
  std::string mode;
  int map = 0;
};

enum class Op { Create, Resolve };
enum class Skip { None, Empty, Malformed, TooLong, Incomplete, NoLevel, NotConfigured, NoToken, Busy, Stopped };
// The first five are the plan's own (NoLevel: the chosen map card has no level name); the last four are the worker's
// (invite_worker.h): the feature is off, no token is held, the queue is full, the worker has stopped.

struct Request {
  std::string rpc;
  std::string body;
};

struct Plan {
  Op op = Op::Create;
  std::optional<Request> request;
  Skip skip = Skip::None;
  std::size_t code_len = 0;   // Resolve: symbols in the typed code
  std::int64_t expires_s = 0; // Create: seconds asked for; 0 leaves it to the service
  std::string code;           // Resolve: the normalised code, only when a request is sent; never logged
};

// ---- the map page: which level a card is ----

// The levels of the two combat map pages (page 39 of mnu_master), as CSymbol64 of the level name. The game's own page script
// (a38aa87473ea079a) builds one card slot per handler with a payload label and a capture label: slot 1 FISSION / DYSON, slot 2
// SURGE / COMBUSTION, and SURGE is the level `gauss` (the label table of accde4944e61e68f maps the short symbol gauss to
// SURGE). The names are the generated symbol cache's (log/symcache_data.cpp), which is where a symbol becomes a name.
inline constexpr std::uint64_t kLevelFission = 0xdf5ca7b7dfa383d4ULL;     // mpl_combat_fission
inline constexpr std::uint64_t kLevelGauss = 0x43e2da7a0c623a19ULL;       // mpl_combat_gauss (the card says SURGE)
inline constexpr std::uint64_t kLevelDyson = 0x43e2da7914642604ULL;       // mpl_combat_dyson
inline constexpr std::uint64_t kLevelCombustion = 0x42670f2bed45703cULL;  // mpl_combat_combustion

/// The level symbol a map card leads to, 0 when the choices name no map card: a mode with no map page, or a card the page does
/// not have. Payload: card 1 FISSION, card 2 SURGE. Capture point: card 1 DYSON, card 2 COMBUSTION.
inline std::uint64_t MapLevelSymbol(const Choices& choices) {
  if (choices.experience != "combat") return 0;
  if (choices.mode == "payload") return choices.map == 1 ? kLevelFission : choices.map == 2 ? kLevelGauss : 0;
  if (choices.mode == "capture") return choices.map == 1 ? kLevelDyson : choices.map == 2 ? kLevelCombustion : 0;
  return 0;
}

/// A level symbol's name, nullptr when unknown. The runtime passes the symbol cache's lookup; with none, no level has a name.
using LevelNamer = const char* (*)(std::uint64_t);

/// The region the service reads. The review page's `Server_region` is display text ("US WEST"), not a code, and no generic
/// region code exists: the service picks a server by the player's stored pings when none is sent. So none is sent, until
/// the row carries a code the service itself offered (#1192).
inline std::string RegionCode(const Metric& metric) {
  (void)metric;
  return std::string();
}

/// `level_name` resolves a map card's symbol to the level name the service reads in `map` (a string: a number makes it refuse
/// the request as `invalid`). A mode with no map page sends "", which the service reads as any level of the mode; a map card
/// whose symbol has no name is not sent (Skip::NoLevel), never sent as any level.
inline Plan PlanRequest(const Metric& metric, const Choices& choices, LevelNamer level_name = nullptr) {
  Plan plan;
  if (metric.kind == Kind::Join) {
    plan.op = Op::Resolve;
    const CheckedCode checked = CheckCode(metric.join_code);
    plan.code_len = checked.length;
    switch (checked.check) {
      case CodeCheck::Empty:
        plan.skip = Skip::Empty;
        return plan;
      case CodeCheck::Malformed:
        plan.skip = Skip::Malformed;
        return plan;
      case CodeCheck::TooLong:
        plan.skip = Skip::TooLong;
        return plan;
      case CodeCheck::Ok:
        break;
    }
    plan.code = checked.code;
    const nlohmann::json body = {{"code", checked.code}};
    plan.request = Request{"invite/resolve", body.dump()};
    return plan;
  }
  if (metric.kind == Kind::Create) {
    plan.op = Op::Create;
    // A combat payload or capture-point game is played on one of two maps, chosen on the map page; without it there is
    // no level to ask for.
    const bool needsMap = choices.experience == "combat" && (choices.mode == "payload" || choices.mode == "capture");
    if (choices.experience.empty() || (choices.experience != "lobby" && choices.mode.empty()) ||
        (needsMap && choices.map == 0)) {
      plan.skip = Skip::Incomplete;
      return plan;
    }
    std::string map;
    if (needsMap) {
      const std::uint64_t symbol = MapLevelSymbol(choices);
      const char* name = symbol != 0 && level_name != nullptr ? level_name(symbol) : nullptr;
      if (name == nullptr || *name == '\0') {
        plan.skip = Skip::NoLevel;
        return plan;
      }
      map = name;
    }
    plan.expires_s = ExpirySeconds(metric.expires_text);
    const nlohmann::json body = {{"experience", choices.experience},
                                 {"mode", choices.mode},
                                 {"map", map},
                                 {"region", RegionCode(metric)},
                                 {"expires_s", plan.expires_s}};
    plan.request = Request{"invite/create", body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)};
  }
  return plan;
}

// ---- the log lines (docs/standards/logging.md, tag [NEVR.INVITE]) ----

// A short stand-in for a code in a log line. FNV-1a over the salt, a NUL and the code. The caller picks a salt that
// differs from run to run, so a fingerprint in one run's log cannot be looked up against another's; 32 bits of an
// unsalted hash of so small a code space would give the code away.
inline std::string CodeFingerprint(std::string_view code, std::string_view salt) {
  std::uint32_t hash = 0x811c9dc5u;
  const auto mix = [&hash](char c) {
    hash ^= static_cast<std::uint8_t>(c);
    hash *= 0x01000193u;
  };
  for (const char c : salt) mix(c);
  mix('\0');
  for (const char c : code) mix(c);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(8, '0');
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHex[hash & 0xfu];
    hash >>= 4;
  }
  return out;
}

inline std::string ClickLine(Kind kind) {
  switch (kind) {
    case Kind::Create:
      return "[NEVR.INVITE] wizard click seen event=create_invite panel=deep_linking";
    case Kind::Join:
      return "[NEVR.INVITE] wizard click seen event=join_via_code panel=deep_linking";
    case Kind::NotWizard:
      break;
  }
  return "";
}

inline std::string NotSentLine(Skip skip, std::size_t code_len) {
  const char* reason = nullptr;
  switch (skip) {
    case Skip::Empty:
      reason = "empty";
      break;
    case Skip::Malformed:
      reason = "malformed";
      break;
    case Skip::TooLong:
      reason = "too_long";
      break;
    case Skip::Incomplete:
      reason = "incomplete";
      break;
    case Skip::NoLevel:
      reason = "no_level";
      break;
    case Skip::NotConfigured:
      reason = "not_configured";
      break;
    case Skip::NoToken:
      reason = "no_token";
      break;
    case Skip::Busy:
      reason = "busy";
      break;
    case Skip::Stopped:
      reason = "stopped";
      break;
    case Skip::None:
      return "";
  }
  return std::string("[NEVR.INVITE] request not sent reason=") + reason + " code_len=" + std::to_string(code_len);
}

inline std::string SentLine(const Plan& plan, std::string_view fingerprint) {
  if (!plan.request.has_value()) return "";
  if (plan.op == Op::Create) {
    return "[NEVR.INVITE] request sent op=create expires_s=" + std::to_string(plan.expires_s);
  }
  return "[NEVR.INVITE] request sent op=resolve code_len=" + std::to_string(plan.code_len) +
         " code_fp=" + std::string(fingerprint);
}

// ---- the answer ----

// NoServer is the service's `unavailable` token (the match could not be allocated); Unavailable is the client's own: no answer.
enum class Result { Ok, Invalid, Expired, Closed, Denied, Limit, Rate, Unavailable, ServerError, NoServer };

// The service's RPC error carries one exact token in its message (ADR 0017, "Service"); nothing else in the body is
// interpreted or logged. Without a token the HTTP status decides; status 0 is no answer at all.
namespace detail {

inline std::optional<Result> TokenResult(std::string_view body) {
  const nlohmann::json doc = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
  if (!doc.is_object()) return std::nullopt;
  const nlohmann::json* message = StringField(doc, "message");
  if (message == nullptr) return std::nullopt;
  const std::string token = message->get<std::string>();
  if (token == "invalid") return Result::Invalid;
  if (token == "expired") return Result::Expired;
  if (token == "closed") return Result::Closed;
  if (token == "denied") return Result::Denied;
  if (token == "limit") return Result::Limit;
  if (token == "rate") return Result::Rate;
  if (token == "unavailable") return Result::NoServer;
  return std::nullopt;
}

}  // namespace detail

inline Result ResultFromAnswer(int http_status, std::string_view body) {
  if (http_status == 200) return Result::Ok;
  if (http_status <= 0) return Result::Unavailable;
  if (const std::optional<Result> named = detail::TokenResult(body)) return *named;
  if (http_status == 404) return Result::Invalid;
  if (http_status == 403) return Result::Denied;
  if (http_status == 429) return Result::Rate;
  return Result::ServerError;
}

inline constexpr std::size_t kRefusalMessageMax = 96;

/// The message of an error body, printable ASCII only (anything else becomes '?') and at most kRefusalMessageMax bytes.
/// For the scenario endpoint, which tells a missing route from a failed allocation; the log and the menu never carry it.
inline std::string RefusalMessage(std::string_view body) {
  const nlohmann::json doc = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
  if (!doc.is_object()) return std::string();
  const nlohmann::json* message = detail::StringField(doc, "message");
  if (message == nullptr) return std::string();
  std::string out;
  for (const char raw : message->get<std::string>()) {
    if (out.size() >= kRefusalMessageMax) break;
    const auto u = static_cast<unsigned char>(raw);
    out.push_back(u >= 0x20 && u <= 0x7e ? raw : '?');
  }
  return out;
}

/// True when the message is one of the service's seven tokens. A refusal that is not one (a route that is not registered
/// answers 404 with another message; a rejected token answers 401) is mapped from the HTTP status alone, and the answer
/// line says so (`via=status`), because `invalid` from a missing route is not `invalid` from the service.
inline bool AnswerNamesResult(int http_status, std::string_view body) {
  if (http_status == 200 || http_status <= 0) return false;
  return detail::TokenResult(body).has_value();
}

inline const char* ResultName(Result result) {
  switch (result) {
    case Result::Ok:
      return "ok";
    case Result::Invalid:
      return "invalid";
    case Result::Expired:
      return "expired";
    case Result::Closed:
      return "closed";
    case Result::Denied:
      return "denied";
    case Result::Limit:
      return "limit";
    case Result::Rate:
      return "rate";
    case Result::Unavailable:
      return "unavailable";
    case Result::ServerError:
      return "server_error";
    case Result::NoServer:
      return "unavailable";
  }
  return "server_error";
}

// What the wizard shows for a result ("" for Ok: nothing to say).
inline const char* WizardText(Result result) {
  switch (result) {
    case Result::Ok:
      return "";
    case Result::Invalid:
      return "INVALID CODE";
    case Result::Expired:
      return "CODE EXPIRED";
    case Result::Closed:
      return "CODE CLOSED";
    case Result::Denied:
      return "NOT ALLOWED";
    case Result::Limit:
      return "TOO MANY CODES";
    case Result::Rate:
      return "TRY AGAIN LATER";
    case Result::Unavailable:
      return "NO CONNECTION";
    case Result::ServerError:
      return "SERVER ERROR";
    case Result::NoServer:
      return "NO SERVER AVAILABLE";
  }
  return "SERVER ERROR";
}

inline bool IsFailure(Result result) { return result != Result::Ok; }

inline std::string AnswerLine(Op op, int http_status, long latency_ms, Result result, bool via_status = false) {
  return std::string("[NEVR.INVITE] answer op=") + (op == Op::Create ? "create" : "resolve") +
         " status=" + std::to_string(http_status) + " latency_ms=" + std::to_string(latency_ms) +
         " result=" + ResultName(result) + (via_status ? " via=status" : "");
}

namespace detail {

inline bool IsHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// 8-4-4-4-12 hex digits, lower-cased; anything else (braces, no hyphens, extra bytes) is not a match id.
inline std::optional<std::string> NormalisedUuid(std::string_view text) {
  if (text.size() != 36) return std::nullopt;
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return std::nullopt;
      out.push_back(c);
      continue;
    }
    if (!IsHex(c)) return std::nullopt;
    out.push_back((c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}

// nakama writes its match id as MatchID.String(): "<uuid>.<node>" (server/match_id.go:57). The lobby GUID is the UUID
// part; the node names the nakama node and is not part of the GUID. A bare UUID is still accepted. The node is one
// non-empty run of letters, digits, '-' and '_'; anything else after the dot is no match id.
inline std::optional<std::string> MatchIdUuid(std::string_view text) {
  const std::size_t dot = text.find('.');
  if (dot == std::string_view::npos) return NormalisedUuid(text);
  const std::optional<std::string> uuid = NormalisedUuid(text.substr(0, dot));
  if (!uuid.has_value()) return std::nullopt;
  const std::string_view node = text.substr(dot + 1);
  if (node.empty()) return std::nullopt;
  for (const char c : node) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!allowed) return std::nullopt;
  }
  return uuid;
}

inline bool Digits(std::string_view s, std::size_t from, std::size_t count, int low, int high, int& value) {
  value = 0;
  for (std::size_t i = from; i < from + count; ++i) {
    if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
    value = value * 10 + (s[i] - '0');
  }
  return value >= low && value <= high;
}

// "2026-10-21T05:38:59.63Z" -> "EXPIRES: 05:38 UTC". Only the UTC form ('Z') is read; anything else is a dash.
inline std::string ExpiresText(const nlohmann::json& doc) {
  static const char* const kNone = "EXPIRES: -";
  const nlohmann::json* field = StringField(doc, "expires_at");
  if (field == nullptr) return kNone;
  const std::string s = field->get<std::string>();
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!Digits(s, 0, 4, 0, 9999, year) || s.size() < 20 || s[4] != '-' || !Digits(s, 5, 2, 1, 12, month) ||
      s[7] != '-' || !Digits(s, 8, 2, 1, 31, day) || s[10] != 'T' || !Digits(s, 11, 2, 0, 23, hour) ||
      s[13] != ':' || !Digits(s, 14, 2, 0, 59, minute) || s[16] != ':' || !Digits(s, 17, 2, 0, 60, second)) {
    return kNone;
  }
  std::size_t at = 19;
  if (s[at] == '.') {
    ++at;
    std::size_t fraction = 0;
    while (at < s.size() && s[at] >= '0' && s[at] <= '9') {
      ++at;
      ++fraction;
    }
    if (fraction == 0) return kNone;
  }
  if (at + 1 != s.size() || s[at] != 'Z') return kNone;
  char text[32];
  std::snprintf(text, sizeof(text), "EXPIRES: %02d:%02d UTC", hour, minute);
  return text;
}

}  // namespace detail

struct Created {
  std::string code;
  std::string match_id;
  std::string expires_text;
};

// A created code is written into the game's text, so it is checked first: the service sends the canonical form, and
// anything outside the alphabet or the length bound, or a match id that is not a UUID, is no answer.
inline std::optional<Created> ParseCreated(std::string_view body) {
  const nlohmann::json doc = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
  if (!doc.is_object()) return std::nullopt;
  const nlohmann::json* code = detail::StringField(doc, "code");
  const nlohmann::json* match = detail::StringField(doc, "match_id");
  if (code == nullptr || match == nullptr) return std::nullopt;
  const CheckedCode checked = CheckCode(code->get<std::string>());
  if (checked.check != CodeCheck::Ok || checked.code != code->get<std::string>()) return std::nullopt;
  const std::optional<std::string> uuid = detail::MatchIdUuid(match->get<std::string>());
  if (!uuid.has_value()) return std::nullopt;
  return Created{checked.code, *uuid, detail::ExpiresText(doc)};
}

struct Resolved {
  std::string match_id;
};

inline std::optional<Resolved> ParseResolved(std::string_view body) {
  const nlohmann::json doc = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
  if (!doc.is_object()) return std::nullopt;
  const nlohmann::json* match = detail::StringField(doc, "match_id");
  if (match == nullptr) return std::nullopt;
  const std::optional<std::string> uuid = detail::MatchIdUuid(match->get<std::string>());
  if (!uuid.has_value()) return std::nullopt;
  return Resolved{*uuid};
}

}  // namespace nevr_invite
