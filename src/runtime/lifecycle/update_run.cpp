#include "runtime/lifecycle/update_run.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/update/names.h"

namespace nevr_update {
namespace {

using nlohmann::json;
namespace names = nevr_update_names;

// ---- paths: composed here from the nevr root and the closed vocabulary, never from a file's text ------------------------------

// The root and the file system it is read through. Every path under it is a UTF-8 string joined with the fs's separator.
struct Paths {
  const UpdateFs& fs;
  std::string root;
  std::string Join(const std::string& dir, const std::string& name) const { return dir + fs.Separator() + name; }
  std::string Update() const { return Join(root, names::kUpdateDir); }
  std::string UpdateFile(const char* name) const { return Join(Update(), name); }
  std::string Staging() const { return Join(Update(), names::kStagingDir); }
  // A member's path inside a release's folder, written with the fs's separator ("plugins/<name>.dll" has a slash in the files).
  std::string MemberIn(const std::string& folder, const names::Member& member) const {
    std::string rel = names::MemberPath(member);
    std::replace(rel.begin(), rel.end(), '/', fs.Separator());
    return Join(folder, rel);
  }
};

// The core and the fs take UTF-8. A profile folder with a letter outside ASCII (%LOCALAPPDATA% under the player's own name) must
// reach them whole, not as the low byte of each UTF-16 unit. Strict: text that is not UTF-16 (a lone surrogate) does not convert,
// and the caller stops rather than reach another path. An empty string converts to an empty string.
bool Utf8(const std::wstring& w, std::string* out) {
  out->clear();
  if (w.empty()) return true;
  const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return false;
  out->assign(static_cast<std::size_t>(n), '\0');
  return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), out->data(), n, nullptr, nullptr) == n;
}

using Read = ReadResult;

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

    // A root that is not UTF-16 text (or no root at all) cannot be given to the core or the fs: nothing is read from it, the channel is
    // the config's, and the check ends `failed reason=root_unconvertible`.
    std::string root_utf8;
    if (env.root.empty() || !Utf8(env.root, &root_utf8)) {
      channel = ResolveChannel(env.config_channel, std::nullopt);
      return done(CheckAction::kFailed, "root_unconvertible", "", "");
    }
    const UpdateFs& fs = *env.fs;
    const Paths paths{fs, root_utf8};
    const std::string update = paths.Update();
    // update\ is composed from the nevr root and nothing else; one that is a link is never written through (section 4.2).
    if (fs.IsLink(update) || fs.IsLink(paths.Staging())) return done(CheckAction::kBlocked, "reparse_point", "", "");

    // A release the host has swapped in and not yet seen start is the last confirmed one's replacement; staging another would move
    // an unconfirmed release over it (section 6, Q1). Nothing is fetched.
    if (fs.Exists(paths.UpdateFile(names::kPendingJson))) return done(CheckAction::kNone, "release_pending", "", "");

    std::string state_text;
    std::optional<std::string> state_channel;
    std::vector<QuarantineEntry> quarantine;
    if (fs.ReadSmallFile(paths.UpdateFile(names::kStateJson), &state_text) == Read::kOk) {
      state_channel = StateChannel(state_text);
      quarantine = StateQuarantine(state_text);
    }
    channel = ResolveChannel(env.config_channel, state_channel);

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
    if (!env.core.stage || !env.core.stage(root_utf8, BuildCurrentJson(env.version, env.runtime_sha256, quarantine), &result_text, &error)) {
      run.Finish("unchanged", "stage_failed");
      return done(CheckAction::kFailed, "stage_failed", "", "");
    }

    StageResult staged;
    if (!ParseStageResult(result_text, &staged, &error)) {
      run.Finish("unchanged", "bad_result");
      return done(CheckAction::kFailed, "bad_result", "", "");
    }
    const std::string folder = paths.Join(paths.Staging(), staged.version);  // the version passed the vocabulary
    // The whole stage's budget: a result that arrives after it is not published, whatever it holds, and what it staged is removed.
    const auto over_budget = [&] { return env.now_ms && env.now_ms() - started > env.budget_ms; };
    const auto late = [&]() -> CheckOutcome& {
      if (staged.action == "staged" && !fs.IsLink(folder)) fs.RemoveTree(folder);
      run.Finish("unchanged", "timeout");
      return done(CheckAction::kFailed, "timeout", staged.version, staged.direction);
    };
    if (over_budget()) return late();
    if (staged.action != "staged") {
      run.Finish("unchanged", staged.reason.empty() ? "checked" : staged.reason);
      return done(staged.action == "none" ? CheckAction::kNone : CheckAction::kBlocked, staged.reason, staged.version, staged.direction);
    }

    // The core checked every byte against the signed manifest. The Authenticode check sits between that and the publish: it is
    // this runtime's, with the production policy (ADR 0024, section 3).
    if (fs.IsLink(folder)) {
      run.Finish("unchanged", "reparse_point");
      return done(CheckAction::kBlocked, "reparse_point", staged.version, staged.direction);
    }
    bool all_ok = true;
    bool refused = false;
    for (const StagedFile& file : staged.files) {
      names::Member member;
      names::ParseMember(file.path, &member);  // parsed once already (ParseStageResult)
      std::wstring path;
      VerifyOutcome v;
      if (!Utf8ToWide(paths.MemberIn(folder, member), &path)) {
        v.reason = "path_unconvertible";  // not a verdict on the bytes: nothing is discarded for it
      } else if (env.verify) {
        v = env.verify(path);
      }
      Emit(env.log, v.ok ? kInfo : kWarning, "update verify",
           {{"path", file.path}, {"version", staged.version}, {"ok", v.ok ? "true" : "false"},
            {"reason", v.ok ? std::string("accepted") : v.reason}});
      all_ok = all_ok && v.ok;
      refused = refused || (!v.ok && v.refused);
    }
    if (!all_ok) {
      fs.RemoveTree(folder);  // staging removed; nothing was published
      if (refused) {
        // The verifier refused these bytes: the core records the release, and the next stage does not fetch it again. A file that
        // could not be read is not a refusal (a scanner's lock): nothing is recorded, and the next launch tries again.
        std::string runtime_sha;
        for (const StagedFile& f : staged.files) {
          if (f.path == names::kRuntimeMember) runtime_sha = f.sha256;
        }
        std::string discard_error;
        const bool recorded = env.core.discard && env.core.discard(root_utf8, staged.version, runtime_sha, "verify_failed", &discard_error);
        Emit(env.log, recorded ? kInfo : kWarning, "update discard",
             {{"version", staged.version}, {"recorded", recorded ? "true" : "false"}, {"reason", "verify_failed"}});
      }
      run.Finish("unchanged", "verify_failed");
      return done(CheckAction::kBlocked, "verify_failed", staged.version, staged.direction);
    }
    if (over_budget()) return late();
    run.Finish("unchanged", "checked");

    int lock_error = 0;
    std::unique_ptr<FsLock> lock = fs.TakeLock(update, env.lock_timeout_ms, kLockStepMs, &lock_error);
    if (!lock) return done(CheckAction::kFailed, "lock_unavailable", staged.version, staged.direction);
    // Under the lock again: a start may have swapped something in while this one downloaded.
    if (fs.Exists(paths.UpdateFile(names::kPendingJson))) return done(CheckAction::kNone, "release_pending", staged.version, staged.direction);
    // staged.json is published last: its existence is the host's signal that there is something to swap.
    if (!fs.WriteByRename(paths.UpdateFile(names::kStagedJson), BuildStagedJson(staged))) {
      return done(CheckAction::kFailed, "publish_failed", staged.version, staged.direction);
    }
    lock->Release();
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
    // No root: no pending record anywhere, as an absent file always said. A root that is not UTF-16 text cannot be given to the fs, and
    // the confirmation fails closed (the release stays pending, and the host decides by its own rules at the next start).
    if (env.root.empty()) return skipped(ConfirmResult::kNotPending);
    std::string root_utf8;
    if (!Utf8(env.root, &root_utf8)) return skipped(ConfirmResult::kPendingUnreadable);
    const UpdateFs& fs = *env.fs;
    const Paths paths{fs, root_utf8};
    const std::string update = paths.Update();
    if (fs.IsLink(update)) return skipped(ConfirmResult::kPendingUnreadable);

    std::string text;
    PendingTarget target;
    const Read first = fs.ReadSmallFile(paths.UpdateFile(names::kPendingJson), &text);
    if (first == Read::kAbsent) return skipped(ConfirmResult::kNotPending);
    if (first != Read::kOk || !ParsePendingTarget(text, &target)) return skipped(ConfirmResult::kPendingUnreadable);
    // A runtime that is not the pending one confirms nothing: a second process that still maps the old file (or an old
    // launcher's runtime) must not mark the new release as started.
    if (target.sha256 != env.runtime_sha256) return skipped(ConfirmResult::kNotThePendingRelease, target.version);

    int lock_error = 0;
    std::unique_ptr<FsLock> lock = fs.TakeLock(update, env.lock_timeout_ms, kLockStepMs, &lock_error);
    if (!lock) return skipped(ConfirmResult::kLockUnavailable, target.version);
    // Under the lock: the host may have rolled the release back since the first read.
    if (fs.ReadSmallFile(paths.UpdateFile(names::kPendingJson), &text) != Read::kOk || !ParsePendingTarget(text, &target) ||
        target.sha256 != env.runtime_sha256) {
      return skipped(ConfirmResult::kNotThePendingRelease, target.version);
    }
    std::string state;
    const Read state_read = fs.ReadSmallFile(paths.UpdateFile(names::kStateJson), &state);
    if (state_read == Read::kAbsent) state = "{}";
    else if (state_read != Read::kOk) return skipped(ConfirmResult::kStateUnreadable, target.version);
    std::string confirmed;
    // A state file that is not an object is damage, and the host rebuilds it; it is not written over here.
    if (!ConfirmState(state, target.version, target.sha256, &confirmed)) return skipped(ConfirmResult::kStateUnreadable, target.version);
    if (!fs.WriteByRename(paths.UpdateFile(names::kStateJson), confirmed)) return skipped(ConfirmResult::kWriteFailed, target.version);
    // state.json first, pending.json after: a kill between them leaves a pending record that names the confirmed runtime, which
    // the host drops as stale (section 6), never a pending record with no state behind it.
    fs.RemoveFile(paths.UpdateFile(names::kPendingJson));
    fs.RemoveFile(paths.UpdateFile(names::kCrashedFile));
    lock->Release();
    Emit(env.log, kInfo, "update confirmed",
         {{"version", target.version}, {"after_ms", std::to_string(after_ms)}, {"role", env.role != nullptr ? env.role : "client"}});
    return ConfirmResult::kConfirmed;
  } catch (const std::exception&) {  // out of memory: the confirmation is tried again at the next start
    Emit(env.log, kWarning, "update confirm skipped", {{"reason", "internal_error"}});
    return ConfirmResult::kWriteFailed;
  }
}

}  // namespace nevr_update
