#include "runtime/host/update_swap.h"

#include "runtime/ext/plugin_signature.h"
#include "runtime/host/file_digest.h"
#include "runtime/host/runtime_load.h"
#include "runtime/host/update_names.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace nevr_host {
namespace {

namespace names = update_names;
using nlohmann::json;

#if defined(NEVR_HOST_TESTING)
std::atomic<DWORD> g_lock_timeout_for_test{0};
std::atomic<DWORD> g_lock_step_for_test{0};
std::atomic<bool (*)(int)> g_step_hook_for_test{nullptr};
std::atomic<int> g_failing_move_for_test{0};
std::atomic<DWORD> g_failing_move_error_for_test{0};
#endif

DWORD LockTimeoutMs() {
#if defined(NEVR_HOST_TESTING)
  if (const DWORD ms = g_lock_timeout_for_test.load()) return ms;
#endif
  return kUpdateLockTimeoutMs;
}

DWORD LockStepMs() {
#if defined(NEVR_HOST_TESTING)
  if (const DWORD ms = g_lock_step_for_test.load()) return ms;
#endif
  return kUpdateLockStepMs;
}

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

enum class JsonRead { kAbsent, kBad, kOk };

// A JSON file of update\, at most names::kMaxJsonBytes (ADR 0024, section 4.3).
JsonRead ReadJsonFile(const std::wstring& path, json* out) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? JsonRead::kAbsent : JsonRead::kBad;
  }
  LARGE_INTEGER size{};
  std::string text;
  bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 &&
            static_cast<unsigned long long>(size.QuadPart) <= names::kMaxJsonBytes;
  if (ok) {
    text.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    ok = text.empty() || (ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) != FALSE &&
                          read == text.size());
  }
  CloseHandle(file);
  if (!ok) return JsonRead::kBad;
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
    if (m.runtime == member.runtime && m.plugin == member.plugin) return true;
  }
  return false;
}

// staged.json: {"version", "files": [{"path", "size", "sha256"}]}, the spelling of NevrPcStage's result. Every name in
// the vocabulary, no file twice, exactly one runtime.
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
class UpdateStart {
 public:
  UpdateStart(const wchar_t* runtime_path, BootstrapDiagnostics& diagnostics, UpdateLock& lock)
      : diagnostics_(diagnostics), lock_(lock), started_(GetTickCount64()) {
    root_ = runtime_path;
    const std::size_t separator = root_.find_last_of(L"\\/");
    root_ = separator == std::wstring::npos ? std::wstring() : root_.substr(0, separator);
    update_ = Join(root_, names::kUpdateDir);
  }

  UpdateStartResult Run();

  /// The answer after an exception: stop when a journal says the root may be half moved, else load.
  UpdateStartResult AfterException() {
    Record("blocked", "internal_error");
    return Exists(UpdateFile(names::kSwapJson)) ? Stop("internal_error") : Load();
  }

 private:
  static UpdateStartResult Load() { return {UpdateStartAction::kLoad, nullptr}; }
  static UpdateStartResult Stop(const char* reason) { return {UpdateStartAction::kStop, reason}; }

  std::wstring UpdateFile(const wchar_t* name) const { return Join(update_, name); }
  std::wstring StagingDir(const std::string& version) const {
    return Join(Join(update_, names::kStagingDir), Widen(version));
  }
  std::wstring PreviousDir(const std::string& version) const {
    return Join(Join(update_, names::kPreviousDir), Widen(version));
  }
  std::wstring InRoot(const names::Member& member) const { return Join(root_, names::MemberRelativePath(member)); }
  std::wstring In(const std::wstring& dir, const names::Member& member) const {
    return Join(dir, names::MemberRelativePath(member));
  }

  // A step that changed the disk. False when the test seam ended the start here (as a kill would).
  bool Step() {
#if defined(NEVR_HOST_TESTING)
    ++steps_;
    if (const auto hook = g_step_hook_for_test.load()) {
      if (hook(steps_)) killed_ = true;
    }
#endif
    return !killed_;
  }

  bool Move(const std::wstring& from, const std::wstring& to);
  bool Remove(const std::wstring& path);
  bool RemoveDir(const std::wstring& path);
  bool WriteJson(const std::wstring& path, const json& doc);
  void MakeDirFor(const std::wstring& file) { CreateDirectoryW(file.substr(0, file.find_last_of(L'\\')).c_str(), nullptr); }

  void Record(const char* outcome, const char* reason, json fields = json::object());
  bool LoadState();
  json RebuiltState() const;
  UpdateStartResult Recover();
  bool UndoMoves(const Journal& journal, bool phase_in);
  bool FinishDone(const Journal& journal);
  UpdateStartResult SwapStaged();
  std::string CheckStagedFile(const Staged& staged, const StagedFile& file) const;
  UpdateStartResult Discard(const Staged& staged, const std::string& why);
  bool RuntimeInUse(DWORD* error) const;
  bool ClearPrevious();
  UpdateStartResult Swap(const Staged& staged, const std::string& from, const std::vector<names::Member>& out);

  BootstrapDiagnostics& diagnostics_;
  UpdateLock& lock_;
  ULONGLONG started_;
  std::wstring root_;
  std::wstring update_;
  json state_ = json::object();
  int moves_ = 0;
  DWORD last_error_ = 0;
  bool killed_ = false;
#if defined(NEVR_HOST_TESTING)
  int steps_ = 0;
#endif
};

void UpdateStart::Record(const char* outcome, const char* reason, json fields) {
  if (killed_) return;  // a kill writes nothing after the step it ended on
  json record = std::move(fields);
  record["event"] = "update_swap";
  record["outcome"] = outcome;
  if (reason != nullptr) record["reason"] = reason;
  record["elapsed_ms"] = GetTickCount64() - started_;
  diagnostics_.Emit(record);
}

bool UpdateStart::Move(const std::wstring& from, const std::wstring& to) {
  if (killed_) return false;
  ++moves_;
#if defined(NEVR_HOST_TESTING)
  if (moves_ == g_failing_move_for_test.load()) {
    last_error_ = g_failing_move_error_for_test.load();
    return false;
  }
#endif
  // No MOVEFILE_COPY_ALLOWED: a move to another volume fails, and the journal undoes it (ADR 0024, section 4.2).
  // No MOVEFILE_REPLACE_EXISTING: a file where one is going is a failure, never overwritten.
  if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE) {
    last_error_ = GetLastError();
    return false;
  }
  return Step();
}

bool UpdateStart::Remove(const std::wstring& path) {
  if (killed_) return false;
  if (!Exists(path)) return true;
  if (DeleteFileW(path.c_str()) == FALSE) {
    last_error_ = GetLastError();
    return false;
  }
  return Step();
}

bool UpdateStart::RemoveDir(const std::wstring& path) {
  if (killed_) return false;
  if (!IsDirectory(path)) return true;
  if (RemoveDirectoryW(path.c_str()) == FALSE) return true;  // not empty: something that is not ours stays
  return Step();
}

// A temporary file of this process, flushed, then renamed over the file: a reader sees the old file or the new one.
bool UpdateStart::WriteJson(const std::wstring& path, const json& doc) {
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

// What a damaged state.json is replaced by (ADR 0024, section 4.8): the runtime in the root, owned and current.
json UpdateStart::RebuiltState() const {
  json state = {{"owned", json::array()}};
  const FileFacts runtime = FactsOf(Join(root_, names::kRuntimeFile));
  if (!runtime.sha256.empty()) {
    state["owned"].push_back({{"path", names::kRuntimeMember}, {"sha256", runtime.sha256}});
    state["current"] = {{"version", runtime.version}, {"runtime_sha256", runtime.sha256}};
  }
  return state;
}

// False only when the start was ended (the seam).
bool UpdateStart::LoadState() {
  json doc;
  const JsonRead read = ReadJsonFile(UpdateFile(names::kStateJson), &doc);
  if (read == JsonRead::kAbsent) {
    state_ = json::object();
    return true;
  }
  if (read == JsonRead::kOk && ValidState(doc)) {
    state_ = doc;
    return true;
  }
  state_ = RebuiltState();
  const bool written = WriteJson(UpdateFile(names::kStateJson), state_);
  if (killed_) return false;
  json fields = {{"state_written", written}};
  if (!written) fields["win32_error"] = last_error_;
  Record("recovered", "state_rebuilt", fields);
  return true;
}

UpdateStartResult UpdateStart::Run() {
  if (root_.empty() || !IsDirectory(update_)) return {UpdateStartAction::kNoUpdateFolder, nullptr};
  if (!lock_.Acquire(update_.c_str(), LockTimeoutMs(), LockStepMs())) {
    // Another start holds it past the wait. With a journal the root may be half moved: nothing is loaded.
    const bool journal = Exists(UpdateFile(names::kSwapJson));
    Record("blocked", "lock_timeout", {{"journal", journal}});
    return journal ? Stop("lock_timeout") : Load();
  }
  for (const std::wstring& dir : {update_, Join(update_, names::kStagingDir), Join(update_, names::kPreviousDir),
                                  Join(root_, names::kPluginsDir)}) {
    if (IsReparsePoint(dir)) {
      Record("blocked", "reparse_point");
      return Load();
    }
  }
  const bool journal = Exists(UpdateFile(names::kSwapJson));
  if (!journal && !Exists(UpdateFile(names::kStagedJson))) return Load();
  if (!LoadState()) return Load();
  if (journal) {
    const UpdateStartResult recovered = Recover();
    // A journal still there is a recovery that did not finish: no new swap starts over it.
    if (killed_ || recovered.action == UpdateStartAction::kStop || Exists(UpdateFile(names::kSwapJson))) return recovered;
  }
  if (Exists(UpdateFile(names::kStagedJson))) return SwapStaged();
  return Load();
}

// ADR 0024, section 4.6: out or in are undone to the last whole state; done finishes its cleanup.
UpdateStartResult UpdateStart::Recover() {
  json doc;
  Journal journal;
  if (ReadJsonFile(UpdateFile(names::kSwapJson), &doc) != JsonRead::kOk || !ParseJournal(doc, &journal)) {
    Record("blocked", "bad_journal");
    return Stop("bad_journal");
  }
  const bool whole = journal.phase == "done" ? FinishDone(journal) : UndoMoves(journal, journal.phase == "in");
  if (killed_) return Load();
  json fields = {{"kind", "swap"}, {"phase", journal.phase}, {"from", journal.from}, {"to", journal.to}};
  if (!whole) {
    fields["win32_error"] = last_error_;
    Record("blocked", "move_failed", fields);
    return journal.phase == "done" ? Load() : Stop("move_failed");
  }
  Record("recovered", nullptr, fields);
  return Load();
}

// Files that moved in go back to staging, then files that moved out go back to the root; the journal goes last.
bool UpdateStart::UndoMoves(const Journal& journal, bool phase_in) {
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

// At done: the files the swap placed are owned, the release is pending, and staged.json, the staging folder, a stale
// crash marker and the journal go, in that order (ADR 0024, section 4.5). Every step can be run again.
bool UpdateStart::FinishDone(const Journal& journal) {
  json owned = json::array();
  for (const names::Member& member : journal.in) {
    owned.push_back({{"path", names::MemberPath(member)}, {"sha256", FactsOf(InRoot(member)).sha256}});
  }
  state_["owned"] = owned;
  if (!WriteJson(UpdateFile(names::kStateJson), state_)) return false;
  const FileFacts to = FactsOf(Join(root_, names::kRuntimeFile));
  const FileFacts from = FactsOf(Join(PreviousDir(journal.from), names::kRuntimeFile));
  const json pending = {{"to", {{"version", journal.to}, {"sha256", to.sha256}}},
                        {"from", from.present ? json{{"version", journal.from}, {"sha256", from.sha256}} : json()},
                        {"boot_attempts", 1},
                        {"failed_reason", nullptr}};
  if (!WriteJson(UpdateFile(names::kPendingJson), pending)) return false;
  if (!Remove(UpdateFile(names::kStagedJson))) return false;
  const std::wstring staging = StagingDir(journal.to);
  if (!RemoveDir(Join(staging, names::kPluginsDir)) || !RemoveDir(staging)) return false;
  if (!Remove(UpdateFile(names::kCrashedFile))) return false;
  return Remove(UpdateFile(names::kSwapJson));
}

// ADR 0024, section 4.3: the file as it is on disk now, against staged.json and the verifier. Empty when it passes,
// else the reason: "unreadable", "size", "sha256", or the verifier's own words.
std::string UpdateStart::CheckStagedFile(const Staged& staged, const StagedFile& file) const {
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

// A failed check removes staged.json and counts the release's discard; the third quarantines it (section 4.3).
UpdateStartResult UpdateStart::Discard(const Staged& staged, const std::string& why) {
  if (!Remove(UpdateFile(names::kStagedJson))) {
    if (killed_) return Load();
  }
  if (!state_.contains("discards") || !state_["discards"].is_object()) state_["discards"] = json::object();
  const std::uint64_t discards = state_["discards"].value(staged.version, std::uint64_t{0}) + 1;
  state_["discards"][staged.version] = discards;
  if (discards >= kDiscardsToQuarantine) {
    if (!state_.contains("quarantine") || !state_["quarantine"].is_array()) state_["quarantine"] = json::array();
    state_["quarantine"].push_back(
        {{"version", staged.version}, {"runtime_sha256", staged.runtime_sha256}, {"reason", why}, {"time", UtcNow()}});
  }
  const bool written = WriteJson(UpdateFile(names::kStateJson), state_);
  if (killed_) return Load();
  json fields = {{"to", staged.version}, {"discards", discards}, {"quarantined", discards >= kDiscardsToQuarantine}};
  if (!written) fields["state_written"] = false;
  Record("discarded", why.c_str(), fields);
  return Load();
}

// ADR 0024, section 4.4: a runtime mapped as an image by another process refuses a write-open.
bool UpdateStart::RuntimeInUse(DWORD* error) const {
  const std::wstring path = Join(root_, names::kRuntimeFile);
  const HANDLE file =
      CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    CloseHandle(file);
    return false;
  }
  *error = GetLastError();
  return *error != ERROR_FILE_NOT_FOUND && *error != ERROR_PATH_NOT_FOUND;
}

UpdateStartResult UpdateStart::SwapStaged() {
  json doc;
  Staged staged;
  if (ReadJsonFile(UpdateFile(names::kStagedJson), &doc) != JsonRead::kOk || !ParseStaged(doc, &staged)) {
    // A name outside the vocabulary: the file is not used (section 4.2), and not counted against any release.
    Remove(UpdateFile(names::kStagedJson));
    if (killed_) return Load();
    Record("discarded", "bad_staged");
    return Load();
  }
  for (const StagedFile& file : staged.files) {
    const std::string why = CheckStagedFile(staged, file);
    if (!why.empty()) return Discard(staged, why);
  }
  DWORD in_use_error = 0;
  if (RuntimeInUse(&in_use_error)) {
    Record("deferred", "runtime_in_use", {{"to", staged.version}, {"win32_error", in_use_error}});
    return Load();
  }
  const FileFacts runtime = FactsOf(Join(root_, names::kRuntimeFile));
  if (runtime.version.empty()) {
    // The release in the root has no NEVR-BUILD version to name update\previous\<from>\ by (or there is no runtime at
    // all). ADR 0024 does not say what happens then (an open question on #2105); until it does, nothing is moved.
    Record("blocked", "no_from_version", {{"to", staged.version}});
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
bool UpdateStart::ClearPrevious() {
  const std::wstring previous = Join(update_, names::kPreviousDir);
  std::vector<std::wstring> versions;
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW(Join(previous, L"*").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return true;
  do {
    std::string name;
    for (const wchar_t* c = data.cFileName; *c != L'\0'; ++c) name.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && names::IsVersionName(name)) {
      versions.push_back(Join(previous, data.cFileName));
    }
  } while (FindNextFileW(find, &data) != FALSE);
  FindClose(find);
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
UpdateStartResult UpdateStart::Swap(const Staged& staged, const std::string& from, const std::vector<names::Member>& out) {
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
  if (!ClearPrevious()) return failed(false);
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

UpdateStartResult RunUpdateAtStart(const wchar_t* runtime_path, BootstrapDiagnostics& diagnostics,
                                   UpdateLock& lock) noexcept {
  if (runtime_path == nullptr || runtime_path[0] == L'\0') return {};
  try {
    UpdateStart start(runtime_path, diagnostics, lock);
    try {
      return start.Run();
    } catch (const std::exception&) {
      return start.AfterException();
    }
  } catch (const std::exception&) {
    // Not even the record could be built: load only what a journal does not call into doubt.
    return {UpdateStartAction::kStop, "internal_error"};
  }
}

#if defined(NEVR_HOST_TESTING)
void SetUpdateLockTimingForTest(DWORD timeout_ms, DWORD step_ms) noexcept {
  g_lock_timeout_for_test.store(timeout_ms);
  g_lock_step_for_test.store(step_ms);
}
void SetUpdateStepHookForTest(bool (*hook)(int step)) noexcept { g_step_hook_for_test.store(hook); }
void SetUpdateMoveFailureForTest(int move_number, DWORD error) noexcept {
  g_failing_move_for_test.store(move_number);
  g_failing_move_error_for_test.store(error);
}
#endif

}  // namespace nevr_host
