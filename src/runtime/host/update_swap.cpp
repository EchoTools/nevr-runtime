#include "runtime/host/update_swap.h"

#include "runtime/ext/plugin_signature.h"
#include "runtime/host/file_digest.h"
#include "runtime/host/runtime_load.h"
#include "runtime/host/update_names.h"
#include "runtime/host/update_seams.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace nevr_host {
namespace {

namespace names = update_names;
using nlohmann::json;

std::wstring Widen(std::string_view ascii) {
  std::wstring out;
  for (const char c : ascii) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  return out;
}

std::wstring Join(const std::wstring& base, const std::wstring& child) { return base + L"\\" + child; }

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool IsDirectory(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsReparsePoint(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool IsSha256Hex(const std::string& text) {
  if (text.size() != 64) return false;
  for (const char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

std::string UtcNow() {
  SYSTEMTIME now{};
  GetSystemTime(&now);
  char text[32] = {};
  std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear, now.wMonth, now.wDay, now.wHour,
                now.wMinute, now.wSecond);
  return text;
}

// Two names of a release's files are one file when they differ only in case: Windows opens either.
bool SameName(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

// kUnreadable is a file that is there but cannot be opened or read (another program holds it, access is denied): not
// damage, and never treated as damage (logging.md Rule 10; ADR 0024, section 4.6). kBad is a file read whole that is
// not usable: too large, too deep, or not JSON.
enum class JsonRead { kAbsent, kUnreadable, kBad, kOk };

// True when the text's arrays and objects, outside strings, nest at most `max` deep. nlohmann's parser builds a tree
// without recursion, but copying or writing one recurses once per level, so a file nested tens of thousands deep would
// end the process; the text is measured before it is parsed.
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

// A JSON file of update\, at most names::kMaxJsonBytes and names::kMaxJsonDepth (ADR 0024, section 4.3). `error` is the
// Win32 error of a kUnreadable result.
JsonRead ReadJsonFile(const std::wstring& path, json* out, DWORD* error) {
  *error = 0;
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    *error = GetLastError();
    return (*error == ERROR_FILE_NOT_FOUND || *error == ERROR_PATH_NOT_FOUND) ? JsonRead::kAbsent
                                                                                : JsonRead::kUnreadable;
  }
  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) == FALSE) {
    *error = GetLastError();
    CloseHandle(file);
    return JsonRead::kUnreadable;
  }
  if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > names::kMaxJsonBytes) {
    CloseHandle(file);
    return JsonRead::kBad;
  }
  std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  if (!text.empty() && ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) == FALSE) {
    *error = GetLastError();
    CloseHandle(file);
    return JsonRead::kUnreadable;
  }
  CloseHandle(file);
  if (read != text.size() || !NestedAtMost(text, names::kMaxJsonDepth)) return JsonRead::kBad;
  *out = json::parse(text, nullptr, false);
  return out->is_discarded() ? JsonRead::kBad : JsonRead::kOk;
}

// The bytes, sha256 and NEVR-BUILD version of a file in the root. Empty fields when it cannot be read.
struct FileFacts {
  bool present = false;
  // The file is there (or may be) and could not be read: another program holds it, access is denied. Never the same as
  // absent, and never written into a record as if it were (ADR 0024, section 4.9).
  bool unreadable = false;
  std::string sha256;
  std::string version;
};

FileFacts FactsOf(const std::wstring& path) {
  FileFacts facts;
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    facts.unreadable = error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
    return facts;
  }
  facts.present = true;
  std::vector<unsigned char> bytes;
  if (ReadWholeFile(file, &bytes)) {
    facts.sha256 = Sha256Hex(bytes);
    facts.version = BuildVersion(bytes);
  } else {
    facts.unreadable = true;
  }
  CloseHandle(file);
  return facts;
}

// One file of a staged release (staged.json, ADR 0024 section 2).
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

// staged.json: {"version", "files": [{"path", "size", "sha256"}]}, the spelling of NevrPcStage's result. Every name in
// the vocabulary, no file twice (in any case), exactly one runtime.
bool ParseStaged(const json& doc, Staged* out) {
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
    if (!names::ParseMember(file["path"].get<std::string>(), &staged.member)) return false;
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
  // What the host verified before it moved anything, so the cleanup at `done` writes records from facts it has and not
  // from a re-read that can fail: the sha256 of every file that moves in, by member path, and for a swap the sha256 of the
  // runtime that moved out. A journal without them (an older one) falls back to reading the files.
  std::map<std::string, std::string> in_sha256;
  std::string from_sha256;
};

bool ParseMembers(const json& list, std::vector<names::Member>* out) {
  if (!list.is_array()) return false;
  for (const json& path : list) {
    names::Member member;
    if (!path.is_string() || !names::ParseMember(path.get<std::string>(), &member) || HasMember(*out, member)) return false;
    out->push_back(member);
  }
  return true;
}

// A reason word: what a load outcome, a failure or the host calls a cause. Lower case, digits and '_', at most 40.
bool IsReasonToken(const std::string& text) {
  if (text.empty() || text.size() > 40) return false;
  for (const char c : text) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

bool ParseJournal(const json& doc, Journal* out) {
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
      if (!names::ParseMember(path, &member) || !sha.is_string() || !IsSha256Hex(sha.get<std::string>())) return false;
      out->in_sha256[path] = sha.get<std::string>();
    }
  }
  return doc.contains("out") && doc.contains("in") && ParseMembers(doc["out"], &out->out) && ParseMembers(doc["in"], &out->in);
}

json JournalJson(const Journal& journal) {
  json out = json::array();
  json in = json::array();
  for (const names::Member& m : journal.out) out.push_back(names::MemberPath(m));
  for (const names::Member& m : journal.in) in.push_back(names::MemberPath(m));
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

// pending.json (ADR 0024, section 2): {"to": {"version", "sha256"}, "from": {"version", "sha256"} or null,
// "boot_attempts", "failed_reason": null or a reason word}. `from` is null when the swap found no previous runtime.
struct Pending {
  std::string to_version;
  std::string to_sha256;
  bool has_from = false;
  std::string from_version;
  std::string from_sha256;
  std::uint64_t boot_attempts = 0;
  std::string failed_reason;  // empty: null
};

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

// state.json (ADR 0024, section 2): an object; "owned", when present, a list of {"path", "sha256"} in the vocabulary;
// "discards", when present, an object of version to count; "quarantine", when present, a list.
bool ValidState(const json& doc) {
  if (!doc.is_object()) return false;
  if (doc.contains("owned")) {
    if (!doc["owned"].is_array()) return false;
    for (const json& entry : doc["owned"]) {
      names::Member member;
      if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string() || !entry.contains("sha256") ||
          !entry["sha256"].is_string() || !names::ParseMember(entry["path"].get<std::string>(), &member) ||
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

// One start's work on update\. Every path is composed here from the root and the vocabulary.
class SwapRun {
 public:
  SwapRun(const wchar_t* runtime_path, BootstrapDiagnostics& diagnostics, UpdateLock& lock)
      : diagnostics_(diagnostics), lock_(lock), started_(GetTickCount64()) {
    root_ = runtime_path;
    const std::size_t separator = root_.find_last_of(L"\\/");
    root_ = separator == std::wstring::npos ? std::wstring() : root_.substr(0, separator);
    update_ = Join(root_, names::kUpdateDir);
  }

  UpdateResult Run();

  /// R1, never ran (ADR 0024, section 6): put the previous release back now, in this start.
  UpdateResult RollBackNow(const char* reason);
  /// R1, mapped: write `failed_reason` into pending.json. True when it is there.
  bool MarkFailed(const char* reason);

  /// The answer after an exception: stop when a journal says the root may be half moved, else load.
  UpdateResult AfterException() {
    Record("blocked", "internal_error");
    return Exists(UpdateFile(names::kSwapJson)) ? Stop("internal_error") : Load();
  }

 private:
  static UpdateResult Load() { return {UpdateAction::kLoad, nullptr, false}; }
  // Load, and the release in the root is on trial (the load of it that fails is the host's to handle).
  static UpdateResult Trial() { return {UpdateAction::kLoad, nullptr, true}; }
  static UpdateResult Stop(const char* reason) { return {UpdateAction::kStop, reason, false}; }

  std::wstring UpdateFile(const wchar_t* name) const { return Join(update_, name); }
  // The folders the swap works in, relative to the root: what a reparse_point record names.
  static std::wstring StagingRel(const std::string& version) {
    return Join(Join(names::kUpdateDir, names::kStagingDir), Widen(version));
  }
  static std::wstring GenerationRel(const wchar_t* folder, const std::string& version) {
    return Join(Join(names::kUpdateDir, folder), Widen(version));
  }
  static std::wstring PreviousRel(const std::string& version) { return GenerationRel(names::kPreviousDir, version); }
  static std::wstring FailedRel(const std::string& version) { return GenerationRel(names::kFailedDir, version); }
  std::wstring StagingDir(const std::string& version) const { return Join(root_, StagingRel(version)); }
  std::wstring PreviousDir(const std::string& version) const { return Join(root_, PreviousRel(version)); }
  std::wstring FailedDir(const std::string& version) const { return Join(root_, FailedRel(version)); }
  std::string FirstLink(std::initializer_list<std::wstring> relative) const;
  std::wstring InRoot(const names::Member& member) const { return Join(root_, names::MemberRelativePath(member)); }
  std::wstring In(const std::wstring& dir, const names::Member& member) const {
    return Join(dir, names::MemberRelativePath(member));
  }

  // A step that changed the disk. False when the test seam ended the start here (as a kill would).
  bool Step() {
    ++steps_;
    if (SwapStepEnds(steps_)) killed_ = true;
    return !killed_;
  }

  bool Move(const std::wstring& from, const std::wstring& to);
  bool Remove(const std::wstring& path);
  bool RemoveDir(const std::wstring& path);
  bool WriteJson(const std::wstring& path, const json& doc);
  void MakeDirFor(const std::wstring& file) { CreateDirectoryW(file.substr(0, file.find_last_of(L'\\')).c_str(), nullptr); }

  // How state.json was read: kEnded when the test seam ended the start while the file was rebuilt.
  enum class StateRead { kOk, kUnreadable, kEnded };

  void Record(const char* outcome, const char* reason, json fields = json::object());
  void RemoveStaleTemporaries();
  StateRead LoadState();
  json RebuiltState() const;
  UpdateResult Recover(bool state_readable);
  UpdateResult RecoverRollback(const Journal& journal, bool state_readable);
  bool UndoMoves(const Journal& journal, bool phase_in);
  bool PendingNames(const std::string& version, const std::string& runtime_sha256) const;
  bool StagedNames(const std::string& version) const;
  bool FinishDone(const Journal& journal);
  UpdateResult SwapStaged();
  std::string CheckStagedFile(const Staged& staged, const StagedFile& file) const;
  bool Quarantined(const Staged& staged) const;
  UpdateResult Discard(const Staged& staged, const std::string& why);
  DWORD RuntimeWriteError() const;
  bool ClearGenerations(const wchar_t* folder, std::string* linked);
  UpdateResult Swap(const Staged& staged, const std::string& from, const std::string& from_sha256,
                    const std::vector<names::Member>& out);

  // The pending release (ADR 0024, section 6).
  enum class PendingStep { kNone, kTrial, kHold, kStop };
  enum class RollbackEnd { kDone, kBlocked, kStop };
  void RecordRollback(const char* outcome, const char* reason, json fields = json::object());
  void RecordTrial(const char* outcome, const char* reason, const Pending& pending, std::uint64_t boot_attempts,
                   json fields = json::object());
  bool MarkInRecord(const json& doc, const std::string& reason);
  std::vector<names::Member> OwnedMembers() const;
  std::vector<names::Member> MembersIn(const std::wstring& dir) const;
  std::string VerifyRuntimeAt(const std::wstring& path) const;
  bool IsQuarantined(const std::string& version, const std::string& runtime_sha256) const;
  PendingStep DecidePending(UpdateResult* stop);
  RollbackEnd DoRollback(const Pending& pending, const std::string& reason, const char* mode, UpdateResult* stop);
  bool CompleteRollback(Journal* journal);
  bool FinishRollback(const Journal& journal);

  BootstrapDiagnostics& diagnostics_;
  UpdateLock& lock_;
  ULONGLONG started_;
  std::wstring root_;
  std::wstring update_;
  json state_ = json::object();
  int moves_ = 0;
  DWORD last_error_ = 0;
  bool killed_ = false;
  int steps_ = 0;
  // A swap's cleanup this start finished: that swap's start was killed before its runtime could load, so this start's
  // load is the release's first try, not a second.
  bool finished_swap_ = false;
};

void SwapRun::Record(const char* outcome, const char* reason, json fields) {
  if (killed_) return;  // a kill writes nothing after the step it ended on
  json record = std::move(fields);
  record["event"] = "update_swap";
  record["outcome"] = outcome;
  if (reason != nullptr) record["reason"] = reason;
  record["elapsed_ms"] = GetTickCount64() - started_;
  diagnostics_.Emit(record);
}

bool SwapRun::Move(const std::wstring& from, const std::wstring& to) {
  if (killed_) return false;
  ++moves_;
  if (SwapMoveFails(moves_, &last_error_)) return false;
  // No MOVEFILE_COPY_ALLOWED: a move to another volume fails, and the journal undoes it (ADR 0024, section 4.2).
  // No MOVEFILE_REPLACE_EXISTING: a file where one is going is a failure, never overwritten.
  if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE) {
    last_error_ = GetLastError();
    return false;
  }
  return Step();
}

bool SwapRun::Remove(const std::wstring& path) {
  if (killed_) return false;
  if (!Exists(path)) return true;
  if (DeleteFileW(path.c_str()) == FALSE) {
    last_error_ = GetLastError();
    return false;
  }
  return Step();
}

bool SwapRun::RemoveDir(const std::wstring& path) {
  if (killed_) return false;
  if (!IsDirectory(path)) return true;
  if (RemoveDirectoryW(path.c_str()) == FALSE) return true;  // not empty: something that is not ours stays
  return Step();
}

// A temporary file of this process, flushed, then renamed over the file: a reader sees the old file or the new one.
bool SwapRun::WriteJson(const std::wstring& path, const json& doc) {
  if (killed_) return false;
  const std::wstring temporary = path + L".tmp-" + std::to_wstring(GetCurrentProcessId());
  const std::string text = doc.dump();
  const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    last_error_ = GetLastError();
    return false;
  }
  DWORD written = 0;
  const bool wrote = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE &&
                     written == text.size() && FlushFileBuffers(file) != FALSE;
  if (!wrote) last_error_ = GetLastError();
  CloseHandle(file);
  if (!wrote ||
      MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
    if (wrote) last_error_ = GetLastError();
    DeleteFileW(temporary.c_str());
    return false;
  }
  return Step();
}

// The first of `relative` (folders relative to the root, each parent before its children) that is a reparse point,
// spelled as the record names it ("update/staging/1.2.3"); empty when none is. ADR 0024 section 4.2: the swap never
// moves, makes or deletes anything through a link out of the root. Every name here is from the vocabulary.
std::string SwapRun::FirstLink(std::initializer_list<std::wstring> relative) const {
  for (const std::wstring& dir : relative) {
    if (!IsReparsePoint(Join(root_, dir))) continue;
    std::string label;
    for (const wchar_t c : dir) label.push_back(c == L'\\' ? '/' : static_cast<char>(c));
    return label;
  }
  return {};
}

// What a damaged state.json is replaced by (ADR 0024, section 4.8): the runtime in the root, owned, and current when
// its NEVR-BUILD version is a version name (it is read from a file nothing has verified yet).
json SwapRun::RebuiltState() const {
  json state = {{"owned", json::array()}};
  const FileFacts runtime = FactsOf(Join(root_, names::kRuntimeFile));
  if (!runtime.sha256.empty()) {
    state["owned"].push_back({{"path", names::kRuntimeMember}, {"sha256", runtime.sha256}});
    if (names::IsVersionName(runtime.version)) {
      state["current"] = {{"version", runtime.version}, {"runtime_sha256", runtime.sha256}};
    }
  }
  return state;
}

// A start killed between a JSON file's temporary write and its rename (WriteJson) leaves <file>.tmp-<pid>. Under the
// lock no writer is live, so each one of the host's own files is a leftover: it is removed, and nothing else is.
void SwapRun::RemoveStaleTemporaries() {
  std::vector<std::wstring> stale;
  WIN32_FIND_DATAW data;
  const HANDLE find = FindFirstFileW(UpdateFile(L"*.tmp-*").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    const std::wstring name = data.cFileName;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    for (const wchar_t* file : {names::kStateJson, names::kPendingJson, names::kSwapJson}) {
      const std::wstring prefix = std::wstring(file) + L".tmp-";
      if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
          name.find_first_not_of(L"0123456789", prefix.size()) == std::wstring::npos) {
        stale.push_back(name);
      }
    }
  } while (FindNextFileW(find, &data) != FALSE);
  FindClose(find);
  int removed = 0;
  int failed = 0;
  DWORD error = 0;
  for (const std::wstring& name : stale) {
    if (Remove(UpdateFile(name.c_str()))) {
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

SwapRun::StateRead SwapRun::LoadState() {
  json doc;
  DWORD error = 0;
  const JsonRead read = ReadJsonFile(UpdateFile(names::kStateJson), &doc, &error);
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
  if (read == JsonRead::kOk && ValidState(doc)) {
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

UpdateResult SwapRun::Run() {
  if (root_.empty() || !IsDirectory(update_)) return {UpdateAction::kNoUpdateFolder, nullptr, false};
  // update\ itself first: the lock is a file inside it, and a link is never followed, not even to take the lock.
  if (IsReparsePoint(update_)) {
    Record("blocked", "reparse_point", {{"folder", "update"}});
    return Load();
  }
  if (!lock_.Acquire(update_.c_str(), SwapLockTimeoutMs(), SwapLockStepMs())) {
    // Another start holds it past the wait. With a journal the root may be half moved: nothing is loaded.
    const bool journal = Exists(UpdateFile(names::kSwapJson));
    Record("blocked", "lock_timeout", {{"journal", journal}});
    return journal ? Stop("lock_timeout") : Load();
  }
  const bool journal = Exists(UpdateFile(names::kSwapJson));
  const std::string linked =
      FirstLink({Join(names::kUpdateDir, names::kStagingDir), Join(names::kUpdateDir, names::kPreviousDir),
                 Join(names::kUpdateDir, names::kFailedDir), names::kPluginsDir});
  if (!linked.empty()) {
    // With a journal the root may be half moved, and its recovery would move files through the link: nothing loads.
    Record("blocked", "reparse_point", {{"folder", linked}, {"journal", journal}});
    return journal ? Stop("reparse_point") : Load();
  }
  RemoveStaleTemporaries();
  if (killed_) return Load();
  if (!journal && !Exists(UpdateFile(names::kStagedJson)) && !Exists(UpdateFile(names::kPendingJson))) return Load();
  const StateRead state = LoadState();
  if (state == StateRead::kEnded) return Load();
  if (journal) {
    const UpdateResult recovered = Recover(state == StateRead::kOk);
    // A journal still there is a recovery that did not finish: no new swap starts over it.
    if (killed_ || recovered.action == UpdateAction::kStop || Exists(UpdateFile(names::kSwapJson))) return recovered;
  }
  if (state != StateRead::kOk) return Load();  // its record is written: no swap and no rollback without the ownership list
  if (Exists(UpdateFile(names::kPendingJson))) {
    UpdateResult stop;
    switch (DecidePending(&stop)) {
      case PendingStep::kStop: return stop;
      case PendingStep::kHold: return Load();
      case PendingStep::kTrial:
        // A staged release waits: swapping now would move the unconfirmed release over the last confirmed one.
        if (Exists(UpdateFile(names::kStagedJson))) Record("deferred", "release_pending");
        return Trial();
      case PendingStep::kNone: break;
    }
    if (killed_) return Load();
  }
  if (Exists(UpdateFile(names::kStagedJson))) return SwapStaged();
  return Load();
}

// ADR 0024, section 4.6: out or in are undone to the last whole state; done finishes its cleanup, which writes
// state.json and so waits for a start that can read it (`state_readable`).
UpdateResult SwapRun::Recover(bool state_readable) {
  json doc;
  Journal journal;
  DWORD error = 0;
  const JsonRead read = ReadJsonFile(UpdateFile(names::kSwapJson), &doc, &error);
  if (read == JsonRead::kUnreadable) {
    // The journal may be fine; this start cannot tell, and the root may be half moved.
    Record("blocked", "journal_unreadable", {{"win32_error", error}});
    return Stop("journal_unreadable");
  }
  if (read != JsonRead::kOk || !ParseJournal(doc, &journal)) {
    Record("blocked", "bad_journal");
    return Stop("bad_journal");
  }
  if (journal.kind == "rollback") return RecoverRollback(journal, state_readable);
  json fields = {{"kind", "swap"}, {"phase", journal.phase}, {"from", journal.from}, {"to", journal.to}};
  const std::string linked =
      FirstLink({StagingRel(journal.to), Join(StagingRel(journal.to), names::kPluginsDir), PreviousRel(journal.from),
                 Join(PreviousRel(journal.from), names::kPluginsDir)});
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
bool SwapRun::UndoMoves(const Journal& journal, bool phase_in) {
  const std::wstring staging = StagingDir(journal.to);
  const std::wstring previous = PreviousDir(journal.from);
  if (phase_in) {
    for (auto it = journal.in.rbegin(); it != journal.in.rend(); ++it) {
      if (Exists(InRoot(*it)) && !Exists(In(staging, *it))) {
        MakeDirFor(In(staging, *it));
        if (!Move(InRoot(*it), In(staging, *it))) return false;
      }
    }
  }
  for (auto it = journal.out.rbegin(); it != journal.out.rend(); ++it) {
    if (Exists(In(previous, *it)) && !Exists(InRoot(*it))) {
      MakeDirFor(InRoot(*it));
      if (!Move(In(previous, *it), InRoot(*it))) return false;
    }
  }
  return Remove(UpdateFile(names::kSwapJson));
}

// True when pending.json is the record of this swap: `to` is `version` with the runtime's sha256.
bool SwapRun::PendingNames(const std::string& version, const std::string& runtime_sha256) const {
  json doc;
  DWORD error = 0;
  if (runtime_sha256.empty() || ReadJsonFile(UpdateFile(names::kPendingJson), &doc, &error) != JsonRead::kOk) return false;
  if (!doc.is_object() || !doc.contains("to") || !doc["to"].is_object()) return false;
  const json& to = doc["to"];
  return to.value("version", json()) == json(version) && to.value("sha256", json()) == json(runtime_sha256);
}

// True when staged.json, read whole, names `version`.
bool SwapRun::StagedNames(const std::string& version) const {
  json doc;
  DWORD error = 0;
  return ReadJsonFile(UpdateFile(names::kStagedJson), &doc, &error) == JsonRead::kOk && doc.is_object() &&
         doc.value("version", json()) == json(version);
}

// At done: the files the swap placed are owned, a stale crash marker goes and the release is pending, then
// staged.json, the staging folder and the journal go (ADR 0024, section 4.5). Every step can be run again, and a run
// that finishes an earlier start's cleanup undoes nothing that happened since: a pending.json that already names this
// swap is kept with what the release did since (boot_attempts, failed_reason) and so is a crash marker (the stale one
// went before pending.json was written), and a staged.json that names another release is the next swap's.
bool SwapRun::FinishDone(const Journal& journal) {
  json owned = json::array();
  std::string to_sha256;
  for (const names::Member& member : journal.in) {
    const std::string path = names::MemberPath(member);
    const auto known = journal.in_sha256.find(path);
    std::string sha256;
    if (known != journal.in_sha256.end()) {
      if (Exists(InRoot(member))) sha256 = known->second;  // a file removed since the swap is not owned
    } else {
      const FileFacts facts = FactsOf(InRoot(member));
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
      const FileFacts previous = FactsOf(Join(PreviousDir(journal.from), names::kRuntimeFile));
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
  const std::wstring staging = StagingDir(journal.to);
  if (!RemoveDir(Join(staging, names::kPluginsDir)) || !RemoveDir(staging)) return false;
  return Remove(UpdateFile(names::kSwapJson));
}

// ADR 0024, section 4.3: the file as it is on disk now, against staged.json and the verifier. Empty when it passes,
// else the reason: "unreadable", "size", "sha256", or the verifier's own words.
std::string SwapRun::CheckStagedFile(const Staged& staged, const StagedFile& file) const {
  const std::wstring path = In(StagingDir(staged.version), file.member);
  const HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return "unreadable";
  std::string why;
  LARGE_INTEGER size{};
  std::vector<unsigned char> bytes;
  if (GetFileSizeEx(handle, &size) == FALSE || static_cast<std::uint64_t>(size.QuadPart) != file.size) {
    why = "size";
  } else if (!ReadWholeFile(handle, &bytes) || Sha256Hex(bytes) != file.sha256) {
    why = "sha256";
  } else {
    const nevr_plugin_signature::Verdict verdict = nevr_plugin_signature::VerifyPluginFile(
        handle, HostVerifyPolicy(), static_cast<std::int64_t>(std::time(nullptr)));
    if (!verdict.accepted()) why = verdict.Describe();
  }
  CloseHandle(handle);  // closed before any rename: a rename needs the share mode this handle refuses
  return why;
}

// True when state.json's quarantine already holds this release (its version and runtime sha256).
bool SwapRun::IsQuarantined(const std::string& version, const std::string& runtime_sha256) const {
  if (!state_.contains("quarantine") || !state_["quarantine"].is_array()) return false;
  for (const json& entry : state_["quarantine"]) {
    if (entry.is_object() && entry.value("version", json()) == json(version) &&
        entry.value("runtime_sha256", json()) == json(runtime_sha256)) {
      return true;
    }
  }
  return false;
}

bool SwapRun::Quarantined(const Staged& staged) const { return IsQuarantined(staged.version, staged.runtime_sha256); }

// A failed check removes staged.json and counts the release's discard; the third quarantines it, once (section 4.3).
// A staged.json that cannot be removed comes back at the next start, so the record says so, with the error.
UpdateResult SwapRun::Discard(const Staged& staged, const std::string& why) {
  const bool removed = Remove(UpdateFile(names::kStagedJson));
  if (killed_) return Load();
  const DWORD remove_error = removed ? 0 : last_error_;
  if (!state_.contains("discards") || !state_["discards"].is_object()) state_["discards"] = json::object();
  const std::uint64_t discards = state_["discards"].value(staged.version, std::uint64_t{0}) + 1;
  state_["discards"][staged.version] = discards;
  if (discards >= kDiscardsToQuarantine && !Quarantined(staged)) {
    if (!state_.contains("quarantine") || !state_["quarantine"].is_array()) state_["quarantine"] = json::array();
    state_["quarantine"].push_back(
        {{"version", staged.version}, {"runtime_sha256", staged.runtime_sha256}, {"reason", why}, {"time", UtcNow()}});
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

// ADR 0024, section 4.4: a runtime mapped as an image by another process refuses a write-open with a sharing
// violation. 0 when it opens (or there is none), else the error.
DWORD SwapRun::RuntimeWriteError() const {
  const std::wstring path = Join(root_, names::kRuntimeFile);
  const HANDLE file =
      CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    CloseHandle(file);
    return 0;
  }
  const DWORD error = GetLastError();
  return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? 0 : error;
}

UpdateResult SwapRun::SwapStaged() {
  json doc;
  Staged staged;
  DWORD read_error = 0;
  const JsonRead read = ReadJsonFile(UpdateFile(names::kStagedJson), &doc, &read_error);
  if (read == JsonRead::kUnreadable) {
    // Not a damaged file: it stays for a start that can read it, and nothing is counted.
    Record("deferred", "staged_unreadable", {{"win32_error", read_error}});
    return Load();
  }
  if (read != JsonRead::kOk || !ParseStaged(doc, &staged)) {
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
  const std::string linked =
      FirstLink({StagingRel(staged.version), Join(StagingRel(staged.version), names::kPluginsDir)});
  if (!linked.empty()) {
    Record("blocked", "reparse_point", {{"to", staged.version}, {"folder", linked}});
    return Load();
  }
  for (const StagedFile& file : staged.files) {
    const std::string why = CheckStagedFile(staged, file);
    if (!why.empty()) return Discard(staged, why);
  }
  const DWORD write_error = RuntimeWriteError();
  if (write_error == ERROR_SHARING_VIOLATION) {
    Record("deferred", "runtime_in_use", {{"to", staged.version}, {"win32_error", write_error}});
    return Load();
  }
  if (write_error != 0) {
    // Not "in use": a cause the swap cannot see past (access denied, ...). Nothing moves until it is gone.
    Record("blocked", "runtime_not_writable", {{"to", staged.version}, {"win32_error", write_error}});
    return Load();
  }
  const FileFacts runtime = FactsOf(Join(root_, names::kRuntimeFile));
  if (runtime.version.empty()) {
    // The release in the root has no NEVR-BUILD version to name update\previous\<from>\ by (or there is no runtime at
    // all). ADR 0024 does not say what happens then (an open question on #2105); until it does, nothing is moved.
    Record("blocked", "no_from_version", {{"to", staged.version}});
    return Load();
  }
  if (!names::IsVersionName(runtime.version)) {
    // The NEVR-BUILD field of a file nothing has verified yet would name update\previous\<from>\: a value outside the
    // vocabulary is never used as a path (section 4.2), and never logged.
    Record("blocked", "bad_from_version", {{"to", staged.version}});
    return Load();
  }
  // Ownership (section 4.7): the runtime always; a plugin when state.json lists it with the sha256 of the file in the root.
  const std::vector<names::Member> out = OwnedMembers();
  for (const StagedFile& file : staged.files) {
    if (!file.member.runtime && Exists(InRoot(file.member)) && !HasMember(out, file.member)) {
      Record("blocked", "foreign_file", {{"to", staged.version}, {"path", names::MemberPath(file.member)}});
      return Load();
    }
  }
  return Swap(staged, runtime.version, runtime.sha256, out);
}

// The older generation in update\<folder>\ (previous, failed) goes before a new one moves in (section 4.5). Only names
// in the vocabulary. A version folder or its plugins folder that is a link stops it before anything is deleted, and
// `linked` names it.
bool SwapRun::ClearGenerations(const wchar_t* folder, std::string* linked) {
  const std::wstring previous = Join(update_, folder);
  std::vector<std::wstring> versions;
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW(Join(previous, L"*").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return true;
  do {
    std::string name;
    for (const wchar_t* c = data.cFileName; *c != L'\0'; ++c) name.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && names::IsVersionName(name)) {
      *linked = FirstLink({GenerationRel(folder, name), Join(GenerationRel(folder, name), names::kPluginsDir)});
      if (!linked->empty()) break;
      versions.push_back(Join(previous, data.cFileName));
    }
  } while (FindNextFileW(find, &data) != FALSE);
  FindClose(find);
  if (!linked->empty()) return false;
  for (const std::wstring& dir : versions) {
    if (!Remove(Join(dir, names::kRuntimeFile))) return false;
    const std::wstring plugins = Join(dir, names::kPluginsDir);
    std::vector<std::wstring> files;
    find = FindFirstFileW(Join(plugins, L"*.dll").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
      do {
        std::string name;
        for (const wchar_t* c = data.cFileName; *c != L'\0'; ++c) name.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
        names::Member member;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
            names::ParseMember(std::string(names::kPluginPrefix) + name, &member)) {
          files.push_back(Join(plugins, data.cFileName));
        }
      } while (FindNextFileW(find, &data) != FALSE);
      FindClose(find);
    }
    for (const std::wstring& file : files) {
      if (!Remove(file)) return false;
    }
    if (!RemoveDir(plugins) || !RemoveDir(dir)) return false;
  }
  return true;
}

// Journal, out, in, done (section 4.5). A failed move or journal write is undone to the last whole state.
UpdateResult SwapRun::Swap(const Staged& staged, const std::string& from, const std::string& from_sha256,
                           const std::vector<names::Member>& out) {
  json files = json::array();
  Journal journal;
  journal.from = from;
  journal.to = staged.version;
  journal.phase = "out";
  journal.out = out;
  journal.from_sha256 = from_sha256;
  for (const StagedFile& file : staged.files) {
    journal.in.push_back(file.member);
    journal.in_sha256[names::MemberPath(file.member)] = file.sha256;
    files.push_back({{"path", names::MemberPath(file.member)}, {"sha256", file.sha256}});
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
  if (!WriteJson(UpdateFile(names::kSwapJson), JournalJson(journal))) {
    if (killed_) return Load();
    json fields = identity;
    fields["win32_error"] = last_error_;
    Record("blocked", "journal_write_failed", fields);
    return Load();
  }
  const std::wstring previous = PreviousDir(from);
  CreateDirectoryW(Join(update_, names::kPreviousDir).c_str(), nullptr);
  CreateDirectoryW(previous.c_str(), nullptr);
  for (const names::Member& member : journal.out) {
    MakeDirFor(In(previous, member));
    if (!Move(InRoot(member), In(previous, member))) return failed(false);
  }
  journal.phase = "in";
  if (!WriteJson(UpdateFile(names::kSwapJson), JournalJson(journal))) return failed(false);
  const std::wstring staging = StagingDir(staged.version);
  for (const names::Member& member : journal.in) {
    MakeDirFor(InRoot(member));
    if (!Move(In(staging, member), InRoot(member))) return failed(true);
  }
  journal.phase = "done";
  if (!WriteJson(UpdateFile(names::kSwapJson), JournalJson(journal))) return failed(true);
  const bool finished = FinishDone(journal);
  if (killed_) return Load();
  json fields = identity;
  fields["files"] = files;
  if (!finished) fields["cleanup"] = "incomplete";  // the next start finishes it from the journal
  Record("swapped", nullptr, fields);
  return Trial();
}

// ---- the pending release: rollback (ADR 0024, section 6) -----------------------------------------------------------

void SwapRun::RecordRollback(const char* outcome, const char* reason, json fields) {
  if (killed_) return;
  json record = std::move(fields);
  record["event"] = "update_rollback";
  record["outcome"] = outcome;
  if (reason != nullptr) record["reason"] = reason;
  record["elapsed_ms"] = GetTickCount64() - started_;
  diagnostics_.Emit(record);
}

// One start on a release that is pending says which try it is, or why it is not one (ADR 0024, requirement 4).
void SwapRun::RecordTrial(const char* outcome, const char* reason, const Pending& pending, std::uint64_t boot_attempts,
                          json fields) {
  if (killed_) return;
  json record = std::move(fields);
  record["event"] = "update_trial";
  record["outcome"] = outcome;
  record["reason"] = reason;
  record["release"] = pending.to_version;
  record["boot_attempts"] = boot_attempts;
  record["elapsed_ms"] = GetTickCount64() - started_;
  diagnostics_.Emit(record);
}

// Writes `failed_reason` into the pending record `doc` was read from, unless it already names a failure (the first one
// stays). True when the record names a failure afterwards. `last_error_` holds the error of a write that failed.
bool SwapRun::MarkInRecord(const json& doc, const std::string& reason) {
  if (doc.contains("failed_reason") && doc["failed_reason"].is_string()) return true;
  json marked = doc;
  marked["failed_reason"] = reason;
  return WriteJson(UpdateFile(names::kPendingJson), marked);
}

std::string AsciiName(const wchar_t* name) {
  std::string out;
  for (const wchar_t* c = name; *c != L'\0'; ++c) out.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
  return out;
}

// Ownership (section 4.7): the runtime always; a plugin when state.json lists it with the sha256 of the file in the root.
std::vector<names::Member> SwapRun::OwnedMembers() const {
  std::vector<names::Member> out = {names::Member{true, {}}};
  if (!state_.contains("owned")) return out;
  for (const json& entry : state_["owned"]) {
    names::Member member;
    names::ParseMember(entry["path"].get<std::string>(), &member);
    if (member.runtime || HasMember(out, member)) continue;
    const FileFacts facts = FactsOf(InRoot(member));
    if (facts.present && facts.sha256 == entry["sha256"].get<std::string>()) out.push_back(member);
  }
  return out;
}

// The files of a release kept in a generation folder: its runtime, then its plugins in name order.
std::vector<names::Member> SwapRun::MembersIn(const std::wstring& dir) const {
  std::vector<names::Member> out;
  if (Exists(Join(dir, names::kRuntimeFile))) out.push_back(names::Member{true, {}});
  std::vector<std::string> plugins;
  WIN32_FIND_DATAW data;
  const HANDLE find = FindFirstFileW(Join(Join(dir, names::kPluginsDir), L"*.dll").c_str(), &data);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      names::Member member;
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
          names::ParseMember(std::string(names::kPluginPrefix) + AsciiName(data.cFileName), &member)) {
        plugins.push_back(member.plugin);
      }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
  }
  std::sort(plugins.begin(), plugins.end());
  for (const std::string& plugin : plugins) out.push_back(names::Member{false, plugin});
  return out;
}

// The restored runtime passes the same check as any runtime before it loads (section 6). Empty when it does.
std::string SwapRun::VerifyRuntimeAt(const std::wstring& path) const {
  const HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return "unreadable";
  const nevr_plugin_signature::Verdict verdict = nevr_plugin_signature::VerifyPluginFile(
      handle, HostVerifyPolicy(), static_cast<std::int64_t>(std::time(nullptr)));
  CloseHandle(handle);
  return verdict.accepted() ? std::string() : verdict.Describe();
}

// Section 4.6, "A rollback is never undone: it is completed". Phase out moves the failed release's files to
// update\failed\<failed>\, phase in moves the previous release's files back, phase done finishes. Every step can be
// run again: a file is moved only when it is still where the journal says it was.
bool SwapRun::CompleteRollback(Journal* journal) {
  const std::wstring failed = FailedDir(journal->from);
  const std::wstring previous = PreviousDir(journal->to);
  if (journal->phase == "out") {
    CreateDirectoryW(Join(update_, names::kFailedDir).c_str(), nullptr);
    CreateDirectoryW(failed.c_str(), nullptr);
    for (const names::Member& member : journal->out) {
      if (!Exists(InRoot(member)) || Exists(In(failed, member))) continue;
      MakeDirFor(In(failed, member));
      if (!Move(InRoot(member), In(failed, member))) return false;
    }
    journal->phase = "in";
    if (!WriteJson(UpdateFile(names::kSwapJson), JournalJson(*journal))) return false;
  }
  if (journal->phase == "in") {
    for (const names::Member& member : journal->in) {
      if (!Exists(In(previous, member)) || Exists(InRoot(member))) continue;
      MakeDirFor(InRoot(member));
      if (!Move(In(previous, member), InRoot(member))) return false;
    }
    journal->phase = "done";
    if (!WriteJson(UpdateFile(names::kSwapJson), JournalJson(*journal))) return false;
  }
  return FinishRollback(*journal);
}

// At done: the files that came back are the owned ones, the previous release is current, the failed one is quarantined
// (before pending.json goes, so it is never offered again), then the pending record and the crash marker, the emptied
// generation folder and the journal. Every step can be run again.
bool SwapRun::FinishRollback(const Journal& journal) {
  json owned = json::array();
  std::string runtime_sha256;
  for (const names::Member& member : journal.in) {
    const std::string path = names::MemberPath(member);
    const auto known = journal.in_sha256.find(path);
    std::string sha256;
    if (known != journal.in_sha256.end()) {
      if (Exists(InRoot(member))) sha256 = known->second;  // a file removed since is not owned
    } else {
      const FileFacts facts = FactsOf(InRoot(member));
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
        {{"version", journal.from}, {"runtime_sha256", journal.failed_sha256}, {"reason", journal.reason}, {"time", UtcNow()}});
  }
  if (!WriteJson(UpdateFile(names::kStateJson), state_)) return false;
  if (!Remove(UpdateFile(names::kCrashedFile)) || !Remove(UpdateFile(names::kPendingJson))) return false;
  const std::wstring previous = PreviousDir(journal.to);
  if (!RemoveDir(Join(previous, names::kPluginsDir)) || !RemoveDir(previous)) return false;
  return Remove(UpdateFile(names::kSwapJson));
}

UpdateResult SwapRun::RecoverRollback(const Journal& journal, bool state_readable) {
  json fields = {{"kind", "rollback"}, {"phase", journal.phase}, {"from", journal.from}, {"to", journal.to}};
  const std::string linked = FirstLink({Join(names::kUpdateDir, names::kFailedDir), FailedRel(journal.from),
                                        Join(FailedRel(journal.from), names::kPluginsDir), PreviousRel(journal.to),
                                        Join(PreviousRel(journal.to), names::kPluginsDir)});
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
SwapRun::RollbackEnd SwapRun::DoRollback(const Pending& pending, const std::string& reason, const char* mode,
                                         UpdateResult* stop) {
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
  const std::wstring previous = PreviousDir(restored);
  const std::string linked = FirstLink({Join(names::kUpdateDir, names::kFailedDir), FailedRel(pending.to_version),
                                        Join(FailedRel(pending.to_version), names::kPluginsDir), PreviousRel(restored),
                                        Join(PreviousRel(restored), names::kPluginsDir)});
  if (!linked.empty()) return fail("reparse_point", RollbackEnd::kBlocked, {{"folder", linked}});
  const std::wstring previous_runtime = Join(previous, names::kRuntimeFile);
  if (!Exists(previous_runtime)) return fail("no_previous", RollbackEnd::kStop);
  const std::string verdict = VerifyRuntimeAt(previous_runtime);
  if (!verdict.empty()) return fail("previous_unverified", RollbackEnd::kStop, {{"verify_reason", verdict}});

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
    const FileFacts facts = FactsOf(In(previous, member));
    if (facts.unreadable || facts.sha256.empty()) {
      return fail("previous_unreadable", RollbackEnd::kBlocked, {{"path", names::MemberPath(member)}});
    }
    journal.in_sha256[names::MemberPath(member)] = facts.sha256;
  }
  // A file that is not the failed release's sits where a restored plugin goes: it is not replaced (section 4.7).
  for (const names::Member& member : journal.in) {
    if (!member.runtime && Exists(InRoot(member)) && !HasMember(journal.out, member)) {
      return fail("foreign_file", RollbackEnd::kBlocked, {{"path", names::MemberPath(member)}});
    }
  }
  std::string cleared;
  if (!ClearGenerations(names::kFailedDir, &cleared)) {
    if (killed_) return RollbackEnd::kBlocked;
    if (!cleared.empty()) return fail("reparse_point", RollbackEnd::kBlocked, {{"folder", cleared}});
    return fail("move_failed", RollbackEnd::kBlocked, {{"win32_error", last_error_}});
  }
  if (!WriteJson(UpdateFile(names::kSwapJson), JournalJson(journal))) {
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
    files.push_back({{"path", names::MemberPath(member)}, {"sha256", FactsOf(InRoot(member)).sha256}});
  }
  json fields = common;
  fields["files"] = files;
  RecordRollback("rolled_back", reason.c_str(), fields);
  return RollbackEnd::kDone;
}

// ADR 0024, section 6, R2: what a start does with a release that is pending.
SwapRun::PendingStep SwapRun::DecidePending(UpdateResult* stop) {
  json doc;
  DWORD error = 0;
  const JsonRead read = ReadJsonFile(UpdateFile(names::kPendingJson), &doc, &error);
  if (read == JsonRead::kAbsent) return PendingStep::kNone;
  if (read == JsonRead::kUnreadable) {
    // Not damage: the record is there and something keeps this start from it. Nothing is judged, counted or swapped.
    RecordRollback("deferred", "pending_unreadable", {{"win32_error", error}});
    return PendingStep::kHold;
  }
  Pending pending;
  if (read != JsonRead::kOk || !ParsePending(doc, &pending)) {
    // A record the host did not write: it names nothing the host can put back. The release in the root is verified at
    // every load, so it is treated as started and the record and its marker go.
    const bool removed = Remove(UpdateFile(names::kPendingJson)) && Remove(UpdateFile(names::kCrashedFile));
    if (killed_) return PendingStep::kHold;
    RecordRollback("discarded", "bad_pending", {{"removed", removed}});
    return PendingStep::kNone;
  }
  const FileFacts runtime = FactsOf(Join(root_, names::kRuntimeFile));
  if (runtime.present && !runtime.sha256.empty() && runtime.sha256 != pending.to_sha256) {
    // The root holds a runtime this record is not about: the host moves no file it did not place (section 4.7).
    const bool removed = Remove(UpdateFile(names::kPendingJson)) && Remove(UpdateFile(names::kCrashedFile));
    if (killed_) return PendingStep::kHold;
    RecordRollback("discarded", "pending_stale", {{"from", pending.to_version}, {"removed", removed}});
    return PendingStep::kNone;
  }
  std::string reason = pending.failed_reason;
  if (reason.empty() && Exists(UpdateFile(names::kCrashedFile))) reason = "crashed";
  if (reason.empty() && pending.boot_attempts >= 2) reason = "unconfirmed";
  const DWORD write_error = RuntimeWriteError();
  json common = {{"mode", "next_start"}, {"from", pending.to_version}, {"boot_attempts", pending.boot_attempts}};
  if (pending.has_from) common["to"] = pending.from_version;
  if (!reason.empty()) {
    if (write_error == ERROR_SHARING_VIOLATION) {
      // Another process runs the pending release, perhaps inside its ten seconds: it is not condemned (R2).
      common["cause"] = reason;
      RecordRollback("deferred", "runtime_in_use", common);
      return PendingStep::kTrial;
    }
    if (write_error != 0) {
      common["cause"] = reason;
      common["win32_error"] = write_error;
      RecordRollback("blocked", "runtime_not_writable", common);
      return PendingStep::kTrial;
    }
    const RollbackEnd end = DoRollback(pending, reason, "next_start", stop);
    if (killed_) return PendingStep::kHold;
    if (end == RollbackEnd::kStop) return PendingStep::kStop;
    return end == RollbackEnd::kDone ? PendingStep::kNone : PendingStep::kTrial;
  }
  // One more try. A start beside a running one is not a try (the first one's trial is the trial), and neither is the
  // load that follows a swap whose cleanup this start finished: the swap's own start never got to load. Each says so.
  if (write_error == ERROR_SHARING_VIOLATION) {
    RecordTrial("not_counted", "runtime_in_use", pending, pending.boot_attempts);
  } else if (write_error != 0) {
    RecordTrial("not_counted", "runtime_not_writable", pending, pending.boot_attempts, {{"win32_error", write_error}});
  } else if (finished_swap_) {
    RecordTrial("not_counted", "swap_finished", pending, pending.boot_attempts);
  } else {
    json counted = doc;
    counted["boot_attempts"] = pending.boot_attempts + 1;
    if (WriteJson(UpdateFile(names::kPendingJson), counted)) {
      RecordTrial("counted", "try", pending, pending.boot_attempts + 1);
    } else {
      if (killed_) return PendingStep::kHold;
      RecordRollback("blocked", "pending_write_failed", {{"win32_error", last_error_}});
      RecordTrial("not_counted", "pending_write_failed", pending, pending.boot_attempts, {{"win32_error", last_error_}});
    }
  }
  return PendingStep::kTrial;
}

bool SwapRun::MarkFailed(const char* reason) {
  if (root_.empty() || !IsDirectory(update_) || IsReparsePoint(update_)) return false;
  if (!lock_.Acquire(update_.c_str(), SwapLockTimeoutMs(), SwapLockStepMs())) {
    RecordRollback("failed", "lock_timeout", {{"mode", "next_start"}, {"cause", reason}});
    return false;
  }
  json doc;
  DWORD error = 0;
  Pending pending;
  if (ReadJsonFile(UpdateFile(names::kPendingJson), &doc, &error) != JsonRead::kOk || !ParsePending(doc, &pending)) {
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

UpdateResult SwapRun::RollBackNow(const char* reason) {
  if (root_.empty() || !IsDirectory(update_)) return Stop("no_previous");
  if (IsReparsePoint(update_)) {
    Record("blocked", "reparse_point", {{"folder", "update"}});
    return Stop("reparse_point");
  }
  if (!lock_.Acquire(update_.c_str(), SwapLockTimeoutMs(), SwapLockStepMs())) {
    RecordRollback("failed", "lock_timeout", {{"mode", "in_process"}, {"cause", reason}});
    return Stop("lock_timeout");
  }
  const bool journal = Exists(UpdateFile(names::kSwapJson));
  const std::string linked =
      FirstLink({Join(names::kUpdateDir, names::kStagingDir), Join(names::kUpdateDir, names::kPreviousDir),
                 Join(names::kUpdateDir, names::kFailedDir), names::kPluginsDir});
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
    const UpdateResult recovered = Recover(state == StateRead::kOk);
    if (killed_ || recovered.action == UpdateAction::kStop || Exists(UpdateFile(names::kSwapJson))) return recovered;
  }
  if (state != StateRead::kOk) return Stop("state_unreadable");
  json doc;
  DWORD error = 0;
  const JsonRead read = ReadJsonFile(UpdateFile(names::kPendingJson), &doc, &error);
  if (read == JsonRead::kUnreadable) {
    RecordRollback("failed", "pending_unreadable", {{"mode", "in_process"}, {"win32_error", error}});
    return Stop("pending_unreadable");
  }
  Pending pending;
  if (read != JsonRead::kOk || !ParsePending(doc, &pending)) return Load();  // nothing is pending any more
  const FileFacts runtime = FactsOf(Join(root_, names::kRuntimeFile));
  if (runtime.present && !runtime.sha256.empty() && runtime.sha256 != pending.to_sha256) return Load();  // already put back
  const DWORD write_error = RuntimeWriteError();
  if (write_error != 0) {
    // The failed runtime cannot be moved now; the next start puts it back.
    const bool marked = MarkInRecord(doc, reason);
    json fields = {{"mode", "in_process"}, {"from", pending.to_version}, {"win32_error", write_error}, {"marked", marked}};
    if (!marked) fields["mark_win32_error"] = last_error_;
    RecordRollback("failed", write_error == ERROR_SHARING_VIOLATION ? "runtime_in_use" : "runtime_not_writable", fields);
    return Stop(write_error == ERROR_SHARING_VIOLATION ? "runtime_in_use" : "runtime_not_writable");
  }
  UpdateResult stop;
  const RollbackEnd end = DoRollback(pending, reason, "in_process", &stop);
  if (killed_) return Load();
  if (end == RollbackEnd::kDone) return Load();  // the lock stays held for the load that follows
  // The failed release stays in the root: it is marked, so the next start tries again (the first failure stays).
  const bool marked = MarkInRecord(doc, reason);
  if (!marked) RecordRollback("failed", "pending_write_failed", {{"mode", "in_process"}, {"win32_error", last_error_}});
  if (end == RollbackEnd::kStop) return stop;
  return Stop("rollback_blocked");
}

}  // namespace

UpdateLock::~UpdateLock() { Release(); }

// No wait handle exists for a file another process holds open, so the wait is a bounded poll, as ADR 0024 section 4.1
// specifies: one open every step, for at most the timeout.
bool UpdateLock::Acquire(const wchar_t* update_dir, DWORD timeout_ms, DWORD step_ms) noexcept {
  if (Held()) return true;
  if (update_dir == nullptr || update_dir[0] == L'\0') return false;
  std::wstring path;
  try {
    path = std::wstring(update_dir) + L"\\" + update_names::kLockFile;
  } catch (const std::exception&) {
    return false;
  }
  const ULONGLONG started = GetTickCount64();
  for (;;) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      handle_ = file;
      return true;
    }
    if (GetTickCount64() - started >= timeout_ms) return false;
    Sleep(step_ms == 0 ? 1 : step_ms);
  }
}

void UpdateLock::Release() noexcept {
  if (handle_ == INVALID_HANDLE_VALUE) return;
  CloseHandle(handle_);
  handle_ = INVALID_HANDLE_VALUE;
}

UpdateResult RunUpdateAtStart(const wchar_t* runtime_path, BootstrapDiagnostics& diagnostics,
                                   UpdateLock& lock) noexcept {
  if (runtime_path == nullptr || runtime_path[0] == L'\0') return {};
  try {
    SwapRun start(runtime_path, diagnostics, lock);
    try {
      return start.Run();
    } catch (const std::exception&) {
      return start.AfterException();
    }
  } catch (const std::exception&) {
    // Not even the start could be set up, or its record written: nothing is known about the root, so nothing loads.
    return {UpdateAction::kStop, "internal_error"};
  }
}


UpdateResult RollBackInProcess(const wchar_t* runtime_path, BootstrapDiagnostics& diagnostics, UpdateLock& lock,
                               const char* reason) noexcept {
  if (runtime_path == nullptr || runtime_path[0] == L'\0' || reason == nullptr) return {UpdateAction::kStop, "no_previous", false};
  try {
    SwapRun start(runtime_path, diagnostics, lock);
    try {
      return start.RollBackNow(reason);
    } catch (const std::exception&) {
      return {UpdateAction::kStop, "internal_error", false};
    }
  } catch (const std::exception&) {
    return {UpdateAction::kStop, "internal_error", false};
  }
}

bool MarkPendingFailed(const wchar_t* runtime_path, BootstrapDiagnostics& diagnostics, UpdateLock& lock,
                       const char* reason) noexcept {
  if (runtime_path == nullptr || runtime_path[0] == L'\0' || reason == nullptr) return false;
  bool marked = false;
  try {
    SwapRun start(runtime_path, diagnostics, lock);
    try {
      marked = start.MarkFailed(reason);
    } catch (const std::exception&) {
      marked = false;
    }
  } catch (const std::exception&) {
    marked = false;
  }
  lock.Release();  // the load released it before initialize; this start took it only to write the record
  return marked;
}

}  // namespace nevr_host
