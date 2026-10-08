#pragma once
// Quest login rewrite: the portable half.
//
// The Quest game logs in with Oculus identity. NEVR needs the same login the
// PCVR bridge sends (src/runtime/compat/ws_bridge.cpp BuildLoginRequest): the NEVR
// token, the NEVR account id, platform code 4 (OVR_ORG) and the HMD serial the
// game itself reports. This file decides WHAT the login says; it contains no game
// pointers and no Android headers, so the host test (tests/login_rewrite_test.cpp)
// exercises the same code the device runs. login_hook.cpp is the Android adapter
// that applies the result to the live CNSUser and CJson.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace QuestLogin {

// Platform code the PCVR bridge logs in as (ws_bridge.cpp kBridgeLoginPlatform). The game
// then names itself with this platform in every later request, so the CNSUser the game
// keeps must carry it too (see RewriteUserId).
constexpr std::uint64_t kPlatformOvrOrg = 4;
constexpr std::uint64_t kProviderMask = 0xf;

// What token auth hands the login. Never logged.
struct Identity {
  std::uint64_t account_id = 0;
  std::string display_name;
  std::string access_token;  // the NEVR token that replaces the Oculus access_token
  std::string password;
};

// Facts about this build that the profile reports (nevr_identity).
struct BuildInfo {
  std::string project_version;
  std::string git_commit;
  std::string git_describe;
  std::string build_type;
};

// Values the game already placed in the login JSON; the rewrite relays them.
struct GameValues {
  std::string hmd_serial;    // top-level "hmdserialnumber"; empty when the key is absent
  std::string headset_type;  // "system_info|headset_type"; empty when absent
};

// The two words the game keeps in CNSUser (+0x90, +0x88) and sends as SNSUserID.
struct UserIdWords {
  std::uint64_t platform_word = 0;  // low nibble is the provider enum
  std::uint64_t account_id = 0;
};

// Same transformation as the PCVR bridge's CNSUser patch: provider nibble -> 4, account id
// -> the NEVR account, every other bit of the platform word preserved.
UserIdWords RewriteUserId(const UserIdWords& original, const Identity& identity);

enum class FieldKind { String, Int, Boolean };

// One CJson assignment. `path` uses the game's own '|' nesting ("system_info|cpu").
struct Field {
  std::string path;
  FieldKind kind = FieldKind::String;
  std::string text;
  std::int64_t number = 0;  // Int value, or 0/1 for Boolean
};

enum class ComposeStatus { Ok, MissingAccountId, MissingToken, ProfileBuildFailed };

struct Composition {
  ComposeStatus status = ComposeStatus::ProfileBuildFailed;
  std::vector<Field> fields;
  // Profile members that have no CJson setter (arrays, nulls, reals, out-of-range
  // integers). Paths only, never values.
  std::vector<std::string> skipped;
  // Where the relayed HMD serial came from (HmdSerial::SourceName) and its length. The
  // value itself is an identifier and is never reported.
  std::string hmd_serial_source;
  std::size_t hmd_serial_length = 0;
};

// Builds the login fields from the shared LoginProfile builder, so Quest and PCVR run one
// implementation of the field set and of JSON escaping. Fail-close: no account id or no
// token yields no fields (the Quest identity is never fabricated).
Composition Compose(const Identity& identity, const GameValues& game, const BuildInfo& build);

const char* StatusName(ComposeStatus status);

// The game's CJson as seen by the rewrite. login_hook.cpp implements it over the exported
// NRadEngine::CJson setters/getters; the host test implements it over nlohmann::json.
// Set* return nothing because CJson::SetString/SetInt are void and report a refused write
// (for example a type change) only in the game's log, so every write is read back.
class JsonAccess {
 public:
  virtual ~JsonAccess() = default;
  virtual void SetString(const char* path, const char* value) = 0;
  virtual void SetInt(const char* path, std::int64_t value) = 0;
  virtual void SetBoolean(const char* path, bool value) = 0;
  // Read-back. `present` is false when the key does not exist.
  virtual std::string GetString(const char* path, bool& present) const = 0;
  virtual std::int64_t GetInt(const char* path, bool& present) const = 0;
  virtual bool GetBoolean(const char* path, bool& present) const = 0;
};

// Reads the values the rewrite relays from the game's login JSON.
GameValues ReadGameValues(const JsonAccess& json);

// Writes every field, then reads each one back. Returns the paths whose read-back differs
// (empty on success). Paths only; values are never reported.
std::vector<std::string> ApplyFields(const std::vector<Field>& fields, JsonAccess& json);

// CNSUser identity words, read and written by the adapter.
class UserAccess {
 public:
  virtual ~UserAccess() = default;
  virtual bool Read(UserIdWords& out) const = 0;
  virtual bool Write(const UserIdWords& words) = 0;
};

// Where identity comes from (token auth). Fail-close: false means "no identity".
class IdentitySource {
 public:
  virtual ~IdentitySource() = default;
  virtual bool Fetch(Identity& out, std::string& reason) = 0;
};

enum class Level { Info, Warning, Error };
using LogFn = void (*)(Level level, const char* line);

enum class Outcome {
  Rewritten,         // JSON and user words carry the NEVR identity
  NoIdentity,        // nothing was changed
  ComposeFailed,     // nothing was changed
  UserUnreadable,    // nothing was changed
  JsonWriteFailed,   // some JSON fields were refused; the user words were NOT rewritten
  UserWriteFailed,   // JSON rewritten, user words unchanged
};

const char* OutcomeName(Outcome outcome);

// The whole decision for one login attempt. Called once per CNSUser::SendLogInRequest, which
// is per login event (not per frame), so it may allocate and log. Every non-Rewritten
// outcome emits one structured log line; no line contains a token, password, serial or
// account id value.
Outcome RewriteLogin(UserAccess& user, JsonAccess& json, IdentitySource& source,
                     const BuildInfo& build, LogFn log);

}  // namespace QuestLogin
