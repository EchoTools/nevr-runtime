// The pins of src/quest/combat/scanner_sphere_draw_targets.h, checked against the store libr15 (the hooks-pinned group). The pool-11
// call of CGRenderPassMainAndroid::Render is decoded: its pool immediate, the `bl` target and the return address are the pinned ones;
// the stub's adrp and ldr name the JUMP_SLOT word, whose relocation names RenderDrawData<CGDrawData>; the gctx word and the blend slot
// are the ones the table pins (texel_buffer_guard_targets.h). The game's own pool-18 draw (the low-res transparents pass) is decoded the
// same way: blend constants through gctx+0x368 on the command buffer at ctx+0x18+8*u8[ctx+0x260], pool 18 through the same stub with the
// pass's frame key; the .text scans show nothing constructs that pass; the one draw-pipeline create, its blend-state table and
// the data of CreateBlendState's two tables, and the two CGS::Initialize fills with the strings they name.
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
using nevr_quest::combat::pinned::AddImmediate;
using nevr_quest::combat::pinned::LdrbOffset;
using nevr_quest::combat::pinned::LdrWOffset;
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

// The [start, end) of the section named `name`, or false.
bool SectionRange(const LoadedElf& elf, const char* name, std::uint64_t* start, std::uint64_t* end) {
  const std::vector<char>& b = elf.bytes;
  Elf64_Ehdr eh;
  std::memcpy(&eh, b.data(), sizeof(eh));
  if (eh.e_shoff == 0 || eh.e_shstrndx >= eh.e_shnum || eh.e_shoff + eh.e_shnum * sizeof(Elf64_Shdr) > b.size()) return false;
  std::vector<Elf64_Shdr> sh(eh.e_shnum);
  std::memcpy(sh.data(), b.data() + eh.e_shoff, eh.e_shnum * sizeof(Elf64_Shdr));
  const Elf64_Shdr& names = sh[eh.e_shstrndx];
  for (const Elf64_Shdr& s : sh) {
    if (s.sh_name < names.sh_size && std::strcmp(b.data() + names.sh_offset + s.sh_name, name) == 0) {
      *start = s.sh_addr;
      *end = s.sh_addr + s.sh_size;
      return true;
    }
  }
  return false;
}

// The NUL-terminated string at a virtual address equals `text`.
bool StringAt(const LoadedElf& elf, std::uint64_t vaddr, const char* text) {
  const std::size_t n = std::strlen(text) + 1;
  if (elf.mem == nullptr || vaddr + n > elf.span) return false;
  return std::memcmp(static_cast<const char*>(elf.mem) + vaddr, text, n) == 0;
}

// An R_AARCH64_ABS64 relocation at `where` that names `symbol` (readelf -rW: the vtable slot of a virtual function).
bool Abs64Names(const LoadedElf& elf, std::uint64_t where, const char* symbol) {
  const std::vector<char>& b = elf.bytes;
  Elf64_Ehdr eh;
  std::memcpy(&eh, b.data(), sizeof(eh));
  std::vector<Elf64_Shdr> sh(eh.e_shnum);
  std::memcpy(sh.data(), b.data() + eh.e_shoff, eh.e_shnum * sizeof(Elf64_Shdr));
  for (const Elf64_Shdr& s : sh) {
    if (s.sh_type != SHT_RELA || s.sh_link >= sh.size()) continue;
    const Elf64_Shdr& symtab = sh[s.sh_link];
    const Elf64_Shdr& str = sh[symtab.sh_link];
    for (std::uint64_t off = 0; off + sizeof(Elf64_Rela) <= s.sh_size; off += sizeof(Elf64_Rela)) {
      Elf64_Rela r;
      std::memcpy(&r, b.data() + s.sh_offset + off, sizeof(r));
      if (r.r_offset != where || ELF64_R_TYPE(r.r_info) != R_AARCH64_ABS64) continue;
      Elf64_Sym sym;
      std::memcpy(&sym, b.data() + symtab.sh_offset + ELF64_R_SYM(r.r_info) * sizeof(Elf64_Sym), sizeof(sym));
      return sym.st_name < str.sh_size && std::strcmp(b.data() + str.sh_offset + sym.st_name, symbol) == 0;
    }
  }
  return false;
}

// Over .text: the `adrp xN,page; ldr xM,[xN,#off]` pairs that read the 8-byte word at `word` (their adrp addresses), and the `b`/`bl`
// whose target is `target` (their addresses).
struct TextScan {
  std::vector<std::uint64_t> wordLoads;
  std::vector<std::uint64_t> branchesTo;
};
TextScan ScanText(const LoadedElf& elf, std::uint64_t start, std::uint64_t end, std::uint64_t word, std::uint64_t target) {
  TextScan out;
  for (std::uint64_t pc = start; pc + 4 <= end; pc += 4) {
    std::uint32_t w = 0;
    if (!WordAt(elf, pc, &w)) break;
    const std::uint32_t top = w & 0xfc000000U;
    if ((top == 0x94000000U || top == 0x14000000U) && BlTarget(w, pc) == target) out.branchesTo.push_back(pc);
    if ((w & 0x9f000000U) != 0x90000000U || pc + 8 > end) continue;
    std::uint32_t next = 0;
    if (!WordAt(elf, pc + 4, &next)) break;
    const bool ldrX = (next & 0xffc00000U) == 0xf9400000U && ((next >> 5) & 0x1fU) == (w & 0x1fU);
    if (ldrX && AdrpPage(w, pc) + LdrXOffset(next) == word) out.wordLoads.push_back(pc);
  }
  return out;
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
  struct List {
    const nevr_quest::combat::pinned::Word* words;
    std::size_t count;
  };
  const List lists[] = {
      {sd::kWords, std::size(sd::kWords)},           {sd::kLowresWords, std::size(sd::kLowresWords)},
      {sd::kCmdBufferWords, std::size(sd::kCmdBufferWords)}, {sd::kPsoWords, std::size(sd::kPsoWords)},
      {sd::kCreateBlendStateWords, std::size(sd::kCreateBlendStateWords)}, {sd::kFillWords, std::size(sd::kFillWords)},
  };
  for (const List& list : lists) {
  for (std::size_t i = 0; i < list.count; ++i) {
    const nevr_quest::combat::pinned::Word& w = list.words[i];
    std::uint32_t have = 0;
    const bool readable = WordAt(elf, w.vaddr, &have);
    if (!readable || have != w.word) {
      std::fprintf(stderr, "word mismatch: %s va=0x%llx expected=0x%08x actual=0x%08x\n", w.what,
                   static_cast<unsigned long long>(w.vaddr), w.word, have);
    }
    QCHECK(readable);
    QCHECK(have == w.word);
  }
  }

  // The pool-11 argument is `mov w2,#pool`: destination w2, immediate the pool.
  QCHECK((sd::kPool11ArgWord & 0x1fU) == 2);
  QCHECK(MovImmediate(sd::kPool11ArgWord) == sd::kPool11Pool);

  // The call is a `bl` to the stub (its return site is a pinned word, checked above).
  QCHECK((sd::kPool11CallWord & 0xfc000000U) == 0x94000000U);
  QCHECK(BlTarget(sd::kPool11CallWord, sd::kPool11CallVaddr) == sd::kThunkVaddr);

  // The stub: `adrp x16,page; ldr x17,[x16,#off]; br x17`. The adrp page plus the ldr offset is the JUMP_SLOT word.
  QCHECK((sd::kThunkAdrpWord & 0x9f000000U) == 0x90000000U);
  QCHECK(AdrpPage(sd::kThunkAdrpWord, sd::kThunkVaddr) + LdrXOffset(sd::kThunkLdrWord) == sd::kRenderDrawDataSlotVaddr);

  // The JUMP_SLOT word is R_AARCH64_JUMP_SLOT for RenderDrawData<CGDrawData> (readelf -rW: 00000000036fad88 JUMP_SLOT).
  const GotTarget slot{pinned::kLibR15, sd::kRenderDrawDataSymbol, RelocKind::kJumpSlot, pinned::kLibR15StockBuildId,
                       sd::kRenderDrawDataSlotVaddr};
  const SlotResolution slotRes = ResolveSlot(elf.image, slot, kAarch64Relocs);
  QCHECK_STATUS(slotRes.status, GotStatus::kOk);
  QCHECK(slotRes.slotVaddr == sd::kRenderDrawDataSlotVaddr);
  // The symbol that relocation names is defined at the pinned function, whose first word is a pinned word.
  Elf64_Sym drawSym;
  QCHECK(DynamicSymbol(elf, sd::kRenderDrawDataSymbol, &drawSym));
  if (drawSym.st_value != sd::kRenderDrawDataVaddr) {
    std::fprintf(stderr, "RenderDrawData<CGDrawData> .dynsym value %#llx, pinned %#llx\n", static_cast<unsigned long long>(drawSym.st_value),
                 static_cast<unsigned long long>(sd::kRenderDrawDataVaddr));
  }
  QCHECK(ELF64_ST_TYPE(drawSym.st_info) == STT_FUNC);
  QCHECK(drawSym.st_value == sd::kRenderDrawDataVaddr);

  // The gctx word and the object it names, as in texel_buffer_guard_targets.h.
  const GotTarget gctx{pinned::kLibR15, sd::kGctxSymbol, RelocKind::kGlobDat, pinned::kLibR15StockBuildId, sd::kGctxWordVaddr};
  QCHECK_STATUS(ResolveSlot(elf.image, gctx, kAarch64Relocs).status, GotStatus::kOk);
  // The object the gctx word names is the .dynsym object of the symbol: its address and its size are the pinned ones.
  Elf64_Sym gctxSym;
  QCHECK(DynamicSymbol(elf, sd::kGctxSymbol, &gctxSym));
  if (gctxSym.st_value != sd::kGctxObjectVaddr || gctxSym.st_size != sd::kGctxObjectSize) {
    std::fprintf(stderr, "gContext .dynsym is %#llx size %#llx, pinned %#llx size %#llx\n", static_cast<unsigned long long>(gctxSym.st_value),
                 static_cast<unsigned long long>(gctxSym.st_size), static_cast<unsigned long long>(sd::kGctxObjectVaddr),
                 static_cast<unsigned long long>(sd::kGctxObjectSize));
  }
  QCHECK(ELF64_ST_TYPE(gctxSym.st_info) == STT_OBJECT);
  QCHECK(gctxSym.st_value == sd::kGctxObjectVaddr);
  QCHECK(gctxSym.st_size == sd::kGctxObjectSize);
  QCHECK(InBss(elf, sd::kGctxObjectVaddr, sd::kGctxObjectSize));

  // The blend slot is the one the fill stores (`str x0,[x29,#0x368]`).
  QCHECK(LdrXOffset(sd::kFillBlendConstants.word) == sd::kSlotVkCmdSetBlendConstants);

  // (c) The low-res pass: a virtual Render (its vtable slot names it), whose pool-18 call goes through the eye pass's stub.
  Elf64_Sym lowres;
  QCHECK(DynamicSymbol(elf, sd::kLowresRenderSymbol, &lowres));
  QCHECK(lowres.st_value == sd::kLowresRenderVaddr);
  QCHECK(lowres.st_size == sd::kLowresRenderSize);
  QCHECK(Abs64Names(elf, sd::kLowresVtableSlotVaddr, sd::kLowresRenderSymbol));
  QCHECK(sd::kPool18CallVaddr > sd::kLowresRenderVaddr && sd::kPool18CallVaddr < sd::kLowresRenderVaddr + sd::kLowresRenderSize);
  QCHECK((sd::kPool18ArgWord & 0x1fU) == 2);
  QCHECK(MovImmediate(sd::kPool18ArgWord) == sd::kPool18Pool);
  QCHECK((sd::kPool18CallWord & 0xfc000000U) == 0x94000000U);
  QCHECK(BlTarget(sd::kPool18CallWord, sd::kPool18CallVaddr) == sd::kThunkVaddr);
  QCHECK(sd::kPool18ReturnVaddr == sd::kPool18CallVaddr + 4);
  // The constants are four 1.0f: `fmov v0.4s,#imm8` with imm8 0x70 (abc in bits 16-18, defgh in bits 5-9), which is 1.0.
  QCHECK((sd::kLowresBlendFmovWord & 0xfff8fc00U) == 0x4f00f400U);
  QCHECK((((sd::kLowresBlendFmovWord >> 16) & 7U) << 5 | ((sd::kLowresBlendFmovWord >> 5) & 0x1fU)) == 0x70U);
  // The gctx word, its +0x368 slot, and the command buffer at ctx + 0x18 + 8 * u8[ctx + 0x260].
  QCHECK(AdrpPage(sd::kLowresGctxAdrpWord, sd::kLowresGctxAdrpVaddr) + LdrXOffset(sd::kLowresGctxLdrWord) == sd::kGctxWordVaddr);
  QCHECK(LdrXOffset(sd::kLowresBlendSlotWord) == sd::kSlotVkCmdSetBlendConstants);
  QCHECK(LdrbOffset(sd::kLowresCmdIndexWord) == sd::kCtxCmdIndexOffset);
  QCHECK(LdrXOffset(sd::kLowresCmdLoadWord) == sd::kCtxCmdBuffersOffset);

  // The stub is the target of kThunkCallSites branches in .text; each has `mov w2,#imm` four words before it, and exactly one imm is
  // the pool 18, at the pinned call: no other RenderDrawData call has an immediate pool 18.
  std::uint64_t textBegin = 0;
  std::uint64_t textFinish = 0;
  QCHECK(SectionRange(elf, ".text", &textBegin, &textFinish));
  const TextScan stubScan = ScanText(elf, textBegin, textFinish, 0, sd::kThunkVaddr);
  QCHECK(stubScan.branchesTo.size() == sd::kThunkCallSites);
  std::size_t pool18Sites = 0;
  for (const std::uint64_t site : stubScan.branchesTo) {
    std::uint32_t mov = 0;
    QCHECK(WordAt(elf, site - 16, &mov));
    QCHECK((mov & 0xffe0001fU) == 0x52800002U);  // mov w2,#imm16
    if (MovImmediate(mov) == sd::kPool18Pool) {
      ++pool18Sites;
      QCHECK(site == sd::kPool18CallVaddr);
    }
  }
  QCHECK(pool18Sites == 1);

  // (b) SGThreadContext::CmdBuffer is exported and reads the same two fields.
  Elf64_Sym cmd;
  QCHECK(DynamicSymbol(elf, sd::kCmdBufferSymbol, &cmd));
  QCHECK(cmd.st_value == sd::kCmdBufferVaddr);
  QCHECK(cmd.st_size == 16);
  QCHECK(LdrbOffset(sd::kCmdBufferWords[0].word) == sd::kCtxCmdIndexOffset);
  QCHECK(LdrXOffset(sd::kCmdBufferWords[2].word) == sd::kCtxCmdBuffersOffset);

  // Nothing in .text builds the pass: its vtable's GOT word is read only by the constructor and the two destructors, and nothing
  // branches to the constructor.
  const GotTarget vtable{pinned::kLibR15, sd::kLowresVtableSymbol, RelocKind::kGlobDat, pinned::kLibR15StockBuildId, sd::kLowresVtableGotVaddr};
  QCHECK_STATUS(ResolveSlot(elf.image, vtable, kAarch64Relocs).status, GotStatus::kOk);
  Elf64_Sym vt;
  QCHECK(DynamicSymbol(elf, sd::kLowresVtableSymbol, &vt));
  QCHECK(vt.st_value == sd::kLowresVtableVaddr);
  Elf64_Sym ctor;
  QCHECK(DynamicSymbol(elf, sd::kLowresCtorSymbol, &ctor));
  QCHECK(ctor.st_value == sd::kLowresCtorVaddr);
  std::uint64_t textStart = 0;
  std::uint64_t textEnd = 0;
  QCHECK(SectionRange(elf, ".text", &textStart, &textEnd));
  const TextScan lowresScan = ScanText(elf, textStart, textEnd, sd::kLowresVtableGotVaddr, sd::kLowresCtorVaddr);
  QCHECK(lowresScan.wordLoads.size() == std::size(sd::kLowresVtableLoads));
  for (std::size_t i = 0; i < lowresScan.wordLoads.size() && i < std::size(sd::kLowresVtableLoads); ++i) {
    QCHECK(lowresScan.wordLoads[i] == sd::kLowresVtableLoads[i]);
  }
  QCHECK(sd::kLowresVtableLoads[0] >= sd::kLowresCtorVaddr && sd::kLowresVtableLoads[0] < sd::kLowresCtorVaddr + ctor.st_size);
  if (!lowresScan.branchesTo.empty()) {
    std::fprintf(stderr, "a branch to the low-res pass constructor at %#llx\n", static_cast<unsigned long long>(lowresScan.branchesTo[0]));
  }
  QCHECK(lowresScan.branchesTo.empty());

  // (a) The draw-pipeline create: reached only from its two pinned callers; it reads the gctx word's +0x258 slot and the blend mode at
  // desc+0x2d8 (at most 0x19), and indexes blendstates (GLOB_DAT) by 0x558-byte entries.
  const TextScan psoScan = ScanText(elf, textStart, textEnd, sd::kBlendStatesGotVaddr, sd::kPsoCreateVaddr);
  QCHECK(psoScan.branchesTo.size() == std::size(sd::kPsoCreateCallers));
  for (std::size_t i = 0; i < psoScan.branchesTo.size() && i < std::size(sd::kPsoCreateCallers); ++i) {
    QCHECK(psoScan.branchesTo[i] == sd::kPsoCreateCallers[i]);
  }
  QCHECK(AdrpPage(sd::kPsoGctxAdrpWord, 0x18da3c0ULL) + LdrXOffset(sd::kPsoGctxLdrWord) == sd::kGctxWordVaddr);
  QCHECK(LdrXOffset(sd::kPsoCreateSlotWord) == sd::kSlotVkCreateGraphicsPipelines);
  QCHECK(LdrXOffset(sd::kFillCreateGraphicsPipelines.word) == sd::kSlotVkCreateGraphicsPipelines);
  QCHECK(LdrWOffset(sd::kDescBlendModeLdrWord) == sd::kDescBlendModeOffset);
  QCHECK(AddImmediate(sd::kBlendModeCmpWord) == sd::kBlendModeMax);
  QCHECK(MovImmediate(sd::kBlendStateStrideWord) == sd::kBlendStateSize);
  QCHECK(AdrpPage(sd::kBlendStatesAdrpWord, sd::kBlendStatesAdrpVaddr) + LdrXOffset(sd::kBlendStatesLdrWord) == sd::kBlendStatesGotVaddr);
  const GotTarget blend{pinned::kLibR15, sd::kBlendStatesSymbol, RelocKind::kGlobDat, pinned::kLibR15StockBuildId, sd::kBlendStatesGotVaddr};
  QCHECK_STATUS(ResolveSlot(elf.image, blend, kAarch64Relocs).status, GotStatus::kOk);
  Elf64_Sym bs;
  QCHECK(DynamicSymbol(elf, sd::kBlendStatesSymbol, &bs));
  QCHECK(bs.st_value == sd::kBlendStatesVaddr);
  QCHECK(bs.st_size == (sd::kBlendModeMax + 1) * sd::kBlendStateSize);
  QCHECK(InBss(elf, sd::kBlendStatesVaddr, bs.st_size));

  // CreateBlendState's two tables hold the pinned words (the adrp + add words name them).
  Elf64_Sym cbs;
  QCHECK(DynamicSymbol(elf, sd::kCreateBlendStateSymbol, &cbs));
  QCHECK(cbs.st_value == sd::kCreateBlendStateVaddr);
  QCHECK(AdrpPage(sd::kCreateBlendStateWords[0].word, sd::kCreateBlendStateVaddr) + AddImmediate(sd::kCreateBlendStateWords[1].word) ==
         sd::kBlendFactorTableVaddr);
  // The ops table's adrp is its own word (x12's, at 0x1881950), not x11's: each table is named by its own page and add.
  QCHECK(AdrpPage(sd::kCreateBlendStateWords[6].word, sd::kCreateBlendStateWords[6].vaddr) + AddImmediate(sd::kCreateBlendStateWords[2].word) ==
         sd::kBlendOpTableVaddr);
  QCHECK(AddImmediate(sd::kCreateBlendStateWords[3].word) == sd::kBlendStateAttachmentsOffset);
  for (std::size_t i = 0; i < std::size(sd::kBlendFactorTable); ++i) {
    std::uint32_t have = 0;
    QCHECK(WordAt(elf, sd::kBlendFactorTableVaddr + 4 * i, &have));
    QCHECK(have == sd::kBlendFactorTable[i]);
  }
  for (std::size_t i = 0; i < std::size(sd::kBlendOpTable); ++i) {
    std::uint32_t have = 0;
    QCHECK(WordAt(elf, sd::kBlendOpTableVaddr + 4 * i, &have));
    QCHECK(have == sd::kBlendOpTable[i]);
  }

  // (d) The fills name their functions by string: adrp page + add immediate is the string's address.
  QCHECK(AdrpPage(sd::kNameAdrpWord, 0x18de7e4ULL) + AddImmediate(sd::kFillWords[2].word) == sd::kCreatePipelinesNameVaddr);
  QCHECK(AdrpPage(sd::kNameAdrpWord, 0x18deb9cULL) + AddImmediate(sd::kFillWords[7].word) == sd::kSetBlendConstantsNameVaddr);
  QCHECK(StringAt(elf, sd::kCreatePipelinesNameVaddr, "vkCreateGraphicsPipelines"));
  QCHECK(StringAt(elf, sd::kSetBlendConstantsNameVaddr, "vkCmdSetBlendConstants"));
  QCHECK(LdrXOffset(sd::kGdpaLoadWord) == 0x88);

  if (quest_test::Failures() == 0) std::printf("scanner_sphere_draw_pinned_test: ok (%s)\n", buildId);
  return quest_test::Failures() == 0 ? 0 : 1;
}
