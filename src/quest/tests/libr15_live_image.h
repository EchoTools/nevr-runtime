// The shared fixture of the host tests that need a libr15 the sentinel has verified: a synthetic STOCK libr15 at the real
// table's addresses, the shipped patcher's output for it, that output laid out in host memory (text pages r-x, so a stray
// store into code faults), and the seams Attach is driven through (lookup, protection, map, process end, log sink).
//
// Built the strongest way the host allows: the shipped patcher (PatchLibr15) runs on the synthetic file, so a test that
// uses this image ties the patcher's output to the sentinel's checks. synthetic_elf.h cannot serve: its layout is fixed at
// invented addresses, and Attach reads the real table.
//
// A test executable includes this header once and places NEVR_LIVE_IMAGE_MPROTECT_OVERRIDE() at global scope. The
// executable's own definition of mprotect wins over libc's for the code linked into it, so a regression that changes a
// page's protection without the seam is seen too; the test's own protection changes go through the system call directly.
#pragma once

#include <elf.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "hook_log.h"
#include "libr15_attach.h"
#include "mapping_perms.h"
#include "mini_test.h"
#include "quest/data/sha256.h"
#include "quest/patch/libr15_patcher.h"
#include "quest/patch/stub_codec.h"

#define NEVR_LIVE_IMAGE_MPROTECT_OVERRIDE()                                                      \
  extern "C" int mprotect(void* addr, std::size_t len, int prot) noexcept {                      \
    live_fixture::g_rawProtects.push_back({addr, len, prot});                                    \
    return static_cast<int>(syscall(SYS_mprotect, addr, len, prot));                             \
  }

namespace live_fixture {

using namespace nevr_quest;
using libr15_patch::AttachOptions;
using sentinel::ElfImage;

// ---- the synthetic stock libr15 at the real table's addresses ---------------------------------------------------------

inline std::string g_fixtures;

// ---- the synthetic stock libr15 at the real table's addresses ---------------------------------------------------------

constexpr std::uint64_t kDataVaddr = 0x35f9eb0;                      // the real writable PT_LOAD's start
constexpr std::uint64_t kEdata = 0x3761178;                          // _edata: the end of its file-backed part
constexpr std::uint64_t kDataFilesz = kEdata - kDataVaddr;
constexpr std::uint64_t kRelroEnd = 0x372c000;                       // the real GNU_RELRO's end
constexpr std::uint64_t kFixturePage = 0x1400000;                    // the extra-rows fixture's invented words (0x1500010 - bias)
constexpr std::uint64_t kNoteVaddr = 0x400;
constexpr std::uint64_t kNoteDescVaddr = kNoteVaddr + sizeof(Elf64_Nhdr) + 4;

std::uint32_t Filler(std::uint64_t vaddr) { return 0xd2800000u | (static_cast<std::uint32_t>((vaddr / 4) & 0xFFFF) << 5); }

inline std::vector<std::uint64_t> TextPages() {
  std::set<std::uint64_t> pages;
  pages.insert(kFixturePage);
  for (std::size_t r = 0; r < patch::kRowCount; ++r) {
    for (std::size_t s = 0; s < patch::kRows[r].siteCount; ++s) pages.insert(patch::kRows[r].sites[s].Vaddr() & ~std::uint64_t{0xFFF});
  }
  return std::vector<std::uint64_t>(pages.begin(), pages.end());
}

template <typename T>
inline T ReadAt(const std::vector<std::uint8_t>& b, std::uint64_t off) {
  T v;
  std::memcpy(&v, b.data() + off, sizeof v);
  return v;
}
template <typename T>
inline void WriteAt(std::vector<std::uint8_t>& b, std::uint64_t off, const T& v) {
  std::memcpy(b.data() + off, &v, sizeof v);
}

inline std::vector<Elf64_Phdr> ProgramHeaders(const std::vector<std::uint8_t>& b) {
  const Elf64_Ehdr eh = ReadAt<Elf64_Ehdr>(b, 0);
  std::vector<Elf64_Phdr> ph(eh.e_phnum);
  for (std::size_t i = 0; i < ph.size(); ++i) ph[i] = ReadAt<Elf64_Phdr>(b, eh.e_phoff + i * sizeof(Elf64_Phdr));
  return ph;
}
inline void SetProgramHeader(std::vector<std::uint8_t>& b, std::size_t i, const Elf64_Phdr& p) {
  WriteAt(b, ReadAt<Elf64_Ehdr>(b, 0).e_phoff + i * sizeof(Elf64_Phdr), p);
}
inline std::size_t DataLoadIndex(const std::vector<std::uint8_t>& b) {
  const std::vector<Elf64_Phdr> ph = ProgramHeaders(b);
  for (std::size_t i = 0; i < ph.size(); ++i) {
    if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_W) != 0) return i;
  }
  return ph.size();
}
inline std::size_t RelroIndex(const std::vector<std::uint8_t>& b) {
  const std::vector<Elf64_Phdr> ph = ProgramHeaders(b);
  for (std::size_t i = 0; i < ph.size(); ++i) {
    if (ph[i].p_type == PT_GNU_RELRO) return i;
  }
  return ph.size();
}

// The file offset of a vaddr that lies in a file-backed PT_LOAD.
inline std::uint64_t FileOffsetOf(const std::vector<std::uint8_t>& b, std::uint64_t vaddr) {
  for (const Elf64_Phdr& p : ProgramHeaders(b)) {
    if (p.p_type == PT_LOAD && vaddr >= p.p_vaddr && vaddr - p.p_vaddr < p.p_filesz) return p.p_offset + (vaddr - p.p_vaddr);
  }
  return ~std::uint64_t{0};
}
inline void PutWord(std::vector<std::uint8_t>& b, std::uint64_t vaddr, std::uint32_t w) {
  const std::uint64_t off = FileOffsetOf(b, vaddr);
  CHECK(off != ~std::uint64_t{0});
  if (off != ~std::uint64_t{0}) WriteAt(b, off, w);
}

inline std::vector<std::uint8_t> BuildStock() {
  const std::vector<std::uint64_t> pages = TextPages();
  const std::uint64_t dataOff = 0x1000 * (pages.size() + 1) + (kDataVaddr & 0xFFF);
  std::string shstr(1, '\0');
  auto name = [&shstr](const char* n) {
    const std::uint32_t at = static_cast<std::uint32_t>(shstr.size());
    shstr += n;
    shstr.push_back('\0');
    return at;
  };
  const std::uint32_t nNote = name(".note.gnu.build-id"), nDynsym = name(".dynsym"), nDynstr = name(".dynstr"),
                      nData = name(".data"), nBss = name(".bss"), nShstr = name(".shstrtab");
  const std::uint64_t shstrOff = patch::RoundUp(dataOff + kDataFilesz, 16);
  const std::uint64_t shdrOff = patch::RoundUp(shstrOff + shstr.size(), 8);
  std::vector<std::uint8_t> b(shdrOff + 7 * sizeof(Elf64_Shdr), 0);
  auto put = [&b](std::uint64_t off, const void* p, std::size_t n) { std::memcpy(b.data() + off, p, n); };

  // Text: every word distinct, then the table's stock words, then the fixture's two words.
  for (std::size_t k = 0; k < pages.size(); ++k) {
    for (std::uint64_t va = pages[k]; va < pages[k] + 0x1000; va += 4) {
      const std::uint32_t w = Filler(va);
      put(0x1000 * (k + 1) + (va - pages[k]), &w, 4);
    }
  }
  auto textOffset = [&](std::uint64_t va) {
    const std::uint64_t page = va & ~std::uint64_t{0xFFF};
    const std::size_t k = static_cast<std::size_t>(std::find(pages.begin(), pages.end(), page) - pages.begin());
    return 0x1000 * (k + 1) + (va - page);
  };
  for (std::size_t r = 0; r < patch::kRowCount; ++r) {
    for (std::size_t s = 0; s < patch::kRows[r].siteCount; ++s) {
      const patch::Site& site = patch::kRows[r].sites[s];
      for (std::size_t w = 0; w < site.wordCount; ++w) put(textOffset(site.Vaddr() + 4 * w), &site.words[w], 4);
    }
  }
  const std::uint8_t guardA[4] = {0xe0, 0x03, 0x1f, 0xaa};
  const std::uint8_t guardB[4] = {0x00, 0x01, 0x00, 0x54};
  put(textOffset(kFixturePage + 0x10), guardA, 4);
  put(textOffset(kFixturePage + 0x18), guardB, 4);

  // The build-ID note, and the six end symbols.
  Elf64_Nhdr nh;
  nh.n_namesz = 4;
  nh.n_descsz = 20;
  nh.n_type = NT_GNU_BUILD_ID;
  put(kNoteVaddr, &nh, sizeof nh);
  put(kNoteVaddr + sizeof nh, "GNU", 4);
  patch::BuildIdBytes stockId{};
  CHECK(patch::ParseBuildIdHex(patch::kLibR15StockBuildId, &stockId));
  put(kNoteDescVaddr, stockId.data(), 20);
  const char* names[] = {"__end__", "__bss_end__", "_bss_end__", "_end", "_edata", "__bss_start"};
  const std::uint64_t values[] = {patch::kStockImageEnd, patch::kStockImageEnd, patch::kStockImageEnd, patch::kStockImageEnd, kEdata, kEdata};
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
  put(0x500, syms.data(), syms.size() * sizeof(Elf64_Sym));
  put(0x600, dynstr.data(), dynstr.size());
  put(shstrOff, shstr.data(), shstr.size());

  // Program headers: the headers page, the text pages, the writable segment, the note, GNU_RELRO.
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
  phdr(PT_LOAD, PF_R, 0, 0, 0x1000, 0x1000, 0x1000);
  for (std::size_t k = 0; k < pages.size(); ++k) phdr(PT_LOAD, PF_R | PF_X, 0x1000 * (k + 1), pages[k], 0x1000, 0x1000, 0x1000);
  phdr(PT_LOAD, PF_R | PF_W, dataOff, kDataVaddr, kDataFilesz, patch::kStockImageEnd - kDataVaddr, 0x1000);
  phdr(PT_NOTE, PF_R, kNoteVaddr, kNoteVaddr, 36, 36, 4);
  phdr(PT_GNU_RELRO, PF_R, dataOff, kDataVaddr, kRelroEnd - kDataVaddr, kRelroEnd - kDataVaddr, 1);
  put(sizeof(Elf64_Ehdr), ph.data(), ph.size() * sizeof(Elf64_Phdr));

  // Sections: the note, the dynamic symbols, .data and the NOBITS .bss that ends the segment.
  std::vector<Elf64_Shdr> sh(7);
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
  sec(1, nNote, SHT_NOTE, SHF_ALLOC, kNoteVaddr, kNoteVaddr, 36, 0, 4, 0);
  sec(2, nDynsym, SHT_DYNSYM, SHF_ALLOC, 0x500, 0x500, syms.size() * sizeof(Elf64_Sym), 3, 8, sizeof(Elf64_Sym));
  sec(3, nDynstr, SHT_STRTAB, SHF_ALLOC, 0x600, 0x600, dynstr.size(), 0, 1, 0);
  sec(4, nData, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, kDataVaddr, dataOff, kDataFilesz, 0, 16, 0);
  sec(5, nBss, SHT_NOBITS, SHF_ALLOC | SHF_WRITE, kEdata, dataOff + kDataFilesz, patch::kStockImageEnd - kEdata, 0, 64, 0);
  sec(6, nShstr, SHT_STRTAB, 0, 0, shstrOff, shstr.size(), 0, 1, 0);
  put(shdrOff, sh.data(), sh.size() * sizeof(Elf64_Shdr));

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
  eh.e_shoff = shdrOff;
  eh.e_ehsize = sizeof(Elf64_Ehdr);
  eh.e_phentsize = sizeof(Elf64_Phdr);
  eh.e_phnum = static_cast<Elf64_Half>(ph.size());
  eh.e_shentsize = sizeof(Elf64_Shdr);
  eh.e_shnum = static_cast<Elf64_Half>(sh.size());
  eh.e_shstrndx = 6;
  put(0, &eh, sizeof eh);
  return b;
}

inline const std::vector<std::uint8_t>& Stock() {
  static const std::vector<std::uint8_t> stock = BuildStock();
  return stock;
}

inline patch::PatchSpec Spec() {
  patch::PatchSpec spec = patch::PublicSpec();
  spec.stockSha256.clear();  // the synthetic file is not the pinned libr15
  return spec;
}

inline std::string ReadText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
std::string Fixture() { return ReadText(g_fixtures + "/libr15_extra_rows_example.json"); }

inline std::vector<patch::WordPatchRow> FixtureRows() {
  std::vector<patch::WordPatchRow> rows;
  patch::Refusal refusal;
  CHECK(patch::ParseExtraRows(Fixture(), Spec(), &rows, &refusal));
  CHECK_EQ(rows.size(), 1u);
  return rows;
}

// The shipped patcher's output for the synthetic stock file.
inline std::vector<std::uint8_t> PatchedBytes(const std::vector<patch::WordPatchRow>& extra = {}) {
  const std::string fixture = Fixture();
  const patch::PatchResult r = patch::PatchLibr15(Stock(), Spec(), extra, extra.empty() ? "" : data::Sha256Hex(fixture.data(), fixture.size()));
  if (!r.ok) std::fprintf(stderr, "  the patcher refused the synthetic stock file: %s\n", r.refusal.ToJson().dump().c_str());
  CHECK(r.ok);
  return r.output;
}

// ---- the patched file, laid out in host memory ------------------------------------------------------------------------

class LiveImage {
 public:
  // `skew` moves the load bias off a page boundary (check (b)); the text pages are then not made r-x.
  explicit LiveImage(const std::vector<std::uint8_t>& file, std::uintptr_t skew = 0) {
    phdrs_ = ProgramHeaders(file);
    std::uint64_t end = 0;
    for (const Elf64_Phdr& p : phdrs_) {
      if (p.p_type == PT_LOAD) end = std::max(end, p.p_vaddr + p.p_memsz);
    }
    size_ = patch::RoundUp(end, 4096) + 0x2000;
    void* const region = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(region != MAP_FAILED);
    if (region == MAP_FAILED) return;
    region_ = reinterpret_cast<std::uintptr_t>(region);
    base_ = region_ + skew;
    for (const Elf64_Phdr& p : phdrs_) {
      if (p.p_type != PT_LOAD || p.p_offset >= file.size()) continue;
      const std::uint64_t n = std::min<std::uint64_t>(p.p_filesz, file.size() - p.p_offset);
      std::memcpy(reinterpret_cast<void*>(base_ + p.p_vaddr), file.data() + p.p_offset, n);
    }
    if (skew == 0) {
      for (const Elf64_Phdr& p : phdrs_) {
        if (p.p_type == PT_LOAD && (p.p_flags & PF_X) != 0) {
          CHECK_EQ(syscall(SYS_mprotect, reinterpret_cast<void*>(base_ + p.p_vaddr), p.p_memsz, PROT_READ | PROT_EXEC), 0L);
        }
      }
    }
    image_.base = base_;
    image_.phdr = phdrs_.data();
    image_.phnum = phdrs_.size();
    std::strcpy(image_.name, "libr15.so");
  }
  ~LiveImage() {
    if (region_ != 0) munmap(reinterpret_cast<void*>(region_), size_);
  }
  LiveImage(const LiveImage&) = delete;
  LiveImage& operator=(const LiveImage&) = delete;

  const ElfImage& Image() const { return image_; }
  std::uintptr_t Base() const { return base_; }
  std::uintptr_t Region() const { return region_; }
  std::size_t Size() const { return size_; }
  void** Slot(std::size_t row) const { return reinterpret_cast<void**>(base_ + patch::kRows[row].SlotVaddr()); }
  static void* Canary(std::size_t row) { return reinterpret_cast<void*>(0xC0DE000000000000ull | row); }
  void FillCanaries() const {
    for (std::size_t i = 0; i < patch::kRowCount; ++i) *Slot(i) = Canary(i);
  }
  bool SlotsAreCanaries() const {
    for (std::size_t i = 0; i < patch::kRowCount; ++i) {
      if (*Slot(i) != Canary(i)) return false;
    }
    return true;
  }
  // The /proc/self/maps lines that overlap the image's mapping: the evidence that no page changed protection.
  std::vector<std::string> MapsLines() const {
    std::vector<std::string> out;
    std::FILE* f = std::fopen("/proc/self/maps", "re");
    if (f == nullptr) return out;
    char line[512];
    while (std::fgets(line, sizeof line, f) != nullptr) {
      unsigned long lo = 0, hi = 0;
      if (std::sscanf(line, "%lx-%lx", &lo, &hi) != 2) continue;
      if (lo < region_ + size_ && region_ < hi) out.push_back(line);
    }
    std::fclose(f);
    return out;
  }
  bool Intersects(const void* addr, std::size_t len) const {
    const auto a = reinterpret_cast<std::uintptr_t>(addr);
    return a < region_ + size_ && region_ < a + len;
  }

 private:
  std::vector<Elf64_Phdr> phdrs_;
  std::uintptr_t region_ = 0;
  std::uintptr_t base_ = 0;
  std::size_t size_ = 0;
  ElfImage image_{};
};

// ---- the seams ----------------------------------------------------------------------------------------------------------

struct ProtectCall {
  void* addr;
  std::size_t len;
  int prot;
};
struct EndCall {
  int code;
  std::string reason;
};
struct Seams {
  std::vector<ProtectCall> protects;
  int protectFailAt = -1;
  int mapCalls = 0;
  int mapFailAt = -1;
  std::vector<void*> mapped;
  std::vector<EndCall> ends;
  std::vector<std::string> lines;
  std::vector<sentinel::LogLevel> levels;
};
inline Seams g;
inline LiveImage* g_live = nullptr;

// Every mprotect the library code makes, whether it goes through the seam or calls mprotect itself. The executable's own
// definition wins over libc's for the code linked into it, so a regression that changes a page's protection without the
// seam is seen too. The test's own protection changes go through the system call directly.
inline std::vector<ProtectCall> g_rawProtects;


inline bool Lookup(const char*, ElfImage* out) {
  if (g_live == nullptr) return false;
  *out = g_live->Image();
  return true;
}
inline int RecordingProtect(void* addr, std::size_t len, int prot) {
  const int index = static_cast<int>(g.protects.size());
  g.protects.push_back({addr, len, prot});
  if (index == g.protectFailAt) {
    errno = EACCES;
    return -1;
  }
  return mprotect(addr, len, prot);
}
inline void* RecordingMap(void* addr, std::size_t len, int prot, int flags, int fd, off_t off) {
  const int index = g.mapCalls++;
  if (index == g.mapFailAt) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  void* const p = mmap(addr, len, prot, flags, fd, off);
  g.mapped.push_back(p);
  return p;
}
void RecordingEnd(int code, const char* reason) noexcept { g.ends.push_back({code, reason}); }
inline void Sink(sentinel::LogLevel level, const char* line) {
  g.lines.push_back(line);
  g.levels.push_back(level);
}

// One test's process-wide state: the record cleared, the log captured, the seams reset.
struct Env {
  Env() {
    libr15_patch::ResetForTest();
    g = Seams();
    g_rawProtects.clear();
    sentinel::SetLogSink(&Sink);
  }
  ~Env() {
    sentinel::SetLogSink(nullptr);
    g_live = nullptr;
    libr15_patch::ResetForTest();
  }
};

inline AttachOptions Opts() {
  AttachOptions o;
  o.lookup = &Lookup;
  o.protect = &RecordingProtect;
  o.map = &RecordingMap;
  o.end = &RecordingEnd;
  return o;
}

inline int CountLines(const char* needle) {
  int n = 0;
  for (const std::string& l : g.lines) n += l.find(needle) != std::string::npos ? 1 : 0;
  return n;
}
inline const std::string* FindLine(const char* needle) {
  for (const std::string& l : g.lines) {
    if (l.find(needle) != std::string::npos) return &l;
  }
  return nullptr;
}
bool Has(const std::string& line, const std::string& needle) { return line.find(needle) != std::string::npos; }

inline std::string Hex(std::uint64_t v) {
  char buf[19];
  return sentinel::HexString(buf, v);
}

// The data-only rule: no protection change anywhere on the image, by the seam or by a direct call, and no direct call
// that the seam did not see.
inline void ExpectNoImageProtection(const LiveImage& live) {
  for (const ProtectCall& c : g_rawProtects) CHECK(!live.Intersects(c.addr, c.len));
  for (const ProtectCall& c : g.protects) CHECK(!live.Intersects(c.addr, c.len));
  CHECK_EQ(g_rawProtects.size(), g.protects.size());
}

inline bool UnmappedNow(const void* page) {
  char perms[5];
  return !sentinel::MappingPermsOf(reinterpret_cast<std::uintptr_t>(page), perms);
}

}  // namespace live_fixture
