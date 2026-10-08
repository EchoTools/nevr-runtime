#include "quest/auth/quest_token_auth.h"

namespace nevr::quest_auth {

namespace {
SessionConfig MakeSessionConfig(const QuestAuthConfig& c) {
  SessionConfig s;
  s.base_url = c.base_url;
  s.http_key = c.http_key;
  s.login_url = c.login_url;
  return s;
}

std::string CredentialsPath(const QuestAuthConfig& c, const nevr::auth::LogSink& log) {
  std::string dir = c.credentials_dir.empty() ? AppInternalFilesDir() : c.credentials_dir;
  if (dir.empty()) {
    if (log) {
      log(nevr::auth::LogLevel::Error,
          "[NEVR.AUTH] could not derive the app-internal directory from /proc/self/cmdline; the refresh token "
          "will not be persisted (external storage is never used for it)");
    }
    return "";
  }
  return JoinPath(dir, kCredentialsFileName);
}
}  // namespace

QuestTokenAuth::QuestTokenAuth(QuestAuthConfig config, nevr::auth::LogSink log)
    : http_(config.ca_dirs, log),
      store_(CredentialsPath(config, log), log),
      presenter_(JoinPath(config.files_dir, kLoginLinkFileName), log),
      session_(MakeSessionConfig(config), http_, clock_, store_, presenter_, log) {}

QuestTokenAuth::~QuestTokenAuth() { Stop(); }

void QuestTokenAuth::Start() { session_.Start(); }
void QuestTokenAuth::Stop() { session_.Stop(); }
std::string QuestTokenAuth::Token() const { return session_.Token(); }
uint64_t QuestTokenAuth::DiscordId() const { return session_.Get().discord_id; }
Snapshot QuestTokenAuth::Get() const { return session_.Get(); }

}  // namespace nevr::quest_auth
