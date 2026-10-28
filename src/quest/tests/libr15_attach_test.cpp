// Host test for the sentinel's run-time half of the libr15 build-time patch (src/quest/sentinel/libr15_attach.h),
// ADR 0021 test 4 and the Attach half of test 9.
//
// The image under test is built the strongest way the host allows: a synthetic STOCK libr15 whose text holds the
// compiled-in table's stock words at the table's real vaddrs and whose writable PT_LOAD ends at the real image end, then
// the SHIPPED patcher (PatchLibr15) run on it, then the patched file's PT_LOADs laid out in host memory (text pages
// r-x, so a stray store into code would fault) and handed to Attach through the lookup seam. That ties the patcher's
// output to the sentinel's checks. synthetic_elf.h cannot serve: its layout is fixed at invented addresses, and Attach
// reads the real table.
//
// What is not run here: the trampolines and the stubs execute AArch64; this host is x86-64, so their bytes are
// compared, not run (arm64_reloc_test runs the relocator's sequences on an interpreter).
//
// Run: libr15_attach_test <fixtures dir> [name-filter]

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
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "arm64_reloc.h"
#include "credits_text.h"
#include "hook_install.h"
#include "hook_log.h"
#include "libr15_attach.h"
#include "mapping_perms.h"
#include "mini_test.h"
#include "process_end_hook.h"
#include "quest/data/sha256.h"
#include "quest/patch/libr15_patcher.h"
#include "quest/patch/stub_codec.h"

namespace nevr_quest::libr15_patch {
// Reaches the private activation, as the thunk installer does.
struct AttachTestAccess {
  static ActivateStatus Activate(const char* rowId, void* detour, void** originalOut) {
    return RowActivation::ActivateRow(rowId, detour, originalOut);
  }
};
}  // namespace nevr_quest::libr15_patch

namespace {

using namespace nevr_quest;
using libr15_patch::ActivateStatus;
using libr15_patch::AttachOptions;
using libr15_patch::AttachResult;
using libr15_patch::AttachStatus;
using libr15_patch::AttachTestAccess;
using sentinel::ElfImage;

std::string g_fixtures;

// ---- the synthetic stock libr15 at the real table's addresses ---------------------------------------------------------

constexpr std::uint64_t kDataVaddr = 0x35f9eb0;                      // the real writable PT_LOAD's start
constexpr std::uint64_t kEdata = 0x3761178;                          // _edata: the end of its file-backed part
constexpr std::uint64_t kDataFilesz = kEdata - kDataVaddr;
constexpr std::uint64_t kRelroEnd = 0x372c000;                       // the real GNU_RELRO's end
constexpr std::uint64_t kFixturePage = 0x1400000;                    // the extra-rows fixture's invented words (0x1500010 - bias)
constexpr std::uint64_t kNoteVaddr = 0x400;
constexpr std::uint64_t kNoteDescVaddr = kNoteVaddr + sizeof(Elf64_Nhdr) + 4;

std::uint32_t Filler(std::uint64_t vaddr) { return 0xd2800000u | (static_cast<std::uint32_t>((vaddr / 4) & 0xFFFF) << 5); }

std::vector<std::uint64_t> TextPages() {
  std::set<std::uint64_t> pages;
  pages.insert(kFixturePage);
  for (std::size_t r = 0; r < patch::kRowCount; ++r) {
    for (std::size_t s = 0; s < patch::kRows[r].siteCount; ++s) pages.insert(patch::kRows[r].sites[s].Vaddr() & ~std::uint64_t{0xFFF});
  }
  return std::vector<std::uint64_t>(pages.begin(), pages.end());
}

template <typename T>
T ReadAt(const std::vector<std::uint8_t>& b, std::uint64_t off) {
  T v;
  std::memcpy(&v, b.data() + off, sizeof v);
  return v;
}
template <typename T>
void WriteAt(std::vector<std::uint8_t>& b, std::uint64_t off, const T& v) {
  std::memcpy(b.data() + off, &v, sizeof v);
}

std::vector<Elf64_Phdr> ProgramHeaders(const std::vector<std::uint8_t>& b) {
  const Elf64_Ehdr eh = ReadAt<Elf64_Ehdr>(b, 0);
  std::vector<Elf64_Phdr> ph(eh.e_phnum);
  for (std::size_t i = 0; i < ph.size(); ++i) ph[i] = ReadAt<Elf64_Phdr>(b, eh.e_phoff + i * sizeof(Elf64_Phdr));
  return ph;
}
void SetProgramHeader(std::vector<std::uint8_t>& b, std::size_t i, const Elf64_Phdr& p) {
  WriteAt(b, ReadAt<Elf64_Ehdr>(b, 0).e_phoff + i * sizeof(Elf64_Phdr), p);
}
std::size_t DataLoadIndex(const std::vector<std::uint8_t>& b) {
  const std::vector<Elf64_Phdr> ph = ProgramHeaders(b);
  for (std::size_t i = 0; i < ph.size(); ++i) {
    if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_W) != 0) return i;
  }
  return ph.size();
}
std::size_t RelroIndex(const std::vector<std::uint8_t>& b) {
  const std::vector<Elf64_Phdr> ph = ProgramHeaders(b);
  for (std::size_t i = 0; i < ph.size(); ++i) {
    if (ph[i].p_type == PT_GNU_RELRO) return i;
  }
  return ph.size();
}

// The file offset of a vaddr that lies in a file-backed PT_LOAD.
std::uint64_t FileOffsetOf(const std::vector<std::uint8_t>& b, std::uint64_t vaddr) {
  for (const Elf64_Phdr& p : ProgramHeaders(b)) {
    if (p.p_type == PT_LOAD && vaddr >= p.p_vaddr && vaddr - p.p_vaddr < p.p_filesz) return p.p_offset + (vaddr - p.p_vaddr);
  }
  return ~std::uint64_t{0};
}
void PutWord(std::vector<std::uint8_t>& b, std::uint64_t vaddr, std::uint32_t w) {
  const std::uint64_t off = FileOffsetOf(b, vaddr);
  CHECK(off != ~std::uint64_t{0});
  if (off != ~std::uint64_t{0}) WriteAt(b, off, w);
}

std::vector<std::uint8_t> BuildStock() {
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

const std::vector<std::uint8_t>& Stock() {
  static const std::vector<std::uint8_t> stock = BuildStock();
  return stock;
}

patch::PatchSpec Spec() {
  patch::PatchSpec spec = patch::PublicSpec();
  spec.stockSha256.clear();  // the synthetic file is not the pinned libr15
  return spec;
}

std::string ReadText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
std::string Fixture() { return ReadText(g_fixtures + "/libr15_extra_rows_example.json"); }

std::vector<patch::WordPatchRow> FixtureRows() {
  std::vector<patch::WordPatchRow> rows;
  patch::Refusal refusal;
  CHECK(patch::ParseExtraRows(Fixture(), Spec(), &rows, &refusal));
  CHECK_EQ(rows.size(), 1u);
  return rows;
}

// The shipped patcher's output for the synthetic stock file.
std::vector<std::uint8_t> PatchedBytes(const std::vector<patch::WordPatchRow>& extra = {}) {
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
Seams g;
LiveImage* g_live = nullptr;

// Every mprotect the library code makes, whether it goes through the seam or calls mprotect itself. The executable's own
// definition wins over libc's for the code linked into it, so a regression that changes a page's protection without the
// seam is seen too. The test's own protection changes go through the system call directly.
std::vector<ProtectCall> g_rawProtects;
}  // namespace

extern "C" int mprotect(void* addr, std::size_t len, int prot) noexcept {
  g_rawProtects.push_back({addr, len, prot});
  return static_cast<int>(syscall(SYS_mprotect, addr, len, prot));
}

namespace {

bool Lookup(const char*, ElfImage* out) {
  if (g_live == nullptr) return false;
  *out = g_live->Image();
  return true;
}
int RecordingProtect(void* addr, std::size_t len, int prot) {
  const int index = static_cast<int>(g.protects.size());
  g.protects.push_back({addr, len, prot});
  if (index == g.protectFailAt) {
    errno = EACCES;
    return -1;
  }
  return mprotect(addr, len, prot);
}
void* RecordingMap(void* addr, std::size_t len, int prot, int flags, int fd, off_t off) {
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
void Sink(sentinel::LogLevel level, const char* line) {
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

AttachOptions Opts() {
  AttachOptions o;
  o.lookup = &Lookup;
  o.protect = &RecordingProtect;
  o.map = &RecordingMap;
  o.end = &RecordingEnd;
  return o;
}

int CountLines(const char* needle) {
  int n = 0;
  for (const std::string& l : g.lines) n += l.find(needle) != std::string::npos ? 1 : 0;
  return n;
}
const std::string* FindLine(const char* needle) {
  for (const std::string& l : g.lines) {
    if (l.find(needle) != std::string::npos) return &l;
  }
  return nullptr;
}
bool Has(const std::string& line, const std::string& needle) { return line.find(needle) != std::string::npos; }

std::string Hex(std::uint64_t v) {
  char buf[19];
  return sentinel::HexString(buf, v);
}

// The data-only rule: no protection change anywhere on the image, by the seam or by a direct call, and no direct call
// that the seam did not see.
void ExpectNoImageProtection(const LiveImage& live) {
  for (const ProtectCall& c : g_rawProtects) CHECK(!live.Intersects(c.addr, c.len));
  for (const ProtectCall& c : g.protects) CHECK(!live.Intersects(c.addr, c.len));
  CHECK_EQ(g_rawProtects.size(), g.protects.size());
}

bool UnmappedNow(const void* page) {
  char perms[5];
  return !sentinel::MappingPermsOf(reinterpret_cast<std::uintptr_t>(page), perms);
}

// ---- the happy path ---------------------------------------------------------------------------------------------------

TEST(TheShippedPatchersOutputIsVerifiedAndEverySlotHoldsItsTrampoline) {
  Env env;
  const std::vector<std::uint8_t> file = PatchedBytes();
  LiveImage live(file);
  g_live = &live;
  live.FillCanaries();
  const std::vector<std::string> mapsBefore = live.MapsLines();

  const AttachResult r = libr15_patch::Attach(Opts());
  CHECK_EQ(r.status, AttachStatus::kVerified);
  CHECK(libr15_patch::Verified());
  CHECK(libr15_patch::VerifiedBuildId() != nullptr);
  CHECK(std::string(libr15_patch::VerifiedBuildId()) == patch::PatchedBuildId());
  CHECK(g.ends.empty());

  std::set<void*> distinct;
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    void* const value = *live.Slot(i);
    if (patch::kRows[i].kind == patch::RowKind::kInlineHook) {
      CHECK(value != nullptr && value != LiveImage::Canary(i));
      CHECK(!live.Intersects(value, 1));  // a trampoline is its own anonymous page, not a page of the image
      distinct.insert(value);
    } else {
      CHECK(value == reinterpret_cast<void*>(live.Base() + credits_text::kBlobVaddr));  // the game's own blob
    }
  }
  CHECK_EQ(distinct.size(), 5u);

  // One verified line, once.
  CHECK_EQ(CountLines("\"event\":\"libr15_patch\""), 1);
  const std::string* line = FindLine("\"status\":\"verified\"");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"build_id\":\"" + patch::PatchedBuildId() + "\""));
    CHECK(Has(*line, std::string("\"stock_build_id\":\"") + patch::kLibR15StockBuildId + "\""));
    CHECK(Has(*line, "\"rows\":6"));
    CHECK(Has(*line, "\"slot_area\":\"" + Hex(patch::kSlotAreaVaddr) + "\""));
    CHECK(!Has(*line, "extra_rows"));
  }

  // Data only: the only protection calls are the five trampoline pages (RW to RX), none on the image.
  CHECK_EQ(g.protects.size(), 5u);
  for (const ProtectCall& c : g.protects) CHECK_EQ(c.prot, PROT_READ | PROT_EXEC);
  ExpectNoImageProtection(live);
  CHECK_EQ(g.mapCalls, 5);
  CHECK(live.MapsLines() == mapsBefore);
}

TEST(TheTrampolinesAreTheTablesStockWordsRelocatedForTheirSitesAndEndAtSitePlus12) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    const patch::Row& row = patch::kRows[i];
    if (row.kind != patch::RowKind::kInlineHook) continue;
    const std::uint32_t window[3] = {row.sites[0].words[0], row.sites[0].words[1], row.sites[0].words[2]};
    const std::uint64_t site = live.Base() + row.sites[0].Vaddr();
    sentinel::Trampoline want;
    sentinel::RelocRefusal refusal{};
    CHECK(sentinel::BuildTrampoline(site, window, want, refusal));
    const std::uint32_t* got = static_cast<const std::uint32_t*>(*live.Slot(i));
    bool same = true;
    for (std::size_t w = 0; w < want.count; ++w) same = same && got[w] == want.words[w];
    CHECK(same);
    // The three displaced words are not PC-relative in this table, so they are copied unchanged.
    CHECK_EQ(got[0], row.sites[0].words[0]);
    CHECK_EQ(got[1], row.sites[0].words[1]);
    CHECK_EQ(got[2], row.sites[0].words[2]);
    // The continue address, site + 12, sits in the literal pool.
    bool continues = false;
    for (std::size_t w = 0; w + 1 < want.count; w += 2) {
      std::uint64_t quad = 0;
      std::memcpy(&quad, got + w, sizeof quad);
      continues = continues || quad == site + 12;
    }
    CHECK(continues);
    char perms[5] = {};
    CHECK(sentinel::MappingPermsOf(reinterpret_cast<std::uintptr_t>(got), perms));
    CHECK_EQ(std::string(perms), std::string("r-xp"));
  }
}

TEST(TheCreditsSlotValueIsTheBlobTheStockPairsLoad) {
  // Attach stores base + credits_text::kBlobVaddr; that is what the replaced ADRP+ADD pairs computed.
  const patch::Row* credits = nullptr;
  for (const patch::Row& row : patch::kRows) {
    if (row.kind == patch::RowKind::kCreditsPair) credits = &row;
  }
  CHECK(credits != nullptr);
  if (credits == nullptr) return;
  for (std::size_t pair = 0; pair < 2; ++pair) {
    const patch::Site& adrp = credits->sites[2 * pair];
    const patch::Site& add = credits->sites[2 * pair + 1];
    unsigned rd = 0;
    std::uint64_t page = 0;
    CHECK(patch::DecodeAdrp(adrp.words[0], adrp.Vaddr(), &rd, &page));
    CHECK_EQ(page + ((add.words[0] >> 10) & 0xFFFu), credits_text::kBlobVaddr);
  }
}

// ---- identity: the refusals of ADR section 7 --------------------------------------------------------------------------

void ExpectNothingInstalled(const LiveImage& live, const char* reason) {
  CHECK(!libr15_patch::Verified());
  CHECK(libr15_patch::VerifiedBuildId() == nullptr);
  CHECK(live.SlotsAreCanaries());
  CHECK(g.ends.empty());  // the game runs as it came: the process is not ended
  CHECK(g.protects.empty());
  CHECK(g_rawProtects.empty());
  CHECK_EQ(g.mapCalls, 0);
  const std::string* line = FindLine(reason);
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"event\":\"libr15_patch\""));
    CHECK(Has(*line, "\"level\":\"error\""));
  }
  CHECK_EQ(CountLines("\"status\":\"verified\""), 0);
}

TEST(TheStockLibr15IsNotPatchedAndNothingIsInstalled) {
  Env env;
  LiveImage live(Stock());
  g_live = &live;
  live.FillCanaries();
  const AttachResult r = libr15_patch::Attach(Opts());
  CHECK_EQ(r.status, AttachStatus::kNotPatched);
  CHECK(r.reason == "libr15_not_patched");
  ExpectNothingInstalled(live, "\"reason\":\"libr15_not_patched\"");
}

TEST(AnotherBuildIdIsABuildIdMismatch) {
  Env env;
  std::vector<std::uint8_t> file = PatchedBytes();
  file[FileOffsetOf(file, kNoteDescVaddr)] ^= 0x01;  // a patched ID with one bit changed
  LiveImage live(file);
  g_live = &live;
  live.FillCanaries();
  const AttachResult r = libr15_patch::Attach(Opts());
  CHECK_EQ(r.status, AttachStatus::kBuildIdMismatch);
  CHECK(r.reason == "build_id_mismatch");
  ExpectNothingInstalled(live, "\"reason\":\"build_id_mismatch\"");
}

TEST(ALibr15PatchedWithoutExtraRowsIsAMismatchForAnAttachThatHasThem) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  live.FillCanaries();
  AttachOptions o = Opts();
  o.extraRows = FixtureRows();
  const AttachResult r = libr15_patch::Attach(o);
  CHECK_EQ(r.status, AttachStatus::kBuildIdMismatch);
  ExpectNothingInstalled(live, "\"reason\":\"build_id_mismatch\"");
}

TEST(ALibr15PatchedWithExtraRowsIsAMismatchForAnAttachThatHasNone) {
  Env env;
  LiveImage live(PatchedBytes(FixtureRows()));
  g_live = &live;
  live.FillCanaries();
  const AttachResult r = libr15_patch::Attach(Opts());
  CHECK_EQ(r.status, AttachStatus::kBuildIdMismatch);
  ExpectNothingInstalled(live, "\"reason\":\"build_id_mismatch\"");
}

TEST(NoLibr15AtAllIsModuleNotLoaded) {
  Env env;
  g_live = nullptr;
  const AttachResult r = libr15_patch::Attach(Opts());
  CHECK_EQ(r.status, AttachStatus::kModuleNotLoaded);
  CHECK(!libr15_patch::Verified());
  CHECK(g.ends.empty());
  CHECK(FindLine("\"reason\":\"module_not_loaded\"") != nullptr);
}

TEST(ASecondAttachTouchesNothing) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  void* const slot0 = *live.Slot(0);
  const std::size_t protects = g.protects.size();
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kAlreadyVerified);
  CHECK(*live.Slot(0) == slot0);
  CHECK_EQ(g.protects.size(), protects);
  CHECK_EQ(CountLines("\"status\":\"verified\""), 1);
}

// ---- the row checks ---------------------------------------------------------------------------------------------------

// Runs Attach on `file` (laid out with `skew`) and expects check `check` of row `rowId` to refuse with `reason`: every
// slot untouched, no trampoline made, the process-end call made once with the reason.
void ExpectRowRefusal(const std::vector<std::uint8_t>& file, const char* reason, const char* check, const char* rowId,
                      std::uintptr_t skew = 0, const std::vector<patch::WordPatchRow>& extra = {}) {
  Env env;
  LiveImage live(file, skew);
  g_live = &live;
  live.FillCanaries();
  AttachOptions o = Opts();
  o.extraRows = extra;
  const AttachResult r = libr15_patch::Attach(o);
  CHECK_EQ(r.status, AttachStatus::kRowFailed);
  CHECK(r.reason == reason);
  CHECK(r.rowId == rowId);
  CHECK(!libr15_patch::Verified());
  CHECK(live.SlotsAreCanaries());
  CHECK(g.protects.empty());
  CHECK(g_rawProtects.empty());
  CHECK_EQ(g.mapCalls, 0);
  CHECK_EQ(g.ends.size(), 1u);
  if (g.ends.size() == 1) {
    CHECK_EQ(g.ends[0].code, process_end::kFailureExitCode);
    CHECK(g.ends[0].reason == reason);
  }
  const std::string* line = FindLine(reason);
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"level\":\"error\""));
    CHECK(Has(*line, std::string("\"id\":\"") + rowId + "\""));
    CHECK(Has(*line, std::string("\"check\":\"") + check + "\""));
  }
  CHECK_EQ(CountLines("\"status\":\"verified\""), 0);
}

std::uint64_t SiteVaddr(std::size_t row, std::size_t site = 0) { return patch::kRows[row].sites[site].Vaddr(); }

TEST(CheckA_AStubWordThatIsNotTheTablesIsRefusedWithTheRowsId) {
  std::vector<std::uint8_t> file = PatchedBytes();
  PutWord(file, SiteVaddr(0) + 8, 0);  // the BR of lobby_full.join
  ExpectRowRefusal(file, "libr15_patch_row_stub", "stub", "lobby_full.join");
}

TEST(CheckA_AStockSiteIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  for (std::size_t w = 0; w < 3; ++w) PutWord(file, SiteVaddr(1) + 4 * w, patch::kRows[1].sites[0].words[w]);
  ExpectRowRefusal(file, "libr15_patch_row_stub", "stub", "lobby_full.failure");
}

TEST(CheckA_AStubWithAnotherTablesSlotIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  patch::Stub other{};
  CHECK(patch::EncodeSlotStub(SiteVaddr(2), patch::kRows[0].SlotVaddr(), &other));  // row 2's site, row 0's slot
  for (std::size_t w = 0; w < 3; ++w) PutWord(file, SiteVaddr(2) + 4 * w, other.words[w]);
  ExpectRowRefusal(file, "libr15_patch_row_stub", "stub", "lobby_full.success");
}

TEST(CheckA_ACreditsLdrThatIsStillTheStockAddIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  const std::size_t credits = patch::kRowCount - 1;
  PutWord(file, SiteVaddr(credits, 3), patch::kRows[credits].sites[3].words[0]);  // the second pair's LDR back to ADD
  ExpectRowRefusal(file, "libr15_patch_row_stub", "stub", "credits.pairs");
}

TEST(CheckA_ACreditsAdrpOfTheFirstPairThatIsStockIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  const std::size_t credits = patch::kRowCount - 1;
  PutWord(file, SiteVaddr(credits, 0), patch::kRows[credits].sites[0].words[0]);
  ExpectRowRefusal(file, "libr15_patch_row_stub", "stub", "credits.pairs");
}

TEST(CheckB_ADecodedSlotThatIsNotTheTablesIsRefused) {
  // The words are exactly the stub the table implies, so check (a) passes; but the stub is position independent only for
  // a page-aligned load bias. At a bias that is not, the live ADRP names another slot than base + the table's.
  ExpectRowRefusal(PatchedBytes(), "libr15_patch_row_slot", "slot", "lobby_full.join", 0x200);
}

TEST(CheckC_ASlotOutsideTheGrownSegmentIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  const std::size_t d = DataLoadIndex(file);
  Elf64_Phdr p = ProgramHeaders(file)[d];
  p.p_memsz = patch::kStockImageEnd - p.p_vaddr;  // the stock segment: the patcher's growth missing
  SetProgramHeader(file, d, p);
  ExpectRowRefusal(file, "libr15_patch_row_segment", "segment", "lobby_full.join");
}

TEST(CheckC_ASlotInTheFileBackedPartIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  const std::size_t d = DataLoadIndex(file);
  Elf64_Phdr p = ProgramHeaders(file)[d];
  p.p_filesz = p.p_memsz;  // no bss part: the slots would be file data
  SetProgramHeader(file, d, p);
  ExpectRowRefusal(file, "libr15_patch_row_segment", "segment", "lobby_full.join");
}

TEST(CheckC_ASlotInAReadOnlySegmentIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  const std::size_t d = DataLoadIndex(file);
  Elf64_Phdr p = ProgramHeaders(file)[d];
  p.p_flags = PF_R;
  SetProgramHeader(file, d, p);
  ExpectRowRefusal(file, "libr15_patch_row_segment", "segment", "lobby_full.join");
}

TEST(CheckC_ASlotInsideRelroIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  const std::size_t r = RelroIndex(file);
  Elf64_Phdr p = ProgramHeaders(file)[r];
  p.p_memsz = patch::kSlotAreaVaddr + 8 - p.p_vaddr;  // GNU_RELRO reaching the first slot
  SetProgramHeader(file, r, p);
  ExpectRowRefusal(file, "libr15_patch_row_segment", "segment", "lobby_full.join");
}

TEST(AWordPatchRowWhoseLiveWordDiffersIsRefusedAndNamesTheRow) {
  const std::vector<patch::WordPatchRow> rows = FixtureRows();
  std::vector<std::uint8_t> file = PatchedBytes(rows);
  const std::uint8_t stockBytes[4] = {0xe0, 0x03, 0x1f, 0xaa};
  std::uint32_t stockWord = 0;
  std::memcpy(&stockWord, stockBytes, 4);
  PutWord(file, kFixturePage + 0x10, stockWord);  // the replacement put back to the stock word
  ExpectRowRefusal(file, "libr15_patch_row_words", "words", "x.example_guard", 0, rows);
}

TEST(AWordPatchRowOutsideEveryLoadIsRefused) {
  std::vector<patch::WordPatchRow> rows = FixtureRows();
  rows[0].revaultVa = 0x7f000010;  // not in the image
  // The ID of a build with these rows differs from the file's, so build the file with the same rows.
  const patch::PatchSpec spec = Spec();
  // The patcher refuses a word outside the text, so the file cannot be made; Attach must refuse the same row on a
  // file whose build ID names it, which only a stage-1 defect could produce. Here the ID is forged to say so.
  const patch::BuildIdBytes forged = patch::DeriveBuildId(spec.stockBuildId, patch::CanonicalTable(spec.rows, spec.rowCount, rows, patch::kSlotAreaVaddr));
  std::vector<std::uint8_t> patched = PatchedBytes();
  std::memcpy(patched.data() + FileOffsetOf(patched, kNoteDescVaddr), forged.data(), forged.size());
  ExpectRowRefusal(patched, "libr15_patch_row_words", "words", "x.example_guard", 0, rows);
}

TEST(AnImageWithTheExtraRowsAppliedIsVerifiedAndTheLineNeverNamesTheRows) {
  Env env;
  const std::vector<patch::WordPatchRow> rows = FixtureRows();
  LiveImage live(PatchedBytes(rows));
  g_live = &live;
  AttachOptions o = Opts();
  o.extraRows = rows;
  const std::string fixture = Fixture();
  o.extraRowsSha256 = data::Sha256Hex(fixture.data(), fixture.size());
  CHECK_EQ(libr15_patch::Attach(o).status, AttachStatus::kVerified);
  CHECK(std::string(libr15_patch::VerifiedBuildId()) == patch::PatchedBuildId(rows));
  CHECK(patch::PatchedBuildId(rows) != patch::PatchedBuildId());
  const std::string* line = FindLine("\"status\":\"verified\"");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"extra_rows\":1"));
    CHECK(Has(*line, "\"extra_rows_sha256\":\"" + o.extraRowsSha256 + "\""));
    CHECK(!Has(*line, "x.example_guard"));
    CHECK(!Has(*line, "1500010"));
  }
}

// ---- the trampolines ----------------------------------------------------------------------------------------------------

void ExpectTrampolineRefusal(const char* op, int errnoValue, std::size_t failingRow) {
  LiveImage live(PatchedBytes());
  g_live = &live;
  live.FillCanaries();
  const AttachResult r = libr15_patch::Attach(Opts());
  CHECK_EQ(r.status, AttachStatus::kTrampolineFailed);
  CHECK(r.reason == "libr15_patch_trampoline_failed");
  CHECK(r.rowId == patch::kRows[failingRow].id);
  CHECK(!libr15_patch::Verified());
  CHECK(live.SlotsAreCanaries());  // nothing activated: no slot holds a trampoline
  CHECK_EQ(g.ends.size(), 1u);
  if (g.ends.size() == 1) {
    CHECK_EQ(g.ends[0].code, process_end::kFailureExitCode);
    CHECK(g.ends[0].reason == "libr15_patch_trampoline_failed");
  }
  const std::string* line = FindLine("\"reason\":\"libr15_patch_trampoline_failed\"");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, std::string("\"op\":\"") + op + "\""));
    CHECK(Has(*line, "\"errno\":" + std::to_string(errnoValue)));
    CHECK(Has(*line, "\"page_perms\":\"r-xp\""));  // the site's page, read from /proc/self/maps
    CHECK(Has(*line, std::string("\"id\":\"") + patch::kRows[failingRow].id + "\""));
  }
  for (void* page : g.mapped) CHECK(UnmappedNow(page));  // every page made so far is given back
  CHECK_EQ(CountLines("\"status\":\"verified\""), 0);
}

TEST(AFailedTrampolineMapEndsTheProcessWithItsErrnoAndActivatesNothing) {
  Env env;
  g.mapFailAt = 2;
  ExpectTrampolineRefusal("trampoline_map", ENOMEM, 2);
  CHECK_EQ(g.mapped.size(), 2u);  // the two before it were made
}

TEST(AFailedTrampolineProtectEndsTheProcessWithItsErrnoAndActivatesNothing) {
  Env env;
  g.protectFailAt = 1;
  ExpectTrampolineRefusal("trampoline_rx", EACCES, 1);
  CHECK_EQ(g.mapped.size(), 2u);
}

// ---- activation -----------------------------------------------------------------------------------------------------------

void DummyDetour() {}

TEST(ActivationPublishesTheTrampolineThenStoresTheDetourAndTouchesNoPage) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  const std::vector<std::string> mapsBefore = live.MapsLines();
  const std::size_t protectsBefore = g.protects.size();
  void* const trampoline = *live.Slot(0);
  void* original = reinterpret_cast<void*>(0x1234);
  void* const detour = reinterpret_cast<void*>(&DummyDetour);
  g.lines.clear();
  CHECK_EQ(AttachTestAccess::Activate("lobby_full.join", detour, &original), ActivateStatus::kOk);
  CHECK(*live.Slot(0) == detour);
  CHECK(original == trampoline);
  const std::string* line = FindLine("\"event\":\"inline_hook\"");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"status\":\"ok\""));
    CHECK(Has(*line, "\"id\":\"lobby_full.join\""));
    CHECK(Has(*line, "\"target\":\"" + Hex(live.Base() + SiteVaddr(0)) + "\""));
    CHECK(Has(*line, "\"slot\":\"" + Hex(reinterpret_cast<std::uintptr_t>(live.Slot(0))) + "\""));
    CHECK(Has(*line, "\"trampoline\":\"" + Hex(reinterpret_cast<std::uintptr_t>(trampoline)) + "\""));
    CHECK(Has(*line, "\"page_perms\":\"r-xp\""));
  }
  CHECK_EQ(g.protects.size(), protectsBefore);
  ExpectNoImageProtection(live);
  CHECK(live.MapsLines() == mapsBefore);
}

TEST(ActivatingEveryInlineRowNeverCallsTheProtectionSeamOnTheImage) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  const std::vector<std::string> mapsBefore = live.MapsLines();
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  for (const patch::Row& row : patch::kRows) {
    if (row.kind != patch::RowKind::kInlineHook) continue;
    void* original = nullptr;
    CHECK_EQ(AttachTestAccess::Activate(row.id, reinterpret_cast<void*>(&DummyDetour), &original), ActivateStatus::kOk);
  }
  // Zero calls whose range intersects the image's mapping: the test that fails if a code write comes back.
  ExpectNoImageProtection(live);
  CHECK_EQ(g.protects.size(), 5u);
  CHECK(live.MapsLines() == mapsBefore);
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    if (patch::kRows[i].kind == patch::RowKind::kInlineHook) CHECK(*live.Slot(i) == reinterpret_cast<void*>(&DummyDetour));
  }
}

TEST(ASecondActivationOfARowIsRefusedAndChangesNothing) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  void* original = nullptr;
  void* const first = reinterpret_cast<void*>(&DummyDetour);
  CHECK_EQ(AttachTestAccess::Activate("lobby_full.failure", first, &original), ActivateStatus::kOk);
  void* const trampoline = original;
  void* other = reinterpret_cast<void*>(0x4242);
  void* nonce = reinterpret_cast<void*>(0x77);
  CHECK_EQ(AttachTestAccess::Activate("lobby_full.failure", other, &nonce), ActivateStatus::kAlreadyActivated);
  CHECK(*live.Slot(1) == first);
  CHECK(nonce == reinterpret_cast<void*>(0x77));
  CHECK(trampoline != nullptr);
}

TEST(ActivationBeforeAttachIsRefusedAndStoresNothing) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  live.FillCanaries();
  void* original = nullptr;
  CHECK_EQ(AttachTestAccess::Activate("lobby_full.join", reinterpret_cast<void*>(&DummyDetour), &original), ActivateStatus::kNotVerified);
  CHECK(original == nullptr);
  CHECK(live.SlotsAreCanaries());
  CHECK(FindLine("\"status\":\"not_verified\"") != nullptr);
}

TEST(ActivationRefusesAnUnknownRowTheCreditsRowAndNullArguments) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  void* original = nullptr;
  void* const detour = reinterpret_cast<void*>(&DummyDetour);
  void* const creditsSlot = *live.Slot(patch::kRowCount - 1);
  CHECK_EQ(AttachTestAccess::Activate("no.such_row", detour, &original), ActivateStatus::kUnknownRow);
  CHECK_EQ(AttachTestAccess::Activate("credits.pairs", detour, &original), ActivateStatus::kNotInlineRow);
  CHECK(*live.Slot(patch::kRowCount - 1) == creditsSlot);
  CHECK_EQ(AttachTestAccess::Activate(nullptr, detour, &original), ActivateStatus::kBadArgument);
  CHECK_EQ(AttachTestAccess::Activate("lobby_full.join", nullptr, &original), ActivateStatus::kBadArgument);
  CHECK_EQ(AttachTestAccess::Activate("lobby_full.join", detour, nullptr), ActivateStatus::kBadArgument);
  CHECK(original == nullptr);
}

struct RowTestTag {};
using RowTestThunk = sentinel::CallbackThunk<RowTestTag, int(int)>;

TEST(InstallRowThunkStoresTheThunksEntryAndPublishesTheTrampolineAsItsOriginal) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  void* const trampoline = *live.Slot(3);
  CHECK_EQ(sentinel::InstallRowThunk<RowTestThunk>("ui_texture.attach"), ActivateStatus::kOk);
  CHECK(*live.Slot(3) == RowTestThunk::EntryAddress());
  CHECK(*RowTestThunk::OriginalOut() == trampoline);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: libr15_attach_test <fixtures dir> [name-filter]\n");
    return 2;
  }
  g_fixtures = argv[1];
  return mini_test::RunAll(argc - 1, argv + 1);
}
