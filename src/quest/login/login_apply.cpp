// Everything in the login rewrite that calls into the game: reading its state, the account id,
// the JSON transaction and its rollback. Built -fno-exceptions (src/quest/CMakeLists.txt), so no
// frame in this file carries a personality or an LSDA and a foreign exception thrown by game
// code passes through on CFI alone (quest/sentinel/callback_thunk.h). There is no try/catch
// here; an allocation failure is expected to raise std::bad_alloc from libc++'s operator new
// (the library is built with exceptions), uncaught: out of memory only.
#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "quest/login/login_rewrite.h"
#include "quest/login/login_standin.h"

#if defined(__cpp_exceptions)
#error "login_apply.cpp must be built with -fno-exceptions (it runs while game code is live)"
#endif

namespace QuestLogin {

// Names for log records and tests: plain leaf functions kept in this personality-free unit.
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

constexpr char kPathSeparator = '|';

// What a key held before the rewrite touched it.
struct Saved {
  std::string path;
  std::string top;  // first path segment when the path is nested, else empty
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

// Puts one key back.
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
    default: break;  // never written: the pre-check refuses these before any write
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

LogKv Text(const char* key, const char* value) { return LogKv{key, value, 0}; }
LogKv Num(const char* key, long long value) { return LogKv{key, nullptr, value}; }

// Ends a login the rewrite did not take: puts the Oculus account id back (a no-op when no
// earlier login replaced it) and writes the one record, with `restored` saying whether the id
// was put back. The JSON needs no undoing here; ApplyFieldsAtomically restores its own.
Outcome Decline(UserAccess& user, LogFn log, Outcome outcome, const LogKv* details, std::size_t n) {
  const bool restored = user.RestoreAccountId();
  LogKv fields[9];
  std::size_t count = 0;
  for (std::size_t i = 0; i < n && count < sizeof(fields) / sizeof(fields[0]) - 1; ++i) fields[count++] = details[i];
  fields[count++] = Num("restored", restored ? 1 : 0);
  Emit(log, Level::Error, outcome, fields, count);
  return outcome;
}

// The details a plan's decline carries (fixed tokens and counts, never values).
Outcome DeclinePlan(UserAccess& user, LogFn log, const Plan& plan) {
  switch (plan.outcome) {
    case Outcome::NoIdentity: {
      const LogKv d[] = {Text("reason", IdentityStatusName(plan.identity_status))};
      return Decline(user, log, plan.outcome, d, 1);
    }
    case Outcome::UserUnreadable: {
      const LogKv d[] = {Text("reason", "provider")};
      return Decline(user, log, plan.outcome, d, 1);
    }
    case Outcome::PlatformMismatch: {
      const LogKv d[] = {Num("provider", static_cast<long long>(plan.provider)),
                         Num("expected", static_cast<long long>(kPlatformOvrOrg))};
      return Decline(user, log, plan.outcome, d, 2);
    }
    case Outcome::ComposeFailed: {
      const LogKv d[] = {Text("reason", StatusName(plan.compose_status))};
      return Decline(user, log, plan.outcome, d, 1);
    }
    default:
      return Decline(user, log, plan.outcome, nullptr, 0);
  }
}

}  // namespace

GameValues ReadGameValues(const JsonAccess& json) {
  GameValues values;
  if (json.TypeOf("hmdserialnumber") == JsonType::String) values.hmd_serial = json.GetString("hmdserialnumber");
  if (json.TypeOf("system_info|headset_type") == JsonType::String) {
    values.headset_type = json.GetString("system_info|headset_type");
  }
  return values;
}

void Observe(const UserAccess& user, const JsonAccess& json, Observation& out) {
  out = Observation();
  out.provider_readable = user.Provider(out.provider);
  out.game = ReadGameValues(json);
  for (std::size_t i = 0; i < kMeasuredCount; ++i) {
    out.measured_present[i] = json.TypeOf(kMeasuredPaths[i]) != JsonType::Absent;
  }
}

bool ApplyFieldsAtomically(const std::vector<Field>& fields, JsonAccess& json,
                           std::string& failed_path) {
  failed_path.clear();
  std::vector<Saved> saved;
  saved.reserve(fields.size());
  for (const Field& field : fields) {
    Saved before = Snapshot(field, json);
    // A key of another type would refuse the write (CJson will not change a type), and a real,
    // array or object value could not be put back; either way nothing is written.
    const bool same_type = before.type == WrittenType(field.kind);
    const bool replaceable = before.type == JsonType::Absent || before.type == JsonType::Null;
    // A nested write under a parent that exists but is not an object is refused by CJson
    // ("$ json path: %s is not an object.", libpnsovr.so string 0x5825b1; the setter's path
    // walker 0x35ba84 branches to 0x364bdc, which tests the node type at 0x364bf8-0x364c00);
    // the rewrite does not attempt it. This check is defensive: with it off, the refused write
    // fails the read-back and the rollback finds nothing to undo, so the game-visible result is
    // the same; no test can pin it against real behaviour. Every setter also refuses when the CJson is cached
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
  std::size_t written = 0;
  for (const Field& field : fields) {
    ++written;
    if (!WriteAndVerify(field, json)) {
      failed_path = field.path;
      RestoreAll(saved, written, json);
      return false;
    }
  }
  return true;
}

Outcome RewriteLogin(UserAccess& user, JsonAccess& json, IdentitySource& source,
                     const BuildInfo& build, LogFn log) {
  Observation observed;
  Observe(user, json, observed);

  Plan plan;
  ComposePlan(source, build, observed, plan);
  if (plan.outcome != Outcome::Rewritten) return DeclinePlan(user, log, plan);

  // The account id is not in the JSON: the game asks the user object for it through a virtual
  // call. Change what that call returns, then ask it the same way the sender will. This step
  // runs first so the JSON transaction is the last thing that can fail and the account id is
  // restored if it does.
  std::uint64_t wire = 0;
  if (!user.SetAccountId(plan.account_id) || !user.WireAccountId(wire) || wire != plan.account_id) {
    return Decline(user, log, Outcome::AccountIdNotCarried, nullptr, 0);
  }

  std::string failed_path;
  if (!ApplyFieldsAtomically(plan.fields, json, failed_path)) {
    const LogKv d[] = {Text("first_path", failed_path.c_str()),
                       Num("fields", static_cast<long long>(plan.fields.size()))};
    return Decline(user, log, Outcome::JsonWriteFailed, d, 2);
  }

  const LogKv d[] = {Num("platform", static_cast<long long>(kPlatformOvrOrg)),
                     Text("hmd_serial_source", plan.hmd_serial_source.c_str()),
                     Num("fields", static_cast<long long>(plan.fields.size())),
                     Num("skipped", static_cast<long long>(plan.skipped)),
                     Num("kept_client_class", static_cast<long long>(plan.kept_client_class)),
                     Num("kept_measured", static_cast<long long>(plan.kept_measured))};
  Emit(log, Level::Info, Outcome::Rewritten, d, sizeof(d) / sizeof(d[0]));
  return Outcome::Rewritten;
}

FinishResult FinishLogin(UserAccess& user, const JsonAccess& json, PrerequisiteState& state, Outcome outcome,
                         LogFn log) {
  FinishResult result;
  std::uint64_t id = 0;
  const bool readable = user.WireAccountId(id);
  result.wire.account_id_unreadable = !readable;
  result.wire.account_id_stand_in = readable && StandIn::IsOrgId(id);
  result.wire.account_id_invalid = readable && !OculusIdMemory::IsRealId(id);
  if (json.TypeOf("access_token") == JsonType::String) {
    result.wire.access_token_stand_in = StandIn::IsAccessToken(json.GetString("access_token").c_str());
  }
  if (json.TypeOf("nonce") == JsonType::String) {
    result.wire.nonce_stand_in = StandIn::IsNonce(json.GetString("nonce").c_str());
  }
  result.decision = DecideSend(result.wire);

  const bool nevr_login = outcome == Outcome::Rewritten && result.decision == SendDecision::SendOriginal;
  if (!nevr_login) {
    // Put every stand-in that has a re-fetch marker back to it, so the next attempt asks Oculus.
    if (readable && StandIn::IsOrgId(id)) {
      state.ResetOrgIdToRefetch();
      result.reset_org_id = true;
    }
    if (state.UserNameIsStandIn()) {
      state.ResetUserNameToRefetch();
      result.reset_user_name = true;
    }
  } else if (state.UserNameIsStandIn() && json.TypeOf("displayname") == JsonType::String) {
    const std::string name = json.GetString("displayname");
    if (!name.empty()) {
      state.SetUserName(name.c_str());
      result.renamed_user = true;
    }
  }

  if (log != nullptr) {
    const LogKv fields[] = {
        Text("decision", result.decision == SendDecision::SendOriginal ? "send" : "fail_closed"),
        Text("outcome", OutcomeName(outcome)),
        Num("account_id_unreadable", result.wire.account_id_unreadable ? 1 : 0),
        Num("account_id_stand_in", result.wire.account_id_stand_in ? 1 : 0),
        Num("account_id_invalid", result.wire.account_id_invalid ? 1 : 0),
        Num("access_token_stand_in", result.wire.access_token_stand_in ? 1 : 0),
        Num("nonce_stand_in", result.wire.nonce_stand_in ? 1 : 0),
        Num("reset", (result.reset_org_id ? 1 : 0) | (result.reset_user_name ? 2 : 0) | (result.renamed_user ? 4 : 0)),
    };
    log(result.decision == SendDecision::SendOriginal ? Level::Info : Level::Warning, "quest_login_send", fields,
        sizeof(fields) / sizeof(fields[0]));
  }
  return result;
}

Outcome RewriteAndSend(UserAccess& user, JsonAccess& json, IdentitySource& source,
                       const BuildInfo& build, LogFn log, SendFn send, void* context) {
  const Outcome outcome = RewriteLogin(user, json, source, build, log);
  send(context);
  return outcome;
}

}  // namespace QuestLogin
