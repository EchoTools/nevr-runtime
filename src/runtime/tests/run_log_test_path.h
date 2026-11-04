#pragma once

// Where a test finds the process's one main log (core/run_log_path.h) under a %LOCALAPPDATA% folder of its own. The host writes its
// records there, named by the creation time of the process that wrote them: a test that runs the host in-process gets this
// process's name, and a test that runs a child process finds the child's file, the only run log in that scenario's folder.
// There is no other file to look for: the host writes no log beside the game or in %TEMP%.

#include <windows.h>

#include <algorithm>
#include <string>

#include "core/nevr_root.h"
#include "core/run_log_path.h"

namespace nevr_test {

/// The main log under `local_app_data`: the newest `nevr-*.jsonl` in `<local>\nevr\echovr\logs` (a rotation's archive,
/// `nevr-<ts>.<n>.jsonl`, is not the live file), or, when there is none yet, the path this process would write.
inline std::wstring HostLogPath(const std::wstring& local_app_data) {
  const std::wstring logs = nevr_root::LogsDir(nevr_root::RootUnder(local_app_data));
  WIN32_FIND_DATAW found;
  HANDLE find = FindFirstFileW((logs + L"\\nevr-*.jsonl").c_str(), &found);
  std::wstring newest;
  if (find != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = found.cFileName;
      const std::size_t dots = std::count(name.begin(), name.end(), L'.');
      if (dots != 2) continue;  // nevr-<date>T<time>.<ms>.jsonl has two dots; an archive has three
      if (name > newest) newest = name;
    } while (FindNextFileW(find, &found));
    FindClose(find);
  }
  if (!newest.empty()) return logs + L"\\" + newest;
  return nevr_run_log::PathForThisProcess(logs);
}

}  // namespace nevr_test
