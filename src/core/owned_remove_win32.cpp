// The Win32 side of the owned-removal guard (core/owned_remove.h), the algorithm the guard is specified by. Windows gives no
// openat, so every component is opened by its full final path under the root's real path, and every earlier handle is
// held without FILE_SHARE_DELETE: nothing above an open component can be renamed while the entry is judged. Each
// component is judged twice, by its reparse-point attribute and by its final path. The entry is opened as itself
// (FILE_FLAG_OPEN_REPARSE_POINT) and deleted through that handle; no path is looked up again after the checks.

#include "core/owned_remove.h"

#include <windows.h>

#include <cstddef>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include "core/owned_remove_rules.h"

namespace nevr_owned {
namespace {

// Closes its handle when it leaves scope.
class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE h) : h_(h) {}
  Handle(Handle&& other) noexcept : h_(other.h_) { other.h_ = INVALID_HANDLE_VALUE; }
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      Reset();
      h_ = other.h_;
      other.h_ = INVALID_HANDLE_VALUE;
    }
    return *this;
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() { Reset(); }

  bool valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
  HANDLE get() const { return h_; }

 private:
  void Reset() {
    if (valid()) ::CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }
  HANDLE h_ = INVALID_HANDLE_VALUE;
};

// UTF-8 to UTF-16, strict: an invalid sequence is a failure, not a replacement character.
std::optional<std::wstring> ToWide(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                                           nullptr, 0);
  if (needed <= 0) return std::nullopt;
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                            static_cast<int>(utf8.size()), wide.data(), needed);
  if (written != needed) return std::nullopt;
  return wide;
}

std::optional<std::wstring> FinalPathOf(HANDLE h) {
  const DWORD needed = ::GetFinalPathNameByHandleW(h, nullptr, 0, 0);
  if (needed == 0) return std::nullopt;
  std::wstring path(needed, L'\0');
  const DWORD got = ::GetFinalPathNameByHandleW(h, path.data(), needed, 0);
  if (got == 0 || got >= needed) return std::nullopt;
  path.resize(got);
  return path;
}

bool IsAbsentError(DWORD err) { return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND; }

// root_final, then the first `count` components, joined with '\\'.
std::wstring JoinPath(const std::wstring& root_final, const std::vector<std::wstring>& parts, std::size_t count) {
  std::wstring path(root_final);
  for (std::size_t i = 0; i < count; ++i) {
    if (path.empty() || path.back() != L'\\') path.push_back(L'\\');
    path += parts[i];
  }
  return path;
}

std::vector<std::wstring> Prefix(const std::vector<std::wstring>& parts, std::size_t count) {
  return std::vector<std::wstring>(parts.begin(), parts.begin() + static_cast<std::ptrdiff_t>(count));
}

// Opens the object at root_final\parts[0..count) as itself and judges it. Returns nullopt when it is opened and
// judged fine (`*out` then owns the handle); otherwise the Outcome that ends the entry, with the OS error in `*os_error`.
// `want_directory`: a parent, which must be a folder. `allow_link`: the entry itself, which may be the link to remove.
std::optional<Outcome> OpenChecked(const std::wstring& root_final, const std::vector<std::wstring>& parts, std::size_t count,
                                   DWORD access, bool want_directory, bool allow_link, Handle* out, DWORD* os_error) {
  const std::wstring path = JoinPath(root_final, parts, count);
  Handle h(::CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!h.valid()) {
    *os_error = ::GetLastError();
    return IsAbsentError(*os_error) ? Outcome::kAbsent : Outcome::kFailed;
  }
  FILE_ATTRIBUTE_TAG_INFO tag = {};
  if (!::GetFileInformationByHandleEx(h.get(), FileAttributeTagInfo, &tag, sizeof(tag))) {
    *os_error = ::GetLastError();
    return Outcome::kFailed;
  }
  if (!allow_link && (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    *os_error = 0;
    return Outcome::kRefusedLink;
  }
  if (want_directory && (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    // A file where a recorded folder should be: nothing recorded lies under it.
    *os_error = 0;
    return Outcome::kAbsent;
  }
  const std::optional<std::wstring> final_path = FinalPathOf(h.get());
  if (!final_path) {
    *os_error = ::GetLastError();
    return Outcome::kFailed;
  }
  if (!FinalPathMatches(*final_path, root_final, Prefix(parts, count))) {
    *os_error = 0;
    return Outcome::kRefusedOutsideRoot;
  }
  *out = std::move(h);
  return std::nullopt;
}

Result Make(const std::string& entry, Outcome outcome, int os_error) { return Result{entry, outcome, os_error}; }

Result RemoveEntryChecked(const std::string& root_utf8, const std::string& entry) {
  if (!ValidEntry(entry) || !ValidRoot(root_utf8)) return Make(entry, Outcome::kRefusedName, 0);

  const std::optional<std::wstring> root_w = ToWide(root_utf8);
  if (!root_w) return Make(entry, Outcome::kFailed, static_cast<int>(::GetLastError()));

  // The root is followed (a root that is itself a link is resolved), and held for the whole entry. Windows checks a new
  // open's requested access against each existing handle's share mode, and each existing handle's granted access against
  // the new open's share mode [UNVERIFIED on Windows: the rule is from the documented CreateFile sharing semantics; nothing
  // here runs on Windows]. Our share mode omits FILE_SHARE_DELETE, so another process cannot delete or rename a component
  // while it is held. FILE_LIST_DIRECTORY is a read right on the folder, so it adds the converse: an opener whose share mode
  // omits FILE_SHARE_READ is refused, and the folder's contents are not read or changed under the walk by such an opener.
  // Both rights are requested for the root and for every parent below.
  Handle root(::CreateFileW(root_w->c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!root.valid()) {
    const DWORD err = ::GetLastError();
    return Make(entry, IsAbsentError(err) ? Outcome::kAbsent : Outcome::kFailed, static_cast<int>(err));
  }
  const std::optional<std::wstring> root_final = FinalPathOf(root.get());
  if (!root_final) return Make(entry, Outcome::kFailed, static_cast<int>(::GetLastError()));

  std::vector<std::wstring> parts;
  for (const std::string& part : SplitEntry(entry)) {
    const std::optional<std::wstring> wide = ToWide(part);
    if (!wide) return Make(entry, Outcome::kRefusedName, 0);
    parts.push_back(*wide);
  }

  // Every parent, in order, each held until the entry is done (declared before the entry handle, so it closes after).
  std::vector<Handle> parents;
  parents.reserve(parts.size());
  for (std::size_t i = 1; i < parts.size(); ++i) {
    Handle parent;
    DWORD err = 0;
    const std::optional<Outcome> stop =
        OpenChecked(*root_final, parts, i, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, true, false, &parent, &err);
    if (stop) return Make(entry, *stop, static_cast<int>(err));
    parents.push_back(std::move(parent));
  }

  // The entry itself, opened as itself, judged, and deleted through this handle.
  Handle target;
  DWORD err = 0;
  const std::size_t count = parts.size();
  const std::optional<Outcome> stop = OpenChecked(*root_final, parts, count, DELETE | FILE_READ_ATTRIBUTES, false, true,
                                                  &target, &err);
  if (stop) return Make(entry, *stop, static_cast<int>(err));

  FILE_DISPOSITION_INFO disposition = {};
  disposition.DeleteFile = TRUE;
  if (!::SetFileInformationByHandle(target.get(), FileDispositionInfo, &disposition, sizeof(disposition))) {
    const DWORD set_err = ::GetLastError();
    return Make(entry, set_err == ERROR_DIR_NOT_EMPTY ? Outcome::kNotEmpty : Outcome::kFailed,
                static_cast<int>(set_err));
  }
  return Make(entry, Outcome::kRemoved, 0);
}

}  // namespace

const char* OutcomeName(Outcome outcome) noexcept {
  switch (outcome) {
    case Outcome::kRemoved:
      return "removed";
    case Outcome::kAbsent:
      return "absent";
    case Outcome::kNotEmpty:
      return "not_empty";
    case Outcome::kRefusedName:
      return "refused_name";
    case Outcome::kRefusedLink:
      return "refused_link";
    case Outcome::kRefusedOutsideRoot:
      return "refused_outside_root";
    case Outcome::kFailed:
      return "failed";
  }
  return "unknown";
}

Result RemoveEntry(const std::string& root_utf8, const std::string& entry) noexcept {
  try {
    return RemoveEntryChecked(root_utf8, entry);
  } catch (const std::bad_alloc&) {
    return Result{std::string(), Outcome::kFailed, ERROR_NOT_ENOUGH_MEMORY};
  }
}

std::vector<Result> RemoveOwned(const std::string& root_utf8, const std::vector<std::string>& entries) noexcept {
  std::vector<Result> results;
  try {
    results.reserve(entries.size());
    for (const std::string& entry : entries) results.push_back(RemoveEntry(root_utf8, entry));
  } catch (const std::bad_alloc&) {
    // Fewer results than entries: the entries after the last result were not tried.
  }
  return results;
}

}  // namespace nevr_owned
