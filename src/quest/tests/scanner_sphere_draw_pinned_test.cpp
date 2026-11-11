// The pins of src/quest/combat/scanner_sphere_draw_targets.h, checked against the store libr15 (the hooks-pinned group). The pool-11
// call of CGRenderPassMainAndroid::Render is decoded: its pool immediate, the `bl` target and the return address are the pinned ones;
// the stub's adrp and ldr name the JUMP_SLOT word, whose relocation names RenderDrawData<CGDrawData>; the gctx word and the blend slot
// are the ones the table pins (texel_buffer_guard_targets.h).
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
#include "quest/combat/scanner_sphere_draw_targets.h"
#include "quest/sentinel/got_hook.h"
#include "quest/tests/test_check.h"

namespace {

using namespace sentinel;
namespace sd = nevr_quest::combat::scanner_sphere_draw::targets;
using nevr_quest::combat::pinned::LdrXOffset;
using nevr_quest::combat::pinned::MovImmediate;

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

// The target of a `bl` at pc: pc + imm26 * 4, imm26 sign-extended.
std::uint64_t BlTarget(std::uint32_t word, std::uint64_t pc) {
  std::int64_t imm = static_cast<std::int64_t>(word & 0x3ffffffU);
  if ((imm & 0x2000000) != 0) imm -= 0x4000000;
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(pc) + imm * 4);
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
    std::fprintf(stderr, "usage: scanner_sphere_draw_pinned_test <libr15.so>\n");
    return 2;
  }
  LoadedElf elf;
  if (!Load(argv[1], &elf)) {
    std::fprintf(stderr, "scanner_sphere_draw_pinned_test: cannot load %s as an AArch64 ELF\n", argv[1]);
    return 2;
  }

  char buildId[64] = {};
  QCHECK(ReadBuildId(elf.image, buildId, sizeof(buildId)));
  QCHECK(std::strcmp(buildId, pinned::kLibR15StockBuildId) == 0);

  // Every named word is the file's word at its address.
  for (const nevr_quest::combat::pinned::Word& w : sd::kWords) {
    std::uint32_t have = 0;
    const bool readable = WordAt(elf, w.vaddr, &have);
    if (!readable || have != w.word) {
      std::fprintf(stderr, "word mismatch: %s va=0x%llx expected=0x%08x actual=0x%08x\n", w.what,
                   static_cast<unsigned long long>(w.vaddr), w.word, have);
    }
    QCHECK(readable);
    QCHECK(have == w.word);
  }

  // The pool-11 argument is `mov w2,#pool`: destination w2, immediate the pool.
  QCHECK((sd::kPool11ArgWord & 0x1fU) == 2);
  QCHECK(MovImmediate(sd::kPool11ArgWord) == sd::kPool11Pool);
  QCHECK(sd::kPool11Pool == 11);

  // The call is a `bl` to the stub, and it returns to the next instruction (the return site is the call plus four).
  QCHECK((sd::kPool11CallWord & 0xfc000000U) == 0x94000000U);
  QCHECK(BlTarget(sd::kPool11CallWord, sd::kPool11CallVaddr) == sd::kThunkVaddr);
  QCHECK(sd::kPool11ReturnVaddr == sd::kPool11CallVaddr + 4);

  // The stub: `adrp x16,page; ldr x17,[x16,#off]; br x17`. The adrp page plus the ldr offset is the JUMP_SLOT word.
  QCHECK((sd::kThunkAdrpWord & 0x9f000000U) == 0x90000000U);
  QCHECK(AdrpPage(sd::kThunkAdrpWord, sd::kThunkVaddr) + LdrXOffset(sd::kThunkLdrWord) == sd::kRenderDrawDataSlotVaddr);
  QCHECK(sd::kThunkLdrVaddr == sd::kThunkVaddr + 4);

  // The JUMP_SLOT word is R_AARCH64_JUMP_SLOT for RenderDrawData<CGDrawData> (readelf -rW: 00000000036fad88 JUMP_SLOT).
  const GotTarget slot{pinned::kLibR15, sd::kRenderDrawDataSymbol, RelocKind::kJumpSlot, pinned::kLibR15StockBuildId,
                       sd::kRenderDrawDataSlotVaddr};
  const SlotResolution slotRes = ResolveSlot(elf.image, slot, kAarch64Relocs);
  QCHECK_STATUS(slotRes.status, GotStatus::kOk);
  QCHECK(slotRes.slotVaddr == sd::kRenderDrawDataSlotVaddr);

  // The gctx word and the object it names, as in texel_buffer_guard_targets.h.
  const GotTarget gctx{pinned::kLibR15, sd::kGctxSymbol, RelocKind::kGlobDat, pinned::kLibR15StockBuildId, sd::kGctxWordVaddr};
  QCHECK_STATUS(ResolveSlot(elf.image, gctx, kAarch64Relocs).status, GotStatus::kOk);
  QCHECK(InBss(elf, sd::kGctxObjectVaddr, sd::kGctxObjectSize));

  // The blend slot is the one the fill stores (`str x0,[x29,#0x368]`).
  QCHECK(LdrXOffset(sd::kFillBlendConstants.word) == sd::kSlotVkCmdSetBlendConstants);

  if (quest_test::Failures() == 0) std::printf("scanner_sphere_draw_pinned_test: ok (%s)\n", buildId);
  return quest_test::Failures() == 0 ? 0 : 1;
}
