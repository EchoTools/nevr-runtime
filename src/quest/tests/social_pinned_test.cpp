// Checks the social ABI pins against the real pinned libraries, on any host.
//
//   - libr15.so: build id, and the JUMP_SLOT for CNSProvider::Social at the pinned address.
//   - libr15.so: CJson::Reset, DecodeFrom and EncodeToCompact are the dynamic symbols at kLibR15CJson*Vaddr (sizes and
//     first instructions checked), ResolveGameJson finds them in the image, and SUuid::kInvalid is in .bss (zero at load).
//   - libpnsovr.so: build id, the CNSOVRSocial vtable symbol (address and size), and every slot of
//     the vtable, read from the library's own R_AARCH64_ABS64 relocations, against kSlotNames.
//
// The ELF is laid out in memory the way a loader would (PT_LOAD segments copied to their vaddrs), so
// the production ResolveSlot walks the real dynamic section. Nothing is executed or patched.
//
// Run: social_pinned_test <libr15.so> <libpnsovr.so>
//      social_pinned_test --dump <libpnsovr.so>      prints fixtures/cnsovrsocial_vtable.txt

#include <elf.h>
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "got_hook.h"
#include "pinned_targets.h"
#include "quest/social/social_abi.h"
#include "quest/social/social_install.h"
#include "quest/social/social_invite_gate.h"
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
  const std::uint8_t* At(std::uint64_t vaddr) const { return static_cast<const std::uint8_t*>(mem) + vaddr; }
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

// The dynamic tables of an image, by link-time address.
struct Dynamic {
  const Elf64_Rela* rela = nullptr;
  std::size_t relaCount = 0;
  const Elf64_Sym* symtab = nullptr;
  const char* strtab = nullptr;
  std::size_t symCount = 0;
};

bool ReadDynamic(const LoadedElf& elf, Dynamic* out) {
  for (const Elf64_Phdr& ph : elf.phdrs) {
    if (ph.p_type != PT_DYNAMIC) continue;
    std::uint64_t rela = 0, relasz = 0, symtab = 0, strtab = 0;
    for (const Elf64_Dyn* d = reinterpret_cast<const Elf64_Dyn*>(elf.At(ph.p_vaddr)); d->d_tag != DT_NULL; ++d) {
      if (d->d_tag == DT_RELA) rela = d->d_un.d_ptr;
      if (d->d_tag == DT_RELASZ) relasz = d->d_un.d_val;
      if (d->d_tag == DT_SYMTAB) symtab = d->d_un.d_ptr;
      if (d->d_tag == DT_STRTAB) strtab = d->d_un.d_ptr;
    }
    if (rela == 0 || symtab == 0 || strtab == 0) return false;
    out->rela = reinterpret_cast<const Elf64_Rela*>(elf.At(rela));
    out->relaCount = relasz / sizeof(Elf64_Rela);
    out->symtab = reinterpret_cast<const Elf64_Sym*>(elf.At(symtab));
    out->strtab = reinterpret_cast<const char*>(elf.At(strtab));
    out->symCount = (strtab - symtab) / sizeof(Elf64_Sym);  // .dynstr follows .dynsym in these libraries
    return true;
  }
  return false;
}

// "_ZN10NRadEngine12CNSOVRSocial11SwapMembersEjj" -> "CNSOVRSocial11SwapMembersEjj" (also _ZNK).
std::string Strip(const char* mangled) {
  std::string s(mangled);
  const char* prefixes[] = {"_ZNK10NRadEngine", "_ZN10NRadEngine"};
  for (const char* p : prefixes) {
    if (s.rfind(p, 0) == 0) {
      s.erase(0, std::strlen(p));
      std::size_t digits = 0;
      while (digits < s.size() && s[digits] >= '0' && s[digits] <= '9') ++digits;
      s.erase(0, digits);
      return s;
    }
  }
  return s;
}

std::vector<std::string> VtableMethods(const LoadedElf& pnsovr, std::size_t* sizeOut, std::uint64_t* symValue) {
  std::vector<std::string> methods;
  Dynamic dyn;
  if (!ReadDynamic(pnsovr, &dyn)) return methods;
  const Elf64_Sym* vt = nullptr;
  for (std::size_t i = 0; i < dyn.symCount; ++i) {
    if (std::strcmp(dyn.strtab + dyn.symtab[i].st_name, "_ZTVN10NRadEngine12CNSOVRSocialE") == 0) vt = &dyn.symtab[i];
  }
  if (vt == nullptr) return methods;
  *sizeOut = vt->st_size;
  *symValue = vt->st_value;
  const std::size_t slots = (vt->st_size - 16) / 8;
  methods.assign(slots, std::string());
  for (std::size_t i = 0; i < dyn.relaCount; ++i) {
    const Elf64_Rela& r = dyn.rela[i];
    if (ELF64_R_TYPE(r.r_info) != R_AARCH64_ABS64) continue;
    if (r.r_offset < vt->st_value + 16 || r.r_offset >= vt->st_value + vt->st_size) continue;
    const std::size_t slot = (r.r_offset - vt->st_value - 16) / 8;
    methods[slot] = Strip(dyn.strtab + dyn.symtab[ELF64_R_SYM(r.r_info)].st_name);
  }
  return methods;
}

const Elf64_Sym* FindSymbol(const Dynamic& dyn, const char* name) {
  for (std::size_t i = 0; i < dyn.symCount; ++i) {
    if (std::strcmp(dyn.strtab + dyn.symtab[i].st_name, name) == 0) return &dyn.symtab[i];
  }
  return nullptr;
}

const ElfImage* g_lookupImage = nullptr;
bool LookupFixed(const char*, ElfImage* out) {
  if (g_lookupImage == nullptr) return false;
  *out = *g_lookupImage;
  return true;
}

void CheckGameFunctions(const LoadedElf& r15) {
  Dynamic dyn;
  QCHECK(ReadDynamic(r15, &dyn));
  const Elf64_Sym* reset = FindSymbol(dyn, "_ZN10NRadEngine5CJson5ResetEv");
  QCHECK(reset != nullptr);
  if (reset != nullptr) {
    QCHECK(ELF64_ST_TYPE(reset->st_info) == STT_FUNC);
    QCHECK(reset->st_value == quest_social::kLibR15CJsonResetVaddr);
    QCHECK(reset->st_size == 36);
    // stp x19, x30, [sp, #-0x10]! : the first instruction of the 36 bytes the game's Reset is made of
    std::uint32_t first = 0;
    std::memcpy(&first, r15.At(reset->st_value), sizeof(first));
    QCHECK(first == 0xa9bf7bf3U);
  }
  // DecodeFrom(char const*, unsigned long long) and EncodeToCompact(char*, unsigned long long&, unsigned, char const*)
  // const: the exports the party and member data go through, their sizes, and their first instructions (sub sp, sp,
  // #0x150 and mov x5, x4).
  const Elf64_Sym* decode = FindSymbol(dyn, "_ZN10NRadEngine5CJson10DecodeFromEPKcy");
  QCHECK(decode != nullptr);
  if (decode != nullptr) {
    QCHECK(ELF64_ST_TYPE(decode->st_info) == STT_FUNC && decode->st_size == 356);
    QCHECK(decode->st_value == quest_social::kLibR15CJsonDecodeFromVaddr);
    std::uint32_t first = 0;
    std::memcpy(&first, r15.At(decode->st_value), sizeof(first));
    QCHECK(first == 0xd10543ffU);
  }
  const Elf64_Sym* encode = FindSymbol(dyn, "_ZNK10NRadEngine5CJson15EncodeToCompactEPcRyjPKc");
  QCHECK(encode != nullptr);
  if (encode != nullptr) {
    QCHECK(ELF64_ST_TYPE(encode->st_info) == STT_FUNC && encode->st_size == 12);
    QCHECK(encode->st_value == quest_social::kLibR15CJsonEncodeToCompactVaddr);
    std::uint32_t first = 0;
    std::memcpy(&first, r15.At(encode->st_value), sizeof(first));
    QCHECK(first == 0xaa0403e5U);
  }
  // The production resolver, on the real image, lands on those addresses.
  g_lookupImage = &r15.image;
  const quest_social::GameJson json = quest_social::ResolveGameJson(&LookupFixed);
  g_lookupImage = nullptr;
  const auto address = [](auto fn) {
    std::uintptr_t value = 0;
    std::memcpy(&value, &fn, sizeof(value));
    return value;
  };
  QCHECK(address(json.reset) == r15.image.base + quest_social::kLibR15CJsonResetVaddr);
  QCHECK(address(json.decode) == r15.image.base + quest_social::kLibR15CJsonDecodeFromVaddr);
  QCHECK(address(json.encode) == r15.image.base + quest_social::kLibR15CJsonEncodeToCompactVaddr);

  // SUuid::kInvalid (ExitLobby and Reset copy it; the facade stores sixteen zero bytes instead): a 16-byte
  // object in the .bss part of a PT_LOAD, so zero at load.
  const Elf64_Sym* invalid = FindSymbol(dyn, "_ZN10NRadEngine5SUuid8kInvalidE");
  QCHECK(invalid != nullptr);
  if (invalid != nullptr) {
    QCHECK(ELF64_ST_TYPE(invalid->st_info) == STT_OBJECT && invalid->st_size == 16);
    bool inBss = false;
    for (const Elf64_Phdr& ph : r15.phdrs) {
      if (ph.p_type == PT_LOAD && invalid->st_value >= ph.p_vaddr + ph.p_filesz &&
          invalid->st_value + invalid->st_size <= ph.p_vaddr + ph.p_memsz) {
        inBss = true;
      }
    }
    QCHECK(inBss);
  }
}

void CheckBuildId(const LoadedElf& elf, const char* expected) {
  char actual[64] = {};
  QCHECK(ReadBuildId(elf.image, actual, sizeof(actual)));
  QCHECK(std::strcmp(actual, expected) == 0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--dump") == 0) {
    LoadedElf ovr;
    std::size_t size = 0;
    std::uint64_t value = 0;
    if (!Load(argv[2], &ovr)) return 2;
    char id[64] = {};
    ReadBuildId(ovr.image, id, sizeof(id));
    const std::vector<std::string> methods = VtableMethods(ovr, &size, &value);
    std::printf("# CNSOVRSocial vtable of libpnsovr.so build id %s\n", id);
    std::printf("# symbol _ZTVN10NRadEngine12CNSOVRSocialE value 0x%llx size 0x%zx; slot n is at value + 16 + 8n\n",
                static_cast<unsigned long long>(value), size);
    std::printf("# generated by: social_pinned_test --dump libpnsovr.so (src/quest/tests/social_pinned_test.cpp)\n");
    for (std::size_t i = 0; i < methods.size(); ++i) std::printf("%zu %s\n", i, methods[i].c_str());
    return 0;
  }
  if (argc != 3) {
    std::fprintf(stderr, "usage: social_pinned_test <libr15.so> <libpnsovr.so> | --dump <libpnsovr.so>\n");
    return 2;
  }
  LoadedElf r15, ovr;
  if (!Load(argv[1], &r15) || !Load(argv[2], &ovr)) {
    std::fprintf(stderr, "social_pinned_test: cannot load an AArch64 ELF64 from the given paths\n");
    return 2;
  }
  CheckBuildId(r15, pinned::kLibR15BuildId);
  CheckBuildId(ovr, quest_social::kLibPnsovrBuildId);
  CheckGameFunctions(r15);

  // libr15's slot for CNSProvider::Social: pinned, and the only one for the symbol.
  const GotTarget target = quest_social::LibR15Social();
  const SlotResolution pinnedSlot = ResolveSlot(r15.image, target, kAarch64Relocs);
  QCHECK_STATUS(pinnedSlot.status, GotStatus::kOk);
  QCHECK(pinnedSlot.slotVaddr == quest_social::kSocialSlotVaddr);
  GotTarget unpinned = target;
  unpinned.slotVaddr = std::nullopt;
  const SlotResolution alone = ResolveSlot(r15.image, unpinned, kAarch64Relocs);
  QCHECK_STATUS(alone.status, GotStatus::kOk);
  QCHECK(alone.slotVaddr == pinnedSlot.slotVaddr);
  GotTarget shifted = target;
  shifted.slotVaddr = quest_social::kSocialSlotVaddr + 8;
  QCHECK_STATUS(ResolveSlot(r15.image, shifted, kAarch64Relocs).status, GotStatus::kSlotOffsetMismatch);

  // libr15's slot for CJson::Boolean (the invite gate): pinned, the only one for the symbol, and the export it imports.
  const GotTarget gate = quest_social::LibR15Boolean();
  QCHECK_STATUS(ResolveSlot(r15.image, gate, kAarch64Relocs).status, GotStatus::kOk);
  QCHECK(ResolveSlot(r15.image, gate, kAarch64Relocs).slotVaddr == quest_social::kBooleanSlotVaddr);
  GotTarget gateUnpinned = gate;
  gateUnpinned.slotVaddr = std::nullopt;
  QCHECK_STATUS(ResolveSlot(r15.image, gateUnpinned, kAarch64Relocs).status, GotStatus::kOk);
  QCHECK(ResolveSlot(r15.image, gateUnpinned, kAarch64Relocs).slotVaddr == quest_social::kBooleanSlotVaddr);
  {
    Dynamic r15dyn;
    QCHECK(ReadDynamic(r15, &r15dyn));
    const Elf64_Sym* boolean = FindSymbol(r15dyn, quest_social::kBooleanSymbol);
    QCHECK(boolean != nullptr && boolean->st_value == 0xfa4370ULL && ELF64_ST_TYPE(boolean->st_info) == STT_FUNC);
  }

  // libpnsovr's CNSOVRSocial vtable: the address point the hook compares against, and every slot.
  std::size_t size = 0;
  std::uint64_t value = 0;
  const std::vector<std::string> methods = VtableMethods(ovr, &size, &value);
  QCHECK(value + 16 == quest_social::kOvrSocialVptrVaddr);
  QCHECK(methods.size() == quest_social::kSlotCount);
  for (std::size_t i = 0; i < methods.size() && i < quest_social::kSlotCount; ++i) {
    if (methods[i] != quest_social::kSlotNames[i]) {
      std::fprintf(stderr, "slot %zu: library \"%s\" header \"%s\"\n", i, methods[i].c_str(), quest_social::kSlotNames[i]);
    }
    QCHECK(methods[i] == quest_social::kSlotNames[i]);
  }

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "social_pinned_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("social_pinned_test: the Social slot, CJson::Reset, kInvalid and the CNSOVRSocial vtable match the real ELFs\n");
  return 0;
}
