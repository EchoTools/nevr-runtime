#pragma once

// Host <-> runtime C ABI for the planned BugSplat64.dll / nevr.dll split
// (docs/design/2026-10-05-bugsplat-nevr-split.md, §"nevr.dll: changing runtime").
//
// BugSplat64.dll (the stable host the game statically imports) loads nevr.dll
// lazily, resolves NEVR_GetRuntimeApi, and validates the descriptor it returns
// BEFORE calling anything through it. The runtime validates the host context it
// is handed the same way. Only C types cross this boundary: no C++ classes, no
// STL, no exceptions, no allocator-owned objects.
//
// Versioning:
//   - abi_major: a mismatch is a hard reject in either direction.
//   - abi_minor: informational. Newer minors may only APPEND fields; a reader
//     uses an appended field only after NevrDescriptorCovers() proves the
//     writer's struct_size includes it.
//   - struct_size: the writer's sizeof. It is read before anything past the
//     16-byte header, so a smaller (older) descriptor is never over-read.
//
// Header-only and stateless on purpose: both DLLs compile it, neither owns
// state through it (design step 5, "no accidental duplicate static state").

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

extern "C" {

/// echovr.exe PreprocessCommandLine, `UINT64 (PVOID pGame)` in the x64 Microsoft
/// ABI (src/abi/echovr_functions.h:75; ReVault echovr.exe 0x140116720
/// CR15Game::vfunction1, single argument moved from RCX at 0x14011672F).
typedef std::uint64_t (*NevrPreprocessCommandLineFn)(void* game);

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
  /// The MinHook trampoline for the host-owned PreprocessCommandLine detour. The
  /// runtime calls the original through this and never installs a second hook
  /// on the rendezvous address.
  NevrPreprocessCommandLineFn original_preprocess;
};

/// Returned by nevr.dll's NEVR_GetRuntimeApi export. All entries are required in v1.
struct NevrRuntimeApi {
  NevrAbiHeader header;
  /// Returns 0 on success. Called once, after the descriptor validated.
  std::int32_t (*initialize)(const NevrHostContext* host);
  /// Called by the host detour before the original PreprocessCommandLine.
  void (*preprocess_before)(void* game);
  /// Called by the host detour after the original, with its exact result.
  void (*preprocess_after)(void* game, std::uint64_t result);
};

/// Signature of nevr.dll's single versioned entry point.
typedef const NevrRuntimeApi* (*NevrGetRuntimeApiFn)();

}  // extern "C"

namespace NevrAbi {

inline constexpr char kGetRuntimeApiExport[] = "NEVR_GetRuntimeApi";
inline constexpr std::uint32_t kMajor = 1;
inline constexpr std::uint32_t kMinor = 0;

/// Smallest struct_size a v1 writer may report: every v1 field present.
inline constexpr std::uint32_t kRuntimeApiV1Size =
    static_cast<std::uint32_t>(offsetof(NevrRuntimeApi, preprocess_after) + sizeof(void*));
inline constexpr std::uint32_t kHostContextV1Size =
    static_cast<std::uint32_t>(offsetof(NevrHostContext, original_preprocess) + sizeof(void*));

static_assert(std::is_standard_layout_v<NevrAbiHeader> && std::is_trivially_copyable_v<NevrAbiHeader>);
static_assert(std::is_standard_layout_v<NevrHostContext> && std::is_trivially_copyable_v<NevrHostContext>);
static_assert(std::is_standard_layout_v<NevrRuntimeApi> && std::is_trivially_copyable_v<NevrRuntimeApi>);
// Pinned x64 layout. Changing any of these is an ABI break: bump kMajor.
static_assert(sizeof(NevrAbiHeader) == 16);
static_assert(offsetof(NevrHostContext, host_module) == 16);
static_assert(offsetof(NevrHostContext, game_module) == 24);
static_assert(offsetof(NevrHostContext, original_preprocess) == 32);
static_assert(sizeof(NevrHostContext) == kHostContextV1Size && kHostContextV1Size == 40);
static_assert(offsetof(NevrRuntimeApi, initialize) == 16);
static_assert(offsetof(NevrRuntimeApi, preprocess_before) == 24);
static_assert(offsetof(NevrRuntimeApi, preprocess_after) == 32);
static_assert(sizeof(NevrRuntimeApi) == kRuntimeApiV1Size && kRuntimeApiV1Size == 40);

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
  if (api->initialize == nullptr || api->preprocess_before == nullptr ||
      api->preprocess_after == nullptr) {
    return Status::kMissingEntry;
  }
  return Status::kOk;
}

/// Runtime side: validate the host context before using it.
inline Status ValidateHostContext(const NevrHostContext* host) noexcept {
  const Status header = CheckHeader(host == nullptr ? nullptr : &host->header, kHostContextV1Size);
  if (header != Status::kOk) return header;
  if (host->game_module == nullptr || host->original_preprocess == nullptr) {
    return Status::kMissingEntry;
  }
  return Status::kOk;
}

}  // namespace NevrAbi
