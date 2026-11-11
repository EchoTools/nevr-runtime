// Resolves the outline-mode module's pinned slots (src/quest/combat/outline_mode_targets.h) against the real libr15.so: the two
// SetOutlineMode JUMP_SLOTs, each one's function as the file's .dynsym gives it, and the store's build ID. Nothing is executed or
// patched. Run by `just test-quest-hooks-pinned`, which passes the store libr15.so.
//
// Run: outline_mode_pinned_test <libr15.so>

#include <elf.h>
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

#include "got_hook.h"
#include "pinned_targets.h"
#include "quest/combat/outline_mode_targets.h"
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
    std::fprintf(stderr, "outline_mode_pinned_test: %s: %s\n", what, GotStatusName(pinned.status));
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
    std::fprintf(stderr, "outline_mode_pinned_test: %s: .dynsym value %#llx, pinned %#llx\n", what,
                 static_cast<unsigned long long>(value), static_cast<unsigned long long>(fn));
  }
  QCHECK(value == fn);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: outline_mode_pinned_test <libr15.so>\n");
    return 2;
  }
  LoadedElf r15;
  if (!Load(argv[1], &r15)) {
    std::fprintf(stderr, "outline_mode_pinned_test: cannot load an AArch64 ELF64 from %s\n", argv[1]);
    return 2;
  }
  namespace ot = nevr_quest::combat::outline_mode::targets;
  CheckBuildId(r15, pinned::kLibR15StockBuildId);
  CheckJumpSlot(r15, ot::SceneTarget(), ot::kPins.sceneFn, "CGScene::SetOutlineMode");
  CheckJumpSlot(r15, ot::DrawSetTarget(), ot::kPins.drawSetFn, "CGDrawDataSet::SetOutlineMode");

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "outline_mode_pinned_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("outline_mode_pinned_test: the outline-mode slots resolve in the real libr15\n");
  return 0;
}
