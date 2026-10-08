#include "quest/login/login_rewrite.h"

#include <initializer_list>
#include <exception>
#include <limits>
#include <string_view>

#include <nlohmann/json.hpp>

#include "runtime/compat/hmd_serial.h"
#include "runtime/compat/login_profile.h"

namespace QuestLogin {

namespace {

constexpr char kPathSeparator = '|';

// The game's value when there is no VR (CR15NetGame::LogIn sends "N/A"); relayed as is.
constexpr std::string_view kNoVrSerial = "N/A";
constexpr std::string_view kGameMeasuredPrefix = "system_info|";

void Flatten(const nlohmann::json& node, const std::string& prefix, std::vector<Field>& fields,
             std::vector<std::string>& skipped) {
  for (auto it = node.begin(); it != node.end(); ++it) {
    const std::string path = prefix.empty() ? it.key() : prefix + kPathSeparator + it.key();
    const nlohmann::json& value = it.value();
    Field field;
    field.path = path;
    if (value.is_object()) {
      Flatten(value, path, fields, skipped);
      continue;
    }
    if (value.is_string()) {
      field.kind = FieldKind::String;
      field.text = value.get<std::string>();
    } else if (value.is_boolean()) {
      field.kind = FieldKind::Boolean;
      field.number = value.get<bool>() ? 1 : 0;
    } else if (value.is_number_integer() || value.is_number_unsigned()) {
      if (value.is_number_unsigned() &&
          value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        skipped.push_back(path);
        continue;
      }
      field.kind = FieldKind::Int;
      field.number = value.get<std::int64_t>();
    } else {
      // Arrays, nulls and reals have no setter the login needs. Recorded, not dropped silently.
      skipped.push_back(path);
      continue;
    }
    fields.push_back(std::move(field));
  }
}

}  // namespace

const char* IdentityStatusName(IdentityStatus status) {
  switch (status) {
    case IdentityStatus::Ok: return "ok";
    case IdentityStatus::NotReady: return "not-ready";
    case IdentityStatus::NoAccount: return "no-account";
    default: return "no-token";
  }
}

const char* StatusName(ComposeStatus status) {
  switch (status) {
    case ComposeStatus::Ok: return "ok";
    case ComposeStatus::MissingAccountId: return "missing-account-id";
    case ComposeStatus::MissingToken: return "missing-token";
    default: return "profile-build-failed";
  }
}

Composition Compose(const Identity& identity, const GameValues& game, const BuildInfo& build) {
  Composition result;
  if (identity.account_id == 0 ||
      identity.account_id > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    result.status = ComposeStatus::MissingAccountId;
    return result;
  }
  if (identity.access_token.empty()) {
    result.status = ComposeStatus::MissingToken;
    return result;
  }

  // The serial the stock client sends: the game's own value, "N/A" with no VR, "unknown" only
  // when the game has none or it is not a serial (HmdSerial::Select is the PCVR rule).
  HmdSerial::Choice serial = game.hmd_serial == kNoVrSerial
                                 ? HmdSerial::Choice{std::string(kNoVrSerial), HmdSerial::Source::NoVr}
                                 : HmdSerial::Select(false, game.hmd_serial.c_str());
  result.hmd_serial_source = HmdSerial::SourceName(serial.source);
  result.hmd_serial_length = serial.value.size();

  LoginProfile::LoginProfileInputs inputs;
  inputs.account_id = identity.account_id;
  inputs.display_name = identity.display_name;
  inputs.access_token = identity.access_token;
  // inputs.password stays empty: see Identity.
  inputs.hmd_serial_number = serial.value;
  if (!game.headset_type.empty()) inputs.headset_type = game.headset_type;
  inputs.project_version = build.project_version;
  inputs.git_commit = build.git_commit;
  inputs.git_describe = build.git_describe;
  inputs.build_type = build.build_type;
  // social_level stays 0: the Quest adapter implements no social handler yet, and the server
  // sends a newer social message only to a session that declared its level (ADR 0003).

  // dump() throws on a display name that is not valid UTF-8; the game ABI must never see it.
  try {
    const std::string text = LoginProfile::BuildLoginProfileJson(inputs);
    const nlohmann::json profile = nlohmann::json::parse(text, nullptr, false);
    if (profile.is_discarded() || !profile.is_object()) {
      result.status = ComposeStatus::ProfileBuildFailed;
      return result;
    }
    Flatten(profile, std::string(), result.fields, result.skipped);
  } catch (const std::exception&) {
    result.fields.clear();
    result.skipped.clear();
    result.status = ComposeStatus::ProfileBuildFailed;
    return result;
  }
  result.status = ComposeStatus::Ok;
  return result;
}

bool IsClientClassPath(const std::string& path) {
  return path == "buildversion" || path == "appid" || path == "lobbyversion" ||
         path == "publisher_lock";
}

bool IsGameMeasuredPath(const std::string& path) {
  return std::string_view(path).substr(0, kGameMeasuredPrefix.size()) == kGameMeasuredPrefix;
}

GameValues ReadGameValues(const JsonAccess& json) {
  GameValues values;
  if (json.TypeOf("hmdserialnumber") == JsonType::String) values.hmd_serial = json.GetString("hmdserialnumber");
  if (json.TypeOf("system_info|headset_type") == JsonType::String) {
    values.headset_type = json.GetString("system_info|headset_type");
  }
  return values;
}

namespace {

// What a key held before the rewrite touched it.
struct Saved {
  std::string path;
  std::string top;        // first path segment when the path is nested, else empty
  JsonType top_type = JsonType::Absent;
  JsonType type = JsonType::Absent;
  std::string text;
  std::int64_t number = 0;
};

bool Restorable(JsonType type) {
  return type == JsonType::Absent || type == JsonType::Null || type == JsonType::String ||
         type == JsonType::Int || type == JsonType::Boolean;
}

// The type a Field writes.
JsonType WrittenType(FieldKind kind) {
  switch (kind) {
    case FieldKind::String: return JsonType::String;
    case FieldKind::Int: return JsonType::Int;
    default: return JsonType::Boolean;
  }
}

Saved Snapshot(const Field& field, const JsonAccess& json) {
  Saved saved;
  saved.path = field.path;
  const std::size_t bar = field.path.find(kPathSeparator);
  if (bar != std::string::npos) {
    saved.top = field.path.substr(0, bar);
    saved.top_type = json.TypeOf(saved.top.c_str());
  }
  saved.type = json.TypeOf(field.path.c_str());
  switch (saved.type) {
    case JsonType::String: saved.text = json.GetString(field.path.c_str()); break;
    case JsonType::Int: saved.number = json.GetInt(field.path.c_str()); break;
    case JsonType::Boolean: saved.number = json.GetBoolean(field.path.c_str()) ? 1 : 0; break;
    default: break;
  }
  return saved;
}

// Puts one key back. Allocation-free: it only reads strings it already owns.
void Restore(const Saved& saved, JsonAccess& json) {
  switch (saved.type) {
    case JsonType::Absent:
      if (!saved.top.empty() && saved.top_type == JsonType::Absent) {
        json.Clear(saved.top.c_str());
      } else {
        json.Clear(saved.path.c_str());
      }
      break;
    case JsonType::Null:
      // SetNull is a typed write like the others, so the value written over the null is
      // removed first.
      json.Clear(saved.path.c_str());
      json.SetNull(saved.path.c_str());
      break;
    case JsonType::String: json.SetString(saved.path.c_str(), saved.text.c_str()); break;
    case JsonType::Int: json.SetInt(saved.path.c_str(), saved.number); break;
    case JsonType::Boolean: json.SetBoolean(saved.path.c_str(), saved.number != 0); break;
    default: break;  // never written: Snapshot refuses these before any write
  }
}

void RestoreAll(const std::vector<Saved>& saved, std::size_t count, JsonAccess& json) {
  for (std::size_t i = count; i > 0; --i) Restore(saved[i - 1], json);
}

bool WriteAndVerify(const Field& field, JsonAccess& json) {
  switch (field.kind) {
    case FieldKind::String:
      json.SetString(field.path.c_str(), field.text.c_str());
      return json.TypeOf(field.path.c_str()) == JsonType::String &&
             json.GetString(field.path.c_str()) == field.text;
    case FieldKind::Int:
      json.SetInt(field.path.c_str(), field.number);
      return json.TypeOf(field.path.c_str()) == JsonType::Int &&
             json.GetInt(field.path.c_str()) == field.number;
    case FieldKind::Boolean:
      json.SetBoolean(field.path.c_str(), field.number != 0);
      return json.TypeOf(field.path.c_str()) == JsonType::Boolean &&
             json.GetBoolean(field.path.c_str()) == (field.number != 0);
  }
  return false;
}

}  // namespace

bool ApplyFieldsAtomically(const std::vector<Field>& fields, JsonAccess& json,
                           std::string& failed_path) {
  failed_path.clear();
  std::vector<Saved> saved;
  std::size_t written = 0;
  try {
    saved.reserve(fields.size());
    for (const Field& field : fields) {
      Saved before = Snapshot(field, json);
      // A key of another type would refuse the write (CJson will not change a type), and a
      // real, array or object value could not be put back; either way nothing is written.
      const bool same_type = before.type == WrittenType(field.kind);
      const bool replaceable = before.type == JsonType::Absent || before.type == JsonType::Null;
      // A nested write under a parent that exists but is not an object is refused by CJson
      // ("$ json path: %s is not an object.", libpnsovr.so string 0x5825b1; the setter's path
      // walker 0x35ba84 branches to 0x364bdc, which tests the node type at 0x364bf8-0x364c00);
      // the rewrite does not attempt it. Every setter also refuses when the CJson is cached
      // ([this+8] != 0, "json db is cached, read only", string 0x5820c0); the read-back after
      // each write covers that.
      const bool parent_ok = before.top.empty() || before.top_type == JsonType::Absent ||
                             before.top_type == JsonType::Object;
      if (!Restorable(before.type) || !(same_type || replaceable) || !parent_ok) {
        failed_path = field.path;
        return false;
      }
      saved.push_back(std::move(before));
    }
    for (const Field& field : fields) {
      ++written;
      if (!WriteAndVerify(field, json)) {
        failed_path = field.path;
        RestoreAll(saved, written, json);
        return false;
      }
    }
  } catch (const std::exception&) {
    RestoreAll(saved, written, json);
    failed_path = "(exception)";
    return false;
  }
  return true;
}

const char* OutcomeName(Outcome outcome) {
  switch (outcome) {
    case Outcome::Rewritten: return "rewritten";
    case Outcome::NoIdentity: return "no-identity";
    case Outcome::PlatformMismatch: return "platform-mismatch";
    case Outcome::UserUnreadable: return "user-unreadable";
    case Outcome::ComposeFailed: return "compose-failed";
    case Outcome::JsonWriteFailed: return "json-write-failed";
    case Outcome::AccountIdNotCarried: return "account-id-not-carried";
    default: return "exception";
  }
}

namespace {

// One structured record: event "quest_login", then `outcome`, then the detail fields.
void Emit(LogFn log, Level level, Outcome outcome, const LogKv* details, std::size_t n) {
  if (log == nullptr) return;
  LogKv fields[10];
  std::size_t count = 0;
  fields[count++] = LogKv{"outcome", OutcomeName(outcome), 0};
  for (std::size_t i = 0; i < n; ++i) {
    if (count < sizeof(fields) / sizeof(fields[0])) fields[count++] = details[i];
  }
  log(level, "quest_login", fields, count);
}

void Emit(LogFn log, Level level, Outcome outcome, std::initializer_list<LogKv> details) {
  Emit(log, level, outcome, details.begin(), details.size());
}

LogKv Text(const char* key, const char* value) { return LogKv{key, value, 0}; }
LogKv Num(const char* key, long long value) { return LogKv{key, nullptr, value}; }

}  // namespace

namespace {

// Ends a login the rewrite did not take: puts the Oculus account id back (a no-op when no
// earlier login replaced it) and writes the one record, with `restored` saying whether the
// id was put back. The JSON needs no undoing here; ApplyFieldsAtomically restores its own.
Outcome Decline(UserAccess& user, LogFn log, Outcome outcome, Level level,
                std::initializer_list<LogKv> details) noexcept {
  bool restored = false;
  try {
    restored = user.RestoreAccountId();
  } catch (const std::exception&) {
  }
  try {
    LogKv fields[10];
    std::size_t count = 0;
    for (const LogKv& detail : details) {
      if (count < sizeof(fields) / sizeof(fields[0]) - 1) fields[count++] = detail;
    }
    fields[count++] = LogKv{"restored", nullptr, restored ? 1 : 0};
    Emit(log, level, outcome, fields, count);
  } catch (const std::exception&) {
  }
  return outcome;
}

Outcome RewriteLoginImpl(UserAccess& user, JsonAccess& json, IdentitySource& source,
                         const BuildInfo& build, LogFn log) {
  try {
    Identity identity;
    const IdentityStatus identity_status = source.Fetch(identity);
    if (identity_status != IdentityStatus::Ok) {
      // The ADR forbids fabricating an identity: the original Oculus login goes out unchanged
      // and the server answers it.
      return Decline(user, log, Outcome::NoIdentity, Level::Error,
                     {Text("reason", IdentityStatusName(identity_status))});
    }

    // CNSOVRUser's constructor stores provider 4 in the platform word, which is the platform
    // the NEVR login carries. Anything else means this is not the user class the rewrite was
    // measured on, and the later requests would name a different platform.
    std::uint64_t provider = 0;
    if (!user.Provider(provider)) {
      return Decline(user, log, Outcome::UserUnreadable, Level::Error, {Text("reason", "provider")});
    }
    if ((provider & kProviderMask) != kPlatformOvrOrg) {
      return Decline(user, log, Outcome::PlatformMismatch, Level::Error,
                     {Num("provider", static_cast<long long>(provider & kProviderMask)),
                      Num("expected", static_cast<long long>(kPlatformOvrOrg))});
    }

    Composition composition = Compose(identity, ReadGameValues(json), build);
    if (composition.status != ComposeStatus::Ok) {
      return Decline(user, log, Outcome::ComposeFailed, Level::Error,
                     {Text("reason", StatusName(composition.status))});
    }

    // Client-class members are never written; hardware members the game measured are kept.
    std::vector<Field> fields;
    fields.reserve(composition.fields.size());
    std::size_t kept_client_class = 0;
    std::size_t kept_measured = 0;
    for (Field& field : composition.fields) {
      if (IsClientClassPath(field.path)) {
        ++kept_client_class;
        continue;
      }
      if (IsGameMeasuredPath(field.path) && json.TypeOf(field.path.c_str()) != JsonType::Absent) {
        ++kept_measured;
        continue;
      }
      fields.push_back(std::move(field));
    }

    // The account id is not in the JSON: the game asks the user object for it through a
    // virtual call. Change what that call returns, then ask it the same way the sender will.
    // This step runs first so the JSON transaction is the last thing that can fail and the
    // account id is restored if it does.
    std::uint64_t wire = 0;
    if (!user.SetAccountId(identity.account_id) || !user.WireAccountId(wire) ||
        wire != identity.account_id) {
      return Decline(user, log, Outcome::AccountIdNotCarried, Level::Error, {});
    }

    std::string failed_path;
    if (!ApplyFieldsAtomically(fields, json, failed_path)) {
      return Decline(user, log, Outcome::JsonWriteFailed, Level::Error,
                     {Text("first_path", failed_path.c_str()), Num("fields", static_cast<long long>(fields.size()))});
    }

    // The JSON is committed from here on. Nothing below may throw into the catch clause, which
    // would otherwise report a failure over a rewritten login; a failing log sink is ignored.
    try {
      Emit(log, Level::Info, Outcome::Rewritten,
           {Num("platform", static_cast<long long>(kPlatformOvrOrg)),
            Text("hmd_serial_source", composition.hmd_serial_source.c_str()),
            Num("fields", static_cast<long long>(fields.size())),
            Num("skipped", static_cast<long long>(composition.skipped.size())),
            Num("kept_client_class", static_cast<long long>(kept_client_class)),
            Num("kept_measured", static_cast<long long>(kept_measured))});
    } catch (const std::exception&) {
      // Logging failed after the commit; the rewrite itself stands.
    }
    return Outcome::Rewritten;
  } catch (const std::exception&) {
    // Reached only before the JSON transaction commits: Fetch, Compose and the field filter
    // run first, ApplyFieldsAtomically restores its own writes, and nothing after it throws.
    return Decline(user, log, Outcome::Exception, Level::Error, {});
  }
}

}  // namespace

Outcome RewriteLogin(UserAccess& user, JsonAccess& json, IdentitySource& source,
                     const BuildInfo& build, LogFn log) {
  return RewriteLoginImpl(user, json, source, build, log);
}

Outcome RewriteLoginNoThrow(UserAccess& user, JsonAccess& json, IdentitySource& source,
                            const BuildInfo& build, LogFn log) noexcept {
  try {
    return RewriteLogin(user, json, source, build, log);
  } catch (const std::exception&) {
    // RewriteLogin handles its own std::exception; this is the last line before noexcept.
    try {
      user.RestoreAccountId();
    } catch (const std::exception&) {
    }
    return Outcome::Exception;
  }
}

Outcome RewriteAndSend(UserAccess& user, JsonAccess& json, IdentitySource& source,
                       const BuildInfo& build, LogFn log, SendFn send, void* context) {
  const Outcome outcome = RewriteLogin(user, json, source, build, log);
  send(context);
  return outcome;
}

}  // namespace QuestLogin
