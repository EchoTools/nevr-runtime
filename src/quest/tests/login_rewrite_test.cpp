// Host-buildable test for the Quest login rewrite (src/quest/login/login_rewrite.cpp).
// Compiles the same sources the device links, plus the shared PCVR login builder
// (src/runtime/compat/login_profile.cpp), against a fake CJson that follows CJson's type
// rules and a fake CNSOVRUser whose account id comes from a virtual AccountID().

#include "quest/login/login_rewrite.h"

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/tests/test_check.h"
#include "runtime/compat/login_profile.h"
#include "runtime/compat/social_level.h"

namespace {

using QuestLogin::FieldKind;
using QuestLogin::JsonType;

// CJson stand-in with the type rules measured in the game:
//   SetString writes over absent, string or null; SetInt over absent, int or null;
//   SetBoolean over absent, boolean or null; any other existing type is left unchanged.
//   Clear removes a key and everything under it.
class FakeJson final : public QuestLogin::JsonAccess {
 public:
  struct Value {
    JsonType type = JsonType::Absent;
    std::string text;
    std::int64_t number = 0;
    double real = 0;
  };
  std::map<std::string, Value> values;  // leaves and the objects that contain them
  std::string refuse_path;              // writes to this path are dropped
  bool int_overwrites_real = false;     // a build whose SetInt replaces a real instead of refusing
  bool nested_write_replaces_parent = false;  // a build whose nested write turns a non-object parent into an object

  JsonType TypeOf(const char* path) const override {
    auto it = values.find(path);
    return it == values.end() ? JsonType::Absent : it->second.type;
  }
  std::string GetString(const char* path) const override { return values.at(path).text; }
  std::int64_t GetInt(const char* path) const override { return values.at(path).number; }
  bool GetBoolean(const char* path) const override { return values.at(path).number != 0; }

  void SetString(const char* path, const char* value) override {
    Put(path, JsonType::String, value, 0, 0);
  }
  void SetInt(const char* path, std::int64_t value) override { Put(path, JsonType::Int, "", value, 0); }
  void SetBoolean(const char* path, bool value) override {
    Put(path, JsonType::Boolean, "", value ? 1 : 0, 0);
  }
  void SetNull(const char* path) override { Put(path, JsonType::Null, "", 0, 0); }

  void SetReal(const char* path, double value) { Put(path, JsonType::Real, "", 0, value); }
  void SetEmptyArray(const char* path) { Put(path, JsonType::Array, "", 0, 0); }

  void Clear(const char* path) override {
    const std::string key = path;
    for (auto it = values.begin(); it != values.end();) {
      it = (it->first == key || it->first.rfind(key + "|", 0) == 0) ? values.erase(it) : std::next(it);
    }
  }

  nlohmann::json ToJson() const {
    nlohmann::json doc = nlohmann::json::object();
    for (const auto& [path, value] : values) {
      nlohmann::json* node = &doc;
      std::size_t start = 0;
      for (;;) {
        const std::size_t bar = path.find('|', start);
        const std::string part = path.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        if (bar != std::string::npos) {
          node = &(*node)[part];
          start = bar + 1;
          continue;
        }
        switch (value.type) {
          case JsonType::String: (*node)[part] = value.text; break;
          case JsonType::Int: (*node)[part] = value.number; break;
          case JsonType::Boolean: (*node)[part] = value.number != 0; break;
          case JsonType::Real: (*node)[part] = value.real; break;
          case JsonType::Null: (*node)[part] = nullptr; break;
          case JsonType::Array: (*node)[part] = nlohmann::json::array(); break;
          default: (*node)[part] = nlohmann::json::object(); break;
        }
        break;
      }
    }
    return doc;
  }

 private:
  void Put(const char* path, JsonType type, const char* text, std::int64_t number, double real) {
    if (refuse_path == path) return;
    const std::string key = path;
    auto it = values.find(key);
    if (it != values.end()) {
      const JsonType old = it->second.type;
      const bool replaceable = old == type || old == JsonType::Null ||
                               (int_overwrites_real && type == JsonType::Int && old == JsonType::Real);
      if (!replaceable) return;  // a type change is refused
    }
    // Parents become objects; a parent that is not an object blocks the write.
    for (std::size_t bar = key.find('|'); bar != std::string::npos; bar = key.find('|', bar + 1)) {
      Value& parent = values[key.substr(0, bar)];
      if (parent.type == JsonType::Absent) parent.type = JsonType::Object;
      if (parent.type != JsonType::Object && nested_write_replaces_parent) {
        parent = Value();
        parent.type = JsonType::Object;
      }
      if (parent.type != JsonType::Object) return;
    }
    Value value;
    value.type = type;
    value.text = text;
    value.number = number;
    value.real = real;
    values[key] = value;
  }
};

// The CNSOVRUser as measured: provider already 4, and the account id the sender uses comes
// from a virtual AccountID() that returns a process global, never from the object.
class FakeUser final : public QuestLogin::UserAccess {
 public:
  std::uint64_t provider = QuestLogin::kPlatformOvrOrg;
  std::uint64_t global_account_id = 5551234;     // what AccountID() returns (the org-scoped id)
  std::uint64_t object_account_field = 5551234;  // [this+0x88]: ignored by the override
  bool setter_reaches_global = true;             // false: the write lands somewhere AccountID() never reads
  bool wire_forced = false;                      // true: AccountID() returns forced_wire whatever the global holds
  std::uint64_t forced_wire = 0;
  int restores = 0;

  bool Provider(std::uint64_t& code) const override {
    code = provider;
    return true;
  }
  bool WireAccountId(std::uint64_t& id) const override {
    id = wire_forced ? forced_wire : global_account_id;
    return true;
  }
  bool SetAccountId(std::uint64_t id) override {
    if (!memory_.NoteBeforeWrite(global_account_id, id)) return false;
    if (setter_reaches_global) global_account_id = id;
    else object_account_field = id;
    return true;
  }
  bool RestoreAccountId() override {
    ++restores;
    std::uint64_t oculus = 0;
    if (!memory_.RestoreFor(global_account_id, oculus)) return false;
    global_account_id = oculus;
    return true;
  }

 private:
  QuestLogin::OculusIdMemory memory_;
};

class FakeSource final : public QuestLogin::IdentitySource {
 public:
  QuestLogin::IdentityStatus status = QuestLogin::IdentityStatus::Ok;
  QuestLogin::Identity identity;
  QuestLogin::IdentityStatus Fetch(QuestLogin::Identity& out) override {
    if (status != QuestLogin::IdentityStatus::Ok) return status;
    out = identity;
    return QuestLogin::IdentityStatus::Ok;
  }
};

// Log records flattened to `event key=value ...` text for assertions.
std::vector<std::string> g_log;
void CaptureLog(QuestLogin::Level, const char* event, const QuestLogin::LogKv* fields, std::size_t count) {
  std::string line = event;
  for (std::size_t i = 0; i < count; ++i) {
    line += std::string(" ") + fields[i].key + "=";
    line += fields[i].text != nullptr ? std::string(fields[i].text) : std::to_string(fields[i].number);
  }
  g_log.push_back(line);
}

constexpr std::uint64_t kNevrAccount = 987654321012345ULL;
// Values the Quest build reports and the server classifies on (nakama server/evr/login_request.go
// StandaloneBuildNumber; server/evr_authenticate.go QuestAppId). The build number is also the
// constant libpnsovr's GotUserProofCB stores (0x1ed938-0x1ed944).
constexpr std::int64_t kQuestBuild = 630783;
constexpr std::int64_t kQuestAppId = 0x7de88f07bd07aLL;
constexpr std::int64_t kQuestLobbyVersion = 0x3f69c77a;

QuestLogin::Identity MakeIdentity() {
  QuestLogin::Identity id;
  id.account_id = kNevrAccount;
  id.display_name = "Pilot";
  id.access_token = "NEVR-TOKEN-SECRET";
  return id;
}

QuestLogin::BuildInfo MakeBuild() {
  QuestLogin::BuildInfo build;
  build.project_version = "1.2.3";
  build.git_commit = "abc123";
  build.git_describe = "v1.2.3-4-gabc123";
  build.build_type = "Release";
  return build;
}

// The login JSON as the Oculus path leaves it (libpnsovr GotUserProofCB, CNSUser::SystemInfo,
// libr15 CR15NetGame::LogIn key names).
void SeedOculusLogin(FakeJson& json) {
  json.SetInt("appid", kQuestAppId);
  json.SetInt("accountid", 5551234);
  json.SetString("access_token", "OCULUS-ACCESS-TOKEN");
  json.SetString("nonce", "OCULUS-NONCE");
  json.SetInt("lobbyversion", kQuestLobbyVersion);
  json.SetInt("buildversion", kQuestBuild);
  json.SetString("publisher_lock", "rad15_live");
  json.SetString("hmdserialnumber", "1WMHH000000000");
  json.SetString("hmdproductname", "Quest 2");
  json.SetString("system_info|headset_type", "Quest 2");
  json.SetInt("system_info|build_version", 123456789);
  json.SetString("system_info|cpu", "Snapdragon XR2");
  json.SetInt("system_info|num_physical_cores", 4);
  json.SetInt("system_info|num_logical_cores", 8);
  json.SetInt("system_info|memory_total", 5800);
  json.SetInt("system_info|memory_used", 3100);
  json.SetString("system_info|network_type", "wifi");
}

// Member of a JSON document by '|' path; null when any step is missing.
nlohmann::json At(const nlohmann::json& doc, const std::string& path) {
  const nlohmann::json* node = &doc;
  std::size_t start = 0;
  for (;;) {
    const std::size_t bar = path.find('|', start);
    const std::string part = path.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
    if (!node->is_object() || !node->contains(part)) return nlohmann::json();
    node = &(*node)[part];
    if (bar == std::string::npos) return *node;
    start = bar + 1;
  }
}

void TestComposeFailsClosed() {
  QuestLogin::Identity id = MakeIdentity();
  id.account_id = 0;
  QCHECK(QuestLogin::Compose(id, {}, MakeBuild()).status == QuestLogin::ComposeStatus::MissingAccountId);
  id = MakeIdentity();
  id.account_id = 0x8000000000000000ULL;  // does not fit CJson's signed integer
  QCHECK(QuestLogin::Compose(id, {}, MakeBuild()).status == QuestLogin::ComposeStatus::MissingAccountId);
  id = MakeIdentity();
  id.access_token.clear();
  const QuestLogin::Composition c = QuestLogin::Compose(id, {}, MakeBuild());
  QCHECK(c.status == QuestLogin::ComposeStatus::MissingToken);
  QCHECK(c.fields.empty());
}

// The composed fields are the PCVR builder's output for the same inputs, minus the empty
// plugin array (CJson has no setter for it) and with no password value: the server reads the
// password from the upgrade URL, and the game's own log of the outgoing login does not redact it.
void TestComposedProfileMatchesPcvrBuilder() {
  const QuestLogin::Identity id = MakeIdentity();
  QuestLogin::GameValues game;
  game.hmd_serial = "1WMHH000000000";
  game.headset_type = "Quest 2";
  const QuestLogin::Composition c = QuestLogin::Compose(id, game, MakeBuild());
  QCHECK(c.status == QuestLogin::ComposeStatus::Ok);
  QCHECK(c.skipped.size() == 1 && c.skipped[0] == "nevr_plugins");

  FakeJson json;
  std::string failed;
  QCHECK(QuestLogin::ApplyFieldsAtomically(c.fields, json, failed));

  LoginProfile::LoginProfileInputs pc;
  pc.account_id = id.account_id;
  pc.display_name = id.display_name;
  pc.access_token = id.access_token;
  pc.hmd_serial_number = "1WMHH000000000";
  pc.headset_type = "Quest 2";
  pc.project_version = "1.2.3";
  pc.git_commit = "abc123";
  pc.git_describe = "v1.2.3-4-gabc123";
  pc.build_type = "Release";
  pc.social_level = SocialParty::kSocialLevel;  // what the PCVR login declares (ws_bridge.cpp)
  nlohmann::json expected = nlohmann::json::parse(LoginProfile::BuildLoginProfileJson(pc));
  expected.erase("nevr_plugins");
  QCHECK(json.ToJson() == expected);
  QCHECK(At(expected, "accountid") == kNevrAccount);
  QCHECK(At(expected, "access_token") == "NEVR-TOKEN-SECRET");
  QCHECK(At(expected, "password") == "");
  for (const QuestLogin::Field& f : c.fields) {
    if (f.path == "password") QCHECK(f.text.empty());
  }
}

// The login declares the social message level the PCVR login declares, from the same shared
// constant, so the server sends friend presence, recently met, the lobby tablet and party data
// (each requires level 1 or more: nakama evr_friend_presence.go, evr_recently_met.go,
// evr_lobby_tablet.go, evr_pipeline_party_data.go). With social off the login carries 0.
void TestSocialLevelIsDeclaredAndCanBeTurnedOff() {
  auto level_of = [](const QuestLogin::BuildInfo& build) {
    const QuestLogin::Composition c = QuestLogin::Compose(MakeIdentity(), {}, build);
    for (const QuestLogin::Field& f : c.fields) {
      if (f.path == "nevr_social") return f.kind == FieldKind::Int ? f.number : std::int64_t{-1};
    }
    return std::int64_t{-2};  // member absent
  };
  QCHECK(SocialParty::kSocialLevel >= 1);
  QCHECK(level_of(MakeBuild()) == SocialParty::kSocialLevel);
  QuestLogin::BuildInfo off = MakeBuild();
  off.social_level = 0;
  QCHECK(level_of(off) == 0);

  // End to end through the rewrite: the member lands in the game's JSON, and the client-class
  // members next to it are still the game's.
  for (int level : {SocialParty::kSocialLevel, 0}) {
    FakeJson json;
    SeedOculusLogin(json);
    FakeUser user;
    FakeSource source;
    source.identity = MakeIdentity();
    QuestLogin::BuildInfo build = MakeBuild();
    build.social_level = level;
    QCHECK(QuestLogin::RewriteLogin(user, json, source, build, &CaptureLog) == QuestLogin::Outcome::Rewritten);
    const nlohmann::json doc = json.ToJson();
    QCHECK(At(doc, "nevr_social") == level);
    QCHECK(At(doc, "buildversion") == kQuestBuild);
    QCHECK(At(doc, "appid") == kQuestAppId);
  }
}

void TestSerialRelay() {
  auto serial_of = [](const std::string& game_serial) {
    QuestLogin::GameValues game;
    game.hmd_serial = game_serial;
    const QuestLogin::Composition c = QuestLogin::Compose(MakeIdentity(), game, MakeBuild());
    for (const QuestLogin::Field& f : c.fields) {
      if (f.path == "hmdserialnumber") return f.text;
    }
    return std::string("<absent>");
  };
  QCHECK(serial_of("1WMHH000000000") == "1WMHH000000000");  // the game's own value
  QCHECK(serial_of("N/A") == "N/A");                          // the game's no-VR value
  QCHECK(serial_of("") == "unknown");                         // nothing to relay
  QCHECK(serial_of("bad serial") == "unknown");               // a space is not a serial
  QCHECK(serial_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") == "ABCDEFGHIJKLMNOPQRSTUVWX");  // 24-byte buffer
}

void TestRewriteCarriesNevrIdentityToTheWire() {
  FakeJson json;
  SeedOculusLogin(json);
  FakeUser user;
  FakeSource source;
  source.identity = MakeIdentity();
  g_log.clear();

  std::uint64_t wire = 0;
  QCHECK(user.WireAccountId(wire) && wire == 5551234);  // before: the org-scoped id

  const QuestLogin::Outcome out = QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog);
  QCHECK(out == QuestLogin::Outcome::Rewritten);
  QCHECK(user.WireAccountId(wire) && wire == kNevrAccount);

  const nlohmann::json doc = json.ToJson();
  QCHECK(At(doc, "access_token") == "NEVR-TOKEN-SECRET");
  QCHECK(At(doc, "accountid") == kNevrAccount);
  QCHECK(At(doc, "nonce") == "");                          // the Oculus proof nonce is not relayed
  QCHECK(At(doc, "hmdserialnumber") == "1WMHH000000000");  // the game's value survives
  QCHECK(At(doc, "hmdproductname") == "Quest 2");          // a game-only key is left alone
  QCHECK(At(doc, "password") == "");                       // no credential in the login JSON
  QCHECK(At(doc, "displayname") == "Pilot");
  QCHECK(doc.dump().find("OCULUS") == std::string::npos);
  QCHECK(At(doc, "system_info|cpu") == "Snapdragon XR2");
  QCHECK(At(doc, "system_info|num_physical_cores") == 4);
  QCHECK(At(doc, "system_info|memory_total") == 5800);
  QCHECK(At(doc, "system_info|network_type") == "wifi");
  QCHECK(!At(doc, "system_info|driver_version").is_null());  // members the game left out are filled

  QCHECK(g_log.size() == 1);
  for (const std::string& line : g_log) {
    QCHECK(line.rfind("quest_login outcome=rewritten ", 0) == 0);
    QCHECK(line.find("platform=4") != std::string::npos);
    QCHECK(line.find("NEVR-TOKEN-SECRET") == std::string::npos);
    QCHECK(line.find("1WMHH000000000") == std::string::npos);
    QCHECK(line.find("987654321012345") == std::string::npos);
  }
}

// buildversion, appid, lobbyversion and publisher_lock say which client this is. The server
// classifies on them (IsPCVR() is buildversion != 630783; appid maps to a platform), so a
// Quest login must keep the game's values and must not gain the PCVR constants.
void TestClientClassKeysAreNeverOverwritten() {
  FakeJson json;
  SeedOculusLogin(json);
  FakeUser user;
  FakeSource source;
  source.identity = MakeIdentity();
  QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::Rewritten);
  const nlohmann::json doc = json.ToJson();
  QCHECK(At(doc, "buildversion") == kQuestBuild);
  QCHECK(At(doc, "appid") == kQuestAppId);
  QCHECK(At(doc, "lobbyversion") == kQuestLobbyVersion);
  QCHECK(At(doc, "publisher_lock") == "rad15_live");
  QCHECK(At(doc, "buildversion") != 631547);  // the PCVR constant

  // A key the game did not send is not invented either.
  for (const char* key : {"buildversion", "appid", "lobbyversion", "publisher_lock"}) {
    FakeJson partial;
    SeedOculusLogin(partial);
    partial.Clear(key);
    FakeUser u;
    QCHECK(QuestLogin::RewriteLogin(u, partial, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::Rewritten);
    QCHECK(partial.TypeOf(key) == JsonType::Absent);
  }
}

// A write that lands somewhere AccountID() never reads (the [this+0x88] field) must be caught:
// the wire id would still be the Oculus one.
void TestAccountIdThatDoesNotReachTheWireIsRejected() {
  FakeJson json;
  SeedOculusLogin(json);
  const nlohmann::json before = json.ToJson();
  FakeUser user;
  user.setter_reaches_global = false;
  FakeSource source;
  source.identity = MakeIdentity();
  QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) ==
         QuestLogin::Outcome::AccountIdNotCarried);
  QCHECK(json.ToJson() == before);
  std::uint64_t wire = 0;
  QCHECK(user.WireAccountId(wire) && wire == 5551234);
}

void TestPlatformMustAlreadyBeOvrOrg() {
  FakeJson json;
  SeedOculusLogin(json);
  const nlohmann::json before = json.ToJson();
  FakeUser user;
  user.provider = 5;
  FakeSource source;
  source.identity = MakeIdentity();
  QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) ==
         QuestLogin::Outcome::PlatformMismatch);
  QCHECK(json.ToJson() == before);
  QCHECK(user.global_account_id == 5551234);
}

void TestNoIdentityChangesNothing() {
  FakeJson json;
  SeedOculusLogin(json);
  const nlohmann::json before = json.ToJson();
  FakeUser user;
  FakeSource source;
  source.status = QuestLogin::IdentityStatus::NoToken;
  g_log.clear();
  QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::NoIdentity);
  QCHECK(json.ToJson() == before);
  QCHECK(user.global_account_id == 5551234);
  QCHECK(g_log.size() == 1 && g_log[0].find("outcome=no-identity reason=no-token") != std::string::npos);
}

// All-or-nothing: whichever single write the game refuses, the whole JSON and the account id
// are exactly what they were. Sweeps every path the rewrite writes.
void TestEveryRefusedWriteLeavesEverythingUntouched() {
  FakeJson probe;
  SeedOculusLogin(probe);
  FakeSource source;
  source.identity = MakeIdentity();
  const QuestLogin::GameValues game = QuestLogin::ReadGameValues(probe);
  const QuestLogin::Composition c = QuestLogin::Compose(source.identity, game, MakeBuild());
  QCHECK(c.fields.size() > 10);

  for (const QuestLogin::Field& field : c.fields) {
    if (QuestLogin::IsClientClassPath(field.path)) continue;  // never written
    FakeJson json;
    SeedOculusLogin(json);
    const nlohmann::json before = json.ToJson();
    json.refuse_path = field.path;
    FakeUser user;
    g_log.clear();
    const QuestLogin::Outcome out = QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog);
    if (out == QuestLogin::Outcome::Rewritten) {
      // A refusal is harmless only when the write would have changed nothing: the game's
      // measured value is kept, or the key already holds the value being written.
      const nlohmann::json current = At(before, field.path);
      const bool same_value = field.kind == FieldKind::String ? current == field.text : current == field.number;
      QCHECK(QuestLogin::IsGameMeasuredPath(field.path) || same_value);
      QCHECK(user.global_account_id == kNevrAccount);
    } else {
      QCHECK(out == QuestLogin::Outcome::JsonWriteFailed);
      if (json.ToJson() != before) std::fprintf(stderr, "state changed after refused path %s\n", field.path.c_str());
      QCHECK(json.ToJson() == before);
      QCHECK(user.global_account_id == 5551234);
      QCHECK(g_log.size() == 1 && g_log[0].find("first_path=") != std::string::npos);
    }
  }

  FakeJson json;
  SeedOculusLogin(json);
  const nlohmann::json before = json.ToJson();
  json.refuse_path = "access_token";
  FakeUser user;
  QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::JsonWriteFailed);
  QCHECK(json.ToJson() == before);
  QCHECK(user.global_account_id == 5551234);
}


// CJson holds more than strings, integers and booleans, and the rollback has to put each back.
void TestEveryJsonTypeSurvivesRollback() {
  FakeSource source;
  source.identity = MakeIdentity();

  // A real-typed hardware value the game filled is kept, not replaced and not removed.
  {
    FakeJson json;
    SeedOculusLogin(json);
    json.Clear("system_info|memory_total");
    json.SetReal("system_info|memory_total", 5800.5);
    const nlohmann::json before = json.ToJson();
    FakeUser user;
    QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::Rewritten);
    QCHECK(At(json.ToJson(), "system_info|memory_total") == 5800.5);
    QCHECK(json.TypeOf("system_info|memory_total") == JsonType::Real);
  }
  // A null the rewrite writes over comes back as a null, not as an absent key.
  {
    FakeJson json;
    SeedOculusLogin(json);
    json.SetNull("nevr_social");
    json.refuse_path = "system_info|dedicated_gpu_memory";  // fails after nevr_social was written
    const nlohmann::json before = json.ToJson();
    FakeUser user;
    QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::JsonWriteFailed);
    QCHECK(json.TypeOf("nevr_social") == JsonType::Null);
    QCHECK(json.ToJson() == before);
  }
  // A path the rewrite writes that holds a real, an array or a boolean-for-int is refused up
  // front: it could not be put back, and CJson would not change its type.
  for (int variant = 0; variant < 3; ++variant) {
    FakeJson json;
    SeedOculusLogin(json);
    if (variant == 0) json.SetReal("nevr_social", 1.5);
    json.int_overwrites_real = true;  // the real could not be put back if it were overwritten
    if (variant == 1) json.SetEmptyArray("nevr_social");
    if (variant == 2) json.SetBoolean("nevr_social", true);
    const nlohmann::json before = json.ToJson();
    FakeUser user;
    QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::JsonWriteFailed);
    QCHECK(json.ToJson() == before);
    QCHECK(user.global_account_id == 5551234);
  }
}

void TestNestedKeyAddedByRewriteIsRemovedOnRollback() {
  // The game's JSON has no nevr_identity object; a refusal late in the sweep must not leave
  // the empty object behind.
  FakeJson json;
  SeedOculusLogin(json);
  json.refuse_path = "system_info|dedicated_gpu_memory";  // written after the nevr_identity members
  const nlohmann::json before = json.ToJson();
  QCHECK(!before.contains("nevr_identity"));
  FakeUser user;
  FakeSource source;
  source.identity = MakeIdentity();
  QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::JsonWriteFailed);
  QCHECK(json.ToJson() == before);
}

// The id stays installed after the send: LogInSuccessCB compares {platform word, AccountID()}
// with the server's reply, and every later vtable+0x70 caller must agree.
struct SendProbe {
  FakeUser* user = nullptr;
  FakeJson* json = nullptr;
  int calls = 0;
  std::uint64_t wire_at_send = 0;
  nlohmann::json json_at_send;
};

void ProbeSend(void* context) {
  auto* probe = static_cast<SendProbe*>(context);
  ++probe->calls;
  probe->user->WireAccountId(probe->wire_at_send);
  probe->json_at_send = probe->json->ToJson();
}

void TestAccountIdStaysInstalledAfterSend() {
  FakeJson json;
  SeedOculusLogin(json);
  FakeUser user;
  FakeSource source;
  source.identity = MakeIdentity();
  SendProbe probe;
  probe.user = &user;
  probe.json = &json;
  QCHECK(QuestLogin::RewriteAndSend(user, json, source, MakeBuild(), &CaptureLog, &ProbeSend, &probe) ==
         QuestLogin::Outcome::Rewritten);
  QCHECK(probe.calls == 1);
  QCHECK(probe.wire_at_send == kNevrAccount);
  QCHECK(At(probe.json_at_send, "access_token") == "NEVR-TOKEN-SECRET");

  // What LogInSuccessCB does with the server's reply {OVR_ORG, NEVR id}.
  std::uint64_t later = 0;
  QCHECK(user.WireAccountId(later) && later == kNevrAccount);
  QCHECK(user.global_account_id == kNevrAccount);
  QCHECK(user.restores == 0);
}

void TestSendHappensOnceEvenWhenTheRewriteFails() {
  for (int variant = 0; variant < 3; ++variant) {
    FakeJson json;
    SeedOculusLogin(json);
    FakeUser user;
    FakeSource source;
    source.identity = MakeIdentity();
    if (variant == 0) source.status = QuestLogin::IdentityStatus::NotReady;
    if (variant == 1) user.provider = 5;
    if (variant == 2) json.refuse_path = "access_token";
    const nlohmann::json before = json.ToJson();
    SendProbe probe;
    probe.user = &user;
    probe.json = &json;
    QCHECK(QuestLogin::RewriteAndSend(user, json, source, MakeBuild(), &CaptureLog, &ProbeSend, &probe) !=
           QuestLogin::Outcome::Rewritten);
    QCHECK(probe.calls == 1);
    QCHECK(probe.json_at_send == before);
    QCHECK(probe.wire_at_send == 5551234);
  }
}

// CNSOVRUser::LogInInternal re-reads the Oculus org id only when the global holds -1, so after
// one Rewritten login the global keeps the NEVR id. A later login the rewrite declines sends
// the Oculus login unchanged, and AccountID() must then be the Oculus id again.
void TestDeclinedLoginAfterARewrittenOneRestoresTheOculusId() {
  const char* names[] = {"no-identity", "not-ready", "platform-mismatch", "compose-failed", "json-write-failed"};
  for (int variant = 0; variant < 5; ++variant) {
    FakeUser user;
    FakeSource source;
    source.identity = MakeIdentity();
    FakeJson first;
    SeedOculusLogin(first);
    QCHECK(QuestLogin::RewriteLogin(user, first, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::Rewritten);
    QCHECK(user.global_account_id == kNevrAccount);

    FakeJson second;
    SeedOculusLogin(second);
    const nlohmann::json before = second.ToJson();
    if (variant == 0) source.status = QuestLogin::IdentityStatus::NoToken;
    if (variant == 1) source.status = QuestLogin::IdentityStatus::NotReady;
    if (variant == 2) user.provider = 5;
    if (variant == 3) source.identity.display_name = "bad\xff\xfe";
    if (variant == 4) second.refuse_path = "access_token";
    SendProbe probe;
    probe.user = &user;
    probe.json = &second;
    const QuestLogin::Outcome out =
        QuestLogin::RewriteAndSend(user, second, source, MakeBuild(), &CaptureLog, &ProbeSend, &probe);
    if (out == QuestLogin::Outcome::Rewritten) std::fprintf(stderr, "variant %s was rewritten\n", names[variant]);
    QCHECK(out != QuestLogin::Outcome::Rewritten);
    QCHECK(probe.calls == 1);
    QCHECK(probe.json_at_send == before);                 // the Oculus login, untouched
    QCHECK(probe.wire_at_send == 5551234);                // carrying the Oculus account id
    QCHECK(user.global_account_id == 5551234);
  }
}

void TestSecondRewrittenLoginKeepsTheNevrIdAndALaterDeclineRestoresOculus() {
  FakeUser user;
  FakeSource source;
  source.identity = MakeIdentity();
  for (int attempt = 0; attempt < 2; ++attempt) {
    FakeJson json;
    SeedOculusLogin(json);
    QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::Rewritten);
    QCHECK(user.global_account_id == kNevrAccount);
  }
  source.status = QuestLogin::IdentityStatus::NotReady;
  FakeJson json;
  SeedOculusLogin(json);
  QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::NoIdentity);
  QCHECK(user.global_account_id == 5551234);

  // A new Oculus id written by the game in between is the one remembered.
  user.global_account_id = 777;
  source.status = QuestLogin::IdentityStatus::Ok;
  FakeJson again;
  SeedOculusLogin(again);
  QCHECK(QuestLogin::RewriteLogin(user, again, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::Rewritten);
  source.status = QuestLogin::IdentityStatus::NoToken;
  FakeJson last;
  SeedOculusLogin(last);
  QCHECK(QuestLogin::RewriteLogin(user, last, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::NoIdentity);
  QCHECK(user.global_account_id == 777);
}

// A top-level member the profile nests under can exist and not be an object. The real CJson
// refuses a nested write under it ("$ json path: %s is not an object.", libpnsovr.so string
// 0x5825b1) and writes nothing; the fake does the same by default. The rewrite's own parent
// check is defensive on top of that (the refused write would fail the read-back and the
// rollback would find nothing to undo), so this default-model test documents real behaviour but
// does not need the check.
void TestParentThatIsNotAnObjectIsLeftAlone() {
  for (const char* top : {"nevr_identity", "system_info"}) {
    FakeJson json;
    SeedOculusLogin(json);
    json.Clear(top);
    json.SetString(top, "game-value");
    const nlohmann::json before = json.ToJson();
    FakeUser user;
    FakeSource source;
    source.identity = MakeIdentity();
    QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::JsonWriteFailed);
    QCHECK(json.ToJson() == before);
    QCHECK(At(json.ToJson(), top) == "game-value");
    QCHECK(user.global_account_id == 5551234);
  }
}

// Defensive case only: a build whose nested write would turn a non-object parent into an object.
// The game does not do this (see above); the parent check is what keeps such a build from losing
// the game's value, and this test is the only one that exercises the check.
void TestParentCheckIsDefensiveAgainstAnOverwritingBuild() {
  for (const char* top : {"nevr_identity", "system_info"}) {
    FakeJson json;
    SeedOculusLogin(json);
    json.Clear(top);
    json.SetString(top, "game-value");
    json.nested_write_replaces_parent = true;  // a build whose nested write would overwrite the value
    const nlohmann::json before = json.ToJson();
    FakeUser user;
    FakeSource source;
    source.identity = MakeIdentity();
    QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::JsonWriteFailed);
    QCHECK(json.ToJson() == before);
    QCHECK(At(json.ToJson(), top) == "game-value");
    QCHECK(user.global_account_id == 5551234);
  }
}





// AccountIdNotCarried after the global was really written: the restore must put it back.
void TestAccountIdNotCarriedRestoresTheWrittenGlobal() {
  FakeJson json;
  SeedOculusLogin(json);
  const nlohmann::json before = json.ToJson();
  FakeUser user;
  user.wire_forced = true;  // AccountID() answers something else than the global
  user.forced_wire = 4242;
  FakeSource source;
  source.identity = MakeIdentity();
  g_log.clear();
  QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) ==
         QuestLogin::Outcome::AccountIdNotCarried);
  QCHECK(user.global_account_id == 5551234);  // written, then put back
  QCHECK(json.ToJson() == before);
  QCHECK(g_log.size() == 1 && g_log[0].find("restored=1") != std::string::npos);
}

// The id is put back only while the global still holds what the rewrite wrote, and only a real
// id is ever remembered.
void TestRestoreLeavesAGameWriteAlone() {
  FakeUser user;
  FakeSource source;
  source.identity = MakeIdentity();
  FakeJson first;
  SeedOculusLogin(first);
  QCHECK(QuestLogin::RewriteLogin(user, first, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::Rewritten);
  user.global_account_id = 2222;  // the game stored a new id
  source.status = QuestLogin::IdentityStatus::NotReady;
  FakeJson second;
  SeedOculusLogin(second);
  g_log.clear();
  QCHECK(QuestLogin::RewriteLogin(user, second, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::NoIdentity);
  QCHECK(user.global_account_id == 2222);
  QCHECK(g_log.size() == 1 && g_log[0].find("restored=0") != std::string::npos);
}

void TestNoRealIdToRememberRefusesTheRewrite() {
  for (std::uint64_t value : {std::uint64_t{0}, ~std::uint64_t{0}}) {
    FakeJson json;
    SeedOculusLogin(json);
    const nlohmann::json before = json.ToJson();
    FakeUser user;
    user.global_account_id = value;  // RadPluginShutdown wrote 0, or the "fetch again" marker -1
    FakeSource source;
    source.identity = MakeIdentity();
    QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) ==
           QuestLogin::Outcome::AccountIdNotCarried);
    QCHECK(user.global_account_id == value);
    QCHECK(json.ToJson() == before);
  }
  // A shutdown that zeroes the global after a rewrite: the next rewrite is refused too.
  FakeUser user;
  FakeSource source;
  source.identity = MakeIdentity();
  FakeJson first;
  SeedOculusLogin(first);
  QCHECK(QuestLogin::RewriteLogin(user, first, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::Rewritten);
  user.global_account_id = 0;
  FakeJson second;
  SeedOculusLogin(second);
  const nlohmann::json before = second.ToJson();
  QCHECK(QuestLogin::RewriteLogin(user, second, source, MakeBuild(), &CaptureLog) ==
         QuestLogin::Outcome::AccountIdNotCarried);
  QCHECK(user.global_account_id == 0);
  QCHECK(second.ToJson() == before);
}

// The record says whether the Oculus id was put back, without values.
void TestDeclineRecordSaysWhetherTheOculusIdWasRestored() {
  FakeUser user;
  FakeSource source;
  source.status = QuestLogin::IdentityStatus::NoToken;
  FakeJson never;
  SeedOculusLogin(never);
  g_log.clear();
  QuestLogin::RewriteLogin(user, never, source, MakeBuild(), &CaptureLog);
  QCHECK(g_log.size() == 1 && g_log[0].find("restored=0") != std::string::npos);

  source.status = QuestLogin::IdentityStatus::Ok;
  source.identity = MakeIdentity();
  FakeJson ok;
  SeedOculusLogin(ok);
  QuestLogin::RewriteLogin(user, ok, source, MakeBuild(), &CaptureLog);
  source.status = QuestLogin::IdentityStatus::NoToken;
  FakeJson after;
  SeedOculusLogin(after);
  g_log.clear();
  QuestLogin::RewriteLogin(user, after, source, MakeBuild(), &CaptureLog);
  QCHECK(g_log.size() == 1 && g_log[0].find("restored=1") != std::string::npos);
  QCHECK(g_log[0].find("5551234") == std::string::npos && g_log[0].find("987654321012345") == std::string::npos);
}

// The compose phase is the only part that runs exceptions-enabled code; an exception from the
// identity source becomes an outcome, with nothing changed and the account id at the Oculus value.
class ThrowingSource final : public QuestLogin::IdentitySource {
 public:
  QuestLogin::IdentityStatus Fetch(QuestLogin::Identity&) override { throw std::runtime_error("source"); }
};

void TestComposeFailureBecomesAnOutcome() {
  FakeJson json;
  SeedOculusLogin(json);
  const nlohmann::json before = json.ToJson();
  FakeUser user;
  ThrowingSource source;
  g_log.clear();
  QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::Exception);
  QCHECK(json.ToJson() == before);
  QCHECK(user.global_account_id == 5551234);
  QCHECK(g_log.size() == 1 && g_log[0].find("outcome=exception") != std::string::npos);

  // After a rewritten login the same failure still puts the Oculus id back.
  FakeSource ok;
  ok.identity = MakeIdentity();
  FakeJson first;
  SeedOculusLogin(first);
  QCHECK(QuestLogin::RewriteLogin(user, first, ok, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::Rewritten);
  FakeJson second;
  SeedOculusLogin(second);
  QCHECK(QuestLogin::RewriteLogin(user, second, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::Exception);
  QCHECK(user.global_account_id == 5551234);
}

// The compose phase keeps game-measured members using only what Observe recorded, so every
// system_info member the profile can produce has to be one Observe looks at.
void TestEveryComposedMeasuredPathIsObserved() {
  const QuestLogin::Composition c = QuestLogin::Compose(MakeIdentity(), {}, MakeBuild());
  QCHECK(c.status == QuestLogin::ComposeStatus::Ok);
  std::size_t measured = 0;
  for (const QuestLogin::Field& f : c.fields) {
    if (!QuestLogin::IsGameMeasuredPath(f.path)) continue;
    ++measured;
    bool known = false;
    for (const char* path : QuestLogin::kMeasuredPaths) known = known || f.path == path;
    QCHECK(known);
  }
  QCHECK(measured == QuestLogin::kMeasuredCount);
}

void TestInvalidUtf8NameDoesNotThrow() {
  QuestLogin::Identity id = MakeIdentity();
  id.display_name = "bad\xff\xfe";
  const QuestLogin::Composition c = QuestLogin::Compose(id, {}, MakeBuild());
  QCHECK(c.status == QuestLogin::ComposeStatus::ProfileBuildFailed);
  QCHECK(c.fields.empty());
  FakeJson json;
  SeedOculusLogin(json);
  const nlohmann::json before = json.ToJson();
  FakeUser user;
  FakeSource source;
  source.identity = id;
  QCHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::ComposeFailed);
  QCHECK(json.ToJson() == before);
}

}  // namespace

int main() {
  TestComposeFailsClosed();
  TestComposedProfileMatchesPcvrBuilder();
  TestSerialRelay();
  TestSocialLevelIsDeclaredAndCanBeTurnedOff();
  TestRewriteCarriesNevrIdentityToTheWire();
  TestClientClassKeysAreNeverOverwritten();
  TestAccountIdThatDoesNotReachTheWireIsRejected();
  TestPlatformMustAlreadyBeOvrOrg();
  TestNoIdentityChangesNothing();
  TestEveryRefusedWriteLeavesEverythingUntouched();
  TestEveryJsonTypeSurvivesRollback();
  TestNestedKeyAddedByRewriteIsRemovedOnRollback();
  TestAccountIdStaysInstalledAfterSend();
  TestSendHappensOnceEvenWhenTheRewriteFails();
  TestDeclinedLoginAfterARewrittenOneRestoresTheOculusId();
  TestSecondRewrittenLoginKeepsTheNevrIdAndALaterDeclineRestoresOculus();
  TestParentThatIsNotAnObjectIsLeftAlone();
  TestParentCheckIsDefensiveAgainstAnOverwritingBuild();
  TestComposeFailureBecomesAnOutcome();
  TestEveryComposedMeasuredPathIsObserved();
  TestAccountIdNotCarriedRestoresTheWrittenGlobal();
  TestRestoreLeavesAGameWriteAlone();
  TestNoRealIdToRememberRefusesTheRewrite();
  TestDeclineRecordSaysWhetherTheOculusIdWasRestored();
  TestInvalidUtf8NameDoesNotThrow();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_rewrite_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_rewrite_test: all checks passed\n");
  return 0;
}
