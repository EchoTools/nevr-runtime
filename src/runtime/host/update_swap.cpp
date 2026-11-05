#include "runtime/host/update_swap.h"

#include "runtime/ext/plugin_signature.h"
#include "runtime/host/file_digest.h"
#include "runtime/host/runtime_load.h"
#include "runtime/host/update_names.h"
#include "runtime/host/update_seams.h"

#include <nlohmann/json.hpp>

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
  std::string sha256;
  std::string version;
};

FileFacts FactsOf(const std::wstring& path) {
  FileFacts facts;
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return facts;
  facts.present = true;
  std::vector<unsigned char> bytes;
  if (ReadWholeFile(file, &bytes)) {
    facts.sha256 = Sha256Hex(bytes);
    facts.version = BuildVersion(bytes);
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

// swap.json: {"kind": "swap", "from", "to", "phase": "out"|"in"|"done", "out": [member path], "in": [member path]}.
struct Journal {
  std::string from;
  std::string to;
  std::string phase;
  std::vector<names::Member> out;
  std::vector<names::Member> in;
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

// This host executes the kind "swap" only; a "rollback" journal is the next step's (#2105).
bool ParseJournal(const json& doc, Journal* out) {
  if (!doc.is_object()) return false;
  for (const char* key : {"kind", "from", "to", "phase"}) {
    if (!doc.contains(key) || !doc[key].is_string()) return false;
  }
  if (doc["kind"].get<std::string>() != "swap") return false;
  out->from = doc["from"].get<std::string>();
  out->to = doc["to"].get<std::string>();
  out->phase = doc["phase"].get<std::string>();
  if (!names::IsVersionName(out->from) || !names::IsVersionName(out->to)) return false;
  if (out->phase != "out" && out->phase != "in" && out->phase != "done") return false;
  return doc.contains("out") && doc.contains("in") && ParseMembers(doc["out"], &out->out) && ParseMembers(doc["in"], &out->in);
}

json JournalJson(const Journal& journal) {
  json out = json::array();
  json in = json::array();
  for (const names::Member& m : journal.out) out.push_back(names::MemberPath(m));
  for (const names::Member& m : journal.in) in.push_back(names::MemberPath(m));
  return {{"kind", "swap"}, {"from", journal.from}, {"to", journal.to}, {"phase", journal.phase}, {"out", out}, {"in", in}};
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

  /// The answer after an exception: stop when a journal says the root may be half moved, else load.
  UpdateResult AfterException() {
    Record("blocked", "internal_error");
    return Exists(UpdateFile(names::kSwapJson)) ? Stop("internal_error") : Load();
  }

 private:
  static UpdateResult Load() { return {UpdateAction::kLoad, nullptr}; }
  static UpdateResult Stop(const char* reason) { return {UpdateAction::kStop, reason}; }

  std::wstring UpdateFile(const wchar_t* name) const { return Join(update_, name); }
  // The folders the swap works in, relative to the root: what a reparse_point record names.
  static std::wstring StagingRel(const std::string& version) {
    return Join(Join(names::kUpdateDir, names::kStagingDir), Widen(version));
  }
  static std::wstring PreviousRel(const std::string& version) {
    return Join(Join(names::kUpdateDir, names::kPreviousDir), Widen(version));
  }
  std::wstring StagingDir(const std::string& version) const { return Join(root_, StagingRel(version)); }
  std::wstring PreviousDir(const std::string& version) const { return Join(root_, PreviousRel(version)); }
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
  bool UndoMoves(const Journal& journal, bool phase_in);
  bool PendingNames(const std::string& version, const std::string& runtime_sha256) const;
  bool StagedNames(const std::string& version) const;
  bool FinishDone(const Journal& journal);
  UpdateResult SwapStaged();
  std::string CheckStagedFile(const Staged& staged, const StagedFile& file) const;
  bool Quarantined(const Staged& staged) const;
  UpdateResult Discard(const Staged& staged, const std::string& why);
  DWORD RuntimeWriteError() const;
  bool ClearPrevious(std::string* linked);
  UpdateResult Swap(const Staged& staged, const std::string& from, const std::vector<names::Member>& out);

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
  if (root_.empty() || !IsDirectory(update_)) return {UpdateAction::kNoUpdateFolder, nullptr};
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
                 names::kPluginsDir});
  if (!linked.empty()) {
    // With a journal the root may be half moved, and its recovery would move files through the link: nothing loads.
    Record("blocked", "reparse_point", {{"folder", linked}, {"journal", journal}});
    return journal ? Stop("reparse_point") : Load();
  }
  RemoveStaleTemporaries();
  if (killed_) return Load();
  if (!journal && !Exists(UpdateFile(names::kStagedJson))) return Load();
  const StateRead state = LoadState();
  if (state == StateRead::kEnded) return Load();
  if (journal) {
    const UpdateResult recovered = Recover(state == StateRead::kOk);
    // A journal still there is a recovery that did not finish: no new swap starts over it.
    if (killed_ || recovered.action == UpdateAction::kStop || Exists(UpdateFile(names::kSwapJson))) return recovered;
  }
  if (state != StateRead::kOk) return Load();  // its record is written: no swap without the ownership list
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
  for (const names::Member& member : journal.in) {
    owned.push_back({{"path", names::MemberPath(member)}, {"sha256", FactsOf(InRoot(member)).sha256}});
  }
  state_["owned"] = owned;
  if (!WriteJson(UpdateFile(names::kStateJson), state_)) return false;
  const FileFacts to = FactsOf(Join(root_, names::kRuntimeFile));
  if (!PendingNames(journal.to, to.sha256)) {
    if (!Remove(UpdateFile(names::kCrashedFile))) return false;
    const FileFacts from = FactsOf(Join(PreviousDir(journal.from), names::kRuntimeFile));
    const json pending = {{"to", {{"version", journal.to}, {"sha256", to.sha256}}},
                          {"from", from.present ? json{{"version", journal.from}, {"sha256", from.sha256}} : json()},
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
bool SwapRun::Quarantined(const Staged& staged) const {
  if (!state_.contains("quarantine") || !state_["quarantine"].is_array()) return false;
  for (const json& entry : state_["quarantine"]) {
    if (entry.is_object() && entry.value("version", json()) == json(staged.version) &&
        entry.value("runtime_sha256", json()) == json(staged.runtime_sha256)) {
      return true;
    }
  }
  return false;
}

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
  std::vector<names::Member> out = {names::Member{true, {}}};
  if (state_.contains("owned")) {
    for (const json& entry : state_["owned"]) {
      names::Member member;
      names::ParseMember(entry["path"].get<std::string>(), &member);
      if (member.runtime || HasMember(out, member)) continue;
      const FileFacts facts = FactsOf(InRoot(member));
      if (facts.present && facts.sha256 == entry["sha256"].get<std::string>()) out.push_back(member);
    }
  }
  for (const StagedFile& file : staged.files) {
    if (!file.member.runtime && Exists(InRoot(file.member)) && !HasMember(out, file.member)) {
      Record("blocked", "foreign_file", {{"to", staged.version}, {"path", names::MemberPath(file.member)}});
      return Load();
    }
  }
  return Swap(staged, runtime.version, out);
}

// The older generation in update\previous\ goes before a new one moves in (section 4.5). Only names in the vocabulary.
// A version folder or its plugins folder that is a link stops it before anything is deleted, and `linked` names it.
bool SwapRun::ClearPrevious(std::string* linked) {
  const std::wstring previous = Join(update_, names::kPreviousDir);
  std::vector<std::wstring> versions;
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW(Join(previous, L"*").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return true;
  do {
    std::string name;
    for (const wchar_t* c = data.cFileName; *c != L'\0'; ++c) name.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && names::IsVersionName(name)) {
      *linked = FirstLink({PreviousRel(name), Join(PreviousRel(name), names::kPluginsDir)});
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
UpdateResult SwapRun::Swap(const Staged& staged, const std::string& from, const std::vector<names::Member>& out) {
  json files = json::array();
  Journal journal;
  journal.from = from;
  journal.to = staged.version;
  journal.phase = "out";
  journal.out = out;
  for (const StagedFile& file : staged.files) {
    journal.in.push_back(file.member);
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
  if (!ClearPrevious(&linked)) {
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
  return Load();
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


}  // namespace nevr_host
