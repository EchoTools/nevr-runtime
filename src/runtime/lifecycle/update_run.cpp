#include "runtime/lifecycle/update_run.h"

#include <windows.h>

#include <cstddef>
#include <utility>

#include "runtime/lifecycle/update_fs_win32.h"

namespace nevr_update {

bool RootToUtf8(const std::wstring& wide, std::string* utf8) {
  utf8->clear();
  if (wide.empty()) return true;
  const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return false;
  utf8->assign(static_cast<std::size_t>(n), '\0');
  const bool ok =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), utf8->data(), n, nullptr, nullptr) == n;
  if (!ok) utf8->clear();
  return ok;
}

std::function<VerifyOutcome(const std::string& utf8_path)> WidenVerifier(
    std::function<VerifyOutcome(const std::wstring& path)> verify) {
  return [verify = std::move(verify)](const std::string& utf8_path) -> VerifyOutcome {
    std::wstring wide;
    if (!Utf8ToWide(utf8_path, &wide)) {
      VerifyOutcome refused;  // not a verdict on the bytes: nothing is discarded for it
      refused.reason = "path_unconvertible";
      return refused;
    }
    return verify ? verify(wide) : VerifyOutcome{};
  };
}

}  // namespace nevr_update
