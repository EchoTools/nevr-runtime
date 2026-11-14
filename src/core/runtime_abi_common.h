#pragma once

// What the PC ABI (core/runtime_abi.h, BugSplat64.dll <-> nevr-runtime.dll) and the Quest ABI (core/quest_runtime_abi.h,
// the shim <-> libnevr_runtime.so) share: the header every descriptor starts with, the status codes and the two checks that
// read only that header, the one capability bit both name, and the name of the one entry point. Nothing here is specific to
// a platform: no <windows.h>, no pointer-sized field, no major or minor number (each ABI keeps its own, because the two
// versions move independently; docs/adr/0008-bugsplat-host-and-nevr-runtime.md, docs/adr/0024-pc-self-update.md).
//
// Versioning, for both ABIs:
//   - abi_major: a mismatch is a hard reject in either direction. CheckHeader is told the major to expect.
//   - abi_minor: informational. Newer minors may only APPEND fields; a reader uses an appended field only after
//     nevr_runtime_abi::DescriptorCovers() proves the writer's struct_size includes it.
//   - struct_size: the writer's sizeof. It is read before anything past the 16-byte header, so a smaller (older) descriptor
//     is never over-read.
//
// Header-only and stateless on purpose: both sides compile it and neither owns state through it, so the host and the runtime
// never hold two copies of one piece of state.

#include <cstddef>
#include <cstdint>
#include <type_traits>

extern "C" {

/// The first 16 bytes of every descriptor crossing a host/runtime boundary.
struct NevrAbiHeader {
  std::uint32_t struct_size;
  std::uint32_t abi_major;
  std::uint32_t abi_minor;
  std::uint32_t reserved;
};

}  // extern "C"

namespace nevr_runtime_abi {

/// The name of the runtime's single versioned entry point, on PC (nevr-runtime.dll) and on Quest (libnevr_runtime.so).
inline constexpr char kGetRuntimeApiExport[] = "NEVR_GetRuntimeApi";

/// Capability bit 0 of a host context's `capabilities` word, on PC and on Quest: this host performs the staged swap and puts
/// the previous release back when one does not start (docs/adr/0024-pc-self-update.md, sections 3 and 6). A runtime stages
/// nothing without it.
inline constexpr std::uint64_t kHostCapabilitySwap = std::uint64_t{1} << 0;

static_assert(std::is_standard_layout_v<NevrAbiHeader> && std::is_trivially_copyable_v<NevrAbiHeader>);
// Pinned layout of the header. Changing any of these is an ABI break on both platforms.
static_assert(sizeof(NevrAbiHeader) == 16);
static_assert(offsetof(NevrAbiHeader, struct_size) == 0);
static_assert(offsetof(NevrAbiHeader, abi_major) == 4);
static_assert(offsetof(NevrAbiHeader, abi_minor) == 8);
static_assert(offsetof(NevrAbiHeader, reserved) == 12);

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

/// Header-only checks. Reads struct_size first and nothing beyond the header. `expected_major` is the major of the ABI the
/// caller speaks (PC's `kMajor`, Quest's `kMajor`).
inline Status CheckHeader(const NevrAbiHeader* header, std::uint32_t required_size, std::uint32_t expected_major) noexcept {
  if (header == nullptr) return Status::kNullDescriptor;
  if (header->struct_size < sizeof(NevrAbiHeader)) return Status::kHeaderTooSmall;
  if (header->abi_major != expected_major) return Status::kMajorMismatch;
  if (header->struct_size < required_size) return Status::kStructTooSmall;
  return Status::kOk;
}

/// True when the writer's struct_size includes the field ending at `field_end`
/// — the gate for reading any field appended by a later minor version.
inline bool DescriptorCovers(const NevrAbiHeader* header, std::size_t field_end) noexcept {
  return header != nullptr && header->struct_size >= field_end;
}

}  // namespace nevr_runtime_abi
