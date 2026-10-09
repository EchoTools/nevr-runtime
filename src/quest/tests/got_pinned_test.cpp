// Resolves the pinned Quest targets (src/quest/sentinel/pinned_targets.h) against
// the real libr15.so and libpnsradmatchmaking.so, on any host.
//
// The ELF is laid out in memory the way a loader would (PT_LOAD segments copied to
// their vaddrs inside one anonymous mapping), so the production ResolveSlot walks
// the real dynamic section, 100k-entry relocation tables and string table with
// link-time-relative pointers, exactly as on Bionic. Nothing is executed or
// patched, and relocation numbers are passed as AArch64's, so an x86_64 host can
// check an AArch64 image.
//
// Run: got_pinned_test <libr15.so> <libpnsradmatchmaking.so>

#include <elf.h>
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "got_hook.h"
#include "pinned_targets.h"
#include "quest/tests/test_check.h"

namespace {

using namespace sentinel;

struct LoadedElf {
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

void CheckTarget(const LoadedElf& elf, const GotTarget& pinnedTarget, const char* what) {
  const SlotResolution pinned = ResolveSlot(elf.image, pinnedTarget, kAarch64Relocs);
  if (pinned.status != GotStatus::kOk) std::fprintf(stderr, "target: %s\n", what);
  QCHECK_STATUS(pinned.status, GotStatus::kOk);
  QCHECK(pinned.slotVaddr == *pinnedTarget.slotVaddr);
  QCHECK(pinned.restoreReadOnly);  // every pinned slot is inside PT_GNU_RELRO

  // The symbol and relocation type alone name exactly one slot: no pin needed.
  GotTarget unpinned = pinnedTarget;
  unpinned.slotVaddr = std::nullopt;
  const SlotResolution alone = ResolveSlot(elf.image, unpinned, kAarch64Relocs);
  QCHECK_STATUS(alone.status, GotStatus::kOk);
  QCHECK(alone.slotVaddr == pinned.slotVaddr);

  // The other relocation type for the same symbol does not exist.
  GotTarget otherKind = unpinned;
  otherKind.kind = unpinned.kind == RelocKind::kJumpSlot ? RelocKind::kGlobDat : RelocKind::kJumpSlot;
  QCHECK_STATUS(ResolveSlot(elf.image, otherKind, kAarch64Relocs).status, GotStatus::kWrongRelocationType);

  // A pin off by one slot is refused.
  GotTarget shifted = pinnedTarget;
  shifted.slotVaddr = *pinnedTarget.slotVaddr + 8;
  QCHECK_STATUS(ResolveSlot(elf.image, shifted, kAarch64Relocs).status, GotStatus::kSlotOffsetMismatch);
}

void CheckBuildId(const LoadedElf& elf, const char* expected) {
  char actual[64] = {};
  QCHECK(ReadBuildId(elf.image, actual, sizeof(actual)));
  QCHECK(std::strcmp(actual, expected) == 0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: got_pinned_test <libr15.so> <libpnsradmatchmaking.so>\n");
    return 2;
  }
  LoadedElf r15, mm;
  if (!Load(argv[1], &r15) || !Load(argv[2], &mm)) {
    std::fprintf(stderr, "got_pinned_test: cannot load an AArch64 ELF64 from the given paths\n");
    return 2;
  }
  CheckBuildId(r15, pinned::kLibR15BuildId);
  CheckBuildId(mm, pinned::kMatchmakingBuildId);

  CheckTarget(r15, pinned::LibR15ClockGettime(), "libr15 clock_gettime JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15TString(), "libr15 CJson::TString JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15ConfigRequestSend(), "libr15 SNSConfigRequestv24Send JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15SetDelimitedErrorMessage(), "libr15 CR15NetGame::SetDelimitedErrorMessage JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15BindNodeGlobDat(), "libr15 BindNode GLOB_DAT");
  CheckTarget(r15, pinned::LibR15LookupDataBindingGlobDat(), "libr15 LookupDataBinding GLOB_DAT");
  CheckTarget(mm, pinned::MatchmakingTString(), "libpnsradmatchmaking CJson::TString JUMP_SLOT");

  // The matchmaking library's slot for the other CJson::TString overload is a
  // different symbol and a different slot.
  GotTarget symbolOverload{pinned::kMatchmaking, "_ZNK10NRadEngine5CJson7TStringENS_9CSymbol64EPKc",
                           RelocKind::kJumpSlot};
  const SlotResolution overload = ResolveSlot(mm.image, symbolOverload, kAarch64Relocs);
  QCHECK_STATUS(overload.status, GotStatus::kOk);
  QCHECK(overload.slotVaddr == 0x6bb458ULL);

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "got_pinned_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("got_pinned_test: all pinned targets resolve in the real ELFs\n");
  return 0;
}
