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
constexpr const char* kPluginsKey = "nevr_plugins";

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

// The `nevr_plugins` array as CJson indexed paths. CJson has no array setter the login uses, but the game's own code builds
// array members as `name[i]|key` through SetString and SetInt (libpnsovr NRadEngine::CTcpMarkovChain::ToJson, 0x3310c0), so
// each member is one Field. The setter chain that makes the indexed form work, link-time addresses in libpnsovr: SetString
// 0x35917c -> path walker 0x35ba84 -> object walker 0x364bdc -> array member 0x36522c -> array walker 0x364a0c; the tokenizer
// 0x363718 parses "N]|", and 0x364dd4 pads the array with json_null.
// An empty array is no fields: the service reads an absent key as an empty report.
void FlattenPlugins(const nlohmann::json& array, std::vector<Field>& fields) {
  std::size_t index = 0;
  for (const nlohmann::json& entry : array) {
    if (!entry.is_object()) continue;
    const std::string base = std::string(kPluginsKey) + "[" + std::to_string(index++) + "]";
    std::vector<std::string> skipped;
    Flatten(entry, base, fields, skipped);
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
  // when the game has none or it is not a serial (nevr_hmd_serial::Select is the PCVR rule).
  nevr_hmd_serial::Choice serial = game.hmd_serial == kNoVrSerial
                                 ? nevr_hmd_serial::Choice{std::string(kNoVrSerial), nevr_hmd_serial::Source::NoVr}
                                 : nevr_hmd_serial::Select(false, game.hmd_serial.c_str());
  result.hmd_serial_source = nevr_hmd_serial::SourceName(serial.source);
  result.hmd_serial_length = serial.value.size();

  nevr_login_profile::LoginProfileInputs inputs;
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
  // The loader's report goes through the PCVR login's own serializer, so both platforms send one shape.
  try {
    if (build.plugin_report) {
      const std::string manifest = BuildPluginManifest(build.plugin_report());
      nlohmann::json plugins = nlohmann::json::parse(manifest, nullptr, false);
      if (plugins.is_array()) inputs.plugins = std::move(plugins);
    }
  } catch (const std::exception&) {
    // A report that cannot be built is an empty one; the login does not depend on it.
  }

  // dump() throws on a display name that is not valid UTF-8; the game ABI must never see it.
  try {
    const std::string text = nevr_login_profile::BuildLoginProfileJson(inputs);
    nlohmann::json profile = nlohmann::json::parse(text, nullptr, false);
    if (profile.is_discarded() || !profile.is_object()) {
      result.status = ComposeStatus::ProfileBuildFailed;
      return result;
    }
    // Taken out first: Flatten has no array case, and the members go in as indexed paths (FlattenPlugins).
    nlohmann::json plugins = nlohmann::json::array();
    if (profile.contains(kPluginsKey)) {
      plugins = profile[kPluginsKey];
      profile.erase(kPluginsKey);
    }
    Flatten(profile, std::string(), result.fields, result.skipped);
    FlattenPlugins(plugins, result.plugin_fields);
  } catch (const std::exception&) {
    result.fields.clear();
    result.plugin_fields.clear();
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
  plan.plugin_fields = std::move(composition.plugin_fields);
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
