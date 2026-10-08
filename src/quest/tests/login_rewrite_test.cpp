// Host-buildable test for the Quest login rewrite (src/quest/login/login_rewrite.cpp).
// Compiles the same sources the device links, plus the shared PCVR login builder
// (src/runtime/compat/login_profile.cpp), against a fake CJson that mimics the game's
// refusal to change a key's type.

#include "quest/login/login_rewrite.h"

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/compat/login_profile.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

using QuestLogin::FieldKind;

// A CJson stand-in. Like the game's, a write that would change a key's type is refused, and
// clearing a nested key leaves its (possibly empty) parent object behind.
class FakeJson final : public QuestLogin::JsonAccess {
 public:
  struct Value {
    FieldKind kind = FieldKind::String;
    std::string text;
    std::int64_t number = 0;
  };
  std::map<std::string, Value> values;
  std::set<std::string> objects;  // top-level objects that exist, even when empty
  std::string refuse_path;        // writes to this path are dropped, like a refused write
  int throw_after = -1;           // throw std::bad_alloc on the Nth mutating call (0-based); -1 never
  int mutations = 0;

  void SetString(const char* path, const char* value) override { Put(path, FieldKind::String, value, 0); }
  void SetInt(const char* path, std::int64_t value) override { Put(path, FieldKind::Int, "", value); }
  void SetBoolean(const char* path, bool value) override { Put(path, FieldKind::Boolean, "", value ? 1 : 0); }

  void Clear(const char* path) override {
    Tick();
    const std::string key = path;
    if (objects.erase(key) != 0) {
      for (auto it = values.begin(); it != values.end();) {
        it = it->first.rfind(key + "|", 0) == 0 ? values.erase(it) : std::next(it);
      }
    }
    values.erase(key);
  }

  std::string GetString(const char* path, bool& present) const override {
    auto it = values.find(path);
    present = it != values.end() && it->second.kind == FieldKind::String;
    return present ? it->second.text : std::string();
  }
  std::int64_t GetInt(const char* path, bool& present) const override {
    auto it = values.find(path);
    present = it != values.end() && it->second.kind == FieldKind::Int;
    return present ? it->second.number : 0;
  }
  bool GetBoolean(const char* path, bool& present) const override {
    auto it = values.find(path);
    present = it != values.end() && it->second.kind == FieldKind::Boolean;
    return present && it->second.number != 0;
  }
  bool IsObject(const char* path) const override { return objects.count(path) != 0; }

  // Rebuilds the nested document the keys describe ('|' nesting), empty objects included.
  nlohmann::json ToJson() const {
    nlohmann::json doc = nlohmann::json::object();
    for (const std::string& name : objects) doc[name] = nlohmann::json::object();
    for (const auto& [path, value] : values) {
      nlohmann::json* node = &doc;
      std::size_t start = 0;
      for (;;) {
        const std::size_t bar = path.find('|', start);
        if (bar == std::string::npos) {
          const std::string leaf = path.substr(start);
          if (value.kind == FieldKind::String) (*node)[leaf] = value.text;
          else if (value.kind == FieldKind::Int) (*node)[leaf] = value.number;
          else (*node)[leaf] = value.number != 0;
          break;
        }
        node = &(*node)[path.substr(start, bar - start)];
        start = bar + 1;
      }
    }
    return doc;
  }

 private:
  void Tick() {
    if (throw_after >= 0 && mutations++ == throw_after) throw std::bad_alloc();
  }
  void Put(const char* path, FieldKind kind, const char* text, std::int64_t number) {
    Tick();
    if (refuse_path == path) return;
    auto it = values.find(path);
    if (it != values.end() && it->second.kind != kind) return;  // type change refused
    const std::string key = path;
    const std::size_t bar = key.find('|');
    if (bar != std::string::npos) objects.insert(key.substr(0, bar));
    Value value;
    value.kind = kind;
    value.text = text;
    value.number = number;
    values[key] = value;
  }
};

// The CNSOVRUser as measured: provider already 4, and the account id the sender uses comes
// from a virtual AccountID() that returns a process global, never from the object.
class FakeUser final : public QuestLogin::UserAccess {
 public:
  std::uint64_t provider = QuestLogin::kPlatformOvrOrg;
  std::uint64_t global_account_id = 5551234;  // what AccountID() returns (the OrgScopedID global)
  std::uint64_t object_account_field = 5551234;  // [this+0x88]: ignored by the override
  bool setter_reaches_global = true;  // false models a write that AccountID() never sees
  int restores = 0;

  bool Provider(std::uint64_t& code) const override {
    code = provider;
    return true;
  }
  bool WireAccountId(std::uint64_t& id) const override {
    id = global_account_id;  // the virtual call; object_account_field is not consulted
    return true;
  }
  bool SetAccountId(std::uint64_t id) override {
    previous_ = global_account_id;
    if (setter_reaches_global) global_account_id = id;
    else object_account_field = id;  // the old, wrong target ([this+0x88])
    return true;
  }
  void RestoreAccountId() override {
    global_account_id = previous_;
    ++restores;
  }

 private:
  std::uint64_t previous_ = 0;
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

std::vector<std::string> g_log;
void CaptureLog(QuestLogin::Level, const char* line) { g_log.emplace_back(line); }

constexpr std::uint64_t kNevrAccount = 987654321012345ULL;

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

// What the game's own login JSON looks like when the Oculus path built it
// (libpnsovr GotUserProofCB, CNSUser::SystemInfo and libr15 CR15NetGame::LogIn key names).
void SeedOculusLogin(FakeJson& json) {
  json.SetInt("appid", 1234);
  json.SetInt("accountid", 5551234);
  json.SetString("access_token", "OCULUS-ACCESS-TOKEN");
  json.SetString("nonce", "OCULUS-NONCE");
  json.SetInt("lobbyversion", 0x3f69c77a);
  json.SetInt("buildversion", 1111);
  json.SetString("publisher_lock", "rad15_live");
  json.SetString("hmdserialnumber", "1WMHH000000000");
  json.SetString("hmdproductname", "Quest 2");
  json.SetString("system_info|headset_type", "Quest 2");
  json.SetString("system_info|build_version", "QuestOS-1");
  json.SetString("system_info|cpu", "Snapdragon XR2");
  json.SetInt("system_info|num_physical_cores", 4);
  json.SetInt("system_info|num_logical_cores", 8);
  json.SetInt("system_info|memory_total", 5800);
  json.SetInt("system_info|memory_used", 3100);
  json.SetString("system_info|network_type", "wifi");
}

void TestComposeFailsClosed() {
  QuestLogin::Identity id = MakeIdentity();
  id.account_id = 0;
  CHECK(QuestLogin::Compose(id, {}, MakeBuild()).status == QuestLogin::ComposeStatus::MissingAccountId);
  id = MakeIdentity();
  id.account_id = 0x8000000000000000ULL;  // does not fit CJson's signed integer
  CHECK(QuestLogin::Compose(id, {}, MakeBuild()).status == QuestLogin::ComposeStatus::MissingAccountId);
  id = MakeIdentity();
  id.access_token.clear();
  const QuestLogin::Composition c = QuestLogin::Compose(id, {}, MakeBuild());
  CHECK(c.status == QuestLogin::ComposeStatus::MissingToken);
  CHECK(c.fields.empty());
}

// The Quest login carries what the PCVR builder produces for the same inputs, minus the
// empty plugin array (CJson has no setter for it) and minus the password: the server reads
// it from the upgrade URL, and the game's own log of the outgoing login does not redact it.
void TestSameProfileAsPcvrWithoutPassword() {
  const QuestLogin::Identity id = MakeIdentity();
  QuestLogin::GameValues game;
  game.hmd_serial = "1WMHH000000000";
  game.headset_type = "Quest 2";
  const QuestLogin::Composition c = QuestLogin::Compose(id, game, MakeBuild());
  CHECK(c.status == QuestLogin::ComposeStatus::Ok);
  CHECK(c.skipped.size() == 1 && c.skipped[0] == "nevr_plugins");

  FakeJson json;
  std::string failed;
  CHECK(QuestLogin::ApplyFieldsAtomically(c.fields, json, failed));

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
  nlohmann::json expected = nlohmann::json::parse(LoginProfile::BuildLoginProfileJson(pc));
  expected.erase("nevr_plugins");
  CHECK(json.ToJson() == expected);
  CHECK(expected.at("accountid") == kNevrAccount);
  CHECK(expected.at("access_token") == "NEVR-TOKEN-SECRET");
  CHECK(expected.at("password") == "");
  // No composed field carries a password value.
  for (const QuestLogin::Field& f : c.fields) {
    if (f.path == "password") CHECK(f.text.empty());
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
  CHECK(serial_of("1WMHH000000000") == "1WMHH000000000");  // the game's own value
  CHECK(serial_of("N/A") == "N/A");                          // the game's no-VR value
  CHECK(serial_of("") == "unknown");                         // nothing to relay
  CHECK(serial_of("bad serial") == "unknown");               // a space is not a serial
  CHECK(serial_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") == "ABCDEFGHIJKLMNOPQRSTUVWX");  // 24-byte buffer
}

void TestRewriteCarriesNevrIdentityToTheWire() {
  FakeJson json;
  SeedOculusLogin(json);
  FakeUser user;
  FakeSource source;
  source.identity = MakeIdentity();
  g_log.clear();

  // Before: the wire account id is the Oculus org-scoped id.
  std::uint64_t wire = 0;
  CHECK(user.WireAccountId(wire) && wire == 5551234);

  const QuestLogin::Outcome out = QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog);
  CHECK(out == QuestLogin::Outcome::Rewritten);
  CHECK(user.WireAccountId(wire) && wire == kNevrAccount);  // what AccountID() now returns

  const nlohmann::json doc = json.ToJson();
  CHECK(doc.at("access_token") == "NEVR-TOKEN-SECRET");
  CHECK(doc.at("accountid") == kNevrAccount);
  CHECK(doc.at("nonce") == "");                          // the Oculus proof nonce is not relayed
  CHECK(doc.at("hmdserialnumber") == "1WMHH000000000");  // the game's value survives
  CHECK(doc.at("hmdproductname") == "Quest 2");          // a game-only key is left alone
  CHECK(doc.at("password") == "");                       // no credential in the login JSON
  CHECK(doc.dump().find("OCULUS") == std::string::npos);
  // The headset's real measurements are kept, not replaced by the PCVR placeholders.
  const nlohmann::json& sys = doc.at("system_info");
  CHECK(sys.at("cpu") == "Snapdragon XR2");
  CHECK(sys.at("num_physical_cores") == 4);
  CHECK(sys.at("num_logical_cores") == 8);
  CHECK(sys.at("memory_total") == 5800);
  CHECK(sys.at("memory_used") == 3100);
  CHECK(sys.at("network_type") == "wifi");
  // Members the game left out are filled from the profile.
  CHECK(sys.contains("driver_version"));

  CHECK(g_log.size() == 1);
  for (const std::string& line : g_log) {
    CHECK(line.rfind("event=quest_login outcome=rewritten ", 0) == 0);
    CHECK(line.find("platform=4") != std::string::npos);
    CHECK(line.find("NEVR-TOKEN-SECRET") == std::string::npos);
    CHECK(line.find("1WMHH000000000") == std::string::npos);
    CHECK(line.find("987654321012345") == std::string::npos);
    CHECK(line.find("(0,0)") == std::string::npos);
  }
}

// The reviewer's trap: a change that lands in [this+0x88] (or anything AccountID() does not
// read) must be caught, because the wire id would still be the Oculus one.
void TestAccountIdThatDoesNotReachTheWireIsRejected() {
  FakeJson json;
  SeedOculusLogin(json);
  const nlohmann::json before = json.ToJson();
  FakeUser user;
  user.setter_reaches_global = false;
  FakeSource source;
  source.identity = MakeIdentity();
  CHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) ==
        QuestLogin::Outcome::AccountIdNotCarried);
  CHECK(json.ToJson() == before);
  std::uint64_t wire = 0;
  CHECK(user.WireAccountId(wire) && wire == 5551234);
}

void TestPlatformMustAlreadyBeOvrOrg() {
  FakeJson json;
  SeedOculusLogin(json);
  const nlohmann::json before = json.ToJson();
  FakeUser user;
  user.provider = 5;
  FakeSource source;
  source.identity = MakeIdentity();
  CHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) ==
        QuestLogin::Outcome::PlatformMismatch);
  CHECK(json.ToJson() == before);
  CHECK(user.global_account_id == 5551234);
}

void TestNoIdentityChangesNothing() {
  FakeJson json;
  SeedOculusLogin(json);
  const nlohmann::json before = json.ToJson();
  FakeUser user;
  FakeSource source;
  source.status = QuestLogin::IdentityStatus::NoToken;
  g_log.clear();
  CHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::NoIdentity);
  CHECK(json.ToJson() == before);
  CHECK(user.global_account_id == 5551234);
  CHECK(g_log.size() == 1 && g_log[0].find("outcome=no-identity reason=no-token") != std::string::npos);
}

// All-or-nothing: whichever single write the game refuses, the whole JSON and the account id
// are exactly what they were. Sweeps every path the rewrite writes.
void TestEveryRefusedWriteLeavesEverythingUntouched() {
  FakeJson probe;
  SeedOculusLogin(probe);
  FakeUser probe_user;
  FakeSource source;
  source.identity = MakeIdentity();
  QuestLogin::GameValues game = QuestLogin::ReadGameValues(probe);
  const QuestLogin::Composition c = QuestLogin::Compose(source.identity, game, MakeBuild());
  CHECK(c.fields.size() > 10);

  for (const QuestLogin::Field& field : c.fields) {
    FakeJson json;
    SeedOculusLogin(json);
    const nlohmann::json before = json.ToJson();
    json.refuse_path = field.path;
    FakeUser user;
    g_log.clear();
    const QuestLogin::Outcome out = QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog);
    // A refused write to a key the game owns (already present with the same value) is not a
    // failure; everything else is, and in both cases the state is consistent.
    if (out == QuestLogin::Outcome::Rewritten) {
      CHECK(json.ToJson().at("access_token") == "NEVR-TOKEN-SECRET");
      CHECK(user.global_account_id == kNevrAccount);
    } else {
      CHECK(out == QuestLogin::Outcome::JsonWriteFailed);
      if (json.ToJson() != before) std::fprintf(stderr, "state changed after refused path %s\n", field.path.c_str());
      CHECK(json.ToJson() == before);
      CHECK(user.global_account_id == 5551234);
      CHECK(g_log.size() == 1 && g_log[0].find("first_path=") != std::string::npos);
    }
  }

  // The token itself cannot be written: the reviewer's probe.
  FakeJson json;
  SeedOculusLogin(json);
  const nlohmann::json before = json.ToJson();
  json.refuse_path = "access_token";
  FakeUser user;
  CHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::JsonWriteFailed);
  CHECK(json.ToJson() == before);
  CHECK(user.global_account_id == 5551234);
}

// An exception at any point (an allocation failing mid-rewrite) also leaves nothing behind.
void TestExceptionAnywhereRestoresEverything() {
  FakeSource source;
  source.identity = MakeIdentity();
  bool saw_exception_outcome = false;
  for (int n = 0; n < 200; ++n) {
    FakeJson json;
    SeedOculusLogin(json);
    const nlohmann::json before = json.ToJson();
    json.throw_after = n;
    FakeUser user;
    const QuestLogin::Outcome out = QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog);
    json.throw_after = -1;
    if (out == QuestLogin::Outcome::Rewritten) {
      CHECK(user.global_account_id == kNevrAccount);
      break;  // n is past the last mutating call
    }
    saw_exception_outcome = true;
    CHECK(json.ToJson() == before);
    CHECK(user.global_account_id == 5551234);
  }
  CHECK(saw_exception_outcome);
}

void TestTypeConflictWritesNothing() {
  // The game's "accountid" is an integer; a build that kept it as a string would refuse the
  // write, so nothing at all is written.
  FakeJson json;
  SeedOculusLogin(json);
  json.values.erase("accountid");
  json.SetString("accountid", "5551234");
  const nlohmann::json before = json.ToJson();
  FakeUser user;
  FakeSource source;
  source.identity = MakeIdentity();
  CHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::JsonWriteFailed);
  CHECK(json.ToJson() == before);
  CHECK(user.global_account_id == 5551234);
}

void TestNestedKeyAddedByRewriteIsRemovedOnRollback() {
  // The game's JSON has no nevr_identity object; a refusal late in the sweep must not leave
  // the empty object behind.
  FakeJson json;
  SeedOculusLogin(json);
  json.refuse_path = "system_info|dedicated_gpu_memory";  // written after nevr_identity members
  const nlohmann::json before = json.ToJson();
  CHECK(!before.contains("nevr_identity"));
  FakeUser user;
  FakeSource source;
  source.identity = MakeIdentity();
  CHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::JsonWriteFailed);
  CHECK(json.ToJson() == before);
}

void TestInvalidUtf8NameDoesNotThrow() {
  QuestLogin::Identity id = MakeIdentity();
  id.display_name = "bad\xff\xfe";
  const QuestLogin::Composition c = QuestLogin::Compose(id, {}, MakeBuild());
  CHECK(c.status == QuestLogin::ComposeStatus::ProfileBuildFailed);
  CHECK(c.fields.empty());
  FakeJson json;
  SeedOculusLogin(json);
  const nlohmann::json before = json.ToJson();
  FakeUser user;
  FakeSource source;
  source.identity = id;
  CHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::ComposeFailed);
  CHECK(json.ToJson() == before);
}

}  // namespace

int main() {
  TestComposeFailsClosed();
  TestSameProfileAsPcvrWithoutPassword();
  TestSerialRelay();
  TestRewriteCarriesNevrIdentityToTheWire();
  TestAccountIdThatDoesNotReachTheWireIsRejected();
  TestPlatformMustAlreadyBeOvrOrg();
  TestNoIdentityChangesNothing();
  TestEveryRefusedWriteLeavesEverythingUntouched();
  TestExceptionAnywhereRestoresEverything();
  TestTypeConflictWritesNothing();
  TestNestedKeyAddedByRewriteIsRemovedOnRollback();
  TestInvalidUtf8NameDoesNotThrow();
  if (g_failures != 0) {
    std::fprintf(stderr, "login_rewrite_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("login_rewrite_test: all checks passed\n");
  return 0;
}
