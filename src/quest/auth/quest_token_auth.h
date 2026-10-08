#pragma once
// Quest entry point for token auth: the Android collaborators (libcurl over the system
// CA store, the credential cache in the app-internal directory, the login-link file under
// the external files dir, SystemClock) wired into a Session. NOT yet called from the sentinel: the hook that hands the token to the
// login path is gated by ADR 0003; this is the part that can be built and tested now.

#include "quest/auth/curl_http.h"
#include "quest/auth/file_store.h"
#include "quest/auth/session.h"

#include <memory>
#include <string>
#include <vector>

namespace nevr::quest_auth {

inline constexpr char kDefaultLoginUrl[] = "https://echovrce.com/login/device";

struct QuestAuthConfig {
  std::string base_url;  // public nakama HTTP endpoint
  std::string http_key;  // public RPC key (a credential for the RPC; never logged)
  // Login link (readable by the player, not a long-lived secret).
  std::string files_dir = kQuestFilesDir;
  // Refresh-token cache. Empty = derive /data/user/<uid/100000>/<package>/files from the process; if
  // that fails the login still runs but is not persisted, and the log says so.
  std::string credentials_dir;
  std::string login_url = kDefaultLoginUrl;
  std::vector<std::string> ca_dirs = AndroidCaDirs();
};

class QuestTokenAuth {
 public:
  // Construction allocates and reads /proc/self/cmdline, so it can fail; it is a noexcept factory
  // for the caller that lives in game/loader frames. Returns nullptr, with the reason sent to `log`
  // (when the sink itself still works), instead of throwing.
  static std::unique_ptr<QuestTokenAuth> Create(QuestAuthConfig config, nevr::auth::LogSink log) noexcept;
  ~QuestTokenAuth();

  // Every call below is noexcept and catches std::exception itself: the hook that will call them
  // from game frames must never see an exception cross that boundary. A failure is logged and
  // reported as "no token" / a Failed snapshot.
  //
  // Returns at once; the login runs on a worker thread named "nevr-auth".
  void Start() noexcept;
  void Stop() noexcept;
  std::string Token() const noexcept;
  uint64_t DiscordId() const noexcept;
  Snapshot Get() const noexcept;

 private:
  QuestTokenAuth(QuestAuthConfig config, nevr::auth::LogSink log);
  void ReportFailure(const char* what, const char* where) const noexcept;

  nevr::auth::LogSink log_;
  SystemClock clock_;
  CurlHttpClient http_;
  FileCredentialStore store_;
  FileLinkPresenter presenter_;
  Session session_;
};

}  // namespace nevr::quest_auth
