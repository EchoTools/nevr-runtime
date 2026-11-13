// Resolves the chassis-fork module's pinned sites (src/quest/combat/chassis_fork_targets.h) against the real libr15.so: the
// BeginModelSwap JUMP_SLOT and its vtable word, the LookupResourcePath JUMP_SLOT, BeginModelSwap's prologue words, and the
// resource manager word in .bss with the four instructions that store to it. Nothing is executed or patched. Run by
// `just test-quest-hooks-pinned`, which passes the store libr15.so.
//
// Run: chassis_fork_pinned_test <libr15.so>

#include <elf.h>
#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

#include "got_hook.h"
#include "pinned_targets.h"
#include "quest/combat/chassis_fork_targets.h"
#include "quest/tests/test_check.h"

namespace {

using namespace sentinel;

struct LoadedElf {
  std::vector<char> bytes;  // the file, for the section headers
  void* mem = nullptr;
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
  if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_machine != EM_AARCH64 ||
      eh.e_ident[EI_CLASS] != ELFCLASS64) {
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

// The value .dynsym gives `symbol` (its link-time address), or 0 when absent.
std::uint64_t DynamicSymbolValue(const LoadedElf& elf, const char* symbol) {
  Elf64_Sym sym;
  return DynamicSymbol(elf, symbol, &sym) ? sym.st_value : 0;
}

// The build ID of the file is the store's, the one pinned_targets.h names.
void CheckBuildId(const LoadedElf& elf, const char* expected) {
  char actual[64] = {};
  QCHECK(ReadBuildId(elf.image, actual, sizeof(actual)));
  QCHECK(std::strcmp(actual, expected) == 0);
}

// One JUMP_SLOT pin, checked the way got_pinned_test checks its targets: the pinned slot resolves and is in RELRO, the symbol
// alone names the same slot, the other relocation kind for the symbol is refused, a pin one word off is refused, and the
// symbol's .dynsym value is the pinned function.
void CheckJumpSlot(const LoadedElf& elf, const GotTarget& pinnedTarget, std::uint64_t fn, const char* what) {
  const SlotResolution pinned = ResolveSlot(elf.image, pinnedTarget, kAarch64Relocs);
  if (pinned.status != GotStatus::kOk) {
    std::fprintf(stderr, "chassis_fork_pinned_test: %s: %s\n", what, GotStatusName(pinned.status));
  }
  QCHECK_STATUS(pinned.status, GotStatus::kOk);
  QCHECK(pinned.slotVaddr == *pinnedTarget.slotVaddr);
  QCHECK(pinned.restoreReadOnly);

  GotTarget unpinned = pinnedTarget;
  unpinned.slotVaddr = std::nullopt;
  const SlotResolution alone = ResolveSlot(elf.image, unpinned, kAarch64Relocs);
  QCHECK_STATUS(alone.status, GotStatus::kOk);
  QCHECK(alone.slotVaddr == pinned.slotVaddr);

  GotTarget otherKind = unpinned;
  otherKind.kind = RelocKind::kGlobDat;
  QCHECK_STATUS(ResolveSlot(elf.image, otherKind, kAarch64Relocs).status, GotStatus::kWrongRelocationType);

  GotTarget shifted = pinnedTarget;
  shifted.slotVaddr = *pinnedTarget.slotVaddr + 8;
  QCHECK_STATUS(ResolveSlot(elf.image, shifted, kAarch64Relocs).status, GotStatus::kSlotOffsetMismatch);

  const std::uint64_t value = DynamicSymbolValue(elf, pinnedTarget.symbol);
  if (value != fn) {
    std::fprintf(stderr, "chassis_fork_pinned_test: %s: .dynsym value %#llx, pinned %#llx\n", what,
                 static_cast<unsigned long long>(value), static_cast<unsigned long long>(fn));
  }
  QCHECK(value == fn);
}

// The vtable word of BeginModelSwap: an ABS64 relocation, which the production rule says must be pinned by its address (an
// unpinned lookup is refused as kPinRequired). The pinned word resolves to the pinned function's .dynsym value.
void CheckAbs64Word(const LoadedElf& elf, const GotTarget& pinnedTarget, std::uint64_t fn, const char* what) {
  const SlotResolution pinned = ResolveSlot(elf.image, pinnedTarget, kAarch64Relocs);
  if (pinned.status != GotStatus::kOk) {
    std::fprintf(stderr, "chassis_fork_pinned_test: %s: %s\n", what, GotStatusName(pinned.status));
  }
  QCHECK_STATUS(pinned.status, GotStatus::kOk);
  QCHECK(pinned.slotVaddr == *pinnedTarget.slotVaddr);
  QCHECK(pinned.restoreReadOnly);

  GotTarget unpinned = pinnedTarget;
  unpinned.slotVaddr = std::nullopt;
  QCHECK_STATUS(ResolveSlot(elf.image, unpinned, kAarch64Relocs).status, GotStatus::kPinRequired);

  GotTarget shifted = pinnedTarget;
  shifted.slotVaddr = *pinnedTarget.slotVaddr + 8;
  QCHECK_STATUS(ResolveSlot(elf.image, shifted, kAarch64Relocs).status, GotStatus::kSlotOffsetMismatch);

  const std::uint64_t value = DynamicSymbolValue(elf, pinnedTarget.symbol);
  if (value != fn) {
    std::fprintf(stderr, "chassis_fork_pinned_test: %s: .dynsym value %#llx, pinned %#llx\n", what,
                 static_cast<unsigned long long>(value), static_cast<unsigned long long>(fn));
  }
  QCHECK(value == fn);
}

// The 32-bit word at `vaddr` in the file's bytes (the PT_LOAD segment that holds it), little-endian. False when no segment holds it.
bool FileWord(const LoadedElf& elf, std::uint64_t vaddr, std::uint32_t* out) {
  for (const Elf64_Phdr& ph : elf.phdrs) {
    if (ph.p_type != PT_LOAD || vaddr < ph.p_vaddr || vaddr + 4 > ph.p_vaddr + ph.p_filesz) continue;
    const auto* at = reinterpret_cast<const unsigned char*>(elf.bytes.data()) + ph.p_offset + (vaddr - ph.p_vaddr);
    *out = static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
           (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
    return true;
  }
  return false;
}

// The bounds and type of the section named `name`, from the file's own section header table.
bool SectionBounds(const LoadedElf& elf, const char* name, std::uint64_t* addr, std::uint64_t* size, std::uint32_t* type) {
  const std::vector<char>& b = elf.bytes;
  Elf64_Ehdr eh;
  std::memcpy(&eh, b.data(), sizeof(eh));
  if (eh.e_shoff == 0 || eh.e_shoff + eh.e_shnum * sizeof(Elf64_Shdr) > b.size() || eh.e_shstrndx >= eh.e_shnum) return false;
  std::vector<Elf64_Shdr> sh(eh.e_shnum);
  std::memcpy(sh.data(), b.data() + eh.e_shoff, eh.e_shnum * sizeof(Elf64_Shdr));
  const Elf64_Shdr& names = sh[eh.e_shstrndx];
  for (const Elf64_Shdr& s : sh) {
    if (s.sh_name >= names.sh_size) continue;
    if (std::strcmp(b.data() + names.sh_offset + s.sh_name, name) != 0) continue;
    *addr = s.sh_addr;
    *size = s.sh_size;
    *type = s.sh_type;
    return true;
  }
  return false;
}

// The resource manager word's writer: the four words of the file at the pinned addresses, decoded. The adrp names the page that
// x8 holds, the store writes through x8 at a 64-bit unsigned offset, and page plus offset is the pinned word, which lies in .bss.
void CheckResMgrWriter(const LoadedElf& elf) {
  namespace ct = nevr_quest::combat::chassis_fork::targets;
  std::uint32_t enc[ct::kResMgrWriterCount] = {};
  for (std::size_t i = 0; i < ct::kResMgrWriterCount; ++i) {
    std::uint32_t got = 0;
    QCHECK(FileWord(elf, ct::kResMgrWriter[i].vaddr, &got));
    if (got != ct::kResMgrWriter[i].word) {
      std::fprintf(stderr, "chassis_fork_pinned_test: writer word at %#llx is %#x, pinned %#x\n",
                   static_cast<unsigned long long>(ct::kResMgrWriter[i].vaddr), got, ct::kResMgrWriter[i].word);
    }
    QCHECK(got == ct::kResMgrWriter[i].word);
    enc[i] = got;
  }

  const std::uint32_t adrp = enc[0];
  QCHECK((adrp & 0x9f000000U) == 0x90000000U);  // ADRP
  const std::uint32_t rd = adrp & 0x1fU;
  QCHECK(rd == 8U);                              // x8
  std::int64_t imm = (static_cast<std::int64_t>((adrp >> 5) & 0x7ffffU) << 2) | static_cast<std::int64_t>((adrp >> 29) & 0x3U);
  if ((imm & (std::int64_t{1} << 20)) != 0) imm -= std::int64_t{1} << 21;
  const std::uint64_t page = (ct::kResMgrWriterAt & ~std::uint64_t{0xfff}) + static_cast<std::uint64_t>(imm * 4096);

  const std::uint32_t str = enc[3];
  QCHECK((str & 0xffc00000U) == 0xf9000000U);    // STR (unsigned immediate), 64-bit
  QCHECK(((str >> 5) & 0x1fU) == rd);            // through the adrp's register
  const std::uint64_t offset = static_cast<std::uint64_t>((str >> 10) & 0xfffU) * 8;
  if (page + offset != ct::kResMgrWord) {
    std::fprintf(stderr, "chassis_fork_pinned_test: the store at %#llx writes %#llx, pinned %#llx\n",
                 static_cast<unsigned long long>(ct::kResMgrWriter[3].vaddr), static_cast<unsigned long long>(page + offset),
                 static_cast<unsigned long long>(ct::kResMgrWord));
  }
  QCHECK(page + offset == ct::kResMgrWord);

  std::uint64_t addr = 0, size = 0;
  std::uint32_t type = 0;
  QCHECK(SectionBounds(elf, ct::kResMgrSection, &addr, &size, &type));
  QCHECK(type == SHT_NOBITS);  // .bss holds no file bytes: the word is zero in the file
  QCHECK(ct::kResMgrWord >= addr && ct::kResMgrWord + 8 <= addr + size);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: chassis_fork_pinned_test <libr15.so>\n");
    return 2;
  }
  LoadedElf r15;
  if (!Load(argv[1], &r15)) {
    std::fprintf(stderr, "chassis_fork_pinned_test: cannot load an AArch64 ELF64 from %s\n", argv[1]);
    return 2;
  }
  namespace ct = nevr_quest::combat::chassis_fork::targets;
  CheckBuildId(r15, pinned::kLibR15StockBuildId);
  CheckJumpSlot(r15, ct::SwapTarget(), ct::kPins.swapFn, "BeginModelSwap JUMP_SLOT");
  CheckAbs64Word(r15, ct::SwapVtableTarget(), ct::kPins.swapFn, "BeginModelSwap vtable word");
  CheckJumpSlot(r15, ct::LookupTarget(), ct::kPins.lookupFn, "LookupResourcePath JUMP_SLOT");
  for (const nevr_quest::combat::pinned::Word& w : ct::kSwapPrologue) {
    std::uint32_t got = 0;
    QCHECK(FileWord(r15, w.vaddr, &got));
    if (got != w.word) {
      std::fprintf(stderr, "chassis_fork_pinned_test: BeginModelSwap word at %#llx is %#x, pinned %#x\n",
                   static_cast<unsigned long long>(w.vaddr), got, w.word);
    }
    QCHECK(got == w.word);
  }
  CheckResMgrWriter(r15);

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "chassis_fork_pinned_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("chassis_fork_pinned_test: the chassis-fork sites resolve in the real libr15\n");
  return 0;
}
