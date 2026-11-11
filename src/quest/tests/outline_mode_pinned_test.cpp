// Checks the outline-mode module's pins (src/quest/combat/outline_mode_targets.h) against the real libr15.so: the store's build ID, the
// two SetOutlineMode JUMP_SLOTs and their functions, every pinned instruction and data word, the .dynsym values of the functions the
// header names (the five CGVertexFormat helpers and their near namesakes among them), each PLT stub's `bl` target and JUMP_SLOT, and
// CGComputeVertexBuffer::Bind's jump table, decoded usage by usage. Nothing is executed or patched. Run by `just test-quest-hooks-pinned`,
// which passes the store libr15.so.
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
#include "quest/combat/pinned_words.h"
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

// The bytes at a virtual address of the loaded file; false past the image.
bool BytesAt(const LoadedElf& elf, std::uint64_t vaddr, void* out, std::size_t n) {
  if (elf.mem == nullptr || vaddr + n > elf.span) return false;
  std::memcpy(out, static_cast<const char*>(elf.mem) + vaddr, n);
  return true;
}

// The target of a `bl` at pc: pc + imm26 * 4, imm26 sign-extended.
std::uint64_t BlTarget(std::uint32_t word, std::uint64_t pc) {
  std::int64_t imm = static_cast<std::int64_t>(word & 0x3ffffffU);
  if ((imm & 0x2000000) != 0) imm -= 0x4000000;
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(pc) + imm * 4);
}

// The target of a `b.cond`, `cbz` or `cbnz` at pc: pc + imm19 * 4, imm19 (bits 5-23) sign-extended.
std::uint64_t CondTarget(std::uint32_t word, std::uint64_t pc) {
  std::int64_t imm = static_cast<std::int64_t>((word >> 5) & 0x7ffffU);
  if ((imm & 0x40000) != 0) imm -= 0x80000;
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(pc) + imm * 4);
}

// The page an `adrp` at pc names: (pc & ~0xfff) + immhi:immlo << 12, immediate sign-extended.
std::uint64_t AdrpPage(std::uint32_t word, std::uint64_t pc) {
  std::int64_t imm = (static_cast<std::int64_t>((word >> 5) & 0x7ffffU) << 2) | static_cast<std::int64_t>((word >> 29) & 3U);
  if ((imm & (1 << 20)) != 0) imm -= (1 << 21);
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(pc & ~static_cast<std::uint64_t>(0xfff)) + (imm << 12));
}

// The address an `adr` at pc names: pc + immhi:immlo, sign-extended.
std::uint64_t AdrTarget(std::uint32_t word, std::uint64_t pc) {
  std::int64_t imm = (static_cast<std::int64_t>((word >> 5) & 0x7ffffU) << 2) | static_cast<std::int64_t>((word >> 29) & 3U);
  if ((imm & (1 << 20)) != 0) imm -= (1 << 21);
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(pc) + imm);
}

// One pinned word: the file holds it.
void CheckWord(const LoadedElf& elf, const nevr_quest::combat::pinned::Word& w) {
  std::uint32_t have = 0;
  const bool readable = BytesAt(elf, w.vaddr, &have, sizeof(have));
  if (!readable || have != w.word) {
    std::fprintf(stderr, "outline_mode_pinned_test: word mismatch at %#llx (%s): expected=%#x actual=%#x%s\n",
                 static_cast<unsigned long long>(w.vaddr), w.what, w.word, have, readable ? "" : " (unreadable)");
  }
  QCHECK(readable && have == w.word);
}

// One function by name: .dynsym gives it the pinned value, and it is a function.
void CheckFunction(const LoadedElf& elf, const nevr_quest::combat::pinned::Symbol& f) {
  Elf64_Sym sym{};
  const bool found = DynamicSymbol(elf, f.symbol, &sym);
  if (!found || sym.st_value != f.vaddr || ELF64_ST_TYPE(sym.st_info) != STT_FUNC) {
    std::fprintf(stderr, "outline_mode_pinned_test: %s: .dynsym value %#llx, pinned %#llx%s\n", f.symbol,
                 static_cast<unsigned long long>(sym.st_value), static_cast<unsigned long long>(f.vaddr), found ? "" : " (absent)");
  }
  QCHECK(found);
  QCHECK(sym.st_value == f.vaddr);
  QCHECK(ELF64_ST_TYPE(sym.st_info) == STT_FUNC);
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

  // Every pinned word and 8-byte record holds in the file.
  for (const nevr_quest::combat::pinned::Word& w : ot::kWords) CheckWord(r15, w);
  for (const nevr_quest::combat::pinned::Quad& q : ot::kQuads) {
    std::uint64_t have = 0;
    QCHECK(BytesAt(r15, q.vaddr, &have, sizeof(have)));
    if (have != q.value) std::fprintf(stderr, "outline_mode_pinned_test: %s: %#llx\n", q.what, static_cast<unsigned long long>(have));
    QCHECK(have == q.value);
  }

  // The functions, by .dynsym name: the five helpers, the two near namesakes (each at its own address, none a helper's), and the
  // functions the words are read in. The pinned slot functions are the same check's first use.
  for (const nevr_quest::combat::pinned::Symbol& f : ot::kHelpers) CheckFunction(r15, f);
  for (const nevr_quest::combat::pinned::Symbol& f : ot::kNotHelpers) {
    CheckFunction(r15, f);
    for (const nevr_quest::combat::pinned::Symbol& h : ot::kHelpers) QCHECK(f.vaddr != h.vaddr);
  }
  for (const nevr_quest::combat::pinned::Symbol& f : ot::kFunctions) CheckFunction(r15, f);
  CheckFunction(r15, {ot::kSceneSymbol, ot::kPins.sceneFn});
  CheckFunction(r15, {ot::kDrawSetSymbol, ot::kPins.drawSetFn});

  // Each word lies in the function the header says it is read in.
  auto within = [&](const char* symbol, std::uint64_t vaddr) {
    Elf64_Sym sym{};
    QCHECK(DynamicSymbol(r15, symbol, &sym));
    const bool in = vaddr >= sym.st_value && vaddr < sym.st_value + sym.st_size;
    if (!in) std::fprintf(stderr, "outline_mode_pinned_test: %#llx is not in %s\n", static_cast<unsigned long long>(vaddr), symbol);
    QCHECK(in);
  };
  for (const auto* w : {&ot::kSceneMeshCount, &ot::kScenePrimRead, &ot::kSceneDrawSetCall, &ot::kSceneDrawSetStride}) {
    within(ot::kSceneSymbol, w->vaddr);
  }
  for (const auto* w : {&ot::kMeshSkinnedRead, &ot::kMeshBindCall, &ot::kMeshDrawSetInitCall, &ot::kMeshStoreAt260, &ot::kMeshThisMove,
                        &ot::kMeshThisSave, &ot::kMeshThisReload, &ot::kMeshBindResultArg, &ot::kMeshDrawSetInitBufArg}) {
    within(ot::kMeshInitializeSymbol, w->vaddr);
  }
  for (const auto* w : {&ot::kBindComputeFormat, &ot::kBindCaseBaseAdr, &ot::kBindCopySize, &ot::kBindThisMove, &ot::kBindInputMove,
                        &ot::kBindResultMove, &ot::kBindElementThis, &ot::kBindElementIndex, &ot::kBindElementArg, &ot::kBindUsageBranch,
                        &ot::kBindTableLoad, &ot::kBindCaseAdd, &ot::kBindCaseBranch, &ot::kBindCaseFormatArg, &ot::kBindAppendDst,
                        &ot::kBindAppendSrc, &ot::kBindCopySrc, &ot::kBindCopyDst}) {
    within(ot::kBindSymbol, w->vaddr);
  }
  for (const ot::Call& c : ot::kBindCalls) within(ot::kBindSymbol, c.site.vaddr);
  for (const auto* w : {&ot::kInitOutlineModeRead, &ot::kInitOutlineModeTest, &ot::kInitOutlineModeBranch}) {
    within(ot::kInitOutlineDrawSymbol, w->vaddr);
  }
  for (const auto* w : {&ot::kOutlinePoolAdrp, &ot::kOutlinePoolTable, &ot::kOutlinePoolLoad, &ot::kOutlinePoolRet}) {
    within(ot::kOutlineRenderPoolSymbol, w->vaddr);
  }
  for (const auto* w : {&ot::kNodeDataMove, &ot::kNodeModeRead, &ot::kNodeModeArg, &ot::kNodeSetModeCall.site}) within(ot::kNodeEnterSymbol, w->vaddr);

  // The stubs: each `bl` reaches its stub, and each stub's adrp + ldr name the JUMP_SLOT its symbol's relocation names.
  QCHECK(BlTarget(ot::kSceneDrawSetCall.word, ot::kSceneDrawSetCall.vaddr) == ot::kDrawSetStubVaddr);
  QCHECK(BlTarget(ot::kMeshCountUsedCall.word, ot::kMeshCountUsedCall.vaddr) == ot::kStubs[0].vaddr);
  QCHECK(BlTarget(ot::kMeshBindCall.word, ot::kMeshBindCall.vaddr) == ot::kStubs[1].vaddr);
  QCHECK(BlTarget(ot::kMeshDrawSetInitCall.word, ot::kMeshDrawSetInitCall.vaddr) == ot::kStubs[2].vaddr);
  for (const ot::Stub& stub : ot::kStubs) {
    CheckWord(r15, {"stub adrp", stub.vaddr, stub.adrpWord});
    CheckWord(r15, {"stub ldr", stub.vaddr + 4, stub.ldrWord});
    QCHECK(AdrpPage(stub.adrpWord, stub.vaddr) + nevr_quest::combat::pinned::LdrXOffset(stub.ldrWord) == stub.slotVaddr);
    const GotTarget target{pinned::kLibR15, stub.symbol, RelocKind::kJumpSlot, pinned::kLibR15StockBuildId, std::nullopt};
    const SlotResolution r = ResolveSlot(r15.image, target, kAarch64Relocs);
    if (r.status != GotStatus::kOk || r.slotVaddr != stub.slotVaddr) {
      std::fprintf(stderr, "outline_mode_pinned_test: %s: slot %s\n", stub.symbol, GotStatusName(r.status));
    }
    QCHECK_STATUS(r.status, GotStatus::kOk);
    QCHECK(r.slotVaddr == stub.slotVaddr);
  }
  QCHECK(ot::kStubs[3].slotVaddr == ot::kPins.drawSetSlot);
  QCHECK(ot::kStubs[ot::kStubDrawSetSetMode].slotVaddr == ot::kPins.drawSetSlot);
  // The other pinned `bl`s each reach the stub named for them (the words themselves are in kBindCalls and the node's call).
  std::vector<ot::Call> calls(std::begin(ot::kBindCalls), std::end(ot::kBindCalls));
  calls.push_back(ot::kNodeSetModeCall);
  for (const ot::Call& c : calls) {
    CheckWord(r15, c.site);
    QCHECK((c.site.word & 0xfc000000U) == 0x94000000U);
    QCHECK(c.stub < std::size(ot::kStubs));
    const std::uint64_t reached = BlTarget(c.site.word, c.site.vaddr);
    if (c.stub >= std::size(ot::kStubs) || reached != ot::kStubs[c.stub].vaddr) {
      std::fprintf(stderr, "outline_mode_pinned_test: %s reaches %#llx\n", c.site.what, static_cast<unsigned long long>(reached));
    }
    QCHECK(c.stub < std::size(ot::kStubs) && reached == ot::kStubs[c.stub].vaddr);
  }
  // The merge branches: both `b.ne` and `b.hi` of the element test go to the plain append, both `cbz` of Initialize go past the Bind call.
  QCHECK(CondTarget(ot::kBindStreamBranch.word, ot::kBindStreamBranch.vaddr) == ot::kBindAppendAsIs);
  QCHECK(CondTarget(ot::kBindUsageBranch.word, ot::kBindUsageBranch.vaddr) == ot::kBindAppendAsIs);
  QCHECK(CondTarget(ot::kMeshSkinnedTest.word, ot::kMeshSkinnedTest.vaddr) == ot::kMeshMergeSkipVaddr);
  QCHECK(CondTarget(ot::kMeshCountUsedTest.word, ot::kMeshCountUsedTest.vaddr) == ot::kMeshMergeSkipVaddr);
  QCHECK(ot::kMeshMergeSkipVaddr > ot::kMeshBindCall.vaddr);

  // OutlineRenderPool: its adrp + add name the table, and the function is those four words (its .dynsym size).
  QCHECK(AdrpPage(ot::kOutlinePoolAdrp.word, ot::kOutlinePoolAdrp.vaddr) +
             nevr_quest::combat::pinned::AddImmediate(ot::kOutlinePoolTable.word) ==
         ot::kOutlinePoolTableVaddr);
  Elf64_Sym poolSym{};
  QCHECK(DynamicSymbol(r15, ot::kOutlineRenderPoolSymbol, &poolSym));
  QCHECK(poolSym.st_size == ot::kOutlineRenderPoolSize);
  QCHECK(ot::kOutlinePoolRet.vaddr + 4 == poolSym.st_value + poolSym.st_size);

  // The merge rule: the adr names the case base, the adrp + add name the table, and the table sends usages 0, 2, 3 and 11 to the
  // case whose `mov w1` names that usage, every other usage to the plain append.
  QCHECK(AdrTarget(ot::kBindCaseBaseAdr.word, ot::kBindCaseBaseAdr.vaddr) == ot::kBindCaseBase);
  QCHECK(AdrpPage(ot::kBindTablePage.word, ot::kBindTablePage.vaddr) + nevr_quest::combat::pinned::AddImmediate(ot::kBindTableOffset.word) ==
         ot::kBindJumpTable);
  std::uint8_t table[ot::kBindJumpTableSize] = {};
  QCHECK(BytesAt(r15, ot::kBindJumpTable, table, sizeof(table)));
  for (std::size_t usage = 0; usage <= ot::kMaxCaseUsage; ++usage) {
    const std::uint64_t target = ot::kBindCaseBase + 4ULL * table[usage];
    const ot::MergeCase* replaced = nullptr;
    for (const ot::MergeCase& c : ot::kMergeCases) {
      if (c.usage == usage) replaced = &c;
    }
    if (replaced != nullptr) {
      if (target != replaced->caseVaddr) std::fprintf(stderr, "outline_mode_pinned_test: usage %zu -> %#llx\n", usage,
                                                      static_cast<unsigned long long>(target));
      QCHECK(target == replaced->caseVaddr);
      CheckWord(r15, replaced->usageArg);
    } else {
      if (target != ot::kBindAppendAsIs) std::fprintf(stderr, "outline_mode_pinned_test: usage %zu -> %#llx, not the plain append\n",
                                                      usage, static_cast<unsigned long long>(target));
      QCHECK(target == ot::kBindAppendAsIs);
    }
  }
  // usage 0's `mov w1,wzr` is `orr w1,wzr,wzr`: no immediate to decode, so its word is the check.
  QCHECK(ot::kMergeCases[0].usage == 0 && ot::kMergeCases[0].usageArg.word == 0x2a1f03e1U);

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "outline_mode_pinned_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("outline_mode_pinned_test: the outline-mode slots, words, functions, stubs and merge table hold in the real libr15\n");
  return 0;
}
