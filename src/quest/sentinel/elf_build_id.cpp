#include "quest/sentinel/elf_build_id.h"

#include <elf.h>

#include <cstring>

namespace sentinel {

namespace {

constexpr std::size_t kMaxProgramHeaders = 128;
constexpr std::uint64_t kMaxNoteBytes = 64 * 1024;
constexpr std::uint64_t kPageMask = 0xfff;
constexpr std::uint32_t kNoteGnuBuildId = 3;  // NT_GNU_BUILD_ID

bool CanRead(ReadableFn readable, const void* address, std::size_t length) {
  return readable == nullptr || readable(address, length);
}

std::uint64_t Align4(std::uint64_t value) { return (value + 3u) & ~static_cast<std::uint64_t>(3u); }

}  // namespace

bool ReadElfBuildId(const void* base, ReadableFn readable, std::uint8_t* out, std::size_t capacity,
                    std::size_t* length) {
  if (base == nullptr || out == nullptr || length == nullptr) return false;
  *length = 0;
  const auto* image = static_cast<const std::uint8_t*>(base);
  if (!CanRead(readable, image, sizeof(Elf64_Ehdr))) return false;
  Elf64_Ehdr header;
  std::memcpy(&header, image, sizeof(header));
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0 || header.e_phnum > kMaxProgramHeaders) {
    return false;
  }
  const std::size_t table_bytes = static_cast<std::size_t>(header.e_phnum) * sizeof(Elf64_Phdr);
  const std::uint8_t* table = image + header.e_phoff;
  if (!CanRead(readable, table, table_bytes)) return false;

  // The image's load bias: where the first mapping sits less the lowest virtual address a PT_LOAD asks for.
  bool has_load = false;
  std::uint64_t lowest = 0;
  for (std::size_t i = 0; i < header.e_phnum; ++i) {
    Elf64_Phdr phdr;
    std::memcpy(&phdr, table + i * sizeof(phdr), sizeof(phdr));
    if (phdr.p_type != PT_LOAD) continue;
    const std::uint64_t start = phdr.p_vaddr & ~kPageMask;
    if (!has_load || start < lowest) lowest = start;
    has_load = true;
  }
  if (!has_load) return false;
  const std::uint64_t bias = reinterpret_cast<std::uintptr_t>(image) - lowest;

  for (std::size_t i = 0; i < header.e_phnum; ++i) {
    Elf64_Phdr phdr;
    std::memcpy(&phdr, table + i * sizeof(phdr), sizeof(phdr));
    if (phdr.p_type != PT_NOTE || phdr.p_memsz == 0 || phdr.p_memsz > kMaxNoteBytes) continue;
    const auto* notes = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(bias + phdr.p_vaddr));
    if (!CanRead(readable, notes, static_cast<std::size_t>(phdr.p_memsz))) continue;
    std::uint64_t at = 0;
    while (at + sizeof(Elf64_Nhdr) <= phdr.p_memsz) {
      Elf64_Nhdr note;
      std::memcpy(&note, notes + at, sizeof(note));
      const std::uint64_t name_at = at + sizeof(note);
      const std::uint64_t desc_at = name_at + Align4(note.n_namesz);
      const std::uint64_t next = desc_at + Align4(note.n_descsz);
      if (next > phdr.p_memsz || next < at) break;
      if (note.n_type == kNoteGnuBuildId && note.n_namesz == 4 && std::memcmp(notes + name_at, "GNU", 4) == 0 &&
          note.n_descsz > 0 && note.n_descsz <= capacity) {
        std::memcpy(out, notes + desc_at, note.n_descsz);
        *length = note.n_descsz;
        return true;
      }
      at = next;
    }
  }
  return false;
}

}  // namespace sentinel
