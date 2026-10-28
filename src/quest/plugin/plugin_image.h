// What the loader asks of a plugin file before it maps it, and of the plugin's declared build IDs (ADR 0019, sections 3
// and 7). Pure: no Android, no dlopen. Every read of `bytes` is bounds-checked: the file is data the loader has not trusted
// yet.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nevr_quest::plugin {

inline constexpr std::uint16_t kMachineAarch64 = 183;  // EM_AARCH64
inline constexpr std::uint16_t kMachineX86_64 = 62;    // EM_X86_64, for the host test's fixture objects

// True when `bytes` is a little-endian 64-bit ELF shared object (ET_DYN) for `machine`. Anything else (a PE, a
// truncated file, another machine, another class or byte order, an executable) is not: the reason token is
// `machine_type`.
bool IsSharedObjectFor(const std::uint8_t* bytes, std::size_t size, std::uint16_t machine);

// The GNU build ID of the object in `bytes` (NT_GNU_BUILD_ID in a PT_NOTE segment) as lowercase hex, or "" when there is
// none or the headers do not hold together. Reads the file as it is on disk, so it works before the object is mapped.
std::string BuildIdOf(const std::uint8_t* bytes, std::size_t size);

enum class BuildIdVerdict {
  kSupported,
  kUndeclared,   // the plugin has no NEVR_GetSupportedBuildIds export (`list` is null)
  kUnsupported,  // the list does not hold the running build
  kUnknownGame,  // the running build ID could not be read: nothing can be proved
};

// `list` is the NUL-terminated text the plugin's NEVR_GetSupportedBuildIds returned (null: no export): lowercase hex
// build IDs separated by spaces, commas or newlines. `running` is the running libr15's build ID, lowercase hex ("" when
// it could not be read). The match is the whole ID, never a prefix or a substring.
BuildIdVerdict DecideBuildId(const char* list, const std::string& running);

}  // namespace nevr_quest::plugin
