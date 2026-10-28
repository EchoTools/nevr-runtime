#include "runtime/ext/self_provenance.h"

#include <windows.h>

namespace nevr_self_provenance {

namespace {

// A development version is X.Y.(Z+1)-dev.N+sha (cmake/set_project_version_from_git.cmake); only a CI build on a release tag
// carries plain X.Y.Z, and only that build is signed by the release pipeline.
bool IsDevVersion(std::string_view projectVersion) {
  return projectVersion.find("-dev") != std::string_view::npos;
}

}  // namespace

Result Decide(std::string_view projectVersion, bool devBuild, const nevr_plugin_signature::Verdict& verdict) {
  Result result;
  result.version = std::string(projectVersion);
  const bool devVersion = IsDevVersion(projectVersion);
  if (devBuild || devVersion) {
    result.word = "dev";
    if (devVersion && devBuild) {
      result.reason = "dev version and dev build";
    } else if (devVersion) {
      result.reason = "dev version";
    } else {
      result.reason = "dev build";
    }
    return result;
  }
  if (verdict.accepted()) {
    result.word = "signed-release";
    return result;
  }
  result.word = "self-built";
  result.reason = verdict.Describe();
  return result;
}

Result CheckOwnModule(std::string_view projectVersion, bool devBuild, const nevr_plugin_signature::Policy& policy,
                      int64_t nowUnix) {
  // The address of this function names the module it is compiled into: the runtime DLL in a game, the test binary in a test.
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&CheckOwnModule), &self)) {
    nevr_plugin_signature::Verdict verdict;
    verdict.reason = nevr_plugin_signature::Reason::Unreadable;
    verdict.detail = "module handle error=" + std::to_string(GetLastError());
    return Decide(projectVersion, devBuild, verdict);
  }
  wchar_t path[MAX_PATH * 4];
  const DWORD pathLength = GetModuleFileNameW(self, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
  if (pathLength == 0 || pathLength >= sizeof(path) / sizeof(path[0])) {
    nevr_plugin_signature::Verdict verdict;
    verdict.reason = nevr_plugin_signature::Reason::Unreadable;
    verdict.detail = "module path error=" + std::to_string(GetLastError());
    return Decide(projectVersion, devBuild, verdict);
  }
  // FILE_SHARE_READ: the loader holds this file open for reading, and a reader does not take the loader's lock.
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    nevr_plugin_signature::Verdict verdict;
    verdict.reason = nevr_plugin_signature::Reason::Unreadable;
    verdict.detail = "open error=" + std::to_string(GetLastError());
    return Decide(projectVersion, devBuild, verdict);
  }
  const nevr_plugin_signature::Verdict verdict = nevr_plugin_signature::VerifyPluginFile(file, policy, nowUnix);
  CloseHandle(file);
  return Decide(projectVersion, devBuild, verdict);
}

}  // namespace nevr_self_provenance
