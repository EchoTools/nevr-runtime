#pragma once

// The shim's start of the Quest runtime, the baseline (DESIGN.md of the Quest self-update, piece shim-loads-baseline; ADR 0024
// for the PC counterpart). The shim maps libnevr-runtime.so from the APK's native library folder by file descriptor, resolves
// NEVR_GetRuntimeApi, validates the descriptor it returns before calling through it, and calls `initialize` with a
// NevrQuestHostContext (core/quest_runtime_abi.h). Each start writes one `runtime_load` record (tag NEVR.HOST).
//
// The fence of this unit: it loads ONLY the baseline the APK carries, which the APK signature covers. There is no update
// path, no proof check, no ELF check and no choice between two files here. LoadRuntimeFromFd takes the descriptor to map, so
// a later stage can check the file and then pass its own descriptor without a change to this interface.
//
// No sentinel header is included: the caller (sentinel/entry.cpp) supplies the values and the log sink, so this unit builds and
// is tested on the host.

#include <cstddef>
#include <cstdint>

#include "core/quest_runtime_abi.h"

namespace nevr_quest::runtime_load {

/// The file the baseline is read from: the APK's own copy in its native library folder (the sentinel's build puts it beside
/// libovrplatformloader.so).
inline constexpr char kBaselineFileName[] = "libnevr-runtime.so";

/// The record's level, in the numbering of the sentinel's log and EchoVR::LogLevel (1 debug, 2 info, 4 warning, 8 error).
inline constexpr std::uint32_t kLogLevelDebug = 1;
inline constexpr std::uint32_t kLogLevelInfo = 2;
inline constexpr std::uint32_t kLogLevelWarning = 4;
inline constexpr std::uint32_t kLogLevelError = 8;

/// What the shim hands the runtime. The pointers are valid for the life of the process. The first three are required: without
/// them the runtime cannot say which libr15 it would hook, and the shim refuses to call it (Outcome::kHostContextRefused).
struct HostInputs {
  void* shim_handle = nullptr;            ///< the shim's own dlopen handle
  const void* libr15_base = nullptr;      ///< the load base of the libr15.so the shim attached to
  const char* libr15_build_id = nullptr;  ///< that libr15's patched build ID, lower-case hex
  const char* update_root = nullptr;      ///< the folder that holds update/; null until the update root is formed
  const char* shim_version = nullptr;     ///< the shim's NEVR-BUILD version
  std::uint32_t variant = 0;              ///< the APK's variant word (build_variant::Variant numbering)
  NevrQuestLogFn log = nullptr;           ///< the shim's log sink; null when there is none
};

/// How a start ended. OutcomeName gives the record's `outcome` token.
enum class Outcome : std::uint8_t {
  kLoaded,              ///< initialize returned 0: the runtime runs
  kAbsent,              ///< the baseline file cannot be opened (or the shim cannot name its folder)
  kLoadFailed,          ///< the dynamic linker refused to map the file
  kMissingEntry,        ///< the file has no NEVR_GetRuntimeApi export
  kNullDescriptor,      ///< NEVR_GetRuntimeApi returned null
  kAbiRejected,         ///< the descriptor's header, its size or its initialize field is not one this shim speaks
  kInitFailed,          ///< initialize returned nonzero
  kHostContextRefused,  ///< a required pointer of HostInputs is null: nothing was called
};

/// The record's `outcome` token for `outcome` (lower case, stable).
const char* OutcomeName(Outcome outcome) noexcept;

/// Opens `<dir>/libnevr-runtime.so` read-only and close-on-exec. Returns the descriptor, or -1 with errno set.
int OpenBaselineIn(const char* dir) noexcept;

/// Maps `fd` as the runtime, resolves NEVR_GetRuntimeApi, validates the descriptor, and calls `initialize` once with a context
/// built from `host` (abi major and minor from core/quest_runtime_abi.h, capabilities 0: the staged swap is not wired yet). It
/// takes ownership of `fd` and closes it before it returns; the mapping stays. `file` is the basename the record names.
/// Writes exactly one runtime_load record through host.log (Info when loaded, Error otherwise).
Outcome LoadRuntimeFromFd(int fd, const char* file, const HostInputs& host) noexcept;

/// The baseline start from `dir`: OpenBaselineIn, then LoadRuntimeFromFd. A file that cannot be opened is outcome absent with
/// the errno in its record.
Outcome LoadBaseline(const char* dir, const HostInputs& host) noexcept;

/// The baseline start from the folder that holds the shared object at `in_shim` (the shim's own folder, which is the APK's native
/// library folder once the APK is installed). Sets `shim_handle` from that object; the caller sets the rest of `host`.
Outcome StartBaselineBesideShim(const void* in_shim, HostInputs host) noexcept;

// The version in the NEVR-BUILD literal of the runtime that loaded, read from the file the shim mapped; "" when no runtime
// loaded or the file has no literal. It points at storage the loader keeps for the process.
const char* LoadedRuntimeVersion() noexcept;

/// The folder the shared object containing `in_shim` was loaded from, NUL-terminated into `out`. False when the address is in no
/// loaded object, the object's name has no folder part, or the folder does not fit in `capacity`.
bool NativeLibraryDirOf(const void* in_shim, char* out, std::size_t capacity) noexcept;

/// The dlopen handle of the shared object containing `in_shim`, or null when there is none.
void* ShimHandleOf(const void* in_shim) noexcept;

}  // namespace nevr_quest::runtime_load
