#pragma once

// Host <-> runtime C ABI between BugSplat64.dll and nevr-runtime.dll
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md).
//
// BugSplat64.dll (the stable host the game statically imports) loads nevr-runtime.dll at
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
//       minor 1: NevrRuntimeApi::exports, the runtime's side of the 16 exports the host
//                publishes under the names plugins resolve from BugSplat64.dll.
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
  /// 1 when the host read wrapper_hook_bytes from the target after enabling its hook, 0 when it
  /// could not (the bytes are then zeros and mean nothing). Anything else is malformed. Use
  /// nevr_runtime_abi::HookRecordUsable before registering the bytes with HookGuard.
  std::uint32_t wrapper_hook_bytes_valid;
  std::uint32_t reserved;
};

/// The runtime's implementation of the exports BugSplat64.dll publishes for the runtime (minor 1). The host
/// exports a typed thunk under each name, so the export table the game, the launcher and the plugins resolve
/// stays the one tools/verify_pe_contract.py pins, and a thunk calls through here only once the runtime is
/// loaded and initialized. Each entry has the signature of the export it serves; a pointer the host cannot
/// prove non-null is never called. The runtime catches every exception inside an entry: none crosses the
/// boundary.
struct NevrRuntimeExports {
  // NEVR_* (plugins and the launcher)
  void (*schedule_return_to_lobby)();
  /// `out` is a NevrUPnPConfig (runtime/hook/patching.h); the host passes the caller's pointer through.
  void (*get_upnp_config)(void* out);
  void (*register_resource_override)(std::uint64_t type_hash, std::uint64_t name_hash, const void* data,
                                     std::uint64_t size, const char* label);
  void (*reset_resource_overrides)();
  void (*deregister_resource_overrides)(const void* data_start, const void* data_end);
  int (*register_asset)(std::uint64_t type_hash, std::uint64_t name_hash, const void* data, std::uint64_t size,
                        const char* label);
  void (*deregister_assets)(const void* data_start, const void* data_end);
  // TokenAuth_* (resolved by name from the host module)
  const char* (*token_auth_get_token)();
  std::uint64_t (*token_auth_get_discord_id)();
  const char* (*token_auth_get_username)();
  // The two static modules' entry points (src/extension/module_interface.h); `ctx` is an NvrModuleContext.
  std::uint32_t (*platform_compat_api_version)();
  int (*platform_compat_init)(const void* ctx);
  void (*platform_compat_shutdown)();
  std::uint32_t (*token_auth_api_version)();
  int (*token_auth_init)(const void* ctx);
  void (*token_auth_shutdown)();
};

/// Returned by nevr-runtime.dll's NEVR_GetRuntimeApi export. initialize and game_main are required in v1;
/// `exports` is read only when the descriptor covers it (minor 1).
struct NevrRuntimeApi {
  NevrAbiHeader header;
  /// Returns 0 on success. Called once, after the descriptor validated, at the
  /// wrapper's entry.
  std::int32_t (*initialize)(const NevrHostContext* host);
  /// Runs the game's main under the runtime's recovery, in place of the original
  /// wrapper. Called by the host detour with the wrapper's argument; its result is
  /// the wrapper's result.
  std::int64_t (*game_main)(std::int64_t command_line);
  /// Minor 1. Absent from a descriptor whose struct_size ends at game_main.
  NevrRuntimeExports exports;
};

/// Signature of nevr-runtime.dll's single versioned entry point.
typedef const NevrRuntimeApi* (*NevrGetRuntimeApiFn)();

}  // extern "C"

namespace nevr_runtime_abi {

inline constexpr char kGetRuntimeApiExport[] = "NEVR_GetRuntimeApi";
inline constexpr std::uint32_t kMajor = 1;
inline constexpr std::uint32_t kMinor = 1;

/// Smallest struct_size a v1 writer may report: every v1 field present.
inline constexpr std::uint32_t kRuntimeApiV1Size =
    static_cast<std::uint32_t>(offsetof(NevrRuntimeApi, game_main) + sizeof(void*));
/// Size of a descriptor that carries the export table (minor 1).
inline constexpr std::uint32_t kRuntimeApiV1_1Size =
    static_cast<std::uint32_t>(offsetof(NevrRuntimeApi, exports) + sizeof(NevrRuntimeExports));
inline constexpr std::uint32_t kHostContextV1Size = static_cast<std::uint32_t>(
    offsetof(NevrHostContext, reserved) + sizeof(NevrHostContext::reserved));

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
static_assert(offsetof(NevrHostContext, wrapper_hook_bytes_valid) == 64);
static_assert(offsetof(NevrHostContext, reserved) == 68);
static_assert(sizeof(NevrHostContext) == kHostContextV1Size && kHostContextV1Size == 72);
static_assert(offsetof(NevrRuntimeApi, initialize) == 16);
static_assert(offsetof(NevrRuntimeApi, game_main) == 24);
static_assert(offsetof(NevrRuntimeApi, exports) == 32);
static_assert(sizeof(NevrRuntimeExports) == 16 * sizeof(void*));
static_assert(sizeof(NevrRuntimeApi) == kRuntimeApiV1_1Size && kRuntimeApiV1_1Size == 32 + 16 * 8);
static_assert(kRuntimeApiV1Size == 32);

enum class Status : std::uint32_t {
  kOk = 0,
  kNullDescriptor = 1,
  kHeaderTooSmall = 2,
  kMajorMismatch = 3,
  kStructTooSmall = 4,
  kMissingEntry = 5,
  kInvalidField = 6,
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
    case Status::kInvalidField: return "invalid_field";
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

/// The runtime's export table, or nullptr when the descriptor is absent, malformed or older than minor 1. The
/// host calls an entry only through this and only after checking the entry it wants is non-null.
inline const NevrRuntimeExports* RuntimeExports(const NevrRuntimeApi* api) noexcept {
  if (api == nullptr || !DescriptorCovers(&api->header, kRuntimeApiV1_1Size)) return nullptr;
  return &api->exports;
}

/// Host side: validate nevr-runtime.dll's descriptor before calling through it.
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
  if (host->host_module == nullptr || host->game_module == nullptr ||
      host->original_game_main_wrapper == nullptr || host->wrapper_hook_target == nullptr) {
    return Status::kMissingEntry;
  }
  // An unread record (0) is accepted: the runtime can still run, and refusing to start over a
  // HookGuard registration would take every runtime fix with it. It is not usable (below).
  if (host->wrapper_hook_bytes_valid > 1) return Status::kInvalidField;
  return Status::kOk;
}

/// True when the host's hook record may be registered with HookGuard: the writer's size covers
/// the flag and the flag says the host read the bytes. False for an unread or malformed record.
/// Only meaningful for a context ValidateHostContext accepted: it does not re-check the header,
/// the target or the flag's range, so call it after that returned kOk.
inline bool HookRecordUsable(const NevrHostContext* host) noexcept {
  return host != nullptr &&
         DescriptorCovers(&host->header, offsetof(NevrHostContext, wrapper_hook_bytes_valid) +
                                             sizeof(host->wrapper_hook_bytes_valid)) &&
         host->wrapper_hook_bytes_valid == 1;
}

}  // namespace nevr_runtime_abi
