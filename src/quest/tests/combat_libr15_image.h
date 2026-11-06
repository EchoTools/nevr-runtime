// A libr15 image in memory for the combat host tests: the build-ID note, a .dynsym with the symbols the test names, the relocations
// (JUMP_SLOT in the PLT table, GLOB_DAT in the other one) and the program headers, in one anonymous mapping that is as big as the real
// file's address space, so a pinned word is written at its pinned address. Built like equip_refire_test.cpp's, so the real GotHook and
// GotDataSlot resolve it. The test keeps its words in the mapping through Put/Get; nothing here executes game code.
#pragma once

#include <elf.h>
#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"
#include "quest/tests/test_check.h"

namespace combat_test {

// One relocation of the image: the symbol it names (a data object or a function), the slot, and whether it is a GLOB_DAT word or a
// JUMP_SLOT.
struct Reloc {
  const char* symbol;
  std::uint64_t slot;
  bool globDat;
  bool object;          // STT_OBJECT, else STT_FUNC
  std::uint64_t value;  // the symbol's address (st_value)
  bool abs64 = false;   // an R_AARCH64_ABS64 word (a vtable entry) instead of a JUMP_SLOT or GLOB_DAT word; it goes in the same table as GLOB_DAT
};

class Image {
 public:
  static constexpr std::size_t kSize = 0x3800000;
  static constexpr std::uint64_t kRwStart = 0x3600000;

  // `buildId` null: the patched libr15's (the one the running sentinel checks). `note` false: no PT_NOTE, so no build ID.
  void Build(const std::vector<Reloc>& relocs, const char* buildId = nullptr, bool note = true) {
    Drop();
    void* const mem = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    QCHECK(mem != MAP_FAILED);
    mem_ = static_cast<unsigned char*>(mem);
    const char* const id = buildId != nullptr ? buildId : sentinel::pinned::LibR15PatchedBuildId();
    unsigned char desc[20] = {};
    QCHECK(std::strlen(id) == 40);
    for (std::size_t i = 0; i < 20 && std::strlen(id) == 40; ++i) desc[i] = static_cast<unsigned char>((Nibble(id[2 * i]) << 4) | Nibble(id[2 * i + 1]));
    const Elf64_Nhdr nh{4, 20, NT_GNU_BUILD_ID};
    std::memcpy(mem_ + kNoteAt, &nh, sizeof(nh));
    std::memcpy(mem_ + kNoteAt + sizeof(nh), "GNU", 4);
    std::memcpy(mem_ + kNoteAt + sizeof(nh) + 4, desc, 20);

    std::string dynstr(1, '\0');
    std::vector<Elf64_Sym> syms(relocs.size() + 1);
    std::vector<Elf64_Rela> jmprel;
    std::vector<Elf64_Rela> rela;
    for (std::size_t i = 0; i < relocs.size(); ++i) {
      const Reloc& r = relocs[i];
      Elf64_Sym& sym = syms[i + 1];
      sym.st_name = static_cast<Elf64_Word>(dynstr.size());
      sym.st_info = ELF64_ST_INFO(STB_GLOBAL, r.object ? STT_OBJECT : STT_FUNC);
      sym.st_shndx = 1;
      sym.st_value = r.value;
      dynstr += r.symbol;
      dynstr.push_back('\0');
      Elf64_Rela rel{};
      rel.r_offset = r.slot;
      rel.r_info = ELF64_R_INFO(static_cast<Elf64_Xword>(i + 1), r.abs64     ? sentinel::kNativeRelocs.abs64
                                                                  : r.globDat ? sentinel::kNativeRelocs.globDat
                                                                              : sentinel::kNativeRelocs.jumpSlot);
      (r.globDat || r.abs64 ? rela : jmprel).push_back(rel);
    }
    QCHECK(dynstr.size() < kDynsymAt - kDynstrAt);
    QCHECK(syms.size() * sizeof(Elf64_Sym) < kJmprelAt - kDynsymAt);
    std::memcpy(mem_ + kDynstrAt, dynstr.data(), dynstr.size());
    std::memcpy(mem_ + kDynsymAt, syms.data(), syms.size() * sizeof(Elf64_Sym));
    std::memcpy(mem_ + kJmprelAt, jmprel.data(), jmprel.size() * sizeof(Elf64_Rela));
    std::memcpy(mem_ + kRelaAt, rela.data(), rela.size() * sizeof(Elf64_Rela));

    Elf64_Dyn dyn[12] = {};
    std::size_t n = 0;
    auto add = [&dyn, &n](Elf64_Sxword tag, std::uint64_t value) {
      dyn[n].d_tag = tag;
      dyn[n].d_un.d_val = value;
      ++n;
    };
    add(DT_STRTAB, kDynstrAt);
    add(DT_SYMTAB, kDynsymAt);
    add(DT_STRSZ, dynstr.size());
    add(DT_JMPREL, kJmprelAt);
    add(DT_PLTRELSZ, jmprel.size() * sizeof(Elf64_Rela));
    add(DT_PLTREL, DT_RELA);
    add(DT_RELA, kRelaAt);
    add(DT_RELASZ, rela.size() * sizeof(Elf64_Rela));
    add(DT_RELAENT, sizeof(Elf64_Rela));
    add(DT_FLAGS, DF_BIND_NOW);
    add(DT_NULL, 0);
    std::memcpy(mem_ + kDynamicAt, dyn, sizeof(dyn));

    phdr_[0].p_type = PT_LOAD;
    phdr_[0].p_flags = PF_R | PF_X;
    phdr_[0].p_vaddr = 0;
    phdr_[0].p_filesz = phdr_[0].p_memsz = kRwStart;
    phdr_[1].p_type = PT_LOAD;
    phdr_[1].p_flags = PF_R | PF_W;
    phdr_[1].p_vaddr = kRwStart;
    phdr_[1].p_filesz = phdr_[1].p_memsz = kSize - kRwStart;
    phdr_[2].p_type = PT_DYNAMIC;
    phdr_[2].p_flags = PF_R | PF_W;
    phdr_[2].p_vaddr = kDynamicAt;
    phdr_[2].p_filesz = phdr_[2].p_memsz = sizeof(dyn);
    phdr_[3].p_type = PT_NOTE;
    phdr_[3].p_vaddr = kNoteAt;
    phdr_[3].p_filesz = phdr_[3].p_memsz = sizeof(nh) + 4 + 20;
    image_.base = reinterpret_cast<std::uintptr_t>(mem_);
    image_.phdr = phdr_;
    image_.phnum = note ? 4 : 3;
    std::strcpy(image_.name, sentinel::pinned::kLibR15);
    present_ = true;
  }

  void Drop() {
    present_ = false;
    if (mem_ != nullptr) munmap(mem_, kSize);
    mem_ = nullptr;
    image_ = sentinel::ElfImage{};
  }

  std::uintptr_t Base() const { return image_.base; }
  bool Present() const { return present_; }
  const sentinel::ElfImage& Elf() const { return image_; }

  template <typename T>
  void Put(std::uint64_t vaddr, T value) {
    std::memcpy(mem_ + vaddr, &value, sizeof(value));
  }
  template <typename T>
  T Get(std::uint64_t vaddr) const {
    T value{};
    std::memcpy(&value, mem_ + vaddr, sizeof(value));
    return value;
  }
  void** Slot(std::uint64_t vaddr) { return reinterpret_cast<void**>(mem_ + vaddr); }

 private:
  static constexpr std::uint64_t kNoteAt = 0x100;
  static constexpr std::uint64_t kDynstrAt = 0x400;
  static constexpr std::uint64_t kDynsymAt = 0x1000;
  static constexpr std::uint64_t kJmprelAt = 0x1400;
  static constexpr std::uint64_t kRelaAt = 0x1600;
  static constexpr std::uint64_t kDynamicAt = 0x1800;

  static int Nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  }

  unsigned char* mem_ = nullptr;
  Elf64_Phdr phdr_[4] = {};
  sentinel::ElfImage image_;
  bool present_ = false;
};

}  // namespace combat_test
