// Resolves the pinned Quest targets (src/quest/sentinel/pinned_targets.h and the login
// prerequisites' src/quest/login/login_prerequisite_targets.h) against the real libr15.so,
// libpnsradmatchmaking.so, libpnsovr.so and the login-failed page's script library, on any host.
//
// The ELF is laid out in memory the way a loader would (PT_LOAD segments copied to
// their vaddrs inside one anonymous mapping), so the production ResolveSlot walks
// the real dynamic section, 100k-entry relocation tables and string table with
// link-time-relative pointers, exactly as on Bionic. Nothing is executed or
// patched, and relocation numbers are passed as AArch64's, so an x86_64 host can
// check an AArch64 image.
//
// Run: got_pinned_test <libr15.so> <libpnsradmatchmaking.so> <libpnsovr.so> <lib7f2363b8976491aa.so>

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
#include "login_title_hook.h"
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

// True when a NOBITS section (.bss) of the file holds [vaddr, vaddr + size).
bool NobitsHolds(const LoadedElf& elf, std::uint64_t vaddr, std::uint64_t size) {
  const std::vector<char>& b = elf.bytes;
  Elf64_Ehdr eh;
  std::memcpy(&eh, b.data(), sizeof(eh));
  if (eh.e_shoff == 0 || eh.e_shoff + eh.e_shnum * sizeof(Elf64_Shdr) > b.size()) return false;
  std::vector<Elf64_Shdr> sh(eh.e_shnum);
  std::memcpy(sh.data(), b.data() + eh.e_shoff, eh.e_shnum * sizeof(Elf64_Shdr));
  for (const Elf64_Shdr& s : sh) {
    if (s.sh_type == SHT_NOBITS && s.sh_addr <= vaddr && vaddr + size <= s.sh_addr + s.sh_size) return true;
  }
  return false;
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

// `stubVaddr` is a PLT stub that loads `slot` (adrp x16, <page>; ldr x17, [x16, #<offset>]; add x16, x16, #<offset>;
// br x17), and returns every `b`/`bl` in the executable segments that reaches the stub.
std::vector<std::uint64_t> StubCallers(const LoadedElf& elf, std::uint64_t stubVaddr, std::uint64_t slot, const char* what) {
  std::uint32_t stub[4] = {};
  std::memcpy(stub, static_cast<const unsigned char*>(elf.mem) + stubVaddr, sizeof(stub));
  const std::uint64_t immlo = (stub[0] >> 29) & 0x3U;
  const std::uint64_t immhi = (stub[0] >> 5) & 0x7FFFFU;
  const std::uint64_t adrpPage = (stubVaddr & ~0xFFFULL) + (((immhi << 2) | immlo) << 12);
  const std::uint64_t ldrOffset = ((stub[1] >> 10) & 0xFFFU) * 8;
  if ((stub[0] & 0x9F00001FU) != 0x90000010U || adrpPage != (slot & ~0xFFFULL) || (stub[1] & 0xFFC003FFU) != 0xF9400211U ||
      ldrOffset != (slot & 0xFFFU) || stub[3] != 0xD61F0220U) {
    std::fprintf(stderr, "%s: PLT stub at %#llx does not load the slot %#llx\n", what,
                 static_cast<unsigned long long>(stubVaddr), static_cast<unsigned long long>(slot));
    QCHECK(false);
  }
  std::vector<std::uint64_t> callers;
  for (const Elf64_Phdr& ph : elf.phdrs) {
    if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0) continue;
    for (std::uint64_t at = ph.p_vaddr; at + 4 <= ph.p_vaddr + ph.p_filesz; at += 4) {
      std::uint32_t word = 0;
      std::memcpy(&word, static_cast<const unsigned char*>(elf.mem) + at, sizeof(word));
      if ((word & 0x7c000000U) != 0x14000000U) continue;  // b or bl
      std::int64_t imm = static_cast<std::int64_t>(word & 0x03ffffffU);
      if ((imm & 0x02000000) != 0) imm -= 0x04000000;
      if (at + static_cast<std::uint64_t>(imm * 4) == stubVaddr) callers.push_back(at);
    }
  }
  return callers;
}

// The symbol is defined at `vaddr` in .dynsym.
void CheckDefinedAt(const LoadedElf& elf, const char* symbol, std::uint64_t vaddr) {
  if (DynamicSymbolValue(elf, symbol) != vaddr) {
    std::fprintf(stderr, "%s: .dynsym value is not %#llx\n", symbol, static_cast<unsigned long long>(vaddr));
    QCHECK(false);
  }
}

void CheckBuildId(const LoadedElf& elf, const char* expected) {
  char actual[64] = {};
  QCHECK(ReadBuildId(elf.image, actual, sizeof(actual)));
  QCHECK(std::strcmp(actual, expected) == 0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::fprintf(stderr,
                 "usage: got_pinned_test <libr15.so> <libpnsradmatchmaking.so> <libpnsovr.so> <lib7f2363b8976491aa.so> "
                 "<libpnsrad.so>\n");
    return 2;
  }
  LoadedElf r15, mm, ovr, script, rad;
  if (!Load(argv[1], &r15) || !Load(argv[2], &mm) || !Load(argv[3], &ovr) || !Load(argv[4], &script) || !Load(argv[5], &rad)) {
    std::fprintf(stderr, "got_pinned_test: cannot load an AArch64 ELF64 from the given paths\n");
    return 2;
  }
  CheckBuildId(r15, pinned::kLibR15StockBuildId);
  CheckBuildId(mm, pinned::kMatchmakingBuildId);
  CheckBuildId(ovr, nevr_quest_login::PrerequisiteTargets::kPnsovrBuildId);

  CheckTarget(r15, pinned::LibR15ClockGettime(), "libr15 clock_gettime JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15Sigaction(), "libr15 sigaction JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15Signal(), "libr15 signal JUMP_SLOT");
  // The provider libraries the crash-first hook reaches after they load (#1605): both slots of each, and each build ID.
  CheckBuildId(rad, pinned::kPnsradBuildId);
  for (const pinned::CrashLateModule& module : pinned::kCrashLateModules) {
    const LoadedElf& elf = std::strcmp(module.module, pinned::kPnsrad) == 0 ? rad : (std::strcmp(module.module, pinned::kPnsovr) == 0 ? ovr : mm);
    CheckTarget(elf, pinned::CrashLateSigaction(module), module.module);
    CheckTarget(elf, pinned::CrashLateSignal(module), module.module);
  }
  CheckTarget(r15, pinned::LibR15TString(), "libr15 CJson::TString JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15CreateConnection(), "libr15 CSysHttp::CreateConnection JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15ConfigRequestSend(), "libr15 SNSConfigRequestv24Send JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15SetDelimitedErrorMessage(), "libr15 CR15NetGame::SetDelimitedErrorMessage JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15NetGameUpdate(), "libr15 CR15NetGame::Update JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15AndroidMain(), "libr15 android_main JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15EnablePageNodeEnter(), "libr15 CR15UIPage2EnablePageNode::Enter JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15RequestPermissions(), "libr15 CSysInfo::RequestPermissions JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15CheckPermission(), "libr15 CSysInfo::CheckPermission JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15OSBuildVersion(), "libr15 CSysInfo::OSBuildVersion JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15ProviderCreate(), "libr15 CNSProvider::Create JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15ProviderDestroy(), "libr15 CNSProvider::Destroy JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15ProviderId(), "libr15 CNSProvider::ProviderID JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15UserProviderId(), "libr15 CNSProvider::UserProviderID JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15ShowSystemUI(), "libr15 CVR::ShowSystemUI JUMP_SLOT");
  // The Texture Resolution control (#1012): the five hooked functions' slots, and (below) the six functions the live apply calls.
  CheckTarget(r15, pinned::LibR15JsonDecodeFromFile(), "libr15 CJson::DecodeFrom(CFileSp const&, unsigned) JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15JsonDecodeFromText(), "libr15 CJson::DecodeFrom(char const*, unsigned long long) JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15ValidateGfxSettings(), "libr15 NRadGame::ValidateGfxSettings JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15ApplyRuntimeGfxSettings(), "libr15 NRadGame::ApplyRuntimeGfxSettings JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15CheckForResize(), "libr15 CEngine::CheckForResize JUMP_SLOT");
  // The free of GPU ranges (#1759): its slot, its first words, and the words the guard's layout rests on.
  CheckTarget(r15, pinned::LibR15VkFree(), "libr15 CGRawInterfaceVK::Free(VkDeviceMemory_T*, ...) JUMP_SLOT");
  for (const pinned::PinnedCode* block :
       {&pinned::kVkFreeCountBlock, &pinned::kVkFreeAppendBlock, &pinned::kVkFreeAppendStore}) {
    if (std::memcmp(static_cast<const unsigned char*>(r15.mem) + block->vaddr, block->code, block->words * sizeof(std::uint32_t)) != 0) {
      std::fprintf(stderr, "libr15 %s: the words at %#llx differ from the pinned ones\n", block->name,
                   static_cast<unsigned long long>(block->vaddr));
      QCHECK(false);
    }
  }
  // The array and its count lie in .bss, and the count follows the last of 512 entries.
  QCHECK(NobitsHolds(r15, pinned::vk_hole_layout::kTableVaddr,
                     pinned::vk_hole_layout::kCountVaddr + sizeof(std::uint32_t) - pinned::vk_hole_layout::kTableVaddr));
  for (const pinned::PinnedFunction* f :
       {&pinned::kRefreshRTSize, &pinned::kEngineResize, &pinned::kVrInitialized, &pinned::kVrGraphicsInitialized,
        &pinned::kVrDisplayLost, &pinned::kLocalAppDataDirectory, &pinned::kVkFree}) {
    if (DynamicSymbolValue(r15, f->name) != f->vaddr) {
      std::fprintf(stderr, "libr15 %s: .dynsym value is not %#llx\n", f->name, static_cast<unsigned long long>(f->vaddr));
      QCHECK(false);
    }
    if (std::memcmp(static_cast<const unsigned char*>(r15.mem) + f->vaddr, f->code, f->words * sizeof(std::uint32_t)) != 0) {
      std::fprintf(stderr, "libr15 %s: first instructions differ from the pinned ones\n", f->name);
      QCHECK(false);
    }
  }
  CheckTarget(r15, pinned::LibR15CanvasSetText(), "libr15 CCanvasUICS::SetText(char const*) JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15MountObb(), "libr15 AStorageManager_mountObb JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15GetMountedObbPath(), "libr15 AStorageManager_getMountedObbPath JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15GetDataRootDir(), "libr15 CSysInfo::GetDataRootDir JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15FindMethod(), "libr15 CSysModule::FindMethod JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15GetDeviceExtensionsVulkan(), "libr15 vrapi_GetDeviceExtensionsVulkan JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15GetSystemPropertyInt(), "libr15 vrapi_GetSystemPropertyInt JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15SetDisplayRefreshRate(), "libr15 vrapi_SetDisplayRefreshRate JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15SetClockLevels(), "libr15 vrapi_SetClockLevels JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15SetPropertyInt(), "libr15 vrapi_SetPropertyInt JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15GetSystemPropertyFloat(), "libr15 vrapi_GetSystemPropertyFloat JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15SubmitFrame2(), "libr15 vrapi_SubmitFrame2 JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15GetCurrentInputState(), "libr15 vrapi_GetCurrentInputState JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15GetInputDeviceCapabilities(), "libr15 vrapi_GetInputDeviceCapabilities JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15FillResourceFileRequest(), "libr15 CResourceManager::FillResourceFileRequest JUMP_SLOT");
  CheckTarget(r15, pinned::LibR15BindNodeGlobDat(), "libr15 BindNode GLOB_DAT");
  CheckTarget(r15, pinned::LibR15LookupDataBindingGlobDat(), "libr15 LookupDataBinding GLOB_DAT");
  CheckTarget(mm, pinned::MatchmakingTString(), "libpnsradmatchmaking CJson::TString JUMP_SLOT");

  // The login-failed page's title (#1137): the script library has no build ID, so the slot is pinned by module, symbol
  // and address, the symbol is defined in the library itself (string_t::string_t(char const*), ReVault 0x10df98 = link
  // address 0xdf98), and the literal the hook replaces is in the file.
  CheckTarget(script, pinned::LoginScriptStringCtor(), "lib7f2363b8976491aa string_t::string_t(char const*) JUMP_SLOT");
  if (DynamicSymbolValue(script, pinned::kStringTCtorSymbol) != 0xdf98ULL) {
    std::fprintf(stderr, "lib7f2363b8976491aa %s: .dynsym value is not 0xdf98\n", pinned::kStringTCtorSymbol);
    QCHECK(false);
  }
  const std::vector<char> scriptFile = ReadAll(argv[4]);
  QCHECK(HoldsString(scriptFile, nevr_quest::login_title::kGameTitle));

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

  // CResourceManager::AsyncResourceIOCallback (#966): the GLOB_DAT slot resolves at its pin, the symbol it names is defined
  // where the hook's comments say, ResourceLoadInternal loads the slot at the two pinned sites (`ldr x4, [x4, #1464]`, the
  // GOT page's 0x5b8 offset into 0x37195b8, passed on as the read-finished callback), and the memory context the hook
  // reads to find the allocator FreeResource releases through is exported at its pin.
  CheckTarget(r15, pinned::LibR15AsyncResourceIOCallback(), "libr15 CResourceManager::AsyncResourceIOCallback GLOB_DAT");
  if (DynamicSymbolValue(r15, pinned::kAsyncResourceIOCallbackSymbol) != 0x2543448ULL) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not 0x2543448\n", pinned::kAsyncResourceIOCallbackSymbol);
    QCHECK(false);
  }
  const unsigned char* const r15_base = static_cast<const unsigned char*>(r15.mem);
  for (const std::uint64_t site : {0x254391cULL, 0x25439acULL}) {
    std::uint32_t word = 0;
    std::memcpy(&word, r15_base + site, sizeof(word));
    if (word != 0xF942DC84u) {
      std::fprintf(stderr, "libr15 ResourceLoadInternal: word at %#llx is %#x, not ldr x4,[x4,#1464]\n",
                   static_cast<unsigned long long>(site), word);
      QCHECK(false);
    }
  }
  if (DynamicSymbolValue(r15, pinned::kMemoryContextSymbol) != pinned::kMemoryContextVaddr) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not %#llx\n", pinned::kMemoryContextSymbol,
                 static_cast<unsigned long long>(pinned::kMemoryContextVaddr));
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

  // CR15NetGame::BeginLogIn, which the login prompt hook calls once the device code is accepted (#672; not a hook
  // target): its symbol is defined at the pinned address and its first four instructions are the ones the install
  // proves before using it. (ReVault lists libr15 at these addresses plus 0x100000: the first headset run of the
  // restart found the pin 0x100000 too high, `prologue_mismatch`.)
  if (DynamicSymbolValue(r15, pinned::kBeginLogInSymbol) != pinned::kBeginLogInVaddr) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not %#llx\n", pinned::kBeginLogInSymbol,
                 static_cast<unsigned long long>(pinned::kBeginLogInVaddr));
    QCHECK(false);
  }
  if (std::memcmp(static_cast<const unsigned char*>(r15.mem) + pinned::kBeginLogInVaddr, pinned::kBeginLogInCode,
                  sizeof(pinned::kBeginLogInCode)) != 0) {
    std::fprintf(stderr, "libr15 CR15NetGame::BeginLogIn: first instructions differ from the pinned ones\n");
    QCHECK(false);
  }

  // CncaGameSpace::SendComponentEvent, through which the hook sends the RETRY select after BeginLogIn (#672): the
  // first four instructions are the ones the install proves before calling it.
  if (std::memcmp(static_cast<const unsigned char*>(r15.mem) + pinned::kSendComponentEventVaddr,
                  pinned::kSendComponentEventCode, sizeof(pinned::kSendComponentEventCode)) != 0) {
    std::fprintf(stderr, "libr15 CncaGameSpace::SendComponentEvent: first instructions differ from the pinned ones\n");
    QCHECK(false);
  }

  // NR15Logger::MetricLog, the wizard observer's JUMP_SLOT (ADR 0017, #966): the slot resolves at its pin, the symbol is
  // defined at the pinned address, and the first four instructions are the ones the observer proves before it installs.
  CheckTarget(r15, pinned::LibR15MetricLog(), "libr15 NR15Logger::MetricLog JUMP_SLOT");
  if (DynamicSymbolValue(r15, pinned::kMetricLogSymbol) != pinned::kMetricLogVaddr) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not %#llx\n", pinned::kMetricLogSymbol,
                 static_cast<unsigned long long>(pinned::kMetricLogVaddr));
    QCHECK(false);
  }
  if (std::memcmp(static_cast<const unsigned char*>(r15.mem) + pinned::kMetricLogVaddr, pinned::kMetricLogCode,
                  sizeof(pinned::kMetricLogCode)) != 0) {
    std::fprintf(stderr, "libr15 NR15Logger::MetricLog: first instructions differ from the pinned ones\n");
    QCHECK(false);
  }

  // The main-menu join (#966 S4): the two functions it calls, at their pinned addresses, with the prologue words ReVault reads
  // (0x1301be4 BeginMultiplayer, 0x13005fc EnableTransitionMenu; ReVault = pinned + 0x100000). The join checks the same words.
  if (std::memcmp(static_cast<const unsigned char*>(r15.mem) + pinned::kBeginMultiplayerVaddr, pinned::kBeginMultiplayerCode,
                  sizeof(pinned::kBeginMultiplayerCode)) != 0) {
    std::fprintf(stderr, "libr15 CR15Game::BeginMultiplayer: first instructions differ from the pinned ones\n");
    QCHECK(false);
  }
  if (std::memcmp(static_cast<const unsigned char*>(r15.mem) + pinned::kEnableTransitionMenuVaddr,
                  pinned::kEnableTransitionMenuCode, sizeof(pinned::kEnableTransitionMenuCode)) != 0) {
    std::fprintf(stderr, "libr15 CR15Game::EnableTransitionMenu: first instructions differ from the pinned ones\n");
    QCHECK(false);
  }

  // The UI text writer's three game calls (runtime/patch/ui_text.h, quest/invite/ui_text_quest.cpp): each at its pinned address with
  // the words the writer checks. The Itanium-mangled name of each is the .dynsym entry at that address.
  const struct {
    const char* name;
    std::uint64_t vaddr;
    const std::uint32_t* words;
    std::size_t count;
    const char* symbol;
  } textCalls[] = {
      {"CncaGame::GameSpace", pinned::kGameSpaceVaddr, pinned::kGameSpaceCode, sizeof(pinned::kGameSpaceCode) / 4,
       "_ZN10NRadEngine8CncaGame9GameSpaceEy"},
      {"CLocalComponentHandle<CCanvasUICS>::Find", pinned::kCanvasFindVaddr, pinned::kCanvasFindCode,
       sizeof(pinned::kCanvasFindCode) / 4,
       "_ZN10NRadEngine21CLocalComponentHandleINS_11CCanvasUICSEE4FindEPNS_15CncaGameSpaceMTENS_6SActorENS_9CSymbol64E"},
      {"CCanvasUICS::SetText", pinned::kCanvasSetTextVaddr, pinned::kCanvasSetTextCode, sizeof(pinned::kCanvasSetTextCode) / 4,
       pinned::kCanvasSetTextSymbol},
  };
  for (const auto& call : textCalls) {
    if (DynamicSymbolValue(r15, call.symbol) != call.vaddr) {
      std::fprintf(stderr, "libr15 %s: .dynsym value is not %#llx\n", call.name, static_cast<unsigned long long>(call.vaddr));
      QCHECK(false);
    }
    if (std::memcmp(static_cast<const unsigned char*>(r15.mem) + call.vaddr, call.words, call.count * 4) != 0) {
      std::fprintf(stderr, "libr15 %s: first instructions differ from the pinned ones\n", call.name);
      QCHECK(false);
    }
  }

  // CGTextureResource::ForceLoadAllMips, which the UI texture hook calls through the address it proves: its first
  // four instructions are the ones ui_texture_mips.cpp checks before calling it. The inline hook rows (the widget
  // update, the lobby-full handlers, the texture attach) are not pinned here: the patcher pinned test checks every row's
  // stock words against the real file (libr15_patcher_pinned_test.cpp).
  if (std::memcmp(static_cast<const unsigned char*>(r15.mem) + pinned::kForceLoadAllMipsVaddr,
                  pinned::kForceLoadAllMipsCode, sizeof(pinned::kForceLoadAllMipsCode)) != 0) {
    std::fprintf(stderr,
                 "libr15 CGTextureResource::ForceLoadAllMips: first instructions at %#llx differ from the pinned ones\n",
                 static_cast<unsigned long long>(pinned::kForceLoadAllMipsVaddr));
    QCHECK(false);
  }

  // The permission-wait hook (#640, #758): RequestPermissions is defined where the slot's comment says, and
  // CheckPermission, which the hook calls and also hooks (its JUMP_SLOT is checked above), is defined at the pinned
  // address with the first four instructions the install proves before using it.
  if (DynamicSymbolValue(r15, pinned::kRequestPermissionsSymbol) != 0xf873ecULL) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not 0xf873ec\n", pinned::kRequestPermissionsSymbol);
    QCHECK(false);
  }
  // The first word the hook classifies (ClassifyOriginalWord, #1737 C2): the symbol is defined at the pinned address, and the
  // store file's word there is the stock word, `sub sp,sp,#0x40`.
  if (DynamicSymbolValue(r15, pinned::kRequestPermissionsSymbol) != pinned::kRequestPermissionsVaddr) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not %#llx\n", pinned::kRequestPermissionsSymbol,
                 static_cast<unsigned long long>(pinned::kRequestPermissionsVaddr));
    QCHECK(false);
  }
  {
    std::uint32_t firstWord = 0;
    std::memcpy(&firstWord, static_cast<const unsigned char*>(r15.mem) + pinned::kRequestPermissionsVaddr, sizeof(firstWord));
    if (firstWord != pinned::kRequestPermissionsStockWord) {
      std::fprintf(stderr, "libr15 CSysInfo::RequestPermissions: first word is %#x, not the store file's stock word\n", firstWord);
      QCHECK(false);
    }
  }
  if (DynamicSymbolValue(r15, pinned::kCheckPermissionSymbol) != pinned::kCheckPermissionVaddr) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not %#llx\n", pinned::kCheckPermissionSymbol,
                 static_cast<unsigned long long>(pinned::kCheckPermissionVaddr));
    QCHECK(false);
  }
  if (std::memcmp(static_cast<const unsigned char*>(r15.mem) + pinned::kCheckPermissionVaddr,
                  pinned::kCheckPermissionCode, sizeof(pinned::kCheckPermissionCode)) != 0) {
    std::fprintf(stderr, "libr15 CSysInfo::CheckPermission: first instructions differ from the pinned ones\n");
    QCHECK(false);
  }

  // The OS build floor (#945): OSBuildVersion and the floor are defined where pinned_targets.h says, the floor has the
  // value the hook's comments give, the PLT stub loads the hooked slot, and the stub has exactly one caller in the whole
  // executable segment, the `bl` in CR15NetGame::Initialize. A second caller would also be answered "no number".
  if (DynamicSymbolValue(r15, pinned::kOSBuildVersionSymbol) != pinned::kOSBuildVersionVaddr) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not %#llx\n", pinned::kOSBuildVersionSymbol,
                 static_cast<unsigned long long>(pinned::kOSBuildVersionVaddr));
    QCHECK(false);
  }
  if (DynamicSymbolValue(r15, pinned::kMinOSBuildVersionSymbol) != pinned::kMinOSBuildVersionVaddr) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not %#llx\n", pinned::kMinOSBuildVersionSymbol,
                 static_cast<unsigned long long>(pinned::kMinOSBuildVersionVaddr));
    QCHECK(false);
  }
  {
    std::uint64_t floor = 0;
    std::memcpy(&floor, static_cast<const unsigned char*>(r15.mem) + pinned::kMinOSBuildVersionVaddr, sizeof(floor));
    if (floor != pinned::kMinOSBuildVersionValue) {
      std::fprintf(stderr, "libr15 kMinOSBuildVersion is %llu, not %llu\n", static_cast<unsigned long long>(floor),
                   static_cast<unsigned long long>(pinned::kMinOSBuildVersionValue));
      QCHECK(false);
    }
    // The stub: adrp x16, <page of the slot>; ldr x17, [x16, #<slot offset>]; add x16, x16, #<slot offset>; br x17.
    std::uint32_t stub[4] = {};
    std::memcpy(stub, static_cast<const unsigned char*>(r15.mem) + pinned::kOSBuildVersionStub, sizeof(stub));
    const std::uint64_t slot = pinned::LibR15OSBuildVersion().slotVaddr.value_or(0);
    const std::uint64_t page = slot & ~0xFFFULL;
    const std::uint64_t immlo = (stub[0] >> 29) & 0x3U;
    const std::uint64_t immhi = (stub[0] >> 5) & 0x7FFFFU;
    const std::uint64_t adrpPage = (pinned::kOSBuildVersionStub & ~0xFFFULL) + (((immhi << 2) | immlo) << 12);
    const std::uint64_t ldrOffset = ((stub[1] >> 10) & 0xFFFU) * 8;
    if ((stub[0] & 0x9F00001FU) != 0x90000010U || adrpPage != page || (stub[1] & 0xFFC003FFU) != 0xF9400211U ||
        ldrOffset != (slot & 0xFFFU) || stub[3] != 0xD61F0220U) {
      std::fprintf(stderr, "libr15 OSBuildVersion PLT stub at %#llx does not load the slot %#llx\n",
                   static_cast<unsigned long long>(pinned::kOSBuildVersionStub), static_cast<unsigned long long>(slot));
      QCHECK(false);
    }
    std::vector<std::uint64_t> callers;
    for (const Elf64_Phdr& ph : r15.phdrs) {
      if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0) continue;
      for (std::uint64_t at = ph.p_vaddr; at + 4 <= ph.p_vaddr + ph.p_filesz; at += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, static_cast<const unsigned char*>(r15.mem) + at, sizeof(word));
        if ((word & 0x7c000000U) != 0x14000000U) continue;  // b or bl
        std::int64_t imm = static_cast<std::int64_t>(word & 0x03ffffffU);
        if ((imm & 0x02000000) != 0) imm -= 0x04000000;
        if (at + static_cast<std::uint64_t>(imm * 4) == pinned::kOSBuildVersionStub) callers.push_back(at);
      }
    }
    if (callers.size() != 1 || callers[0] != pinned::kOSBuildVersionCallSite) {
      std::fprintf(stderr, "libr15 OSBuildVersion stub has %zu b/bl callers, not the one at %#llx\n", callers.size(),
                   static_cast<unsigned long long>(pinned::kOSBuildVersionCallSite));
      for (const std::uint64_t at : callers) std::fprintf(stderr, "  caller %#llx\n", static_cast<unsigned long long>(at));
      QCHECK(false);
    }
  }

  // The platform provider (#1220): each provider function and the two functions the hooks call are defined where
  // pinned_targets.h says; each stub loads its hooked slot; Create's and Destroy's stubs have exactly the call sites
  // pinned_targets.h names (CR15Game::Initialize, ShutdownEngine), and ProviderID's and UserProviderID's the counts it gives.
  CheckDefinedAt(r15, pinned::kProviderCreateSymbol, pinned::kProviderCreateVaddr);
  CheckDefinedAt(r15, pinned::kProviderDestroySymbol, pinned::kProviderDestroyVaddr);
  CheckDefinedAt(r15, pinned::kProviderIdSymbol, pinned::kProviderIdVaddr);
  CheckDefinedAt(r15, pinned::kUserProviderIdSymbol, pinned::kUserProviderIdVaddr);
  CheckDefinedAt(r15, pinned::kSysModuleUnloadSymbol, pinned::kSysModuleUnloadVaddr);
  CheckDefinedAt(r15, pinned::kCJsonTStringSymbol, pinned::kCJsonTStringVaddr);
  {
    struct Expected {
      const char* what;
      std::uint64_t stub;
      std::uint64_t slot;
      std::vector<std::uint64_t> sites;  // empty: only the count is pinned
      std::size_t count;
    };
    const Expected expected[] = {
        {"libr15 CNSProvider::Create", pinned::kProviderCreateStub, *pinned::LibR15ProviderCreate().slotVaddr,
         std::vector<std::uint64_t>(std::begin(pinned::kProviderCreateCallSites), std::end(pinned::kProviderCreateCallSites)),
         std::size(pinned::kProviderCreateCallSites)},
        {"libr15 CNSProvider::Destroy", pinned::kProviderDestroyStub, *pinned::LibR15ProviderDestroy().slotVaddr,
         std::vector<std::uint64_t>(std::begin(pinned::kProviderDestroyCallSites),
                                    std::end(pinned::kProviderDestroyCallSites)),
         std::size(pinned::kProviderDestroyCallSites)},
        {"libr15 CNSProvider::ProviderID", pinned::kProviderIdStub, *pinned::LibR15ProviderId().slotVaddr, {},
         pinned::kProviderIdCallers},
        {"libr15 CNSProvider::UserProviderID", pinned::kUserProviderIdStub, *pinned::LibR15UserProviderId().slotVaddr, {},
         pinned::kUserProviderIdCallers},
    };
    for (const Expected& e : expected) {
      const std::vector<std::uint64_t> callers = StubCallers(r15, e.stub, e.slot, e.what);
      if (callers.size() != e.count || (!e.sites.empty() && callers != e.sites)) {
        std::fprintf(stderr, "%s: stub has %zu b/bl callers, not the %zu pinned\n", e.what, callers.size(), e.count);
        for (const std::uint64_t at : callers) std::fprintf(stderr, "  caller %#llx\n", static_cast<unsigned long long>(at));
        QCHECK(false);
      }
    }
  }

  // The main-menu QUIT (#1276): CVR::ShowSystemUI is defined where pinned_targets.h says, its PLT stub loads the hooked
  // slot, and the stub has exactly one caller in the whole executable segment, state 0 of
  // CR15VRMenuWidgetCS::UpdateExclusive. A second caller would also finish the activity.
  if (DynamicSymbolValue(r15, pinned::kShowSystemUISymbol) != pinned::kShowSystemUIVaddr) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not %#llx\n", pinned::kShowSystemUISymbol,
                 static_cast<unsigned long long>(pinned::kShowSystemUIVaddr));
    QCHECK(false);
  }
  {
    std::uint32_t stub[4] = {};
    std::memcpy(stub, static_cast<const unsigned char*>(r15.mem) + pinned::kShowSystemUIStub, sizeof(stub));
    const std::uint64_t slot = pinned::LibR15ShowSystemUI().slotVaddr.value_or(0);
    const std::uint64_t page = slot & ~0xFFFULL;
    const std::uint64_t immlo = (stub[0] >> 29) & 0x3U;
    const std::uint64_t immhi = (stub[0] >> 5) & 0x7FFFFU;
    const std::uint64_t adrpPage = (pinned::kShowSystemUIStub & ~0xFFFULL) + (((immhi << 2) | immlo) << 12);
    const std::uint64_t ldrOffset = ((stub[1] >> 10) & 0xFFFU) * 8;
    if ((stub[0] & 0x9F00001FU) != 0x90000010U || adrpPage != page || (stub[1] & 0xFFC003FFU) != 0xF9400211U ||
        ldrOffset != (slot & 0xFFFU) || stub[3] != 0xD61F0220U) {
      std::fprintf(stderr, "libr15 ShowSystemUI PLT stub at %#llx does not load the slot %#llx\n",
                   static_cast<unsigned long long>(pinned::kShowSystemUIStub), static_cast<unsigned long long>(slot));
      QCHECK(false);
    }
    std::vector<std::uint64_t> callers;
    for (const Elf64_Phdr& ph : r15.phdrs) {
      if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0) continue;
      for (std::uint64_t at = ph.p_vaddr; at + 4 <= ph.p_vaddr + ph.p_filesz; at += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, static_cast<const unsigned char*>(r15.mem) + at, sizeof(word));
        if ((word & 0x7c000000U) != 0x14000000U) continue;  // b or bl
        std::int64_t imm = static_cast<std::int64_t>(word & 0x03ffffffU);
        if ((imm & 0x02000000) != 0) imm -= 0x04000000;
        if (at + static_cast<std::uint64_t>(imm * 4) == pinned::kShowSystemUIStub) callers.push_back(at);
      }
    }
    if (callers.size() != 1 || callers[0] != pinned::kShowSystemUICallSite) {
      std::fprintf(stderr, "libr15 ShowSystemUI stub has %zu b/bl callers, not the one at %#llx\n", callers.size(),
                   static_cast<unsigned long long>(pinned::kShowSystemUICallSite));
      for (const std::uint64_t at : callers) std::fprintf(stderr, "  caller %#llx\n", static_cast<unsigned long long>(at));
      QCHECK(false);
    }
  }

  // The Quest menu version (#747): CCanvasUICS::SetText(char const*) is defined where pinned_targets.h says, the PLT stub loads
  // the hooked slot, every call in the executable segments reaches the function through that stub (kCanvasSetTextStubCallers
  // `b`/`bl` to the stub, none to the function itself), and the slot lies in a writable segment that is not executable: the
  // install writes a data page and never a code page (docs/adr/0021).
  if (DynamicSymbolValue(r15, pinned::kCanvasSetTextSymbol) != pinned::kCanvasSetTextVaddr) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not %#llx\n", pinned::kCanvasSetTextSymbol,
                 static_cast<unsigned long long>(pinned::kCanvasSetTextVaddr));
    QCHECK(false);
  }
  {
    std::uint32_t stub[4] = {};
    std::memcpy(stub, static_cast<const unsigned char*>(r15.mem) + pinned::kCanvasSetTextStub, sizeof(stub));
    const std::uint64_t slot = pinned::LibR15CanvasSetText().slotVaddr.value_or(0);
    const std::uint64_t page = slot & ~0xFFFULL;
    const std::uint64_t immlo = (stub[0] >> 29) & 0x3U;
    const std::uint64_t immhi = (stub[0] >> 5) & 0x7FFFFU;
    const std::uint64_t adrpPage = (pinned::kCanvasSetTextStub & ~0xFFFULL) + (((immhi << 2) | immlo) << 12);
    const std::uint64_t ldrOffset = ((stub[1] >> 10) & 0xFFFU) * 8;
    if ((stub[0] & 0x9F00001FU) != 0x90000010U || adrpPage != page || (stub[1] & 0xFFC003FFU) != 0xF9400211U ||
        ldrOffset != (slot & 0xFFFU) || stub[3] != 0xD61F0220U) {
      std::fprintf(stderr, "libr15 CCanvasUICS::SetText PLT stub at %#llx does not load the slot %#llx\n",
                   static_cast<unsigned long long>(pinned::kCanvasSetTextStub), static_cast<unsigned long long>(slot));
      QCHECK(false);
    }
    std::size_t viaStub = 0;
    std::size_t direct = 0;
    for (const Elf64_Phdr& ph : r15.phdrs) {
      if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0) continue;
      for (std::uint64_t at = ph.p_vaddr; at + 4 <= ph.p_vaddr + ph.p_filesz; at += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, static_cast<const unsigned char*>(r15.mem) + at, sizeof(word));
        if ((word & 0x7c000000U) != 0x14000000U) continue;  // b or bl
        std::int64_t imm = static_cast<std::int64_t>(word & 0x03ffffffU);
        if ((imm & 0x02000000) != 0) imm -= 0x04000000;
        const std::uint64_t target = at + static_cast<std::uint64_t>(imm * 4);
        if (target == pinned::kCanvasSetTextStub) ++viaStub;
        if (target == pinned::kCanvasSetTextVaddr) ++direct;
      }
    }
    if (viaStub != pinned::kCanvasSetTextStubCallers || direct != 0) {
      std::fprintf(stderr, "libr15 CCanvasUICS::SetText: %zu b/bl to the stub (want %zu), %zu to the function (want 0)\n",
                   viaStub, static_cast<std::size_t>(pinned::kCanvasSetTextStubCallers), direct);
      QCHECK(false);
    }
    bool slotInDataSegment = false;
    for (const Elf64_Phdr& ph : r15.phdrs) {
      if (ph.p_type != PT_LOAD || slot < ph.p_vaddr || slot + 8 > ph.p_vaddr + ph.p_memsz) continue;
      slotInDataSegment = (ph.p_flags & PF_W) != 0 && (ph.p_flags & PF_X) == 0;
    }
    if (!slotInDataSegment) {
      std::fprintf(stderr, "libr15 CCanvasUICS::SetText slot %#llx is not in a writable, non-executable segment\n",
                   static_cast<unsigned long long>(slot));
      QCHECK(false);
    }
  }

  // CResourceManager::FillResourceFileRequest (#871): the symbol the slot names is defined at the address the hook's comments
  // give, and each of the six call sites the hook names is a `bl` to the PLT stub that loads the slot (the stub address
  // is what the call-site table is relative to).
  if (DynamicSymbolValue(r15, pinned::kFillResourceFileRequestSymbol) != 0x25439f0ULL) {
    std::fprintf(stderr, "libr15 %s: .dynsym value is not 0x25439f0\n", pinned::kFillResourceFileRequestSymbol);
    QCHECK(false);
  }
  for (const pinned::FillCallSite& site : pinned::kFillCallSites) {
    std::uint32_t word = 0;
    std::memcpy(&word, static_cast<const unsigned char*>(r15.mem) + site.bl, sizeof(word));
    std::int64_t imm = static_cast<std::int64_t>(word & 0x03ffffffU);
    if ((imm & 0x02000000) != 0) imm -= 0x04000000;  // 26-bit signed word offset
    const std::uint64_t target = site.bl + static_cast<std::uint64_t>(imm * 4);
    if ((word & 0xfc000000U) != 0x94000000U || target != pinned::kFillResourceFileRequestStub) {
      std::fprintf(stderr, "libr15 FillResourceFileRequest call site %s at %#llx is not a bl to the stub (word %#x)\n", site.name,
                   static_cast<unsigned long long>(site.bl), word);
      QCHECK(false);
    }
  }

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "got_pinned_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("got_pinned_test: all pinned targets resolve in the real ELFs\n");
  return 0;
}
