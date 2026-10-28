// config_yaml_source.cpp: see config_yaml_source.h.

#include <winsock2.h>  // before windows.h: core/logging.h pulls it in
#include <windows.h>

#include "runtime/lifecycle/config_yaml_source.h"

#include "abi/echovr.h"
#include "core/logging.h"
#include "core/nevr_root.h"

namespace nevr_config_yaml {
namespace {

std::wstring Wide(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
  return out;
}

std::string Utf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return std::string();
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
  return out;
}

// A regular file opened for reading, or INVALID_HANDLE_VALUE with the Win32 error left set.
HANDLE OpenForReading(const std::wstring& path) {
  return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, nullptr);
}

}  // namespace

Probe RealProbe() {
  Probe probe;
  probe.exists = [](const std::string& path) {
    const std::wstring wide = Wide(path);
    return !wide.empty() && GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
  };
  probe.readable = [](const std::string& path) {
    const std::wstring wide = Wide(path);
    if (wide.empty()) return false;
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    const HANDLE file = OpenForReading(wide);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
  };
  return probe;
}

bool ReadWholeFile(const std::string& path, std::string* bytes, unsigned long* error) {
  const HANDLE file = OpenForReading(Wide(path));
  if (file == INVALID_HANDLE_VALUE) {
    *error = GetLastError();
    return false;
  }
  bytes->clear();
  char buffer[4096];
  bool ok = true;
  for (;;) {
    DWORD got = 0;
    if (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) == FALSE) {
      *error = GetLastError();
      ok = false;
      break;
    }
    if (got == 0) break;
    bytes->append(buffer, got);
  }
  CloseHandle(file);
  return ok;
}

bool CopyIfAbsent(const std::string& from, const std::string& to, unsigned long* error) {
  const std::wstring wide_from = Wide(from);
  const std::wstring wide_to = Wide(to);
  const std::size_t slash = wide_to.find_last_of(nevr_root::Names<wchar_t>::kSeparators);
  if (wide_from.empty() || slash == std::wstring::npos) {
    *error = ERROR_INVALID_NAME;
    return false;
  }
  if (!nevr_root::EnsureDirectoryTree(wide_to.substr(0, slash))) {
    *error = GetLastError();
    return false;
  }
  const BOOL copied = CopyFileW(wide_from.c_str(), wide_to.c_str(), TRUE);
  *error = copied != FALSE ? 0 : GetLastError();
  return copied != FALSE;
}

Choice Resolve(const Inputs& inputs, const Probe& probe) {
  Choice choice = Choose(inputs, probe);
  if (choice.rejection != Rejection::kNone) return choice;

  if (!choice.ignored_legacy.empty()) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.CONFIG] %s is ignored: the nevr root has %s; the old file was left as it is, delete it",
        choice.ignored_legacy.c_str(), choice.root_path.c_str());
  }
  if (!choice.copy_from.empty()) {
    unsigned long error = 0;
    if (CopyIfAbsent(choice.copy_from, choice.root_path, &error)) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.CONFIG] config.yaml copied from %s to %s; the old file was left as it is, delete it",
          choice.copy_from.c_str(), choice.root_path.c_str());
      if (choice.chosen_by == ChosenBy::kNone) {
        choice.path = choice.root_path;
        choice.chosen_by = ChosenBy::kNevrRoot;
      }
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.CONFIG] could not copy config.yaml from %s to %s (error %lu); running without it",
          choice.copy_from.c_str(), choice.root_path.c_str(), error);
    }
  }
  return choice;
}

void AddProcessPaths(const std::string& game_module_dir, Inputs* inputs) {
  inputs->root = Utf8(nevr_root::RootW());
  inputs->legacy = LegacyCandidates(game_module_dir);
}

}  // namespace nevr_config_yaml
