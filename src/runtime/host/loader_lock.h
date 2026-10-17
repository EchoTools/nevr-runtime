#pragma once

// Whether this thread owns the process loader lock, read from the PEB. The host links no
// runtime code, so it carries its own copy of the check the runtime's DLL-load hook makes
// (runtime/hook/dll_load_hook.cpp, LoaderLockHeldByThisThread). The runtime_load record
// reports it, which is how a log shows the runtime was loaded outside the loader lock.
//
// No allocation, no lock, no call: reads two fields of the PEB.

#include <windows.h>
#include <winternl.h>  // TEB::ProcessEnvironmentBlock

#include <cstddef>
#include <cstdint>

namespace nevr_host {

// PEB.LoaderLock (an RTL_CRITICAL_SECTION*) on x64. winternl.h's PEB names the field Reserved.
inline constexpr std::size_t kPebLoaderLockOffset = 0x110;

inline bool LoaderLockHeldByThisThread() noexcept {
  const auto* peb = reinterpret_cast<const std::uint8_t*>(NtCurrentTeb()->ProcessEnvironmentBlock);
  const auto* loader_lock = *reinterpret_cast<RTL_CRITICAL_SECTION* const*>(peb + kPebLoaderLockOffset);
  if (loader_lock == nullptr) return false;
  // OwningThread holds the owner's thread id (not a handle) while the section is entered, 0 otherwise.
  return reinterpret_cast<std::uintptr_t>(loader_lock->OwningThread) ==
         static_cast<std::uintptr_t>(GetCurrentThreadId());
}

}  // namespace nevr_host
