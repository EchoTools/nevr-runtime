#pragma once

// Shim <-> runtime C ABI on the Quest, between the shim (libovrplatformloader.so, the trust anchor the APK carries) and the
// runtime (libnevr-runtime.so, the part self-update replaces; docs/adr/0024-pc-self-update.md and its Quest counterpart).
//
// The shim maps the runtime from the app's private files folder (or, as the baseline, from the APK's native library
// folder), resolves NEVR_GetRuntimeApi, validates the descriptor it returns BEFORE calling anything through it, and calls
// `initialize` with a NevrQuestHostContext. The runtime validates the context the same way. Only C types cross this
// boundary: no C++ classes, no STL, no exceptions, no allocator-owned objects.
//
// This is the Quest's own ABI, with its own major and minor (it starts at 1.0); it shares only the descriptor header, the
// status codes, the checks that read that header, the entry-point name and capability bit 0 with the PC ABI
// (core/runtime_abi.h), through core/runtime_abi_common.h. It carries no <windows.h>. The versioning rules are the common
// header's: a major mismatch is a hard reject, a newer minor only APPENDS fields, and a reader uses an appended field only
// after nevr_runtime_abi::DescriptorCovers() proves the writer's struct_size includes it.
//
// The layout is the AArch64 (LP64) layout, pinned by static_asserts below. src/quest/tests/quest_runtime_abi_test.cpp
// restates every offset, so moving a field here breaks that file's build.
//
// Header-only and stateless on purpose: the shim and the runtime both compile it and neither owns state through it.

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core/runtime_abi_common.h"

extern "C" {

/// One key/value pair of a record written through NevrQuestLogFn. Both strings are copied before the call returns; a null
/// key drops the pair and a null value reads as "".
struct NevrQuestLogField {
  const char* key;
  const char* value;
};

/// The shim's structured log, as the runtime sees it: one sentinel record with `tag` (for example "NEVR.UPDATE"), `event`,
/// an optional `message` and the fields, at `level` (the numbering of EchoVR::LogLevel: 1 debug, 2 info, 4 warning, 8 error).
/// Every string is plain text, never a printf format. Thread-safe.
typedef void (*NevrQuestLogFn)(std::uint32_t level, const char* tag, const char* event, const char* message,
                               const NevrQuestLogField* fields, std::uint32_t field_count);

/// Handed by the shim to NevrQuestRuntimeApi::initialize. Every pointer is valid for the life of the process.
struct NevrQuestHostContext {
  NevrAbiHeader header;
  /// The shim's own dlopen handle.
  void* shim_handle;
  /// The load base of the libr15.so the shim attached to.
  const void* libr15_base;
  /// The GNU build ID of that libr15.so, lower-case hex, NUL-terminated: the patched one
  /// (sentinel::pinned::LibR15PatchedBuildId()), which a runtime checks against the builds it supports.
  const char* libr15_build_id;
  /// The folder that holds `update/`, NUL-terminated UTF-8. Null when the shim could not form one.
  const char* update_root;
  /// The shim's `NEVR-BUILD` version, NUL-terminated. Null when absent.
  const char* shim_version;
  /// The shim's log. Null when the shim has none.
  NevrQuestLogFn log;
  /// The variant the APK is stamped with: the value of nevr_quest::build_variant::Variant (0 none, then the enum order).
  std::uint32_t variant;
  std::uint32_t reserved;
  /// What this shim can do, a bit per capability (nevr_runtime_abi::kHostCapabilitySwap is bit 0, as on PC).
  std::uint64_t capabilities;
};

/// Returned by libnevr-runtime.so's NEVR_GetRuntimeApi export.
struct NevrQuestRuntimeApi {
  NevrAbiHeader header;
  /// Returns 0 on success. Called once, after the descriptor validated.
  std::int32_t (*initialize)(const NevrQuestHostContext* host);
};

/// Signature of libnevr-runtime.so's single versioned entry point (nevr_runtime_abi::kGetRuntimeApiExport).
typedef const NevrQuestRuntimeApi* (*NevrQuestGetRuntimeApiFn)();

}  // extern "C"

namespace nevr_quest_runtime_abi {

inline constexpr std::uint32_t kMajor = 1;
inline constexpr std::uint32_t kMinor = 0;

/// Smallest struct_size a v1 writer may report: every v1 field present.
inline constexpr std::uint32_t kRuntimeApiV1Size =
    static_cast<std::uint32_t>(offsetof(NevrQuestRuntimeApi, initialize) + sizeof(void*));
inline constexpr std::uint32_t kHostContextV1Size = static_cast<std::uint32_t>(
    offsetof(NevrQuestHostContext, capabilities) + sizeof(NevrQuestHostContext::capabilities));

static_assert(std::is_standard_layout_v<NevrQuestLogField> && std::is_trivially_copyable_v<NevrQuestLogField>);
static_assert(std::is_standard_layout_v<NevrQuestHostContext> && std::is_trivially_copyable_v<NevrQuestHostContext>);
static_assert(std::is_standard_layout_v<NevrQuestRuntimeApi> && std::is_trivially_copyable_v<NevrQuestRuntimeApi>);
// Pinned AArch64 layout. Changing any of these is an ABI break: bump kMajor.
static_assert(sizeof(void*) == 8, "the Quest ABI is LP64 (AArch64)");
static_assert(sizeof(NevrQuestLogField) == 16);
static_assert(offsetof(NevrQuestHostContext, shim_handle) == 16);
static_assert(offsetof(NevrQuestHostContext, libr15_base) == 24);
static_assert(offsetof(NevrQuestHostContext, libr15_build_id) == 32);
static_assert(offsetof(NevrQuestHostContext, update_root) == 40);
static_assert(offsetof(NevrQuestHostContext, shim_version) == 48);
static_assert(offsetof(NevrQuestHostContext, log) == 56);
static_assert(offsetof(NevrQuestHostContext, variant) == 64);
static_assert(offsetof(NevrQuestHostContext, reserved) == 68);
static_assert(offsetof(NevrQuestHostContext, capabilities) == 72);
static_assert(sizeof(NevrQuestHostContext) == kHostContextV1Size && kHostContextV1Size == 80);
static_assert(offsetof(NevrQuestRuntimeApi, initialize) == 16);
static_assert(sizeof(NevrQuestRuntimeApi) == kRuntimeApiV1Size && kRuntimeApiV1Size == 24);

/// Shim side: validate libnevr-runtime.so's descriptor before calling through it.
inline nevr_runtime_abi::Status ValidateRuntimeApi(const NevrQuestRuntimeApi* api) noexcept {
  const nevr_runtime_abi::Status header =
      nevr_runtime_abi::CheckHeader(api == nullptr ? nullptr : &api->header, kRuntimeApiV1Size, kMajor);
  if (header != nevr_runtime_abi::Status::kOk) return header;
  if (api->initialize == nullptr) return nevr_runtime_abi::Status::kMissingEntry;
  return nevr_runtime_abi::Status::kOk;
}

/// Runtime side: validate the shim's context before using it. The shim handle, the libr15 base and its build ID are required:
/// without them the runtime cannot tell which libr15 it would hook. The update root, the shim version and the log may be
/// absent.
inline nevr_runtime_abi::Status ValidateHostContext(const NevrQuestHostContext* host) noexcept {
  const nevr_runtime_abi::Status header =
      nevr_runtime_abi::CheckHeader(host == nullptr ? nullptr : &host->header, kHostContextV1Size, kMajor);
  if (header != nevr_runtime_abi::Status::kOk) return header;
  if (host->shim_handle == nullptr || host->libr15_base == nullptr || host->libr15_build_id == nullptr) {
    return nevr_runtime_abi::Status::kMissingEntry;
  }
  return nevr_runtime_abi::Status::kOk;
}

/// The shim's capabilities word, or 0 when the context is absent or its struct_size ends before the word.
inline std::uint64_t HostCapabilities(const NevrQuestHostContext* host) noexcept {
  return host != nullptr && nevr_runtime_abi::DescriptorCovers(&host->header, kHostContextV1Size) ? host->capabilities : 0;
}

/// True when the shim says it performs the staged swap (nevr_runtime_abi::kHostCapabilitySwap, bit 0).
inline bool HostCanSwap(const NevrQuestHostContext* host) noexcept {
  return (HostCapabilities(host) & nevr_runtime_abi::kHostCapabilitySwap) != 0;
}

}  // namespace nevr_quest_runtime_abi
