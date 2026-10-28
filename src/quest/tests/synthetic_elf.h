// A small AArch64 shared object built in memory, shaped like the pinned libr15 where the patcher looks: a read-only
// executable PT_LOAD that holds the headers, the build-ID note, .dynsym/.dynstr and the code, a writable PT_LOAD with
// .data and .bss, a PT_NOTE, a GNU_RELRO and (optionally) a trailing PT_LOAD. Used by the patcher's host tests
// (ADR 0021 tests 2 and 9); no real libr15 byte or address is in it, and the addresses are invented.
//
//   file 0x0000  ELF header, program headers (to 0x200)       PT_LOAD #0  vaddr 0x13ff000  filesz 0x3000  R E
//        0x0200  .note.gnu.build-id (36 bytes)
//        0x0300  .dynsym  0x0400 .dynstr                       (the six end symbols)
//        0x1000  .text, vaddr 0x1400000 .. 0x1402000           (every word distinct: mov x0, #n)
//        0x3000  .data (0x200 bytes), vaddr 0x1403000          PT_LOAD #1  vaddr 0x1403000  filesz 0x200  memsz varies  RW
//        0x3200  .shstrtab, 0x3300 section headers             .bss ends the segment
#pragma once

#include <elf.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "quest/patch/libr15_patch_identity.h"

namespace nevr_quest::patch::testing {

inline constexpr std::uint64_t kLoad0Vaddr = 0x13ff000;
inline constexpr std::uint64_t kTextVaddr = 0x1400000;
inline constexpr std::uint64_t kTextEnd = 0x1402000;
inline constexpr std::uint64_t kDataVaddr = 0x1403000;
inline constexpr std::uint64_t kDataSize = 0x200;
inline constexpr std::uint64_t kFileSize = 0x3500;

struct SynthOptions {
  BuildIdBytes buildId{};
  std::uint64_t imageEnd = 0x14035f8;                  // end of the writable PT_LOAD
  std::optional<std::uint64_t> trailingLoadVaddr;      // a PT_LOAD (memory only, 0x100 bytes) at this address
  std::optional<std::uint64_t> relroEnd;               // GNU_RELRO from the data start to here
};

struct SynthElf {
  std::vector<std::uint8_t> bytes;
  std::uint64_t imageEnd = 0;
  std::size_t writableLoadIndex = 1;  // index of the writable PT_LOAD in the program header table
  std::size_t bssSection = 6;

  static std::uint64_t FileOffsetOfText(std::uint64_t vaddr) { return vaddr - kLoad0Vaddr; }
  static std::uint32_t Filler(std::uint64_t vaddr) { return 0xd2800000u | (static_cast<std::uint32_t>((vaddr / 4) & 0xFFFF) << 5); }

  std::uint32_t WordAt(std::uint64_t vaddr) const {
    std::uint32_t w;
    std::memcpy(&w, bytes.data() + FileOffsetOfText(vaddr), 4);
    return w;
  }
  void PutWord(std::uint64_t vaddr, std::uint32_t w) { std::memcpy(bytes.data() + FileOffsetOfText(vaddr), &w, 4); }
  void PutBytes(std::uint64_t vaddr, const std::uint8_t (&b)[4]) { std::memcpy(bytes.data() + FileOffsetOfText(vaddr), b, 4); }
  std::uint64_t PhdrMemszOffset() const { return sizeof(Elf64_Ehdr) + writableLoadIndex * sizeof(Elf64_Phdr) + offsetof(Elf64_Phdr, p_memsz); }
  std::uint64_t BssSizeOffset() const { return 0x3300 + bssSection * sizeof(Elf64_Shdr) + offsetof(Elf64_Shdr, sh_size); }
};

inline SynthElf BuildSyntheticElf(const SynthOptions& opt) {
  SynthElf s;
  s.imageEnd = opt.imageEnd;
  s.bytes.assign(kFileSize, 0);
  auto put = [&s](std::uint64_t off, const void* p, std::size_t n) { std::memcpy(s.bytes.data() + off, p, n); };

  // Code: every word distinct.
  for (std::uint64_t va = kTextVaddr; va < kTextEnd; va += 4) {
    const std::uint32_t w = SynthElf::Filler(va);
    put(SynthElf::FileOffsetOfText(va), &w, 4);
  }

  // .dynstr and .dynsym with the six end symbols.
  const char* names[] = {"__end__", "__bss_end__", "_bss_end__", "_end", "_edata", "__bss_start"};
  const std::uint64_t values[] = {opt.imageEnd, opt.imageEnd, opt.imageEnd, opt.imageEnd, kDataVaddr + kDataSize, kDataVaddr + kDataSize};
  std::string dynstr(1, '\0');
  std::vector<Elf64_Sym> syms(1);
  std::memset(&syms[0], 0, sizeof(Elf64_Sym));
  for (int i = 0; i < 6; ++i) {
    Elf64_Sym sym;
    std::memset(&sym, 0, sizeof sym);
    sym.st_name = static_cast<Elf64_Word>(dynstr.size());
    sym.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE);
    sym.st_shndx = SHN_ABS;
    sym.st_value = values[i];
    dynstr += names[i];
    dynstr.push_back('\0');
    syms.push_back(sym);
  }
  put(0x300, syms.data(), syms.size() * sizeof(Elf64_Sym));
  put(0x400, dynstr.data(), dynstr.size());

  // The build-ID note.
  Elf64_Nhdr nh;
  nh.n_namesz = 4;
  nh.n_descsz = 20;
  nh.n_type = NT_GNU_BUILD_ID;
  put(0x200, &nh, sizeof nh);
  put(0x200 + sizeof nh, "GNU", 4);
  put(0x200 + sizeof nh + 4, opt.buildId.data(), 20);

  // Section names.
  std::string shstr(1, '\0');
  auto name = [&shstr](const char* n) {
    const std::uint32_t at = static_cast<std::uint32_t>(shstr.size());
    shstr += n;
    shstr.push_back('\0');
    return at;
  };
  const std::uint32_t nNote = name(".note.gnu.build-id");
  const std::uint32_t nText = name(".text");
  const std::uint32_t nDynsym = name(".dynsym");
  const std::uint32_t nDynstr = name(".dynstr");
  const std::uint32_t nData = name(".data");
  const std::uint32_t nBss = name(".bss");
  const std::uint32_t nShstr = name(".shstrtab");
  put(0x3200, shstr.data(), shstr.size());

  // Program headers.
  std::vector<Elf64_Phdr> ph;
  auto phdr = [&ph](Elf64_Word type, Elf64_Word flags, std::uint64_t off, std::uint64_t va, std::uint64_t filesz, std::uint64_t memsz, std::uint64_t align) {
    Elf64_Phdr p;
    std::memset(&p, 0, sizeof p);
    p.p_type = type;
    p.p_flags = flags;
    p.p_offset = off;
    p.p_vaddr = va;
    p.p_paddr = va;
    p.p_filesz = filesz;
    p.p_memsz = memsz;
    p.p_align = align;
    ph.push_back(p);
  };
  phdr(PT_LOAD, PF_R | PF_X, 0, kLoad0Vaddr, 0x3000, 0x3000, 0x1000);
  phdr(PT_LOAD, PF_R | PF_W, 0x3000, kDataVaddr, kDataSize, opt.imageEnd - kDataVaddr, 0x1000);
  phdr(PT_NOTE, PF_R, 0x200, kLoad0Vaddr + 0x200, 36, 36, 4);
  if (opt.relroEnd.has_value()) phdr(PT_GNU_RELRO, PF_R, 0x3000, kDataVaddr, *opt.relroEnd - kDataVaddr, *opt.relroEnd - kDataVaddr, 1);
  if (opt.trailingLoadVaddr.has_value()) phdr(PT_LOAD, PF_R, 0, *opt.trailingLoadVaddr, 0, 0x100, 0x1000);
  put(sizeof(Elf64_Ehdr), ph.data(), ph.size() * sizeof(Elf64_Phdr));

  // Section headers.
  std::vector<Elf64_Shdr> sh(8);
  std::memset(sh.data(), 0, sh.size() * sizeof(Elf64_Shdr));
  auto sec = [&sh](int i, std::uint32_t nm, Elf64_Word type, Elf64_Xword flags, std::uint64_t addr, std::uint64_t off, std::uint64_t size,
                   Elf64_Word link, Elf64_Xword align, Elf64_Xword entsize) {
    sh[i].sh_name = nm;
    sh[i].sh_type = type;
    sh[i].sh_flags = flags;
    sh[i].sh_addr = addr;
    sh[i].sh_offset = off;
    sh[i].sh_size = size;
    sh[i].sh_link = link;
    sh[i].sh_addralign = align;
    sh[i].sh_entsize = entsize;
  };
  sec(1, nNote, SHT_NOTE, SHF_ALLOC, kLoad0Vaddr + 0x200, 0x200, 36, 0, 4, 0);
  sec(2, nText, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, kTextVaddr, 0x1000, kTextEnd - kTextVaddr, 0, 16, 0);
  sec(3, nDynsym, SHT_DYNSYM, SHF_ALLOC, kLoad0Vaddr + 0x300, 0x300, syms.size() * sizeof(Elf64_Sym), 4, 8, sizeof(Elf64_Sym));
  sec(4, nDynstr, SHT_STRTAB, SHF_ALLOC, kLoad0Vaddr + 0x400, 0x400, dynstr.size(), 0, 1, 0);
  sec(5, nData, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, kDataVaddr, 0x3000, kDataSize, 0, 16, 0);
  sec(6, nBss, SHT_NOBITS, SHF_ALLOC | SHF_WRITE, kDataVaddr + kDataSize, 0x3200, opt.imageEnd - (kDataVaddr + kDataSize), 0, 64, 0);
  sec(7, nShstr, SHT_STRTAB, 0, 0, 0x3200, shstr.size(), 0, 1, 0);
  put(0x3300, sh.data(), sh.size() * sizeof(Elf64_Shdr));

  // The ELF header.
  Elf64_Ehdr eh;
  std::memset(&eh, 0, sizeof eh);
  std::memcpy(eh.e_ident, ELFMAG, SELFMAG);
  eh.e_ident[EI_CLASS] = ELFCLASS64;
  eh.e_ident[EI_DATA] = ELFDATA2LSB;
  eh.e_ident[EI_VERSION] = EV_CURRENT;
  eh.e_type = ET_DYN;
  eh.e_machine = EM_AARCH64;
  eh.e_version = EV_CURRENT;
  eh.e_phoff = sizeof(Elf64_Ehdr);
  eh.e_shoff = 0x3300;
  eh.e_ehsize = sizeof(Elf64_Ehdr);
  eh.e_phentsize = sizeof(Elf64_Phdr);
  eh.e_phnum = static_cast<Elf64_Half>(ph.size());
  eh.e_shentsize = sizeof(Elf64_Shdr);
  eh.e_shnum = 8;
  eh.e_shstrndx = 7;
  put(0, &eh, sizeof eh);
  return s;
}

}  // namespace nevr_quest::patch::testing
