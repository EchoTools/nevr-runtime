// Host-buildable test for the Quest login rewrite (src/quest/login/login_rewrite.cpp).
// Compiles the same sources the device links, plus the shared PCVR login builder
// (src/runtime/compat/login_profile.cpp), against a fake CJson that mimics the game's
// refusal to change a key's type.

#include "quest/login/login_rewrite.h"

#include <cstdint>
#include <cstdio>
#include <map>
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

// A CJson stand-in. Like the game's, a write that would change a key's type is refused.
class FakeJson final : public QuestLogin::JsonAccess {
 public:
  struct Value {
    FieldKind kind = FieldKind::String;
    std::string text;
    std::int64_t number = 0;
  };
  std::map<std::string, Value> values;
  std::string refuse_path;  // writes to this path are dropped, like a refused write

  void SetString(const char* path, const char* value) override {
    Put(path, FieldKind::String, value, 0);
  }
  void SetInt(const char* path, std::int64_t value) override { Put(path, FieldKind::Int, "", value); }
  void SetBoolean(const char* path, bool value) override { Put(path, FieldKind::Boolean, "", value ? 1 : 0); }

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

  // Rebuilds the nested document the fields describe ('|' nesting).
  nlohmann::json ToJson() const {
    nlohmann::json doc = nlohmann::json::object();
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
  void Put(const char* path, FieldKind kind, const char* text, std::int64_t number) {
    if (refuse_path == path) return;
    auto it = values.find(path);
    if (it != values.end() && it->second.kind != kind) return;  // type change refused
    Value value;
    value.kind = kind;
    value.text = text;
    value.number = number;
    values[path] = value;
  }
};

class FakeUser final : public QuestLogin::UserAccess {
 public:
  QuestLogin::UserIdWords words;
  bool readable = true;
  bool writable = true;
  int writes = 0;
  bool Read(QuestLogin::UserIdWords& out) const override {
    if (!readable) return false;
    out = words;
    return true;
  }
  bool Write(const QuestLogin::UserIdWords& next) override {
    if (!writable) return false;
    words = next;
    ++writes;
    return true;
  }
};

class FakeSource final : public QuestLogin::IdentitySource {
 public:
  bool available = true;
  QuestLogin::Identity identity;
  bool Fetch(QuestLogin::Identity& out, std::string& reason) override {
    if (!available) {
      reason = "no-token";
      return false;
    }
    out = identity;
    return true;
  }
};

std::vector<std::string> g_log;
void CaptureLog(QuestLogin::Level, const char* line) { g_log.emplace_back(line); }

QuestLogin::Identity MakeIdentity() {
  QuestLogin::Identity id;
  id.account_id = 987654321012345ULL;
  id.display_name = "Pilot";
  id.access_token = "NEVR-TOKEN-SECRET";
  id.password = "PW-SECRET";
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
// (libpnsovr GotUserProofCB and libr15 CR15NetGame::LogIn key names).
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
}

void TestUserIdRewrite() {
  QuestLogin::UserIdWords before;
  before.platform_word = 0xAABBCCDD00000005ULL;  // provider 5 (OVR) plus high bits
  before.account_id = 5551234;
  const QuestLogin::UserIdWords after = QuestLogin::RewriteUserId(before, MakeIdentity());
  CHECK(after.platform_word == 0xAABBCCDD00000004ULL);
  CHECK(after.account_id == 987654321012345ULL);
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

// The Quest login carries exactly what the PCVR builder produces for the same inputs; the
// only member absent is the empty plugin array, which CJson has no setter for.
void TestSameProfileAsPcvr() {
  const QuestLogin::Identity id = MakeIdentity();
  QuestLogin::GameValues game;
  game.hmd_serial = "1WMHH000000000";
  game.headset_type = "Quest 2";
  const QuestLogin::Composition c = QuestLogin::Compose(id, game, MakeBuild());
  CHECK(c.status == QuestLogin::ComposeStatus::Ok);
  CHECK(c.skipped.size() == 1 && c.skipped[0] == "nevr_plugins");

  FakeJson json;
  CHECK(QuestLogin::ApplyFields(c.fields, json).empty());

  LoginProfile::LoginProfileInputs pc;
  pc.account_id = id.account_id;
  pc.display_name = id.display_name;
  pc.access_token = id.access_token;
  pc.password = id.password;
  pc.hmd_serial_number = "1WMHH000000000";
  pc.headset_type = "Quest 2";
  pc.project_version = "1.2.3";
  pc.git_commit = "abc123";
  pc.git_describe = "v1.2.3-4-gabc123";
  pc.build_type = "Release";
  nlohmann::json expected = nlohmann::json::parse(LoginProfile::BuildLoginProfileJson(pc));
  expected.erase("nevr_plugins");
  CHECK(json.ToJson() == expected);
  CHECK(expected.at("accountid") == 987654321012345ULL);
  CHECK(expected.at("access_token") == "NEVR-TOKEN-SECRET");
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

void TestRewriteLoginReplacesOculusIdentity() {
  FakeJson json;
  SeedOculusLogin(json);
  FakeUser user;
  user.words.platform_word = 0x5;
  user.words.account_id = 5551234;
  FakeSource source;
  source.identity = MakeIdentity();
  g_log.clear();

  const QuestLogin::Outcome out = QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog);
  CHECK(out == QuestLogin::Outcome::Rewritten);
  const nlohmann::json doc = json.ToJson();
  CHECK(doc.at("access_token") == "NEVR-TOKEN-SECRET");
  CHECK(doc.at("accountid") == 987654321012345ULL);
  CHECK(doc.at("nonce") == "");                       // the Oculus proof nonce is not relayed
  CHECK(doc.at("hmdserialnumber") == "1WMHH000000000");  // the game's value survives
  CHECK(doc.at("system_info").at("headset_type") == "Quest 2");
  CHECK(doc.at("hmdproductname") == "Quest 2");       // a game-only key is left alone
  CHECK(doc.dump().find("OCULUS") == std::string::npos);
  CHECK(user.words.platform_word == 0x4);
  CHECK(user.words.account_id == 987654321012345ULL);

  // One Info line, and no line carries a secret or identifier value.
  CHECK(g_log.size() == 1);
  for (const std::string& line : g_log) {
    CHECK(line.find("NEVR-TOKEN-SECRET") == std::string::npos);
    CHECK(line.find("PW-SECRET") == std::string::npos);
    CHECK(line.find("1WMHH000000000") == std::string::npos);
    CHECK(line.find("987654321012345") == std::string::npos);
  }
}

void TestNoIdentityChangesNothing() {
  FakeJson json;
  SeedOculusLogin(json);
  const nlohmann::json before = json.ToJson();
  FakeUser user;
  user.words.platform_word = 0x5;
  user.words.account_id = 5551234;
  FakeSource source;
  source.available = false;
  g_log.clear();
  CHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::NoIdentity);
  CHECK(json.ToJson() == before);
  CHECK(user.words.platform_word == 0x5 && user.writes == 0);
  CHECK(g_log.size() == 1 && g_log[0].find("outcome=no-identity") != std::string::npos);
}

void TestRefusedJsonWriteKeepsUserIntact() {
  FakeJson json;
  SeedOculusLogin(json);
  json.refuse_path = "access_token";  // the NEVR token cannot be written
  FakeUser user;
  user.words.platform_word = 0x5;
  FakeSource source;
  source.identity = MakeIdentity();
  g_log.clear();
  CHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::JsonWriteFailed);
  CHECK(user.writes == 0 && user.words.platform_word == 0x5);
  CHECK(g_log.size() == 1 && g_log[0].find("first_path=access_token") != std::string::npos);
}

void TestTypeChangeIsDetected() {
  // The game's "accountid" is an integer; if a build kept it as a string the refused write
  // must surface instead of leaving the Oculus id in place.
  FakeJson json;
  json.SetString("accountid", "5551234");
  FakeUser user;
  FakeSource source;
  source.identity = MakeIdentity();
  CHECK(QuestLogin::RewriteLogin(user, json, source, MakeBuild(), &CaptureLog) == QuestLogin::Outcome::JsonWriteFailed);
}

void TestUserFailures() {
  FakeJson json;
  SeedOculusLogin(json);
  FakeSource source;
  source.identity = MakeIdentity();

  FakeUser unreadable;
  unreadable.readable = false;
  CHECK(QuestLogin::RewriteLogin(unreadable, json, source, MakeBuild(), &CaptureLog) ==
        QuestLogin::Outcome::UserUnreadable);
  CHECK(json.ToJson().at("access_token") == "OCULUS-ACCESS-TOKEN");  // untouched

  FakeUser unwritable;
  unwritable.writable = false;
  CHECK(QuestLogin::RewriteLogin(unwritable, json, source, MakeBuild(), &CaptureLog) ==
        QuestLogin::Outcome::UserWriteFailed);
}

void TestInvalidUtf8NameDoesNotThrow() {
  QuestLogin::Identity id = MakeIdentity();
  id.display_name = "bad\xff\xfe";
  const QuestLogin::Composition c = QuestLogin::Compose(id, {}, MakeBuild());
  CHECK(c.status == QuestLogin::ComposeStatus::ProfileBuildFailed);
  CHECK(c.fields.empty());
}

}  // namespace

int main() {
  TestUserIdRewrite();
  TestComposeFailsClosed();
  TestSameProfileAsPcvr();
  TestSerialRelay();
  TestRewriteLoginReplacesOculusIdentity();
  TestNoIdentityChangesNothing();
  TestRefusedJsonWriteKeepsUserIntact();
  TestTypeChangeIsDetected();
  TestUserFailures();
  TestInvalidUtf8NameDoesNotThrow();
  if (g_failures != 0) {
    std::fprintf(stderr, "login_rewrite_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("login_rewrite_test: all checks passed\n");
  return 0;
}
