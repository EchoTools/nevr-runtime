// Host test of the shipped quest_patch_libr15 tool on the real store libr15.so (ADR 0021 test 2, "pinned"): the file is
// passed as an argument (it is the store APK's, 58 MB, and is never copied into the repository) and the tool is run
// as a process. Every fact below is read back from the files, with readelf and a small independent ELF reader, not
// from the patcher's own code.
//
// Run: libr15_patcher_pinned_test <quest_patch_libr15> <stock libr15.so> <scratch dir> [--signed-libr15=<file>]
//        [--reference-libr15=<file>]... [name-filter]
// The recipe removes the scratch dir's files afterwards; the patched copies are written there.
// --signed-libr15=<file> is the lib/arm64-v8a/libr15.so taken out of the signed APK of a repack (ADR 0021 test 5): with it
// SignedApkLibr15IsThePatchersOutput runs; without it that test is dropped from the run and the run prints a SKIP line
// naming it (it is never left out silently).
// --reference-libr15=<file> is a reference build's own patched libr15.so (one per flag): with one or more,
// ReferenceLibr15sArePatchedWithTheirStateWordsClearOfTheSlots runs; without, it is dropped and a SKIP line names it.
#include <elf.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "mini_test.h"
#include "quest/data/sha256.h"
#include "quest/patch/libr15_patch_identity.h"
#include "quest/patch/libr15_patch_table.h"
#include "quest/patch/stub_codec.h"

namespace {

using namespace nevr_quest::patch;

std::string g_tool, g_stock, g_scratch, g_signed;
std::vector<std::string> g_refs;
constexpr const char* kSignedFlag = "--signed-libr15=";
constexpr const char* kSignedTest = "SignedApkLibr15IsThePatchersOutput";
constexpr const char* kRefFlag = "--reference-libr15=";
constexpr const char* kRefTest = "ReferenceLibr15sArePatchedWithTheirStateWordsClearOfTheSlots";

std::vector<std::uint8_t> ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string ReadText(const std::string& path) {
  const auto b = ReadAll(path);
  return std::string(b.begin(), b.end());
}

int Run(const std::vector<std::string>& argv, const std::string& outPath, const std::string& errPath) {
  const pid_t pid = fork();
  if (pid == 0) {
    const int o = open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int e = open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (o < 0 || e < 0 || dup2(o, 1) < 0 || dup2(e, 2) < 0) _exit(127);
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

// A small reader: the program and section headers of an ELF64 file.
struct Image {
  std::vector<std::uint8_t> bytes;
  Elf64_Ehdr eh{};
  std::vector<Elf64_Phdr> ph;
  std::vector<Elf64_Shdr> sh;
  bool Load(const std::string& path) {
    bytes = ReadAll(path);
    if (bytes.size() < sizeof(Elf64_Ehdr)) return false;
    std::memcpy(&eh, bytes.data(), sizeof eh);
    if (eh.e_phoff + eh.e_phnum * sizeof(Elf64_Phdr) > bytes.size() || eh.e_shoff + eh.e_shnum * sizeof(Elf64_Shdr) > bytes.size()) return false;
    ph.resize(eh.e_phnum);
    sh.resize(eh.e_shnum);
    std::memcpy(ph.data(), bytes.data() + eh.e_phoff, ph.size() * sizeof(Elf64_Phdr));
    std::memcpy(sh.data(), bytes.data() + eh.e_shoff, sh.size() * sizeof(Elf64_Shdr));
    return true;
  }
  // The file offset of a virtual address inside a file-backed PT_LOAD, or 0 when there is none.
  std::uint64_t Offset(std::uint64_t vaddr) const {
    for (const Elf64_Phdr& p : ph) {
      if (p.p_type == PT_LOAD && vaddr >= p.p_vaddr && vaddr < p.p_vaddr + p.p_filesz) return vaddr - p.p_vaddr + p.p_offset;
    }
    return 0;
  }
  std::uint32_t Word(std::uint64_t vaddr) const {
    std::uint32_t w = 0;
    const std::uint64_t off = Offset(vaddr);
    if (off != 0) std::memcpy(&w, bytes.data() + off, 4);
    return w;
  }
  std::size_t WritableLoad() const {
    std::size_t best = ph.size();
    for (std::size_t i = 0; i < ph.size(); ++i) {
      if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_W) != 0 &&
          (best == ph.size() || ph[i].p_vaddr + ph[i].p_memsz > ph[best].p_vaddr + ph[best].p_memsz)) {
        best = i;
      }
    }
    return best;
  }
};

struct Range {
  std::uint64_t begin, end;
  const char* what;
};

std::string Hex(std::uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
  return b;
}

// One run of the tool: exit code, stdout, stderr and the files it wrote.
struct Run1 {
  int exitCode = -1;
  std::string out, err, report, outFile;
};

Run1 RunTool(const std::string& tag) {
  Run1 r;
  r.outFile = g_scratch + "/" + tag + ".so";
  const std::string report = g_scratch + "/" + tag + ".json";
  const std::string so = g_scratch + "/" + tag + ".stdout", se = g_scratch + "/" + tag + ".stderr";
  std::remove(r.outFile.c_str());
  r.exitCode = Run({g_tool, "--stock", g_stock, "--out", r.outFile, "--report", report}, so, se);
  r.out = ReadText(so);
  r.err = ReadText(se);
  r.report = ReadText(report);
  return r;
}

const Run1& First() {
  static const Run1 r = RunTool("run1");
  return r;
}
const Run1& Second() {
  static const Run1 r = RunTool("run2");
  return r;
}

std::string Readelf(const std::string& flag, const std::string& file, const std::string& tag) {
  const std::string so = g_scratch + "/" + tag + ".txt", se = g_scratch + "/" + tag + ".err";
  const int rc = Run({"readelf", flag, "-W", file}, so, se);
  CHECK_EQ(rc, 0);
  CHECK(ReadText(se).empty());  // readelf warns about a malformed file on stderr
  return ReadText(so);
}

// ---- the tests -------------------------------------------------------------------------------------------------------

TEST(TheInputIsThePinnedStockFile) {
  const auto bytes = ReadAll(g_stock);
  CHECK(bytes.size() > 1000000);
  CHECK_EQ(nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()), std::string(kLibR15StockSha256));
  CHECK(Readelf("-n", g_stock, "stock_note").find(std::string("Build ID: ") + kLibR15StockBuildId) != std::string::npos);
}

TEST(EveryRowsStockWordsAreWhatTheStockFileHoldsAtThatAddress) {
  Image stock;
  if (!stock.Load(g_stock)) {
    CHECK(false);  // the file is missing or is not an ELF
    return;
  }
  for (std::size_t r = 0; r < kRowCount; ++r) {
    for (std::size_t s = 0; s < kRows[r].siteCount; ++s) {
      const Site& site = kRows[r].sites[s];
      for (std::size_t w = 0; w < site.wordCount; ++w) {
        const std::uint64_t vaddr = site.revaultVa - kRevaultBias + 4 * w;
        if (stock.Word(vaddr) != site.words[w]) {
          std::fprintf(stderr, "  %s site %zu word %zu at %s: file %08x, table %08x\n", kRows[r].id, s, w, Hex(vaddr).c_str(),
                       stock.Word(vaddr), site.words[w]);
          ++mini_test::Failures();
        }
      }
    }
  }
}

TEST(TheToolPatchesTheStockFileAndSaysSo) {
  const Run1& r = First();
  CHECK_EQ(r.exitCode, 0);
  CHECK(r.err.empty());
  CHECK(!ReadAll(r.outFile).empty());
  // Stdout is JSON lines only: 5 inline sites + 4 credits sites, then the summary.
  std::size_t lines = 0, sites = 0, summaries = 0;
  std::size_t at = 0;
  while (at < r.out.size()) {
    const std::size_t nl = r.out.find('\n', at);
    const std::string line = r.out.substr(at, nl - at);
    CHECK(!line.empty() && line.front() == '{' && line.back() == '}');
    if (line.find("\"event\":\"libr15_patch_site\"") != std::string::npos) ++sites;
    if (line.find("\"event\":\"libr15_patch\"") != std::string::npos) ++summaries;
    ++lines;
    if (nl == std::string::npos) break;
    at = nl + 1;
  }
  CHECK_EQ(sites, static_cast<std::size_t>(9));
  CHECK_EQ(summaries, static_cast<std::size_t>(1));
  CHECK_EQ(lines, static_cast<std::size_t>(10));
}

TEST(TwoRunsGiveByteIdenticalFilesAndReports) {
  const Run1& a = First();
  const Run1& b = Second();
  CHECK_EQ(a.exitCode, 0);
  CHECK_EQ(b.exitCode, 0);
  CHECK(ReadAll(a.outFile) == ReadAll(b.outFile));
  CHECK(a.report == b.report);
  CHECK(a.out == b.out);
}

TEST(ThePatchedBuildIdIsTheDerivedOne) {
  const Run1& r = First();
  const std::string note = Readelf("-n", r.outFile, "patched_note");
  CHECK(note.find("Build ID: " + PatchedBuildId()) != std::string::npos);
  CHECK(PatchedBuildId() != kLibR15StockBuildId);
  CHECK(r.out.find("\"build_id\":\"" + PatchedBuildId() + "\"") != std::string::npos);
}

TEST(OnlyTheExpectedByteRangesDiffer) {
  Image stock, patched;
  if (!stock.Load(g_stock)) {
    CHECK(false);  // the file is missing or is not an ELF
    return;
  }
  if (!patched.Load(First().outFile)) {
    CHECK(false);  // the file is missing or is not an ELF
    return;
  }
  CHECK_EQ(stock.bytes.size(), patched.bytes.size());
  const std::size_t wl = stock.WritableLoad();
  CHECK(wl < stock.ph.size());
  if (wl >= stock.ph.size()) return;
  // The ranges the ADR allows, built from the table and the headers of the stock file.
  std::vector<Range> allowed;
  for (std::size_t r = 0; r < kRowCount; ++r) {
    for (std::size_t s = 0; s < kRows[r].siteCount; ++s) {
      const Site& site = kRows[r].sites[s];
      const std::uint64_t off = stock.Offset(site.Vaddr());
      CHECK(off != 0);
      allowed.push_back({off, off + 4ull * (kRows[r].kind == RowKind::kInlineHook ? 3 : 1), kRows[r].id});
    }
  }
  allowed.push_back({stock.eh.e_phoff + wl * sizeof(Elf64_Phdr) + offsetof(Elf64_Phdr, p_memsz),
                     stock.eh.e_phoff + wl * sizeof(Elf64_Phdr) + offsetof(Elf64_Phdr, p_memsz) + 8, "p_memsz"});
  std::size_t bss = stock.sh.size();
  for (std::size_t i = 0; i < stock.sh.size(); ++i) {
    if (stock.sh[i].sh_type == SHT_NOBITS && stock.sh[i].sh_addr + stock.sh[i].sh_size == kStockImageEnd) bss = i;
  }
  CHECK(bss < stock.sh.size());
  if (bss >= stock.sh.size()) return;
  allowed.push_back({stock.eh.e_shoff + bss * sizeof(Elf64_Shdr) + offsetof(Elf64_Shdr, sh_size),
                     stock.eh.e_shoff + bss * sizeof(Elf64_Shdr) + offsetof(Elf64_Shdr, sh_size) + 8, ".bss sh_size"});
  // The build-ID note's 20 descriptor bytes: the note is the PT_NOTE that holds "GNU".
  std::uint64_t idOffset = 0;
  for (const Elf64_Phdr& p : stock.ph) {
    if (p.p_type != PT_NOTE) continue;
    for (std::uint64_t o = p.p_offset; o + 16 + 20 <= p.p_offset + p.p_filesz; o += 4) {
      Elf64_Nhdr nh;
      std::memcpy(&nh, stock.bytes.data() + o, sizeof nh);
      if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4 && nh.n_descsz == 20 && std::memcmp(stock.bytes.data() + o + 12, "GNU", 4) == 0) {
        idOffset = o + 16;
      }
    }
  }
  CHECK(idOffset != 0);
  allowed.push_back({idOffset, idOffset + 20, "build id"});

  // Every differing byte, merged into ranges, listed, and each inside an allowed range.
  std::vector<Range> differing;
  for (std::uint64_t i = 0; i < stock.bytes.size(); ++i) {
    if (stock.bytes[i] == patched.bytes[i]) continue;
    if (!differing.empty() && differing.back().end == i) {
      ++differing.back().end;
    } else {
      differing.push_back({i, i + 1, ""});
    }
  }
  std::uint64_t total = 0;
  for (const Range& d : differing) {
    total += d.end - d.begin;
    const Range* in = nullptr;
    for (const Range& a : allowed) {
      if (d.begin >= a.begin && d.end <= a.end) in = &a;
    }
    std::printf("  differs %s..%s (%llu bytes): %s\n", Hex(d.begin).c_str(), Hex(d.end).c_str(), static_cast<unsigned long long>(d.end - d.begin),
                in != nullptr ? in->what : "NOT ALLOWED");
    CHECK(in != nullptr);
  }
  // Each allowed range changed somewhere.
  for (const Range& a : allowed) {
    bool touched = false;
    for (const Range& d : differing) touched = touched || (d.begin < a.end && a.begin < d.end);
    if (!touched) std::fprintf(stderr, "  %s %s..%s did not change\n", a.what, Hex(a.begin).c_str(), Hex(a.end).c_str());
    CHECK(touched);
  }
  std::printf("  %zu ranges, %llu bytes differ\n", differing.size(), static_cast<unsigned long long>(total));
}

TEST(EveryStubAndCreditsWordDecodesBackToItsSlot) {
  Image stock, patched;
  if (!stock.Load(g_stock)) {
    CHECK(false);  // the file is missing or is not an ELF
    return;
  }
  if (!patched.Load(First().outFile)) {
    CHECK(false);  // the file is missing or is not an ELF
    return;
  }
  for (std::size_t r = 0; r < kRowCount; ++r) {
    const std::uint64_t slot = kSlotAreaVaddr + kSlotBytes * r;
    if (kRows[r].kind == RowKind::kInlineHook) {
      const std::uint64_t site = kRows[r].sites[0].Vaddr();
      const std::uint32_t words[3] = {patched.Word(site), patched.Word(site + 4), patched.Word(site + 8)};
      std::uint64_t decoded = 0;
      CHECK(DecodeSlotStub(words, site, &decoded));
      CHECK_EQ(decoded, slot);
    } else {
      for (std::size_t pair = 0; pair < 2; ++pair) {
        const std::uint64_t adrp = kRows[r].sites[2 * pair].Vaddr();
        const std::uint64_t ldr = kRows[r].sites[2 * pair + 1].Vaddr();
        unsigned rdA = 0, rdStock = 0, rt = 0, rn = 0;
        std::uint64_t page = 0, stockPage = 0, offset = 0;
        CHECK(DecodeAdrp(patched.Word(adrp), adrp, &rdA, &page));
        CHECK(DecodeAdrp(stock.Word(adrp), adrp, &rdStock, &stockPage));
        CHECK(DecodeLdrX(patched.Word(ldr), &rt, &rn, &offset));
        CHECK_EQ(rdA, rdStock);  // the same register as the stock ADRP
        CHECK_EQ(rt, rdStock);
        CHECK_EQ(rn, rdStock);
        CHECK_EQ(page + offset, slot);
      }
    }
  }
}

TEST(TheSegmentsAndSectionsAreSaneAndTheEndSymbolsUnchanged) {
  const Run1& r = First();
  const std::string stockLoad = Readelf("-l", g_stock, "stock_l"), patchedLoad = Readelf("-l", r.outFile, "patched_l");
  const std::string stockSec = Readelf("-S", g_stock, "stock_s"), patchedSec = Readelf("-S", r.outFile, "patched_s");
  Image stock, patched;
  if (!stock.Load(g_stock)) {
    CHECK(false);  // the file is missing or is not an ELF
    return;
  }
  if (!patched.Load(r.outFile)) {
    CHECK(false);  // the file is missing or is not an ELF
    return;
  }
  // PT_LOAD: ascending vaddrs, filesz <= memsz, only the writable one grew, by exactly the slot area.
  std::uint64_t previous = 0;
  for (std::size_t i = 0; i < patched.ph.size(); ++i) {
    CHECK(std::memcmp(&stock.ph[i], &patched.ph[i], sizeof(Elf64_Phdr)) == 0 || i == stock.WritableLoad());
    if (patched.ph[i].p_type != PT_LOAD) continue;
    CHECK(patched.ph[i].p_vaddr >= previous);
    previous = patched.ph[i].p_vaddr;
    CHECK(patched.ph[i].p_filesz <= patched.ph[i].p_memsz);
  }
  const std::size_t wl = stock.WritableLoad();
  CHECK(wl < stock.ph.size());
  if (wl >= stock.ph.size()) return;
  CHECK_EQ(stock.ph[wl].p_vaddr + stock.ph[wl].p_memsz, kStockImageEnd);
  CHECK_EQ(patched.ph[wl].p_vaddr + patched.ph[wl].p_memsz, kSlotAreaVaddr + kSlotBytes * kRowCount);
  CHECK_EQ(patched.ph[wl].p_filesz, stock.ph[wl].p_filesz);
  CHECK_EQ(patched.ph[wl].p_offset, stock.ph[wl].p_offset);
  CHECK_EQ(patched.ph[wl].p_memsz, static_cast<std::uint64_t>(0x1d87c0));  // ADR 0021 section 4
  // Sections: every one unchanged except the NOBITS one that ended the segment, which ends at the new end.
  std::size_t grown = 0;
  for (std::size_t i = 0; i < stock.sh.size(); ++i) {
    if (std::memcmp(&stock.sh[i], &patched.sh[i], sizeof(Elf64_Shdr)) == 0) continue;
    ++grown;
    CHECK(patched.sh[i].sh_type == SHT_NOBITS);
    CHECK_EQ(patched.sh[i].sh_addr, stock.sh[i].sh_addr);
    CHECK_EQ(patched.sh[i].sh_addr + patched.sh[i].sh_size, kSlotAreaVaddr + kSlotBytes * kRowCount);
  }
  CHECK_EQ(grown, static_cast<std::size_t>(1));
  // readelf agrees with the headers it printed.
  CHECK(patchedLoad.find(Hex(0x1d87c0).substr(0)) != std::string::npos);
  CHECK(stockLoad.find("0x1d8688") != std::string::npos);
  CHECK(!stockSec.empty() && !patchedSec.empty());
  // The six end symbols have the same values and sizes in both files.
  const std::string stockSyms = Readelf("--dyn-syms", g_stock, "stock_syms"), patchedSyms = Readelf("--dyn-syms", r.outFile, "patched_syms");
  CHECK(stockSyms == patchedSyms);
  for (const char* name : {"__end__", "__bss_end__", "_bss_end__", "_end", "_edata", "__bss_start"}) {
    CHECK(patchedSyms.find(std::string(" ") + name + "\n") != std::string::npos);
  }
}

TEST(TheToolRefusesItsOwnOutputAsAnAlreadyPatchedInput) {
  const Run1& r = First();
  const std::string out = g_scratch + "/again.so", report = g_scratch + "/again.json";
  std::remove(out.c_str());
  const int rc = Run({g_tool, "--stock", r.outFile, "--out", out, "--report", report}, g_scratch + "/again.stdout", g_scratch + "/again.stderr");
  CHECK_EQ(rc, 1);
  CHECK(ReadText(g_scratch + "/again.stderr").find("\"reason\":\"already_patched\"") != std::string::npos);
  CHECK(access(out.c_str(), F_OK) != 0);
}

// ADR 0021 test 5: the libr15.so a repack put into its signed APK is the tool's output for the store file, byte for byte,
// and its build-ID note is the one PatchedBuildId() computes. Only registered into the run when --signed-libr15 is given.
TEST(SignedApkLibr15IsThePatchersOutput) {
  const Run1& r = First();
  // A tool that failed leaves no (or an empty) output file, which cannot equal the signed file's bytes.
  CHECK(ReadAll(g_signed) == ReadAll(r.outFile));
  CHECK(Readelf("-n", g_signed, "signed_note").find("Build ID: " + PatchedBuildId()) != std::string::npos);
}

// The reference build's libr15 files keep two state words in the page slack (word A, 4 bytes at 0x37d2540, and word B, 8 bytes
// at 0x37d2560; eight sites form them, below). The tool must patch each and leave its code and those words clear of the slots.
TEST(ReferenceLibr15sArePatchedWithTheirStateWordsClearOfTheSlots) {
  struct Word { std::uint64_t vaddr, bytes; };
  const Word words[] = {{0x37d2540, 4}, {0x37d2560, 8}};
  const std::uint64_t sites[] = {0x1a848ac, 0x1a848b0, 0x1a84988, 0x1a84990, 0x1a84a18, 0x1a84a1c, 0x1a84a94, 0x1a84a98};
  for (std::size_t n = 0; n < g_refs.size(); ++n) {
    const std::string tag = "ref" + std::to_string(n);
    const std::vector<std::uint8_t> in = ReadAll(g_refs[n]);
    const std::string sha = nevr_quest::data::Sha256Hex(in.data(), in.size());
    bool listed = false;
    for (const char* known : kLibR15ReferenceSha256) listed = listed || sha == known;
    CHECK(listed);  // the file is one the table names
    const std::string out = g_scratch + "/" + tag + ".so", report = g_scratch + "/" + tag + ".json";
    const std::string so = g_scratch + "/" + tag + ".stdout", se = g_scratch + "/" + tag + ".stderr";
    std::remove(out.c_str());
    CHECK_EQ(Run({g_tool, "--stock", g_refs[n], "--out", out, "--report", report}, so, se), 0);
    CHECK(ReadText(se).empty());
    Image before, after;
    if (!before.Load(g_refs[n]) || !after.Load(out)) {
      CHECK(false);
      return;
    }
    for (const std::uint64_t s : sites) CHECK_EQ(before.Word(s), after.Word(s));  // the reference's own code is untouched
    const std::size_t wl = after.WritableLoad();
    CHECK(wl < after.ph.size());
    if (wl >= after.ph.size()) return;
    const Elf64_Phdr& seg = after.ph[wl];
    CHECK_EQ(seg.p_vaddr + seg.p_memsz, kSlotAreaVaddr + kSlotBytes * kRowCount);
    for (const Word& w : words) {
      CHECK(w.vaddr + w.bytes <= kSlotAreaVaddr);                        // below the first slot
      CHECK(w.vaddr >= seg.p_vaddr + seg.p_filesz);                      // in the bss part
      CHECK(w.vaddr + w.bytes <= seg.p_vaddr + seg.p_memsz);             // inside the grown segment
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: libr15_patcher_pinned_test <quest_patch_libr15> <stock libr15.so> <scratch dir> [--signed-libr15=<file>] "
                 "[--reference-libr15=<file>]... [name-filter]\n");
    return 2;
  }
  g_tool = argv[1];
  g_stock = argv[2];
  g_scratch = argv[3];
  // The arguments after the scratch dir: the optional --signed-libr15=<file> and --reference-libr15=<file> flags, then the
  // optional name filter. RunAll reads its filter from its argv[1], so argv[3] stays as its program name and the flags are taken out.
  std::vector<char*> rest = {argv[3]};
  for (int i = 4; i < argc; ++i) {
    if (std::strncmp(argv[i], kSignedFlag, std::strlen(kSignedFlag)) == 0) {
      g_signed = argv[i] + std::strlen(kSignedFlag);
    } else if (std::strncmp(argv[i], kRefFlag, std::strlen(kRefFlag)) == 0) {
      g_refs.push_back(argv[i] + std::strlen(kRefFlag));
    } else {
      rest.push_back(argv[i]);
    }
  }
  if (g_refs.empty()) {
    // The test needs the reference build's files; without them it is dropped from the registry, and the run names it.
    auto& registry = mini_test::Registry();
    for (auto it = registry.begin(); it != registry.end(); ++it) {
      if (std::strcmp(it->name, kRefTest) == 0) {
        registry.erase(it);
        break;
      }
    }
    std::printf("SKIP %s (no %s<file> given: the reference build's libr15 files were not patched)\n", kRefTest, kRefFlag);
    std::fflush(stdout);
  }
  if (g_signed.empty()) {
    // The test needs a file; without one it is dropped from the registry, and the run names it.
    auto& registry = mini_test::Registry();
    for (auto it = registry.begin(); it != registry.end(); ++it) {
      if (std::strcmp(it->name, kSignedTest) == 0) {
        registry.erase(it);
        break;
      }
    }
    std::printf("SKIP %s (no %s<file> given: the signed APK's libr15.so was not checked)\n", kSignedTest, kSignedFlag);
    std::fflush(stdout);
  }
  return mini_test::RunAll(static_cast<int>(rest.size()), rest.data());
}
