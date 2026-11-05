#pragma once

// Where a test finds the process's one main log (core/run_log_path.h) under a %LOCALAPPDATA% folder of its own. The host writes its
// records there, named by the creation time of the process that wrote them: a test that runs the host in-process gets this
// process's name, and a test that runs a child process finds the child's file, the only run log in that scenario's folder.
// There is no other file to look for: the host writes no log beside the game or in %TEMP%.

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/nevr_root.h"
#include "core/run_log_path.h"

namespace nevr_test {

/// Every live main log under `local_app_data`: each `nevr-*.jsonl` in `<local>\nevr\echovr\logs`, oldest name first (a rotation's
/// archive, `nevr-<ts>.<n>.jsonl`, is not a live file). A test that runs several processes finds one file for each.
inline std::vector<std::wstring> HostLogPaths(const std::wstring& local_app_data) {
  const std::wstring logs = nevr_root::LogsDir(nevr_root::RootUnder(local_app_data));
  std::vector<std::wstring> names;
  WIN32_FIND_DATAW found;
  HANDLE find = FindFirstFileW((logs + L"\\nevr-*.jsonl").c_str(), &found);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = found.cFileName;
      const std::size_t dots = std::count(name.begin(), name.end(), L'.');
      // nevr-<date>T<time>.<ms>.jsonl has two dots, nevr-pid<hex>.jsonl (no readable creation time) has one, and an archive,
      // nevr-<date>T<time>.<ms>.<n>.jsonl, has three.
      if (dots == 2 || (dots == 1 && name.compare(0, 8, L"nevr-pid") == 0)) names.push_back(name);
    } while (FindNextFileW(find, &found));
    FindClose(find);
  }
  std::sort(names.begin(), names.end());
  std::vector<std::wstring> paths;
  for (const std::wstring& name : names) paths.push_back(logs + L"\\" + name);
  return paths;
}

/// The main log under `local_app_data`: the newest live file, or, when there is none yet, the path this process would write.
inline std::wstring HostLogPath(const std::wstring& local_app_data) {
  const std::vector<std::wstring> paths = HostLogPaths(local_app_data);
  if (!paths.empty()) return paths.back();
  return nevr_run_log::PathForThisProcess(nevr_root::LogsDir(nevr_root::RootUnder(local_app_data)));
}

}  // namespace nevr_test
