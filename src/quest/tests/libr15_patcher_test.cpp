// Host test for the libr15 build-time patcher (src/quest/patch/libr15_patcher.h), ADR 0021 tests 2 and 9, on a
// synthetic stock ELF built in memory (synthetic_elf.h): the byte diff, determinism, every refusal with the row's
// id, the word patches and the extra-rows file (format 1) with one case per rule, and the command line.
//
// Run: libr15_patcher_test <scratch dir> <fixtures dir> <quest_patch_libr15 binary> [name-filter]
// The scratch directory is the test's own (files are written there and removed by the recipe); the binary is the
// shipped tool, run against the synthetic file (it must refuse: the synthetic file is not the pinned libr15).

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "mini_test.h"
#include "quest/data/sha256.h"
#include "quest/patch/libr15_patcher.h"
#include "quest/patch/stub_codec.h"
#include "synthetic_elf.h"

namespace {

using namespace nevr_quest::patch;
using namespace nevr_quest::patch::testing;
using Json = nlohmann::json;

std::string g_scratch;
std::string g_fixtures;
std::string g_tool;

BuildIdBytes StockId() {
  BuildIdBytes id{};
  CHECK(ParseBuildIdHex(kLibR15StockBuildId, &id));
  return id;
}

constexpr std::uint64_t kJoinVa = 0x1400200;
constexpr std::uint64_t kFailVa = 0x1400300;
constexpr std::uint64_t kAdrp0 = 0x1400400, kAdd0 = 0x1400408, kAdrp1 = 0x1400500, kAdd1 = 0x1400504;
constexpr std::uint64_t kBlob = 0x13ff800;  // read-only, on another page than the slot area (so the ADRPs change)
constexpr std::uint64_t kGuardA = 0x1400010, kGuardB = 0x1400018;  // the fixture's two words (revault_va 0x1500010)
constexpr std::uint64_t kSlotArea = 0x1403600;                     // roundup(0x14035f8, 64)

std::uint32_t AddImm(unsigned rd, unsigned rn, unsigned imm12) { return 0x91000000u | (imm12 << 10) | (rn << 5) | rd; }

// A synthetic libr15 and the table that describes it: two inline hooks and the credits pairs.
struct World {
  SynthElf elf;
  std::vector<Row> rows;
  PatchSpec spec;

  explicit World(const SynthOptions& options = DefaultOptions()) {
    elf = BuildSyntheticElf(options);
    const std::uint8_t guardA[4] = {0xe0, 0x03, 0x1f, 0xaa};
    const std::uint8_t guardB[4] = {0x00, 0x01, 0x00, 0x54};
    elf.PutBytes(kGuardA, guardA);
    elf.PutBytes(kGuardB, guardB);
    std::uint32_t adrp0 = 0, adrp1 = 0;
    EncodeAdrp(kAdrp0, kBlob, 8, &adrp0);
    EncodeAdrp(kAdrp1, kBlob, 12, &adrp1);
    elf.PutWord(kAdrp0, adrp0);
    elf.PutWord(kAdd0, AddImm(8, 8, kBlob & 0xFFF));
    elf.PutWord(kAdrp1, adrp1);
    elf.PutWord(kAdd1, AddImm(12, 12, kBlob & 0xFFF));
    rows.push_back(Inline("s.join", kJoinVa, 0));
    rows.push_back(Inline("s.fail", kFailVa, 1));
    Row credits{"s.credits", "Credits", RowKind::kCreditsPair, "install", 2, 4, {}};
    const std::uint64_t at[4] = {kAdrp0, kAdd0, kAdrp1, kAdd1};
    for (int i = 0; i < 4; ++i) credits.sites[i] = {at[i] + kRevaultBias, 1, {elf.WordAt(at[i])}};
    rows.push_back(credits);
    spec.rows = rows.data();
    spec.rowCount = rows.size();
    spec.stockBuildId = StockId();
    spec.expectedImageEnd = options.imageEnd;
  }

  static SynthOptions DefaultOptions() {
    SynthOptions o;
    o.buildId = StockId();
    return o;
  }

  Row Inline(const char* id, std::uint64_t vaddr, std::uint8_t slot) const {
    Row r{id, id, RowKind::kInlineHook, "handler", slot, 1, {}};
    r.sites[0] = {vaddr + kRevaultBias, 3, {elf.WordAt(vaddr), elf.WordAt(vaddr + 4), elf.WordAt(vaddr + 8)}};
    return r;
  }

  void Rebind() {
    spec.rows = rows.data();
    spec.rowCount = rows.size();
  }

  PatchResult Patch(const std::vector<WordPatchRow>& extra = {}) const { return PatchLibr15(elf.bytes, spec, extra, ""); }
};

// The byte ranges in which two equal-sized buffers differ, adjacent bytes merged.
std::vector<std::pair<std::uint64_t, std::uint64_t>> DiffRanges(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
  for (std::uint64_t i = 0; i < a.size(); ++i) {
    if (a[i] == b[i]) continue;
    if (!out.empty() && out.back().second == i) {
      out.back().second = i + 1;
    } else {
      out.emplace_back(i, i + 1);
    }
  }
  return out;
}

std::string ReadText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string Fixture() { return ReadText(g_fixtures + "/libr15_extra_rows_example.json"); }

bool Replace(std::string* text, const std::string& from, const std::string& to) {
  const std::size_t at = text->find(from);
  if (at == std::string::npos || text->find(from, at + 1) != std::string::npos) return false;
  text->replace(at, from.size(), to);
  return true;
}

// ---- ADR test 2: the patcher on the synthetic stock ELF -----------------------------------------------------------

TEST(TheOutputDiffersFromTheInputOnlyInTheNamedRanges) {
  World w;
  const PatchResult r = w.Patch();
  CHECK(r.ok);
  if (!r.ok) {
    std::fprintf(stderr, "  %s\n", r.summary.dump().c_str());
    return;
  }
  CHECK_EQ(r.output.size(), w.elf.bytes.size());
  // The windows the patch is allowed to write, from the synthetic layout and the ADR, not from the patcher.
  struct Window { std::uint64_t offset, length; };
  const Window windows[] = {
      {SynthElf::FileOffsetOfText(kJoinVa), 12}, {SynthElf::FileOffsetOfText(kFailVa), 12},
      {SynthElf::FileOffsetOfText(kAdrp0), 4},   {SynthElf::FileOffsetOfText(kAdd0), 4},
      {SynthElf::FileOffsetOfText(kAdrp1), 4},   {SynthElf::FileOffsetOfText(kAdd1), 4},
      {w.elf.PhdrMemszOffset(), 8},              {w.elf.BssSizeOffset(), 8},
      {0x210, 20},  // the build-ID descriptor: header 12 + "GNU\0" 4
  };
  const auto diff = DiffRanges(w.elf.bytes, r.output);
  for (const auto& d : diff) {
    bool inside = false;
    for (const Window& win : windows) inside = inside || (d.first >= win.offset && d.second <= win.offset + win.length);
    if (!inside) std::fprintf(stderr, "  differing range %llx..%llx is in no window\n", static_cast<unsigned long long>(d.first), static_cast<unsigned long long>(d.second));
    CHECK(inside);
  }
  for (const Window& win : windows) {
    bool touched = false;
    for (const auto& d : diff) touched = touched || (d.first < win.offset + win.length && win.offset < d.second);
    if (!touched) std::fprintf(stderr, "  window %llx+%llu was not written\n", static_cast<unsigned long long>(win.offset), static_cast<unsigned long long>(win.length));
    CHECK(touched);
  }
  // The report accounts for the same windows.
  const Json& changes = r.summary["changes"];
  CHECK_EQ(changes.size(), sizeof(windows) / sizeof(windows[0]));
  std::size_t matched = 0;
  for (const Json& c : changes) {
    for (const Window& win : windows) {
      char buf[24];
      std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(win.offset));
      if (c["offset"] == buf && c["length"] == win.length) ++matched;
    }
  }
  CHECK_EQ(matched, changes.size());
  CHECK_EQ(r.summary["bytes_changed"].get<std::uint64_t>(), [&] { std::uint64_t n = 0; for (const auto& d : diff) n += d.second - d.first; return n; }());

  // What the named ranges hold.
  Stub stub{};
  CHECK(EncodeSlotStub(kJoinVa, kSlotArea, &stub));
  for (std::size_t i = 0; i < 3; ++i) { std::uint32_t got; std::memcpy(&got, r.output.data() + SynthElf::FileOffsetOfText(kJoinVa) + 4 * i, 4); CHECK_EQ(got, stub.words[i]); }
  CHECK(EncodeSlotStub(kFailVa, kSlotArea + 8, &stub));
  for (std::size_t i = 0; i < 3; ++i) { std::uint32_t got; std::memcpy(&got, r.output.data() + SynthElf::FileOffsetOfText(kFailVa) + 4 * i, 4); CHECK_EQ(got, stub.words[i]); }
  std::uint32_t adrp = 0, ldr = 0;
  CHECK(EncodeAdrp(kAdrp0, kSlotArea + 16, 8, &adrp));
  CHECK(EncodeLdrX(8, 8, (kSlotArea + 16) & 0xFFF, &ldr));
  std::uint32_t got;
  std::memcpy(&got, r.output.data() + SynthElf::FileOffsetOfText(kAdrp0), 4);
  CHECK_EQ(got, adrp);
  std::memcpy(&got, r.output.data() + SynthElf::FileOffsetOfText(kAdd0), 4);
  CHECK_EQ(got, ldr);
  CHECK(EncodeAdrp(kAdrp1, kSlotArea + 16, 12, &adrp));
  CHECK(EncodeLdrX(12, 12, (kSlotArea + 16) & 0xFFF, &ldr));
  std::memcpy(&got, r.output.data() + SynthElf::FileOffsetOfText(kAdrp1), 4);
  CHECK_EQ(got, adrp);
  std::memcpy(&got, r.output.data() + SynthElf::FileOffsetOfText(kAdd1), 4);
  CHECK_EQ(got, ldr);
  std::uint64_t memsz = 0, bss = 0;
  std::memcpy(&memsz, r.output.data() + w.elf.PhdrMemszOffset(), 8);
  std::memcpy(&bss, r.output.data() + w.elf.BssSizeOffset(), 8);
  CHECK_EQ(memsz, w.elf.imageEnd - kDataVaddr + (kSlotArea + 24 - w.elf.imageEnd));  // grown to the slot area's end
  CHECK_EQ(bss, 0x14035f8 - 0x1403200 + (kSlotArea + 24 - 0x14035f8));
  BuildIdBytes id{};
  std::memcpy(id.data(), r.output.data() + 0x210, 20);
  CHECK(id == DeriveBuildId(StockId(), CanonicalTable(w.spec.rows, w.spec.rowCount, {}, kSlotArea)));
  CHECK(id != StockId());
  // The six end symbols keep their stock values.
  CHECK_EQ(r.summary["end_symbols"].size(), static_cast<std::size_t>(6));
  for (const Json& s : r.summary["end_symbols"]) {
    const std::string n = s["name"];
    CHECK(s["value"] == ((n == "_edata" || n == "__bss_start") ? "0x1403200" : "0x14035f8"));
  }
  // One site event per site, with the ADR's fields.
  std::size_t sites = 0;
  for (const Json& e : r.events) {
    if (e["event"] != "libr15_patch_site") continue;
    ++sites;
    CHECK(e.contains("id") && e.contains("revault_va") && e.contains("vaddr") && e.contains("stock") && e.contains("stub") && e.contains("slot"));
  }
  CHECK_EQ(sites, static_cast<std::size_t>(6));
  CHECK(r.summary["status"] == "patched");
  CHECK_EQ(r.summary["rows"].get<int>(), 3);
  CHECK(r.summary["patched_sha256"] != r.summary["stock_sha256"]);
}

TEST(TheSameInputsGiveTheSameBytesAndTheSameReport) {
  World w;
  const PatchResult a = w.Patch();
  const PatchResult b = w.Patch();
  CHECK(a.ok && b.ok);
  CHECK(a.output == b.output);
  CHECK_EQ(a.summary.dump(), b.summary.dump());
  CHECK_EQ(Json(a.events).dump(), Json(b.events).dump());
}

struct Refused {
  const char* code;
  const char* id;
};

void ExpectRefusal(const PatchResult& r, const char* code, const char* id, const char* what) {
  if (r.ok || r.refusal.code != code || r.refusal.id != id) {
    std::fprintf(stderr, "  %s: expected %s/%s, got ok=%d %s/%s (%s)\n", what, code, id, r.ok ? 1 : 0, r.refusal.code.c_str(), r.refusal.id.c_str(), r.refusal.detail.c_str());
    ++mini_test::Failures();
  }
  CHECK(r.output.empty());
  CHECK(r.summary["status"] == "refused");
}

TEST(AWrongStockWordIsRefusedWithTheRowId) {
  World w;
  w.rows[1].sites[0].words[1] ^= 4;
  ExpectRefusal(w.Patch(), "row_words_mismatch", "s.fail", "wrong stock word");
  World c;
  c.rows[2].sites[3].words[0] ^= 1;  // a credits word
  ExpectRefusal(c.Patch(), "row_words_mismatch", "s.credits", "wrong credits word");
}

TEST(AWrongBuildIdIsRefused) {
  World w;
  w.elf.bytes[0x210 + 5] ^= 1;
  ExpectRefusal(w.Patch(), "stock_build_id_mismatch", "", "wrong build id");
}

TEST(AnAlreadyPatchedInputIsRefusedAsSuch) {
  World w;
  const PatchResult first = w.Patch();
  CHECK(first.ok);
  const PatchResult second = PatchLibr15(first.output, w.spec, {}, "");
  ExpectRefusal(second, "already_patched", "", "already patched");
}

TEST(ASiteOutsideTheTextIsRefusedWithTheRowId) {
  World inData;
  inData.rows[1].sites[0].revaultVa = kDataVaddr + 0x40 + kRevaultBias;  // .data: not executable
  ExpectRefusal(inData.Patch(), "site_outside_text", "s.fail", "site in data");
  World unmapped;
  unmapped.rows[0].sites[0].revaultVa = 0x9000000 + kRevaultBias;
  ExpectRefusal(unmapped.Patch(), "site_outside_text", "s.join", "site unmapped");
  World straddle;  // the last word of the text and the next two are not
  straddle.rows[0].sites[0].revaultVa = (kTextEnd - 4) + kRevaultBias;
  ExpectRefusal(straddle.Patch(), "site_outside_text", "s.join", "site straddles the end");
}

TEST(AGrowthThatWouldOverlapTheNextSegmentIsRefused) {
  SynthOptions o = World::DefaultOptions();
  o.imageEnd = 0x1403ff8;  // the slot area starts on the next page ...
  o.trailingLoadVaddr = 0x1404000;  // ... where another PT_LOAD is
  World blocked(o);
  ExpectRefusal(blocked.Patch(), "layout_overlap", "", "growth overlaps");
  SynthOptions sharedPage = o;  // the next segment starts past the grown end but inside the last page it maps
  sharedPage.imageEnd = 0x1403f00;
  sharedPage.trailingLoadVaddr = 0x1403f80;
  World shares(sharedPage);
  ExpectRefusal(shares.Patch(), "layout_overlap", "", "shares the last page");
  SynthOptions clear = o;
  clear.trailingLoadVaddr = 0x1405000;  // one page further: room
  World fine(clear);
  CHECK(fine.Patch().ok);
  SynthOptions none = o;
  none.trailingLoadVaddr.reset();
  World alone(none);
  CHECK(alone.Patch().ok);
}

TEST(TheSlotAreaMustBeOutsideGnuRelro) {
  SynthOptions o = World::DefaultOptions();
  o.relroEnd = 0x1403700;
  World w(o);
  ExpectRefusal(w.Patch(), "layout_relro", "", "relro");
  SynthOptions fine = World::DefaultOptions();
  fine.relroEnd = 0x1403200;
  World ok(fine);
  CHECK(ok.Patch().ok);
}

TEST(TheFilesDigestAndImageEndArePinnedWhenTheSpecSaysSo) {
  World w;
  w.spec.stockSha256 = std::string(64, '0');
  ExpectRefusal(w.Patch(), "stock_sha256_mismatch", "", "wrong sha");
  w.spec.stockSha256 = nevr_quest::data::Sha256Hex(w.elf.bytes.data(), w.elf.bytes.size());
  CHECK(w.Patch().ok);
  w.spec.expectedImageEnd = 0x14035f0;
  ExpectRefusal(w.Patch(), "layout_image_end", "", "wrong image end");
}

TEST(RowsThatShareBytesAreRefusedNamingTheSecondRow) {
  World w;
  w.rows[1] = w.Inline("s.fail", kJoinVa + 8, 1);  // overlaps s.join's last word
  ExpectRefusal(w.Patch(), "sites_overlap", "s.fail", "rows overlap");
}

TEST(AnAddressBelowTheBiasIsRefusedAsSuchNotAsAnUnmappedOne) {
  World w;
  w.rows[0].sites[0].revaultVa = 0x1000;
  const PatchResult r = w.Patch();
  ExpectRefusal(r, "site_outside_text", "s.join", "site below the bias");
  CHECK(r.refusal.detail.find("below the bias") != std::string::npos);
}

TEST(AWindowTheRelocatorCannotDisplaceIsRefusedWithTheRowId) {
  World w;
  w.elf.PutWord(kJoinVa, 0xb4000040u);  // cbz x0, +8: lands inside the 12 bytes the stub overwrites
  w.rows[0] = w.Inline("s.join", kJoinVa, 0);
  ExpectRefusal(w.Patch(), "row_not_relocatable", "s.join", "cbz into the window");
}

TEST(ACreditsPairThatIsNotAnAdrpAndAnAddIsRefused) {
  World w;
  w.rows[2].sites[1].words[0] = AddImm(9, 8, 0x40);  // add x9, x8, #0x40: another destination
  w.elf.PutWord(kAdd0, w.rows[2].sites[1].words[0]);
  ExpectRefusal(w.Patch(), "credits_shape", "s.credits", "credits shape");
  World source;  // add x8, x9, #imm: another source register
  source.rows[2].sites[1].words[0] = AddImm(8, 9, kBlob & 0xFFF);
  source.elf.PutWord(kAdd0, source.rows[2].sites[1].words[0]);
  ExpectRefusal(source.Patch(), "credits_shape", "s.credits", "credits source register");
  World shifted;  // add x8, x8, #imm, lsl #12: another opcode
  shifted.rows[2].sites[1].words[0] = AddImm(8, 8, kBlob & 0xFFF) | (1u << 22);
  shifted.elf.PutWord(kAdd0, shifted.rows[2].sites[1].words[0]);
  ExpectRefusal(shifted.Patch(), "credits_shape", "s.credits", "credits shifted add");
  World notAdrp;  // the first word of a pair is not an ADRP
  notAdrp.rows[2].sites[0].words[0] = AddImm(8, 8, 0);
  notAdrp.elf.PutWord(kAdrp0, notAdrp.rows[2].sites[0].words[0]);
  const PatchResult notAdrpResult = notAdrp.Patch();
  ExpectRefusal(notAdrpResult, "credits_shape", "s.credits", "credits first word");
  CHECK(notAdrpResult.refusal.detail.find("not an ADRP") != std::string::npos);
  World b;
  b.rows[2].sites[3].words[0] = AddImm(12, 12, 0x44);  // the second pair loads another address
  b.elf.PutWord(kAdd1, b.rows[2].sites[3].words[0]);
  ExpectRefusal(b.Patch(), "credits_blob_mismatch", "s.credits", "credits blob");
}

TEST(AnInputThatIsNotTheExpectedElfIsRefused) {
  World w;
  std::vector<std::uint8_t> shortFile(w.elf.bytes.begin(), w.elf.bytes.begin() + 100);
  ExpectRefusal(PatchLibr15(shortFile, w.spec, {}, ""), "elf_invalid", "", "truncated");
  std::vector<std::uint8_t> notArm = w.elf.bytes;
  notArm[0x12] = 0x3e;  // EM_X86_64
  ExpectRefusal(PatchLibr15(notArm, w.spec, {}, ""), "elf_invalid", "", "wrong machine");
  std::vector<std::uint8_t> unwritable = w.elf.bytes;
  const std::uint64_t flagsAt = sizeof(Elf64_Ehdr) + 1 * sizeof(Elf64_Phdr) + offsetof(Elf64_Phdr, p_flags);
  unwritable[flagsAt] = PF_R;
  ExpectRefusal(PatchLibr15(unwritable, w.spec, {}, ""), "layout_not_writable", "", "no writable segment");
  std::vector<std::uint8_t> noBss = w.elf.bytes;
  noBss[w.elf.BssSizeOffset()] ^= 0x40;  // .bss no longer ends at the segment's end
  ExpectRefusal(PatchLibr15(noBss, w.spec, {}, ""), "layout_bss_section", "", "no bss section");
}

TEST(ATableOrAFileTheCodeCannotWriteIsRefusedNotGuessedAt) {
  // A slot area more than 4 GiB from a site: the ADRP cannot reach it.
  SynthOptions far = World::DefaultOptions();
  far.imageEnd = 0x140000000;
  World farWorld(far);
  ExpectRefusal(farWorld.Patch(), "stub_out_of_range", "s.join", "slot out of ADRP range");
  World farCredits(far);  // only the credits row: its LDR cannot reach the slot either
  farCredits.rows.erase(farCredits.rows.begin(), farCredits.rows.begin() + 2);
  farCredits.rows[0].slot = 0;
  farCredits.Rebind();
  ExpectRefusal(farCredits.Patch(), "stub_out_of_range", "s.credits", "credits slot out of range");
  // A writable segment whose memory ends inside its file-backed part is not a valid ELF to patch.
  SynthOptions small = World::DefaultOptions();
  small.imageEnd = 0x1403100;
  World smallWorld(small);
  ExpectRefusal(smallWorld.Patch(), "elf_invalid", "", "memsz below filesz");
  // A public row of the word_patch kind: the table (not a file) may not hold one.
  World kind;
  kind.rows[0].kind = RowKind::kWordPatch;
  ExpectRefusal(kind.Patch(), "row_kind", "s.join", "public word_patch row");
  // Word edits that did not come through the file reader: the patcher checks them again.
  World w;
  WordPatchRow edit;
  edit.id = "x.direct";
  edit.revaultVa = kGuardA + kRevaultBias;
  edit.edits.push_back({2, {0xe0, 0x03, 0x1f, 0xaa}, {0x1f, 0x20, 0x03, 0xd5}});
  ExpectRefusal(w.Patch({edit}), "word_offset", "x.direct", "offset not a multiple of 4");
  edit.edits[0].offset = 0;
  edit.revaultVa = 0x1000;
  const PatchResult below = w.Patch({edit});
  ExpectRefusal(below, "word_outside_text", "x.direct", "address below the bias");
  CHECK(below.refusal.detail.find("below the bias") != std::string::npos);
  edit.revaultVa = kGuardA + kRevaultBias;
  CHECK(w.Patch({edit}).ok);
}

// ---- ADR test 9: word patches and the extra-rows file (format 1) ---------------------------------------------------

bool Parse(const std::string& text, const World& w, std::vector<WordPatchRow>* rows, Refusal* refusal) {
  return ParseExtraRows(text, w.spec, rows, refusal);
}

TEST(TheFixtureLoadsAndItsWordsAreBytesInMemoryOrder) {
  World w;
  std::vector<WordPatchRow> rows;
  Refusal refusal;
  CHECK(Parse(Fixture(), w, &rows, &refusal));
  CHECK_EQ(rows.size(), static_cast<std::size_t>(1));
  if (rows.size() != 1) return;
  CHECK_EQ(rows[0].id, std::string("x.example_guard"));
  CHECK_EQ(rows[0].revaultVa, static_cast<std::uint64_t>(0x1500010));
  CHECK_EQ(rows[0].edits.size(), static_cast<std::size_t>(2));
  CHECK_EQ(rows[0].edits[1].offset, 8u);
  // "1f2003d5" is the NOP 0xd503201f; "e0031faa" is `mov x0, xzr`, 0xaa1f03e0.
  CHECK_EQ(WordValue(rows[0].edits[0].word), 0xd503201fu);
  CHECK_EQ(WordValue(rows[0].edits[0].stock), 0xaa1f03e0u);
  CHECK_EQ(WordValue(rows[0].edits[1].stock), 0x54000100u);
}

TEST(AFileOfExtraRowsChangesTheIdAndOnlyTheNamedWords) {
  World w;
  std::vector<WordPatchRow> rows;
  Refusal refusal;
  CHECK(Parse(Fixture(), w, &rows, &refusal));
  const PatchResult plain = w.Patch();
  const PatchResult withRows = w.Patch(rows);
  CHECK(plain.ok && withRows.ok);
  if (!withRows.ok) {
    std::fprintf(stderr, "  %s\n", withRows.summary.dump().c_str());
    return;
  }
  CHECK(plain.summary["build_id"] != withRows.summary["build_id"]);
  // The output differs from the public output in the two named words and the build ID, nowhere else.
  const auto diff = DiffRanges(plain.output, withRows.output);
  std::size_t words = 0;
  for (const auto& d : diff) {
    const bool guard = (d.first == SynthElf::FileOffsetOfText(kGuardA) && d.second == d.first + 4) ||
                       (d.first == SynthElf::FileOffsetOfText(kGuardB) && d.second == d.first + 4);
    const bool id = d.first >= 0x210 && d.second <= 0x210 + 20;
    CHECK(guard || id);
    if (guard) ++words;
  }
  CHECK_EQ(words, static_cast<std::size_t>(2));
  std::uint32_t at = 0;
  std::memcpy(&at, withRows.output.data() + SynthElf::FileOffsetOfText(kGuardA), 4);
  CHECK_EQ(at, 0xd503201fu);
  std::memcpy(&at, withRows.output.data() + SynthElf::FileOffsetOfText(kGuardB), 4);
  CHECK_EQ(at, 0xd503201fu);
  // The same file twice gives the same bytes.
  const PatchResult again = w.Patch(rows);
  CHECK(again.output == withRows.output);
  // The build ID is the public one plus the rows, and the summary carries a count of the rows.
  CHECK(withRows.summary["build_id"] == BuildIdHex(DeriveBuildId(StockId(), CanonicalTable(w.spec.rows, w.spec.rowCount, rows, kSlotArea))));
  CHECK_EQ(withRows.summary["extra_rows"].get<int>(), 1);
  // The public table's ID is the one a build without the file computes.
  CHECK(plain.summary["build_id"] == BuildIdHex(DeriveBuildId(StockId(), CanonicalTable(w.spec.rows, w.spec.rowCount, {}, kSlotArea))));
}

TEST(TheReportPrintsEachWordInBothForms) {
  World w;
  std::vector<WordPatchRow> rows;
  Refusal refusal;
  CHECK(Parse(Fixture(), w, &rows, &refusal));
  const PatchResult r = w.Patch(rows);
  CHECK(r.ok);
  std::vector<Json> words;
  for (const Json& e : r.events) {
    if (e["event"] == "libr15_patch_word") words.push_back(e);
  }
  CHECK_EQ(words.size(), static_cast<std::size_t>(2));
  if (words.size() != 2) return;
  CHECK(words[0]["id"] == "x.example_guard");
  CHECK(words[0]["offset"] == 0);
  CHECK(words[0]["vaddr"] == "0x1400010");
  CHECK(words[0]["stock_bytes"] == "e0031faa");
  CHECK(words[0]["stock_word"] == "0xaa1f03e0");
  CHECK(words[0]["new_bytes"] == "1f2003d5");
  CHECK(words[0]["new_word"] == "0xd503201f");
  CHECK(words[1]["offset"] == 8);
  CHECK(words[1]["stock_bytes"] == "00010054");
  CHECK(words[1]["stock_word"] == "0x54000100");
}

TEST(TheBuildIdDoesNotDependOnKeyOrderSpacingOrRowOrder) {
  World w;
  const std::uint8_t secondStock[4] = {0, 0, 0, 0};
  (void)secondStock;
  // Two rows, written two ways: different key order, spacing and row order.
  const std::string secondStockHex = [&] { char b[16]; const std::uint32_t v = w.elf.WordAt(0x1400020); std::snprintf(b, sizeof b, "%02x%02x%02x%02x", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24); return std::string(b); }();
  const std::string first = R"({"format":"nevr-libr15-extra-rows","version":1,"stock_build_id":"b243509c08ce677aeb95fa348016949b3fc45230","rows":[
    {"id":"x.a","kind":"word_patch","revault_va":"0x1500010","words":[{"offset":0,"stock_bytes":"e0031faa","new_bytes":"1f2003d5"}]},
    {"id":"x.b","kind":"word_patch","revault_va":"0x1500020","words":[{"offset":0,"stock_bytes":")" + secondStockHex + R"(","new_bytes":"1f2003d5"}]}]})";
  const std::string second = R"({
      "rows" : [
         { "words" : [ { "new_bytes" : "1f2003d5", "stock_bytes" : ")" + secondStockHex + R"(", "offset" : 0 } ],
           "revault_va" : "0x1500020", "kind" : "word_patch", "id" : "x.b" },
         { "words" : [ { "new_bytes" : "1f2003d5", "offset" : 0, "stock_bytes" : "e0031faa" } ],
           "revault_va" : "0x1500010", "id" : "x.a", "kind" : "word_patch" } ],
      "stock_build_id" : "b243509c08ce677aeb95fa348016949b3fc45230", "version" : 1, "format" : "nevr-libr15-extra-rows" })";
  std::vector<WordPatchRow> a, b;
  Refusal refusal;
  CHECK(Parse(first, w, &a, &refusal));
  CHECK(Parse(second, w, &b, &refusal));
  const PatchResult ra = w.Patch(a);
  const PatchResult rb = w.Patch(b);
  CHECK(ra.ok && rb.ok);
  CHECK(ra.output == rb.output);
  CHECK(ra.summary["build_id"] == rb.summary["build_id"]);
  CHECK(Json(ra.events).dump() == Json(rb.events).dump());
}

struct RuleCase {
  const char* rule;      // the rule of ADR section 11 the case breaks
  const char* from;      // a substring of the fixture (exactly one occurrence), or "" to use `whole`
  const char* to;
  const char* code;      // the refusal code
  const char* id;        // the row id the refusal names ("" for the file)
};

TEST(EachRuleOfTheExtraRowsFormatIsRefusedInItsOwnCase) {
  const RuleCase cases[] = {
      {"format", "\"format\": \"nevr-libr15-extra-rows\"", "\"format\": \"nevr-libr15-extra-rows-2\"", "extra_rows_format", ""},
      {"format is a string", "\"format\": \"nevr-libr15-extra-rows\"", "\"format\": 7", "extra_rows_format", ""},
      {"version number", "\"version\": 1,", "\"version\": 2,", "extra_rows_version", ""},
      {"version is an integer", "\"version\": 1,", "\"version\": 1.0,", "extra_rows_version", ""},
      {"version is not a string", "\"version\": 1,", "\"version\": \"1\",", "extra_rows_version", ""},
      {"stock build id differs", "b243509c08ce677aeb95fa348016949b3fc45230", "b243509c08ce677aeb95fa348016949b3fc45231", "extra_rows_stock_build_id", ""},
      {"stock build id upper case", "b243509c08ce677aeb95fa348016949b3fc45230", "B243509C08CE677AEB95FA348016949B3FC45230", "extra_rows_stock_build_id", ""},
      {"stock build id length", "b243509c08ce677aeb95fa348016949b3fc45230", "b243509c08ce677aeb95fa348016949b3fc4523", "extra_rows_stock_build_id", ""},
      {"unknown field, file", "\"version\": 1,", "\"version\": 1, \"surprise\": true,", "extra_rows_unknown_field", ""},
      {"unknown field, row", "\"kind\": \"word_patch\",", "\"kind\": \"word_patch\", \"note\": \"x\",", "extra_rows_unknown_field", "x.example_guard"},
      {"unknown field, word", "{ \"offset\": 0,", "{ \"offset\": 0, \"flags\": 1,", "extra_rows_unknown_field", "x.example_guard"},
      {"missing field, file", "\"version\": 1,", "", "extra_rows_missing_field", ""},
      {"missing field, row", "\"kind\": \"word_patch\",", "", "extra_rows_missing_field", "x.example_guard"},
      {"missing field, word", "\"stock_bytes\": \"00010054\", ", "", "extra_rows_missing_field", "x.example_guard"},
      {"duplicate key, file", "\"version\": 1,", "\"version\": 1, \"version\": 1,", "extra_rows_duplicate_key", ""},
      {"duplicate key, word", "{ \"offset\": 0,", "{ \"offset\": 0, \"offset\": 0,", "extra_rows_duplicate_key", ""},
      {"id without prefix", "\"x.example_guard\"", "\"example_guard\"", "extra_rows_id", "example_guard"},
      {"id upper case", "\"x.example_guard\"", "\"x.Example\"", "extra_rows_id", "x.Example"},
      {"id empty suffix", "\"x.example_guard\"", "\"x.\"", "extra_rows_id", "x."},
      {"id 63 characters", "\"x.example_guard\"", "\"x.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"", "extra_rows_id",
       "x.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
      {"id with a space", "\"x.example_guard\"", "\"x.a b\"", "extra_rows_id", "x.a b"},
      {"kind", "\"kind\": \"word_patch\"", "\"kind\": \"kWordPatch\"", "extra_rows_kind", "x.example_guard"},
      {"kind inline hook", "\"kind\": \"word_patch\"", "\"kind\": \"inline_hook\"", "extra_rows_kind", "x.example_guard"},
      {"revault_va not a multiple of 4", "\"0x1500010\"", "\"0x1500012\"", "extra_rows_revault_va", "x.example_guard"},
      {"revault_va upper case", "\"0x1500010\"", "\"0x1500A10\"", "extra_rows_revault_va", "x.example_guard"},
      {"revault_va without 0x", "\"0x1500010\"", "\"1500010\"", "extra_rows_revault_va", "x.example_guard"},
      {"revault_va with a capital X", "\"0x1500010\"", "\"0X1500010\"", "extra_rows_revault_va", "x.example_guard"},
      {"revault_va with another prefix", "\"0x1500010\"", "\"zz1500010\"", "extra_rows_revault_va", "x.example_guard"},
      {"revault_va leading zero", "\"0x1500010\"", "\"0x01500010\"", "extra_rows_revault_va", "x.example_guard"},
      {"revault_va is a number", "\"0x1500010\"", "22020112", "extra_rows_revault_va", "x.example_guard"},
      {"offset not a multiple of 4", "\"offset\": 8", "\"offset\": 6", "extra_rows_offset", "x.example_guard"},
      {"offsets out of order", "\"offset\": 0, \"stock_bytes\": \"e0031faa\"", "\"offset\": 12, \"stock_bytes\": \"e0031faa\"", "extra_rows_offset", "x.example_guard"},
      {"offsets repeat", "\"offset\": 8", "\"offset\": 0", "extra_rows_offset", "x.example_guard"},
      {"offset past 4092", "\"offset\": 8", "\"offset\": 4096", "extra_rows_offset", "x.example_guard"},
      {"offset negative", "\"offset\": 8", "\"offset\": -4", "extra_rows_offset", "x.example_guard"},
      {"offset is a float", "\"offset\": 8", "\"offset\": 8.0", "extra_rows_offset", "x.example_guard"},
      {"stock bytes a number", "\"e0031faa\"", "5", "extra_rows_bytes", "x.example_guard"},
      {"new bytes a number", "\"new_bytes\": \"1f2003d5\" },\n        { \"offset\": 8", "\"new_bytes\": 5 },\n        { \"offset\": 8", "extra_rows_bytes", "x.example_guard"},
      {"a word that is not an object", "{ \"offset\": 8, \"stock_bytes\": \"00010054\", \"new_bytes\": \"1f2003d5\" }", "7", "extra_rows_json", "x.example_guard"},
      {"bytes with a prefix", "\"e0031faa\"", "\"0xe0031faa\"", "extra_rows_bytes", "x.example_guard"},
      {"bytes upper case", "\"e0031faa\"", "\"E0031FAA\"", "extra_rows_bytes", "x.example_guard"},
      {"bytes too short", "\"e0031faa\"", "\"e0031fa\"", "extra_rows_bytes", "x.example_guard"},
      {"bytes too long", "\"e0031faa\"", "\"e0031faa00\"", "extra_rows_bytes", "x.example_guard"},
      {"new bytes equal the stock bytes", "\"new_bytes\": \"1f2003d5\" },\n        { \"offset\": 8", "\"new_bytes\": \"e0031faa\" },\n        { \"offset\": 8", "extra_rows_unchanged", "x.example_guard"},
      {"a word as the little-endian value", "\"e0031faa\"", "\"aa1f03e0\"", "word_stock_mismatch", "x.example_guard"},
      {"a stock mismatch", "\"00010054\"", "\"00010055\"", "word_stock_mismatch", "x.example_guard"},
      {"a word below the bias", "\"0x1500010\"", "\"0x10\"", "word_outside_text", "x.example_guard"},
      {"a word in data", "\"0x1500010\"", "\"0x1503040\"", "word_outside_text", "x.example_guard"},
      {"a word past the text", "\"0x1500010\"", "\"0x1502000\"", "word_outside_text", "x.example_guard"},
      {"empty rows", "\"revault_va\": \"0x1500010\",", "\"revault_va\": \"0x1500010\",", "", ""},  // replaced below
  };
  for (const RuleCase& c : cases) {
    World w;
    std::string text = Fixture();
    if (std::string(c.rule) == "empty rows") {
      text = R"({"format":"nevr-libr15-extra-rows","version":1,"stock_build_id":"b243509c08ce677aeb95fa348016949b3fc45230","rows":[]})";
      std::vector<WordPatchRow> rows;
      Refusal refusal;
      CHECK(!Parse(text, w, &rows, &refusal));
      CHECK_EQ(refusal.code, std::string("extra_rows_missing_field"));
      continue;
    }
    if (!Replace(&text, c.from, c.to)) {
      std::fprintf(stderr, "  rule \"%s\": the fixture does not hold exactly one \"%s\"\n", c.rule, c.from);
      CHECK(false);
      continue;
    }
    std::vector<WordPatchRow> rows;
    Refusal refusal;
    const bool parsed = Parse(text, w, &rows, &refusal);
    std::string code = refusal.code;
    std::string id = refusal.id;
    if (parsed) {
      const PatchResult r = w.Patch(rows);
      if (r.ok) {
        code = "(accepted)";
        id.clear();
      } else {
        code = r.refusal.code;
        id = r.refusal.id;
      }
    }
    if (code != c.code || id != c.id) {
      std::fprintf(stderr, "  rule \"%s\": expected %s/%s, got %s/%s\n", c.rule, c.code, c.id, code.c_str(), id.c_str());
      ++mini_test::Failures();
    }
  }
}

TEST(TheRulesThatNeedMoreThanOneRowOrManyWords) {
  const std::string head = R"({"format":"nevr-libr15-extra-rows","version":1,"stock_build_id":"b243509c08ce677aeb95fa348016949b3fc45230","rows":)";
  auto word = [](unsigned offset) {
    return std::string("{\"offset\":") + std::to_string(offset) + ",\"stock_bytes\":\"00000000\",\"new_bytes\":\"1f2003d5\"}";
  };
  auto row = [&](const std::string& id, const std::string& va, unsigned words) {
    std::string w;
    for (unsigned i = 0; i < words; ++i) w += (i ? "," : "") + word(4 * i);
    return "{\"id\":\"" + id + "\",\"kind\":\"word_patch\",\"revault_va\":\"" + va + "\",\"words\":[" + w + "]}";
  };
  World w;
  std::vector<WordPatchRow> rows;
  Refusal refusal;
  // 64 words in a row are allowed, 65 are not, and 0 are not.
  CHECK(Parse(head + "[" + row("x.big", "0x1500100", 64) + "]}", w, &rows, &refusal));
  CHECK(!Parse(head + "[" + row("x.big", "0x1500100", 65) + "]}", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_words"));
  CHECK_EQ(refusal.id, std::string("x.big"));
  CHECK(!Parse(head + "[" + row("x.none", "0x1500100", 0) + "]}", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_words"));
  // 256 words in all are allowed, 257 are not.
  std::string four = row("x.r1", "0x1500100", 64) + "," + row("x.r2", "0x1500200", 64) + "," + row("x.r3", "0x1500300", 64) + "," + row("x.r4", "0x1500400", 64);
  CHECK(Parse(head + "[" + four + "]}", w, &rows, &refusal));
  CHECK(!Parse(head + "[" + four + "," + row("x.r5", "0x1500500", 1) + "]}", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_words"));
  CHECK_EQ(refusal.id, std::string("x.r5"));
  // The same id twice.
  CHECK(!Parse(head + "[" + row("x.same", "0x1500100", 1) + "," + row("x.same", "0x1500200", 1) + "]}", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_id"));
  CHECK_EQ(refusal.id, std::string("x.same"));
  // An id equal to a public row's.
  World pub;
  pub.rows[0].id = "x.public_row";
  CHECK(!Parse(head + "[" + row("x.public_row", "0x1500100", 1) + "]}", pub, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_id"));
  CHECK_EQ(refusal.id, std::string("x.public_row"));
  // A row that is not an object.
  CHECK(!Parse(head + "[7]}", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_json"));
  CHECK(!Parse(head + "[[]]}", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_json"));
  // A key repeated in a row AFTER the row held nested objects (the words): the scope of the repeat check is the
  // object being read, not the first one.
  const std::string repeated = head + "[{\"id\":\"x.a\",\"kind\":\"word_patch\",\"revault_va\":\"0x1500100\",\"words\":[" + word(0) + "],\"kind\":\"word_patch\"}]}";
  CHECK(!Parse(repeated, w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_duplicate_key"));
  // The same key in two different rows is fine.
  CHECK(Parse(head + "[" + row("x.a", "0x1500100", 1) + "," + row("x.b", "0x1500200", 1) + "]}", w, &rows, &refusal));
  // Ids: digits, dots and underscores are allowed; 62 characters after the prefix are the limit.
  CHECK(Parse(head + "[" + row("x.a.b_9.c", "0x1500100", 1) + "]}", w, &rows, &refusal));
  CHECK(Parse(head + "[" + row("x.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "0x1500100", 1) + "]}", w, &rows, &refusal));
  // Not JSON, trailing text, a top-level array.
  CHECK(!Parse("not json", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_json"));
  CHECK(!Parse(head + "[" + row("x.a", "0x1500100", 1) + "]} trailing", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_json"));
  CHECK(!Parse("[]", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_json"));
}

TEST(AWordInBytesAnotherRowWritesIsRefusedNamingTheSecondRow) {
  World w;
  // x.over_public sits on s.join's stub (the stock bytes are what the file holds there).
  auto bytesAt = [&](std::uint64_t va) { char b[16]; const std::uint32_t v = w.elf.WordAt(va); std::snprintf(b, sizeof b, "%02x%02x%02x%02x", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24); return std::string(b); };
  const std::string head = R"({"format":"nevr-libr15-extra-rows","version":1,"stock_build_id":"b243509c08ce677aeb95fa348016949b3fc45230","rows":[)";
  std::vector<WordPatchRow> rows;
  Refusal refusal;
  const std::string overPublic = head + R"({"id":"x.over_public","kind":"word_patch","revault_va":"0x1500200","words":[{"offset":4,"stock_bytes":")" + bytesAt(kJoinVa + 4) +
                                 R"(","new_bytes":"1f2003d5"}]}]})";
  CHECK(Parse(overPublic, w, &rows, &refusal));
  PatchResult r = w.Patch(rows);
  CHECK(!r.ok);
  CHECK_EQ(r.refusal.code, std::string("word_overlap"));
  CHECK_EQ(r.refusal.id, std::string("x.over_public"));
  CHECK(r.refusal.detail.find("s.join") != std::string::npos);
  // The credits pair's ADD word.
  const std::string overCredits = head + R"({"id":"x.over_credits","kind":"word_patch","revault_va":"0x1500408","words":[{"offset":0,"stock_bytes":")" + bytesAt(kAdd0) +
                                  R"(","new_bytes":"1f2003d5"}]}]})";
  CHECK(Parse(overCredits, w, &rows, &refusal));
  r = w.Patch(rows);
  CHECK_EQ(r.refusal.code, std::string("word_overlap"));
  CHECK(r.refusal.detail.find("s.credits") != std::string::npos);
  // Another extra row: x.second starts on the fixture's second word.
  const std::string overExtra = head + R"({"id":"x.first","kind":"word_patch","revault_va":"0x1500010","words":[{"offset":0,"stock_bytes":"e0031faa","new_bytes":"1f2003d5"},{"offset":8,"stock_bytes":"00010054","new_bytes":"1f2003d5"}]},)" +
                                R"({"id":"x.second","kind":"word_patch","revault_va":"0x1500018","words":[{"offset":0,"stock_bytes":"00010054","new_bytes":"1f2003d5"}]}]})";
  CHECK(Parse(overExtra, w, &rows, &refusal));
  r = w.Patch(rows);
  CHECK_EQ(r.refusal.code, std::string("word_overlap"));
  CHECK_EQ(r.refusal.id, std::string("x.second"));
  CHECK(r.refusal.detail.find("x.first") != std::string::npos);
  // Neighbouring words that do not share a byte are fine.
  const std::string apart = head + R"({"id":"x.first","kind":"word_patch","revault_va":"0x1500010","words":[{"offset":0,"stock_bytes":"e0031faa","new_bytes":"1f2003d5"}]},)" +
                            R"({"id":"x.next","kind":"word_patch","revault_va":"0x1500014","words":[{"offset":0,"stock_bytes":")" + bytesAt(0x1400014) + R"(","new_bytes":"1f2003d5"}]}]})";
  CHECK(Parse(apart, w, &rows, &refusal));
  CHECK(w.Patch(rows).ok);
}

// ---- the command line -------------------------------------------------------------------------------------------

int RunProcess(const std::vector<std::string>& argv, const std::string& stdoutPath, const std::string& stderrPath) {
  const pid_t pid = fork();
  if (pid == 0) {
    const int o = open(stdoutPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int e = open(stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (o < 0 || e < 0 || dup2(o, 1) < 0 || dup2(e, 2) < 0) _exit(127);
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    execv(args[0], args.data());
    _exit(127);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

void WriteBytes(const std::string& path, const std::vector<std::uint8_t>& b) {
  std::ofstream o(path, std::ios::binary | std::ios::trunc);
  o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

bool Exists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }

TEST(TheCommandLineWritesTheFileAndTheReport) {
  World w;
  const std::string stock = g_scratch + "/cli_stock.so", out = g_scratch + "/cli_out.so", report = g_scratch + "/cli_report.json";
  WriteBytes(stock, w.elf.bytes);
  std::remove(out.c_str());
  std::ostringstream so, se;
  const int rc = RunCli({"--stock", stock, "--out", out, "--report", report, "--extra-rows", g_fixtures + "/libr15_extra_rows_example.json"}, w.spec, so, se);
  CHECK_EQ(rc, 0);
  CHECK(se.str().empty());
  CHECK(Exists(out));
  const Json rep = Json::parse(ReadText(report));
  CHECK_EQ(rep["events"].size(), static_cast<std::size_t>(8));  // 6 sites, 2 words
  CHECK(rep["summary"]["status"] == "patched");
  CHECK_EQ(rep["summary"]["extra_rows"].get<int>(), 1);
  const std::string fixture = Fixture();
  CHECK(rep["summary"]["extra_rows_sha256"] == nevr_quest::data::Sha256Hex(fixture.data(), fixture.size()));
  // Every stdout line is one JSON record, the last one the summary.
  std::istringstream lines(so.str());
  std::string line;
  std::size_t n = 0;
  Json last;
  while (std::getline(lines, line)) {
    last = Json::parse(line);
    CHECK(last.contains("event"));
    ++n;
  }
  CHECK_EQ(n, static_cast<std::size_t>(9));
  CHECK(last["event"] == "libr15_patch");
  const std::string written = ReadText(out);
  CHECK(nevr_quest::data::Sha256Hex(written.data(), written.size()) == rep["summary"]["patched_sha256"]);
  // Without the extra rows the file is the public one.
  const std::string plainOut = g_scratch + "/cli_plain.so", plainReport = g_scratch + "/cli_plain.json";
  std::ostringstream so2, se2;
  CHECK_EQ(RunCli({"--stock", stock, "--out", plainOut, "--report", plainReport}, w.spec, so2, se2), 0);
  CHECK(Json::parse(ReadText(plainReport))["summary"]["build_id"] == BuildIdHex(DeriveBuildId(StockId(), CanonicalTable(w.spec.rows, w.spec.rowCount, {}, kSlotArea))));
  CHECK(Json::parse(ReadText(plainReport))["summary"]["extra_rows"] == 0);
  for (const std::string& f : {stock, out, report, plainOut, plainReport}) std::remove(f.c_str());
}

TEST(ARefusalExitsOneWritesTheReasonAndLeavesNoOutput) {
  World w;
  w.rows[0].sites[0].words[0] ^= 1;
  const std::string stock = g_scratch + "/cli_bad.so", out = g_scratch + "/cli_bad_out.so", report = g_scratch + "/cli_bad_report.json";
  WriteBytes(stock, w.elf.bytes);
  std::remove(out.c_str());
  std::ostringstream so, se;
  CHECK_EQ(RunCli({"--stock", stock, "--out", out, "--report", report}, w.spec, so, se), 1);
  CHECK(!Exists(out));
  CHECK(!Exists(out + ".partial"));
  CHECK(so.str().empty());
  const Json err = Json::parse(se.str());
  CHECK(err["reason"] == "row_words_mismatch");
  CHECK(err["id"] == "s.join");
  CHECK(Json::parse(ReadText(report))["summary"]["status"] == "refused");
  // A refused extra-rows file is a refusal too, with its digest in the report.
  World ok;
  const std::string badRows = g_scratch + "/cli_bad_rows.json";
  std::string text = Fixture();
  Replace(&text, "\"version\": 1,", "\"version\": 2,");
  std::ofstream(badRows) << text;
  WriteBytes(stock, ok.elf.bytes);
  std::ostringstream so2, se2;
  CHECK_EQ(RunCli({"--stock", stock, "--out", out, "--report", report, "--extra-rows", badRows}, ok.spec, so2, se2), 1);
  CHECK(!Exists(out));
  const Json rep = Json::parse(ReadText(report));
  CHECK(rep["summary"]["reason"] == "extra_rows_version");
  CHECK(rep["summary"]["extra_rows_sha256"] == nevr_quest::data::Sha256Hex(text.data(), text.size()));
  for (const std::string& f : {stock, report, badRows}) std::remove(f.c_str());
}

TEST(UsageAndIoErrorsExitTwo) {
  World w;
  std::ostringstream so, se;
  CHECK_EQ(RunCli({}, w.spec, so, se), 2);
  CHECK_EQ(RunCli({"--stock", "a", "--out", "b"}, w.spec, so, se), 2);
  CHECK_EQ(RunCli({"--stock", "a", "--out", "b", "--report", "c", "--bogus", "d"}, w.spec, so, se), 2);
  CHECK_EQ(RunCli({"--stock", "a", "--stock", "a2", "--out", "b", "--report", "c"}, w.spec, so, se), 2);
  CHECK_EQ(RunCli({"--stock", "a", "--out", "a", "--report", "c"}, w.spec, so, se), 2);
  CHECK_EQ(RunCli({"--stock", g_scratch + "/no_such_file.so", "--out", g_scratch + "/o.so", "--report", g_scratch + "/r.json"}, w.spec, so, se), 2);
  CHECK(!Exists(g_scratch + "/o.so"));
  CHECK(so.str().empty());
  CHECK(Json::parse(se.str().substr(0, se.str().find('\n')))["status"] == "usage_error");
}

// The one JSON line RunCli writes to `err` for a usage or I/O error: exactly one line, ending in a newline.
Json OneErrorLine(const std::ostringstream& err, const char* what) {
  const std::string text = err.str();
  if (text.empty() || text.back() != '\n' || text.find('\n') != text.size() - 1) {
    std::fprintf(stderr, "  %s: stderr is not exactly one line: \"%s\"\n", what, text.c_str());
    ++mini_test::Failures();
    return Json::object();
  }
  const Json j = Json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) {
    std::fprintf(stderr, "  %s: stderr is not JSON: \"%s\"\n", what, text.c_str());
    ++mini_test::Failures();
    return Json::object();
  }
  return j;
}

TEST(AFileTheToolCannotReadExitsTwoWithOneJsonLineInsteadOfAborting) {
  World w;
  const std::string stock = g_scratch + "/read_stock.so", out = g_scratch + "/read_out.so", report = g_scratch + "/read_report.json";
  const std::string dir = g_scratch + "/read_dir";
  CHECK_EQ(mkdir(dir.c_str(), 0755), 0);
  WriteBytes(stock, w.elf.bytes);
  std::remove(out.c_str());
  std::remove(report.c_str());
  {  // --stock names a directory: open succeeds, the first read fails
    std::ostringstream so, se;
    CHECK_EQ(RunCli({"--stock", dir, "--out", out, "--report", report}, w.spec, so, se), 2);
    Json e = OneErrorLine(se, "stock is a directory");
    CHECK(e["status"] == "io_error");
    CHECK(e["detail"].get<std::string>().find("--stock") != std::string::npos);
    CHECK(so.str().empty());
    CHECK(!Exists(out));
    CHECK(!Exists(report));
  }
  {  // --extra-rows names a directory
    std::ostringstream so, se;
    CHECK_EQ(RunCli({"--stock", stock, "--out", out, "--report", report, "--extra-rows", dir}, w.spec, so, se), 2);
    Json e = OneErrorLine(se, "extra rows is a directory");
    CHECK(e["status"] == "io_error");
    CHECK(e["detail"].get<std::string>().find("--extra-rows") != std::string::npos);
    CHECK(!Exists(out));
  }
  {  // --extra-rows names a file that is not there
    std::ostringstream so, se;
    CHECK_EQ(RunCli({"--stock", stock, "--out", out, "--report", report, "--extra-rows", g_scratch + "/no_such_rows.json"}, w.spec, so, se), 2);
    Json e = OneErrorLine(se, "extra rows missing");
    CHECK(e["status"] == "io_error");
    CHECK(e["detail"].get<std::string>().find("--extra-rows") != std::string::npos);
    CHECK(!Exists(out));
  }
  // The shipped tool does the same as a process: exit 2 (not a signal), one JSON line on stderr, no output file.
  const std::string so = g_scratch + "/read.stdout", se = g_scratch + "/read.stderr";
  CHECK_EQ(RunProcess({g_tool, "--stock", dir, "--out", out, "--report", report}, so, se), 2);
  const std::string text = ReadText(se);
  CHECK(!text.empty() && text.find('\n') == text.size() - 1);
  CHECK(Json::parse(text, nullptr, false)["status"] == "io_error");
  CHECK(!Exists(out));
  for (const std::string& f : {stock, so, se}) std::remove(f.c_str());
  CHECK_EQ(rmdir(dir.c_str()), 0);
}

TEST(NoExtraRowsFileCanMakeTheCommandLineThrow) {
  // Each of these reaches a call that throws (std::stoull, or a typed get<std::string>() on a number) unless the
  // check in front of it refuses the value first; the command line must answer with a refusal, exit 1, never an abort.
  struct Case {
    const char* what;
    const char* from;
    const char* to;
    const char* reason;
  };
  const Case cases[] = {
      {"revault_va is only the prefix", "\"0x1500010\"", "\"0x\"", "extra_rows_revault_va"},
      {"revault_va has 17 digits", "\"0x1500010\"", "\"0x10000000000000000\"", "extra_rows_revault_va"},
      {"stock_build_id is a number", "\"b243509c08ce677aeb95fa348016949b3fc45230\"", "5", "extra_rows_stock_build_id"},
      {"id is a number", "\"x.example_guard\"", "5", "extra_rows_id"},
      {"kind is a number", "\"word_patch\"", "5", "extra_rows_kind"},
  };
  World w;
  const std::string stock = g_scratch + "/throw_stock.so", out = g_scratch + "/throw_out.so", report = g_scratch + "/throw_report.json";
  const std::string rows = g_scratch + "/throw_rows.json";
  WriteBytes(stock, w.elf.bytes);
  for (const Case& c : cases) {
    std::string text = Fixture();
    if (!Replace(&text, c.from, c.to)) {
      std::fprintf(stderr, "  %s: the fixture does not hold exactly one \"%s\"\n", c.what, c.from);
      CHECK(false);
      continue;
    }
    std::ofstream(rows, std::ios::binary | std::ios::trunc) << text;
    std::remove(out.c_str());
    std::ostringstream so, se;
    const int rc = RunCli({"--stock", stock, "--out", out, "--report", report, "--extra-rows", rows}, w.spec, so, se);
    if (rc != 1) std::fprintf(stderr, "  %s: exit %d, stderr %s\n", c.what, rc, se.str().c_str());
    CHECK_EQ(rc, 1);
    Json e = OneErrorLine(se, c.what);
    if (e["reason"] != c.reason) std::fprintf(stderr, "  %s: reason %s, expected %s\n", c.what, e.value("reason", "?").c_str(), c.reason);
    CHECK(e["reason"] == c.reason);
    CHECK(!Exists(out));
  }
  for (const std::string& f : {stock, report, rows}) std::remove(f.c_str());
}

TEST(APathOrASectionNameThatIsNotUtf8StillGetsItsExitCodeAndItsReport) {
  const std::string replacement = "\xEF\xBF\xBD";  // U+FFFD: what the dump writes for a byte that is not UTF-8
  {
    World w;
    std::ostringstream so, se;
    const int rc = RunCli({"--stock", g_scratch + "/no_such_\xff.so", "--out", g_scratch + "/u.so", "--report", g_scratch + "/u.json"}, w.spec, so, se);
    CHECK_EQ(rc, 2);
    Json e = OneErrorLine(se, "path that is not UTF-8");
    CHECK(e["status"] == "io_error");
    CHECK(e["detail"].get<std::string>().find("no_such_" + replacement + ".so") != std::string::npos);
  }
  {
    SynthOptions o = World::DefaultOptions();
    o.bssName = "\xff.bss";
    World w(o);
    const std::string stock = g_scratch + "/utf_stock.so", out = g_scratch + "/utf_out.so", report = g_scratch + "/utf_report.json";
    WriteBytes(stock, w.elf.bytes);
    std::remove(out.c_str());
    std::ostringstream so, se;
    CHECK_EQ(RunCli({"--stock", stock, "--out", out, "--report", report}, w.spec, so, se), 0);
    CHECK(se.str().empty());
    CHECK(Exists(out));
    const Json rep = Json::parse(ReadText(report), nullptr, false);
    CHECK(!rep.is_discarded());
    if (!rep.is_discarded()) CHECK(rep["summary"]["bss_section"] == replacement + ".bss");
    std::istringstream lines(so.str());
    std::string line;
    std::size_t n = 0;
    while (std::getline(lines, line)) {
      CHECK(!Json::parse(line, nullptr, false).is_discarded());
      ++n;
    }
    CHECK_EQ(n, static_cast<std::size_t>(7));  // 6 sites and the summary
    for (const std::string& f : {stock, out, report}) std::remove(f.c_str());
  }
}

TEST(TheShippedToolRefusesAFileThatIsNotThePinnedLibr15) {
  World w;
  const std::string stock = g_scratch + "/tool_stock.so", out = g_scratch + "/tool_out.so", report = g_scratch + "/tool_report.json";
  const std::string so = g_scratch + "/tool.stdout", se = g_scratch + "/tool.stderr";
  WriteBytes(stock, w.elf.bytes);
  std::remove(out.c_str());
  CHECK(!g_tool.empty());
  // The synthetic file carries the pinned build ID but is not the pinned file: the tool's digest check refuses it.
  CHECK_EQ(RunProcess({g_tool, "--stock", stock, "--out", out, "--report", report}, so, se), 1);
  CHECK(!Exists(out));
  const Json err = Json::parse(ReadText(se));
  CHECK(err["reason"] == "stock_sha256_mismatch");
  CHECK(ReadText(so).empty());
  CHECK(Json::parse(ReadText(report))["summary"]["status"] == "refused");
  // A wrong build ID is refused before the digest is computed.
  std::vector<std::uint8_t> other = w.elf.bytes;
  other[0x210] ^= 1;
  WriteBytes(stock, other);
  CHECK_EQ(RunProcess({g_tool, "--stock", stock, "--out", out, "--report", report}, so, se), 1);
  CHECK(Json::parse(ReadText(se))["reason"] == "stock_build_id_mismatch");
  CHECK_EQ(RunProcess({g_tool}, so, se), 2);
  for (const std::string& f : {stock, report, so, se}) std::remove(f.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: libr15_patcher_test <scratch dir> <fixtures dir> <quest_patch_libr15> [name-filter]\n");
    return 2;
  }
  g_scratch = argv[1];
  g_fixtures = argv[2];
  g_tool = argv[3];
  return mini_test::RunAll(argc - 3, argv + 3);
}
