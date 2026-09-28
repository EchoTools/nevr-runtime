#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace Nevr::Lifecycle {

using GetSystemDirectoryFunction = UINT(WINAPI*)(LPWSTR, UINT);
using LoadLibraryFunction = HMODULE(WINAPI*)(LPCWSTR);

// Loads dbgcore.dll from the system directory. Injectable API functions keep
// the sizing/failure boundary testable without loading a real module.
HMODULE LoadSystemDbgCore(GetSystemDirectoryFunction getSystemDirectory,
                          LoadLibraryFunction loadLibrary) noexcept;

}  // namespace Nevr::Lifecycle
