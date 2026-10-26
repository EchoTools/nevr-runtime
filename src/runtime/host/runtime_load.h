#pragma once

// Host-side versioned C ABI: the runtime load/validate state machine
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md). It loads nevr-runtime.dll from the
// path ResolveRuntimePath returns with LoadLibraryExW (DLL-load-directory +
// System32 search flags), validates the descriptor from core/runtime_abi.h
// (major version, minimum struct size, required entries), and initializes it.
// Every outcome, loaded or not, emits a bootstrap diagnostic; a failure degrades
// to running the original game wrapper without ever calling a null pointer. The
// descriptor is valid only when it carries every required entry (initialize and
// game_main).

#include "core/runtime_abi.h"
#include "runtime/host/bootstrap_diagnostics.h"

#include <windows.h>

#include <cstdint>

namespace nevr_host {

enum class RuntimeLoadOutcome : std::uint32_t {
  kLoaded = 0,
  kNoPath = 1,               // sibling path uncomputable
  kRuntimeAbsent = 2,        // LoadLibraryExW: file/path not found
  kDependencyLoadFailed = 3,  // LoadLibraryExW: a dependency of nevr-runtime.dll is missing
  kLoadFailed = 4,           // LoadLibraryExW: other failure
  kMissingEntry = 5,         // NEVR_GetRuntimeApi absent
  kNullDescriptor = 6,       // get_api() returned null
  kAbiRejected = 7,          // ValidateRuntimeApi rejected (see abi_status)
  kInitFailed = 8,           // initialize() returned nonzero
};

struct RuntimeLoadResult {
  RuntimeLoadOutcome outcome = RuntimeLoadOutcome::kNoPath;
  std::uint32_t win32_error = 0;
  nevr_runtime_abi::Status abi_status = nevr_runtime_abi::Status::kOk;
  std::uint32_t expected_major = nevr_runtime_abi::kMajor;
  std::uint32_t expected_minor = nevr_runtime_abi::kMinor;
  std::uint32_t actual_major = 0;
  std::uint32_t actual_minor = 0;
  std::int32_t init_code = 0;
  const NevrRuntimeApi* api = nullptr;
  HMODULE module = nullptr;
  wchar_t path[MAX_PATH] = {};
  bool path_ok = false;
  // Whether the loading thread owned the process loader lock when the load began (measured
  // by Load, from the PEB). False at the rendezvous; the record says so.
  bool loader_lock_held = false;
  // True when the descriptor came from the host's own image (RuntimeLoader::PrepareInProcess): no
  // file was loaded and `module` is the host.
  bool in_process = false;

  bool Loaded() const { return outcome == RuntimeLoadOutcome::kLoaded; }
  const char* OutcomeName() const;
  const char* DegradedAction() const;
};

/// The one place that decides which file is the runtime: `nevr-runtime.dll` in the
/// directory of `host_module`. Fixed buffer, no heap, no I/O, so it is safe under
/// the loader lock. Returns false when the host path cannot be formed or does not
/// fit `out_size` wide characters.
bool ResolveRuntimePath(HMODULE host_module, wchar_t* out, std::size_t out_size) noexcept;

// Pure decision core: map raw load/validate/init facts to an outcome. No Win32
// calls, no allocation, no locks. Mutation-checked in test_host_runtime_load.
RuntimeLoadResult ClassifyLoad(bool path_ok, const wchar_t* path, DWORD load_error,
                               HMODULE module, bool has_entry, const NevrRuntimeApi* api,
                               nevr_runtime_abi::Status abi_status, std::int32_t init_code);

// Emit a bounded JSON diagnostic describing the result (full path, Win32 error,
// expected/actual ABI version, ABI status, init code, degraded action). Never throws: it runs in an
// InitOnce callback under the game's frames. A record that cannot be built (allocation, a bad
// string) is replaced by a fixed-buffer marker line carrying the outcome and what failed.
void EmitLoadDiagnostic(BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result) noexcept;

#if defined(NEVR_HOST_TESTING)
// Test seam (compiled only into the test targets): make the record builder throw, to reach the marker line.
void SetEmitLoadFailureForTest(bool fail) noexcept;
#endif

class RuntimeLoader {
 public:
  // Resolve the runtime path (ResolveRuntimePath) into a fixed buffer. No allocation, no I/O.
  void Prepare(HMODULE host_module);

  // The runtime is compiled into the host's own image: `get_api` is its descriptor entry point,
  // `module` in the result is `host_module`, and nothing is loaded from disk. The descriptor
  // checks and initialize() are the same as for a file runtime.
  void PrepareInProcess(HMODULE host_module, NevrGetRuntimeApiFn get_api);

  // Run the state machine against the prepared path. Emits a diagnostic on every
  // outcome. The failed runtime is left mapped (it may already have
  // patched something); it is never FreeLibrary'd.
  RuntimeLoadResult Load(const NevrHostContext& host, BootstrapDiagnostics& diagnostics);

  const RuntimeLoadResult& Result() const { return result_; }
  const NevrRuntimeApi* Api() const { return result_.api; }

 private:
  RuntimeLoadResult result_{};
  wchar_t path_[MAX_PATH] = {};
  bool path_ok_ = false;
  HMODULE in_process_module_ = nullptr;
  NevrGetRuntimeApiFn in_process_get_api_ = nullptr;
};

}  // namespace nevr_host
