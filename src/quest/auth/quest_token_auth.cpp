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
    : log_(log),
      http_(config.ca_dirs, log),
      store_(CredentialsPath(config, log), log),
      file_presenter_(JoinPath(config.files_dir, kLoginLinkFileName), log),
      game_presenter_(log),
      presenter_({{"file", &file_presenter_}, {"game_error_text", &game_presenter_}}, log),
      session_(MakeSessionConfig(config), http_, clock_, store_, presenter_, log) {}

std::unique_ptr<QuestTokenAuth> QuestTokenAuth::Create(QuestAuthConfig config, nevr::auth::LogSink log) noexcept {
  try {
    return std::unique_ptr<QuestTokenAuth>(new QuestTokenAuth(std::move(config), log));
  } catch (const std::exception& e) {
    try {
      if (log) log(nevr::auth::LogLevel::Error, std::string("[NEVR.AUTH] token auth could not be created: ") + e.what());
    } catch (const std::exception&) {
      // The sink failed too; there is nowhere left to report to.
    }
    return nullptr;
  }
}

QuestTokenAuth::~QuestTokenAuth() { Stop(); }

void QuestTokenAuth::ReportFailure(const char* what, const char* where) const noexcept {
  try {
    if (log_) log_(nevr::auth::LogLevel::Error, std::string("[NEVR.AUTH] ") + where + " threw: " + what);
  } catch (const std::exception&) {
    // The sink itself failed; there is nowhere left to report to.
  }
}

void QuestTokenAuth::Start() noexcept {
  try {
    session_.Start();
  } catch (const std::exception& e) {
    ReportFailure(e.what(), "Start");
  }
}

void QuestTokenAuth::Stop() noexcept {
  try {
    session_.Stop();
  } catch (const std::exception& e) {
    ReportFailure(e.what(), "Stop");
  }
}

std::string QuestTokenAuth::Token() const noexcept {
  try {
    return session_.Token();
  } catch (const std::exception& e) {
    ReportFailure(e.what(), "Token");
    return "";
  }
}

uint64_t QuestTokenAuth::DiscordId() const noexcept {
  try {
    return session_.Get().discord_id;
  } catch (const std::exception& e) {
    ReportFailure(e.what(), "DiscordId");
    return 0;
  }
}

Snapshot QuestTokenAuth::Get() const noexcept {
  try {
    return session_.Get();
  } catch (const std::exception& e) {
    ReportFailure(e.what(), "Get");
    Snapshot failed;
    failed.readiness = Readiness::Failed;
    return failed;
  }
}

}  // namespace nevr::quest_auth
