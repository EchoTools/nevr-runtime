#pragma once
// POSIX (Android) adapters: the credential cache file and the "where to log in" link file.

#include "quest/auth/session.h"

#include <string>

namespace nevr::quest_auth {

// Quest app-private external storage; the cache lives here, never beside the game data.
inline constexpr char kQuestFilesDir[] = "/sdcard/Android/data/com.readyatdawn.r15/files/";
inline constexpr char kCredentialsFileName[] = ".credentials.json";
inline constexpr char kLoginLinkFileName[] = "device_login.txt";

// Reads/writes `<dir>/.credentials.json`. Save writes a sibling temp file with mode
// 0600, fsyncs it and renames it over the target, so a crash or a full disk mid-write
// leaves the previous file whole.
class FileCredentialStore : public CredentialStore {
 public:
  FileCredentialStore(std::string path, nevr::auth::LogSink log);
  CachedAuthToken Load(uint64_t now) override;
  bool Save(const CachedAuthToken& auth) override;

 private:
  std::string path_;
  nevr::auth::LogSink log_;
};

// Writes the login URL (which carries the short-lived device code) to
// `<dir>/device_login.txt`, mode 0600, and removes it when the flow ends. The log
// gets the path only, never the URL.
class FileLinkPresenter : public LinkPresenter {
 public:
  FileLinkPresenter(std::string path, nevr::auth::LogSink log);
  intptr_t Present(const std::string& login_url_with_code) override;
  void Clear() override;

 private:
  std::string path_;
  nevr::auth::LogSink log_;
};

std::string JoinPath(const std::string& dir, const std::string& name);

}  // namespace nevr::quest_auth
