#include "quest/login/login_rewrite.h"

#include <cstdarg>
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

GameValues ReadGameValues(const JsonAccess& json) {
  GameValues values;
  bool present = false;
  std::string serial = json.GetString("hmdserialnumber", present);
  if (present) values.hmd_serial = std::move(serial);
  std::string headset = json.GetString("system_info|headset_type", present);
  if (present) values.headset_type = std::move(headset);
  return values;
}

namespace {

constexpr std::string_view kGameOwnedPrefix = "system_info|";

// What a key held before the rewrite touched it.
struct Saved {
  std::string path;
  std::string top;        // first path segment when the path is nested, else empty
  bool top_existed = false;
  bool existed = false;   // the leaf held a value of some type
  FieldKind kind = FieldKind::String;
  std::string text;
  std::int64_t number = 0;
};

bool PresentAnyKind(const JsonAccess& json, const char* path) {
  bool present = false;
  json.GetString(path, present);
  if (present) return true;
  json.GetInt(path, present);
  if (present) return true;
  json.GetBoolean(path, present);
  return present;
}

Saved Snapshot(const Field& field, const JsonAccess& json) {
  Saved saved;
  saved.path = field.path;
  const std::size_t bar = field.path.find(kPathSeparator);
  if (bar != std::string::npos) {
    saved.top = field.path.substr(0, bar);
    saved.top_existed = json.IsObject(saved.top.c_str());
  }
  bool present = false;
  std::string text = json.GetString(field.path.c_str(), present);
  if (present) {
    saved.existed = true;
    saved.kind = FieldKind::String;
    saved.text = std::move(text);
    return saved;
  }
  const std::int64_t number = json.GetInt(field.path.c_str(), present);
  if (present) {
    saved.existed = true;
    saved.kind = FieldKind::Int;
    saved.number = number;
    return saved;
  }
  const bool flag = json.GetBoolean(field.path.c_str(), present);
  if (present) {
    saved.existed = true;
    saved.kind = FieldKind::Boolean;
    saved.number = flag ? 1 : 0;
  }
  return saved;
}

// Puts one key back. Allocation-free: it only reads strings it already owns.
void Restore(const Saved& saved, JsonAccess& json) {
  if (!saved.existed) {
    if (!saved.top.empty() && !saved.top_existed) {
      json.Clear(saved.top.c_str());
    } else {
      json.Clear(saved.path.c_str());
    }
    return;
  }
  switch (saved.kind) {
    case FieldKind::String: json.SetString(saved.path.c_str(), saved.text.c_str()); break;
    case FieldKind::Int: json.SetInt(saved.path.c_str(), saved.number); break;
    case FieldKind::Boolean: json.SetBoolean(saved.path.c_str(), saved.number != 0); break;
  }
}

void RestoreAll(const std::vector<Saved>& saved, std::size_t count, JsonAccess& json) {
  for (std::size_t i = count; i > 0; --i) Restore(saved[i - 1], json);
}

bool WriteAndVerify(const Field& field, JsonAccess& json) {
  bool present = false;
  switch (field.kind) {
    case FieldKind::String:
      json.SetString(field.path.c_str(), field.text.c_str());
      return json.GetString(field.path.c_str(), present) == field.text && present;
    case FieldKind::Int:
      json.SetInt(field.path.c_str(), field.number);
      return json.GetInt(field.path.c_str(), present) == field.number && present;
    case FieldKind::Boolean:
      json.SetBoolean(field.path.c_str(), field.number != 0);
      return json.GetBoolean(field.path.c_str(), present) == (field.number != 0) && present;
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
      // A key of another type would refuse the write (CJson will not change a type), so
      // nothing is written at all.
      if (before.existed && before.kind != field.kind) {
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

// One structured line: `event=quest_login outcome=<name> <detail>`. Details are fixed tokens
// and counts, never values.
void Emit(LogFn log, Level level, Outcome outcome, const char* detail_format, ...)
    __attribute__((format(printf, 4, 5)));

void Emit(LogFn log, Level level, Outcome outcome, const char* detail_format, ...) {
  if (log == nullptr) return;
  char detail[160];
  va_list args;
  va_start(args, detail_format);
  std::vsnprintf(detail, sizeof(detail), detail_format, args);
  va_end(args);
  char line[256];
  std::snprintf(line, sizeof(line), "event=quest_login outcome=%s %s", OutcomeName(outcome), detail);
  log(level, line);
}

}  // namespace

Outcome RewriteLogin(UserAccess& user, JsonAccess& json, IdentitySource& source,
                     const BuildInfo& build, LogFn log) {
  bool account_set = false;
  try {
    Identity identity;
    const IdentityStatus identity_status = source.Fetch(identity);
    if (identity_status != IdentityStatus::Ok) {
      // The ADR forbids fabricating an identity: the original Oculus login goes out unchanged
      // and the server answers it.
      Emit(log, Level::Error, Outcome::NoIdentity, "reason=%s action=original_login_unchanged",
           IdentityStatusName(identity_status));
      return Outcome::NoIdentity;
    }

    // CNSOVRUser's constructor stores provider 4 in the platform word, which is the platform
    // the NEVR login carries. Anything else means this is not the user class the rewrite was
    // measured on, and the later requests would name a different platform.
    std::uint64_t provider = 0;
    if (!user.Provider(provider)) {
      Emit(log, Level::Error, Outcome::UserUnreadable, "reason=provider");
      return Outcome::UserUnreadable;
    }
    if ((provider & kProviderMask) != kPlatformOvrOrg) {
      Emit(log, Level::Error, Outcome::PlatformMismatch, "provider=%llu expected=%llu",
           static_cast<unsigned long long>(provider & kProviderMask),
           static_cast<unsigned long long>(kPlatformOvrOrg));
      return Outcome::PlatformMismatch;
    }

    Composition composition = Compose(identity, ReadGameValues(json), build);
    if (composition.status != ComposeStatus::Ok) {
      Emit(log, Level::Error, Outcome::ComposeFailed, "reason=%s", StatusName(composition.status));
      return Outcome::ComposeFailed;
    }

    // The game fills system_info (cpu, cores, memory, network type, headset, OS build) with
    // real measurements from the headset; the PCVR builder's empty/zero placeholders must not
    // overwrite them. Only members the game left out are added.
    std::vector<Field> fields;
    fields.reserve(composition.fields.size());
    std::size_t kept_game_values = 0;
    for (Field& field : composition.fields) {
      if (std::string_view(field.path).substr(0, kGameOwnedPrefix.size()) == kGameOwnedPrefix &&
          PresentAnyKind(json, field.path.c_str())) {
        ++kept_game_values;
        continue;
      }
      fields.push_back(std::move(field));
    }

    // The account id is not in the JSON: the game asks the user object for it through a
    // virtual call. Change what that call returns, then ask it the same way the sender will.
    // This step runs first so the JSON transaction is the last thing that can fail and the
    // account id is restored if it does.
    std::uint64_t wire = 0;
    account_set = user.SetAccountId(identity.account_id);
    if (!account_set || !user.WireAccountId(wire) || wire != identity.account_id) {
      user.RestoreAccountId();
      account_set = false;
      Emit(log, Level::Error, Outcome::AccountIdNotCarried, "action=account_id_restored");
      return Outcome::AccountIdNotCarried;
    }

    std::string failed_path;
    if (!ApplyFieldsAtomically(fields, json, failed_path)) {
      user.RestoreAccountId();
      account_set = false;
      Emit(log, Level::Error, Outcome::JsonWriteFailed,
           "first_path=%s fields=%zu action=json_and_account_id_restored original_login_unchanged",
           failed_path.c_str(), fields.size());
      return Outcome::JsonWriteFailed;
    }

    Emit(log, Level::Info, Outcome::Rewritten,
         "platform=%llu hmd_serial_source=%s fields=%zu skipped=%zu kept_game_values=%zu",
         static_cast<unsigned long long>(kPlatformOvrOrg), composition.hmd_serial_source.c_str(),
         fields.size(), composition.skipped.size(), kept_game_values);
    return Outcome::Rewritten;
  } catch (const std::exception&) {
    if (account_set) user.RestoreAccountId();
    Emit(log, Level::Error, Outcome::Exception, "action=original_login_unchanged");
    return Outcome::Exception;
  }
}

}  // namespace QuestLogin
