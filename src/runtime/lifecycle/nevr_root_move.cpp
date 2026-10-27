#include "runtime/lifecycle/nevr_root_move.h"

#include <winsock2.h>  // before windows.h: core/logging.h pulls it in
#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "core/logging.h"
#include "core/nevr_root.h"

namespace nevr_root_move {
namespace {

using Names = nevr_root::Names<wchar_t>;

// One file to move, and the folder it leaves (whose marker records the move).
struct Item {
  std::wstring from;
  std::wstring to;
};

struct Source {
  std::wstring dir;
  std::vector<Item> items;
};

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool IsRegularFile(const WIN32_FIND_DATAW& fd) { return (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0; }

// The names of the regular files in `dir` for which `wanted` is true. A folder that is absent or unreadable has none.
template <typename Predicate>
std::vector<std::wstring> FilesIn(const std::wstring& dir, Predicate wanted) {
  std::vector<std::wstring> names;
  if (dir.empty()) return names;
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) return names;
  do {
    if (IsRegularFile(fd) && wanted(std::wstring(fd.cFileName))) names.emplace_back(fd.cFileName);
  } while (FindNextFileW(find, &fd) != 0);
  FindClose(find);
  return names;
}

bool StartsWith(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }

bool EndsWith(const std::wstring& s, const std::wstring& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// What NEVR keeps in an install's _local folder.
std::vector<const wchar_t*> LocalFileNames() { return {Names::kConfigYaml}; }

std::string ToUtf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return std::string();
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
  return out;
}

std::wstring ParentOf(const std::wstring& path) {
  const std::size_t slash = path.find_last_of(Names::kSeparators);
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// MOVED-TO-NEVR-ROOT.txt in `dir`, naming the root.
bool WriteMarker(const std::wstring& dir, const std::wstring& root, std::wstring* path, unsigned long* error) {
  *path = nevr_root::Join(dir, Names::kMovedMarker);
  const std::string text = "NEVR moved its files out of this folder to " + ToUtf8(root) + "\r\n";
  HANDLE file = CreateFileW(path->c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    *error = GetLastError();
    return false;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
  *error = ok ? 0 : GetLastError();
  CloseHandle(file);
  return ok != FALSE && written == text.size();
}

}  // namespace

bool IsNevrLogFileName(const std::wstring& name) {
  if (StartsWith(name, L"nevr-crash-")) return EndsWith(name, L".txt");
  if (!StartsWith(name, L"nevr-")) return false;
  return EndsWith(name, L".jsonl") || EndsWith(name, L".log");
}

std::vector<Event> Run(const Inputs& inputs) {
  std::vector<Event> events;
  if (inputs.root.empty()) return events;

  std::vector<Source> sources;
  for (const std::wstring& dir : inputs.local_dirs) {
    Source source{dir, {}};
    for (const wchar_t* name : LocalFileNames()) {
      const std::wstring from = nevr_root::Join(dir, name);
      if (Exists(from)) source.items.push_back({from, nevr_root::Join(inputs.root, name)});
    }
    sources.push_back(std::move(source));
  }
  {
    Source logs{inputs.legacy_logs_dir, {}};
    const std::wstring to_dir = nevr_root::LogsDir(inputs.root);
    for (const std::wstring& name : FilesIn(inputs.legacy_logs_dir, IsNevrLogFileName)) {
      logs.items.push_back({nevr_root::Join(inputs.legacy_logs_dir, name.c_str()), nevr_root::Join(to_dir, name.c_str())});
    }
    sources.push_back(std::move(logs));
  }
  {
    Source cosmetics{inputs.legacy_cosmetics_dir, {}};
    const std::wstring to_dir = nevr_root::CosmeticsDir(inputs.root);
    for (const std::wstring& name : FilesIn(inputs.legacy_cosmetics_dir, [](const std::wstring& n) { return n != Names::kMovedMarker; })) {
      cosmetics.items.push_back(
          {nevr_root::Join(inputs.legacy_cosmetics_dir, name.c_str()), nevr_root::Join(to_dir, name.c_str())});
    }
    sources.push_back(std::move(cosmetics));
  }

  std::size_t pending = 0;
  for (const Source& source : sources) pending += source.items.size();
  if (pending == 0) return events;  // the common case after the first run: nothing to do, nothing created

  if (!nevr_root::EnsureDirectoryTree(inputs.root)) {
    events.push_back({Outcome::kRootUnavailable, std::wstring(), inputs.root, GetLastError()});
    return events;
  }

  for (const Source& source : sources) {
    bool moved_any = false;
    for (const Item& item : source.items) {
      if (Exists(item.to)) {
        events.push_back({Outcome::kLeftRootHasIt, item.from, item.to, 0});
        continue;
      }
      if (!nevr_root::EnsureDirectoryTree(ParentOf(item.to)) ||
          !MoveFileExW(item.from.c_str(), item.to.c_str(), MOVEFILE_COPY_ALLOWED)) {
        events.push_back({Outcome::kLeftMoveFailed, item.from, item.to, GetLastError()});
        continue;
      }
      events.push_back({Outcome::kMoved, item.from, item.to, 0});
      moved_any = true;
    }
    if (moved_any) {
      std::wstring marker;
      unsigned long error = 0;
      if (WriteMarker(source.dir, inputs.root, &marker, &error)) {
        events.push_back({Outcome::kMarkerWritten, source.dir, marker, 0});
      } else {
        events.push_back({Outcome::kMarkerFailed, source.dir, marker, error});
      }
    }
  }
  return events;
}

Inputs InputsFromEnvironment() {
  Inputs inputs;
  inputs.root = nevr_root::RootW();

  wchar_t local[MAX_PATH * 2] = {};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(sizeof(local) / sizeof(local[0])));
  if (n != 0 && n < sizeof(local) / sizeof(local[0])) {
    const std::wstring base = nevr_root::Join(std::wstring(local, n), L"EchoVR");
    inputs.legacy_logs_dir = nevr_root::Join(base, L"logs");
    inputs.legacy_cosmetics_dir = nevr_root::Join(base, L"cosmetics\\v1\\packages");
  }

  // The game executable's folder, and the two _local folders above it: the search order config.yaml had.
  wchar_t exe[MAX_PATH * 2] = {};
  const DWORD len = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(sizeof(exe) / sizeof(exe[0])));
  if (len != 0 && len < sizeof(exe) / sizeof(exe[0])) {
    std::wstring dir(exe, len);
    const std::size_t slash = dir.find_last_of(Names::kSeparators);
    if (slash != std::wstring::npos) {
      dir.resize(slash + 1);
      inputs.local_dirs = {dir + L"_local", dir + L"..\\_local", dir + L"..\\..\\_local"};
    }
  }
  return inputs;
}

void RunAndLog() {
  const Inputs inputs = InputsFromEnvironment();
  if (inputs.root.empty()) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.ROOT] LOCALAPPDATA is unset or too long: no nevr root, nothing moved; logs and caches fall back to the game folder or are off");
    return;
  }

  const std::vector<Event> events = Run(inputs);
  unsigned moved = 0;
  unsigned left = 0;
  for (const Event& e : events) {
    const std::string from = ToUtf8(e.from);
    const std::string to = ToUtf8(e.to);
    switch (e.outcome) {
      case Outcome::kMoved:
        ++moved;
        Log(EchoVR::LogLevel::Info, "[NEVR.ROOT] moved %s -> %s", from.c_str(), to.c_str());
        break;
      case Outcome::kLeftRootHasIt:
        ++left;
        Log(EchoVR::LogLevel::Info, "[NEVR.ROOT] left %s: the root already has %s (never overwritten)", from.c_str(), to.c_str());
        break;
      case Outcome::kLeftMoveFailed:
        ++left;
        Log(EchoVR::LogLevel::Warning, "[NEVR.ROOT] left %s: could not move it to %s (error %lu); the next run tries again",
            from.c_str(), to.c_str(), e.error);
        break;
      case Outcome::kMarkerWritten:
        Log(EchoVR::LogLevel::Info, "[NEVR.ROOT] wrote %s", to.c_str());
        break;
      case Outcome::kMarkerFailed:
        Log(EchoVR::LogLevel::Warning, "[NEVR.ROOT] could not write %s (error %lu)", to.c_str(), e.error);
        break;
      case Outcome::kRootUnavailable:
        Log(EchoVR::LogLevel::Warning, "[NEVR.ROOT] could not create %s (error %lu): nothing moved", to.c_str(), e.error);
        break;
    }
  }
  const std::string root = ToUtf8(inputs.root);
  Log(EchoVR::LogLevel::Info, "[NEVR.ROOT] root=%s moved=%u left=%u", root.c_str(), moved, left);
}

}  // namespace nevr_root_move
