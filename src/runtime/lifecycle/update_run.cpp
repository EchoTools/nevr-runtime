#include "runtime/lifecycle/update_run.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/host/update_names.h"

namespace nevr_update {
namespace {

using nlohmann::json;
namespace names = nevr_host::update_names;

// ---- paths: composed here from the nevr root and the host's closed vocabulary, never from a file's text --------------------

std::wstring UpdateDir(const std::wstring& root) { return root + L"\\" + names::kUpdateDir; }
std::wstring UpdateFile(const std::wstring& root, const wchar_t* name) { return UpdateDir(root) + L"\\" + name; }
std::wstring StagingDir(const std::wstring& root) { return UpdateDir(root) + L"\\" + names::kStagingDir; }
std::wstring ToWide(const std::string& ascii) { return std::wstring(ascii.begin(), ascii.end()); }  // a version: the vocabulary is ASCII

// The core takes UTF-8. A profile folder with a letter outside ASCII (%LOCALAPPDATA% under the player's own name) must reach it
// whole, not as the low byte of each UTF-16 unit.
std::string Utf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

bool IsReparsePoint(const std::wstring& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ---- reading and writing the files of update\ ------------------------------------------------------------------------------

enum class Read { kOk, kAbsent, kUnreadable, kTooBig };

// A file of update\, at most the cap the host applies (ADR 0024, section 4.3).
Read ReadSmallFile(const std::wstring& path, std::string* out) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD e = GetLastError();
    return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? Read::kAbsent : Read::kUnreadable;
  }
  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) == FALSE) {
    CloseHandle(file);
    return Read::kUnreadable;
  }
  if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > names::kMaxJsonBytes) {
    CloseHandle(file);
    return Read::kTooBig;
  }
  std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
  DWORD got = 0;
  const BOOL ok = text.empty() ? TRUE : ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &got, nullptr);
  CloseHandle(file);
  if (ok == FALSE || got != text.size()) return Read::kUnreadable;
  *out = std::move(text);
  return Read::kOk;
}

// A temporary file beside the target, then a rename over it: a reader never sees half a file. The name is the host's own
// spelling (`<file>.tmp-<pid>`), which a start removes after a kill between the write and the rename.
bool WriteByRename(const std::wstring& path, const std::string& text) {
  const std::wstring temporary = path + L".tmp-" + std::to_wstring(GetCurrentProcessId());
  const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD wrote = 0;
  const BOOL ok = text.empty() ? TRUE : WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
  const BOOL flushed = ok != FALSE && FlushFileBuffers(file) != FALSE;
  CloseHandle(file);
  if (!flushed || wrote != text.size() ||
      MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
    DeleteFileW(temporary.c_str());
    return false;
  }
  return true;
}

// Removes a folder and what is in it. Never follows a link: a reparse point is removed itself and not entered.
void RemoveTree(const std::wstring& dir) {
  if (!IsReparsePoint(dir)) {
    WIN32_FIND_DATAW f;
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring name = f.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring child = dir + L"\\" + name;
        if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
          RemoveTree(child);
        } else {
          SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
          DeleteFileW(child.c_str());
        }
      } while (FindNextFileW(h, &f) != FALSE);
      FindClose(h);
    }
  }
  RemoveDirectoryW(dir.c_str());
}

// ---- log lines ---------------------------------------------------------------------------------------------------------------

using Fields = std::vector<std::pair<std::string, std::string>>;

// A value is a word: anything outside [A-Za-z0-9._+:/-] becomes '_', so a value can neither start a second field nor a second line.
std::string Word(const std::string& v) {
  std::string out = v.size() > 96 ? v.substr(0, 96) : v;
  for (char& c : out) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!(std::isalnum(u) != 0 || c == '.' || c == '_' || c == '+' || c == ':' || c == '/' || c == '-')) c = '_';
  }
  return out.empty() ? std::string("none") : out;
}

void Emit(const LogFn& log, int level, const std::string& event, const Fields& fields) {
  if (!log) return;
  std::string line = "[NEVR.UPDATE] " + event;
  for (const auto& [key, value] : fields) line += " " + key + "=" + Word(value);
  log(level, line);
}

std::string VersionWord(const std::string& v) { return names::IsVersionName(v) ? v : std::string("unknown"); }

// ---- the core's error text is classified, never logged (logging Rule 4: remote text is not a field) -----------------------

// A manifest that failed a check of the method (ADR 0023: "FAIL step N") is not the network.
const char* ManifestReason(const std::string& core_error) {
  return core_error.find("FAIL step") != std::string::npos ? "manifest_check" : "network";
}

struct CoreRun {
  const Core& core;
  bool opened = false;
  explicit CoreRun(const Core& c) : core(c) {}
  CoreRun(const CoreRun&) = delete;
  CoreRun& operator=(const CoreRun&) = delete;
  // Ends the run and closes the session on every path out, however the check ended.
  void Finish(const std::string& state, const std::string& reason) noexcept {
    if (!opened) return;
    opened = false;
    try {
      if (core.run_end) core.run_end(state, reason);
      if (core.close) core.close();
    } catch (const std::exception&) {  // the core's adapter must not throw; a stray one must not end the game's process
    }
  }
  ~CoreRun() { Finish("unchanged", "abandoned"); }
};

}  // namespace

const char* CheckActionWord(CheckAction action) {
  switch (action) {
    case CheckAction::kNone: return "none";
    case CheckAction::kStaged: return "staged";
    case CheckAction::kBlocked: return "blocked";
    case CheckAction::kFailed: return "failed";
  }
  return "failed";
}

const char* ConfirmResultWord(ConfirmResult result) {
  switch (result) {
    case ConfirmResult::kConfirmed: return "confirmed";
    case ConfirmResult::kNotPending: return "no_pending";
    case ConfirmResult::kNotThePendingRelease: return "not_the_pending_release";
    case ConfirmResult::kPendingUnreadable: return "pending_unreadable";
    case ConfirmResult::kStateUnreadable: return "state_unreadable";
    case ConfirmResult::kLockUnavailable: return "lock_unavailable";
    case ConfirmResult::kWriteFailed: return "write_failed";
  }
  return "write_failed";
}

UpdateFileLock::~UpdateFileLock() { Release(); }

bool UpdateFileLock::Acquire(const std::wstring& update_dir, DWORD timeout_ms, DWORD step_ms, DWORD* win32_error) noexcept {
  if (Held()) return true;
  std::wstring path;
  try {
    path = update_dir + L"\\" + names::kLockFile;
  } catch (const std::exception&) {  // out of memory: no lock
    if (win32_error != nullptr) *win32_error = ERROR_NOT_ENOUGH_MEMORY;
    return false;
  }
  const ULONGLONG started = GetTickCount64();
  for (;;) {
    // The host's own flags (runtime/host/update_swap.cpp, UpdateLock::Acquire): exclusive, delete on close.
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      handle_ = file;
      return true;
    }
    if (win32_error != nullptr) *win32_error = GetLastError();
    if (GetTickCount64() - started >= timeout_ms) return false;
    Sleep(step_ms == 0 ? 1 : step_ms);
  }
}

void UpdateFileLock::Release() noexcept {
  if (handle_ == INVALID_HANDLE_VALUE) return;
  CloseHandle(handle_);
  handle_ = INVALID_HANDLE_VALUE;
}

// ---- the check -----------------------------------------------------------------------------------------------------------------

CheckOutcome RunCheck(const CheckEnv& env) noexcept {
  CheckOutcome out;
  try {
    const std::uint64_t started = env.now_ms ? env.now_ms() : 0;
    Channel channel;
    // One line per check, whatever the outcome: every return below goes through `done`.
    const auto done = [&](CheckAction action, const std::string& reason, const std::string& target,
                          const std::string& direction) -> CheckOutcome& {
      out.action = action;
      out.reason = reason;
      out.version = target;
      out.direction = direction;
      const std::uint64_t now = env.now_ms ? env.now_ms() : started;
      Fields f = {{"channel", channel.name.empty() ? std::string("core_default") : channel.name},
                  {"channel_source", ChannelSourceWord(channel.source)},
                  {"current", VersionWord(env.version)}};
      if (!target.empty()) f.push_back({"target", VersionWord(target)});
      if (!direction.empty()) f.push_back({"direction", direction});
      f.push_back({"action", CheckActionWord(action)});
      if (!reason.empty()) f.push_back({"reason", reason});
      if (channel.config_rejected) f.push_back({"config_channel_rejected", "true"});
      f.push_back({"elapsed_ms", std::to_string(now >= started ? now - started : 0)});
      Emit(env.log, action == CheckAction::kFailed ? kWarning : kInfo, "update check", f);
      return out;
    };

    const std::wstring update = UpdateDir(env.root);
    // update\ is composed from the nevr root and nothing else; one that is a link is never written through (section 4.2).
    if (IsReparsePoint(update) || IsReparsePoint(StagingDir(env.root))) return done(CheckAction::kBlocked, "reparse_point", "", "");

    // A release the host has swapped in and not yet seen start is the last confirmed one's replacement; staging another would move
    // an unconfirmed release over it (section 6, Q1). Nothing is fetched.
    if (Exists(UpdateFile(env.root, names::kPendingJson))) return done(CheckAction::kNone, "release_pending", "", "");

    std::string state_text;
    std::optional<std::string> state_channel;
    std::vector<QuarantineEntry> quarantine;
    if (ReadSmallFile(UpdateFile(env.root, names::kStateJson), &state_text) == Read::kOk) {
      state_channel = StateChannel(state_text);
      quarantine = StateQuarantine(state_text);
    }
    channel = ResolveChannel(env.config_channel, state_channel);

    const std::string root_utf8 = Utf8(env.root);
    if (root_utf8.empty()) return done(CheckAction::kFailed, "root_unconvertible", "", "");
    json config = {{"platform", "pc"}, {"nevr_root", root_utf8}};
    if (!channel.name.empty()) config["channel"] = channel.name;

    CoreRun run(env.core);
    std::string error;
    if (!env.core.open || !env.core.open(config.dump(), &error)) {
      return done(CheckAction::kFailed, "core_open_failed", "", "");
    }
    run.opened = true;
    if (!env.core.manifest_fetch || !env.core.manifest_fetch(&error)) {
      const char* reason = ManifestReason(error);
      run.Finish("unchanged", reason);
      return done(CheckAction::kFailed, reason, "", "");
    }

    std::string result_text;
    if (!env.core.stage || !env.core.stage(root_utf8,
                                           BuildCurrentJson(env.version, env.runtime_sha256, quarantine), &result_text, &error)) {
      run.Finish("unchanged", "stage_failed");
      return done(CheckAction::kFailed, "stage_failed", "", "");
    }
    run.Finish("unchanged", "checked");

    // The whole stage's budget: a result that arrives after it is not published, whatever it holds.
    const auto over_budget = [&] { return env.now_ms && env.now_ms() - started > env.budget_ms; };
    if (over_budget()) return done(CheckAction::kFailed, "timeout", "", "");

    StageResult staged;
    if (!ParseStageResult(result_text, &staged, &error)) return done(CheckAction::kFailed, "bad_result", "", "");
    if (staged.action != "staged") {
      return done(staged.action == "none" ? CheckAction::kNone : CheckAction::kBlocked, staged.reason, staged.version, staged.direction);
    }

    // The core checked every byte against the signed manifest. The Authenticode check sits between that and the publish: it is
    // this runtime's, with the production policy (ADR 0024, section 3).
    const std::wstring folder = StagingDir(env.root) + L"\\" + ToWide(staged.version);
    if (IsReparsePoint(folder)) return done(CheckAction::kBlocked, "reparse_point", staged.version, staged.direction);
    bool all_ok = true;
    for (const StagedFile& file : staged.files) {
      names::Member member;
      names::ParseMember(file.path, &member);  // parsed once already (ParseStageResult)
      const std::wstring path = folder + L"\\" + names::MemberRelativePath(member);
      VerifyOutcome v;
      if (env.verify) v = env.verify(path);
      Emit(env.log, v.ok ? kInfo : kWarning, "update verify",
           {{"path", file.path}, {"version", staged.version}, {"ok", v.ok ? "true" : "false"},
            {"reason", v.ok ? std::string("accepted") : v.reason}});
      all_ok = all_ok && v.ok;
    }
    if (!all_ok) {
      RemoveTree(folder);  // staging removed; nothing was published
      return done(CheckAction::kBlocked, "verify_failed", staged.version, staged.direction);
    }
    if (over_budget()) return done(CheckAction::kFailed, "timeout", staged.version, staged.direction);

    UpdateFileLock lock;
    DWORD lock_error = 0;
    if (!lock.Acquire(update, env.lock_timeout_ms, kLockStepMs, &lock_error)) {
      return done(CheckAction::kFailed, "lock_unavailable", staged.version, staged.direction);
    }
    // Under the lock again: a start may have swapped something in while this one downloaded.
    if (Exists(UpdateFile(env.root, names::kPendingJson))) return done(CheckAction::kNone, "release_pending", staged.version, staged.direction);
    // staged.json is published last: its existence is the host's signal that there is something to swap.
    if (!WriteByRename(UpdateFile(env.root, names::kStagedJson), BuildStagedJson(staged))) {
      return done(CheckAction::kFailed, "publish_failed", staged.version, staged.direction);
    }
    lock.Release();
    Emit(env.log, kInfo, "update published", {{"version", staged.version}, {"files", std::to_string(staged.files.size())}});
    return done(CheckAction::kStaged, "", staged.version, staged.direction);
  } catch (const std::exception&) {  // out of memory or a stray exception from the core's adapter: the check ends, the game goes on
    out.action = CheckAction::kFailed;
    out.reason = "internal_error";
    Emit(env.log, kWarning, "update check", {{"action", "failed"}, {"reason", "internal_error"}});
    return out;
  }
}

// ---- the confirmation ----------------------------------------------------------------------------------------------------------

ConfirmResult UpdateConfirm(const ConfirmEnv& env, std::uint64_t after_ms) noexcept {
  try {
    const auto skipped = [&](ConfirmResult r, const std::string& version = std::string()) {
      Fields f = {{"reason", ConfirmResultWord(r)}};
      if (!version.empty()) f.push_back({"version", version});
      Emit(env.log, r == ConfirmResult::kNotPending ? kDebug : kInfo, "update confirm skipped", f);
      return r;
    };
    const std::wstring update = UpdateDir(env.root);
    if (IsReparsePoint(update)) return skipped(ConfirmResult::kPendingUnreadable);

    std::string text;
    PendingTarget target;
    const Read first = ReadSmallFile(UpdateFile(env.root, names::kPendingJson), &text);
    if (first == Read::kAbsent) return skipped(ConfirmResult::kNotPending);
    if (first != Read::kOk || !ParsePendingTarget(text, &target)) return skipped(ConfirmResult::kPendingUnreadable);
    // A runtime that is not the pending one confirms nothing: a second process that still maps the old file (or an old
    // launcher's runtime) must not mark the new release as started.
    if (target.sha256 != env.runtime_sha256) return skipped(ConfirmResult::kNotThePendingRelease, target.version);

    UpdateFileLock lock;
    DWORD lock_error = 0;
    if (!lock.Acquire(update, env.lock_timeout_ms, kLockStepMs, &lock_error)) return skipped(ConfirmResult::kLockUnavailable, target.version);
    // Under the lock: the host may have rolled the release back since the first read.
    if (ReadSmallFile(UpdateFile(env.root, names::kPendingJson), &text) != Read::kOk || !ParsePendingTarget(text, &target) ||
        target.sha256 != env.runtime_sha256) {
      return skipped(ConfirmResult::kNotThePendingRelease, target.version);
    }
    std::string state;
    const Read state_read = ReadSmallFile(UpdateFile(env.root, names::kStateJson), &state);
    if (state_read == Read::kAbsent) state = "{}";
    else if (state_read != Read::kOk) return skipped(ConfirmResult::kStateUnreadable, target.version);
    std::string confirmed;
    // A state file that is not an object is damage, and the host rebuilds it; it is not written over here.
    if (!ConfirmState(state, target.version, target.sha256, &confirmed)) return skipped(ConfirmResult::kStateUnreadable, target.version);
    if (!WriteByRename(UpdateFile(env.root, names::kStateJson), confirmed)) return skipped(ConfirmResult::kWriteFailed, target.version);
    // state.json first, pending.json after: a kill between them leaves a pending record that names the confirmed runtime, which
    // the host drops as stale (section 6), never a pending record with no state behind it.
    DeleteFileW(UpdateFile(env.root, names::kPendingJson).c_str());
    DeleteFileW(UpdateFile(env.root, names::kCrashedFile).c_str());
    lock.Release();
    Emit(env.log, kInfo, "update confirmed",
         {{"version", target.version}, {"after_ms", std::to_string(after_ms)}, {"role", env.role != nullptr ? env.role : "client"}});
    return ConfirmResult::kConfirmed;
  } catch (const std::exception&) {  // out of memory: the confirmation is tried again at the next start
    Emit(env.log, kWarning, "update confirm skipped", {{"reason", "internal_error"}});
    return ConfirmResult::kWriteFailed;
  }
}

}  // namespace nevr_update
