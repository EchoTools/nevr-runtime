#include "runtime/lifecycle/system_module_loader.h"

#include <array>
#include <new>
#include <string>

namespace Nevr::Lifecycle {
namespace {
constexpr wchar_t kDbgCoreSuffix[] = L"\\dbgcore.dll";
constexpr size_t kDbgCoreSuffixLength = (sizeof(kDbgCoreSuffix) / sizeof(kDbgCoreSuffix[0])) - 1;
}

HMODULE LoadSystemDbgCore(GetSystemDirectoryFunction getSystemDirectory,
                          LoadLibraryFunction loadLibrary) noexcept {
  if (getSystemDirectory == nullptr || loadLibrary == nullptr) return nullptr;

  std::array<wchar_t, MAX_PATH> systemDirectory{};
  const UINT length = getSystemDirectory(systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
  if (length == 0 || length >= systemDirectory.size()) return nullptr;
  if (systemDirectory[length] != L'\0') return nullptr;

  const size_t capacity = systemDirectory.size();
  if (kDbgCoreSuffixLength >= capacity || static_cast<size_t>(length) > capacity - kDbgCoreSuffixLength - 1) {
    return nullptr;
  }

  try {
    std::wstring path(systemDirectory.data(), static_cast<size_t>(length));
    path.append(kDbgCoreSuffix, kDbgCoreSuffixLength);
    return loadLibrary(path.c_str());
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

}  // namespace Nevr::Lifecycle
