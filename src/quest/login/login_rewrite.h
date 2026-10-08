#pragma once
// Quest login rewrite: the portable half.
//
// The Quest game logs in with Oculus identity. NEVR needs the same login the
// PCVR bridge sends (src/runtime/compat/ws_bridge.cpp BuildLoginRequest): the NEVR
// token, the NEVR account id, platform code 4 (OVR_ORG) and the HMD serial the
// game itself reports. This file decides WHAT the login says and applies it
// all-or-nothing; it contains no game pointers and no Android headers, so the host
// test (tests/login_rewrite_test.cpp) exercises the same code the device runs.
// login_hook.cpp is the Android adapter that applies the result to the live
// CNSOVRUser and CJson.
//
// Where each wire field comes from on the Quest (measured, docs/adr/0003):
//   JSON            the CJson passed to CNSUser::SendLogInRequest (rewritten here)
//   platform code   [CNSUser+0x90] & 0xf; CNSOVRUser's constructor already stores 4
//                   (OVR_ORG), so it is checked, not written
//   account id      CNSUser::SendLogInRequest obtains it by a VIRTUAL call (vtable+0x70);
//                   CNSOVRUser overrides it to return a process global, so the rewrite
//                   changes that global (UserAccess::SetAccountId) and proves the wire
//                   value by calling the same virtual (UserAccess::WireAccountId)

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace QuestLogin {

// Platform code the PCVR bridge logs in as (ws_bridge.cpp kBridgeLoginPlatform). The game
// names itself with this platform in every later request.
constexpr std::uint64_t kPlatformOvrOrg = 4;
constexpr std::uint64_t kProviderMask = 0xf;

// What token auth hands the login. Never logged. There is deliberately no password: the
// server authenticates the session from the WebSocket upgrade (session_ws.go reads
// `password` from the URL query), not from the login JSON, and the game's own log of the
// outgoing login does not redact a `password` member (docs/adr/0003).
struct Identity {
  std::uint64_t account_id = 0;
  std::string display_name;
  std::string access_token;  // the NEVR token that replaces the Oculus access_token
};

enum class IdentityStatus { Ok, NotReady, NoAccount, NoToken };
const char* IdentityStatusName(IdentityStatus status);

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

// Which login members the rewrite replaces and which it leaves to the game. The server reads
// the login JSON as a client description plus an identity:
//   identity        accountid, access_token, nonce, displayname, bypassauth,
//                   desiredclientprofileversion, hmdserialnumber, nevr_identity, nevr_social
//                   are written from the NEVR identity and the shared profile.
//   client class    buildversion, appid, lobbyversion and publisher_lock say which client this
//                   is. The server derives Quest vs PCVR from them: IsPCVR() is
//                   buildversion != 630783 (the standalone build number), which selects the
//                   encoder flag layout on lobby join (UseQuestFlags), and the Discord
//                   integrator maps appid to a platform. The PCVR constants the shared builder
//                   emits (buildversion 631547, appid 0) would turn a Quest into a PCVR client,
//                   so these four are never written, present or not.
//   hardware        every system_info member the game already filled is kept; only absent ones
//                   are added.
bool IsClientClassPath(const std::string& path);
bool IsGameMeasuredPath(const std::string& path);

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

// What a CJson path holds. CJson::TypeOf reports 0 for both an absent path and a null value;
// Valid() separates the two. Real, Array and Object values cannot be restored by the setters
// the rewrite uses, so a path holding one is never written.
enum class JsonType { Absent, Null, String, Int, Real, Boolean, Array, Object };

// The game's CJson as seen by the rewrite. login_hook.cpp implements it over the exported
// NRadEngine::CJson members of libpnsovr.so; the host test implements it over a map that
// follows CJson's type rules (SetString and SetInt refuse to change an existing type; a null
// is replaceable).
// Set* return nothing because CJson reports a refused write only in the game's log, so every
// write is read back. Get* are defined only when TypeOf reports the matching type.
class JsonAccess {
 public:
  virtual ~JsonAccess() = default;
  virtual JsonType TypeOf(const char* path) const = 0;
  virtual std::string GetString(const char* path) const = 0;
  virtual std::int64_t GetInt(const char* path) const = 0;
  virtual bool GetBoolean(const char* path) const = 0;
  virtual void SetString(const char* path, const char* value) = 0;
  virtual void SetInt(const char* path, std::int64_t value) = 0;
  virtual void SetBoolean(const char* path, bool value) = 0;
  virtual void SetNull(const char* path) = 0;
  // Removes the key (CJson::Clear).
  virtual void Clear(const char* path) = 0;
};

// Reads the values the rewrite relays from the game's login JSON.
GameValues ReadGameValues(const JsonAccess& json);

// Writes `fields`, reads each back, and on any refusal or exception restores every key it
// touched to its previous value or absence. On a false return the JSON is as it was on entry.
// `failed_path` receives the first path that was refused (a key name, never a value).
bool ApplyFieldsAtomically(const std::vector<Field>& fields, JsonAccess& json,
                           std::string& failed_path);

// The CNSUser as the login path sees it.
class UserAccess {
 public:
  virtual ~UserAccess() = default;
  // The platform word's provider nibble ([CNSUser+0x90] & 0xf).
  virtual bool Provider(std::uint64_t& code) const = 0;
  // The account id CNSUser::SendLogInRequest will put on the wire: the result of the same
  // virtual AccountID() call it makes.
  virtual bool WireAccountId(std::uint64_t& id) const = 0;
  // Changes what AccountID() returns. The first write remembers the Oculus value (see
  // OculusIdMemory) so it can be put back.
  virtual bool SetAccountId(std::uint64_t id) = 0;
  // Puts the Oculus value back; a no-op when no rewrite ever replaced it. Called on every
  // outcome other than Rewritten, because CNSOVRUser::LogInInternal re-reads the Oculus org
  // id only when the global holds -1 (libpnsovr 0x1ec96c-0x1ec984): once the NEVR id is in
  // the global, a later login that the rewrite declines would send the Oculus login with a
  // NEVR AccountID().
  virtual void RestoreAccountId() = 0;
};

// The Oculus account id the game stored before the rewrite first replaced it. Shared by the
// device adapter and the host fake so both follow the same rule: the value found in the
// global is the Oculus id unless it is the value the rewrite itself wrote last, in which case
// the remembered one stays.
class OculusIdMemory {
 public:
  void NoteBeforeWrite(std::uint64_t current, std::uint64_t written) {
    if (!have_ || current != last_written_) oculus_ = current;
    have_ = true;
    last_written_ = written;
  }
  bool Original(std::uint64_t& out) const {
    out = oculus_;
    return have_;
  }

 private:
  bool have_ = false;
  std::uint64_t oculus_ = 0;
  std::uint64_t last_written_ = 0;
};

// Where identity comes from (token auth). Fail-close: anything but Ok means "no identity".
class IdentitySource {
 public:
  virtual ~IdentitySource() = default;
  virtual IdentityStatus Fetch(Identity& out) = 0;
};

enum class Level { Info, Warning, Error };

// One field of a structured log record: a key and either a fixed-token string or a number.
// Callers pass key names, counts and fixed tokens only, never a token, serial, display name
// or account id value.
struct LogKv {
  const char* key = "";
  const char* text = nullptr;  // nullptr: use `number`
  long long number = 0;
};
using LogFn = void (*)(Level level, const char* event, const LogKv* fields, std::size_t count);

enum class Outcome {
  Rewritten,          // JSON and wire account id carry the NEVR identity
  NoIdentity,         // nothing was changed
  PlatformMismatch,   // the user is not OVR_ORG; nothing was changed
  UserUnreadable,     // nothing was changed
  ComposeFailed,      // nothing was changed
  JsonWriteFailed,    // a field was refused; the JSON was restored, the user untouched
  AccountIdNotCarried,  // the wire account id did not become the NEVR id; everything restored
  Exception,          // an exception was caught; everything restored
};

const char* OutcomeName(Outcome outcome);

// The entry the device adapter calls from a translation unit built -fno-exceptions (the GOT
// thunk's contract, quest/sentinel/callback_thunk.h). It catches every std::exception inside
// this exceptions-enabled translation unit and reports Outcome::Exception; it never lets one
// escape. Residual: a non-std exception thrown by game code called from here (the CJson
// functions and the virtual AccountID() of libpnsovr.so, which imports __cxa_throw) is not
// caught (no catch-all, by repo rule) and, since the function is noexcept, ends in
// std::terminate instead of unwinding through frames with a mixed personality. None of the
// called game functions was observed to throw; that is inferred, not measured on a device.
Outcome RewriteLoginNoThrow(UserAccess& user, JsonAccess& json, IdentitySource& source,
                            const BuildInfo& build, LogFn log) noexcept;

// Hands the actual send to the caller so the rewrite and the send form one unit: the account
// id the rewrite installed is read by the sender through the virtual AccountID() and again by
// CNSUser::LogInSuccessCB, which builds {platform word, AccountID()} and compares it with the
// server's reply (libpnsovr 0x383a60-0x383a8c); a mismatch drops the success. It therefore
// stays installed after the send. `send(context)` is called exactly once, last, whatever the
// outcome.
using SendFn = void (*)(void* context);
Outcome RewriteAndSend(UserAccess& user, JsonAccess& json, IdentitySource& source,
                       const BuildInfo& build, LogFn log, SendFn send, void* context);

// The whole decision for one login attempt. Called once per CNSUser::SendLogInRequest, which
// is per login event (not per frame), so it may allocate and log. All-or-nothing: any
// non-Rewritten outcome leaves the JSON as it was on entry and the account id at the Oculus
// value, including when an earlier login was Rewritten. A login after a Rewritten one is
// rewritten again from the current identity.
// Every outcome emits one structured record (event "quest_login", field "outcome"); no
// record contains a token, serial, display name or account id value. Never throws.
Outcome RewriteLogin(UserAccess& user, JsonAccess& json, IdentitySource& source,
                     const BuildInfo& build, LogFn log);

}  // namespace QuestLogin
