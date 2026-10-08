#include "quest/login/login_rewrite.h"

#include <cstdio>
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

UserIdWords RewriteUserId(const UserIdWords& original, const Identity& identity) {
  UserIdWords out;
  out.platform_word = (original.platform_word & ~kProviderMask) | kPlatformOvrOrg;
  out.account_id = identity.account_id;
  return out;
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
  inputs.password = identity.password;
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

GameValues ReadGameValues(const JsonAccess& json) {
  GameValues values;
  bool present = false;
  std::string serial = json.GetString("hmdserialnumber", present);
  if (present) values.hmd_serial = std::move(serial);
  std::string headset = json.GetString("system_info|headset_type", present);
  if (present) values.headset_type = std::move(headset);
  return values;
}

std::vector<std::string> ApplyFields(const std::vector<Field>& fields, JsonAccess& json) {
  std::vector<std::string> mismatched;
  for (const Field& field : fields) {
    bool present = false;
    bool ok = false;
    switch (field.kind) {
      case FieldKind::String:
        json.SetString(field.path.c_str(), field.text.c_str());
        ok = json.GetString(field.path.c_str(), present) == field.text && present;
        break;
      case FieldKind::Int:
        json.SetInt(field.path.c_str(), field.number);
        ok = json.GetInt(field.path.c_str(), present) == field.number && present;
        break;
      case FieldKind::Boolean:
        json.SetBoolean(field.path.c_str(), field.number != 0);
        ok = json.GetBoolean(field.path.c_str(), present) == (field.number != 0) && present;
        break;
    }
    if (!ok) mismatched.push_back(field.path);
  }
  return mismatched;
}

const char* OutcomeName(Outcome outcome) {
  switch (outcome) {
    case Outcome::Rewritten: return "rewritten";
    case Outcome::NoIdentity: return "no-identity";
    case Outcome::ComposeFailed: return "compose-failed";
    case Outcome::UserUnreadable: return "user-unreadable";
    case Outcome::JsonWriteFailed: return "json-write-failed";
    default: return "user-write-failed";
  }
}

namespace {

void Emit(LogFn log, Level level, const char* format, const char* a, const char* b, std::size_t n1,
          std::size_t n2) {
  if (log == nullptr) return;
  char line[256];
  std::snprintf(line, sizeof(line), format, a, b, n1, n2);
  log(level, line);
}

}  // namespace

Outcome RewriteLogin(UserAccess& user, JsonAccess& json, IdentitySource& source,
                     const BuildInfo& build, LogFn log) {
  Identity identity;
  std::string reason;
  if (!source.Fetch(identity, reason)) {
    // The ADR forbids fabricating an identity; the original login goes out unchanged and the
    // server answers it. `reason` is a short fixed token supplied by the source, never a value.
    Emit(log, Level::Error,
         "quest.login outcome=%s reason=%s: no NEVR identity, original Oculus login left intact (%zu,%zu)",
         OutcomeName(Outcome::NoIdentity), reason.c_str(), 0, 0);
    return Outcome::NoIdentity;
  }

  UserIdWords before;
  if (!user.Read(before)) {
    Emit(log, Level::Error, "quest.login outcome=%s reason=%s: CNSUser identity words unreadable (%zu,%zu)",
         OutcomeName(Outcome::UserUnreadable), "read", 0, 0);
    return Outcome::UserUnreadable;
  }

  const Composition composition = Compose(identity, ReadGameValues(json), build);
  if (composition.status != ComposeStatus::Ok) {
    Emit(log, Level::Error, "quest.login outcome=%s reason=%s: login fields not composed (%zu,%zu)",
         OutcomeName(Outcome::ComposeFailed), StatusName(composition.status), 0, 0);
    return Outcome::ComposeFailed;
  }

  const std::vector<std::string> mismatched = ApplyFields(composition.fields, json);
  if (!mismatched.empty()) {
    // First refused path only (a key name, not a value); the count says how many.
    Emit(log, Level::Error,
         "quest.login outcome=%s first_path=%s: CJson refused or altered %zu field(s) of %zu",
         OutcomeName(Outcome::JsonWriteFailed), mismatched.front().c_str(), mismatched.size(),
         composition.fields.size());
    return Outcome::JsonWriteFailed;
  }

  if (!user.Write(RewriteUserId(before, identity))) {
    Emit(log, Level::Error, "quest.login outcome=%s reason=%s: CNSUser identity words not written (%zu,%zu)",
         OutcomeName(Outcome::UserWriteFailed), "write", 0, 0);
    return Outcome::UserWriteFailed;
  }

  Emit(log, Level::Info,
       "quest.login outcome=%s platform=4 hmd_serial_source=%s fields=%zu skipped=%zu",
       OutcomeName(Outcome::Rewritten), composition.hmd_serial_source.c_str(),
       composition.fields.size(), composition.skipped.size());
  return Outcome::Rewritten;
}

}  // namespace QuestLogin
