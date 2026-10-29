#include "quest/plugin/plugin_image.h"

#include <elf.h>

#include <cstring>
#include <string_view>

namespace nevr_quest::plugin {

namespace {

// [offset, offset + length) lies inside a buffer of `size` bytes, with no wrap-around.
bool InRange(std::uint64_t offset, std::uint64_t length, std::size_t size) {
  return offset <= size && length <= size - offset;
}

bool ReadHeader(const std::uint8_t* bytes, std::size_t size, Elf64_Ehdr* out) {
  if (bytes == nullptr || size < sizeof(Elf64_Ehdr)) return false;
  std::memcpy(out, bytes, sizeof(*out));
  if (std::memcmp(out->e_ident, ELFMAG, SELFMAG) != 0) return false;
  return out->e_ident[EI_CLASS] == ELFCLASS64 && out->e_ident[EI_DATA] == ELFDATA2LSB;
}

constexpr std::size_t kMaxBuildIdListBytes = 64 * 1024;

}  // namespace

bool IsSharedObjectFor(const std::uint8_t* bytes, std::size_t size, std::uint16_t machine) {
  Elf64_Ehdr header;
  if (!ReadHeader(bytes, size, &header)) return false;
  return header.e_type == ET_DYN && header.e_machine == machine;
}

namespace {

// The description bytes of the first note in a PT_NOTE segment whose owner is `owner` (NUL included in namesz) and whose
// type is `type`; false when there is none or the headers do not hold together.
bool FindNote(const std::uint8_t* bytes, std::size_t size, const char* owner, std::uint32_t ownerSize, std::uint32_t type,
              const std::uint8_t** desc, std::uint32_t* descSize) {
  Elf64_Ehdr header;
  if (!ReadHeader(bytes, size, &header)) return false;
  if (header.e_phentsize != sizeof(Elf64_Phdr)) return false;
  if (!InRange(header.e_phoff, static_cast<std::uint64_t>(header.e_phnum) * sizeof(Elf64_Phdr), size)) return false;
  for (unsigned i = 0; i < header.e_phnum; ++i) {
    Elf64_Phdr phdr;
    std::memcpy(&phdr, bytes + header.e_phoff + i * sizeof(Elf64_Phdr), sizeof(phdr));
    if (phdr.p_type != PT_NOTE || !InRange(phdr.p_offset, phdr.p_filesz, size)) continue;
    std::uint64_t at = phdr.p_offset;
    const std::uint64_t end = phdr.p_offset + phdr.p_filesz;
    while (end - at >= sizeof(Elf64_Nhdr)) {
      Elf64_Nhdr note;
      std::memcpy(&note, bytes + at, sizeof(note));
      at += sizeof(note);
      const std::uint64_t nameLen = (static_cast<std::uint64_t>(note.n_namesz) + 3) & ~std::uint64_t{3};
      const std::uint64_t descLen = (static_cast<std::uint64_t>(note.n_descsz) + 3) & ~std::uint64_t{3};
      if (nameLen > end - at || descLen > end - at - nameLen) break;
      if (note.n_type == type && note.n_namesz == ownerSize && std::memcmp(bytes + at, owner, ownerSize) == 0 &&
          note.n_descsz <= end - at - nameLen) {
        *desc = bytes + at + nameLen;
        *descSize = note.n_descsz;
        return true;
      }
      at += nameLen + descLen;
    }
  }
  return false;
}

constexpr std::uint32_t kNevrBuildIdsNoteType = 1;  // NEVR_DECLARE_SUPPORTED_BUILD_IDS (plugin_interface.h)

}  // namespace

std::string BuildIdOf(const std::uint8_t* bytes, std::size_t size) {
  const std::uint8_t* desc = nullptr;
  std::uint32_t descSize = 0;
  if (!FindNote(bytes, size, "GNU", 4, NT_GNU_BUILD_ID, &desc, &descSize)) return "";
  static const char kHex[] = "0123456789abcdef";
  std::string id;
  for (std::uint32_t k = 0; k < descSize; ++k) {
    id += kHex[desc[k] >> 4];
    id += kHex[desc[k] & 0xf];
  }
  return id;
}

std::optional<std::string> SupportedBuildIdsOf(const std::uint8_t* bytes, std::size_t size) {
  const std::uint8_t* desc = nullptr;
  std::uint32_t descSize = 0;
  if (!FindNote(bytes, size, "NEVR", 5, kNevrBuildIdsNoteType, &desc, &descSize)) return std::nullopt;
  if (descSize > kMaxBuildIdListBytes) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(desc), descSize);
}

BuildIdVerdict DecideBuildId(const char* list, const std::string& running) {
  if (list == nullptr) return BuildIdVerdict::kUndeclared;
  if (running.empty()) return BuildIdVerdict::kUnknownGame;
  const std::string_view text(list, strnlen(list, kMaxBuildIdListBytes));
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t start = text.find_first_not_of(" ,\t\r\n", at);
    if (start == std::string_view::npos) break;
    std::size_t stop = text.find_first_of(" ,\t\r\n", start);
    if (stop == std::string_view::npos) stop = text.size();
    if (text.substr(start, stop - start) == running) return BuildIdVerdict::kSupported;
    at = stop;
  }
  return BuildIdVerdict::kUnsupported;
}

BuildIdVerdict DecideBuildIdForView(const char* list, const std::string& live, const PluginView& view) {
  const BuildIdVerdict verdict = DecideBuildId(list, std::string(view.buildId));
  if (verdict == BuildIdVerdict::kUnsupported && view.patched) return DecideBuildId(list, live);
  return verdict;
}

}  // namespace nevr_quest::plugin
