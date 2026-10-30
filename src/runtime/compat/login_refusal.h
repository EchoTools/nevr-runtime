#pragma once
// What a refused login does (#1254).
//
// A server that has no credential for the community service, or whose login the service refuses, can do
// nothing useful: ServerDB registration needs the same account, and the HTTP listener binds only after the
// game has logged in. It used to connect without credentials (or send account 0) and retry the refusal every
// few seconds forever, while a supervisor watching the port saw nothing. So the bridge stops it with the
// cause (ServerFatal), and the cause names what is missing and which config was loaded.
//
// A client cannot stop. The game reconnects a closed login socket on its own, so a refusal that is acted on
// every time becomes a network loop. The client says it once per refused sign-in and answers the later sockets
// of that sign-in without a network trip (ClientRefusalLatch).
//
// Platform neutral and pure: no sockets, no logging, no game state. The caller (ws_bridge.cpp) owns the latch
// and holds its own lock around it.

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace nevr_login_refusal {

// The config.yaml keys a server's bridge login needs (N133 S4a). The order is the order they are named in.
inline constexpr std::string_view kDiscordIdKey = "identity.discord_id";
inline constexpr std::string_view kPasswordKey = "auth.password";

// A server signs in one of two ways (#1532). With auth.password set it presents identity.discord_id and the
// password. With none it signs in by device code, like a client: token-auth prints the code and the page on the
// console, waits for the approval and the bridge presents the token it got.
inline bool ServerSignsInByDeviceCode(std::string_view password) { return password.empty(); }

// The keys of the password credential that are unset or empty: only identity.discord_id, and only for a server that
// has a password. Empty result: the password credential is complete, or the server signs in by device code.
inline std::vector<std::string> MissingServerCredentialKeys(std::string_view discordId, std::string_view password) {
  std::vector<std::string> missing;
  if (ServerSignsInByDeviceCode(password)) return missing;
  if (discordId.empty()) missing.emplace_back(kDiscordIdKey);
  return missing;
}

// The statuses of an upgrade the community service refuses for the account presented: unauthenticated or
// forbidden. A 5xx, a 404 or a transport failure is the service being down or misaddressed, not a refusal.
inline bool IsLoginRefusalStatus(int httpStatus) { return httpStatus == 401 || httpStatus == 403; }

// What to tell an operator about the config: the path that was loaded, or where the search looked.
inline std::string DescribeConfigSource(std::string_view loadedPath, const std::vector<std::string>& searched) {
  if (!loadedPath.empty()) return std::string(loadedPath);
  std::string text = "none found";
  for (std::size_t i = 0; i < searched.size(); ++i) text += (i == 0 ? ", searched " : ", ") + searched[i];
  return text;
}

// Where the config came from, for the stop line. The caller passes the loaded path, or "none found, searched
// <dirs>"; an empty description reads "none found".
inline std::string ConfigSourceText(std::string_view configSource) {
  return configSource.empty() ? std::string("none found") : std::string(configSource);
}

inline std::string JoinKeys(const std::vector<std::string>& keys) {
  std::string joined;
  for (const std::string& key : keys) {
    if (!joined.empty()) joined += ", ";
    joined += key;
  }
  return joined;
}

// The one stop line of a server with no credential. It is the cause handed to ServerFatal, so it is also the
// line the operator reads: the cause, the config that was loaded, and the keys to set.
inline std::string NoCredentialCause(const std::vector<std::string>& missing, std::string_view configSource) {
  const std::string keys = JoinKeys(missing);
  return "server login refused: no credential configured (missing " + keys + "); config: " +
         ConfigSourceText(configSource) + "; set " + keys + " in config.yaml";
}

// The one stop line of a server with no password whose device sign-in ended without a token: the code was not
// approved in time, or the service could not be asked. The operator reads what to do.
inline std::string NoSignInCause(std::string_view configSource) {
  return "server login refused: " + std::string(kPasswordKey) +
         " is not set and the device sign-in ended without a token; config: " + ConfigSourceText(configSource) +
         "; approve the code printed on the console within 5 minutes, or set " + std::string(kDiscordIdKey) +
         " and " + std::string(kPasswordKey) + " in config.yaml";
}

// The one stop line of a server whose credential the community service refused. `deviceSignIn`: the server had no
// password and presented its device sign-in token, so the config keys are not what to check.
inline std::string RefusedLoginCause(int httpStatus, std::string_view configSource, bool deviceSignIn = false) {
  if (deviceSignIn) {
    return "server login refused: the service refused the device sign-in token (HTTP " + std::to_string(httpStatus) +
           "); config: " + ConfigSourceText(configSource) +
           "; delete .credentials.json beside config.yaml and start the server to sign in again";
  }
  return "server login refused: the service refused the credential (HTTP " + std::to_string(httpStatus) +
         "); config: " + ConfigSourceText(configSource) + "; check " + std::string(kDiscordIdKey) + " and " +
         std::string(kPasswordKey) + " in config.yaml";
}

// #1293: the one line a client says for a refused sign-in. It names the status, the config that was loaded and what
// to do; the server's keys do not apply, because a client signs in with the token it presents.
inline std::string ClientRefusedCause(int httpStatus, std::string_view configSource) {
  return "the service did not accept this sign-in (HTTP " + std::to_string(httpStatus) +
         "); config: " + ConfigSourceText(configSource) +
         "; not asking again until the sign-in changes; restart Echo VR to sign in again";
}

// A client says a refusal once per sign-in. The key is the bearer the refused socket presented (empty: a
// client with no sign-in), so a different sign-in is a new refusal and says it again.
class ClientRefusalLatch {
 public:
  // True the first time `bearer` is refused, false for every later refusal of it.
  bool ShouldSay(const std::string& bearer) { return refused_.insert(bearer).second; }
  // True once `bearer` has been refused: the caller answers without opening the remote.
  bool IsRefused(const std::string& bearer) const { return refused_.count(bearer) != 0; }

 private:
  std::set<std::string> refused_;
};

// #1446: a client whose sign-in ended without a token answers the game's login itself. The game gets a LoginFailure
// and the service is not asked, because no later login of this run can carry a credential the service accepts.
//   status: 7, the status the legacy translator gives a 401 (docs/reference/legacy-translator.md).
inline constexpr uint64_t kNoSignInLoginFailureStatus = 7;
inline constexpr std::string_view kNoSignInLoginFailureText = "Sign-in did not finish.\nRestart Echo VR to sign in again.";

// True for the one case the bridge answers: a client's login socket that presents no token and no URL credentials,
// once the sign-in has ended without a token. A server, a config or matchmaker socket, a sign-in that is still
// running or a sign-in that presents a credential keeps the path that asks the service. A live login session is never
// answered here (#1446): answering frees the login slot, and the session still holds it with its remote open.
inline bool AnswersLoginLocally(bool isServer, bool isLoginSocket, bool bearerPresent, bool urlCredentials,
                                bool signInEnded, bool loginSessionLive) {
  return !isServer && isLoginSocket && !bearerPresent && !urlCredentials && signInEnded && !loginSessionLive;
}

// The log of the answers the bridge gives locally (#1446). The first answer is written at once. Later answers are
// counted, and the first answer after each window writes one line with the answers since the last line, this one
// included, so a game that reconnects every few seconds writes a line per window, not one per attempt.
class LocalAnswerCounter {
 public:
  static constexpr int64_t kWindowMs = 60000;

  struct Verdict {
    bool write = false;   // a line is due now
    bool first = false;   // the line is the first answer, not a window's summary
    uint64_t count = 0;   // answers the line stands for
  };

  Verdict Record(int64_t nowMs) {
    ++total_;
    if (!started_) {
      started_ = true;
      windowStartMs_ = nowMs;
      reported_ = total_;
      return Verdict{true, true, 1};
    }
    if (nowMs - windowStartMs_ < kWindowMs) return Verdict{};
    windowStartMs_ = nowMs;
    const uint64_t since = total_ - reported_;
    reported_ = total_;
    return Verdict{true, false, since};
  }

 private:
  uint64_t total_ = 0;
  uint64_t reported_ = 0;
  int64_t windowStartMs_ = 0;
  bool started_ = false;
};

}  // namespace nevr_login_refusal
