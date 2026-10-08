#pragma once
// Quest login rewrite.
//
// The Quest game logs in with Oculus identity. NEVR needs the same login the PCVR bridge sends
// (src/runtime/compat/ws_bridge.cpp BuildLoginRequest): the NEVR token, the NEVR account id,
// platform code 4 (OVR_ORG) and the HMD serial the game itself reports. This file declares what
// the login says and how it is applied all-or-nothing; it contains no game pointers and no
// Android headers, so the host test (tests/login_rewrite_test.cpp) runs the same code the
// device runs. login_hook.cpp is the Android adapter that implements JsonAccess and UserAccess
// over the live CNSOVRUser and libpnsovr's CJson.
//
// Where each wire field comes from on the Quest (measured, docs/adr/0003):
//   JSON            the CJson passed to CNSUser::SendLogInRequest (rewritten here)
//   platform code   [CNSUser+0x90] & 0xf; CNSOVRUser's constructor already stores 4
//                   (OVR_ORG), so it is checked, not written
//   account id      CNSUser::SendLogInRequest obtains it by a VIRTUAL call (vtable+0x70);
//                   CNSOVRUser overrides it to return a process global, so the rewrite
//                   changes that global (UserAccess::SetAccountId) and proves the wire
//                   value by calling the same virtual (UserAccess::WireAccountId)
//
// Translation units. A frame that is live while game code runs must be personality-free
// (quest/sentinel/callback_thunk.h), so the work is split by whether it calls into the game:
//   login_apply.cpp    built -fno-exceptions: everything that calls the game (Observe,
//                      ApplyFieldsAtomically, RewriteLogin, RewriteAndSend). No try/catch;
//                      an allocation failure there is expected to raise std::bad_alloc from
//                      libc++'s operator new, uncaught (out of memory only).
//   login_rewrite.cpp  exceptions enabled: Compose and ComposePlan. They call no game code, run
//                      between game calls, and return before the next one.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/compat/social_level.h"

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
  // The social message level the login declares ("nevr_social"), same value and meaning as the
  // PCVR login. The server sends friend presence, recently met, the lobby tablet and party data
  // only to a session that declared level 1 or more. It is declared only when the social
  // feature is effective AND the social facade is actually installed (docs/adr/0003, contract
  // 5), so the default is 0 and the production IdentitySource sets it to
  // SocialParty::kSocialLevel (runtime/compat/social_level.h) when, and only when, both hold.
  int social_level = 0;
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

// The system_info members the shared profile can produce. Observe records which of them the
// game already holds, so the compose phase (which calls no game code) can keep them.
inline constexpr const char* kMeasuredPaths[] = {
    "system_info|headset_type",   "system_info|driver_version",    "system_info|network_type",
    "system_info|video_card",     "system_info|cpu",               "system_info|num_physical_cores",
    "system_info|num_logical_cores", "system_info|memory_total",   "system_info|memory_used",
    "system_info|dedicated_gpu_memory",
};
inline constexpr std::size_t kMeasuredCount = sizeof(kMeasuredPaths) / sizeof(kMeasuredPaths[0]);

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
// token yields no fields (the Quest identity is never fabricated). Exceptions enabled; calls
// no game code.
Composition Compose(const Identity& identity, const GameValues& game, const BuildInfo& build);

const char* StatusName(ComposeStatus status);

// What a CJson path holds. CJson::TypeOf reports 0 for both an absent path and a null value;
// Valid() separates the two. Real, Array and Object values cannot be restored by the setters
// the rewrite uses, so a path holding one is never written.
enum class JsonType { Absent, Null, String, Int, Real, Boolean, Array, Object };

// The game's CJson as seen by the rewrite. login_hook.cpp implements it over libpnsovr's
// NRadEngine::CJson; the host test implements it over a map that follows CJson's type rules
// (SetString and SetInt refuse to change an existing type; a null is replaceable). None of
// these may throw. Set* return nothing because CJson reports a refused write only in the
// game's log, so every write is read back. Get* are defined only when TypeOf reports the
// matching type.
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

// Reads the values the rewrite relays from the game's login JSON. (login_apply.cpp)
GameValues ReadGameValues(const JsonAccess& json);

// Writes `fields`, reads each back, and on any refusal restores every key it touched to its
// previous value or absence. On a false return the JSON is as it was on entry. `failed_path`
// receives the first path that was refused (a key name, never a value). (login_apply.cpp)
bool ApplyFieldsAtomically(const std::vector<Field>& fields, JsonAccess& json,
                           std::string& failed_path);

// The CNSUser as the login path sees it. None of these may throw.
class UserAccess {
 public:
  virtual ~UserAccess() = default;
  // The platform word's provider nibble ([CNSUser+0x90] & 0xf).
  virtual bool Provider(std::uint64_t& code) const = 0;
  // The account id CNSUser::SendLogInRequest will put on the wire: the result of the same
  // virtual AccountID() call it makes.
  virtual bool WireAccountId(std::uint64_t& id) const = 0;
  // Changes what AccountID() returns. Remembers the Oculus value (see OculusIdMemory) so it can
  // be put back. Returns false, changing nothing, when the global holds no real id (0 or -1)
  // and none is remembered: there is no Oculus id to put back and the login is left Oculus.
  virtual bool SetAccountId(std::uint64_t id) = 0;
  // Puts the Oculus value back and returns whether it did. Called on every outcome other than
  // Rewritten, because CNSOVRUser::LogInInternal re-reads the Oculus org id only when the
  // global holds -1 (libpnsovr 0x1ec96c-0x1ec984): once the NEVR id is in the global, a later
  // login that the rewrite declines would send the Oculus login with a NEVR AccountID(). It
  // restores only while the global still holds the value the rewrite wrote; a value the game
  // wrote in between is left alone. False also when no rewrite ever replaced it.
  virtual bool RestoreAccountId() = 0;
};

// The Oculus account id the game stored before the rewrite first replaced it. Shared by the
// device adapter and the host fake so both follow the same rule:
//   - a value found in the global is the Oculus id unless it is the value the rewrite itself
//     wrote last, in which case the remembered one stays;
//   - only a real id is remembered: 0 (RadPluginShutdown) and -1 (GotLoggedInUserOrgIdCb's
//     error path, and the "fetch again" marker) are not ids, and a write that would have to
//     remember one is refused;
//   - the id is put back only while the global still holds the value the rewrite wrote.
// Not thread safe by itself; the adapter serializes its own accesses, and the single game
// thread running the login path is an unverified assumption (docs/adr/0003).
class OculusIdMemory {
 public:
  static bool IsRealId(std::uint64_t value) { return value != 0 && value != ~std::uint64_t{0}; }

  // `current` is the global's value just before the write. False: refused, nothing noted.
  bool NoteBeforeWrite(std::uint64_t current, std::uint64_t written) {
    if (have_ && current == last_written_) {
      last_written_ = written;
      return true;
    }
    if (!IsRealId(current)) return false;
    oculus_ = current;
    have_ = true;
    last_written_ = written;
    return true;
  }
  // True and the Oculus value when `current` is still what the rewrite wrote.
  bool RestoreFor(std::uint64_t current, std::uint64_t& out) const {
    if (!have_ || current != last_written_) return false;
    out = oculus_;
    return true;
  }

 private:
  bool have_ = false;
  std::uint64_t oculus_ = 0;
  std::uint64_t last_written_ = 0;
};

// Where identity comes from (token auth). Fail-close: anything but Ok means "no identity". May
// throw std::exception; it is called only from the exceptions-enabled compose phase.
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
// A LogFn must not throw.
using LogFn = void (*)(Level level, const char* event, const LogKv* fields, std::size_t count);

enum class Outcome {
  Rewritten,            // JSON and wire account id carry the NEVR identity
  NoIdentity,           // nothing was changed
  PlatformMismatch,     // the user is not OVR_ORG; nothing was changed
  UserUnreadable,       // nothing was changed
  ComposeFailed,        // nothing was changed
  JsonWriteFailed,      // a field was refused; the JSON was restored, the user untouched
  AccountIdNotCarried,  // the wire account id did not become the NEVR id; everything restored
  Exception,            // the compose phase threw a std::exception; nothing was changed
};

const char* OutcomeName(Outcome outcome);

// Phase 1 (login_apply.cpp, calls the game): what the later phases need to know about the game's
// state. Plain data; nothing in it refers back to the game.
struct Observation {
  bool provider_readable = false;
  std::uint64_t provider = 0;
  GameValues game;
  bool measured_present[kMeasuredCount] = {};  // parallel to kMeasuredPaths
};
void Observe(const UserAccess& user, const JsonAccess& json, Observation& out);

// Phase 2 (login_rewrite.cpp, exceptions enabled, calls no game code): decides what the login
// says. `outcome` is Rewritten when the apply phase should run, otherwise the reason it is
// declined; the other members are the data for the log record and the apply phase.
struct Plan {
  Outcome outcome = Outcome::Exception;
  IdentityStatus identity_status = IdentityStatus::Ok;
  ComposeStatus compose_status = ComposeStatus::Ok;
  std::uint64_t provider = 0;
  std::uint64_t account_id = 0;
  std::vector<Field> fields;
  std::size_t skipped = 0;
  std::size_t kept_client_class = 0;
  std::size_t kept_measured = 0;
  std::string hmd_serial_source;
};
// Catches every std::exception and reports Outcome::Exception (named types only, no
// catch-all, by repo rule). Never throws.
void ComposePlan(IdentitySource& source, const BuildInfo& build, const Observation& observed,
                 Plan& plan) noexcept;

// Phases 1 to 3 in order (login_apply.cpp, -fno-exceptions): Observe, ComposePlan, then the
// account id and the JSON transaction. All-or-nothing: any non-Rewritten outcome leaves the
// JSON as it was on entry and the account id at the Oculus value, including when an earlier
// login was Rewritten. A login after a rewritten one is rewritten again from the current
// identity. Every outcome emits one structured record (event "quest_login", field "outcome");
// no record contains a token, serial, display name or account id value. This function is the
// frame that is live while the game's CJson and AccountID() run, so it is personality-free:
// it has no try/catch (an allocation failure is the out-of-memory case described below).
//
// Residual, stated exactly. Every frame live during a call into the game (this function, its
// callees in login_apply.cpp, the thunk and the handler) sits under the personality-free "zR"
// CIE, so a foreign exception thrown by libpnsovr's allocator hooks (CJson::InitializeForGame
// 0x357cb0, CAllocator vtable+0x10) or by a registered log callback (CLoggingData::
// ExecuteAllCallbacks) passes through on CFI alone, as it would without this code. The
// exceptions-enabled frames (ComposePlan and what it calls) are never on the stack during a
// game call. Reachability of a throw from the CJson functions and AccountID() was analysed
// statically (independent review, evidence cjson_throw_reach.txt): none of the 10 CJson
// functions nor AccountID() at 0x1ede14 reaches __cxa_throw, __cxa_allocate_exception,
// operator new, terminate or _Unwind_Resume by direct edges (39 to 62 functions each); the
// unresolved indirect edges are the allocator hooks, three in CMemBlock::Resize, two in
// ExecuteAllCallbacks, and one each in json_delete and fn_5083b0. Nothing was run on a device.
Outcome RewriteLogin(UserAccess& user, JsonAccess& json, IdentitySource& source,
                     const BuildInfo& build, LogFn log);

// Hands the actual send to the caller so the rewrite and the send form one unit: the account
// id the rewrite installed is read by the sender through the virtual AccountID() and again by
// CNSUser::LogInSuccessCB, which builds {platform word, AccountID()} and compares it with the
// server's reply (libpnsovr 0x383a60-0x383a8c); a mismatch drops the success. It therefore
// stays installed after the send. `send(context)` is called exactly once, last, whatever the
// outcome.
using SendFn = void (*)(void* context);
Outcome RewriteAndSend(UserAccess& user, JsonAccess& json, IdentitySource& source,
                       const BuildInfo& build, LogFn log, SendFn send, void* context);

}  // namespace QuestLogin
