#pragma once
// Quest entry point for token auth: the Android collaborators (libcurl over the system
// CA store, the cache file under the app's external files dir, SystemClock) wired into
// a Session. NOT yet called from the sentinel: the hook that hands the token to the
// login path is gated by ADR 0003; this is the part that can be built and tested now.

#include "quest/auth/curl_http.h"
#include "quest/auth/file_store.h"
#include "quest/auth/session.h"

#include <memory>
#include <string>

namespace nevr::quest_auth {

inline constexpr char kDefaultLoginUrl[] = "https://echovrce.com/login/device";

struct QuestAuthConfig {
  std::string base_url;  // public nakama HTTP endpoint
  std::string http_key;  // public RPC key (a credential for the RPC; never logged)
  std::string files_dir = kQuestFilesDir;
  std::string login_url = kDefaultLoginUrl;
  std::string ca_dir = kAndroidSystemCaDir;
};

class QuestTokenAuth {
 public:
  QuestTokenAuth(QuestAuthConfig config, nevr::auth::LogSink log);
  ~QuestTokenAuth();

  // Returns at once; the login runs on a worker thread.
  void Start();
  void Stop();
  std::string Token() const;
  uint64_t DiscordId() const;
  Snapshot Get() const;

 private:
  SystemClock clock_;
  CurlHttpClient http_;
  FileCredentialStore store_;
  FileLinkPresenter presenter_;
  Session session_;
};

}  // namespace nevr::quest_auth
