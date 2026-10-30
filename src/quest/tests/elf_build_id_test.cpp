// The GNU build id reader of the Quest crash report (src/quest/sentinel/elf_build_id.h, #1605), over a synthetic ELF
// image laid out byte by byte: the build id is what ties a report to the libr15.so build it came from.

#include "quest/sentinel/elf_build_id.h"

#include <elf.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                              \
  do {                                                                                \
    if (!(condition)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                                                   \
    }                                                                                 \
  } while (0)

constexpr std::size_t kImageBytes = 0x1000;
constexpr std::size_t kNoteAt = 0x200;
const std::uint8_t kId[20] = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                              0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};

// An image whose first (and only) PT_LOAD asks for vaddr `load_vaddr`, with one PT_NOTE holding a GNU build id.
struct Image {
  alignas(16) std::uint8_t bytes[kImageBytes];
  explicit Image(std::uint64_t load_vaddr = 0, std::uint32_t note_type = 3) {
    std::memset(bytes, 0, sizeof(bytes));
    Elf64_Ehdr header;
    std::memset(&header, 0, sizeof(header));
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_phoff = sizeof(Elf64_Ehdr);
    header.e_phentsize = sizeof(Elf64_Phdr);
    header.e_phnum = 2;
    std::memcpy(bytes, &header, sizeof(header));
    Elf64_Phdr load;
    std::memset(&load, 0, sizeof(load));
    load.p_type = PT_LOAD;
    load.p_vaddr = load_vaddr;
    load.p_memsz = kImageBytes;
    Elf64_Phdr note;
    std::memset(&note, 0, sizeof(note));
    note.p_type = PT_NOTE;
    note.p_vaddr = load_vaddr + kNoteAt;
    note.p_memsz = sizeof(Elf64_Nhdr) + 4 + sizeof(kId);
    std::memcpy(bytes + sizeof(Elf64_Ehdr), &load, sizeof(load));
    std::memcpy(bytes + sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr), &note, sizeof(note));
    Elf64_Nhdr nhdr;
    nhdr.n_namesz = 4;
    nhdr.n_descsz = sizeof(kId);
    nhdr.n_type = note_type;
    std::memcpy(bytes + kNoteAt, &nhdr, sizeof(nhdr));
    std::memcpy(bytes + kNoteAt + sizeof(nhdr), "GNU", 4);
    std::memcpy(bytes + kNoteAt + sizeof(nhdr) + 4, kId, sizeof(kId));
  }
};

void ReadsTheBuildIdOfAnImageLoadedAtItsLinkAddress() {
  Image image;
  std::uint8_t out[32];
  std::size_t length = 0;
  CHECK(sentinel::ReadElfBuildId(image.bytes, nullptr, out, sizeof(out), &length));
  CHECK(length == sizeof(kId));
  CHECK(std::memcmp(out, kId, sizeof(kId)) == 0);
}

// A PT_LOAD that asks for a nonzero address moves the note with it: the bias is the mapping less that address.
void FollowsTheLoadBias() {
  Image image(0x100000);
  std::uint8_t out[32];
  std::size_t length = 0;
  CHECK(sentinel::ReadElfBuildId(image.bytes, nullptr, out, sizeof(out), &length));
  CHECK(length == sizeof(kId));
}

void IgnoresANoteThatIsNotTheBuildId() {
  Image image(0, 1);
  std::uint8_t out[32];
  std::size_t length = 0;
  CHECK(!sentinel::ReadElfBuildId(image.bytes, nullptr, out, sizeof(out), &length));
  CHECK(length == 0);
}

void RefusesABufferTooSmallForTheId() {
  Image image;
  std::uint8_t out[8];
  std::size_t length = 0;
  CHECK(!sentinel::ReadElfBuildId(image.bytes, nullptr, out, sizeof(out), &length));
}

void RefusesWhatIsNotAn64BitElf() {
  Image image;
  image.bytes[0] = 0;
  std::uint8_t out[32];
  std::size_t length = 0;
  CHECK(!sentinel::ReadElfBuildId(image.bytes, nullptr, out, sizeof(out), &length));
  Image narrow;
  narrow.bytes[EI_CLASS] = ELFCLASS32;
  CHECK(!sentinel::ReadElfBuildId(narrow.bytes, nullptr, out, sizeof(out), &length));
}

void StopsWhereTheReadabilityProbeSays() {
  Image image;
  std::uint8_t out[32];
  std::size_t length = 0;
  const auto refuse = [](const void*, std::size_t) { return false; };
  CHECK(!sentinel::ReadElfBuildId(image.bytes, refuse, out, sizeof(out), &length));
}

}  // namespace

int main() {
  ReadsTheBuildIdOfAnImageLoadedAtItsLinkAddress();
  FollowsTheLoadBias();
  IgnoresANoteThatIsNotTheBuildId();
  RefusesABufferTooSmallForTheId();
  RefusesWhatIsNotAn64BitElf();
  StopsWhereTheReadabilityProbeSays();
  if (g_failures != 0) {
    std::fprintf(stderr, "elf_build_id_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::puts("elf_build_id_test: all checks passed");
  return 0;
}
