// Resolves the bullet-velocity guard's pins (src/quest/combat/bullet_velocity_guard_targets.h) against the real libr15.so, on any host. The ELF is
// laid out in memory the way a loader would (PT_LOAD segments copied to their vaddrs inside one anonymous mapping), so the production ResolveSlot
// walks the real relocation tables with link-time-relative pointers. Nothing is executed or patched. FireBullet's slot is dronemuzzle's pin
// (dronemuzzle_targets.h), checked here through the same values, not repinned.
//
// Run: bullet_velocity_guard_pinned_test <libr15.so>

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
#include "quest/combat/bullet_velocity_guard_targets.h"
#include "quest/combat/dronemuzzle_targets.h"
#include "quest/tests/test_check.h"

namespace {

using sentinel::GotStatus;
using sentinel::GotTarget;
using sentinel::RelocKind;
using sentinel::SlotResolution;
using sentinel::kAarch64Relocs;
using sentinel::ResolveSlot;
namespace combat_pinned = nevr_quest::combat::pinned;
namespace bv = nevr_quest::combat::bullet_velocity_guard;
namespace dm = nevr_quest::combat::dronemuzzle;

struct LoadedElf {
  std::vector<char> bytes;  // the file, for the section and dynamic symbol tables
  void* mem = nullptr;
  std::size_t span = 0;
  std::vector<Elf64_Phdr> phdrs;
  sentinel::ElfImage image;
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

// The .dynsym entry of `symbol` in the file, false when absent.
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

void CheckBuildId(const LoadedElf& elf, const char* expected) {
  char actual[64] = {};
  QCHECK(sentinel::ReadBuildId(elf.image, actual, sizeof(actual)));
  QCHECK(std::strcmp(actual, expected) == 0);
}

// The slot of `symbol`'s relocation of `kind`, pinned at `slot`: it resolves there, the symbol and kind alone name the same slot, and a pin off by
// one word is refused.
void CheckSlotPin(const LoadedElf& elf, const char* symbol, RelocKind kind, std::uint64_t slot, const char* what) {
  const GotTarget pinnedTarget{sentinel::pinned::kLibR15, symbol, kind, sentinel::pinned::kLibR15StockBuildId, slot};
  const SlotResolution at = ResolveSlot(elf.image, pinnedTarget, kAarch64Relocs);
  if (at.status != GotStatus::kOk) std::fprintf(stderr, "target: %s\n", what);
  QCHECK_STATUS(at.status, GotStatus::kOk);
  QCHECK(at.slotVaddr == slot);
  QCHECK(at.restoreReadOnly);  // every pinned slot is inside PT_GNU_RELRO

  GotTarget alone = pinnedTarget;
  alone.slotVaddr = std::nullopt;
  const SlotResolution aloneAt = ResolveSlot(elf.image, alone, kAarch64Relocs);
  QCHECK_STATUS(aloneAt.status, GotStatus::kOk);
  QCHECK(aloneAt.slotVaddr == at.slotVaddr);

  GotTarget shifted = pinnedTarget;
  shifted.slotVaddr = slot + 8;
  QCHECK_STATUS(ResolveSlot(elf.image, shifted, kAarch64Relocs).status, GotStatus::kSlotOffsetMismatch);
}

void CheckDefinedAt(const LoadedElf& elf, const char* symbol, std::uint64_t vaddr) {
  Elf64_Sym sym;
  if (!DynamicSymbol(elf, symbol, &sym) || sym.st_value != vaddr) {
    std::fprintf(stderr, "%s: .dynsym value is not %#llx\n", symbol, static_cast<unsigned long long>(vaddr));
    QCHECK(false);
  }
}

// Every code word of the list is the file's word at its address; names the first one that is not.
void CheckWords(const LoadedElf& elf, const combat_pinned::Pins& pins, const char* what) {
  combat_pinned::Failure failure;
  if (combat_pinned::FirstFailure(reinterpret_cast<std::uintptr_t>(elf.mem), &pins, 1, &failure)) {
    std::fprintf(stderr, "libr15 %s word '%s' at %#llx is %#llx, pinned %#llx (readable %d)\n", what, failure.what,
                 static_cast<unsigned long long>(failure.vaddr), static_cast<unsigned long long>(failure.have),
                 static_cast<unsigned long long>(failure.want), failure.readable ? 1 : 0);
    QCHECK(false);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: bullet_velocity_guard_pinned_test <libr15.so>\n");
    return 2;
  }
  LoadedElf r15;
  if (!Load(argv[1], &r15)) {
    std::fprintf(stderr, "bullet_velocity_guard_pinned_test: cannot load an AArch64 ELF64 from %s\n", argv[1]);
    return 2;
  }
  CheckBuildId(r15, sentinel::pinned::kLibR15StockBuildId);

  // CR15NetBulletCS::FireBullet: the slot and the function are dronemuzzle's pins; this program checks them against the file.
  CheckSlotPin(r15, dm::kFireSymbol, RelocKind::kJumpSlot, dm::kPins.fireSlot, "libr15 CR15NetBulletCS::FireBullet JUMP_SLOT");
  CheckDefinedAt(r15, dm::kFireSymbol, dm::kPins.fireFn);

  // The normalise the guard relies on: 1.0 / |v| with no test on |v| or on the components.
  CheckWords(r15, bv::kWordPins, "FireBullet normalise");

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "bullet_velocity_guard_pinned_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("bullet_velocity_guard_pinned_test: FireBullet's slot and the normalise words hold in the real libr15.so\n");
  return 0;
}
