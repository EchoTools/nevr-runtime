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

std::string BuildIdOf(const std::uint8_t* bytes, std::size_t size) {
  Elf64_Ehdr header;
  if (!ReadHeader(bytes, size, &header)) return "";
  if (header.e_phentsize != sizeof(Elf64_Phdr)) return "";
  if (!InRange(header.e_phoff, static_cast<std::uint64_t>(header.e_phnum) * sizeof(Elf64_Phdr), size)) return "";
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
      if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 && std::memcmp(bytes + at, "GNU", 4) == 0) {
        static const char kHex[] = "0123456789abcdef";
        std::string id;
        const std::uint8_t* desc = bytes + at + nameLen;
        for (std::uint32_t k = 0; k < note.n_descsz; ++k) {
          id += kHex[desc[k] >> 4];
          id += kHex[desc[k] & 0xf];
        }
        return id;
      }
      at += nameLen + descLen;
    }
  }
  return "";
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

}  // namespace nevr_quest::plugin
