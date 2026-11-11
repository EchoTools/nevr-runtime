// The pins of src/quest/sentinel/texture_quality_targets.h, checked against the store libr15 (the hooks-pinned group).
// Each word is read from the file at its ELF address and compared with its encoding; each adrp and its add or ldr name the upload
// lock, the upload count or the queue lock pointer, and those words lie in .bss; each direct bl reaches the function pinned for it;
// each call on the path from CEngine::Resize reaches a PLT stub whose JUMP_SLOT names the pinned callee; every named function is
// its .dynsym symbol's address.
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
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/texture_quality_targets.h"
#include "quest/tests/test_check.h"

namespace {

using namespace sentinel;
namespace tq = nevr_quest::texture_quality::targets;
using nevr_quest::combat::pinned::AddImmediate;
using nevr_quest::combat::pinned::LdrXOffset;

struct LoadedElf {
  std::vector<char> bytes;  // the file, for the program and section headers
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

// [vaddr, vaddr + size) lies in the part of a PT_LOAD segment that the file does not fill (.bss).
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

bool IsAdrp(std::uint32_t word) { return (word & 0x9f000000U) == 0x90000000U; }
bool IsBl(std::uint32_t word) { return (word & 0xfc000000U) == 0x94000000U; }

// The target of a `bl` at pc: pc + imm26 * 4, sign-extended.
std::uint64_t BlTarget(std::uint32_t word, std::uint64_t pc) {
  std::int64_t imm = static_cast<std::int64_t>(word & 0x03ffffffU);
  if ((imm & (1 << 25)) != 0) imm -= (1 << 26);
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(pc) + imm * 4);
}

bool CheckWord(const LoadedElf& elf, const nevr_quest::combat::pinned::Word& w) {
  std::uint32_t have = 0;
  const bool readable = WordAt(elf, w.vaddr, &have);
  if (!readable || have != w.word) {
    std::fprintf(stderr, "word mismatch: %s va=0x%llx expected=0x%08x actual=0x%08x\n", w.what, static_cast<unsigned long long>(w.vaddr),
                 w.word, have);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: texture_quality_pinned_test <libr15.so>\n");
    return 2;
  }
  LoadedElf elf;
  if (!Load(argv[1], &elf)) {
    std::fprintf(stderr, "texture_quality_pinned_test: cannot load %s as an AArch64 ELF\n", argv[1]);
    return 2;
  }

  // The store file: the build id the pins were measured on.
  char buildId[64] = {};
  QCHECK(ReadBuildId(elf.image, buildId, sizeof(buildId)));
  QCHECK(std::strcmp(buildId, pinned::kLibR15StockBuildId) == 0);

  // Every named word is the file's word at its address.
  for (const nevr_quest::combat::pinned::Word& w : tq::kWords) QCHECK(CheckWord(elf, w));
  for (const nevr_quest::combat::pinned::Word& w : tq::kDisplayDestroyCalls) QCHECK(CheckWord(elf, w));

  // The lock, the count and the queue lock pointer lie in .bss: the game writes them at run time.
  QCHECK(InBss(elf, tq::kUploadLockVaddr, tq::kUploadLockBytes));
  QCHECK(InBss(elf, tq::kUploadCountVaddr, tq::kUploadCountBytes));
  QCHECK(InBss(elf, tq::kDeferredQueueLockPointerVaddr, sizeof(std::uint64_t)));

  // Each adrp and its add or ldr name the pinned data word.
  for (const tq::AddressPair& p : tq::kAddressPairs) {
    QCHECK(IsAdrp(p.page.word));
    const std::uint64_t low = p.lowIsLdrX ? LdrXOffset(p.low.word) : AddImmediate(p.low.word);
    const std::uint64_t names = AdrpPage(p.page.word, p.page.vaddr) + low;
    if (names != p.names) {
      std::fprintf(stderr, "pair %s + %s names 0x%llx, pinned 0x%llx\n", p.page.what, p.low.what, static_cast<unsigned long long>(names),
                   static_cast<unsigned long long>(p.names));
    }
    QCHECK(names == p.names);
  }
  // The reaper and End read and write the slot's submitted word at the same offset.
  QCHECK(LdrXOffset(tq::kReapSubmittedLoad.word) == tq::kSlotSubmittedOffset);
  QCHECK(LdrXOffset(tq::kEndSubmitted.word) == tq::kSlotSubmittedOffset);
  QCHECK(LdrXOffset(tq::kImageEndSubmitted.word) == tq::kSlotSubmittedOffset);
  // The GPU frame FlushGPU stores is the one the deferred-destroy body compares.
  QCHECK(LdrXOffset(tq::kFlushGpuFrameStore.word) == LdrXOffset(tq::kGpuFrameLoad.word));

  // Each direct bl reaches the function pinned for it.
  for (const tq::Branch& b : tq::kBranches) {
    QCHECK(IsBl(b.call.word));
    QCHECK(BlTarget(b.call.word, b.call.vaddr) == b.target);
  }

  // Every named function is its .dynsym symbol's address; CEngine::Resize is the one the live apply's resize slot must hold.
  for (const nevr_quest::combat::pinned::Symbol& s : tq::kSymbols) {
    Elf64_Sym sym;
    const bool found = DynamicSymbol(elf, s.symbol, &sym);
    if (!found || sym.st_value != s.vaddr) {
      std::fprintf(stderr, "symbol %s at 0x%llx, pinned 0x%llx\n", s.symbol, found ? static_cast<unsigned long long>(sym.st_value) : 0ULL,
                   static_cast<unsigned long long>(s.vaddr));
    }
    QCHECK(found);
    QCHECK(found && ELF64_ST_TYPE(sym.st_info) == STT_FUNC && sym.st_value == s.vaddr);
  }
  QCHECK(std::strcmp(tq::kSymbols[0].symbol, pinned::kEngineResize.name) == 0);
  QCHECK(tq::kSymbols[0].vaddr == pinned::kEngineResize.vaddr);
  // The deferred-destroy body sits right after DeferredDestroy(VkBuffer_T*), the 12-byte wrapper that forwards to it.
  QCHECK(tq::kSymbols[5].vaddr + 12 == tq::kDeferredDestroyBodyVaddr);

  // The path from CEngine::Resize: each call is a bl to a PLT stub (adrp x16; ldr x17,[x16,#off]) whose slot is the JUMP_SLOT of the
  // pinned callee, and the callee's symbol is the pinned function.
  for (const tq::PltCall& c : tq::kResizePath) {
    QCHECK(CheckWord(elf, nevr_quest::combat::pinned::Word{c.what, c.site, c.word}));
    QCHECK(IsBl(c.word));
    QCHECK(BlTarget(c.word, c.site) == c.stub);
    std::uint32_t adrp = 0;
    std::uint32_t ldr = 0;
    QCHECK(WordAt(elf, c.stub, &adrp));
    QCHECK(WordAt(elf, c.stub + 4, &ldr));
    QCHECK(IsAdrp(adrp) && (adrp & 0x1fU) == 16);
    QCHECK((ldr & 0xffc003ffU) == 0xf9400211U);  // ldr x17,[x16,#imm]
    QCHECK(AdrpPage(adrp, c.stub) + LdrXOffset(ldr) == c.slot);
    const GotTarget target{pinned::kLibR15, c.callee, RelocKind::kJumpSlot, pinned::kLibR15StockBuildId, c.slot};
    const SlotResolution slot = ResolveSlot(elf.image, target, kAarch64Relocs);
    QCHECK_STATUS(slot.status, GotStatus::kOk);
    QCHECK(slot.slotVaddr == c.slot);
    Elf64_Sym sym;
    QCHECK(DynamicSymbol(elf, c.callee, &sym) && sym.st_value == c.calleeVaddr);
  }

  if (quest_test::Failures() == 0) std::printf("texture_quality_pinned_test: ok (%s)\n", buildId);
  return quest_test::Failures() == 0 ? 0 : 1;
}
