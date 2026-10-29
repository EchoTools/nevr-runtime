#pragma once

// Host-side versioned C ABI: the runtime load/validate state machine
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md). It loads nevr-runtime.dll from the
// path ResolveRuntimePath returns with LoadLibraryExW (DLL-load-directory +
// System32 search flags), validates the descriptor from core/runtime_abi.h
// (major version, minimum struct size, required entries), and initializes it.
// Every outcome, loaded or not, emits a bootstrap diagnostic; a failure stops the
// game (docs/adr/0012-static-install-root.md: the game does not run without NEVR),
// without ever calling a null pointer. The descriptor is valid only when it carries
// every required entry (initialize and game_main).

#include "core/runtime_abi.h"
#include "runtime/host/bootstrap_diagnostics.h"
#include "runtime/ext/plugin_signature.h"

#include <windows.h>

#include <cstdint>
#include <climits>
#include <string>

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
  kRuntimeRefused = 9,       // the verifier refused nevr-runtime.dll, or it could not be read: never loaded (#854)
};

/// Where the root of the runtime came from (docs/adr/0012-static-install-root.md, "Where state resolves").
enum class RuntimeRootSource : std::uint8_t {
  kUnresolved,    ///< no root: neither %LOCALAPPDATA% nor an honoured NEVR_ROOT gave one that fits
  kLocalAppData,  ///< %LOCALAPPDATA%\nevr\echovr (nevr_root::Names<wchar_t>::kRoot)
  kOverride,      ///< NEVR_ROOT, honoured only in the plugin developers' build
};

/// The word the runtime_load record carries for a root source.
const char* RuntimeRootSourceName(RuntimeRootSource source) noexcept;

struct RuntimePathChoice {
  RuntimeRootSource source = RuntimeRootSource::kUnresolved;
  /// NEVR_ROOT was set and not honoured (a release build, or a value that is not an absolute path). The record says so.
  bool override_ignored = false;
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
  // True when initialize() threw a std::exception (init_code is then kInitThrewCode). The runtime catches its
  // own exceptions; one that crosses the boundary is a load failure, not a crash in the host's detour.
  bool init_threw = false;
  const NevrRuntimeApi* api = nullptr;
  HMODULE module = nullptr;
  wchar_t path[MAX_PATH] = {};
  bool path_ok = false;
  // Where the root of `path` came from, and whether a NEVR_ROOT that was set was ignored (a release build).
  RuntimeRootSource root_source = RuntimeRootSource::kUnresolved;
  bool root_override_ignored = false;
  // Whether the loading thread owned the process loader lock when the load began (measured
  // by Load, from the PEB). False at the rendezvous; the record says so.
  bool loader_lock_held = false;
  // The verdict of the signature check on nevr-runtime.dll (docs/adr/0012-static-install-root.md, "The host verifies
  // nevr-runtime.dll before it loads it"): "accepted", "unsigned_allowed" (a dev build loads an unsigned file) or
  // "refused". Null when no file was checked (the file is absent).
  const char* verify = nullptr;
  // What the verifier said, in its own words (nevr_plugin_signature::Verdict::Describe), or "unreadable".
  std::string verify_reason;
  // The version in the NEVR-BUILD literal of nevr-runtime.dll, when the host read the file (it did not for an absent or
  // a refused one): empty otherwise. The stop line states it, or "not loaded" (stop_line.h).
  std::string runtime_version;
  // The same verdict as a code, for the words the player reads (stop_notice.h). Meaningful when `verify` is "refused"
  // or "unsigned_allowed"; Reason::Unreadable for a file that could not be opened for reading.
  nevr_plugin_signature::Reason verify_reason_code = nevr_plugin_signature::Reason::Accepted;
  // The CERT_CHAIN_ENGINE_CONFIG cbSize the verifier made its chain engine with: 88, or 80 on a Wine before 11.5 (#1262).
  // 0 when the file was not accepted by a check.
  std::uint32_t verify_engine_cbsize = 0;

  bool Loaded() const { return outcome == RuntimeLoadOutcome::kLoaded; }
  const char* OutcomeName() const;
  const char* DegradedAction() const;
};

/// What the host found when it looked for nevr-runtime.dll at the path ResolveRuntimePath returned.
enum class RuntimeFile : std::uint8_t {
  kAbsent,      ///< no file there (not found, or no such directory)
  kUnreadable,  ///< a file is there and could not be opened for reading
  kReadable,    ///< opened for reading, with FILE_SHARE_READ, and held
};

/// The decision on one file, from the facts alone (docs/adr/0012-static-install-root.md, "The host verifies
/// nevr-runtime.dll before it loads it"). No Win32 calls, no allocation: the table in test_host_runtime_verify.
struct VerifyDecision {
  bool refused = false;             ///< the game stops, and the file is never mapped
  bool may_load = false;            ///< the host maps the file next
  const char* verify = nullptr;     ///< the word in the runtime_load record; null when no file was checked
  bool reason_is_verdict = false;   ///< verify_reason is the verifier's own words (otherwise "unreadable", or none)
};

/// `verify_enabled` is false only in a test target's harness, which loads unsigned fixture runtimes. `accepted` is the
/// verifier's verdict and is read only for a readable file with the check enabled. `dev_build` is the plugin
/// developers' build (nevr_plugin_pins::IsDevBuild): it loads a file the verifier did not accept, and says so, but never
/// a file the host could not read.
VerifyDecision DecideVerify(RuntimeFile file, bool verify_enabled, bool accepted, bool dev_build) noexcept;

/// The exit code of a game stopped because nevr-runtime.dll did not load: 78, EX_CONFIG in sysexits
/// (docs/adr/0012-static-install-root.md, "Names this record fixes").
inline constexpr std::uint32_t kRuntimeStopExitCode = 78;

/// The init_code recorded when initialize() threw (INT32_MIN: not a value a runtime returns on purpose).
inline constexpr std::int32_t kInitThrewCode = INT32_MIN;

/// True when a file named nevr-runtime.dll is in `directory` (with or without a trailing backslash): the old layout, which
/// kept the runtime beside BugSplat64.dll and which the host no longer loads. One file-attribute call, fixed buffer, no heap.
bool RuntimeFileBeside(const wchar_t* directory) noexcept;

/// The one place that decides which file is the runtime: `nevr-runtime.dll` in the nevr root of the Windows user,
/// `%LOCALAPPDATA%\nevr\echovr`, never beside `BugSplat64.dll`. The inputs are the two environment values (null or
/// empty when unset), so a test supplies its own. `nevr_root_override` (NEVR_ROOT, an absolute path) replaces the root
/// when `dev_build` is true and is ignored otherwise, with `override_ignored` set. Fixed buffer, no heap, no I/O, so it is
/// safe under the loader lock. The result's `source` is kUnresolved, with `out` emptied, when no root was formed or the
/// path does not fit `out_size` wide characters.
RuntimePathChoice ResolveRuntimePathFrom(const wchar_t* local_app_data, const wchar_t* nevr_root_override,
                                         bool dev_build, wchar_t* out, std::size_t out_size) noexcept;

/// ResolveRuntimePathFrom over this process's %LOCALAPPDATA% and NEVR_ROOT, with the build's own dev flag.
RuntimePathChoice ResolveRuntimePath(wchar_t* out, std::size_t out_size) noexcept;

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

/// Emit the record that says the game is being stopped, with the cause and the exit code, before the host ends the
/// process. Same guarantee as EmitLoadDiagnostic: never throws, and a record that cannot be built is replaced by a
/// fixed-buffer marker line.
/// `notice`, when not null, names what the player is told (stop_notice.h, StopNoticeName) and goes in the record.
void EmitStopDiagnostic(BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result,
                        const char* notice = nullptr) noexcept;

#if defined(NEVR_HOST_TESTING)
// Test seam (compiled only into the test targets): make the record builder throw, to reach the marker line.
void SetEmitLoadFailureForTest(bool fail) noexcept;
// Test seam: whether RuntimeLoader::Load verifies nevr-runtime.dll. Off by default in the test targets, whose harnesses
// load unsigned fixture runtimes; the gate's own tests turn it on. Production always verifies.
void SetVerifyRuntimeForTest(bool enabled) noexcept;
// Test seam: the policy the verdict is taken against, instead of nevr_plugin_pins::PinnedPolicy() (the pinned Microsoft
// root), so a harness can verify files signed by a throwaway PKI. Null: the pins. The policy must outlive the loads.
void SetVerifyPolicyForTest(const nevr_plugin_signature::Policy* policy) noexcept;
// Test seam: -1 the build's own (nevr_plugin_pins::IsDevBuild), 0 a release build, 1 the plugin developers' build.
void SetDevBuildForTest(int mode) noexcept;
// Test seam: called once with the path just before the load, while the verified file is still held.
void SetBeforeLoadHookForTest(void (*hook)(const wchar_t* path)) noexcept;
// Test seam: the folder the runtime is looked for in, instead of the nevr root. Null: the production rule. The string
// must outlive the loads; a harness that stages a runtime beside its host stand-in passes that folder.
void SetRuntimeDirForTest(const wchar_t* directory) noexcept;
#endif

class RuntimeLoader {
 public:
  // Resolve the runtime path (ResolveRuntimePath) into a fixed buffer. No allocation, no I/O.
  void Prepare();

  // Run the state machine against the prepared path. Emits a diagnostic on every
  // outcome. The failed runtime is left mapped (it may already have
  // patched something); it is never FreeLibrary'd.
  RuntimeLoadResult Load(const NevrHostContext& host, BootstrapDiagnostics& diagnostics);

  /// Call api->initialize(&host). Returns its result; an exception of any type that escapes it is caught here
  /// (exceptions must not cross the DLL boundary), sets *threw and returns kInitThrewCode.
  static std::int32_t CallInitialize(const NevrRuntimeApi* api, const NevrHostContext& host, bool* threw) noexcept;

  const RuntimeLoadResult& Result() const { return result_; }
  const NevrRuntimeApi* Api() const { return result_.api; }

 private:
  RuntimeLoadResult result_{};
  wchar_t path_[MAX_PATH] = {};
  bool path_ok_ = false;
  RuntimePathChoice choice_{};
};

}  // namespace nevr_host
