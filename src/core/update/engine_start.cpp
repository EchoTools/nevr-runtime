#include "core/update/engine_start.h"

#include "core/update/names.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The start of the self-update, moved from runtime/host/update_swap.cpp (ADR 0024, sections 4 and 6) behind the edges of
// edges.h and fs.h. The behaviour is the one the PC host has always had: the same checks in the same order, the same
// moves, the same journal phases, and the same record for each decision. The only differences are the edges themselves.

namespace nevr_update {
namespace {

namespace names = nevr_update_names;
using nlohmann::json;

constexpr std::size_t kMaxReasonLength = 40;
constexpr char kDigits[] = "0123456789";
constexpr char kTempInfix[] = ".tmp-";
constexpr char kRelSeparator = '/';

// Two names of a release's files are one file when they differ only in case: Windows opens either.
bool SameName(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

// kUnreadable is a file that is there but cannot be opened or read: not damage, and never treated as damage (logging
// Rule 10; ADR 0024, section 4.6). kBad is a file read whole that is not usable: too large, too deep, or not JSON.
enum class JsonRead { kAbsent, kUnreadable, kBad, kOk };

// True when the text's arrays and objects, outside strings, nest at most `max` deep. The text is measured before it is
// parsed: copying or writing a tree recurses once per level, so a file nested tens of thousands deep would end the process.
bool NestedAtMost(std::string_view text, std::size_t max) {
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (const char c : text) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
    } else if (c == '"') {
      in_string = true;
    } else if (c == '[' || c == '{') {
      if (++depth > max) return false;
    } else if ((c == ']' || c == '}') && depth > 0) {
      --depth;
    }
  }
  return true;
}

// staged.json (ADR 0024, section 2): one file of a staged release, and the release.
struct StagedFile {
  names::Member member;
  std::uint64_t size = 0;
  std::string sha256;
};

struct Staged {
  std::string version;
  std::vector<StagedFile> files;
  std::string runtime_sha256;
};

bool HasMember(const std::vector<names::Member>& members, const names::Member& member) {
  for (const names::Member& m : members) {
    if (m.runtime == member.runtime && SameName(m.plugin, member.plugin)) return true;
  }
  return false;
}

// The runtime's file in this start's root is the one name the start treats as its runtime: nevr-runtime.dll on PC, libnevr-runtime.so
// on Quest (StartContext::runtime_file). The rule is names.h's (ParseMemberOf), the same one the check reads a stage result with: the
// other platform's runtime name is refused, and a Quest release is the runtime file alone.
bool ParseFileMember(std::string_view runtime_file, std::string_view path, names::Member* out) {
  return names::ParseMemberOf(runtime_file, path, out);
}

// The path of a member relative to the root, in the spelling of this start's runtime file.
std::string MemberFile(std::string_view runtime_file, const names::Member& member) { return names::MemberPathOf(runtime_file, member); }

// staged.json: {"version", "files": [{"path", "size", "sha256"}]}, the spelling of NevrPcStage's result. Every name in
// the vocabulary, no file twice (in any case), exactly one runtime.
bool ParseStaged(std::string_view runtime_file, const json& doc, Staged* out) {
  if (!doc.is_object() || !doc.contains("version") || !doc["version"].is_string() || !doc.contains("files") ||
      !doc["files"].is_array() || doc["files"].empty()) {
    return false;
  }
  out->version = doc["version"].get<std::string>();
  if (!names::IsVersionName(out->version)) return false;
  std::vector<names::Member> seen;
  int runtimes = 0;
  for (const json& file : doc["files"]) {
    if (!file.is_object() || !file.contains("path") || !file["path"].is_string() || !file.contains("size") ||
        !file["size"].is_number_unsigned() || !file.contains("sha256") || !file["sha256"].is_string()) {
      return false;
    }
    StagedFile staged;
    if (!ParseFileMember(runtime_file, file["path"].get<std::string>(), &staged.member)) return false;
    if (HasMember(seen, staged.member)) return false;
    seen.push_back(staged.member);
    staged.size = file["size"].get<std::uint64_t>();
    staged.sha256 = file["sha256"].get<std::string>();
    if (!IsSha256Hex(staged.sha256)) return false;
    if (staged.member.runtime) {
      ++runtimes;
      out->runtime_sha256 = staged.sha256;
    }
    out->files.push_back(staged);
  }
  return runtimes == 1;
}

bool ParseMembers(std::string_view runtime_file, const json& list, std::vector<names::Member>* out) {
  if (!list.is_array()) return false;
  for (const json& path : list) {
    names::Member member;
    if (!path.is_string() || !ParseFileMember(runtime_file, path.get<std::string>(), &member) || HasMember(*out, member)) return false;
    out->push_back(member);
  }
  return true;
}

// swap.json: {"kind": "swap"|"rollback", "from", "to", "phase": "out"|"in"|"done", "out": [member path], "in": [member
// path]}. For a swap, `from` is the release moved out and `to` the one moved in. For a rollback (ADR 0024, section 6),
// `from` is the failed release leaving the root and `to` the previous release coming back; it also carries
// "failed_sha256" (the failed runtime, for the quarantine) and "reason" (why), so an interrupted rollback can be
// completed from the journal alone.
struct Journal {
  std::string kind = "swap";
  std::string from;
  std::string to;
  std::string phase;
  std::vector<names::Member> out;
  std::vector<names::Member> in;
  std::string failed_sha256;
  std::string reason;
  // What the host verified before it moved anything, so the cleanup at `done` writes records from facts it has and not from
  // a re-read that can fail: the sha256 of every file that moves in, by member path, and for a swap the sha256 of the runtime
  // that moved out. A journal without them (an older one) falls back to reading the files.
  std::map<std::string, std::string> in_sha256;
  std::string from_sha256;
};

bool ParseJournal(std::string_view runtime_file, const json& doc, Journal* out) {
  if (!doc.is_object()) return false;
  for (const char* key : {"kind", "from", "to", "phase"}) {
    if (!doc.contains(key) || !doc[key].is_string()) return false;
  }
  out->kind = doc["kind"].get<std::string>();
  if (out->kind != "swap" && out->kind != "rollback") return false;
  out->from = doc["from"].get<std::string>();
  out->to = doc["to"].get<std::string>();
  out->phase = doc["phase"].get<std::string>();
  if (!names::IsVersionName(out->from) || !names::IsVersionName(out->to)) return false;
  if (out->phase != "out" && out->phase != "in" && out->phase != "done") return false;
  if (out->kind == "rollback") {
    if (!doc.contains("failed_sha256") || !doc["failed_sha256"].is_string() || !doc.contains("reason") ||
        !doc["reason"].is_string()) {
      return false;
    }
    out->failed_sha256 = doc["failed_sha256"].get<std::string>();
    out->reason = doc["reason"].get<std::string>();
    if (!IsSha256Hex(out->failed_sha256) || !IsReasonToken(out->reason)) return false;
  }
  if (doc.contains("from_sha256")) {
    if (!doc["from_sha256"].is_string()) return false;
    out->from_sha256 = doc["from_sha256"].get<std::string>();
    if (!IsSha256Hex(out->from_sha256)) return false;
  }
  if (doc.contains("in_sha256")) {
    if (!doc["in_sha256"].is_object()) return false;
    for (const auto& [path, sha] : doc["in_sha256"].items()) {
      names::Member member;
      if (!ParseFileMember(runtime_file, path, &member) || !sha.is_string() || !IsSha256Hex(sha.get<std::string>())) return false;
      out->in_sha256[path] = sha.get<std::string>();
    }
  }
  return doc.contains("out") && doc.contains("in") && ParseMembers(runtime_file, doc["out"], &out->out) &&
         ParseMembers(runtime_file, doc["in"], &out->in);
}

json JournalJson(std::string_view runtime_file, const Journal& journal) {
  json out = json::array();
  json in = json::array();
  for (const names::Member& m : journal.out) out.push_back(MemberFile(runtime_file, m));
  for (const names::Member& m : journal.in) in.push_back(MemberFile(runtime_file, m));
  json doc = {{"kind", journal.kind}, {"from", journal.from}, {"to", journal.to}, {"phase", journal.phase}, {"out", out}, {"in", in}};
  if (journal.kind == "rollback") {
    doc["failed_sha256"] = journal.failed_sha256;
    doc["reason"] = journal.reason;
  }
  if (!journal.from_sha256.empty()) doc["from_sha256"] = journal.from_sha256;
  if (!journal.in_sha256.empty()) {
    json shas = json::object();
    for (const auto& [path, sha] : journal.in_sha256) shas[path] = sha;
    doc["in_sha256"] = shas;
  }
  return doc;
}

// state.json (ADR 0024, section 2): an object; "owned", when present, a list of {"path", "sha256"} in the vocabulary;
// "discards", when present, an object of version to count; "quarantine", when present, a list.
bool ValidState(std::string_view runtime_file, const json& doc) {
  if (!doc.is_object()) return false;
  if (doc.contains("owned")) {
    if (!doc["owned"].is_array()) return false;
    for (const json& entry : doc["owned"]) {
      names::Member member;
      if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string() || !entry.contains("sha256") ||
          !entry["sha256"].is_string() || !ParseFileMember(runtime_file, entry["path"].get<std::string>(), &member) ||
          !IsSha256Hex(entry["sha256"].get<std::string>())) {
        return false;
      }
    }
  }
  if (doc.contains("discards")) {
    if (!doc["discards"].is_object()) return false;
    for (const auto& [version, count] : doc["discards"].items()) {
      if (!names::IsVersionName(version) || !count.is_number_unsigned()) return false;
    }
  }
  return !doc.contains("quarantine") || doc["quarantine"].is_array();
}

// The in-use answer as the decision's write access (ADR 0024, section 4.4): another process maps the runtime, or it cannot be
// written for another cause, or nothing stands in the way.
WriteAccess WriteAccessOf(const InUseAnswer& answer) {
  WriteAccess write_access;
  if (answer.state == InUse::kInUse) {
    write_access.kind = WriteAccess::Kind::kInUse;
  } else if (answer.state == InUse::kError) {
    write_access.kind = WriteAccess::Kind::kNotWritable;
    write_access.os_error = static_cast<std::uint32_t>(answer.os_error);
  } else {
    write_access.kind = WriteAccess::Kind::kWritable;
  }
  return write_access;
}

// One start's work on update\. Every path is composed here from the root and the vocabulary, with '/' inside the relative
// part; the edge turns it into its own separator.
class SwapRun {
 public:
  explicit SwapRun(const StartContext& context)
      : ctx_(context), fs_(context.edges.fs), started_(context.edges.clock.TickMs()) {
    root_ = context.root;
    runtime_file_ = context.runtime_file;
    sep_ = fs_.Separator();
    update_ = Abs(names::kUpdateDir);
  }

  StartResult Run() {
    if (root_.empty() || !fs_.IsDirectory(update_)) return {StartAction::kNoUpdateFolder, nullptr, false};
    // update\ itself first: the lock is a file inside it, and a link is never followed, not even to take the lock.
    if (fs_.IsLink(update_)) {
      Record("blocked", "reparse_point", {{"folder", names::kUpdateDir}});
      return Load();
    }
    if (!AcquireLock()) {
      // Another start holds it past the wait. With a journal the root may be half moved: nothing is loaded.
      const bool journal = fs_.Exists(UpdateFile(names::kSwapJson));
      Record("blocked", "lock_timeout", {{"journal", journal}});
      return journal ? Stop("lock_timeout") : Load();
    }
    const bool journal = fs_.Exists(UpdateFile(names::kSwapJson));
    const std::string linked = FirstLink({Child(names::kUpdateDir, names::kStagingDir), Child(names::kUpdateDir, names::kPreviousDir),
                                          Child(names::kUpdateDir, names::kFailedDir), names::kPluginsDir});
    if (!linked.empty()) {
      // With a journal the root may be half moved, and its recovery would move files through the link: nothing loads.
      Record("blocked", "reparse_point", {{"folder", linked}, {"journal", journal}});
      return journal ? Stop("reparse_point") : Load();
    }
    RemoveStaleTemporaries();
    if (killed_) return Load();
    if (!journal && !fs_.Exists(UpdateFile(names::kStagedJson)) && !fs_.Exists(UpdateFile(names::kPendingJson))) return Load();
    const StateRead state = LoadState();
    if (state == StateRead::kEnded) return Load();
    if (journal) {
      const StartResult recovered = Recover(state == StateRead::kOk);
      // A journal still there is a recovery that did not finish: no new swap starts over it.
      if (killed_ || recovered.action == StartAction::kStop || fs_.Exists(UpdateFile(names::kSwapJson))) return recovered;
    }
    if (state != StateRead::kOk) return Load();  // its record is written: no swap and no rollback without the ownership list
    if (fs_.Exists(UpdateFile(names::kPendingJson))) {
      StartResult stop;
      switch (DecidePending(&stop)) {
        case PendingStep::kStop: return stop;
        case PendingStep::kHold: return Load();
        case PendingStep::kTrial:
          // A staged release waits: swapping now would move the unconfirmed release over the last confirmed one.
          if (fs_.Exists(UpdateFile(names::kStagedJson))) Record("deferred", "release_pending");
          return Trial();
        case PendingStep::kNone: break;
      }
      if (killed_) return Load();
    }
    if (fs_.Exists(UpdateFile(names::kStagedJson))) return SwapStaged();
    return Load();
  }

  /// R1, never ran (ADR 0024, section 6): put the previous release back now, in this start.
  StartResult RollBackNow(const char* reason) {
    if (root_.empty() || !fs_.IsDirectory(update_)) return Stop("no_previous");
    if (fs_.IsLink(update_)) {
      Record("blocked", "reparse_point", {{"folder", names::kUpdateDir}});
      return Stop("reparse_point");
    }
    if (!AcquireLock()) {
      RecordRollback("failed", "lock_timeout", {{"mode", "in_process"}, {"cause", reason}});
      return Stop("lock_timeout");
    }
    const bool journal = fs_.Exists(UpdateFile(names::kSwapJson));
    const std::string linked = FirstLink({Child(names::kUpdateDir, names::kStagingDir), Child(names::kUpdateDir, names::kPreviousDir),
                                          Child(names::kUpdateDir, names::kFailedDir), names::kPluginsDir});
    if (!linked.empty()) {
      Record("blocked", "reparse_point", {{"folder", linked}, {"journal", journal}});
      return Stop("reparse_point");
    }
    RemoveStaleTemporaries();
    if (killed_) return Load();
    const StateRead state = LoadState();
    if (state == StateRead::kEnded) return Load();
    if (journal) {
      // A cleanup the swap of this start left unfinished, or an earlier one: finished before anything else.
      const StartResult recovered = Recover(state == StateRead::kOk);
      if (killed_ || recovered.action == StartAction::kStop || fs_.Exists(UpdateFile(names::kSwapJson))) return recovered;
    }
    if (state != StateRead::kOk) return Stop("state_unreadable");
    json doc;
    int error = 0;
    const JsonRead read = ReadJson(UpdateFile(names::kPendingJson), &doc, &error);
    if (read == JsonRead::kUnreadable) {
      RecordRollback("failed", "pending_unreadable", {{"mode", "in_process"}, {"win32_error", error}});
      return Stop("pending_unreadable");
    }
    Pending pending;
    if (read != JsonRead::kOk || !ParsePending(doc, &pending)) return Load();  // nothing is pending any more
    const ArtifactFacts runtime = Facts(Abs(runtime_file_));
    if (runtime.present && !runtime.sha256.empty() && runtime.sha256 != pending.to_sha256) return Load();  // already put back
    const InUseAnswer write = RuntimeWrite();
    if (write.state != InUse::kFree) {
      // The failed runtime cannot be moved now; the next start puts it back.
      const bool marked = MarkInRecord(doc, reason);
      const bool in_use = write.state == InUse::kInUse;
      json fields = {{"mode", "in_process"}, {"from", pending.to_version}, {"win32_error", write.os_error}, {"marked", marked}};
      if (!marked) fields["mark_win32_error"] = last_error_;
      RecordRollback("failed", in_use ? "runtime_in_use" : "runtime_not_writable", fields);
      return Stop(in_use ? "runtime_in_use" : "runtime_not_writable");
    }
    StartResult stop;
    const RollbackEnd end = DoRollback(pending, reason, "in_process", &stop);
    if (killed_) return Load();
    if (end == RollbackEnd::kDone) return Load();  // the lock stays held for the load that follows
    // The failed release stays in the root: it is marked, so the next start tries again (the first failure stays).
    const bool marked = MarkInRecord(doc, reason);
    if (!marked) RecordRollback("failed", "pending_write_failed", {{"mode", "in_process"}, {"win32_error", last_error_}});
    if (end == RollbackEnd::kStop) return stop;
    return Stop("rollback_blocked");
  }

  /// R1, mapped (ADR 0024, section 6): writes `failed_reason` into the pending record, unless it already names a failure.
  bool MarkFailed(const char* reason) {
    if (root_.empty() || !fs_.IsDirectory(update_) || fs_.IsLink(update_)) return false;
    if (!AcquireLock()) {
      RecordRollback("failed", "lock_timeout", {{"mode", "next_start"}, {"cause", reason}});
      return false;
    }
    json doc;
    int error = 0;
    Pending pending;
    if (ReadJson(UpdateFile(names::kPendingJson), &doc, &error) != JsonRead::kOk || !ParsePending(doc, &pending)) {
      RecordRollback("failed", "pending_unavailable", {{"mode", "next_start"}, {"cause", reason}, {"win32_error", error}});
      return false;
    }
    json fields = {{"mode", "next_start"}, {"from", pending.to_version}, {"boot_attempts", pending.boot_attempts}};
    if (pending.has_from) fields["to"] = pending.from_version;
    if (!pending.failed_reason.empty()) {
      // The first failure is the cause that stays.
      RecordRollback("scheduled", pending.failed_reason.c_str(), fields);
      return true;
    }
    doc["failed_reason"] = reason;
    if (!WriteJson(UpdateFile(names::kPendingJson), doc)) {
      fields["win32_error"] = last_error_;
      RecordRollback("failed", "pending_write_failed", fields);
      return false;
    }
    RecordRollback("scheduled", reason, fields);
    return true;
  }

  /// The answer after an exception: stop when a journal says the root may be half moved, else load.
  StartResult AfterException() {
    Record("blocked", "internal_error");
    return fs_.Exists(UpdateFile(names::kSwapJson)) ? Stop("internal_error") : Load();
  }

 private:
  enum class StateRead { kOk, kUnreadable, kEnded };  // kEnded: the seam ended the start while state.json was rebuilt
  enum class PendingStep { kNone, kTrial, kHold, kStop };
  enum class RollbackEnd { kDone, kBlocked, kStop };

  static StartResult Load() { return {StartAction::kLoad, nullptr, false}; }
  // Load, and the release in the root is on trial (the load of it that fails is the host's to handle).
  static StartResult Trial() { return {StartAction::kLoad, nullptr, true}; }
  static StartResult Stop(const char* reason) { return {StartAction::kStop, reason, false}; }

  // ---- paths --------------------------------------------------------------------------------------------------------

  static std::string Child(const std::string& dir, const std::string& name) { return dir + kRelSeparator + name; }
  std::string Os(const std::string& relative) const {
    std::string out = relative;
    for (char& c : out) {
      if (c == kRelSeparator) c = sep_;
    }
    return out;
  }
  std::string Abs(const std::string& relative) const { return root_ + sep_ + Os(relative); }
  // A child of a folder whose path is already in the edge's form (from Abs): joined with the edge's separator.
  std::string SubOf(const std::string& absolute_dir, const char* name) const { return absolute_dir + sep_ + name; }
  std::string UpdateFile(const std::string& name) const { return Abs(Child(names::kUpdateDir, name)); }
  static std::string StagingRel(const std::string& version) {
    return Child(Child(names::kUpdateDir, names::kStagingDir), version);
  }
  static std::string GenerationRel(const char* folder, const std::string& version) {
    return Child(Child(names::kUpdateDir, folder), version);
  }
  static std::string PreviousRel(const std::string& version) { return GenerationRel(names::kPreviousDir, version); }
  static std::string FailedRel(const std::string& version) { return GenerationRel(names::kFailedDir, version); }
  std::string StagingDir(const std::string& version) const { return Abs(StagingRel(version)); }
  std::string PreviousDir(const std::string& version) const { return Abs(PreviousRel(version)); }
  std::string FailedDir(const std::string& version) const { return Abs(FailedRel(version)); }
  std::string InRoot(const names::Member& member) const { return Abs(MemberFile(runtime_file_, member)); }
  std::string In(const std::string& dir, const names::Member& member) const { return dir + sep_ + Os(MemberFile(runtime_file_, member)); }
  // The first of `relative` (folders relative to the root) that is a link, spelled as the record names it ("update/staging");
  // empty when none is. The swap never moves, makes or deletes anything through a link out of the root (ADR 0024, section 4.2).
  std::string FirstLink(std::initializer_list<std::string> relative) const {
    for (const std::string& rel : relative) {
      if (fs_.IsLink(Abs(rel))) return rel;
    }
    return {};
  }
  void MakeDirFor(const std::string& file) const { fs_.MakeDir(file.substr(0, file.find_last_of(sep_))); }

  // ---- the edges ----------------------------------------------------------------------------------------------------

  ArtifactFacts Facts(const std::string& path) const { return ctx_.edges.verifier.Measure(path); }
  InUseAnswer RuntimeWrite() const { return ctx_.edges.in_use.Query(Abs(runtime_file_)); }

  // A step that changed the disk. False when the test seam ended the start here (as a kill would).
  bool Step() {
    ++steps_;
    if (ctx_.hooks.step_ends != nullptr && ctx_.hooks.step_ends(steps_)) killed_ = true;
    return !killed_;
  }

  bool Move(const std::string& from, const std::string& to) {
    if (killed_) return false;
    ++moves_;
    int os = 0;
    if (ctx_.hooks.move_fails != nullptr && ctx_.hooks.move_fails(moves_, &os)) {
      last_error_ = os;
      return false;
    }
    // Without a copy: a move to another volume fails, and the journal undoes it (ADR 0024, section 4.2). Nothing is replaced.
    if (!fs_.Move(from, to, &os)) {
      last_error_ = os;
      return false;
    }
    return Step();
  }

  bool Remove(const std::string& path) {
    if (killed_) return false;
    if (!fs_.Exists(path)) return true;
    if (!fs_.RemoveFile(path)) {
      last_error_ = fs_.LastOsError();
      return false;
    }
    return Step();
  }

  bool RemoveDir(const std::string& path) {
    if (killed_) return false;
    if (!fs_.IsDirectory(path)) return true;
    if (!fs_.RemoveEmptyDir(path)) return true;  // not empty: something that is not ours stays
    return Step();
  }

  // A temporary file of this process, flushed, then renamed over the file: a reader sees the old file or the new one.
  bool WriteJson(const std::string& path, const json& doc) {
    if (killed_) return false;
    if (!fs_.WriteByRename(path, doc.dump())) {
      last_error_ = fs_.LastOsError();
      return false;
    }
    return Step();
  }

  // ---- records ------------------------------------------------------------------------------------------------------

  void Record(const char* outcome, const char* reason, json fields = json::object()) {
    if (killed_) return;  // a kill writes nothing after the step it ended on
    json record = std::move(fields);
    record["event"] = "update_swap";
    record["outcome"] = outcome;
    if (reason != nullptr) record["reason"] = reason;
    record["elapsed_ms"] = ctx_.edges.clock.TickMs() - started_;
    ctx_.edges.log.Emit(record);
  }

  void RecordRollback(const char* outcome, const char* reason, json fields = json::object()) {
    if (killed_) return;
    json record = std::move(fields);
    record["event"] = "update_rollback";
    record["outcome"] = outcome;
    if (reason != nullptr) record["reason"] = reason;
    record["elapsed_ms"] = ctx_.edges.clock.TickMs() - started_;
    ctx_.edges.log.Emit(record);
  }

  // One start on a release that is pending says which try it is, or why it is not one (ADR 0024, requirement 4).
  void RecordTrial(const char* outcome, const char* reason, const Pending& pending, std::uint64_t boot_attempts,
                   json fields = json::object()) {
    if (killed_) return;
    json record = std::move(fields);
    record["event"] = "update_trial";
    record["outcome"] = outcome;
    record["reason"] = reason;
    record["release"] = pending.to_version;
    record["boot_attempts"] = boot_attempts;
    record["elapsed_ms"] = ctx_.edges.clock.TickMs() - started_;
    ctx_.edges.log.Emit(record);
  }

  // ---- the lock, the journal's JSON files ---------------------------------------------------------------------------

  // update\lock, taken into the host's slot; a lock already held stays held.
  bool AcquireLock() {
    if (ctx_.lock == nullptr) return false;
    if (*ctx_.lock != nullptr) return true;
    int os = 0;
    *ctx_.lock = fs_.TakeLock(update_, ctx_.lock_timing.timeout_ms, ctx_.lock_timing.step_ms, &os);
    return *ctx_.lock != nullptr;
  }

  // A JSON file of update\, at most names::kMaxJsonBytes and names::kMaxJsonDepth. `error` is the OS error of an
  // unreadable result.
  JsonRead ReadJson(const std::string& path, json* out, int* error) const {
    *error = 0;
    std::string text;
    switch (fs_.ReadSmallFile(path, &text)) {
      case ReadResult::kAbsent: *error = fs_.LastOsError(); return JsonRead::kAbsent;
      case ReadResult::kUnreadable: *error = fs_.LastOsError(); return JsonRead::kUnreadable;
      case ReadResult::kTooBig: return JsonRead::kBad;
      case ReadResult::kOk: break;
    }
    if (!NestedAtMost(text, names::kMaxJsonDepth)) return JsonRead::kBad;
    *out = json::parse(text, nullptr, false);
    return out->is_discarded() ? JsonRead::kBad : JsonRead::kOk;
  }

  json RebuiltState() const {
    json state = {{"owned", json::array()}};
    const ArtifactFacts runtime = Facts(Abs(runtime_file_));
    if (!runtime.sha256.empty()) {
      state["owned"].push_back({{"path", runtime_file_}, {"sha256", runtime.sha256}});
      if (names::IsVersionName(runtime.version)) {
        state["current"] = {{"version", runtime.version}, {"runtime_sha256", runtime.sha256}};
      }
    }
    return state;
  }

  // A start killed between a JSON file's temporary write and its rename leaves <file>.tmp-<pid>. Under the lock no writer is
  // live, so each one of the host's own files is a leftover: it is removed, and nothing else is.
  void RemoveStaleTemporaries() {
    std::vector<DirEntry> entries;
    if (!fs_.ListFolder(update_, &entries)) return;
    std::vector<std::string> stale;
    for (const DirEntry& entry : entries) {
      if (entry.is_dir) continue;
      for (const char* file : {names::kStateJson, names::kPendingJson, names::kSwapJson}) {
        const std::string prefix = std::string(file) + kTempInfix;
        if (entry.name.size() > prefix.size() && entry.name.compare(0, prefix.size(), prefix) == 0 &&
            entry.name.find_first_not_of(kDigits, prefix.size()) == std::string::npos) {
          stale.push_back(entry.name);
        }
      }
    }
    int removed = 0;
    int failed = 0;
    int error = 0;
    for (const std::string& name : stale) {
      if (Remove(UpdateFile(name))) {
        ++removed;
      } else if (killed_) {
        return;
      } else {
        ++failed;
        error = last_error_;
      }
    }
    if (removed == 0 && failed == 0) return;
    json fields = {{"removed", removed}};
    if (failed != 0) {
      fields["failed"] = failed;
      fields["win32_error"] = error;
    }
    Record("recovered", "temporaries_removed", fields);
  }

  StateRead LoadState() {
    json doc;
    int error = 0;
    const JsonRead read = ReadJson(UpdateFile(names::kStateJson), &doc, &error);
    if (read == JsonRead::kAbsent) {
      state_ = json::object();
      return StateRead::kOk;
    }
    if (read == JsonRead::kUnreadable) {
      // Not damage: the file is there and something keeps this start from it. It is neither rebuilt nor written over, and
      // nothing is swapped without its ownership list (sections 4.7, 4.8).
      Record("deferred", "state_unreadable", {{"win32_error", error}});
      return StateRead::kUnreadable;
    }
    if (read == JsonRead::kOk && ValidState(runtime_file_, doc)) {
      state_ = std::move(doc);
      return StateRead::kOk;
    }
    state_ = RebuiltState();
    const bool written = WriteJson(UpdateFile(names::kStateJson), state_);
    if (killed_) return StateRead::kEnded;
    json fields = {{"state_written", written}};
    if (!written) fields["win32_error"] = last_error_;
    Record("recovered", "state_rebuilt", fields);
    return StateRead::kOk;
  }

  // ---- journal recovery (ADR 0024, section 4.6) ---------------------------------------------------------------------

  // out or in are undone to the last whole state; done finishes its cleanup, which writes state.json and so waits for a
  // start that can read it (`state_readable`).
  StartResult Recover(bool state_readable) {
    json doc;
    Journal journal;
    int error = 0;
    const JsonRead read = ReadJson(UpdateFile(names::kSwapJson), &doc, &error);
    if (read == JsonRead::kUnreadable) {
      // The journal may be fine; this start cannot tell, and the root may be half moved.
      Record("blocked", "journal_unreadable", {{"win32_error", error}});
      return Stop("journal_unreadable");
    }
    if (read != JsonRead::kOk || !ParseJournal(runtime_file_, doc, &journal)) {
      Record("blocked", "bad_journal");
      return Stop("bad_journal");
    }
    if (journal.kind == "rollback") return RecoverRollback(journal, state_readable);
    json fields = {{"kind", "swap"}, {"phase", journal.phase}, {"from", journal.from}, {"to", journal.to}};
    const std::string linked = FirstLink({StagingRel(journal.to), Child(StagingRel(journal.to), names::kPluginsDir),
                                          PreviousRel(journal.from), Child(PreviousRel(journal.from), names::kPluginsDir)});
    if (!linked.empty()) {
      // Nothing is moved or removed through the link. At done every file is in place, so the start loads.
      fields["folder"] = linked;
      Record("blocked", "reparse_point", fields);
      return journal.phase == "done" ? Load() : Stop("reparse_point");
    }
    if (journal.phase == "done" && !state_readable) return Load();
    const bool whole = journal.phase == "done" ? FinishDone(journal) : UndoMoves(journal, journal.phase == "in");
    if (killed_) return Load();
    if (!whole) {
      fields["win32_error"] = last_error_;
      Record("blocked", "move_failed", fields);
      return journal.phase == "done" ? Load() : Stop("move_failed");
    }
    Record("recovered", nullptr, fields);
    if (journal.phase == "done") finished_swap_ = true;
    return Load();
  }

  // Files that moved in go back to staging, then files that moved out go back to the root; the journal goes last.
  bool UndoMoves(const Journal& journal, bool phase_in) {
    const std::string staging = StagingDir(journal.to);
    const std::string previous = PreviousDir(journal.from);
    if (phase_in) {
      for (auto it = journal.in.rbegin(); it != journal.in.rend(); ++it) {
        if (fs_.Exists(InRoot(*it)) && !fs_.Exists(In(staging, *it))) {
          MakeDirFor(In(staging, *it));
          if (!Move(InRoot(*it), In(staging, *it))) return false;
        }
      }
    }
    for (auto it = journal.out.rbegin(); it != journal.out.rend(); ++it) {
      if (fs_.Exists(In(previous, *it)) && !fs_.Exists(InRoot(*it))) {
        MakeDirFor(InRoot(*it));
        if (!Move(In(previous, *it), InRoot(*it))) return false;
      }
    }
    return Remove(UpdateFile(names::kSwapJson));
  }

  // True when pending.json is the record of this swap: `to` is `version` with the runtime's sha256.
  bool PendingNames(const std::string& version, const std::string& runtime_sha256) const {
    json doc;
    int error = 0;
    if (runtime_sha256.empty() || ReadJson(UpdateFile(names::kPendingJson), &doc, &error) != JsonRead::kOk) return false;
    if (!doc.is_object() || !doc.contains("to") || !doc["to"].is_object()) return false;
    const json& to = doc["to"];
    return to.value("version", json()) == json(version) && to.value("sha256", json()) == json(runtime_sha256);
  }

  // True when staged.json, read whole, names `version`.
  bool StagedNames(const std::string& version) const {
    json doc;
    int error = 0;
    return ReadJson(UpdateFile(names::kStagedJson), &doc, &error) == JsonRead::kOk && doc.is_object() &&
           doc.value("version", json()) == json(version);
  }

  // At done: the files the swap placed are owned, a stale crash marker goes and the release is pending, then staged.json, the
  // staging folder and the journal go (ADR 0024, section 4.5). Every step can be run again, and a run that finishes an earlier
  // start's cleanup undoes nothing that happened since.
  bool FinishDone(const Journal& journal) {
    json owned = json::array();
    std::string to_sha256;
    for (const names::Member& member : journal.in) {
      const std::string path = MemberFile(runtime_file_, member);
      const auto known = journal.in_sha256.find(path);
      std::string sha256;
      if (known != journal.in_sha256.end()) {
        if (fs_.Exists(InRoot(member))) sha256 = known->second;  // a file removed since the swap is not owned
      } else {
        const ArtifactFacts facts = Facts(InRoot(member));
        if (facts.unreadable) return false;  // not gone, only held: the next start finishes this
        sha256 = facts.sha256;
      }
      if (member.runtime) to_sha256 = known != journal.in_sha256.end() ? known->second : sha256;
      if (sha256.empty()) continue;
      owned.push_back({{"path", path}, {"sha256", sha256}});
    }
    state_["owned"] = owned;
    if (!WriteJson(UpdateFile(names::kStateJson), state_)) return false;
    if (to_sha256.empty()) return false;  // a pending record that cannot name its release is not written
    if (!PendingNames(journal.to, to_sha256)) {
      if (!Remove(UpdateFile(names::kCrashedFile))) return false;
      json from = nullptr;
      if (!journal.from_sha256.empty()) {
        from = {{"version", journal.from}, {"sha256", journal.from_sha256}};
      } else {
        const ArtifactFacts previous = Facts(SubOf(PreviousDir(journal.from), runtime_file_.c_str()));
        if (previous.unreadable) return false;
        if (previous.present && !previous.sha256.empty()) from = {{"version", journal.from}, {"sha256", previous.sha256}};
      }
      const json pending = {{"to", {{"version", journal.to}, {"sha256", to_sha256}}},
                            {"from", from},
                            {"boot_attempts", 1},
                            {"failed_reason", nullptr}};
      if (!WriteJson(UpdateFile(names::kPendingJson), pending)) return false;
    }
    if (StagedNames(journal.to) && !Remove(UpdateFile(names::kStagedJson))) return false;
    const std::string staging = StagingDir(journal.to);
    if (!RemoveDir(SubOf(staging, names::kPluginsDir)) || !RemoveDir(staging)) return false;
    return Remove(UpdateFile(names::kSwapJson));
  }

  // ---- staged releases (ADR 0024, sections 4.3, 4.4, 4.7) -----------------------------------------------------------

  // The file as it is on disk now, against staged.json and the verifier. Empty when it passes, else the reason.
  std::string CheckStagedFile(const Staged& staged, const StagedFile& file) const {
    const ArtifactExpect expect{true, file.size, file.sha256};
    const ArtifactCheck check = ctx_.edges.verifier.Verify(In(StagingDir(staged.version), file.member), expect);
    return check.ok ? std::string() : check.reason;
  }

  // True when state.json's quarantine already holds this release (its version and runtime sha256).
  bool IsQuarantined(const std::string& version, const std::string& runtime_sha256) const {
    if (!state_.contains("quarantine") || !state_["quarantine"].is_array()) return false;
    for (const json& entry : state_["quarantine"]) {
      if (entry.is_object() && entry.value("version", json()) == json(version) &&
          entry.value("runtime_sha256", json()) == json(runtime_sha256)) {
        return true;
      }
    }
    return false;
  }

  bool Quarantined(const Staged& staged) const { return IsQuarantined(staged.version, staged.runtime_sha256); }

  // A failed check removes staged.json and counts the release's discard; the third quarantines it, once (section 4.3).
  StartResult Discard(const Staged& staged, const std::string& why) {
    const bool removed = Remove(UpdateFile(names::kStagedJson));
    if (killed_) return Load();
    const int remove_error = removed ? 0 : last_error_;
    if (!state_.contains("discards") || !state_["discards"].is_object()) state_["discards"] = json::object();
    const std::uint64_t discards = state_["discards"].value(staged.version, std::uint64_t{0}) + 1;
    state_["discards"][staged.version] = discards;
    if (discards >= kDiscardsToQuarantine && !Quarantined(staged)) {
      if (!state_.contains("quarantine") || !state_["quarantine"].is_array()) state_["quarantine"] = json::array();
      state_["quarantine"].push_back(
          {{"version", staged.version}, {"runtime_sha256", staged.runtime_sha256}, {"reason", why}, {"time", ctx_.edges.clock.UtcIso()}});
    }
    const bool written = WriteJson(UpdateFile(names::kStateJson), state_);
    if (killed_) return Load();
    json fields = {{"to", staged.version}, {"discards", discards}, {"quarantined", discards >= kDiscardsToQuarantine}};
    if (!removed) {
      fields["staged_removed"] = false;
      fields["staged_win32_error"] = remove_error;
    }
    if (!written) {
      fields["state_written"] = false;
      fields["state_win32_error"] = last_error_;
    }
    Record("discarded", why.c_str(), fields);
    return Load();
  }

  StartResult SwapStaged() {
    json doc;
    Staged staged;
    int read_error = 0;
    const JsonRead read = ReadJson(UpdateFile(names::kStagedJson), &doc, &read_error);
    if (read == JsonRead::kUnreadable) {
      // Not a damaged file: it stays for a start that can read it, and nothing is counted.
      Record("deferred", "staged_unreadable", {{"win32_error", read_error}});
      return Load();
    }
    if (read != JsonRead::kOk || !ParseStaged(runtime_file_, doc, &staged)) {
      // A name outside the vocabulary: the file is not used (section 4.2), and not counted against any release.
      const bool removed = Remove(UpdateFile(names::kStagedJson));
      if (killed_) return Load();
      json fields = json::object();
      if (!removed) {
        fields["staged_removed"] = false;
        fields["staged_win32_error"] = last_error_;
      }
      Record("discarded", "bad_staged", fields);
      return Load();
    }
    if (IsQuarantined(staged.version, staged.runtime_sha256)) {
      // A release that failed to start is not offered again (ADR 0024, requirement 3): dropped, and not counted as a discard.
      const bool removed = Remove(UpdateFile(names::kStagedJson));
      if (killed_) return Load();
      json fields = {{"to", staged.version}};
      if (!removed) {
        fields["staged_removed"] = false;
        fields["staged_win32_error"] = last_error_;
      }
      Record("discarded", "quarantined", fields);
      return Load();
    }
    const std::string linked = FirstLink({StagingRel(staged.version), Child(StagingRel(staged.version), names::kPluginsDir)});
    if (!linked.empty()) {
      Record("blocked", "reparse_point", {{"to", staged.version}, {"folder", linked}});
      return Load();
    }
    for (const StagedFile& file : staged.files) {
      const std::string why = CheckStagedFile(staged, file);
      if (!why.empty()) return Discard(staged, why);
    }
    const InUseAnswer write = RuntimeWrite();
    if (write.state == InUse::kInUse) {
      Record("deferred", "runtime_in_use", {{"to", staged.version}, {"win32_error", write.os_error}});
      return Load();
    }
    if (write.state == InUse::kError) {
      // Not "in use": a cause the swap cannot see past (access denied, ...). Nothing moves until it is gone.
      Record("blocked", "runtime_not_writable", {{"to", staged.version}, {"win32_error", write.os_error}});
      return Load();
    }
    const ArtifactFacts runtime = Facts(Abs(runtime_file_));
    // A root with no runtime at all, and an edge that names the baseline: the baseline is the release replaced, and nothing is moved
    // out of the root for it (the baseline lives in the APK, not in the root).
    const bool from_baseline = !runtime.present && ctx_.baseline.present && names::IsVersionName(ctx_.baseline.version);
    if (!from_baseline && runtime.version.empty()) {
      // The release in the root has no NEVR-BUILD version to name update\previous\<from>\ by (or there is no runtime at all).
      // ADR 0024 does not say what happens then (an open question on #2105); until it does, nothing is moved.
      Record("blocked", "no_from_version", {{"to", staged.version}});
      return Load();
    }
    if (!from_baseline && !names::IsVersionName(runtime.version)) {
      // The NEVR-BUILD field of a file nothing has verified yet would name update\previous\<from>\: a value outside the
      // vocabulary is never used as a path (section 4.2), and never logged.
      Record("blocked", "bad_from_version", {{"to", staged.version}});
      return Load();
    }
    // Ownership (section 4.7): the runtime always; a plugin when state.json lists it with the sha256 of the file in the root.
    const std::vector<names::Member> out = from_baseline ? std::vector<names::Member>{} : OwnedMembers();
    for (const StagedFile& file : staged.files) {
      if (!file.member.runtime && fs_.Exists(InRoot(file.member)) && !HasMember(out, file.member)) {
        Record("blocked", "foreign_file", {{"to", staged.version}, {"path", MemberFile(runtime_file_, file.member)}});
        return Load();
      }
    }
    if (from_baseline) return Swap(staged, ctx_.baseline.version, ctx_.baseline.sha256, out);
    return Swap(staged, runtime.version, runtime.sha256, out);
  }

  // The older generation in update\<folder>\ (previous, failed) goes before a new one moves in (section 4.5). Only names in
  // the vocabulary. A version folder or its plugins folder that is a link stops it before anything is deleted, and `linked`
  // names it.
  bool ClearGenerations(const char* folder, std::string* linked) {
    const std::string previous = update_ + sep_ + folder;
    std::vector<DirEntry> entries;
    if (!fs_.ListFolder(previous, &entries)) return true;
    std::vector<std::string> versions;
    for (const DirEntry& entry : entries) {
      if (entry.is_dir && names::IsVersionName(entry.name)) {
        const std::string rel = GenerationRel(folder, entry.name);
        *linked = FirstLink({rel, Child(rel, names::kPluginsDir)});
        if (!linked->empty()) break;
        versions.push_back(entry.name);
      }
    }
    if (!linked->empty()) return false;
    for (const std::string& version : versions) {
      const std::string dir = previous + sep_ + version;
      if (!Remove(dir + sep_ + runtime_file_)) return false;
      const std::string plugins = dir + sep_ + names::kPluginsDir;
      std::vector<DirEntry> files;
      if (fs_.ListFolder(plugins, &files)) {
        for (const DirEntry& file : files) {
          names::Member member;
          if (!file.is_dir && names::ParseMember(std::string(names::kPluginPrefix) + file.name, &member) &&
              !Remove(plugins + sep_ + file.name)) {
            return false;
          }
        }
      }
      if (!RemoveDir(plugins) || !RemoveDir(dir)) return false;
    }
    return true;
  }

  // Journal, out, in, done (section 4.5). A failed move or journal write is undone to the last whole state.
  StartResult Swap(const Staged& staged, const std::string& from, const std::string& from_sha256, const std::vector<names::Member>& out) {
    json files = json::array();
    Journal journal;
    journal.from = from;
    journal.to = staged.version;
    journal.phase = "out";
    journal.out = out;
    journal.from_sha256 = from_sha256;
    for (const StagedFile& file : staged.files) {
      journal.in.push_back(file.member);
      journal.in_sha256[MemberFile(runtime_file_, file.member)] = file.sha256;
      files.push_back({{"path", MemberFile(runtime_file_, file.member)}, {"sha256", file.sha256}});
    }
    const json identity = {{"from", from}, {"to", staged.version}};
    const auto failed = [&](bool phase_in) {
      if (killed_) return Load();
      json fields = identity;
      fields["win32_error"] = last_error_;
      // Put back what moved; when that fails too the journal stays and the start loads nothing.
      const bool undone = UndoMoves(journal, phase_in);
      if (killed_) return Load();
      fields["undone"] = undone;
      Record("blocked", "move_failed", fields);
      return undone ? Load() : Stop("move_failed");
    };
    std::string linked;
    if (!ClearGenerations(names::kPreviousDir, &linked)) {
      if (killed_ || linked.empty()) return failed(false);
      json fields = identity;
      fields["folder"] = linked;
      Record("blocked", "reparse_point", fields);
      return Load();
    }
    if (!WriteJson(UpdateFile(names::kSwapJson), JournalJson(runtime_file_, journal))) {
      if (killed_) return Load();
      json fields = identity;
      fields["win32_error"] = last_error_;
      Record("blocked", "journal_write_failed", fields);
      return Load();
    }
    const std::string previous = PreviousDir(from);
    fs_.MakeDir(update_ + sep_ + names::kPreviousDir);
    fs_.MakeDir(previous);
    for (const names::Member& member : journal.out) {
      MakeDirFor(In(previous, member));
      if (!Move(InRoot(member), In(previous, member))) return failed(false);
    }
    journal.phase = "in";
    if (!WriteJson(UpdateFile(names::kSwapJson), JournalJson(runtime_file_, journal))) return failed(false);
    const std::string staging = StagingDir(staged.version);
    for (const names::Member& member : journal.in) {
      MakeDirFor(InRoot(member));
      if (!Move(In(staging, member), InRoot(member))) return failed(true);
    }
    journal.phase = "done";
    if (!WriteJson(UpdateFile(names::kSwapJson), JournalJson(runtime_file_, journal))) return failed(true);
    const bool finished = FinishDone(journal);
    if (killed_) return Load();
    json fields = identity;
    fields["files"] = files;
    if (!finished) fields["cleanup"] = "incomplete";  // the next start finishes it from the journal
    Record("swapped", nullptr, fields);
    return Trial();
  }

  // ---- the pending release (ADR 0024, section 6) --------------------------------------------------------------------

  // Ownership (section 4.7): the runtime always; a plugin when state.json lists it with the sha256 of the file in the root.
  std::vector<names::Member> OwnedMembers() const {
    std::vector<names::Member> out = {names::Member{true, {}}};
    if (!state_.contains("owned")) return out;
    for (const json& entry : state_["owned"]) {
      names::Member member;
      ParseFileMember(runtime_file_, entry["path"].get<std::string>(), &member);
      if (member.runtime || HasMember(out, member)) continue;
      const ArtifactFacts facts = Facts(InRoot(member));
      if (facts.present && facts.sha256 == entry["sha256"].get<std::string>()) out.push_back(member);
    }
    return out;
  }

  // The files of a release kept in a generation folder: its runtime, then its plugins in name order.
  std::vector<names::Member> MembersIn(const std::string& dir) const {
    std::vector<names::Member> out;
    if (fs_.Exists(dir + sep_ + runtime_file_)) out.push_back(names::Member{true, {}});
    std::vector<std::string> plugins;
    std::vector<DirEntry> entries;
    if (fs_.ListFolder(dir + sep_ + names::kPluginsDir, &entries)) {
      for (const DirEntry& entry : entries) {
        names::Member member;
        if (!entry.is_dir && names::ParseMember(std::string(names::kPluginPrefix) + entry.name, &member)) {
          plugins.push_back(member.plugin);
        }
      }
    }
    std::sort(plugins.begin(), plugins.end());
    for (const std::string& plugin : plugins) out.push_back(names::Member{false, plugin});
    return out;
  }

  // Section 4.6, "A rollback is never undone: it is completed". Phase out moves the failed release's files to
  // update\failed\<failed>\, phase in moves the previous release's files back, phase done finishes. Every step can be run
  // again: a file is moved only when it is still where the journal says it was.
  bool CompleteRollback(Journal* journal) {
    const std::string failed = FailedDir(journal->from);
    const std::string previous = PreviousDir(journal->to);
    if (journal->phase == "out") {
      fs_.MakeDir(update_ + sep_ + names::kFailedDir);
      fs_.MakeDir(failed);
      for (const names::Member& member : journal->out) {
        if (!fs_.Exists(InRoot(member)) || fs_.Exists(In(failed, member))) continue;
        MakeDirFor(In(failed, member));
        if (!Move(InRoot(member), In(failed, member))) return false;
      }
      journal->phase = "in";
      if (!WriteJson(UpdateFile(names::kSwapJson), JournalJson(runtime_file_, *journal))) return false;
    }
    if (journal->phase == "in") {
      for (const names::Member& member : journal->in) {
        if (!fs_.Exists(In(previous, member)) || fs_.Exists(InRoot(member))) continue;
        MakeDirFor(InRoot(member));
        if (!Move(In(previous, member), InRoot(member))) return false;
      }
      journal->phase = "done";
      if (!WriteJson(UpdateFile(names::kSwapJson), JournalJson(runtime_file_, *journal))) return false;
    }
    return FinishRollback(*journal);
  }

  // At done: the files that came back are the owned ones, the previous release is current, the failed one is quarantined
  // (before pending.json goes, so it is never offered again), then the pending record and the crash marker, the emptied
  // generation folder and the journal. Every step can be run again.
  bool FinishRollback(const Journal& journal) {
    json owned = json::array();
    std::string runtime_sha256;
    for (const names::Member& member : journal.in) {
      const std::string path = MemberFile(runtime_file_, member);
      const auto known = journal.in_sha256.find(path);
      std::string sha256;
      if (known != journal.in_sha256.end()) {
        if (fs_.Exists(InRoot(member))) sha256 = known->second;  // a file removed since is not owned
      } else {
        const ArtifactFacts facts = Facts(InRoot(member));
        if (facts.unreadable) return false;  // held, not gone: the next start finishes this
        sha256 = facts.sha256;
      }
      if (member.runtime) runtime_sha256 = sha256;
      if (sha256.empty()) continue;
      owned.push_back({{"path", path}, {"sha256", sha256}});
    }
    state_["owned"] = owned;
    if (!runtime_sha256.empty()) state_["current"] = {{"version", journal.to}, {"runtime_sha256", runtime_sha256}};
    if (!IsQuarantined(journal.from, journal.failed_sha256)) {
      if (!state_.contains("quarantine") || !state_["quarantine"].is_array()) state_["quarantine"] = json::array();
      state_["quarantine"].push_back(
          {{"version", journal.from}, {"runtime_sha256", journal.failed_sha256}, {"reason", journal.reason}, {"time", ctx_.edges.clock.UtcIso()}});
    }
    if (!WriteJson(UpdateFile(names::kStateJson), state_)) return false;
    if (!Remove(UpdateFile(names::kCrashedFile)) || !Remove(UpdateFile(names::kPendingJson))) return false;
    const std::string previous = PreviousDir(journal.to);
    if (!RemoveDir(SubOf(previous, names::kPluginsDir)) || !RemoveDir(previous)) return false;
    return Remove(UpdateFile(names::kSwapJson));
  }

  StartResult RecoverRollback(const Journal& journal, bool state_readable) {
    json fields = {{"kind", "rollback"}, {"phase", journal.phase}, {"from", journal.from}, {"to", journal.to}};
    const std::string linked = FirstLink({Child(names::kUpdateDir, names::kFailedDir), FailedRel(journal.from),
                                          Child(FailedRel(journal.from), names::kPluginsDir), PreviousRel(journal.to),
                                          Child(PreviousRel(journal.to), names::kPluginsDir)});
    if (!linked.empty()) {
      // Nothing is moved or removed through the link. At done every file is in place, so the start loads.
      fields["folder"] = linked;
      Record("blocked", "reparse_point", fields);
      return journal.phase == "done" ? Load() : Stop("reparse_point");
    }
    if (journal.phase == "done" && !state_readable) return Load();
    Journal resumed = journal;
    const bool whole = CompleteRollback(&resumed);
    if (killed_) return Load();
    if (!whole) {
      fields["win32_error"] = last_error_;
      Record("blocked", "move_failed", fields);
      return journal.phase == "done" ? Load() : Stop("move_failed");
    }
    Record("recovered", nullptr, fields);
    return Load();
  }

  // The rollback itself: checks first and moves nothing until they pass, then the journal and CompleteRollback. kBlocked
  // leaves the root as it was (the pending release stays); kStop is a cause the start cannot go on from, in `stop`.
  RollbackEnd DoRollback(const Pending& pending, const std::string& reason, const char* mode, StartResult* stop) {
    json common = {{"mode", mode}, {"from", pending.to_version}, {"boot_attempts", pending.boot_attempts}};
    if (pending.has_from) common["to"] = pending.from_version;
    const auto fail = [&](const char* why, RollbackEnd end, json extra = json::object()) {
      json fields = common;
      for (auto& [key, value] : extra.items()) fields[key] = value;
      RecordRollback("failed", why, fields);
      if (end == RollbackEnd::kStop) *stop = Stop(why);
      return end;
    };
    if (!pending.has_from) return fail("no_previous", RollbackEnd::kStop);
    const std::string& restored = pending.from_version;
    const std::string linked = FirstLink({Child(names::kUpdateDir, names::kFailedDir), FailedRel(pending.to_version),
                                          Child(FailedRel(pending.to_version), names::kPluginsDir), PreviousRel(restored),
                                          Child(PreviousRel(restored), names::kPluginsDir)});
    if (!linked.empty()) return fail("reparse_point", RollbackEnd::kBlocked, {{"folder", linked}});
    const std::string previous = PreviousDir(restored);
    const std::string previous_runtime = previous + sep_ + runtime_file_;
    // The baseline is the release a fresh root swapped over (see StartContext::baseline): nothing is restored from previous\, the root
    // is left with no runtime, and the start loads the baseline. Only the failed runtime moves (to failed\).
    const bool to_baseline = ctx_.baseline.present && restored == ctx_.baseline.version && pending.from_sha256 == ctx_.baseline.sha256;
    if (!to_baseline) {
      // The previous release's runtime is there and verifies (ADR 0024, section 6), or the answer is why not.
      const RollbackCheck target = ctx_.edges.rollback.Check(previous_runtime);
      if (target.state == RollbackState::kAbsent) return fail("no_previous", RollbackEnd::kStop);
      if (target.state == RollbackState::kUnverified) {
        return fail("previous_unverified", RollbackEnd::kStop, {{"verify_reason", target.reason}});
      }
    }

    Journal journal;
    journal.kind = "rollback";
    journal.from = pending.to_version;
    journal.to = restored;
    journal.phase = "out";
    journal.out = OwnedMembers();
    journal.in = MembersIn(previous);
    journal.failed_sha256 = pending.to_sha256;
    journal.reason = reason;
    // The sha256 of every file that comes back, read before anything moves, so the cleanup at done needs no re-read.
    for (const names::Member& member : journal.in) {
      const ArtifactFacts facts = Facts(In(previous, member));
      if (facts.unreadable || facts.sha256.empty()) {
        return fail("previous_unreadable", RollbackEnd::kBlocked, {{"path", MemberFile(runtime_file_, member)}});
      }
      journal.in_sha256[MemberFile(runtime_file_, member)] = facts.sha256;
    }
    // A file that is not the failed release's sits where a restored plugin goes: it is not replaced (section 4.7).
    for (const names::Member& member : journal.in) {
      if (!member.runtime && fs_.Exists(InRoot(member)) && !HasMember(journal.out, member)) {
        return fail("foreign_file", RollbackEnd::kBlocked, {{"path", MemberFile(runtime_file_, member)}});
      }
    }
    std::string cleared;
    if (!ClearGenerations(names::kFailedDir, &cleared)) {
      if (killed_) return RollbackEnd::kBlocked;
      if (!cleared.empty()) return fail("reparse_point", RollbackEnd::kBlocked, {{"folder", cleared}});
      return fail("move_failed", RollbackEnd::kBlocked, {{"win32_error", last_error_}});
    }
    if (!WriteJson(UpdateFile(names::kSwapJson), JournalJson(runtime_file_, journal))) {
      if (killed_) return RollbackEnd::kBlocked;
      return fail("journal_write_failed", RollbackEnd::kBlocked, {{"win32_error", last_error_}});
    }
    if (!CompleteRollback(&journal)) {
      if (killed_) return RollbackEnd::kBlocked;
      // The journal stays: a rollback is completed by the next start, and the root may be half moved now.
      return fail("move_failed", RollbackEnd::kStop, {{"win32_error", last_error_}});
    }
    json files = json::array();
    for (const names::Member& member : journal.in) {
      files.push_back({{"path", MemberFile(runtime_file_, member)}, {"sha256", Facts(InRoot(member)).sha256}});
    }
    json fields = common;
    fields["files"] = files;
    RecordRollback("rolled_back", reason.c_str(), fields);
    return RollbackEnd::kDone;
  }

  // ADR 0024, section 6, R2: what a start does with a release that is pending. The decision is nevr_update::DecidePending
  // over the facts this start reads through UpdateFs; this member carries out the action it names: the removals, the count,
  // the rollback and the records, in the order the decision was written for.
  PendingStep DecidePending(StartResult* stop) {
    PendingFacts facts = ReadPendingFacts(fs_, update_, std::string(), finished_swap_);
    if (facts.pending_read == ReadResult::kOk) {
      // The root's runtime is measured only when a record is pending: the decision compares the record's release with it.
      const ArtifactFacts runtime = Facts(Abs(runtime_file_));
      if (runtime.present) facts.root_runtime_sha256 = runtime.sha256;
    }
    const PendingDecision decision = nevr_update::DecidePending(facts, [this]() { return WriteAccessOf(RuntimeWrite()); });
    const Pending& pending = decision.pending;
    const auto next_start_fields = [&pending]() {
      json fields = {{"mode", "next_start"}, {"from", pending.to_version}, {"boot_attempts", pending.boot_attempts}};
      if (pending.has_from) fields["to"] = pending.from_version;
      return fields;
    };
    switch (decision.action) {
      case PendingAction::kNone:
        return PendingStep::kNone;
      case PendingAction::kUnreadable:
        // Not damage: the record is there and something keeps this start from it. Nothing is judged, counted or swapped.
        RecordRollback("deferred", "pending_unreadable", {{"win32_error", decision.os_error}});
        return PendingStep::kHold;
      case PendingAction::kDamaged: {
        // A record the host did not write: it names nothing the host can put back. The release in the root is verified at
        // every load, so it is treated as started and the record and its marker go.
        const bool removed = Remove(UpdateFile(names::kPendingJson)) && Remove(UpdateFile(names::kCrashedFile));
        if (killed_) return PendingStep::kHold;
        RecordRollback("discarded", "bad_pending", {{"removed", removed}});
        return PendingStep::kNone;
      }
      case PendingAction::kStale: {
        // The root holds a runtime this record is not about: the host moves no file it did not place (section 4.7).
        const bool removed = Remove(UpdateFile(names::kPendingJson)) && Remove(UpdateFile(names::kCrashedFile));
        if (killed_) return PendingStep::kHold;
        RecordRollback("discarded", "pending_stale", {{"from", pending.to_version}, {"removed", removed}});
        return PendingStep::kNone;
      }
      case PendingAction::kRollBack: {
        const RollbackEnd end = DoRollback(pending, decision.reason, "next_start", stop);
        if (killed_) return PendingStep::kHold;
        if (end == RollbackEnd::kStop) return PendingStep::kStop;
        return end == RollbackEnd::kDone ? PendingStep::kNone : PendingStep::kTrial;
      }
      case PendingAction::kDeferredInUse: {
        // Another process runs the pending release, perhaps inside its ten seconds: it is not condemned (R2).
        json common = next_start_fields();
        common["cause"] = decision.reason;
        RecordRollback("deferred", "runtime_in_use", common);
        return PendingStep::kTrial;
      }
      case PendingAction::kBlockedNotWritable: {
        json common = next_start_fields();
        common["cause"] = decision.reason;
        common["win32_error"] = decision.os_error;
        RecordRollback("blocked", "runtime_not_writable", common);
        return PendingStep::kTrial;
      }
      case PendingAction::kCountTry:
        // One more try. The count is written, or the start says it could not be.
        if (WriteJson(UpdateFile(names::kPendingJson), decision.counted)) {
          RecordTrial("counted", "try", pending, pending.boot_attempts + 1);
        } else {
          if (killed_) return PendingStep::kHold;
          RecordRollback("blocked", "pending_write_failed", {{"win32_error", last_error_}});
          RecordTrial("not_counted", "pending_write_failed", pending, pending.boot_attempts, {{"win32_error", last_error_}});
        }
        return PendingStep::kTrial;
      case PendingAction::kNotCountedInUse:
        // A start beside a running one is not a try, and the first one's trial is the trial.
        RecordTrial("not_counted", "runtime_in_use", pending, pending.boot_attempts);
        return PendingStep::kTrial;
      case PendingAction::kNotCountedNotWritable:
        RecordTrial("not_counted", "runtime_not_writable", pending, pending.boot_attempts, {{"win32_error", decision.os_error}});
        return PendingStep::kTrial;
      case PendingAction::kNotCountedSwapFinished:
        // The load that follows a swap whose cleanup this start finished is that swap's first try, not a second.
        RecordTrial("not_counted", "swap_finished", pending, pending.boot_attempts);
        return PendingStep::kTrial;
    }
    return PendingStep::kTrial;
  }

  // Writes `failed_reason` into the pending record `doc` was read from, unless it already names a failure (the first one
  // stays). True when the record names a failure afterwards. last_error_ holds the error of a write that failed.
  bool MarkInRecord(const json& doc, const std::string& reason) {
    if (doc.contains("failed_reason") && doc["failed_reason"].is_string()) return true;
    json marked = doc;
    marked["failed_reason"] = reason;
    return WriteJson(UpdateFile(names::kPendingJson), marked);
  }

  const StartContext& ctx_;
  const UpdateFs& fs_;
  std::uint64_t started_;
  char sep_ = '\\';
  std::string root_;
  std::string runtime_file_;  // the runtime's file in the root (StartContext::runtime_file)
  std::string update_;
  json state_ = json::object();
  int moves_ = 0;
  int last_error_ = 0;
  bool killed_ = false;
  int steps_ = 0;
  // A swap's cleanup this start finished: that swap's start was killed before its runtime could load, so this start's load
  // is the release's first try, not a second.
  bool finished_swap_ = false;
};

}  // namespace

namespace {

/// The reasons the decision gives on its own account, before a load outcome or the host names one.
constexpr char kReasonCrashed[] = "crashed";
constexpr char kReasonUnconfirmed[] = "unconfirmed";

}  // namespace

bool IsSha256Hex(std::string_view text) noexcept {
  if (text.size() != kSha256HexChars) return false;
  for (const char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

bool IsReasonToken(std::string_view text) noexcept {
  if (text.empty() || text.size() > kMaxReasonLength) return false;
  for (const char c : text) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

bool ParseJsonText(std::string_view text, json* out) {
  if (text.size() > names::kMaxJsonBytes || !NestedAtMost(text, names::kMaxJsonDepth)) return false;
  *out = json::parse(std::string(text), nullptr, false);
  return !out->is_discarded();
}

bool ParsePending(const json& doc, Pending* out) {
  if (!doc.is_object() || !doc.contains("to") || !doc["to"].is_object() || !doc.contains("boot_attempts") ||
      !doc["boot_attempts"].is_number_unsigned()) {
    return false;
  }
  const json& to = doc["to"];
  if (!to.contains("version") || !to["version"].is_string() || !to.contains("sha256") || !to["sha256"].is_string()) return false;
  out->to_version = to["version"].get<std::string>();
  out->to_sha256 = to["sha256"].get<std::string>();
  if (!names::IsVersionName(out->to_version) || !IsSha256Hex(out->to_sha256)) return false;
  out->boot_attempts = doc["boot_attempts"].get<std::uint64_t>();
  if (doc.contains("from") && !doc["from"].is_null()) {
    const json& from = doc["from"];
    if (!from.is_object() || !from.contains("version") || !from["version"].is_string() || !from.contains("sha256") ||
        !from["sha256"].is_string()) {
      return false;
    }
    out->from_version = from["version"].get<std::string>();
    out->from_sha256 = from["sha256"].get<std::string>();
    if (!names::IsVersionName(out->from_version) || !IsSha256Hex(out->from_sha256)) return false;
    out->has_from = true;
  }
  if (doc.contains("failed_reason") && !doc["failed_reason"].is_null()) {
    if (!doc["failed_reason"].is_string()) return false;
    out->failed_reason = doc["failed_reason"].get<std::string>();
    if (!IsReasonToken(out->failed_reason)) return false;
  }
  return true;
}

PendingFacts ReadPendingFacts(const UpdateFs& fs, const std::string& update_dir, std::string root_runtime_sha256,
                              bool finished_swap) {
  PendingFacts facts;
  const std::string separator(1, fs.Separator());
  facts.pending_read = fs.ReadSmallFile(update_dir + separator + names::kPendingJson, &facts.pending_text);
  // The error of this read is taken before the marker is asked for: a later call on the same UpdateFs may replace it.
  if (facts.pending_read == ReadResult::kUnreadable) facts.pending_os_error = static_cast<std::uint32_t>(fs.LastOsError());
  facts.crashed = fs.Exists(update_dir + separator + names::kCrashedFile);
  facts.root_runtime_sha256 = std::move(root_runtime_sha256);
  facts.finished_swap = finished_swap;
  return facts;
}

PendingDecision DecidePending(const PendingFacts& facts, const std::function<WriteAccess()>& write_access) {
  PendingDecision out;
  switch (facts.pending_read) {
    case ReadResult::kAbsent:
      out.action = PendingAction::kNone;
      return out;
    case ReadResult::kUnreadable:
      // Not damage: the record is there and something keeps this start from it. Nothing is judged, counted or swapped.
      out.action = PendingAction::kUnreadable;
      out.os_error = facts.pending_os_error;
      return out;
    case ReadResult::kTooBig:
      out.action = PendingAction::kDamaged;
      return out;
    case ReadResult::kOk:
      break;
  }
  json doc;
  // A record the host did not write names nothing the host can put back: it is dropped by the edge (see kDamaged).
  if (!ParseJsonText(facts.pending_text, &doc) || !ParsePending(doc, &out.pending)) {
    out.action = PendingAction::kDamaged;
    return out;
  }
  const Pending& pending = out.pending;
  // The root holds a runtime this record is not about: the host moves no file it did not place (ADR 0024, section 4.7).
  if (!facts.root_runtime_sha256.empty() && facts.root_runtime_sha256 != pending.to_sha256) {
    out.action = PendingAction::kStale;
    return out;
  }
  std::string reason = pending.failed_reason;
  if (reason.empty() && facts.crashed) reason = kReasonCrashed;
  if (reason.empty() && pending.boot_attempts >= kBootAttemptsToRollBack) reason = kReasonUnconfirmed;
  // The write access is asked once, after the record is judged not stale, in every branch that follows.
  const WriteAccess runtime = write_access();
  if (!reason.empty()) {
    out.reason = reason;
    if (runtime.kind == WriteAccess::Kind::kInUse) {
      // Another process runs the pending release, perhaps inside its ten seconds: it is not condemned (R2).
      out.action = PendingAction::kDeferredInUse;
    } else if (runtime.kind == WriteAccess::Kind::kNotWritable) {
      out.action = PendingAction::kBlockedNotWritable;
      out.os_error = runtime.os_error;
    } else {
      out.action = PendingAction::kRollBack;
    }
    return out;
  }
  // One more try. A start beside a running one is not a try, and neither is the load that follows a swap whose cleanup
  // this start finished: the swap's own start never got to load.
  if (runtime.kind == WriteAccess::Kind::kInUse) {
    out.action = PendingAction::kNotCountedInUse;
  } else if (runtime.kind == WriteAccess::Kind::kNotWritable) {
    out.action = PendingAction::kNotCountedNotWritable;
    out.os_error = runtime.os_error;
  } else if (facts.finished_swap) {
    out.action = PendingAction::kNotCountedSwapFinished;
  } else {
    out.action = PendingAction::kCountTry;
    out.counted = doc;
    out.counted["boot_attempts"] = pending.boot_attempts + 1;
  }
  return out;
}


PendingFailure ClassifyPendingFailure(LoadClass load, bool code_before_entry) noexcept {
  switch (load) {
    case LoadClass::kNeverRan: return PendingFailure::kNeverRan;
    case LoadClass::kEntryOnly:
      // A runtime whose constructors ran (PC: DllMain, at the map) is mapped and stays so; one that ran no code before its entry
      // (the Quest runtime, no constructors) never ran, so the previous release is put back in the same start.
      return code_before_entry ? PendingFailure::kMapped : PendingFailure::kNeverRan;
    case LoadClass::kInitFailed:
      // code_before_entry does not change the answer yet: both answers are kMapped. ADR 0024, section 6 (R1), defines the answer for a
      // runtime whose code ran, and RuntimeLoaderEdge::CodeBeforeEntry (edges.h) is the input that would split the two.
      static_cast<void>(code_before_entry);
      return PendingFailure::kMapped;
    case LoadClass::kLoaded:
    case LoadClass::kNotLoaded: break;
  }
  return PendingFailure::kNone;
}

StartResult RunStart(const StartContext& context) noexcept {
  try {
    SwapRun start(context);
    try {
      return start.Run();
    } catch (const std::exception&) {
      return start.AfterException();
    }
  } catch (const std::exception&) {
    // Not even the start could be set up, or its record written: nothing is known about the root, so nothing loads.
    return {StartAction::kStop, "internal_error", false};
  }
}

StartResult RollBackNowIn(const StartContext& context, const char* reason) noexcept {
  if (reason == nullptr) return {StartAction::kStop, "no_previous", false};
  try {
    SwapRun start(context);
    try {
      return start.RollBackNow(reason);
    } catch (const std::exception&) {
      return {StartAction::kStop, "internal_error", false};
    }
  } catch (const std::exception&) {
    return {StartAction::kStop, "internal_error", false};
  }
}

bool MarkFailedIn(const StartContext& context, const char* reason) noexcept {
  if (reason == nullptr) return false;
  try {
    SwapRun start(context);
    return start.MarkFailed(reason);
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace nevr_update
