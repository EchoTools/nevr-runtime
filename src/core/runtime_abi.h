#pragma once

// Host <-> runtime C ABI between BugSplat64.dll and nevr.dll
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md).
//
// BugSplat64.dll (the stable host the game statically imports) loads nevr.dll at
// the entry of the game's main wrapper (the one rendezvous), resolves
// NEVR_GetRuntimeApi, and validates the descriptor it returns BEFORE calling
// anything through it. The runtime validates the host context it
// is handed the same way. Only C types cross this boundary: no C++ classes, no
// STL, no exceptions, no allocator-owned objects.
//
// Versioning:
//   - abi_major: a mismatch is a hard reject in either direction.
//   - abi_minor: informational. Newer minors may only APPEND fields; a reader
//     uses an appended field only after nevr_runtime_abi::DescriptorCovers() proves the
//     writer's struct_size includes it.
//   - struct_size: the writer's sizeof. It is read before anything past the
//     16-byte header, so a smaller (older) descriptor is never over-read.
//
// Header-only and stateless on purpose: both DLLs compile it and neither owns
// state through it, so the host and the runtime never hold two copies of one
// piece of state.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

extern "C" {

/// echovr.exe GameMainWrapper (0x1400CD510), `INT64 (INT64 command_line)` in the x64 Microsoft
/// ABI: the game passes the GetCommandLineA pointer and returns the result to its CRT as the
/// process exit code. Also the type of the runtime's `game_main`.
typedef std::int64_t (*NevrGameMainFn)(std::int64_t command_line);

/// The first 16 bytes of every descriptor crossing the host/runtime boundary.
struct NevrAbiHeader {
  std::uint32_t struct_size;
  std::uint32_t abi_major;
  std::uint32_t abi_minor;
  std::uint32_t reserved;
};

/// Handed by the host to NevrRuntimeApi::initialize.
struct NevrHostContext {
  NevrAbiHeader header;
  HMODULE host_module;
  HMODULE game_module;
  /// The MinHook trampoline for the host-owned GameMainWrapper detour. The runtime
  /// calls the original wrapper through this and never installs a second hook on the
  /// wrapper's address.
  NevrGameMainFn original_game_main_wrapper;
  /// The address the host hooked, and the first 16 bytes found there after the host's
  /// MH_EnableHook (the host's jump). The runtime registers the host's hook with
  /// HookGuard from this record instead of hooking the wrapper itself.
  const void* wrapper_hook_target;
  unsigned char wrapper_hook_bytes[16];
};

/// Returned by nevr.dll's NEVR_GetRuntimeApi export. All entries are required in v1.
struct NevrRuntimeApi {
  NevrAbiHeader header;
  /// Returns 0 on success. Called once, after the descriptor validated, at the
  /// wrapper's entry.
  std::int32_t (*initialize)(const NevrHostContext* host);
  /// Runs the game's main under the runtime's recovery, in place of the original
  /// wrapper. Called by the host detour with the wrapper's argument; its result is
  /// the wrapper's result.
  std::int64_t (*game_main)(std::int64_t command_line);
};

/// Signature of nevr.dll's single versioned entry point.
typedef const NevrRuntimeApi* (*NevrGetRuntimeApiFn)();

}  // extern "C"

namespace nevr_runtime_abi {

inline constexpr char kGetRuntimeApiExport[] = "NEVR_GetRuntimeApi";
inline constexpr std::uint32_t kMajor = 1;
inline constexpr std::uint32_t kMinor = 0;

/// Smallest struct_size a v1 writer may report: every v1 field present.
inline constexpr std::uint32_t kRuntimeApiV1Size =
    static_cast<std::uint32_t>(offsetof(NevrRuntimeApi, game_main) + sizeof(void*));
inline constexpr std::uint32_t kHostContextV1Size = static_cast<std::uint32_t>(
    offsetof(NevrHostContext, wrapper_hook_bytes) + sizeof(NevrHostContext::wrapper_hook_bytes));

/// Bytes of the host's hook recorded in NevrHostContext::wrapper_hook_bytes.
inline constexpr std::size_t kHookBytesSize = sizeof(NevrHostContext::wrapper_hook_bytes);

static_assert(std::is_standard_layout_v<NevrAbiHeader> && std::is_trivially_copyable_v<NevrAbiHeader>);
static_assert(std::is_standard_layout_v<NevrHostContext> && std::is_trivially_copyable_v<NevrHostContext>);
static_assert(std::is_standard_layout_v<NevrRuntimeApi> && std::is_trivially_copyable_v<NevrRuntimeApi>);
// Pinned x64 layout. Changing any of these is an ABI break: bump kMajor.
static_assert(sizeof(NevrAbiHeader) == 16);
static_assert(offsetof(NevrHostContext, host_module) == 16);
static_assert(offsetof(NevrHostContext, game_module) == 24);
static_assert(offsetof(NevrHostContext, original_game_main_wrapper) == 32);
static_assert(offsetof(NevrHostContext, wrapper_hook_target) == 40);
static_assert(offsetof(NevrHostContext, wrapper_hook_bytes) == 48);
static_assert(kHookBytesSize == 16);
static_assert(sizeof(NevrHostContext) == kHostContextV1Size && kHostContextV1Size == 64);
static_assert(offsetof(NevrRuntimeApi, initialize) == 16);
static_assert(offsetof(NevrRuntimeApi, game_main) == 24);
static_assert(sizeof(NevrRuntimeApi) == kRuntimeApiV1Size && kRuntimeApiV1Size == 32);

enum class Status : std::uint32_t {
  kOk = 0,
  kNullDescriptor = 1,
  kHeaderTooSmall = 2,
  kMajorMismatch = 3,
  kStructTooSmall = 4,
  kMissingEntry = 5,
};

/// Stable lower-case token for diagnostics records.
inline constexpr const char* StatusName(Status status) noexcept {
  switch (status) {
    case Status::kOk: return "ok";
    case Status::kNullDescriptor: return "null_descriptor";
    case Status::kHeaderTooSmall: return "header_too_small";
    case Status::kMajorMismatch: return "major_mismatch";
    case Status::kStructTooSmall: return "struct_too_small";
    case Status::kMissingEntry: return "missing_entry";
  }
  return "unknown";
}

/// Header-only checks. Reads struct_size first and nothing beyond the header.
inline Status CheckHeader(const NevrAbiHeader* header, std::uint32_t required_size) noexcept {
  if (header == nullptr) return Status::kNullDescriptor;
  if (header->struct_size < sizeof(NevrAbiHeader)) return Status::kHeaderTooSmall;
  if (header->abi_major != kMajor) return Status::kMajorMismatch;
  if (header->struct_size < required_size) return Status::kStructTooSmall;
  return Status::kOk;
}

/// True when the writer's struct_size includes the field ending at `field_end`
/// — the gate for reading any field appended by a later minor version.
inline bool DescriptorCovers(const NevrAbiHeader* header, std::size_t field_end) noexcept {
  return header != nullptr && header->struct_size >= field_end;
}

/// Host side: validate nevr.dll's descriptor before calling through it.
inline Status ValidateRuntimeApi(const NevrRuntimeApi* api) noexcept {
  const Status header = CheckHeader(api == nullptr ? nullptr : &api->header, kRuntimeApiV1Size);
  if (header != Status::kOk) return header;
  if (api->initialize == nullptr || api->game_main == nullptr) return Status::kMissingEntry;
  return Status::kOk;
}

/// Runtime side: validate the host context before using it.
inline Status ValidateHostContext(const NevrHostContext* host) noexcept {
  const Status header = CheckHeader(host == nullptr ? nullptr : &host->header, kHostContextV1Size);
  if (header != Status::kOk) return header;
  if (host->game_module == nullptr || host->original_game_main_wrapper == nullptr ||
      host->wrapper_hook_target == nullptr) {
    return Status::kMissingEntry;
  }
  return Status::kOk;
}

}  // namespace nevr_runtime_abi
