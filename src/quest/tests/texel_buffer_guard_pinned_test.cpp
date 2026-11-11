// The pins of src/quest/combat/texel_buffer_guard_targets.h, checked against the store libr15 (the hooks-pinned group).
// Each word is read from the file at its ELF address and compared with its encoding; the gctx word is a GLOB_DAT relocation for the
// gctx object; the object is gContext's .dynsym object and lies in .bss; the gctx load's adrp and ldr name the word; every slot offset agrees with the store instruction that fills it.
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
#include "quest/combat/pinned_words.h"
#include "quest/combat/texel_buffer_guard_targets.h"
#include "quest/sentinel/got_hook.h"
#include "quest/tests/test_check.h"

namespace {

using namespace sentinel;
namespace tb = nevr_quest::combat::texel_buffer_guard::targets;
using nevr_quest::combat::pinned::LdrXOffset;

struct LoadedElf {
  std::vector<char> bytes;  // the file, for the program headers
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
  if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_machine != EM_AARCH64 || eh.e_ident[EI_CLASS] != ELFCLASS64) {
    return false;
  }
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

// The 4 bytes at a virtual address of the store file.
bool WordAt(const LoadedElf& elf, std::uint64_t vaddr, std::uint32_t* out) {
  if (elf.mem == nullptr || vaddr + sizeof(*out) > elf.span) return false;
  std::memcpy(out, static_cast<const char*>(elf.mem) + vaddr, sizeof(*out));
  return true;
}

// The object [vaddr, vaddr + size) lies in the part of a PT_LOAD segment that the file does not fill (.bss).
bool InBss(const LoadedElf& elf, std::uint64_t vaddr, std::uint64_t size) {
  for (const Elf64_Phdr& ph : elf.phdrs) {
    if (ph.p_type != PT_LOAD) continue;
    if (vaddr >= ph.p_vaddr + ph.p_filesz && vaddr + size <= ph.p_vaddr + ph.p_memsz) return true;
  }
  return false;
}

// The .dynsym entry of `symbol`, or false when absent.
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

// The page an `adrp` at pc names: (pc & ~0xfff) + immhi:immlo << 12, immediate sign-extended.
std::uint64_t AdrpPage(std::uint32_t word, std::uint64_t pc) {
  std::int64_t imm = (static_cast<std::int64_t>((word >> 5) & 0x7ffffU) << 2) | static_cast<std::int64_t>((word >> 29) & 3U);
  if ((imm & (1 << 20)) != 0) imm -= (1 << 21);
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(pc & ~static_cast<std::uint64_t>(0xfff)) + (imm << 12));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: texel_buffer_guard_pinned_test <libr15.so>\n");
    return 2;
  }
  LoadedElf elf;
  if (!Load(argv[1], &elf)) {
    std::fprintf(stderr, "texel_buffer_guard_pinned_test: cannot load %s as an AArch64 ELF\n", argv[1]);
    return 2;
  }

  // The store file: the build id the pins were measured on.
  char buildId[64] = {};
  QCHECK(ReadBuildId(elf.image, buildId, sizeof(buildId)));
  QCHECK(std::strcmp(buildId, pinned::kLibR15StockBuildId) == 0);

  // The gctx word is R_AARCH64_GLOB_DAT for NRadEngine::gContext (the table pointer that CGS::Initialize loads).
  const GotTarget gctx{pinned::kLibR15, tb::kGctxSymbol, RelocKind::kGlobDat, pinned::kLibR15StockBuildId, tb::kGctxWordVaddr};
  const SlotResolution gctxSlot = ResolveSlot(elf.image, gctx, kAarch64Relocs);
  QCHECK_STATUS(gctxSlot.status, GotStatus::kOk);
  QCHECK(gctxSlot.slotVaddr == tb::kGctxWordVaddr);

  // The object the gctx word names is the .dynsym object of the symbol: its address and its size are the pinned ones.
  Elf64_Sym gctxSym;
  QCHECK(DynamicSymbol(elf, tb::kGctxSymbol, &gctxSym));
  if (gctxSym.st_value != tb::kGctxObjectVaddr || gctxSym.st_size != tb::kGctxObjectSize) {
    std::fprintf(stderr, "gContext .dynsym is %#llx size %#llx, pinned %#llx size %#llx\n", static_cast<unsigned long long>(gctxSym.st_value),
                 static_cast<unsigned long long>(gctxSym.st_size), static_cast<unsigned long long>(tb::kGctxObjectVaddr),
                 static_cast<unsigned long long>(tb::kGctxObjectSize));
  }
  QCHECK(ELF64_ST_TYPE(gctxSym.st_info) == STT_OBJECT);
  QCHECK(gctxSym.st_value == tb::kGctxObjectVaddr);
  QCHECK(gctxSym.st_size == tb::kGctxObjectSize);

  // The object the word names lies in .bss: the file holds no value for it, the fill writes it at run time.
  QCHECK(InBss(elf, tb::kGctxObjectVaddr, tb::kGctxObjectSize));
  // The device word and every slot the guard uses lie inside the object.
  QCHECK(tb::kSlotDevice + sizeof(std::uint64_t) <= tb::kGctxObjectSize);
  QCHECK(tb::kSlotVkCreateDescriptorSetLayout + sizeof(std::uint64_t) <= tb::kGctxObjectSize);
  QCHECK(tb::kSlotVkAllocateDescriptorSets + sizeof(std::uint64_t) <= tb::kGctxObjectSize);
  QCHECK(tb::kSlotVkUpdateDescriptorSets + sizeof(std::uint64_t) <= tb::kGctxObjectSize);

  // Every named word is the file's word at its address.
  for (const nevr_quest::combat::pinned::Word& w : tb::kWords) {
    std::uint32_t have = 0;
    const bool readable = WordAt(elf, w.vaddr, &have);
    if (!readable || have != w.word) {
      std::fprintf(stderr, "word mismatch: %s va=0x%llx expected=0x%08x actual=0x%08x\n", w.what,
                   static_cast<unsigned long long>(w.vaddr), w.word, have);
    }
    QCHECK(readable);
    QCHECK(have == w.word);
  }

  // Each slot offset is the offset its fill store writes (`str x0,[x29,#off]`, the same imm12 field as `ldr`).
  QCHECK(LdrXOffset(tb::kFillCreateDescriptorSetLayout.word) == tb::kSlotVkCreateDescriptorSetLayout);
  QCHECK(LdrXOffset(tb::kFillAllocateDescriptorSets.word) == tb::kSlotVkAllocateDescriptorSets);
  QCHECK(LdrXOffset(tb::kFillUpdateDescriptorSets.word) == tb::kSlotVkUpdateDescriptorSets);
  // The gctx base load is `adrp x29,page; ldr x29,[x29,#off]`: page plus offset is the GOT word whose relocation was checked above.
  QCHECK((tb::kLoadGctxPage.word & 0x9f000000U) == 0x90000000U);
  QCHECK((tb::kLoadGctxPage.word & 0x1fU) == 29);
  QCHECK(AdrpPage(tb::kLoadGctxPage.word, tb::kLoadGctxPage.vaddr) + LdrXOffset(tb::kLoadGctxBase.word) == tb::kGctxWordVaddr);

  if (quest_test::Failures() == 0) std::printf("texel_buffer_guard_pinned_test: ok (%s)\n", buildId);
  return quest_test::Failures() == 0 ? 0 : 1;
}
