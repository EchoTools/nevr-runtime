#pragma once

// Host-side versioned C ABI: the runtime load/validate state machine
// (docs/design/2026-10-05-bugsplat-nevr-split.md step 3, "nevr.dll: changing
// runtime"). It loads nevr.dll as the sibling of the recorded BugSplat64.dll
// module path with LoadLibraryExW (DLL-load-directory + System32 search flags),
// validates the descriptor from core/runtime_abi.h (major version, minimum
// struct size, required entries), and initializes it. Every failure emits a
// bootstrap diagnostic and degrades to running the original game function
// without ever calling a null pointer.

#include "core/runtime_abi.h"
#include "runtime/host/bootstrap_diagnostics.h"

#include <windows.h>

#include <cstdint>

namespace NevrHost {

enum class RuntimeLoadOutcome : std::uint32_t {
  kLoaded = 0,
  kNoPath = 1,               // sibling path uncomputable
  kRuntimeAbsent = 2,        // LoadLibraryExW: file/path not found
  kDependencyLoadFailed = 3,  // LoadLibraryExW: a dependency of nevr.dll is missing
  kLoadFailed = 4,           // LoadLibraryExW: other failure
  kMissingEntry = 5,         // NEVR_GetRuntimeApi absent
  kNullDescriptor = 6,       // get_api() returned null
  kAbiRejected = 7,          // ValidateRuntimeApi rejected (see abi_status)
  kInitFailed = 8,           // initialize() returned nonzero
};

struct RuntimeLoadResult {
  RuntimeLoadOutcome outcome = RuntimeLoadOutcome::kNoPath;
  std::uint32_t win32_error = 0;
  NevrAbi::Status abi_status = NevrAbi::Status::kOk;
  std::uint32_t expected_major = NevrAbi::kMajor;
  std::uint32_t expected_minor = NevrAbi::kMinor;
  std::uint32_t actual_major = 0;
  std::uint32_t actual_minor = 0;
  std::int32_t init_code = 0;
  const NevrRuntimeApi* api = nullptr;
  HMODULE module = nullptr;
  wchar_t path[MAX_PATH] = {};
  bool path_ok = false;

  bool Loaded() const { return outcome == RuntimeLoadOutcome::kLoaded; }
  const char* OutcomeName() const;
  const char* DegradedAction() const;
};

// Pure decision core: map raw load/validate/init facts to an outcome. No Win32
// calls, no allocation, no locks. Mutation-checked in test_host_runtime_load.
RuntimeLoadResult ClassifyLoad(bool path_ok, const wchar_t* path, DWORD load_error,
                               HMODULE module, bool has_entry, const NevrRuntimeApi* api,
                               NevrAbi::Status abi_status, std::int32_t init_code);

// Emit a bounded JSON diagnostic describing a non-loaded result (full path,
// Win32 error, expected/actual ABI version, degraded action).
void EmitLoadDiagnostic(BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result);

class RuntimeLoader {
 public:
  // Compute <shim dir>\nevr.dll into a fixed buffer. No allocation, no I/O.
  void Prepare(HMODULE shim_module);

  // Run the state machine against the prepared path. Emits a diagnostic on every
  // non-loaded outcome. The failed runtime is left mapped (it may already have
  // patched something); it is never FreeLibrary'd.
  RuntimeLoadResult Load(const NevrHostContext& host, BootstrapDiagnostics& diagnostics);

  const RuntimeLoadResult& Result() const { return result_; }
  const NevrRuntimeApi* Api() const { return result_.api; }

 private:
  RuntimeLoadResult result_{};
  wchar_t path_[MAX_PATH] = {};
  bool path_ok_ = false;
};

}  // namespace NevrHost
