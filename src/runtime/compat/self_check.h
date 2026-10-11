#pragma once
// Self-checks: a run-card check the build reports itself. One registration names the check and what it
// expects; the event that fires it calls Report (or the check's probe is polled by Flush for events seen
// only from a hooked path). Each result becomes ONE compact string, sent to the game service as a
// SNSRemoteLogSetv3 entry and written to the build's own log at the same time.
//
// Platform neutral by construction (like evr_codec.h and session_router.h): no Windows headers, no sockets,
// no game state. The PC runtime (compat/ws_bridge.cpp) and the Quest sentinel compile this one file; each
// supplies its sender (a login-connection frame sink) and its log sink.
//
// What a result looks like (nlohmann::json, one string):
//   {"message":"nevr_self_check","message_type":"NEVR_SELF_CHECK","userid":"<platform prefix>-<id>","check":"<name>",
//    "pass":true,"expected":"...","observed":"...","seq":3,"build":"4.0.0-rc.1+<commit>"}
// The game service keeps an unknown `message` as a generic remote log and journals it per user
// (server/evr_remotelogset.go, evr/login_remotelogset_messages.go); none of the names its filter drops.
//
// Budget (the service takes at most 50 strings and 256 KiB per set): a frame carries at most
// kMaxStringsPerFrame results of at most kMaxTextBytes per text field, a check reports at most
// kMaxResultsPerCheck results per session and then one "capped" result, and the queue held before login
// keeps the newest kMaxQueued and says how many it dropped.
//
// Threading: every function takes the unit's own mutex briefly (Enabled, Report and Flush return without it
// while the unit is off). Register and Report allocate: neither may run under the loader lock (a DLL's static
// initialiser, a DllMain, an LdrDllNotification callback) or inside a game function whose caller holds a lock;
// those paths only bump an atomic they already own, and a probe reads it on Flush. Flush runs the probes and
// the sender on the thread that calls it: on PC that is the per-frame tick (frame/tick.cpp), which the
// engine's own hooks drive on the game's thread, so a probe must not block and the sender is the bridge's
// non-blocking frame queue; on Quest it is the token-auth poll thread.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "runtime/compat/evr_codec.h"

namespace nevr_self_check {

inline constexpr std::size_t kMaxStringsPerFrame = 16;
inline constexpr std::size_t kMaxResultsPerCheck = 8;
inline constexpr std::size_t kMaxQueued = 64;
inline constexpr std::size_t kMaxTextBytes = 160;
inline constexpr std::size_t kMaxChecks = 64;
inline constexpr uint64_t kRemoteLogLevelInfo = 2;  // the game's WriteLog mask for info lines

using CheckId = int;  // -1: not registered (disabled, or the table is full)

struct Observation {
  std::string observed;
  bool pass = false;
};

// Polled by Flush. Returns true when it has a new result for the check; false when there is nothing to say.
// Runs on whatever thread calls Flush, so it reads atomics and nothing that blocks.
using Probe = bool (*)(Observation* out);

struct CheckSpec {
  const char* name;      // stable, lower_snake_case: what a run card calls the check
  const char* expected;  // what a pass looks like, in words, for the reader of the log
  Probe probe;           // may be null for an event-fired check
};

// What the log sink is given for every result as it happens (the build's own log line).
struct LogRecord {
  std::string name;
  std::string expected;
  std::string observed;
  bool pass = false;
};

// Takes one complete wire frame (BuildRemoteLogSet) for the login connection. Returns false when it could not
// be handed over; the results stay queued for the next Flush.
using Sender = bool (*)(const std::string& frame);
using LogSink = void (*)(const LogRecord& record);

// Off by default: a normal build registers nothing and sends nothing.
void SetEnabled(bool enabled);
bool Enabled();
void SetSender(Sender sender);
void SetLogSink(LogSink sink);
// The build string every result carries (version with the release-candidate label and commit).
void SetBuild(std::string_view build);
// Set at LoginSuccess with the user the service named; false when the login session ends. Nothing is sent
// while this is false (the service drops remote logs from a session that has not logged in).
void SetLoggedIn(bool loggedIn, const nevr_evr_codec::UserId& user = nevr_evr_codec::UserId());

CheckId Register(const CheckSpec& spec);
void Report(CheckId id, std::string_view observed, bool pass);
// Runs the probes, then sends what is queued (if logged in and a sender is set), kMaxStringsPerFrame at a time.
void Flush();

// Whether a connection's upgrade should ask the game service for every remote log category (`debug=true`):
// the login connection (index 1) of a client build with the unit on, never a dedicated game server.
inline bool WantsRemoteDebug(int connIdx, bool isServer) { return connIdx == 1 && !isServer && Enabled(); }

// Test seam: forgets every registration, the queue and the counters; leaves nothing set.
void ResetForTest();

}  // namespace nevr_self_check
