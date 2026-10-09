// Host test for the Quest login prerequisites (src/quest/login/login_prerequisites.{h,cpp}).
//
// The handlers are driven the way the installed hooks drive them on the device: the "game" below
// calls the Platform SDK through function pointers that route through the handler with the real
// function as `original` (what a CallbackThunk does), and its four callbacks do what libpnsovr's
// do (login_prerequisites.h names the addresses):
//   org id    error -> global = -1;           else global = GetID(GetOrgScopedID(msg))
//   user      error -> buffer unchanged;      else buffer = GetOculusID(GetUser(msg)) or ""
//   token     error -> token = "?";           else token = GetString(msg)
//   proof     error -> login fails (500);     else nonce = GetNonce(GetUserProof(msg)), send
// and "prerequisites met" is UpdateInternal's test: id neither 0 nor -1, buffer neither "?" nor
// empty, token neither "?" nor shorter than two bytes (with its terminator).
//
// The core is compiled -fno-exceptions exactly as on the device; this file has exceptions (it parses
// the captured log lines with nlohmann::json). Run by `just test-quest-shared`.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/login/login_prerequisites.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/test_check.h"

namespace {

using QuestLogin::Prerequisite;

// ---- a fake Platform SDK --------------------------------------------------------------------

struct OrgHandle { std::uint64_t id; };
struct UserHandle { const char* oculus_id; };
struct ProofHandle { const char* nonce; };

struct FakeMessage {
  bool error = false;
  int code = 0;
  int http = 0;
  const char* text = nullptr;          // ovr_Message_GetString
  const char* error_json = nullptr;    // ovr_Error_GetMessage (scanned for is_transient)
  bool org_null = false;
  OrgHandle org{0};
  bool user_null = false;
  UserHandle user{nullptr};
  bool proof_null = false;
  ProofHandle proof{nullptr};
};

std::vector<const FakeMessage*> g_live;  // messages in flight; a real accessor on anything else is a violation
int g_violations = 0;                   // the real SDK was handed an error message or a handle it never made

const FakeMessage* Live(const void* message) {
  for (const FakeMessage* m : g_live) {
    if (m == message) return m;
  }
  ++g_violations;
  return nullptr;
}

template <typename H>
const FakeMessage* OwnerOfHandle(const void* handle, H FakeMessage::*member) {
  for (const FakeMessage* m : g_live) {
    if (static_cast<const void*>(&(m->*member)) == handle) return m;
  }
  ++g_violations;
  return nullptr;
}

bool RealIsError(const void* message) {
  const FakeMessage* m = Live(message);
  return m != nullptr && m->error;
}
const void* RealGetError(const void* message) {
  const FakeMessage* m = Live(message);
  return m != nullptr && m->error ? m : nullptr;
}
int RealErrorGetCode(const void* error) { return static_cast<const FakeMessage*>(error)->code; }
int RealErrorGetHttpCode(const void* error) { return static_cast<const FakeMessage*>(error)->http; }
const char* RealErrorGetMessage(const void* error) { return static_cast<const FakeMessage*>(error)->error_json; }

bool ReadyTrue() noexcept { return true; }

// The value accessors: calling one on an error message is a violation (the substitution must never
// let the game's success path reach the real SDK with an error).
const FakeMessage* LiveSuccess(const void* message) {
  const FakeMessage* m = Live(message);
  if (m != nullptr && m->error) ++g_violations;
  return m;
}
const char* RealGetString(const void* message) {
  const FakeMessage* m = LiveSuccess(message);
  return m != nullptr ? m->text : nullptr;
}
const void* RealGetOrgScopedId(const void* message) {
  const FakeMessage* m = LiveSuccess(message);
  return m == nullptr || m->org_null ? nullptr : &m->org;
}
std::uint64_t RealOrgScopedIdGetId(const void* handle) {
  const FakeMessage* m = OwnerOfHandle(handle, &FakeMessage::org);
  return m != nullptr ? m->org.id : 0;
}
const void* RealGetUser(const void* message) {
  const FakeMessage* m = LiveSuccess(message);
  return m == nullptr || m->user_null ? nullptr : &m->user;
}
const char* RealUserGetOculusId(const void* handle) {
  const FakeMessage* m = OwnerOfHandle(handle, &FakeMessage::user);
  return m != nullptr ? m->user.oculus_id : nullptr;
}
const void* RealGetUserProof(const void* message) {
  const FakeMessage* m = LiveSuccess(message);
  return m == nullptr || m->proof_null ? nullptr : &m->proof;
}
const char* RealUserProofGetNonce(const void* handle) {
  const FakeMessage* m = OwnerOfHandle(handle, &FakeMessage::proof);
  return m != nullptr ? m->proof.nonce : nullptr;
}

// What libpnsovr's GOT holds once the accessor hooks are installed.
bool GameIsError(const void* m) { return QuestLogin::OnMessageIsError(&RealIsError, m); }
const char* GameGetString(const void* m) { return QuestLogin::OnMessageGetString(&RealGetString, m); }
const void* GameGetOrgScopedId(const void* m) { return QuestLogin::OnMessageGetOrgScopedId(&RealGetOrgScopedId, m); }
std::uint64_t GameOrgScopedIdGetId(const void* h) { return QuestLogin::OnOrgScopedIdGetId(&RealOrgScopedIdGetId, h); }
const void* GameGetUser(const void* m) { return QuestLogin::OnMessageGetUser(&RealGetUser, m); }
const char* GameUserGetOculusId(const void* h) { return QuestLogin::OnUserGetOculusId(&RealUserGetOculusId, h); }
const void* GameGetUserProof(const void* m) { return QuestLogin::OnMessageGetUserProof(&RealGetUserProof, m); }
const char* GameUserProofGetNonce(const void* h) { return QuestLogin::OnUserProofGetNonce(&RealUserProofGetNonce, h); }

// ---- the game's state and callbacks ---------------------------------------------------------

struct GameState {
  std::uint64_t org_global = 0;
  std::string name;
  std::string token;
  std::string nonce;
  bool login_failed = false;
  bool sent = false;
};
GameState g_game;

void OrgCallback(void*, void* message) {
  if (GameIsError(message)) {
    g_game.org_global = ~std::uint64_t{0};
    return;
  }
  g_game.org_global = GameOrgScopedIdGetId(GameGetOrgScopedId(message));
}
void UserCallback(void*, void* message) {
  if (GameIsError(message)) return;
  const char* id = GameUserGetOculusId(GameGetUser(message));
  g_game.name = id != nullptr ? std::string(id).substr(0, 35) : std::string();
}
void TokenCallback(void*, void* message) {
  if (GameIsError(message)) {
    g_game.token = "?";
    return;
  }
  const char* text = GameGetString(message);
  g_game.token = text != nullptr ? text : "";
}
void ProofCallback(void*, void* message) {
  if (GameIsError(message)) {
    g_game.login_failed = true;
    return;
  }
  const char* nonce = GameUserProofGetNonce(GameGetUserProof(message));
  g_game.nonce = nonce != nullptr ? nonce : "";
  g_game.sent = true;
}

bool PrerequisitesMet() {
  const std::uint64_t id = g_game.org_global;
  return id != 0 && id != ~std::uint64_t{0} && g_game.name != "?" && !g_game.name.empty() &&
         g_game.token != "?" && g_game.token.size() + 1 >= 2;
}

QuestLogin::GameCallback CallbackFor(Prerequisite which) {
  switch (which) {
    case Prerequisite::OrgScopedId: return &OrgCallback;
    case Prerequisite::LoggedInUser: return &UserCallback;
    case Prerequisite::AccessToken: return &TokenCallback;
    default: return &ProofCallback;
  }
}

// What the mailbox does with a message: through the installed callback hook to the game's callback.
void Deliver(Prerequisite which, FakeMessage& message) {
  g_live.push_back(&message);
  QuestLogin::OnPrerequisiteCallback(which, CallbackFor(which), nullptr, &message);
  g_live.pop_back();
}

// ---- log capture ----------------------------------------------------------------------------

std::vector<std::string> g_lines;
void Capture(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

std::vector<nlohmann::json> Records(const char* event) {
  std::vector<nlohmann::json> out;
  for (const std::string& line : g_lines) {
    const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    QCHECK(!parsed.is_discarded());
    if (!parsed.is_discarded() && parsed.value("event", "") == event) out.push_back(parsed);
  }
  return out;
}

// Secrets and identifiers never appear in a line: neither the real values nor the synthesized ones.
void CheckNoValuesLogged(const std::vector<const char*>& values) {
  for (const std::string& line : g_lines) {
    for (const char* value : values) {
      if (line.find(value) != std::string::npos) {
        std::fprintf(stderr, "log line carries a value it must not: %s\n", line.c_str());
        QCHECK(false);
      }
    }
  }
}

const QuestLogin::OvrErrorApi kRealApi{&RealIsError, &RealGetError, &RealErrorGetCode,
                                       &RealErrorGetHttpCode, &RealErrorGetMessage};

void Fresh(bool configure, bool substitute, bool ready = true, const QuestLogin::OvrErrorApi& api = kRealApi) {
  QuestLogin::ResetPrerequisitesForTest();
  g_game = GameState{};
  g_lines.clear();
  g_violations = 0;
  if (configure) QuestLogin::ConfigurePrerequisites(api, substitute, ready ? &ReadyTrue : nullptr);
}

FakeMessage Ok(std::uint64_t org, const char* oculus_id, const char* token, const char* nonce) {
  FakeMessage m;
  m.org.id = org;
  m.user.oculus_id = oculus_id;
  m.text = token;
  m.proof.nonce = nonce;
  return m;
}
FakeMessage Error(int code) {
  FakeMessage m;
  m.error = true;
  m.code = code;
  m.http = 401;
  return m;
}
FakeMessage TransientError(int code) {
  FakeMessage m = Error(code);
  m.error_json = "{\"error\":{\"is_transient\":true,\"code\":123}}";
  return m;
}

void DeliverAll(FakeMessage& org, FakeMessage& user, FakeMessage& token) {
  Deliver(Prerequisite::OrgScopedId, org);
  Deliver(Prerequisite::LoggedInUser, user);
  Deliver(Prerequisite::AccessToken, token);
}

void ExpectRecord(const nlohmann::json& record, const char* call, const char* result, const char* reason) {
  const bool match = record.value("call", "") == call && record.value("result", "") == result &&
                     record.value("reason", "") == reason;
  if (!match) std::fprintf(stderr, "want %s/%s/%s, got %s\n", call, result, reason, record.dump().c_str());
  QCHECK(match);
}

// ---- tests ----------------------------------------------------------------------------------

// Oculus answers everything: the game gets exactly the real values and the login is sent.
void TestRealAnswersPassThroughUnchanged() {
  Fresh(true, true);
  FakeMessage org = Ok(1234567890123456ULL, nullptr, nullptr, nullptr);
  FakeMessage user = Ok(0, "real-oculus-name", nullptr, nullptr);
  FakeMessage token = Ok(0, nullptr, "REAL-ACCESS-TOKEN-VALUE", nullptr);
  DeliverAll(org, user, token);
  QCHECK(PrerequisitesMet());
  FakeMessage proof = Ok(0, nullptr, nullptr, "real-nonce-value");
  Deliver(Prerequisite::UserProof, proof);
  QCHECK(g_game.org_global == 1234567890123456ULL);
  QCHECK(g_game.name == "real-oculus-name");
  QCHECK(g_game.token == "REAL-ACCESS-TOKEN-VALUE");
  QCHECK(g_game.nonce == "real-nonce-value");
  QCHECK(g_game.sent && !g_game.login_failed);
  QCHECK(g_violations == 0);
  const auto records = Records("quest_login_prerequisite");
  QCHECK(records.size() == 4);
  if (records.size() == 4) {
    ExpectRecord(records[0], "ovr_User_GetOrgScopedID", "real", "ok");
    ExpectRecord(records[1], "ovr_User_GetLoggedInUser", "real", "ok");
    ExpectRecord(records[2], "ovr_User_GetAccessToken", "real", "ok");
    ExpectRecord(records[3], "ovr_User_GetUserProof", "real", "ok");
    for (const auto& r : records) QCHECK(r.value("ovr_error", -2) == 0 && r.value("level", "") == "info");
  }
  CheckNoValuesLogged({"1234567890123456", "real-oculus-name", "REAL-ACCESS-TOKEN-VALUE", "real-nonce-value"});
}

// The headset's failure (#240): GetAccessToken answers 2006. The game stores the synthesized token
// instead of "?", the prerequisites are met, and the line says so with the error code.
void TestAccessTokenErrorIsSynthesized() {
  Fresh(true, true);
  FakeMessage org = Ok(1234567890123456ULL, nullptr, nullptr, nullptr);
  FakeMessage user = Ok(0, "real-oculus-name", nullptr, nullptr);
  FakeMessage token = Error(2006);
  DeliverAll(org, user, token);
  QCHECK(g_game.token == QuestLogin::kSynthesizedAccessToken);
  QCHECK(PrerequisitesMet());
  QCHECK(g_violations == 0);
  const auto records = Records("quest_login_prerequisite");
  QCHECK(records.size() == 3);
  if (records.size() == 3) {
    ExpectRecord(records[2], "ovr_User_GetAccessToken", "synthesized", "ovr_error");
    QCHECK(records[2].value("error_code", 0) == 2006);
    QCHECK(records[2].value("http_code", 0) == 401);
    QCHECK(records[2].value("ovr_error", -2) == 1);
    QCHECK(records[2].value("accessor", "") == "ovr_Message_GetString");
    QCHECK(records[2].value("level", "") == "warn");
  }
  CheckNoValuesLogged({QuestLogin::kSynthesizedAccessToken, "real-oculus-name", "1234567890123456"});
}

// Every one of the four fails: the game still reaches the send, with synthesized values, and the
// real SDK is never handed an error message or a handle it did not make.
void TestAllFourErrorsAreSynthesized() {
  Fresh(true, true);
  FakeMessage org = Error(2001);
  FakeMessage user = Error(2002);
  FakeMessage token = Error(2006);
  DeliverAll(org, user, token);
  QCHECK(g_game.org_global == QuestLogin::kSynthesizedOrgScopedId);
  QCHECK(g_game.name == QuestLogin::kSynthesizedOculusId);
  QCHECK(g_game.token == QuestLogin::kSynthesizedAccessToken);
  QCHECK(PrerequisitesMet());
  FakeMessage proof = Error(2007);
  Deliver(Prerequisite::UserProof, proof);
  QCHECK(g_game.sent && !g_game.login_failed);
  QCHECK(g_game.nonce == QuestLogin::kSynthesizedNonce);
  QCHECK(g_violations == 0);
  const auto records = Records("quest_login_prerequisite");
  QCHECK(records.size() == 4);
  if (records.size() == 4) {
    ExpectRecord(records[0], "ovr_User_GetOrgScopedID", "synthesized", "ovr_error");
    ExpectRecord(records[1], "ovr_User_GetLoggedInUser", "synthesized", "ovr_error");
    ExpectRecord(records[2], "ovr_User_GetAccessToken", "synthesized", "ovr_error");
    ExpectRecord(records[3], "ovr_User_GetUserProof", "synthesized", "ovr_error");
    QCHECK(records[0].value("error_code", 0) == 2001 && records[3].value("error_code", 0) == 2007);
    QCHECK(records[0].value("accessor", "") == "ovr_Message_GetOrgScopedID");
    QCHECK(records[1].value("accessor", "") == "ovr_Message_GetUser");
    QCHECK(records[3].value("accessor", "") == "ovr_Message_GetUserProof");
  }
  CheckNoValuesLogged({QuestLogin::kSynthesizedAccessToken, QuestLogin::kSynthesizedNonce,
                       QuestLogin::kSynthesizedOculusId, "1313166930"});
}

// A success that carries nothing usable would leave the game waiting forever (an empty user name
// or token) or failing (-1): each is replaced and logged as empty_value.
void TestUnusableAnswersAreSynthesized() {
  Fresh(true, true);
  FakeMessage org = Ok(~std::uint64_t{0}, nullptr, nullptr, nullptr);
  FakeMessage user = Ok(0, nullptr, nullptr, nullptr);  // GetOculusID answers null
  FakeMessage token = Ok(0, nullptr, "", nullptr);
  DeliverAll(org, user, token);
  QCHECK(g_game.org_global == QuestLogin::kSynthesizedOrgScopedId);
  QCHECK(g_game.name == QuestLogin::kSynthesizedOculusId);
  QCHECK(g_game.token == QuestLogin::kSynthesizedAccessToken);
  FakeMessage proof = Ok(0, nullptr, nullptr, nullptr);
  proof.proof_null = true;  // GetUserProof answers a null handle
  Deliver(Prerequisite::UserProof, proof);
  QCHECK(g_game.nonce == QuestLogin::kSynthesizedNonce);
  QCHECK(g_violations == 0);
  const auto records = Records("quest_login_prerequisite");
  QCHECK(records.size() == 4);
  if (records.size() == 4) {
    ExpectRecord(records[0], "ovr_User_GetOrgScopedID", "synthesized", "empty_value");
    ExpectRecord(records[1], "ovr_User_GetLoggedInUser", "synthesized", "empty_value");
    ExpectRecord(records[2], "ovr_User_GetAccessToken", "synthesized", "empty_value");
    ExpectRecord(records[3], "ovr_User_GetUserProof", "synthesized", "empty_value");
    QCHECK(records[0].value("accessor", "") == "ovr_OrgScopedID_GetID");
    QCHECK(records[1].value("accessor", "") == "ovr_User_GetOculusID");
  }
  // An org id of 0 and a token of "?" are unusable too.
  Fresh(true, true);
  FakeMessage zero = Ok(0, nullptr, nullptr, nullptr);
  FakeMessage question = Ok(0, nullptr, "?", nullptr);
  Deliver(Prerequisite::OrgScopedId, zero);
  Deliver(Prerequisite::AccessToken, question);
  QCHECK(g_game.org_global == QuestLogin::kSynthesizedOrgScopedId);
  QCHECK(g_game.token == QuestLogin::kSynthesizedAccessToken);
  // An empty (not null) user name and nonce are unusable as well.
  Fresh(true, true);
  FakeMessage empty_user = Ok(0, "", nullptr, nullptr);
  FakeMessage empty_nonce = Ok(0, nullptr, nullptr, "");
  Deliver(Prerequisite::LoggedInUser, empty_user);
  Deliver(Prerequisite::UserProof, empty_nonce);
  QCHECK(g_game.name == QuestLogin::kSynthesizedOculusId);
  QCHECK(g_game.nonce == QuestLogin::kSynthesizedNonce);
}

// Without the accessor hooks the handler only measures: the game handles the error itself (the
// token becomes "?") and the line says why nothing was substituted.
void TestWithoutSubstitutionOnlyMeasures() {
  Fresh(true, false);
  FakeMessage token = Error(2006);
  Deliver(Prerequisite::AccessToken, token);
  QCHECK(g_game.token == "?");
  const auto records = Records("quest_login_prerequisite");
  QCHECK(records.size() == 1);
  if (records.size() == 1) {
    ExpectRecord(records[0], "ovr_User_GetAccessToken", "real", "substitution_unavailable");
    QCHECK(records[0].value("error_code", 0) == 2006);
    QCHECK(records[0].value("level", "") == "error");
  }
}

// Before ConfigurePrerequisites every handler is a plain pass-through and logs nothing.
void TestUnconfiguredPassesThroughSilently() {
  Fresh(false, false);
  FakeMessage token = Error(2006);
  Deliver(Prerequisite::AccessToken, token);
  QCHECK(g_game.token == "?");
  QCHECK(g_lines.empty());
  QCHECK(QuestLogin::PrerequisiteCallbacks(Prerequisite::AccessToken) == 0);
}

// Other callers of the same accessors (social, rooms) get the real answer, even while a login
// callback is active: only the claimed message is substituted.
void TestOtherMessagesAreUntouched() {
  Fresh(true, true);
  FakeMessage other = Error(1);
  FakeMessage other_ok = Ok(0, nullptr, "OTHER-REAL-TEXT", nullptr);
  g_live.push_back(&other);
  g_live.push_back(&other_ok);
  QCHECK(GameIsError(&other));  // no attempt active
  FakeMessage token = Error(2006);
  g_live.push_back(&token);
  struct Seen {
    bool other_error = false;
    std::string other_text;
  } seen;
  struct Probe {
    static void Callback(void* self, void* message) {
      // Different messages, mid-callback: a social callback's error and a social callback's string.
      static_cast<Seen*>(self)->other_error = GameIsError(g_live[0]);
      const char* text = GameGetString(g_live[1]);
      static_cast<Seen*>(self)->other_text = text != nullptr ? text : "";
      TokenCallback(nullptr, message);
    }
  };
  QuestLogin::OnPrerequisiteCallback(Prerequisite::AccessToken, &Probe::Callback, &seen, &token);
  g_live.clear();
  QCHECK(seen.other_error);
  QCHECK(seen.other_text == "OTHER-REAL-TEXT");
  QCHECK(g_game.token == QuestLogin::kSynthesizedAccessToken);
  QCHECK(g_violations == 0);
}

// A second callback arriving while one is active is passed through and logged "busy".
void TestNestedCallbackIsBusy() {
  Fresh(true, true);
  FakeMessage token = Ok(0, nullptr, "REAL-ACCESS-TOKEN-VALUE", nullptr);
  struct Nested {
    static void Callback(void*, void* message) {
      FakeMessage org = Error(2001);
      Deliver(Prerequisite::OrgScopedId, org);
      TokenCallback(nullptr, message);
    }
  };
  g_live.push_back(&token);
  QuestLogin::OnPrerequisiteCallback(Prerequisite::AccessToken, &Nested::Callback, nullptr, &token);
  g_live.pop_back();
  QCHECK(g_game.org_global == ~std::uint64_t{0});  // the game's own error path
  QCHECK(g_game.token == "REAL-ACCESS-TOKEN-VALUE");
  const auto records = Records("quest_login_prerequisite");
  QCHECK(records.size() == 2);
  if (records.size() == 2) {
    ExpectRecord(records[0], "ovr_User_GetOrgScopedID", "real", "busy");
    ExpectRecord(records[1], "ovr_User_GetAccessToken", "real", "ok");
  }
}

// Fail closed: when NEVR is not ready, nothing is synthesized even on an error. The game runs its
// own login-failure path (token "?") and the line says not_ready; the synthesis mark stays clear.
void TestNotReadyFailsClosed() {
  Fresh(true, true, /*ready=*/false);
  FakeMessage org = Error(2001);
  FakeMessage user = Error(2002);
  FakeMessage token = Error(2006);
  DeliverAll(org, user, token);
  QCHECK(g_game.org_global == ~std::uint64_t{0});  // the game's own error path, not synthesized
  QCHECK(g_game.name.empty());
  QCHECK(g_game.token == "?");
  QCHECK(!PrerequisitesMet());
  QCHECK(!QuestLogin::PrerequisitesSynthesizedSinceReset());
  QCHECK(g_violations == 0);
  const auto records = Records("quest_login_prerequisite");
  QCHECK(records.size() == 3);
  if (records.size() == 3) {
    ExpectRecord(records[0], "ovr_User_GetOrgScopedID", "real", "not_ready");
    ExpectRecord(records[2], "ovr_User_GetAccessToken", "real", "not_ready");
    QCHECK(records[2].value("error_code", 0) == 2006);
  }
  // A real, usable answer while not ready also passes through untouched, and still no synthesis.
  Fresh(true, true, /*ready=*/false);
  FakeMessage good = Ok(0, nullptr, "REAL-TOKEN", nullptr);
  Deliver(Prerequisite::AccessToken, good);
  QCHECK(g_game.token == "REAL-TOKEN");
  QCHECK(!QuestLogin::PrerequisitesSynthesizedSinceReset());
}

// A transient Oculus error is passed through so the game re-requests, up to the cap; after the cap
// a permanently-transient error is synthesized so login still proceeds.
void TestTransientErrorIsPassedThroughThenSynthesized() {
  Fresh(true, true);
  for (std::uint64_t i = 0; i < QuestLogin::kMaxTransientPasses; ++i) {
    FakeMessage t = TransientError(2006);
    Deliver(Prerequisite::AccessToken, t);
    QCHECK(g_game.token == "?");  // the game's own error path ran; nothing synthesized yet
  }
  QCHECK(!QuestLogin::PrerequisitesSynthesizedSinceReset());
  FakeMessage last = TransientError(2006);
  Deliver(Prerequisite::AccessToken, last);
  QCHECK(g_game.token == QuestLogin::kSynthesizedAccessToken);  // cap reached: synthesized
  QCHECK(QuestLogin::PrerequisitesSynthesizedSinceReset());
  QCHECK(g_violations == 0);
  const auto records = Records("quest_login_prerequisite");
  QCHECK(records.size() == QuestLogin::kMaxTransientPasses + 1);
  if (records.size() == QuestLogin::kMaxTransientPasses + 1) {
    ExpectRecord(records[0], "ovr_User_GetAccessToken", "real", "transient_passthrough");
    ExpectRecord(records.back(), "ovr_User_GetAccessToken", "synthesized", "ovr_error");
  }
  // A non-transient error synthesizes immediately (no passthrough).
  Fresh(true, true);
  FakeMessage hard = Error(2006);
  Deliver(Prerequisite::AccessToken, hard);
  QCHECK(g_game.token == QuestLogin::kSynthesizedAccessToken);
}

// The synthesis mark tracks whether any prerequisite was synthesized since the last reset; the
// login send hook reads and clears it to decide fail-closed.
void TestSynthesisMarkTracksSubstitution() {
  Fresh(true, true);
  FakeMessage real = Ok(1234ULL, "n", "t", nullptr);
  Deliver(Prerequisite::OrgScopedId, real);
  QCHECK(!QuestLogin::PrerequisitesSynthesizedSinceReset());  // a real answer does not set it
  FakeMessage err = Error(2006);
  Deliver(Prerequisite::AccessToken, err);
  QCHECK(QuestLogin::PrerequisitesSynthesizedSinceReset());
  QuestLogin::ResetPrerequisitesSynthesisMark();
  QCHECK(!QuestLogin::PrerequisitesSynthesizedSinceReset());
}

// Callback log lines are capped per prerequisite (like requests): the first kCallbackLogLimit carry
// their fields, then one summary line; the counter still counts every callback.
void TestCallbackLogIsCappedPerCall() {
  Fresh(true, true);
  const std::uint64_t n = QuestLogin::kCallbackLogLimit + 3;
  for (std::uint64_t i = 0; i < n; ++i) {
    FakeMessage t = Ok(0, nullptr, "REAL-TOKEN", nullptr);
    Deliver(Prerequisite::AccessToken, t);
  }
  const auto records = Records("quest_login_prerequisite");
  QCHECK(records.size() == QuestLogin::kCallbackLogLimit + 1);
  if (records.size() == QuestLogin::kCallbackLogLimit + 1) {
    QCHECK(records.back().value("status", "") == "log_limit_reached");
  }
  QCHECK(QuestLogin::PrerequisiteCallbacks(Prerequisite::AccessToken) == n);
}

// Without ovr_Message_IsError the handler cannot measure and never substitutes.
void TestUnmeasuredWithoutIsError() {
  QuestLogin::OvrErrorApi api{};
  Fresh(true, true, true, api);
  FakeMessage token = Error(2006);
  Deliver(Prerequisite::AccessToken, token);
  QCHECK(g_game.token == "?");
  const auto records = Records("quest_login_prerequisite");
  QCHECK(records.size() == 1);
  if (records.size() == 1) {
    QCHECK(records[0].value("ovr_error", -2) == -1);
    QCHECK(records[0].value("reason", "") == "unmeasured");
  }
}

// Requests: the first kRequestLogLimit per prerequisite are logged with their id, then one line.
void TestRequestsAreLoggedThenCounted() {
  Fresh(true, true);
  for (std::uint64_t i = 1; i <= QuestLogin::kRequestLogLimit + 3; ++i) {
    QuestLogin::NoteRequest(Prerequisite::AccessToken, 100 + i);
  }
  QuestLogin::NoteRequest(Prerequisite::UserProof, 0);
  const auto records = Records("quest_login_prerequisite_request");
  QCHECK(records.size() == QuestLogin::kRequestLogLimit + 2);
  if (records.size() == QuestLogin::kRequestLogLimit + 2) {
    QCHECK(records[0].value("call", "") == "ovr_User_GetAccessToken");
    QCHECK(records[0].value("request_id", 0) == 101);
    QCHECK(records[QuestLogin::kRequestLogLimit].value("status", "") == "log_limit_reached");
    QCHECK(records.back().value("call", "") == "ovr_User_GetUserProof");
    QCHECK(records.back().value("request_id", -1) == 0);
  }
  QCHECK(QuestLogin::PrerequisiteRequests(Prerequisite::AccessToken) == QuestLogin::kRequestLogLimit + 3);
}

}  // namespace

int main() {
  sentinel::SetLogSink(&Capture);
  TestRealAnswersPassThroughUnchanged();
  TestAccessTokenErrorIsSynthesized();
  TestAllFourErrorsAreSynthesized();
  TestUnusableAnswersAreSynthesized();
  TestWithoutSubstitutionOnlyMeasures();
  TestUnconfiguredPassesThroughSilently();
  TestOtherMessagesAreUntouched();
  TestNestedCallbackIsBusy();
  TestNotReadyFailsClosed();
  TestTransientErrorIsPassedThroughThenSynthesized();
  TestSynthesisMarkTracksSubstitution();
  TestCallbackLogIsCappedPerCall();
  TestUnmeasuredWithoutIsError();
  TestRequestsAreLoggedThenCounted();
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_prerequisites_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_prerequisites_test: all checks passed\n");
  return 0;
}
