// The pins of src/quest/combat/frozen_frame_exit_targets.h, checked against the store libr15 (the hooks-pinned group, #2377).
// The in-VR word lies in .bss and in the section the section table names .bss; each instruction word is the file's word at its address; each adrp
// and its add, ldr or str name the in-VR word; each call is a bl to a PLT stub whose JUMP_SLOT is the named symbol's; the two functions are their
// .dynsym symbols' addresses and every pinned word lies inside the function it is named for.
//
// Run: frozen_frame_exit_pinned_test <libr15.so>
#include <elf.h>
#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "pinned_targets.h"
#include "quest/combat/frozen_frame_exit_targets.h"
#include "quest/combat/pinned_words.h"
#include "quest/sentinel/got_hook.h"
#include "quest/tests/test_check.h"

namespace {

using namespace sentinel;
namespace ffe = nevr_quest::combat::frozen_frame_exit::targets;
namespace cp = nevr_quest::combat::pinned;

struct LoadedElf {
  std::vector<char> bytes;  // the file, for the section and dynamic symbol tables
  void* mem = nullptr;      // the PT_LOAD segments at their virtual addresses (zero where the file has no bytes: .bss)
  std::size_t span = 0;
  std::vector<Elf64_Phdr> phdrs;
  ElfImage image;
  ~LoadedElf() {
    if (mem != nullptr) munmap(mem, span);
  }
};

bool Load(const char* path, LoadedElf* out) {
  std::ifstream file(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (bytes.size() < sizeof(Elf64_Ehdr)) return false;
  out->bytes = bytes;
  Elf64_Ehdr eh;
  std::memcpy(&eh, bytes.data(), sizeof(eh));
  if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_machine != EM_AARCH64 || eh.e_ident[EI_CLASS] != ELFCLASS64) return false;
  out->phdrs.resize(eh.e_phnum);
  std::memcpy(out->phdrs.data(), bytes.data() + eh.e_phoff, eh.e_phnum * sizeof(Elf64_Phdr));
  for (const Elf64_Phdr& ph : out->phdrs) {
    if (ph.p_type == PT_LOAD && ph.p_vaddr + ph.p_memsz > out->span) out->span = ph.p_vaddr + ph.p_memsz;
  }
  out->span = (out->span + 0xFFF) & ~static_cast<std::size_t>(0xFFF);
  out->mem = mmap(nullptr, out->span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (out->mem == MAP_FAILED) {
    out->mem = nullptr;
    return false;
  }
  for (const Elf64_Phdr& ph : out->phdrs) {
    if (ph.p_type != PT_LOAD) continue;
    if (ph.p_offset + ph.p_filesz > bytes.size()) return false;
    std::memcpy(static_cast<char*>(out->mem) + ph.p_vaddr, bytes.data() + ph.p_offset, ph.p_filesz);
  }
  out->image.base = reinterpret_cast<std::uintptr_t>(out->mem);
  out->image.phdr = out->phdrs.data();
  out->image.phnum = out->phdrs.size();
  std::strncpy(out->image.name, path, sizeof(out->image.name) - 1);
  return true;
}

bool WordAt(const LoadedElf& elf, std::uint64_t vaddr, std::uint32_t* out) {
  if (elf.mem == nullptr || vaddr + sizeof(*out) > elf.span) return false;
  std::memcpy(out, static_cast<const char*>(elf.mem) + vaddr, sizeof(*out));
  return true;
}

// [vaddr, vaddr + size) lies in the part of a PT_LOAD segment that the file does not fill.
bool InZeroFill(const LoadedElf& elf, std::uint64_t vaddr, std::uint64_t size) {
  for (const Elf64_Phdr& ph : elf.phdrs) {
    if (ph.p_type != PT_LOAD) continue;
    if (vaddr >= ph.p_vaddr + ph.p_filesz && vaddr + size <= ph.p_vaddr + ph.p_memsz) return true;
  }
  return false;
}

// The section named `name`, from the section headers.
bool Section(const LoadedElf& elf, const char* name, Elf64_Shdr* out) {
  const std::vector<char>& b = elf.bytes;
  Elf64_Ehdr eh;
  std::memcpy(&eh, b.data(), sizeof(eh));
  if (eh.e_shoff == 0 || eh.e_shoff + eh.e_shnum * sizeof(Elf64_Shdr) > b.size() || eh.e_shstrndx >= eh.e_shnum) return false;
  std::vector<Elf64_Shdr> sh(eh.e_shnum);
  std::memcpy(sh.data(), b.data() + eh.e_shoff, eh.e_shnum * sizeof(Elf64_Shdr));
  const Elf64_Shdr& names = sh[eh.e_shstrndx];
  for (const Elf64_Shdr& s : sh) {
    if (s.sh_name < names.sh_size && std::strcmp(b.data() + names.sh_offset + s.sh_name, name) == 0) {
      *out = s;
      return true;
    }
  }
  return false;
}

bool DynamicSymbol(const LoadedElf& elf, const char* symbol, Elf64_Sym* out) {
  const std::vector<char>& b = elf.bytes;
  Elf64_Ehdr eh;
  std::memcpy(&eh, b.data(), sizeof(eh));
  if (eh.e_shoff == 0 || eh.e_shoff + eh.e_shnum * sizeof(Elf64_Shdr) > b.size()) return false;
  std::vector<Elf64_Shdr> sh(eh.e_shnum);
  std::memcpy(sh.data(), b.data() + eh.e_shoff, eh.e_shnum * sizeof(Elf64_Shdr));
  for (const Elf64_Shdr& s : sh) {
    if (s.sh_type != SHT_DYNSYM || s.sh_link >= sh.size()) continue;
    const Elf64_Shdr& str = sh[s.sh_link];
    for (std::uint64_t off = 0; off + sizeof(Elf64_Sym) <= s.sh_size; off += sizeof(Elf64_Sym)) {
      Elf64_Sym sym;
      std::memcpy(&sym, b.data() + s.sh_offset + off, sizeof(sym));
      if (sym.st_name < str.sh_size && std::strcmp(b.data() + str.sh_offset + sym.st_name, symbol) == 0) {
        *out = sym;
        return true;
      }
    }
  }
  return false;
}

std::uint64_t AdrpPage(std::uint32_t word, std::uint64_t pc) {
  std::int64_t imm = (static_cast<std::int64_t>((word >> 5) & 0x7ffffU) << 2) | static_cast<std::int64_t>((word >> 29) & 3U);
  if ((imm & (1 << 20)) != 0) imm -= (1 << 21);
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(pc & ~static_cast<std::uint64_t>(0xfff)) + (imm << 12));
}
bool IsAdrp(std::uint32_t word) { return (word & 0x9f000000U) == 0x90000000U; }
bool IsBl(std::uint32_t word) { return (word & 0xfc000000U) == 0x94000000U; }
std::uint64_t BlTarget(std::uint32_t word, std::uint64_t pc) {
  std::int64_t imm = static_cast<std::int64_t>(word & 0x03ffffffU);
  if ((imm & (1 << 25)) != 0) imm -= (1 << 26);
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(pc) + imm * 4);
}
unsigned Rd(std::uint32_t word) { return word & 0x1fU; }
unsigned Rn(std::uint32_t word) { return (word >> 5) & 0x1fU; }

bool CheckWord(const LoadedElf& elf, const cp::Word& w) {
  std::uint32_t have = 0;
  const bool readable = WordAt(elf, w.vaddr, &have);
  if (!readable || have != w.word) {
    std::fprintf(stderr, "word mismatch: %s va=0x%llx expected=0x%08x actual=0x%08x\n", w.what, static_cast<unsigned long long>(w.vaddr), w.word,
                 have);
    return false;
  }
  return true;
}

std::uint32_t PinnedWordAt(std::uint64_t vaddr) {
  for (const cp::Word& w : ffe::kWords) {
    if (w.vaddr == vaddr) return w.word;
  }
  QCHECK(false);  // every address the tables name is a pinned word
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: frozen_frame_exit_pinned_test <libr15.so>\n");
    return 2;
  }
  LoadedElf elf;
  if (!Load(argv[1], &elf)) {
    std::fprintf(stderr, "frozen_frame_exit_pinned_test: cannot load %s as an AArch64 ELF\n", argv[1]);
    return 2;
  }

  // The store file: the build ID the pins were measured on.
  char buildId[64] = {};
  QCHECK(ReadBuildId(elf.image, buildId, sizeof(buildId)));
  QCHECK(std::strcmp(buildId, pinned::kLibR15StockBuildId) == 0);

  // Every named word is the file's word at its address.
  for (const cp::Word& w : ffe::kWords) QCHECK(CheckWord(elf, w));

  // The in-VR word is in .bss: the section table's .bss is the pinned range, it holds the word, and the file has no bytes there.
  Elf64_Shdr bss{};
  QCHECK(Section(elf, ".bss", &bss));
  QCHECK(bss.sh_type == SHT_NOBITS && bss.sh_addr == ffe::kBssVaddr && bss.sh_size == ffe::kBssBytes);
  QCHECK(ffe::kInVrWordVaddr >= ffe::kBssVaddr && ffe::kInVrWordVaddr + ffe::kInVrWordBytes <= ffe::kBssVaddr + ffe::kBssBytes);
  QCHECK(InZeroFill(elf, ffe::kInVrWordVaddr, ffe::kInVrWordBytes));
  QCHECK(ffe::kInVrWordVaddr % 8 == 0);  // an aligned 8-byte word: one load reads it whole

  // Each adrp and the add, ldr or str after it name the in-VR word, and the second uses the register the adrp set.
  for (const ffe::Address& a : ffe::kAddresses) {
    const std::uint32_t page = PinnedWordAt(a.page);
    const std::uint32_t low = PinnedWordAt(a.low);
    QCHECK(IsAdrp(page));
    QCHECK(Rn(low) == Rd(page));
    const std::uint64_t offset = a.viaAdd ? cp::AddImmediate(low) : cp::LdrXOffset(low);
    const std::uint64_t names = AdrpPage(page, a.page) + offset;
    if (names != ffe::kInVrWordVaddr) {
      std::fprintf(stderr, "adrp 0x%llx + 0x%llx names 0x%llx, pinned 0x%llx\n", static_cast<unsigned long long>(a.page),
                   static_cast<unsigned long long>(a.low), static_cast<unsigned long long>(names),
                   static_cast<unsigned long long>(ffe::kInVrWordVaddr));
    }
    QCHECK(names == ffe::kInVrWordVaddr);
  }
  // The writer stores x0 (EnterVrMode's result) at [x24 + 0]: x24 is the register the add filled, the store is the word right after the call.
  QCHECK(Rd(PinnedWordAt(0x1a71bac)) == Rn(PinnedWordAt(0x1a71c88)) && Rd(PinnedWordAt(0x1a71c88)) == 0 && cp::LdrXOffset(PinnedWordAt(0x1a71c88)) == 0);
  QCHECK(ffe::kCalls[0].site + 4 == 0x1a71c88);
  // Each clear stores xzr (register 31), right after a LeaveVrMode call.
  QCHECK(Rd(PinnedWordAt(0x1a71cb4)) == 31 && ffe::kCalls[1].site + 4 == 0x1a71cb4);
  QCHECK(Rd(PinnedWordAt(0x1a712d0)) == 31 && ffe::kCalls[2].site + 4 == 0x1a712d0);
  // The UpdateTracking clear is reached when SessionActive's result is zero: the cbz tests w0 right after the call and jumps to the load of the word.
  const std::uint32_t cbz = PinnedWordAt(0x1a71c3c);
  QCHECK((cbz & 0xff000000U) == 0x34000000U && Rd(cbz) == 0);
  const std::int64_t cbzImm = static_cast<std::int64_t>((cbz >> 5) & 0x7ffffU) - (((cbz >> 23) & 1U) != 0 ? (1 << 19) : 0);
  QCHECK(static_cast<std::uint64_t>(0x1a71c3c + cbzImm * 4) == 0x1a71ca8);
  QCHECK(ffe::kCalls[3].site + 4 == 0x1a71c3c);
  // The second way out: when SessionActive is non-zero the next call is CSysJava::NativeWindow, and a null window (`cbz x0`) jumps to the same load of the
  // word. The call follows the SessionActive cbz, the cbz follows the call, and both cbz reach 0x1a71ca8.
  QCHECK(ffe::kCalls[4].site == 0x1a71c3c + 4 && ffe::kCalls[4].site + 4 == 0x1a71c44);
  const std::uint32_t cbzWindow = PinnedWordAt(0x1a71c44);
  QCHECK((cbzWindow & 0xff000000U) == 0xb4000000U && Rd(cbzWindow) == 0);
  const std::int64_t cbzWindowImm = static_cast<std::int64_t>((cbzWindow >> 5) & 0x7ffffU) - (((cbzWindow >> 23) & 1U) != 0 ? (1 << 19) : 0);
  QCHECK(static_cast<std::uint64_t>(0x1a71c44 + cbzWindowImm * 4) == 0x1a71ca8);

  // Each call reaches a PLT stub (adrp x16; ldr x17,[x16,#off]) whose slot is the JUMP_SLOT of the named symbol.
  for (const ffe::Call& c : ffe::kCalls) {
    QCHECK(CheckWord(elf, cp::Word{c.what, c.site, c.word}));
    QCHECK(PinnedWordAt(c.site) == c.word);
    QCHECK(IsBl(c.word) && BlTarget(c.word, c.site) == c.stub);
    std::uint32_t adrp = 0;
    std::uint32_t ldr = 0;
    QCHECK(WordAt(elf, c.stub, &adrp));
    QCHECK(WordAt(elf, c.stub + 4, &ldr));
    QCHECK(IsAdrp(adrp) && Rd(adrp) == 16);
    QCHECK((ldr & 0xffc003ffU) == 0xf9400211U);  // ldr x17,[x16,#imm]
    QCHECK(AdrpPage(adrp, c.stub) + cp::LdrXOffset(ldr) == c.slot);
    const GotTarget target{pinned::kLibR15, c.symbol, RelocKind::kJumpSlot, pinned::kLibR15StockBuildId, c.slot};
    const SlotResolution slot = ResolveSlot(elf.image, target, kAarch64Relocs);
    QCHECK_STATUS(slot.status, GotStatus::kOk);
    QCHECK(slot.slotVaddr == c.slot);
  }

  // The two functions are their .dynsym symbols, and each pinned word lies inside the one it is named for.
  for (const cp::Symbol& s : {ffe::kUpdateTracking, ffe::kShutdownGraphics}) {
    Elf64_Sym sym{};
    const bool found = DynamicSymbol(elf, s.symbol, &sym);
    QCHECK(found && ELF64_ST_TYPE(sym.st_info) == STT_FUNC && sym.st_value == s.vaddr);
    for (const cp::Word& w : ffe::kWords) {
      const bool named = std::strncmp(w.what, s.symbol == ffe::kUpdateTracking.symbol ? "UpdateTracking:" : "ShutdownGraphics:",
                                      s.symbol == ffe::kUpdateTracking.symbol ? 15 : 17) == 0;
      if (named) QCHECK(found && w.vaddr >= sym.st_value && w.vaddr + 4 <= sym.st_value + sym.st_size);
    }
  }

  if (quest_test::Failures() == 0) std::printf("frozen_frame_exit_pinned_test: ok (%s)\n", buildId);
  return quest_test::Failures() == 0 ? 0 : 1;
}
