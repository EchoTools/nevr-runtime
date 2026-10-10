#include "quest/login/login_rewrite.h"

#include <exception>
#include <limits>
#include <string_view>

#include <nlohmann/json.hpp>

#include "runtime/compat/hmd_serial.h"
#include "runtime/compat/login_profile.h"
#include "quest/sentinel/outside_game_call.h"

namespace nevr_quest_login {

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
  // The same social declaration as the PCVR login, through the shared builder's field; the
  // IdentitySource sets it (Identity::social_level), 0 declares none.
  inputs.social_level = static_cast<std::uint64_t>(identity.social_level < 0 ? 0 : identity.social_level);

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

namespace {

bool MeasuredPresent(const Observation& observed, const std::string& path) {
  for (std::size_t i = 0; i < kMeasuredCount; ++i) {
    if (path == kMeasuredPaths[i]) return observed.measured_present[i];
  }
  return false;
}

void ComposePlanImpl(IdentitySource& source, const BuildInfo& build, const Observation& observed,
                     Plan& plan) {
  Identity identity;
  plan.identity_status = source.Fetch(identity);
  if (plan.identity_status != IdentityStatus::Ok) {
    // The ADR forbids fabricating an identity: the original Oculus login goes out unchanged
    // and the server answers it.
    plan.outcome = Outcome::NoIdentity;
    return;
  }

  // CNSOVRUser's constructor stores provider 4 in the platform word, which is the platform the
  // NEVR login carries. Anything else means this is not the user class the rewrite was measured
  // on, and the later requests would name a different platform.
  if (!observed.provider_readable) {
    plan.outcome = Outcome::UserUnreadable;
    return;
  }
  plan.provider = observed.provider & kProviderMask;
  if (plan.provider != kPlatformOvrOrg) {
    plan.outcome = Outcome::PlatformMismatch;
    return;
  }

  Composition composition = Compose(identity, observed.game, build);
  plan.compose_status = composition.status;
  if (composition.status != ComposeStatus::Ok) {
    plan.outcome = Outcome::ComposeFailed;
    return;
  }

  // Client-class members are never written; hardware members the game measured are kept.
  plan.fields.reserve(composition.fields.size());
  for (Field& field : composition.fields) {
    if (IsClientClassPath(field.path)) {
      ++plan.kept_client_class;
      continue;
    }
    if (IsGameMeasuredPath(field.path) && MeasuredPresent(observed, field.path)) {
      ++plan.kept_measured;
      continue;
    }
    plan.fields.push_back(std::move(field));
  }
  plan.account_id = identity.account_id;
  plan.skipped = composition.skipped.size();
  plan.hmd_serial_source = composition.hmd_serial_source;
  plan.outcome = Outcome::Rewritten;
}

}  // namespace

// Runs between game calls and calls none (the compose phase): the one function in the login path that
// carries a personality, marked so the frame sensor does not enter it (outside_game_call.h).
NEVR_OUTSIDE_GAME_CALL void ComposePlan(IdentitySource& source, const BuildInfo& build,
                                        const Observation& observed, Plan& plan) noexcept {
  plan = Plan();
  try {
    ComposePlanImpl(source, build, observed, plan);
  } catch (const std::exception&) {
    // The compose phase runs between game calls and calls none, so nothing needs undoing.
    plan = Plan();
    plan.outcome = Outcome::Exception;
  }
}

}  // namespace nevr_quest_login
