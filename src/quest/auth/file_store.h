#pragma once
// POSIX (Android) adapters: the credential cache file and the "where to log in" link file.

#include "quest/auth/session.h"

#include <string>

namespace nevr::quest_auth {

// The login link is not a long-lived secret and the player has to be able to read it, so
// it goes to the app's external files directory (/sdcard is FUSE: the file mode bits are
// not enforced there, so nothing long-lived may be written to it).
inline constexpr char kQuestFilesDir[] = "/sdcard/Android/data/com.readyatdawn.r15/files/";
inline constexpr char kCredentialsFileName[] = ".credentials.json";
inline constexpr char kLoginLinkFileName[] = "device_login.txt";

// The refresh token is a long-lived credential: it lives in the app-internal directory
// (/data/user/<user>/<package>/files), where Android enforces owner-only access. The package
// is taken from the process's own command line and the Android user from its uid
// (uid / 100000), so a secondary user gets its own directory; user 0's /data/user/0 is what
// /data/data points at. Returns "" when the command line does not look like a package name
// (a ":service" process, an unexpected launcher); the caller must not fall back to external
// storage.
std::string AppInternalFilesDirFromCmdline(const std::string& cmdline, unsigned uid);
std::string AppInternalFilesDir();

// Reads/writes `<dir>/.credentials.json`. Save writes a fresh sibling temp file (created
// exclusively, mode 0600, never through a symlink), fsyncs it, renames it over the target
// and fsyncs the directory, so a crash or a full disk mid-write leaves the previous file
// whole. Load refuses a symlink. An empty path means "no private directory": both
// operations fail and say so.
class FileCredentialStore : public CredentialStore {
 public:
  FileCredentialStore(std::string path, nevr::auth::LogSink log);
  CachedAuthToken Load(uint64_t now) override;
  bool Save(const CachedAuthToken& auth) override;

 private:
  std::string path_;
  nevr::auth::LogSink log_;
};

// Writes what a person holding the headset needs to `<dir>/device_login.txt`, replacing any file left by
// an earlier session, and removes it when the login ends. Four lines:
//   URL: <verification page>
//   Code: <code>
//   <one line of instructions, with the direct link that has the code filled in>
//   Expires: <UTC time> (unix <seconds>)
// One JSON line per attempt, {"event":"login_prompt","mechanism":"file","result":"written",
// "path":...} at Info (never the code). If the directory is not writable, the line says
// "write_failed" with the reason at Error and an Info line carries the direct link instead (the
// code is part of it), so the player can still read it from the log; the login goes on.
class FileLinkPresenter : public LinkPresenter {
 public:
  FileLinkPresenter(std::string path, nevr::auth::LogSink log);
  intptr_t Present(const LoginPrompt& prompt) override;
  void Clear() override;

 private:
  std::string path_;
  nevr::auth::LogSink log_;
};

std::string JoinPath(const std::string& dir, const std::string& name);

}  // namespace nevr::quest_auth
