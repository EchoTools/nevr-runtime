// Resolves the integration's dlopen target (src/quest/integration/dlopen_hook.h, LibR15Dlopen) against
// the real libr15.so, on any host. The ELF is laid out in memory the way a loader would (see
// got_pinned_test.cpp), so the production ResolveSlot walks the real dynamic section and relocation
// tables. Nothing is executed or patched.
//
// Run: integration_pinned_test <libr15.so>

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
#include "quest/integration/dlopen_hook.h"
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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: integration_pinned_test <libr15.so>\n");
    return 2;
  }
  LoadedElf r15;
  if (!Load(argv[1], &r15)) {
    std::fprintf(stderr, "integration_pinned_test: cannot load an AArch64 ELF64 from %s\n", argv[1]);
    return 2;
  }
  char buildId[64] = {};
  QCHECK(ReadBuildId(r15.image, buildId, sizeof(buildId)));
  QCHECK(std::strcmp(buildId, pinned::kLibR15BuildId) == 0);

  const GotTarget target = nevr_quest::integration::LibR15Dlopen();
  const SlotResolution pinnedRes = ResolveSlot(r15.image, target, kAarch64Relocs);
  QCHECK_STATUS(pinnedRes.status, GotStatus::kOk);
  QCHECK(pinnedRes.slotVaddr == 0x36c6380ULL);

  // The symbol and relocation type alone name exactly one slot (libr15 has one dlopen reference).
  GotTarget unpinned = target;
  unpinned.slotVaddr = std::nullopt;
  const SlotResolution alone = ResolveSlot(r15.image, unpinned, kAarch64Relocs);
  QCHECK_STATUS(alone.status, GotStatus::kOk);
  QCHECK(alone.slotVaddr == pinnedRes.slotVaddr);

  // A pin off by one slot is refused.
  GotTarget shifted = target;
  shifted.slotVaddr = *target.slotVaddr + 8;
  QCHECK_STATUS(ResolveSlot(r15.image, shifted, kAarch64Relocs).status, GotStatus::kSlotOffsetMismatch);

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "integration_pinned_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("integration_pinned_test: the dlopen slot resolves in the real libr15.so\n");
  return 0;
}
