// Resolves the pinned Quest targets (src/quest/sentinel/pinned_targets.h and the login
// prerequisites' src/quest/login/login_prerequisite_targets.h) against the real libr15.so,
// libpnsradmatchmaking.so and libpnsovr.so, on any host.
//
// The ELF is laid out in memory the way a loader would (PT_LOAD segments copied to
// their vaddrs inside one anonymous mapping), so the production ResolveSlot walks
// the real dynamic section, 100k-entry relocation tables and string table with
// link-time-relative pointers, exactly as on Bionic. Nothing is executed or
// patched, and relocation numbers are passed as AArch64's, so an x86_64 host can
// check an AArch64 image.
//
// Run: got_pinned_test <libr15.so> <libpnsradmatchmaking.so> <libpnsovr.so>

#include <elf.h>
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <iterator>
#include <vector>

#include "got_hook.h"
#include "pinned_targets.h"
#include "quest/login/login_prerequisite_targets.h"
#include "quest/game_login_failures.h"
#include "quest/login/login_rewrite.h"
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

// The value .dynsym gives `symbol` (its link-time address), or 0 when absent.
std::uint64_t DynamicSymbolValue(const LoadedElf& elf, const char* symbol) {
  const std::vector<char>& b = elf.bytes;
  Elf64_Ehdr eh;
  std::memcpy(&eh, b.data(), sizeof(eh));
  if (eh.e_shoff == 0 || eh.e_shoff + eh.e_shnum * sizeof(Elf64_Shdr) > b.size()) return 0;
  std::vector<Elf64_Shdr> sh(eh.e_shnum);
  std::memcpy(sh.data(), b.data() + eh.e_shoff, eh.e_shnum * sizeof(Elf64_Shdr));
  for (const Elf64_Shdr& s : sh) {
    if (s.sh_type != SHT_DYNSYM || s.sh_link >= sh.size()) continue;
    const Elf64_Shdr& str = sh[s.sh_link];
    for (std::uint64_t off = 0; off + sizeof(Elf64_Sym) <= s.sh_size; off += sizeof(Elf64_Sym)) {
      Elf64_Sym sym;
      std::memcpy(&sym, b.data() + s.sh_offset + off, sizeof(sym));
      if (sym.st_name < str.sh_size && std::strcmp(b.data() + str.sh_offset + sym.st_name, symbol) == 0) {
        return sym.st_value;
      }
    }
  }
  return 0;
}

// `text` is a whole NUL-terminated string in the file (a NUL before and after it).
bool HoldsString(const std::vector<char>& file, const char* text) {
  std::string needle(1, '\0');
  needle += text;
  needle += '\0';
  return std::search(file.begin(), file.end(), needle.begin(), needle.end()) != file.end();
}

std::vector<char> ReadAll(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void CheckBuildId(const LoadedElf& elf, const char* expected) {
  char actual[64] = {};
  QCHECK(ReadBuildId(elf.image, actual, sizeof(actual)));
  QCHECK(std::strcmp(actual, expected) == 0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: got_pinned_test <libr15.so> <libpnsradmatchmaking.so> <libpnsovr.so>\n");
    return 2;
  }
  LoadedElf r15, mm, ovr;
  if (!Load(argv[1], &r15) || !Load(argv[2], &mm) || !Load(argv[3], &ovr)) {
    std::fprintf(stderr, "got_pinned_test: cannot load an AArch64 ELF64 from the given paths\n");
    return 2;
  }
  CheckBuildId(r15, pinned::kLibR15BuildId);
  CheckBuildId(mm, pinned::kMatchmakingBuildId);
  CheckBuildId(ovr, nevr_quest_login::PrerequisiteTargets::kPnsovrBuildId);

  CheckTarget(r15, pinned::LibR15ClockGettime(), "libr15 clock_gettime JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15TString(), "libr15 CJson::TString JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15ConfigRequestSend(), "libr15 SNSConfigRequestv24Send JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15SetDelimitedErrorMessage(), "libr15 CR15NetGame::SetDelimitedErrorMessage JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15NetGameUpdate(), "libr15 CR15NetGame::Update JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15EnablePageNodeEnter(), "libr15 CR15UIPage2EnablePageNode::Enter JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15MountObb(), "libr15 AStorageManager_mountObb JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15GetMountedObbPath(), "libr15 AStorageManager_getMountedObbPath JUMP_SLOT");
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

  // The login prerequisites (login_prerequisite_targets.h): every slot resolves at its pin, and a
  // callback slot's symbol is defined at the function the install expects the slot to hold.
  for (const nevr_quest_login::PrerequisiteTargets::PinnedSlot& slot : nevr_quest_login::PrerequisiteTargets::kAll) {
    CheckTarget(ovr, nevr_quest_login::PrerequisiteTargets::TargetFor(slot), slot.symbol);
    if (slot.function != 0 && DynamicSymbolValue(ovr, slot.symbol) != slot.function) {
      std::fprintf(stderr, "libpnsovr %s: .dynsym value is not %#llx\n", slot.symbol,
                   static_cast<unsigned long long>(slot.function));
      QCHECK(false);
    }
  }
  // The login-failure texts the sign-in prompt hook recognises are the game's own strings.
  const std::vector<char> pnsovr = ReadAll(argv[3]);
  QCHECK(pnsovr.size() > 1000000);
  for (const char* text : nevr_quest::game_login_failures::kAll) {
    if (!HoldsString(pnsovr, text)) std::fprintf(stderr, "not in libpnsovr.so: %s\n", text);
    QCHECK(HoldsString(pnsovr, text));
  }
  const std::vector<char> r15File = ReadAll(argv[1]);
  QCHECK(HoldsString(r15File, nevr_quest::game_login_failures::kServiceUnavailable));

  // The non-GOT facts the login send gate pins in the real libpnsovr.so: CNSUser::DeferredLogInFailed's
  // three instructions, GotLoggedInUserCb's two instructions that form the user-name buffer address,
  // and the exact "prerequisites are missing" bytes the gate passes to the failure (also the #239
  // prompt's recognised local text).
  // The non-GOT byte facts the login send gate pins, checked against the real libpnsovr using the
  // SAME production constants login_hook.cpp acts on (login_prerequisite_targets.h).
  namespace PT = nevr_quest_login::PrerequisiteTargets;
  const unsigned char* ovr_base = static_cast<const unsigned char*>(ovr.mem);
  auto CheckCode = [&](std::uint64_t vaddr, const std::uint32_t* code, std::size_t words, const char* what) {
    std::uint32_t got[4] = {};
    std::memcpy(got, ovr_base + vaddr, words * sizeof(std::uint32_t));
    if (std::memcmp(got, code, words * sizeof(std::uint32_t)) != 0) {
      std::fprintf(stderr, "libpnsovr %s: instruction bytes at %#llx differ\n", what,
                   static_cast<unsigned long long>(vaddr));
      QCHECK(false);
    }
  };
  CheckCode(PT::kDeferredFailedVaddr, PT::kDeferredFailedCode, 3, "CNSUser::DeferredLogInFailed");
  CheckCode(PT::kOfflineIdFnVaddr, PT::kOfflineIdFnCode, 3, "CNSOVRUser::OfflineID");
  CheckCode(PT::kUserNameCodeVaddr, PT::kUserNameCode, 2, "GotLoggedInUserCb user-name address");
  // The symbol DeferredLogInFailed resolves at the pinned vaddr (what ProveDeferredFailed requires).
  if (DynamicSymbolValue(ovr, PT::kDeferredFailedSymbol) != PT::kDeferredFailedVaddr) {
    std::fprintf(stderr, "libpnsovr %s: .dynsym value is not %#llx\n", PT::kDeferredFailedSymbol,
                 static_cast<unsigned long long>(PT::kDeferredFailedVaddr));
    QCHECK(false);
  }
  const char* msg = reinterpret_cast<const char*>(ovr_base + PT::kPrerequisitesMissingTextVaddr);
  if (std::strcmp(msg, nevr_quest_login::kPrerequisitesMissingText) != 0) {
    std::fprintf(stderr, "libpnsovr prerequisites-missing text differs from the production constant\n");
    QCHECK(false);
  }

  // CR15UIPage2EnablePageNode::Enter: the symbol the slot names is defined at the address the hook's comments give.
  if (DynamicSymbolValue(r15, pinned::kEnablePageNodeEnterSymbol) != 0x1fc210cULL) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not 0x1fc210c\n", pinned::kEnablePageNodeEnterSymbol);
    QCHECK(false);
  }
  // CR15NetGame::QuitOnError, which the login prompt hook calls (not a hook target): its symbol is defined at
  // the pinned address and its first four instructions are the ones the install proves before using it.
  if (DynamicSymbolValue(r15, pinned::kQuitOnErrorSymbol) != pinned::kQuitOnErrorVaddr) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not %#llx\n", pinned::kQuitOnErrorSymbol,
                 static_cast<unsigned long long>(pinned::kQuitOnErrorVaddr));
    QCHECK(false);
  }
  if (std::memcmp(static_cast<const unsigned char*>(r15.mem) + pinned::kQuitOnErrorVaddr, pinned::kQuitOnErrorCode,
                  sizeof(pinned::kQuitOnErrorCode)) != 0) {
    std::fprintf(stderr, "libr15 CR15NetGame::QuitOnError: first instructions differ from the pinned ones\n");
    QCHECK(false);
  }

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "got_pinned_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("got_pinned_test: all pinned targets resolve in the real ELFs\n");
  return 0;
}
