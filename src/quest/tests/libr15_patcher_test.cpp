// Host test for the libr15 build-time patcher (src/quest/patch/libr15_patcher.h), ADR 0021 tests 2 and 9, on a
// synthetic stock ELF built in memory (synthetic_elf.h): the byte diff, determinism, every refusal with the row's
// id, the word patches and the extra-rows file (format 1) with one case per rule, and the command line.
//
// Run: libr15_patcher_test <scratch dir> <fixtures dir> <quest_patch_libr15 binary> [name-filter]
// The scratch directory is the test's own (files are written there and removed by the recipe); the binary is the
// shipped tool, run against the synthetic file (it must refuse: the synthetic file is not the pinned libr15).

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
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
constexpr std::uint64_t kGuardA = 0x1400010, kGuardB = 0x1400018;  // the fixture's two words (disasm_va 0x1500010)
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
    for (int i = 0; i < 4; ++i) credits.sites[i] = {at[i] + kDisasmBias, 1, {elf.WordAt(at[i])}};
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
    r.sites[0] = {vaddr + kDisasmBias, 3, {elf.WordAt(vaddr), elf.WordAt(vaddr + 4), elf.WordAt(vaddr + 8)}};
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
    CHECK(e.contains("id") && e.contains("disasm_va") && e.contains("vaddr") && e.contains("stock") && e.contains("stub") && e.contains("slot"));
  }
  CHECK_EQ(sites, static_cast<std::size_t>(6));
  CHECK(r.summary["status"] == "patched");
  CHECK_EQ(r.summary["rows"].get<int>(), 3);
  CHECK(r.summary["patched_sha256"] != r.summary["stock_sha256"]);
}

std::string HexOf(std::uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
  return b;
}
std::string Hex8Of(std::uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08x", v);
  return b;
}

// The site event at a vaddr, or null.
const Json* SiteAt(const PatchResult& r, std::uint64_t vaddr) {
  for (const Json& e : r.events) {
    if (e["event"] == "libr15_patch_site" && e["vaddr"] == HexOf(vaddr)) return &e;
  }
  return nullptr;
}

// The values in the report that the layout, the table and the file determine. The vocabulary is ADR 0021 section 2
// item 3: a libr15_patch_site event's kind is inline_hook or credits_pair, each changed range of a row has a what that
// is the row id and :stub, :credits_adrp, :credits_ldr or :word, and end_symbols is sorted by name in byte order.
TEST(TheReportStatesTheFactsTheFileAndTheTableDetermine) {
  World w;
  const PatchResult r = w.Patch();
  CHECK(r.ok);
  if (!r.ok) return;
  const Json& s = r.summary;
  CHECK(s["event"] == "libr15_patch");
  CHECK(s["status"] == "patched");
  CHECK(s["stock_build_id"] == kLibR15StockBuildId);  // the stock ID, not the derived one
  CHECK(s["build_id"] == BuildIdHex(DeriveBuildId(StockId(), CanonicalTable(w.spec.rows, w.spec.rowCount, {}, kSlotArea))));
  CHECK(s["stock_sha256"] == nevr_quest::data::Sha256Hex(w.elf.bytes.data(), w.elf.bytes.size()));
  CHECK(s["patched_sha256"] == nevr_quest::data::Sha256Hex(r.output.data(), r.output.size()));
  CHECK(s["slot_area"] == "0x1403600");      // roundup(0x14035f8, 64)
  CHECK(s["slot_area_end"] == "0x1403618");  // + 8 bytes per row, 3 rows
  CHECK(s["p_memsz_stock"] == "0x5f8");      // 0x14035f8 - 0x1403000
  CHECK(s["p_memsz_patched"] == "0x618");    // grown by 0x1403618 - 0x14035f8
  CHECK(s["bss_section"] == ".bss");
  CHECK_EQ(s["extra_rows"].get<int>(), 0);
  {  // end_symbols: the six names, sorted by name in byte order (the file lists them in another order), each with its value
    const char* const sorted[] = {"__bss_end__", "__bss_start", "__end__", "_bss_end__", "_edata", "_end"};
    CHECK_EQ(s["end_symbols"].size(), static_cast<std::size_t>(6));
    for (std::size_t i = 0; i < 6 && i < s["end_symbols"].size(); ++i) CHECK(s["end_symbols"][i]["name"] == sorted[i]);
  }

  // The site events: the table's address, the file's address and offset, the words, the slot and its address.
  Stub stub{};
  struct Site {
    std::uint64_t vaddr;
    const char* id;
    unsigned slot;
  };
  for (const Site site : {Site{kJoinVa, "s.join", 0}, Site{kFailVa, "s.fail", 1}}) {
    const Json* e = SiteAt(r, site.vaddr);
    CHECK(e != nullptr);
    if (e == nullptr) continue;
    CHECK(EncodeSlotStub(site.vaddr, kSlotArea + 8 * site.slot, &stub));
    CHECK((*e)["id"] == site.id);
    CHECK((*e)["kind"] == "inline_hook");
    CHECK((*e)["disasm_va"] == HexOf(site.vaddr + kDisasmBias));
    CHECK((*e)["file_offset"] == HexOf(SynthElf::FileOffsetOfText(site.vaddr)));
    CHECK((*e)["slot"] == site.slot);
    CHECK((*e)["slot_vaddr"] == HexOf(kSlotArea + 8 * site.slot));
    CHECK_EQ((*e)["stock"].size(), static_cast<std::size_t>(3));
    CHECK_EQ((*e)["stub"].size(), static_cast<std::size_t>(3));
    for (std::size_t i = 0; i < 3 && (*e)["stock"].size() == 3 && (*e)["stub"].size() == 3; ++i) {
      CHECK((*e)["stock"][i] == Hex8Of(w.elf.WordAt(site.vaddr + 4 * i)));
      CHECK((*e)["stub"][i] == Hex8Of(stub.words[i]));
    }
  }
  // The four credits words: slot 2, each with its own address and offset, its stock word and its replacement.
  const std::uint64_t credits[4] = {kAdrp0, kAdd0, kAdrp1, kAdd1};
  for (const std::uint64_t va : credits) {
    const Json* e = SiteAt(r, va);
    CHECK(e != nullptr);
    if (e == nullptr) continue;
    CHECK((*e)["id"] == "s.credits");
    CHECK((*e)["kind"] == "credits_pair");  // the ADD word's event as well as the ADRP word's
    CHECK((*e)["disasm_va"] == HexOf(va + kDisasmBias));
    CHECK((*e)["file_offset"] == HexOf(SynthElf::FileOffsetOfText(va)));
    CHECK((*e)["slot"] == 2);
    CHECK((*e)["slot_vaddr"] == HexOf(kSlotArea + 16));
    CHECK_EQ((*e)["stock"].size(), static_cast<std::size_t>(1));
    CHECK_EQ((*e)["stub"].size(), static_cast<std::size_t>(1));
    std::uint32_t written = 0;
    std::memcpy(&written, r.output.data() + SynthElf::FileOffsetOfText(va), 4);
    if ((*e)["stock"].size() == 1 && (*e)["stub"].size() == 1) {
      CHECK((*e)["stock"][0] == Hex8Of(w.elf.WordAt(va)));
      CHECK((*e)["stub"][0] == Hex8Of(written));
    }
  }
  // Each change is a byte range of the file, with its length; a row's ranges are named <row id> and the kind of word.
  {
    std::map<std::string, std::string> whatAt;
    for (const Json& c : r.summary["changes"]) whatAt[c["offset"].get<std::string>()] = c["what"].get<std::string>();
    CHECK(whatAt[HexOf(SynthElf::FileOffsetOfText(kJoinVa))] == "s.join:stub");
    CHECK(whatAt[HexOf(SynthElf::FileOffsetOfText(kFailVa))] == "s.fail:stub");
    CHECK(whatAt[HexOf(SynthElf::FileOffsetOfText(kAdrp0))] == "s.credits:credits_adrp");
    CHECK(whatAt[HexOf(SynthElf::FileOffsetOfText(kAdd0))] == "s.credits:credits_ldr");
    CHECK(whatAt[HexOf(SynthElf::FileOffsetOfText(kAdrp1))] == "s.credits:credits_adrp");
    CHECK(whatAt[HexOf(SynthElf::FileOffsetOfText(kAdd1))] == "s.credits:credits_ldr");
  }
  std::size_t checked = 0;
  for (const Json& c : r.summary["changes"]) {
    if (c["offset"] == HexOf(SynthElf::FileOffsetOfText(kJoinVa))) { CHECK_EQ(c["length"].get<int>(), 12); ++checked; }
    if (c["offset"] == HexOf(SynthElf::FileOffsetOfText(kAdd1))) { CHECK_EQ(c["length"].get<int>(), 4); ++checked; }
    if (c["offset"] == HexOf(w.elf.PhdrMemszOffset())) { CHECK_EQ(c["length"].get<int>(), 8); ++checked; }
    if (c["offset"] == HexOf(w.elf.BssSizeOffset())) { CHECK_EQ(c["length"].get<int>(), 8); ++checked; }
    if (c["offset"] == HexOf(kNoteDescOffset)) { CHECK_EQ(c["length"].get<int>(), 20); ++checked; }
  }
  CHECK_EQ(checked, static_cast<std::size_t>(5));
}

TEST(TheReportStatesTheWordsOfAnExtraRowWithTheirWidthsAndAddresses) {
  World w;
  // Words that begin with zero digits: the 8-digit form is zero-padded, in the word events and in a site's words.
  w.elf.PutWord(kGuardA, 0x0000001f);
  w.elf.PutWord(kJoinVa + 8, 0x0000001f);
  w.rows[0] = w.Inline("s.join", kJoinVa, 0);
  WordPatchRow row;
  row.id = "x.zeros";
  row.disasmVa = kGuardA + kDisasmBias;
  row.edits.push_back({0, {0x1f, 0x00, 0x00, 0x00}, {0x01, 0x00, 0x00, 0x00}});
  const PatchResult r = w.Patch({row});
  CHECK(r.ok);
  if (!r.ok) {
    std::fprintf(stderr, "  %s\n", r.summary.dump().c_str());
    return;
  }
  const Json* join = SiteAt(r, kJoinVa);
  CHECK(join != nullptr);
  if (join != nullptr && (*join)["stock"].size() == 3) CHECK((*join)["stock"][2] == "0x0000001f");
  std::size_t words = 0;
  for (const Json& e : r.events) {
    if (e["event"] != "libr15_patch_word") continue;
    ++words;
    CHECK(!e.contains("kind"));  // a word patch has its own event, which has no kind
    CHECK(e["id"] == "x.zeros");
    CHECK(e["offset"] == 0);
    CHECK(e["disasm_va"] == HexOf(kGuardA + kDisasmBias));
    CHECK(e["vaddr"] == HexOf(kGuardA));
    CHECK(e["file_offset"] == HexOf(SynthElf::FileOffsetOfText(kGuardA)));
    CHECK(e["stock_bytes"] == "1f000000");
    CHECK(e["stock_word"] == "0x0000001f");
    CHECK(e["new_bytes"] == "01000000");
    CHECK(e["new_word"] == "0x00000001");
  }
  CHECK_EQ(words, static_cast<std::size_t>(1));
  // The change that accounts for the word is 4 bytes long, at the word's offset.
  std::size_t found = 0;
  for (const Json& c : r.summary["changes"]) {
    if (c["offset"] != HexOf(SynthElf::FileOffsetOfText(kGuardA))) continue;
    CHECK(c["what"] == "x.zeros:word");
    CHECK_EQ(c["length"].get<int>(), 4);
    ++found;
  }
  CHECK_EQ(found, static_cast<std::size_t>(1));
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
  inData.rows[1].sites[0].disasmVa = kDataVaddr + 0x40 + kDisasmBias;  // .data: not executable
  ExpectRefusal(inData.Patch(), "site_outside_text", "s.fail", "site in data");
  World unmapped;
  unmapped.rows[0].sites[0].disasmVa = 0x9000000 + kDisasmBias;
  ExpectRefusal(unmapped.Patch(), "site_outside_text", "s.join", "site unmapped");
  World straddle;  // the last word of the text and the next two are not
  straddle.rows[0].sites[0].disasmVa = (kTextEnd - 4) + kDisasmBias;
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
  w.spec.alsoSha256 = {std::string(64, '1')};
  ExpectRefusal(w.Patch(), "stock_sha256_mismatch", "", "a listed digest that is not the file's");
  w.spec.alsoSha256 = {std::string(64, '1'), nevr_quest::data::Sha256Hex(w.elf.bytes.data(), w.elf.bytes.size())};
  CHECK(w.Patch().ok);  // #1737: the file's digest may be any of the listed ones
  w.spec.alsoSha256.clear();
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
  w.rows[0].sites[0].disasmVa = 0x1000;
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
  edit.disasmVa = kGuardA + kDisasmBias;
  edit.edits.push_back({2, {0xe0, 0x03, 0x1f, 0xaa}, {0x1f, 0x20, 0x03, 0xd5}});
  ExpectRefusal(w.Patch({edit}), "word_offset", "x.direct", "offset not a multiple of 4");
  edit.edits[0].offset = 0;
  edit.disasmVa = 0x1000;
  const PatchResult below = w.Patch({edit});
  ExpectRefusal(below, "word_outside_text", "x.direct", "address below the bias");
  CHECK(below.refusal.detail.find("below the bias") != std::string::npos);
  edit.disasmVa = kGuardA + kDisasmBias;
  CHECK(w.Patch({edit}).ok);
}

// ---- crafted files: each rule of the ELF reader and of the layout, broken in a file of its own --------------------

using Bytes = std::vector<std::uint8_t>;

// An elf_invalid refusal whose detail names the rule (the code alone would not tell two rules apart).
void ExpectElfInvalid(const PatchResult& r, const char* detailPart, const char* what) {
  ExpectRefusal(r, "elf_invalid", "", what);
  if (r.refusal.detail.find(detailPart) == std::string::npos) {
    std::fprintf(stderr, "  %s: the detail \"%s\" does not name \"%s\"\n", what, r.refusal.detail.c_str(), detailPart);
    ++mini_test::Failures();
  }
}

PatchResult PatchBytes(const World& w, const Bytes& b) { return PatchLibr15(b, w.spec, {}, ""); }

// Rewrites the note segment's content: one note (header, name padded to 4, descriptor padded to 4) at the note's offset.
void PutNote(Bytes& b, const std::string& name, std::uint32_t type, std::uint32_t descsz, const Bytes& descBytes) {
  Elf64_Nhdr nh{};
  nh.n_namesz = static_cast<Elf64_Word>(name.size());
  nh.n_descsz = descsz;
  nh.n_type = type;
  WriteAt(b, kNoteOffset, nh);
  std::uint64_t at = kNoteOffset + sizeof nh;
  std::memcpy(b.data() + at, name.data(), name.size());
  at += (name.size() + 3) & ~std::uint64_t{3};
  std::memcpy(b.data() + at, descBytes.data(), descBytes.size());
  Elf64_Phdr note = Phdr(b, kPhNote);
  note.p_filesz = note.p_memsz = (at + ((descsz + 3) & ~std::uint64_t{3})) - kNoteOffset;
  SetPhdr(b, kPhNote, note);
}

Bytes StockIdBytes() {
  const BuildIdBytes id = StockId();
  return Bytes(id.begin(), id.end());
}

TEST(ATruncatedOrMisIdentifiedFileIsRefusedByTheHeaderRuleItBreaks) {
  World w;
  const Bytes good = w.elf.bytes;
  CHECK(PatchBytes(w, good).ok);
  ExpectElfInvalid(PatchBytes(w, Bytes(good.begin(), good.begin() + 63)), "shorter than an ELF header", "63 bytes");
  ExpectElfInvalid(PatchBytes(w, Bytes(good.begin(), good.begin() + 64)), "program header table outside the file", "64 bytes: the header alone");
  Bytes b = good;
  b[1] = 'X';  // the 0x7f stays, the rest of the magic is wrong
  ExpectElfInvalid(PatchBytes(w, b), "not a little-endian ELF64", "magic");
  b = good;
  b[EI_CLASS] = ELFCLASS32;
  ExpectElfInvalid(PatchBytes(w, b), "not a little-endian ELF64", "ELFCLASS32");
  b = good;
  b[EI_DATA] = ELFDATA2MSB;
  ExpectElfInvalid(PatchBytes(w, b), "not a little-endian ELF64", "big endian");
  Elf64_Ehdr eh = Ehdr(good);
  eh.e_type = ET_EXEC;
  b = good;
  SetEhdr(b, eh);
  ExpectElfInvalid(PatchBytes(w, b), "not an AArch64 shared object", "ET_EXEC");
  eh = Ehdr(good);
  eh.e_shentsize = sizeof(Elf64_Shdr) + 1;
  b = good;
  SetEhdr(b, eh);
  ExpectElfInvalid(PatchBytes(w, b), "unexpected header entry sizes", "e_shentsize");
  eh = Ehdr(good);
  eh.e_phentsize = sizeof(Elf64_Phdr) + 1;
  b = good;
  SetEhdr(b, eh);
  ExpectElfInvalid(PatchBytes(w, b), "unexpected header entry sizes", "e_phentsize");
  // A table count whose entry COUNT fits the file but whose byte length (count * entry size) does not.
  eh = Ehdr(good);
  eh.e_phnum = 300;  // 300 * 56 = 16800 bytes from offset 64 in a 16384-byte file
  b = good;
  SetEhdr(b, eh);
  ExpectElfInvalid(PatchBytes(w, b), "program header table outside the file", "e_phnum * entry size");
  eh = Ehdr(good);
  eh.e_shnum = 60;  // 60 * 64 = 3840 bytes from 0x3300 in a file that ends 3328 bytes later
  b = good;
  SetEhdr(b, eh);
  ExpectElfInvalid(PatchBytes(w, b), "section header table outside the file", "e_shnum * entry size");
  // An empty section table at the very end of the file is a table inside it: the file then has no .bss to grow.
  eh = Ehdr(good);
  eh.e_shoff = good.size();
  eh.e_shnum = 0;
  eh.e_shstrndx = 0;
  b = good;
  SetEhdr(b, eh);
  ExpectRefusal(PatchBytes(w, b), "layout_bss_section", "", "an empty section table at the end of the file");
}

TEST(AProgramHeaderTableThatBreaksTheLoadRulesIsRefusedWithTheRule) {
  World w;
  const Bytes good = w.elf.bytes;
  Bytes b = good;
  Elf64_Phdr ph = Phdr(b, kPhLoadData);
  ph.p_filesz = ph.p_memsz = 0x2000;  // 0x3000 + 0x2000 is past the end of the 0x4000-byte file
  SetPhdr(b, kPhLoadData, ph);
  ExpectElfInvalid(PatchBytes(w, b), "file range is outside the file", "PT_LOAD past the end of the file");
  b = good;
  ph = Phdr(b, kPhLoadData);
  ph.p_vaddr = 0x1401000;  // inside the text segment
  SetPhdr(b, kPhLoadData, ph);
  ExpectElfInvalid(PatchBytes(w, b), "not sorted by vaddr or overlap", "overlapping PT_LOADs");
  b = good;
  for (const std::size_t i : {kPhLoadText, kPhLoadData}) {
    ph = Phdr(b, i);
    ph.p_type = PT_NULL;
    SetPhdr(b, i, ph);
  }
  ExpectElfInvalid(PatchBytes(w, b), "no PT_LOAD", "no PT_LOAD at all");
  // A PT_LOAD with no memory at all is a valid (empty) one; one that wraps the address space is not.
  SynthOptions o = World::DefaultOptions();
  o.trailingLoadVaddr = 0x1405000;
  World withTrailing(o);
  b = withTrailing.elf.bytes;
  ph = Phdr(b, 3);
  CHECK(ph.p_type == PT_LOAD && ph.p_vaddr == 0x1405000);
  ph.p_memsz = 0;
  SetPhdr(b, 3, ph);
  CHECK(PatchBytes(withTrailing, b).ok);
  ph.p_vaddr = 0xfffffffffffff000ull;
  ph.p_memsz = 0x2000;
  SetPhdr(b, 3, ph);
  ExpectElfInvalid(PatchBytes(withTrailing, b), "wraps the address space", "PT_LOAD that wraps");
  // A PT_LOAD that starts exactly where the writable one ends is a valid table; growing into it is what is refused.
  SynthOptions touching = World::DefaultOptions();
  touching.trailingLoadVaddr = touching.imageEnd;
  World touches(touching);
  ExpectRefusal(touches.Patch(), "layout_overlap", "", "a PT_LOAD that starts at the image end");
}

TEST(ASectionTableThatBreaksTheRulesIsRefusedOrReadAsTheFileIntends) {
  World w;
  const Bytes good = w.elf.bytes;
  // A null section header is skipped whatever its offset and size say.
  Bytes b = good;
  Elf64_Shdr null = Shdr(b, 0);
  null.sh_offset = 0xffffff;
  null.sh_size = 0x1000;
  SetShdr(b, 0, null);
  CHECK(PatchBytes(w, b).ok);
  // A section that is not NOBITS must lie in the file.
  b = good;
  Elf64_Shdr dynstr = Shdr(b, kSecDynstr);
  dynstr.sh_size = 0x8000;
  SetShdr(b, kSecDynstr, dynstr);
  ExpectElfInvalid(PatchBytes(w, b), "a section's file range is outside the file", "section past the end of the file");
  // The section-name table: an index one past the last section, section 0 (no table), and a section that is not a string
  // table all leave the names unread, so the report has no section name; the real table names the section.
  const PatchResult named = PatchBytes(w, good);
  CHECK(named.ok);
  CHECK(named.summary["bss_section"] == ".bss");
  Elf64_Ehdr eh = Ehdr(good);
  eh.e_shstrndx = eh.e_shnum;
  b = good;
  SetEhdr(b, eh);
  PatchResult r = PatchBytes(w, b);
  CHECK(r.ok);
  CHECK(r.summary["bss_section"] == "");
  eh = Ehdr(good);
  eh.e_shstrndx = 0;
  b = good;
  SetEhdr(b, eh);
  SetShdr(b, 0, Shdr(good, kSecShstr));  // section 0 made to look like the name table, which index 0 never means
  r = PatchBytes(w, b);
  CHECK(r.ok);
  CHECK(r.summary["bss_section"] == "");
  b = good;
  Elf64_Shdr notStrtab = Shdr(b, kSecShstr);
  notStrtab.sh_type = SHT_PROGBITS;
  SetShdr(b, kSecShstr, notStrtab);
  r = PatchBytes(w, b);
  CHECK(r.ok);
  CHECK(r.summary["bss_section"] == "");
  // The section that ends the writable segment: it must be NOBITS and allocated.
  SynthOptions progbits = World::DefaultOptions();
  progbits.bssType = SHT_PROGBITS;
  World p(progbits);
  ExpectRefusal(p.Patch(), "layout_bss_section", "", "the last section is not NOBITS");
  SynthOptions unalloc = World::DefaultOptions();
  unalloc.bssFlags = SHF_WRITE;
  World u(unalloc);
  ExpectRefusal(u.Patch(), "layout_bss_section", "", "the last section is not allocated");
  // Every allocated section lies inside a PT_LOAD, entirely.
  Elf64_Shdr stray{};
  stray.sh_type = SHT_PROGBITS;
  stray.sh_flags = SHF_ALLOC;
  stray.sh_offset = 0x3c00;
  stray.sh_size = 0x10;
  stray.sh_addr = 0x1401000;  // inside the text segment
  SynthOptions inside = World::DefaultOptions();
  inside.extraSections = {stray};
  World in(inside);
  CHECK(in.Patch().ok);
  stray.sh_addr = 0x2000000;  // in no segment
  SynthOptions outside = World::DefaultOptions();
  outside.extraSections = {stray};
  World out(outside);
  const PatchResult outResult = out.Patch();
  ExpectRefusal(outResult, "output_invalid", "", "an allocated section outside every PT_LOAD");
  CHECK(outResult.refusal.detail.find("outside every PT_LOAD") != std::string::npos);
  stray.sh_addr = 0x1401f00;  // starts in the text segment, ends 0x300 bytes past it
  stray.sh_size = 0x400;
  SynthOptions straddling = World::DefaultOptions();
  straddling.extraSections = {stray};
  World straddles(straddling);
  const PatchResult straddleResult = straddles.Patch();
  ExpectRefusal(straddleResult, "output_invalid", "", "an allocated section that leaves its PT_LOAD");
  CHECK(straddleResult.refusal.detail.find("outside every PT_LOAD") != std::string::npos);
}

TEST(ABuildIdNoteThatIsNotTheOneGnuNoteOfTwentyBytesIsRefused) {
  World w;
  const Bytes good = w.elf.bytes;
  Bytes b = good;
  // A name of 8 bytes that begins "GNU\0" is not the name "GNU".
  PutNote(b, std::string("GNU\0\0\0\0\0", 8), NT_GNU_BUILD_ID, 20, StockIdBytes());
  ExpectElfInvalid(PatchBytes(w, b), "no GNU build-ID note", "n_namesz 8");
  // The owner is compared, not just its length.
  b = good;
  PutNote(b, std::string("XYZ\0", 4), NT_GNU_BUILD_ID, 20, StockIdBytes());
  ExpectElfInvalid(PatchBytes(w, b), "no GNU build-ID note", "owner XYZ");
  // A note of another type is not a build ID.
  b = good;
  PutNote(b, std::string("GNU\0", 4), NT_GNU_ABI_TAG, 20, StockIdBytes());
  ExpectElfInvalid(PatchBytes(w, b), "no GNU build-ID note", "type is not NT_GNU_BUILD_ID");
  // The descriptor is 20 bytes: shorter or longer is refused (the bytes after a short one still hold a whole ID).
  b = good;
  PutNote(b, std::string("GNU\0", 4), NT_GNU_BUILD_ID, 16, StockIdBytes());
  ExpectElfInvalid(PatchBytes(w, b), "not 20 bytes", "descriptor of 16 bytes");
  b = good;
  Bytes longer = StockIdBytes();
  longer.insert(longer.end(), {1, 2, 3, 4});
  PutNote(b, std::string("GNU\0", 4), NT_GNU_BUILD_ID, 24, longer);
  ExpectElfInvalid(PatchBytes(w, b), "not 20 bytes", "descriptor of 24 bytes");
  // Two build-ID notes are ambiguous: neither is taken.
  b = good;
  Bytes second(good.begin() + kNoteOffset, good.begin() + kNoteOffset + 36);
  std::memcpy(b.data() + kNoteOffset + 36, second.data(), second.size());
  Elf64_Phdr note = Phdr(b, kPhNote);
  note.p_filesz = note.p_memsz = 72;
  SetPhdr(b, kPhNote, note);
  ExpectElfInvalid(PatchBytes(w, b), "more than one GNU build-ID note", "two notes");
}

TEST(TheEdgesOfAnExecutableSegmentAreInsideItAndTheBytesNextToThemAreNot) {
  SynthOptions o = World::DefaultOptions();
  o.execLoads = {kExecLoadA, kExecLoadB};
  World w(o);
  auto edit = [&](std::uint64_t va) {
    WordPatchRow row;
    row.id = "x.edge";
    row.disasmVa = va + kDisasmBias;
    WordBytes stock{};
    const std::uint32_t word = w.elf.WordAt(va);
    std::memcpy(stock.data(), &word, 4);
    row.edits.push_back({0, stock, {0x1f, 0x20, 0x03, 0xd5}});
    return row;
  };
  // The first word of a segment, its last word, and a segment that is exactly one word long.
  CHECK(w.Patch({edit(kExecLoadA.vaddr)}).ok);
  CHECK(w.Patch({edit(kExecLoadA.vaddr + kExecLoadA.filesz - 4)}).ok);
  CHECK(w.Patch({edit(kExecLoadB.vaddr)}).ok);
  // One word before the first segment and one past its end are in no segment.
  WordPatchRow before = edit(kExecLoadA.vaddr);
  before.disasmVa -= 4;
  const PatchResult beforeResult = w.Patch({before});
  ExpectRefusal(beforeResult, "word_outside_text", "x.edge", "the word before a segment");
  WordPatchRow past = edit(kExecLoadA.vaddr);
  past.disasmVa += kExecLoadA.filesz;
  ExpectRefusal(w.Patch({past}), "word_outside_text", "x.edge", "the word past a segment");
  WordPatchRow pastB = edit(kExecLoadB.vaddr);
  pastB.disasmVa += 4;
  ExpectRefusal(w.Patch({pastB}), "word_outside_text", "x.edge", "the word past a one-word segment");
  // An inline site is three words: it fits in the 0x100-byte segment at its start and at its end, not one word later.
  for (const std::uint64_t va : {kExecLoadA.vaddr, kExecLoadA.vaddr + kExecLoadA.filesz - 12}) {
    World site(o);
    site.rows.push_back(site.Inline("s.far", va, 3));
    site.Rebind();
    const PatchResult r = site.Patch();
    if (!r.ok) std::fprintf(stderr, "  site at %llx: %s\n", static_cast<unsigned long long>(va), r.summary.dump().c_str());
    CHECK(r.ok);
  }
  World tooLate(o);
  {  // the third word is past the segment, so its stock word is not in the file: the row is written by hand
    const std::uint64_t va = kExecLoadA.vaddr + kExecLoadA.filesz - 8;
    Row far{"s.far", "s.far", RowKind::kInlineHook, "handler", 3, 1, {}};
    far.sites[0] = {va + kDisasmBias, 3, {tooLate.elf.WordAt(va), tooLate.elf.WordAt(va + 4), 0}};
    tooLate.rows.push_back(far);
  }
  tooLate.Rebind();
  ExpectRefusal(tooLate.Patch(), "site_outside_text", "s.far", "a site one word past the end of the segment");
  // A segment exactly as long as a site holds it.
  SynthOptions small = o;
  small.execLoads = {ExecLoad{0x1411000, 0x3a00, 12}};
  World tight(small);
  tight.rows.push_back(tight.Inline("s.tight", 0x1411000, 3));
  tight.Rebind();
  CHECK(tight.Patch().ok);
}

TEST(AWritableSegmentThatIsNotALoadIsNotTheOneGrown) {
  SynthOptions o = World::DefaultOptions();
  o.writableTls = true;  // a PT_TLS (R W) that ends past the image end
  World w(o);
  const PatchResult r = w.Patch();
  CHECK(r.ok);
  if (r.ok) CHECK(r.summary["p_memsz_stock"] == "0x5f8");
}

TEST(AnEndSymbolTableIsReadAsTheFileSaysAndNotBeyondIt) {
  World w;
  const Bytes good = w.elf.bytes;
  const std::uint64_t strSize = Shdr(good, kSecDynstr).sh_size;
  const std::uint64_t symSize = Shdr(good, kSecDynsym).sh_size;
  auto appendSymbol = [&](Bytes& b, std::uint32_t nameAt) {
    Elf64_Sym sym{};
    sym.st_name = nameAt;
    sym.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE);
    sym.st_shndx = SHN_ABS;
    sym.st_value = 0x1234;
    WriteAt(b, kDynsymOffset + symSize, sym);
    Elf64_Shdr dynsym = Shdr(b, kSecDynsym);
    dynsym.sh_size += sizeof(Elf64_Sym);
    SetShdr(b, kSecDynsym, dynsym);
  };
  PatchResult r = PatchBytes(w, good);
  CHECK(r.ok);
  CHECK_EQ(r.summary["end_symbols"].size(), static_cast<std::size_t>(6));
  // A .dynsym that links past the section table has no string table: nothing is read from it.
  Bytes b = good;
  Elf64_Shdr dynsym = Shdr(b, kSecDynsym);
  dynsym.sh_link = 100;
  SetShdr(b, kSecDynsym, dynsym);
  r = PatchBytes(w, b);
  CHECK(r.ok);
  CHECK_EQ(r.summary["end_symbols"].size(), static_cast<std::size_t>(0));
  // A name offset past the string table is not read, even where the bytes there spell a wanted name.
  b = good;
  std::memcpy(b.data() + kDynstrOffset + strSize + 1, "_end", 5);
  appendSymbol(b, static_cast<std::uint32_t>(strSize + 1));
  r = PatchBytes(w, b);
  CHECK(r.ok);
  CHECK_EQ(r.summary["end_symbols"].size(), static_cast<std::size_t>(6));
  // The last name of the table, with no terminator inside it, ends at the table's end and not at the next NUL after it.
  b = good;
  std::memcpy(b.data() + kDynstrOffset + strSize, "_endZZ", 7);  // "_end" is the table's last four bytes
  Elf64_Shdr dynstr = Shdr(b, kSecDynstr);
  dynstr.sh_size = strSize + 4;
  SetShdr(b, kSecDynstr, dynstr);
  appendSymbol(b, static_cast<std::uint32_t>(strSize));
  r = PatchBytes(w, b);
  CHECK(r.ok);
  CHECK_EQ(r.summary["end_symbols"].size(), static_cast<std::size_t>(7));
  // A string table that is not in the file (a NOBITS section with a size far past the end) is not read either:
  // the six names are there, in the bytes the section would start at, and still none is reported.
  Elf64_Shdr ghost{};
  ghost.sh_type = SHT_NOBITS;
  ghost.sh_offset = 0x3800;
  ghost.sh_size = 0x10000;
  SynthOptions o = World::DefaultOptions();
  o.extraSections = {ghost};
  World g(o);
  std::memcpy(g.elf.bytes.data() + 0x3800, g.elf.bytes.data() + kDynstrOffset, strSize);
  Elf64_Shdr link = Shdr(g.elf.bytes, kSecDynsym);
  link.sh_link = 8;
  SetShdr(g.elf.bytes, kSecDynsym, link);
  r = g.Patch();
  CHECK(r.ok);
  CHECK_EQ(r.summary["end_symbols"].size(), static_cast<std::size_t>(0));
}

TEST(ASpecWithNoPinnedImageEndPatchesAndRecognisesItsOwnIdentity) {
  World w;
  w.spec.expectedImageEnd.reset();
  PatchResult r = w.Patch();
  CHECK(r.ok);
  if (r.ok) CHECK(r.summary["slot_area"] == "0x1403600");
  // With nothing pinned, the slot area an input would have been patched at is rounded from the input's own end:
  // an input whose note is the ID of that table is the output of this patcher.
  const BuildIdBytes derived = DeriveBuildId(StockId(), CanonicalTable(w.spec.rows, w.spec.rowCount, {}, kSlotArea));
  std::memcpy(w.elf.bytes.data() + kNoteDescOffset, derived.data(), derived.size());
  ExpectRefusal(w.Patch(), "already_patched", "", "the table's own ID, unpinned");
}

TEST(AGnuRelroThatEndsExactlyAtTheSlotAreaLeavesItFree) {
  SynthOptions o = World::DefaultOptions();
  o.relroEnd = kSlotArea;
  World at(o);
  CHECK(at.Patch().ok);
  o.relroEnd = kSlotArea + 1;
  World past(o);
  ExpectRefusal(past.Patch(), "layout_relro", "", "RELRO one byte into the slot area");
}

TEST(AnAddressExactlyAtTheBiasIsInNoSegmentRatherThanBelowTheBias) {
  World w;
  w.rows[0].sites[0].disasmVa = kDisasmBias;
  PatchResult r = w.Patch();
  ExpectRefusal(r, "site_outside_text", "s.join", "site at the bias");
  CHECK(r.refusal.detail.find("not in an executable segment") != std::string::npos);
  WordPatchRow edit;
  edit.id = "x.at_bias";
  edit.disasmVa = kDisasmBias;
  edit.edits.push_back({0, {0, 0, 0, 0}, {0x1f, 0x20, 0x03, 0xd5}});
  World v;
  r = v.Patch({edit});
  ExpectRefusal(r, "word_outside_text", "x.at_bias", "word at the bias");
  CHECK(r.refusal.detail.find("not in an executable segment") != std::string::npos);
}

TEST(ARowWhoseThirdWordCannotBeRelocatedIsRefusedNamingThatWord) {
  World w;
  w.elf.PutWord(kJoinVa + 8, 0x17fffffe);  // b .-8: back into the 12 bytes the stub overwrites
  w.rows[0] = w.Inline("s.join", kJoinVa, 0);
  const PatchResult r = w.Patch();
  ExpectRefusal(r, "row_not_relocatable", "s.join", "a branch back into the window from the third word");
  CHECK(r.refusal.detail.find("word 2") != std::string::npos);
  World first;
  first.elf.PutWord(kJoinVa, 0x14000001);  // b .+4: into the window from the first word, so the detail says word 0
  first.rows[0] = first.Inline("s.join", kJoinVa, 0);
  const PatchResult f = first.Patch();
  ExpectRefusal(f, "row_not_relocatable", "s.join", "a branch back into the window from the first word");
  CHECK(f.refusal.detail.find("word 0") != std::string::npos);
}

TEST(AnOutputBuiltWithExtraRowsIsAlreadyPatchedOnlyForThoseRows) {
  World w;
  std::vector<WordPatchRow> rows;
  Refusal refusal;
  CHECK(ParseExtraRows(Fixture(), w.spec, &rows, &refusal));
  const PatchResult first = w.Patch(rows);
  CHECK(first.ok);
  if (!first.ok) return;
  ExpectRefusal(PatchLibr15(first.output, w.spec, rows, ""), "already_patched", "", "re-patching with the same extra rows");
}

TEST(WordsOfTwoRowsAtTheSameAddressAreOrderedByIdAndRowsByAddress) {
  World w;
  auto bytesAt = [&](std::uint64_t va) {
    WordBytes b{};
    const std::uint32_t v = w.elf.WordAt(va);
    std::memcpy(b.data(), &v, 4);
    return b;
  };
  const WordBytes nop{0x1f, 0x20, 0x03, 0xd5};
  auto row = [&](const char* id, std::uint64_t va, std::uint32_t offset) {
    WordPatchRow r;
    r.id = id;
    r.disasmVa = va + kDisasmBias;
    r.edits.push_back({offset, bytesAt(va + offset), nop});
    return r;
  };
  // Same address, same word: the second row in id order is the one refused, whatever order the file lists them in.
  PatchResult r = w.Patch({row("x.tie_b", kGuardA, 0), row("x.tie_a", kGuardA, 0)});
  ExpectRefusal(r, "word_overlap", "x.tie_b", "equal addresses");
  CHECK(r.refusal.detail.find("x.tie_a") != std::string::npos);
  // Ids against addresses: x.z is lower in memory, x.a is lower by id. Address decides, so x.a is the second one.
  r = w.Patch({row("x.a", kGuardB, 0), row("x.z", kGuardA, 8)});
  ExpectRefusal(r, "word_overlap", "x.a", "address before id");
  CHECK(r.refusal.detail.find("x.z") != std::string::npos);
  // And the report lists the words in address order, then id order.
  r = w.Patch({row("x.a", kGuardB + 0x10, 0), row("x.z", kGuardA, 0)});
  CHECK(r.ok);
  std::vector<std::string> ids;
  for (const Json& e : r.events) {
    if (e["event"] == "libr15_patch_word") ids.push_back(e["id"]);
  }
  CHECK(ids == (std::vector<std::string>{"x.z", "x.a"}));
}

TEST(WordsThatShareOneByteCollideWhetherOrNotTheyAreAligned) {
  World w;
  const std::uint64_t base = SynthElf::FileOffsetOfText(kGuardA);
  auto row = [&](const char* id, std::uint64_t byteOffset) {
    WordPatchRow r;
    r.id = id;
    r.disasmVa = kGuardA + byteOffset + kDisasmBias;
    WordBytes stock{};
    std::memcpy(stock.data(), w.elf.bytes.data() + base + byteOffset, 4);
    r.edits.push_back({0, stock, {0x1f, 0x20, 0x03, 0xd5}});
    return r;
  };
  // [+1, +5) and [+4, +8) share byte +4 (rows built directly: the file reader only produces aligned ones).
  const PatchResult r = w.Patch({row("x.a", 1), row("x.b", 4)});
  ExpectRefusal(r, "word_overlap", "x.b", "unaligned words that share a byte");
  // [+1, +5) and [+5, +9) do not.
  CHECK(w.Patch({row("x.a", 1), row("x.b", 5)}).ok);
}

TEST(AWordNextToAClaimedRangeOnEitherSideIsNotAnOverlap) {
  World w;
  auto edit = [&](std::uint64_t va) {
    WordPatchRow r;
    r.id = "x.next";
    r.disasmVa = va + kDisasmBias;
    WordBytes stock{};
    const std::uint32_t v = w.elf.WordAt(va);
    std::memcpy(stock.data(), &v, 4);
    r.edits.push_back({0, stock, {0x1f, 0x20, 0x03, 0xd5}});
    return r;
  };
  CHECK(w.Patch({edit(kJoinVa - 4)}).ok);   // ends where s.join's stub begins
  CHECK(w.Patch({edit(kJoinVa + 12)}).ok);  // begins where it ends
  ExpectRefusal(w.Patch({edit(kJoinVa + 8)}), "word_overlap", "x.next", "the stub's last word");
  ExpectRefusal(w.Patch({edit(kJoinVa)}), "word_overlap", "x.next", "the stub's first word");
}

// ---- bytes the file's structure and the tool itself own ---------------------------------------------------------------

// The first PT_LOAD is executable and starts at file offset 0, so the ELF header, the program header table and the build-ID
// note are inside the file-backed text; a second executable PT_LOAD over the section header table makes that table
// reachable too (the pinned libr15's tables are not in its text; a different libr15 need not be the same).
constexpr std::uint64_t kShdrLoadVaddr = 0x1412000;

World ProtectedWorld() {
  SynthOptions o = World::DefaultOptions();
  o.execLoads = {ExecLoad{kShdrLoadVaddr, kShdrOffset, 8 * sizeof(Elf64_Shdr)}};
  return World(o);
}

std::uint64_t VaOfFileOffset(std::uint64_t fileOffset) {
  return fileOffset < kShdrOffset ? kLoad0Vaddr + fileOffset : kShdrLoadVaddr + (fileOffset - kShdrOffset);
}

WordPatchRow WordRowAtFileOffset(const World& w, const std::string& id, std::uint64_t fileOffset) {
  WordPatchRow r;
  r.id = id;
  r.disasmVa = VaOfFileOffset(fileOffset) + kDisasmBias;
  WordEdit e;
  std::memcpy(e.stock.data(), w.elf.bytes.data() + fileOffset, 4);
  e.word = e.stock;
  e.word[0] = static_cast<std::uint8_t>(e.word[0] ^ 0xff);
  r.edits.push_back(e);
  return r;
}

// Every byte range the tool writes is its own: the words of the rows (the claims against each other), the p_memsz of the
// grown segment, the sh_size of the section that ends it, and the build ID. The structures around them are the file's.
// An extra row that reaches any of them is refused, naming the row and the region; without that, the later write (the
// derived ID, the grown sizes) would silently replace the row's word while the report said the row's word was written.
TEST(AnExtraRowThatReachesTheElfStructuresOrAByteTheToolWritesItselfIsRefused) {
  World w = ProtectedWorld();
  struct Case {
    const char* what;
    std::uint64_t fileOffset;
    const char* region;
  };
  const std::uint64_t phnum = Ehdr(w.elf.bytes).e_phnum;  // the three of the synthetic file and the executable segment over the section headers
  const std::uint64_t memsz = w.elf.PhdrMemszOffset();
  const std::uint64_t bssSize = w.elf.BssSizeOffset();
  const Case cases[] = {
      {"e_flags", 0x30, "ELF header"},
      {"the last word of the ELF header", sizeof(Elf64_Ehdr) - 4, "ELF header"},
      {"the first word of the program header table", kPhdrOffset, "program header table"},
      {"a field of the first program header (p_align)", kPhdrOffset + offsetof(Elf64_Phdr, p_align), "program header table"},
      {"the last word of the program header table", kPhdrOffset + phnum * sizeof(Elf64_Phdr) - 4, "program header table"},
      {"the first half of the grown segment's p_memsz", memsz, "p_memsz"},
      {"the second half of the grown segment's p_memsz", memsz + 4, "p_memsz"},
      {"the first word of the build-ID note", kNoteOffset, "build-ID note"},
      {"the build-ID descriptor", kNoteDescOffset, "build-ID note"},
      {"the last word of the build-ID note", kNoteDescOffset + 16, "build-ID note"},
      {"the first word of the section header table", kShdrOffset, "section header table"},
      {"a field of a section header (sh_addr of .text)", kShdrOffset + kSecText * sizeof(Elf64_Shdr) + offsetof(Elf64_Shdr, sh_addr), "section header table"},
      {"the last word of the section header table", kShdrOffset + 8 * sizeof(Elf64_Shdr) - 4, "section header table"},
      {"the first half of the .bss section's sh_size", bssSize, "sh_size"},
      {"the second half of the .bss section's sh_size", bssSize + 4, "sh_size"},
  };
  for (const Case& c : cases) {
    const PatchResult r = w.Patch({WordRowAtFileOffset(w, "x.reach", c.fileOffset)});
    if (r.ok || r.refusal.code != "protected_region" || r.refusal.detail.find(c.region) == std::string::npos) {
      std::fprintf(stderr, "  %s: ok=%d code=%s detail=%s (expected protected_region naming %s)\n", c.what, r.ok ? 1 : 0, r.refusal.code.c_str(),
                   r.refusal.detail.c_str(), c.region);
    }
    CHECK(!r.ok);
    CHECK_EQ(r.refusal.code, std::string("protected_region"));
    CHECK_EQ(r.refusal.id, std::string("x.reach"));
    CHECK(r.refusal.detail.find(c.region) != std::string::npos);
    CHECK(r.output.empty());
  }
  // A site of a public row reaches them the same way.
  {
    World pub = ProtectedWorld();
    pub.rows.push_back(pub.Inline("s.in_phdrs", kLoad0Vaddr + kPhdrOffset, 3));
    pub.Rebind();
    const PatchResult r = pub.Patch();
    CHECK(!r.ok);
    CHECK_EQ(r.refusal.code, std::string("protected_region"));
    CHECK_EQ(r.refusal.id, std::string("s.in_phdrs"));
    CHECK(r.refusal.detail.find("program header table") != std::string::npos);
  }
  {  // a three-word window whose first word is outside a region and whose last word is inside it
    World pub = ProtectedWorld();
    pub.rows.push_back(pub.Inline("s.into_note", kLoad0Vaddr + kNoteOffset - 8, 3));
    pub.Rebind();
    const PatchResult r = pub.Patch();
    CHECK(!r.ok);
    CHECK_EQ(r.refusal.code, std::string("protected_region"));
    CHECK_EQ(r.refusal.id, std::string("s.into_note"));
    CHECK(r.refusal.detail.find("build-ID note") != std::string::npos);
  }
  // The words just outside each region are the file's text like any other, and are written.
  const std::uint64_t outside[] = {
      kPhdrOffset + phnum * sizeof(Elf64_Phdr),  // the word after the program header table
      kNoteOffset - 4,                       // the word before the note
      kNoteOffset + 36,                      // the word after the note (12 + 4 + 20 bytes)
  };
  for (const std::uint64_t offset : outside) {
    const PatchResult r = w.Patch({WordRowAtFileOffset(w, "x.beside", offset)});
    if (!r.ok) std::fprintf(stderr, "  file offset 0x%llx: refused: %s %s\n", static_cast<unsigned long long>(offset), r.refusal.code.c_str(), r.refusal.detail.c_str());
    CHECK(r.ok);
  }
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
  CHECK_EQ(rows[0].disasmVa, static_cast<std::uint64_t>(0x1500010));
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
    {"id":"x.a","kind":"word_patch","disasm_va":"0x1500010","words":[{"offset":0,"stock_bytes":"e0031faa","new_bytes":"1f2003d5"}]},
    {"id":"x.b","kind":"word_patch","disasm_va":"0x1500020","words":[{"offset":0,"stock_bytes":")" + secondStockHex + R"(","new_bytes":"1f2003d5"}]}]})";
  const std::string second = R"({
      "rows" : [
         { "words" : [ { "new_bytes" : "1f2003d5", "stock_bytes" : ")" + secondStockHex + R"(", "offset" : 0 } ],
           "disasm_va" : "0x1500020", "kind" : "word_patch", "id" : "x.b" },
         { "words" : [ { "new_bytes" : "1f2003d5", "offset" : 0, "stock_bytes" : "e0031faa" } ],
           "disasm_va" : "0x1500010", "id" : "x.a", "kind" : "word_patch" } ],
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

TEST(TheStockBuildIdTheFileMustNameIsTheSpecsNotAConstant) {
  World w;
  w.spec.stockBuildId = BuildIdBytes{};
  w.spec.stockBuildId.fill(0x11);
  const std::string other(40, '1');
  std::vector<WordPatchRow> rows;
  Refusal refusal;
  std::string named = Fixture();
  CHECK(Replace(&named, kLibR15StockBuildId, other));
  CHECK(Parse(named, w, &rows, &refusal));
  CHECK(!Parse(Fixture(), w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_stock_build_id"));
  CHECK(refusal.detail.find(other) != std::string::npos);
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
      {"disasm_va not a multiple of 4", "\"0x1500010\"", "\"0x1500012\"", "extra_rows_disasm_va", "x.example_guard"},
      {"disasm_va upper case", "\"0x1500010\"", "\"0x1500A10\"", "extra_rows_disasm_va", "x.example_guard"},
      {"disasm_va without 0x", "\"0x1500010\"", "\"1500010\"", "extra_rows_disasm_va", "x.example_guard"},
      {"disasm_va with a capital X", "\"0x1500010\"", "\"0X1500010\"", "extra_rows_disasm_va", "x.example_guard"},
      {"disasm_va with another prefix", "\"0x1500010\"", "\"zz1500010\"", "extra_rows_disasm_va", "x.example_guard"},
      {"disasm_va leading zero", "\"0x1500010\"", "\"0x01500010\"", "extra_rows_disasm_va", "x.example_guard"},
      {"disasm_va is a number", "\"0x1500010\"", "22020112", "extra_rows_disasm_va", "x.example_guard"},
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
      // disasm_va: the prefix alone and one digit too many reach std::stoull unless refused first; "0x0" is a valid
      // spelling (the patcher refuses the address, below the bias); the top of the range keeps room for an offset.
      {"disasm_va is only the prefix", "\"0x1500010\"", "\"0x\"", "extra_rows_disasm_va", "x.example_guard"},
      {"disasm_va has 17 digits", "\"0x1500010\"", "\"0x10000000000000000\"", "extra_rows_disasm_va", "x.example_guard"},
      {"disasm_va with another first character", "\"0x1500010\"", "\"1x1500010\"", "extra_rows_disasm_va", "x.example_guard"},
      {"disasm_va 0x0 parses", "\"0x1500010\"", "\"0x0\"", "word_outside_text", "x.example_guard"},
      {"disasm_va 0xffffffffffffff00", "\"0x1500010\"", "\"0xffffffffffffff00\"", "extra_rows_disasm_va", "x.example_guard"},
      {"disasm_va 0xfffffffffffff000", "\"0x1500010\"", "\"0xfffffffffffff000\"", "extra_rows_disasm_va", "x.example_guard"},
      {"disasm_va 0xffffffffffffeffc is the highest accepted", "\"0x1500010\"", "\"0xffffffffffffeffc\"", "word_outside_text", "x.example_guard"},
      {"stock build id a number", "\"b243509c08ce677aeb95fa348016949b3fc45230\"", "5", "extra_rows_stock_build_id", ""},
      {"id a number", "\"x.example_guard\"", "5", "extra_rows_id", ""},
      {"kind a number", "\"word_patch\"", "5", "extra_rows_kind", "x.example_guard"},
      {"row without an id", "      \"id\": \"x.example_guard\",\n", "", "extra_rows_missing_field", ""},
      {"first offset negative", "\"offset\": 0, \"stock_bytes\": \"e0031faa\"", "\"offset\": -4, \"stock_bytes\": \"e0031faa\"", "extra_rows_offset", "x.example_guard"},
      {"offset 4092 is the last allowed", "\"offset\": 8", "\"offset\": 4092", "word_stock_mismatch", "x.example_guard"},
      {"id with the characters at both ends of the alphabet", "\"x.example_guard\"", "\"x.0z\"", "(accepted)", ""},
      {"empty rows", "\"disasm_va\": \"0x1500010\",", "\"disasm_va\": \"0x1500010\",", "", ""},  // replaced below
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
    return "{\"id\":\"" + id + "\",\"kind\":\"word_patch\",\"disasm_va\":\"" + va + "\",\"words\":[" + w + "]}";
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
  CHECK(refusal.detail.find("public") != std::string::npos);
  // A row that is not an object.
  CHECK(!Parse(head + "[7]}", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_json"));
  CHECK(!Parse(head + "[[]]}", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_json"));
  // A key repeated in a row AFTER the row held nested objects (the words): the scope of the repeat check is the
  // object being read, not the first one.
  const std::string repeated = head + "[{\"id\":\"x.a\",\"kind\":\"word_patch\",\"disasm_va\":\"0x1500100\",\"words\":[" + word(0) + "],\"kind\":\"word_patch\"}]}";
  CHECK(!Parse(repeated, w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_duplicate_key"));
  // The same key in two different rows is fine.
  CHECK(Parse(head + "[" + row("x.a", "0x1500100", 1) + "," + row("x.b", "0x1500200", 1) + "]}", w, &rows, &refusal));
  // Ids: digits, dots and underscores are allowed; 62 characters after the prefix are the limit.
  CHECK(Parse(head + "[" + row("x.a.b_9.c", "0x1500100", 1) + "]}", w, &rows, &refusal));
  CHECK(Parse(head + "[" + row("x.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "0x1500100", 1) + "]}", w, &rows, &refusal));
  // An id equal to the LAST public row's, and the detail names which of the two collisions it is.
  World last;
  last.rows[2].id = "x.last_public";
  CHECK(!Parse(head + "[" + row("x.last_public", "0x1500100", 1) + "]}", last, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_id"));
  CHECK_EQ(refusal.id, std::string("x.last_public"));
  CHECK(refusal.detail.find("public") != std::string::npos);
  CHECK(!Parse(head + "[" + row("x.same", "0x1500100", 1) + "," + row("x.same", "0x1500200", 1) + "]}", w, &rows, &refusal));
  CHECK(refusal.detail.find("twice") != std::string::npos);
  // rows and words must be arrays: an object or a string holding the same content is not accepted as one.
  CHECK(!Parse(head + "{\"k\":" + row("x.a", "0x1500100", 1) + "}}", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_missing_field"));
  CHECK(!Parse(head + "\"x\"}", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_missing_field"));
  const std::string rowHead = head + "[{\"id\":\"x.a\",\"kind\":\"word_patch\",\"disasm_va\":\"0x1500100\",\"words\":";
  CHECK(!Parse(rowHead + "{\"k\":" + word(0) + "}}]}", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_words"));
  CHECK_EQ(refusal.id, std::string("x.a"));
  CHECK(!Parse(rowHead + "\"x\"}]}", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_words"));
  // A repeated key is named in the refusal.
  CHECK(!Parse(R"({"format":"nevr-libr15-extra-rows","version":1,"version":1})", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_duplicate_key"));
  CHECK(refusal.detail.find("\"version\"") != std::string::npos);
  // Not JSON, trailing text, a top-level array.
  CHECK(!Parse("not json", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_json"));
  CHECK(refusal.detail.find("not valid") != std::string::npos);
  CHECK(!Parse(head + "[" + row("x.a", "0x1500100", 1) + "]} trailing", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_json"));
  CHECK(!Parse("[]", w, &rows, &refusal));
  CHECK_EQ(refusal.code, std::string("extra_rows_json"));
  CHECK(refusal.detail.find("not an object") != std::string::npos);
}

TEST(AWordInBytesAnotherRowWritesIsRefusedNamingTheSecondRow) {
  World w;
  // x.over_public sits on s.join's stub (the stock bytes are what the file holds there).
  auto bytesAt = [&](std::uint64_t va) { char b[16]; const std::uint32_t v = w.elf.WordAt(va); std::snprintf(b, sizeof b, "%02x%02x%02x%02x", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24); return std::string(b); };
  const std::string head = R"({"format":"nevr-libr15-extra-rows","version":1,"stock_build_id":"b243509c08ce677aeb95fa348016949b3fc45230","rows":[)";
  std::vector<WordPatchRow> rows;
  Refusal refusal;
  const std::string overPublic = head + R"({"id":"x.over_public","kind":"word_patch","disasm_va":"0x1500200","words":[{"offset":4,"stock_bytes":")" + bytesAt(kJoinVa + 4) +
                                 R"(","new_bytes":"1f2003d5"}]}]})";
  CHECK(Parse(overPublic, w, &rows, &refusal));
  PatchResult r = w.Patch(rows);
  CHECK(!r.ok);
  CHECK_EQ(r.refusal.code, std::string("word_overlap"));
  CHECK_EQ(r.refusal.id, std::string("x.over_public"));
  CHECK(r.refusal.detail.find("s.join") != std::string::npos);
  CHECK(r.refusal.detail.rfind("offset 4: ", 0) == 0);  // the detail names the word's offset in the row first
  // The credits pair's ADD word.
  const std::string overCredits = head + R"({"id":"x.over_credits","kind":"word_patch","disasm_va":"0x1500408","words":[{"offset":0,"stock_bytes":")" + bytesAt(kAdd0) +
                                  R"(","new_bytes":"1f2003d5"}]}]})";
  CHECK(Parse(overCredits, w, &rows, &refusal));
  r = w.Patch(rows);
  CHECK_EQ(r.refusal.code, std::string("word_overlap"));
  CHECK(r.refusal.detail.find("s.credits") != std::string::npos);
  // Another extra row: x.second starts on the fixture's second word.
  const std::string overExtra = head + R"({"id":"x.first","kind":"word_patch","disasm_va":"0x1500010","words":[{"offset":0,"stock_bytes":"e0031faa","new_bytes":"1f2003d5"},{"offset":8,"stock_bytes":"00010054","new_bytes":"1f2003d5"}]},)" +
                                R"({"id":"x.second","kind":"word_patch","disasm_va":"0x1500018","words":[{"offset":0,"stock_bytes":"00010054","new_bytes":"1f2003d5"}]}]})";
  CHECK(Parse(overExtra, w, &rows, &refusal));
  r = w.Patch(rows);
  CHECK_EQ(r.refusal.code, std::string("word_overlap"));
  CHECK_EQ(r.refusal.id, std::string("x.second"));
  CHECK(r.refusal.detail.find("x.first") != std::string::npos);
  // Neighbouring words that do not share a byte are fine.
  const std::string apart = head + R"({"id":"x.first","kind":"word_patch","disasm_va":"0x1500010","words":[{"offset":0,"stock_bytes":"e0031faa","new_bytes":"1f2003d5"}]},)" +
                            R"({"id":"x.next","kind":"word_patch","disasm_va":"0x1500014","words":[{"offset":0,"stock_bytes":")" + bytesAt(0x1400014) + R"(","new_bytes":"1f2003d5"}]}]})";
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
  // No extra-rows file, no digest of one in the refusal (an empty one would read as "a file with no bytes").
  CHECK(!err.contains("extra_rows_sha256"));
  CHECK(!Json::parse(ReadText(report))["summary"].contains("extra_rows_sha256"));
  {  // with a file that parsed, a refusal that comes later carries its digest
    const std::string goodRows = g_fixtures + "/libr15_extra_rows_example.json";
    const std::string fixture = Fixture();
    std::ostringstream so3, se3;
    CHECK_EQ(RunCli({"--stock", stock, "--out", out, "--report", report, "--extra-rows", goodRows}, w.spec, so3, se3), 1);
    const Json later = Json::parse(se3.str());
    CHECK(later["reason"] == "row_words_mismatch");
    CHECK(later["extra_rows_sha256"] == nevr_quest::data::Sha256Hex(fixture.data(), fixture.size()));
  }
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

TEST(ArgumentsThatDoNotNameOneFilePerFlagAreRefusedBeforeAnythingIsWritten) {
  World w;
  const std::string stock = g_scratch + "/arg_stock.so", other = g_scratch + "/arg_other.so", out = g_scratch + "/arg_out.so",
                    out2 = g_scratch + "/arg_out2.so", report = g_scratch + "/arg_report.json", report2 = g_scratch + "/arg_report2.json";
  const std::string rows = g_fixtures + "/libr15_extra_rows_example.json";
  WriteBytes(stock, w.elf.bytes);
  WriteBytes(other, w.elf.bytes);
  auto expectUsage = [&](const std::vector<std::string>& args, const char* what) {
    std::ostringstream so, se;
    const int rc = RunCli(args, w.spec, so, se);
    if (rc != 2) std::fprintf(stderr, "  %s: exit %d, stderr %s\n", what, rc, se.str().c_str());
    CHECK_EQ(rc, 2);
    Json e = OneErrorLine(se, what);
    if (e["status"] != "usage_error") std::fprintf(stderr, "  %s: status %s\n", what, e.dump().c_str());
    CHECK(e["status"] == "usage_error");
    CHECK(so.str().empty());
    for (const std::string& f : {out, out2, report, report2}) {
      CHECK(!Exists(f));
      CHECK(!Exists(f + ".partial"));
    }
  };
  // A flag given twice, both values naming files that exist: refused, not "the last one wins".
  expectUsage({"--stock", stock, "--stock", other, "--out", out, "--report", report}, "--stock twice");
  expectUsage({"--stock", stock, "--out", out, "--out", out2, "--report", report}, "--out twice");
  expectUsage({"--stock", stock, "--out", out, "--report", report, "--report", report2}, "--report twice");
  expectUsage({"--stock", stock, "--out", out, "--report", report, "--extra-rows", rows, "--extra-rows", rows}, "--extra-rows twice");
  // A flag with no value after it, whichever flag ends the line.
  expectUsage({"--out", out, "--report", report, "--stock"}, "--stock without a value");
  expectUsage({"--stock", stock, "--report", report, "--out"}, "--out without a value");
  expectUsage({"--stock", stock, "--out", out, "--report"}, "--report without a value");
  expectUsage({"--stock", stock, "--out", out, "--report", report, "--extra-rows"}, "--extra-rows without a value");
  // Each of the three required flags missing: a usage error, before the stock is read or a report is opened.
  expectUsage({"--out", out, "--report", report}, "no --stock");
  expectUsage({"--stock", stock, "--report", report}, "no --out");
  expectUsage({"--stock", stock, "--out", out}, "no --report");
  for (const std::string& f : {stock, other}) std::remove(f.c_str());
}

TEST(TheStockFileIsNeverTheOutputAndAFailedWriteLeavesAnExistingOutputAlone) {
  World w;
  const std::string stock = g_scratch + "/same_stock.so", out = g_scratch + "/same_out.so", report = g_scratch + "/same_report.json";
  WriteBytes(stock, w.elf.bytes);
  const std::string before = ReadText(stock);
  {  // --out is the --stock file: refused; the stock is byte-identical afterwards and nothing else appeared
    std::ostringstream so, se;
    CHECK_EQ(RunCli({"--stock", stock, "--out", stock, "--report", report}, w.spec, so, se), 2);
    Json e = OneErrorLine(se, "out is stock");
    CHECK(e["status"] == "usage_error");
    CHECK(e["detail"].get<std::string>().find("--out must not be the --stock file") != std::string::npos);
    CHECK(ReadText(stock) == before);
    CHECK(!Exists(stock + ".partial"));
    CHECK(!Exists(report));
  }
  {  // the output is written beside itself and renamed: with the beside-name taken, an existing --out is untouched
    WriteBytes(out, {1, 2, 3});
    CHECK_EQ(mkdir((out + ".partial").c_str(), 0755), 0);
    std::ostringstream so, se;
    CHECK_EQ(RunCli({"--stock", stock, "--out", out, "--report", report}, w.spec, so, se), 2);
    Json e = OneErrorLine(se, "partial taken");
    CHECK(e["status"] == "io_error");
    CHECK(e["detail"].get<std::string>().find("--out") != std::string::npos);
    CHECK(ReadText(out) == std::string("\x01\x02\x03"));
    CHECK_EQ(rmdir((out + ".partial").c_str()), 0);
  }
  {  // --out in a directory that is not there: exit 2, and the report says the write failed (it does not say patched)
    std::remove(report.c_str());
    std::ostringstream so, se;
    CHECK_EQ(RunCli({"--stock", stock, "--out", g_scratch + "/no_such_dir/out.so", "--report", report}, w.spec, so, se), 2);
    Json e = OneErrorLine(se, "out in a missing directory");
    CHECK(e["status"] == "io_error");
    CHECK(e["detail"].get<std::string>().find("--out") != std::string::npos);
    CHECK(so.str().empty());
    CHECK(Json::parse(ReadText(report), nullptr, false)["summary"]["status"] == "io_error");
  }
  {  // --report in a directory that is not there: exit 2 before any output
    std::remove(out.c_str());
    std::ostringstream so, se;
    CHECK_EQ(RunCli({"--stock", stock, "--out", out, "--report", g_scratch + "/no_such_dir/report.json"}, w.spec, so, se), 2);
    Json e = OneErrorLine(se, "report in a missing directory");
    CHECK(e["status"] == "io_error");
    CHECK(e["detail"].get<std::string>().find("--report") != std::string::npos);
    CHECK(!Exists(out));
  }
  for (const std::string& f : {stock, out, report}) std::remove(f.c_str());
}

std::vector<std::uint8_t> FixtureBytes() {
  const std::string text = Fixture();
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

// A file on disk that must come out of a refused run as it went in: its sha-256 before and after are in the failure
// line, so a destroyed input says so. It is put back after the report, so that the next case starts from the same bytes.
bool Unchanged(const std::string& path, const std::string& before, const std::string& label) {
  const std::string now = ReadText(path);
  if (now == before) return true;
  std::fprintf(stderr, "  %s: %s changed: sha256 before=%s after=%s\n", label.c_str(), path.c_str(),
               nevr_quest::data::Sha256Hex(before.data(), before.size()).c_str(), nevr_quest::data::Sha256Hex(now.data(), now.size()).c_str());
  std::ofstream(path, std::ios::binary | std::ios::trunc) << before;
  return false;
}

// The paths the aliasing cases make, removed at the end (the links first, the directories last).
struct AliasTree {
  std::string root, real, linkDir;
  std::vector<std::string> made;
  explicit AliasTree(const std::string& name) {
    // Absolute: a symlink's target is read relative to the link's own directory, and the scratch directory may be given
    // relative to the working directory (the recipe does).
    char* resolved = realpath(g_scratch.c_str(), nullptr);
    CHECK(resolved != nullptr);
    root = std::string(resolved != nullptr ? resolved : g_scratch.c_str()) + "/" + name;
    std::free(resolved);
    real = root + "/real";
    linkDir = root + "/linkdir";
    CHECK_EQ(mkdir(root.c_str(), 0755), 0);
    CHECK_EQ(mkdir(real.c_str(), 0755), 0);
    CHECK_EQ(symlink(real.c_str(), linkDir.c_str()), 0);  // a directory that is another name for real/
  }
  std::string File(const std::string& name, const std::vector<std::uint8_t>& bytes) {
    const std::string path = real + "/" + name;
    WriteBytes(path, bytes);
    made.push_back(path);
    return path;
  }
  void Track(const std::string& path) { made.push_back(path); }
  ~AliasTree() {
    for (auto it = made.rbegin(); it != made.rend(); ++it) std::remove(it->c_str());
    std::remove(linkDir.c_str());
    rmdir(real.c_str());
    rmdir(root.c_str());
  }
};

// One refused spelling: exit 2, one JSON line on stderr naming the rule, nothing written, every listed input as it was.
void ExpectAliasRefused(const World& w, const char* label, const std::vector<std::string>& args, const char* detail,
                        const std::vector<std::pair<std::string, std::string>>& inputs, const std::vector<std::string>& mustNotAppear) {
  std::ostringstream so, se;
  const int rc = RunCli(args, w.spec, so, se);
  if (rc != 2) std::fprintf(stderr, "  %s: exit %d, not 2\n", label, rc);
  CHECK_EQ(rc, 2);
  CHECK(so.str().empty());
  Json e = OneErrorLine(se, label);
  CHECK(e["status"] == "usage_error");
  const std::string got = e["detail"].is_string() ? e["detail"].get<std::string>() : std::string();
  if (got.find(detail) == std::string::npos) std::fprintf(stderr, "  %s: detail \"%s\" does not say \"%s\"\n", label, got.c_str(), detail);
  CHECK(got.find(detail) != std::string::npos);
  if (std::string(label).rfind("the temporary file", 0) == 0) CHECK(got.find(".partial") != std::string::npos);  // it names the temporary file
  for (const auto& in : inputs) CHECK(Unchanged(in.first, in.second, label));
  for (const std::string& path : mustNotAppear) {
    if (Exists(path)) {
      std::fprintf(stderr, "  %s: %s was created\n", label, path.c_str());
      std::remove(path.c_str());
    }
    CHECK(!Exists(path));
  }
}

TEST(AnOutputThatIsTheStockByAnyNameIsRefusedBeforeAnythingIsReadOrWritten) {
  World w;
  const char* kOutIsStock = "--out must not be the --stock file";
  AliasTree t("alias_out");
  const std::string stock = t.File("stock.so", w.elf.bytes);
  const std::string before = ReadText(stock);
  const std::string report = t.real + "/report.json";
  t.Track(report);
  t.Track(stock + ".partial");
  const std::vector<std::pair<std::string, std::string>> keep = {{stock, before}};
  const std::vector<std::string> none = {report, stock + ".partial"};
  auto run = [&](const char* label, const std::string& out) {
    ExpectAliasRefused(w, label, {"--stock", stock, "--out", out, "--report", report}, kOutIsStock, keep, none);
  };
  run("another spelling: a dot component", t.real + "/./stock.so");
  run("another spelling: a dot-dot component", t.real + "/../real/stock.so");
  run("another spelling: a doubled slash", t.real + "//stock.so");
  run("a path through a symlinked directory", t.linkDir + "/stock.so");
  {  // a symlink to the stock
    const std::string link = t.real + "/to_stock.so";
    CHECK_EQ(symlink(stock.c_str(), link.c_str()), 0);
    t.Track(link);
    run("a symlink to the stock", link);
    CHECK(Exists(link));  // refused, so the link is still there
  }
  {  // a hard link to the stock: the same device and inode under another name
    const std::string hard = t.real + "/hard_to_stock.so";
    CHECK_EQ(link(stock.c_str(), hard.c_str()), 0);
    t.Track(hard);
    run("a hard link to the stock", hard);
    CHECK(Unchanged(hard, before, "the hard link"));
  }
  {  // the stock is itself the symlink and --out names its target
    const std::string target = t.File("target.so", w.elf.bytes);
    const std::string viaLink = t.real + "/stock_link.so";
    CHECK_EQ(symlink(target.c_str(), viaLink.c_str()), 0);
    t.Track(viaLink);
    const std::string targetBefore = ReadText(target);
    ExpectAliasRefused(w, "the stock is a symlink to --out", {"--stock", viaLink, "--out", target, "--report", report}, kOutIsStock,
                       {{target, targetBefore}}, none);
  }
  {  // the same spelling of a path that cannot be resolved (it runs through a symlink loop): still the same file
    const std::string loop = t.real + "/loop";
    CHECK_EQ(symlink(loop.c_str(), loop.c_str()), 0);
    t.Track(loop);
    const std::string unresolvable = loop + "/stock.so";
    ExpectAliasRefused(w, "the same spelling of a path that cannot be resolved", {"--stock", unresolvable, "--out", unresolvable, "--report", report},
                       kOutIsStock, keep, none);
  }
  {  // the temporary file the output is written to is the stock
    const std::string stockPartial = t.File("tmp_stock.so.partial", w.elf.bytes);
    const std::string partialBefore = ReadText(stockPartial);
    ExpectAliasRefused(w, "the temporary file beside --out is the stock", {"--stock", stockPartial, "--out", t.real + "/tmp_stock.so", "--report", report},
                       "must not be the --stock file", {{stockPartial, partialBefore}}, {report, t.real + "/tmp_stock.so"});
  }
}

TEST(AReportOrAnExtraRowsFileThatWouldBeOverwrittenByAnotherNameIsRefused) {
  World w;
  AliasTree t("alias_other");
  const std::string stock = t.File("stock.so", w.elf.bytes);
  const std::string stockBefore = ReadText(stock);
  const std::string rows = t.File("rows.json", FixtureBytes());
  const std::string rowsBefore = ReadText(rows);
  const std::string out = t.real + "/out.so", report = t.real + "/report.json";
  t.Track(out);
  t.Track(report);
  t.Track(out + ".partial");
  // --report is the stock: it would be truncated after the stock was read.
  ExpectAliasRefused(w, "--report is the stock", {"--stock", stock, "--out", out, "--report", t.real + "/./stock.so"}, "--report must not be the --stock file",
                     {{stock, stockBefore}}, {out, out + ".partial"});
  // --report is --out, neither exists yet: only a path comparison can tell.
  ExpectAliasRefused(w, "--report is --out (neither exists)", {"--stock", stock, "--out", out, "--report", t.real + "/./out.so"},
                     "--report must not be the --out file", {{stock, stockBefore}}, {out, out + ".partial"});
  ExpectAliasRefused(w, "--report is --out through a symlinked directory (neither exists)", {"--stock", stock, "--out", out, "--report", t.linkDir + "/out.so"},
                     "--report must not be the --out file", {{stock, stockBefore}}, {out, out + ".partial"});
  {  // --report is --out and both exist, as a hard link
    WriteBytes(out, {9, 8, 7});
    const std::string hard = t.real + "/report_hard.json";
    CHECK_EQ(link(out.c_str(), hard.c_str()), 0);
    t.Track(hard);
    ExpectAliasRefused(w, "--report is a hard link to --out", {"--stock", stock, "--out", out, "--report", hard}, "--report must not be the --out file",
                       {{stock, stockBefore}, {out, std::string("\x09\x08\x07")}}, {out + ".partial"});
    std::remove(hard.c_str());
    std::remove(out.c_str());
  }
  // --extra-rows is --out: the rows file would be replaced by the patched ELF.
  ExpectAliasRefused(w, "--extra-rows is --out", {"--stock", stock, "--out", t.real + "/./rows.json", "--report", report, "--extra-rows", rows},
                     "--extra-rows must not be the --out file", {{stock, stockBefore}, {rows, rowsBefore}}, {report, rows + ".partial"});
  // --extra-rows is --report: the rows file would be truncated by the report.
  ExpectAliasRefused(w, "--extra-rows is --report", {"--stock", stock, "--out", out, "--report", t.linkDir + "/rows.json", "--extra-rows", rows},
                     "--extra-rows must not be the --report file", {{stock, stockBefore}, {rows, rowsBefore}}, {out, out + ".partial"});
  // The temporary file beside --out is the report, or the rows file.
  ExpectAliasRefused(w, "the temporary file beside --out is --report", {"--stock", stock, "--out", out, "--report", out + ".partial"},
                     "must not be the --report file", {{stock, stockBefore}}, {out, out + ".partial"});
  const std::string tmpRows = t.File("rows_tmp.json.partial", FixtureBytes());
  ExpectAliasRefused(w, "the temporary file beside --out is --extra-rows", {"--stock", stock, "--out", t.real + "/rows_tmp.json", "--report", report, "--extra-rows", tmpRows},
                     "must not be the --extra-rows file", {{stock, stockBefore}, {tmpRows, rowsBefore}}, {report, t.real + "/rows_tmp.json"});
}

TEST(AnOutputThatIsNotTheStockStillWorksWhateverItsSpelling) {
  World w;
  AliasTree t("alias_ok");
  const std::string stock = t.File("stock.so", w.elf.bytes);
  const std::string before = ReadText(stock);
  const std::string out = t.real + "/out.so", report = t.real + "/report.json";
  t.Track(out);
  t.Track(report);
  t.Track(out + ".partial");
  {  // an --out that does not exist yet, reached through dots and a symlinked directory
    std::ostringstream so, se;
    CHECK_EQ(RunCli({"--stock", t.real + "/./stock.so", "--out", t.linkDir + "/../real/./out.so", "--report", report}, w.spec, so, se), 0);
    CHECK(se.str().empty());
    CHECK(Exists(out));
    CHECK(Unchanged(stock, before, "the stock after a good run"));
    CHECK(ReadText(out) != before);
  }
  {  // an --out that exists and is another file: replaced
    std::ostringstream so, se;
    CHECK_EQ(RunCli({"--stock", stock, "--out", out, "--report", report}, w.spec, so, se), 0);
    CHECK(se.str().empty());
    CHECK(Unchanged(stock, before, "the stock after a second good run"));
  }
  {  // names that only share a prefix are different files
    std::ostringstream so, se;
    CHECK_EQ(RunCli({"--stock", stock, "--out", t.real + "/stock.so2", "--report", t.real + "/stock.so.json"}, w.spec, so, se), 0);
    CHECK(Unchanged(stock, before, "the stock after a run with similar names"));
    std::remove((t.real + "/stock.so2").c_str());
    std::remove((t.real + "/stock.so.json").c_str());
  }
}

std::string ShaOf(const std::string& bytes) { return nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()); }

bool IsSymlink(const std::string& path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

// The output is written to <out>.partial first. A name that is already there, a link included, is not ours to open: the
// run stops before anything is written or read, and what the name leads to is as it was.
TEST(ATemporaryFileThatAlreadyExistsIsNeverFollowedOrWrittenThrough) {
  World w;
  AliasTree t("partial_links");
  const std::string stock = t.File("stock.so", w.elf.bytes);
  const std::string precious = t.File("precious.txt", std::vector<std::uint8_t>{'p', 'r', 'e', 'c', 'i', 'o', 'u', 's'});
  const std::string out = t.real + "/o.so", report = t.real + "/r.json", partial = out + ".partial";
  t.Track(out);
  t.Track(report);
  t.Track(partial);
  const std::string preciousBefore = ReadText(precious);
  auto run = [&](const char* label, int expectedExit) {
    std::ostringstream so, se;
    const int rc = RunCli({"--stock", stock, "--out", out, "--report", report}, w.spec, so, se);
    if (rc != expectedExit) std::fprintf(stderr, "  %s: exit %d, not %d; stderr %s\n", label, rc, expectedExit, se.str().c_str());
    CHECK_EQ(rc, expectedExit);
    CHECK(so.str().empty());
    const Json e = OneErrorLine(se, label);
    const std::string detail = e.value("detail", std::string());
    if (detail.find(".partial") == std::string::npos) std::fprintf(stderr, "  %s: detail \"%s\" does not name the temporary file\n", label, detail.c_str());
    CHECK(detail.find(".partial") != std::string::npos);
    CHECK(!Exists(out));
    CHECK(!Exists(report));  // the run stopped before it wrote anything
  };
  {  // a link to a file that exists: what it leads to keeps its bytes, and the link is still there
    CHECK_EQ(symlink(precious.c_str(), partial.c_str()), 0);
    run("a temporary-file name that is a link to an existing file", 2);
    CHECK(Unchanged(precious, preciousBefore, "the file the link leads to"));
    CHECK(IsSymlink(partial));
    CHECK_EQ(unlink(partial.c_str()), 0);
  }
  {  // a link to a path that does not exist yet and is the report: the report is not created through it
    CHECK_EQ(symlink(report.c_str(), partial.c_str()), 0);
    run("a temporary-file name that is a dangling link to the report", 2);
    CHECK(!Exists(report));
    CHECK(IsSymlink(partial));
    CHECK_EQ(unlink(partial.c_str()), 0);
  }
  {  // a link to a path that does not exist yet and is nothing else: it is not created either
    const std::string victim = t.real + "/victim.txt";
    t.Track(victim);
    CHECK_EQ(symlink(victim.c_str(), partial.c_str()), 0);
    run("a temporary-file name that is a dangling link", 2);
    CHECK(!Exists(victim));
    CHECK(IsSymlink(partial));
    CHECK_EQ(unlink(partial.c_str()), 0);
  }
  {  // a file left by an earlier run: refused, not truncated
    WriteBytes(partial, {7, 7, 7});
    run("a temporary file left behind", 2);
    CHECK(ReadText(partial) == std::string("\x07\x07\x07"));
    CHECK_EQ(unlink(partial.c_str()), 0);
  }
}

// The same through the function itself, which the command line's early look does not stand in front of: the file is
// created with O_EXCL and O_NOFOLLOW, so a link that appears after the look is refused too.
TEST(TheOutputFileIsCreatedExclusivelyAndNeverThroughALink) {
  AliasTree t("write_output");
  const std::string precious = t.File("precious.txt", std::vector<std::uint8_t>{'p', 'r', 'e', 'c', 'i', 'o', 'u', 's'});
  const std::string out = t.real + "/direct.so", partial = out + ".partial", victim = t.real + "/victim.txt";
  t.Track(out);
  t.Track(partial);
  t.Track(victim);
  const std::vector<std::uint8_t> bytes = {1, 2, 3, 4, 5, 6, 7, 8};
  const std::string text(bytes.begin(), bytes.end());
  const std::string preciousBefore = ReadText(precious);
  std::string why;
  CHECK_EQ(symlink(precious.c_str(), partial.c_str()), 0);
  CHECK(!WriteOutputFile(out, bytes, ShaOf(text), &why));
  CHECK(why.find(".partial") != std::string::npos);
  CHECK(why.find("remove it") != std::string::npos);  // it says what to do about a name that is taken
  CHECK(Unchanged(precious, preciousBefore, "the file the link leads to"));
  CHECK(!Exists(out));
  CHECK(IsSymlink(partial));  // not ours, so not removed
  CHECK_EQ(unlink(partial.c_str()), 0);
  why.clear();
  CHECK_EQ(symlink(victim.c_str(), partial.c_str()), 0);
  CHECK(!WriteOutputFile(out, bytes, ShaOf(text), &why));
  CHECK(!Exists(victim));
  CHECK(!Exists(out));
  CHECK(IsSymlink(partial));
  CHECK_EQ(unlink(partial.c_str()), 0);
  why.clear();
  WriteBytes(partial, {9});
  CHECK(!WriteOutputFile(out, bytes, ShaOf(text), &why));
  CHECK(ReadText(partial) == std::string("\x09"));
  CHECK_EQ(unlink(partial.c_str()), 0);
  // With the name free the file is written, is the bytes, and leaves no temporary file.
  why.clear();
  CHECK(WriteOutputFile(out, bytes, ShaOf(text), &why));
  CHECK(why.empty());
  CHECK(ReadText(out) == text);
  CHECK(!Exists(partial));
  {  // the mode an ordinary create gives (0666 less the umask), not the 0600 of the exclusive create
    const mode_t mask = umask(0);
    umask(mask);
    struct stat st;
    CHECK_EQ(stat(out.c_str(), &st), 0);
    CHECK_EQ(static_cast<unsigned>(st.st_mode & 0777), static_cast<unsigned>(0666 & ~mask));
  }
  // A file that does not read back as the validated bytes never replaces the output: the output keeps what it held, and
  // the temporary file is gone.
  why.clear();
  WriteBytes(out, {0x0a, 0x0b});
  CHECK(!WriteOutputFile(out, bytes, ShaOf("not these bytes"), &why));
  CHECK(why.find("read back") != std::string::npos);
  CHECK(ReadText(out) == std::string("\x0a\x0b"));
  CHECK(!Exists(partial));
}

// An alias is judged by what the open reaches, a link to a path that is not there yet included.
TEST(AReportThatIsADanglingLinkToTheOutputIsTheOutput) {
  World w;
  AliasTree t("dangling_report");
  const std::string stock = t.File("stock.so", w.elf.bytes);
  const std::string stockBefore = ReadText(stock);
  const std::string out = t.real + "/out.so", link = t.real + "/report_link.json";
  t.Track(out);
  t.Track(out + ".partial");
  t.Track(link);
  CHECK_EQ(symlink(out.c_str(), link.c_str()), 0);  // out.so does not exist: the link dangles
  ExpectAliasRefused(w, "--report is a dangling link to --out", {"--stock", stock, "--out", out, "--report", link}, "--report must not be the --out file",
                     {{stock, stockBefore}}, {out, out + ".partial"});
  // The same link spelled through a symlinked directory.
  ExpectAliasRefused(w, "--report is a dangling link to --out, through a symlinked directory",
                     {"--stock", stock, "--out", t.linkDir + "/out.so", "--report", link}, "--report must not be the --out file", {{stock, stockBefore}},
                     {out, out + ".partial"});
}

// The report is the account of what is on disk. A run whose --out could not be put in place says so in the report: it never
// leaves one that says "patched", not even the one an earlier good run wrote to the same path.
TEST(AFailedOutputWriteNeverLeavesAReportThatSaysPatched) {
  World w;
  AliasTree t("failed_out");
  const std::string stock = t.File("stock.so", w.elf.bytes);
  const std::string stockBefore = ReadText(stock);
  const std::string report = t.real + "/report.json", goodOut = t.real + "/good.so", outDir = t.real + "/out_is_a_directory";
  t.Track(report);
  t.Track(goodOut);
  t.Track(outDir);
  t.Track(outDir + ".partial");
  CHECK_EQ(mkdir(outDir.c_str(), 0755), 0);
  auto statusOf = [&]() {
    const Json r = Json::parse(ReadText(report), nullptr, /*allow_exceptions=*/false);
    return r.is_object() && r.contains("summary") && r["summary"].contains("status") ? r["summary"]["status"].get<std::string>() : std::string("<no report>");
  };
  auto fails = [&](const char* label, const std::string& out) {
    // A good run first, so that a report saying "patched" is on disk when the failing run starts.
    std::ostringstream so0, se0;
    CHECK_EQ(RunCli({"--stock", stock, "--out", goodOut, "--report", report}, w.spec, so0, se0), 0);
    CHECK_EQ(statusOf(), std::string("patched"));
    std::ostringstream so, se;
    const int rc = RunCli({"--stock", stock, "--out", out, "--report", report}, w.spec, so, se);
    if (rc != 2) std::fprintf(stderr, "  %s: exit %d, not 2; stderr %s\n", label, rc, se.str().c_str());
    CHECK_EQ(rc, 2);
    CHECK(so.str().empty());
    const Json e = OneErrorLine(se, label);
    CHECK(e.value("status", std::string()) == "io_error");
    CHECK(e.value("detail", std::string()).find("--out") != std::string::npos);
    const std::string status = statusOf();
    if (status != "io_error") std::fprintf(stderr, "  %s: the report's summary status is \"%s\", not io_error\n", label, status.c_str());
    CHECK(status != "patched");
    CHECK_EQ(status, std::string("io_error"));
    const Json r = Json::parse(ReadText(report), nullptr, false);
    CHECK(r["summary"].value("detail", std::string()).find("--out") != std::string::npos);
    CHECK(Unchanged(stock, stockBefore, label));
    CHECK(!Exists(out + ".partial"));  // the temporary file is gone
  };
  fails("--out is an existing directory", outDir);
  fails("--out is the stock file with a slash after it", stock + "/");
  fails("--out is in a directory that is not there", t.real + "/no_such_dir/out.so");
  {  // the same bytes and the same status after a good run: the report is the good run's again
    std::ostringstream so, se;
    CHECK_EQ(RunCli({"--stock", stock, "--out", goodOut, "--report", report}, w.spec, so, se), 0);
    CHECK_EQ(statusOf(), std::string("patched"));
  }
}

// A run that dies while --out is being written leaves the report it started with, which says the run did not finish.
// The child's file size limit stops it (SIGXFSZ) on the first write past 8000 bytes: the report written before --out is
// small, the output is 16 KB.
TEST(AReportDoesNotSayPatchedWhileTheOutputIsStillBeingWritten) {
  World w;
  AliasTree t("killed_run");
  const std::string stock = t.File("stock.so", w.elf.bytes);
  const std::string out = t.real + "/out.so", report = t.real + "/report.json", partial = out + ".partial";
  t.Track(out);
  t.Track(report);
  t.Track(partial);
  const pid_t pid = fork();
  if (pid == 0) {
    struct rlimit core = {0, 0};
    struct rlimit fsize = {8000, 8000};
    if (setrlimit(RLIMIT_CORE, &core) != 0 || setrlimit(RLIMIT_FSIZE, &fsize) != 0) _exit(127);
    std::ostringstream so, se;
    RunCli({"--stock", stock, "--out", out, "--report", report}, w.spec, so, se);
    _exit(0);  // not reached: the write past the limit ends the process
  }
  int status = 0;
  CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
  CHECK(WIFSIGNALED(status));
  CHECK_EQ(WTERMSIG(status), SIGXFSZ);
  const Json r = Json::parse(ReadText(report), nullptr, /*allow_exceptions=*/false);
  CHECK(r.is_object() && r.contains("summary"));
  const std::string reported = r.is_object() && r.contains("summary") ? r["summary"].value("status", std::string()) : std::string("<no report>");
  if (reported != "incomplete") std::fprintf(stderr, "  the report of the killed run says \"%s\"\n", reported.c_str());
  CHECK_EQ(reported, std::string("incomplete"));
  CHECK(r.is_object() && r.contains("events") && r["events"].is_array() && r["events"].empty());  // no event of a patch that is not there
  CHECK(!Exists(out));
}

// A --report that cannot take the record is found before --out is touched.
TEST(AReportThatCannotBeWrittenIsFoundBeforeTheOutputIsWritten) {
  World w;
  AliasTree t("full_report");
  const std::string stock = t.File("stock.so", w.elf.bytes);
  const std::string out = t.real + "/out.so";
  t.Track(out);
  t.Track(out + ".partial");
  std::ostringstream so, se;
  CHECK_EQ(RunCli({"--stock", stock, "--out", out, "--report", "/dev/full"}, w.spec, so, se), 2);
  CHECK(OneErrorLine(se, "report on a full device").value("detail", std::string()).find("--report") != std::string::npos);
  CHECK(!Exists(out));
  CHECK(!Exists(out + ".partial"));
}

TEST(AnExtraRowsFileWithoutATrailingNewlineIsTheSameFile) {
  World w;
  const std::string stock = g_scratch + "/nl_stock.so", out = g_scratch + "/nl_out.so", report = g_scratch + "/nl_report.json";
  const std::string rows = g_scratch + "/nl_rows.json";
  WriteBytes(stock, w.elf.bytes);
  std::string text = Fixture();
  CHECK(!text.empty() && text.back() == '\n');
  text.pop_back();
  CHECK(!text.empty() && text.back() == '}');
  std::ofstream(rows, std::ios::binary | std::ios::trunc) << text;
  std::ostringstream so, se;
  CHECK_EQ(RunCli({"--stock", stock, "--out", out, "--report", report, "--extra-rows", rows}, w.spec, so, se), 0);
  CHECK(se.str().empty());
  const Json rep = Json::parse(ReadText(report), nullptr, false);
  CHECK(!rep.is_discarded());
  if (!rep.is_discarded()) {
    CHECK_EQ(rep["summary"]["extra_rows"].get<int>(), 1);
    CHECK(rep["summary"]["extra_rows_sha256"] == nevr_quest::data::Sha256Hex(text.data(), text.size()));
  }
  for (const std::string& f : {stock, out, report, rows}) std::remove(f.c_str());
}

// ReadFile reads in 64 KiB chunks: a file whose last chunk is one byte, and a file of several chunks, arrive whole.
TEST(AnExtraRowsFileLongerThanOneReadChunkIsReadWhole) {
  World w;
  const std::string stock = g_scratch + "/chunk_stock.so", out = g_scratch + "/chunk_out.so", report = g_scratch + "/chunk_report.json";
  const std::string rows = g_scratch + "/chunk_rows.json";
  WriteBytes(stock, w.elf.bytes);
  std::string body = Fixture();
  while (!body.empty() && (body.back() == '\n' || body.back() == ' ')) body.pop_back();
  CHECK(!body.empty() && body.back() == '}');
  for (const std::size_t total : {static_cast<std::size_t>(65537), static_cast<std::size_t>(65536), static_cast<std::size_t>(3 * 65536 + 1)}) {
    // Leading spaces make the length exact and leave the last byte, the closing brace, the byte a short read would lose.
    const std::string text = std::string(total - body.size(), ' ') + body;
    CHECK_EQ(text.size(), total);
    std::ofstream(rows, std::ios::binary | std::ios::trunc) << text;
    std::remove(out.c_str());
    std::ostringstream so, se;
    CHECK_EQ(RunCli({"--stock", stock, "--out", out, "--report", report, "--extra-rows", rows}, w.spec, so, se), 0);
    CHECK(se.str().empty());
    const Json rep = Json::parse(ReadText(report), nullptr, false);
    CHECK(!rep.is_discarded());
    if (!rep.is_discarded()) CHECK(rep["summary"]["extra_rows_sha256"] == nevr_quest::data::Sha256Hex(text.data(), text.size()));
  }
  for (const std::string& f : {stock, out, report, rows}) std::remove(f.c_str());
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
      {"disasm_va is only the prefix", "\"0x1500010\"", "\"0x\"", "extra_rows_disasm_va"},
      {"disasm_va has 17 digits", "\"0x1500010\"", "\"0x10000000000000000\"", "extra_rows_disasm_va"},
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

// ---- the input's own references to the slot area (#1737, row C8) ----------------------------------------------------------

constexpr std::uint64_t kRefAt = 0x1400600;  // free text: no row's word and no fixture word
std::uint32_t AddImmLsl12(unsigned rd, unsigned rn, unsigned imm12) { return AddImm(rd, rn, imm12) | (1u << 22); }
std::uint32_t Adr(std::uint64_t pc, std::uint64_t target, unsigned rd) {
  const std::uint32_t d = static_cast<std::uint32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc));
  return 0x10000000u | ((d & 3u) << 29) | (((d >> 2) & 0x7FFFFu) << 5) | rd;
}
std::uint32_t StrW(unsigned rt, unsigned rn, unsigned byteOffset) { return 0xB9000000u | ((byteOffset / 4) << 10) | (rn << 5) | rt; }
std::uint32_t Ldur(unsigned rt, unsigned rn, int imm9) { return 0xF8400000u | ((static_cast<std::uint32_t>(imm9) & 0x1FFu) << 12) | (rn << 5) | rt; }
std::uint32_t LdpX(unsigned rt, unsigned rt2, unsigned rn, int byteOffset) {
  return 0xA9400000u | ((static_cast<std::uint32_t>(byteOffset / 8) & 0x7Fu) << 15) | (rt2 << 10) | (rn << 5) | rt;
}
std::uint32_t LdrX(unsigned rt, unsigned rn, std::uint64_t byteOffset) {
  std::uint32_t l = 0;
  CHECK(EncodeLdrX(rt, rn, byteOffset, &l));
  return l;
}
constexpr std::uint32_t kMovX10X9 = 0xAA0903EAu;  // orr x10, xzr, x9
constexpr std::uint32_t kBranchPlus4 = 0x14000001u;
void PlantAdrp(World& w, std::uint64_t at, std::uint64_t target, unsigned rd) {
  std::uint32_t a = 0;
  CHECK(EncodeAdrp(at, target, rd, &a));
  w.elf.PutWord(at, a);
}
// The word at a vaddr of an output image.
std::uint32_t WordOf(const std::vector<std::uint8_t>& image, std::uint64_t vaddr) {
  std::uint32_t v = 0;
  std::memcpy(&v, image.data() + SynthElf::FileOffsetOfText(vaddr), 4);
  return v;
}

// Each form the scan reads, planted in the text of a fresh input. The site is the instruction that forms or uses the
// address, the target the address it reaches. The synthetic text's filler words match no pattern of the scan, so the
// planted words are the only ones it sees.
struct ReferenceCase {
  const char* what;
  void (*plant)(World&);
  std::uint64_t site;
  std::uint64_t target;
};

TEST(AnInputWhoseCodeReachesTheSlotAreaIsRefusedNamingTheSiteAndTheTarget) {
  const ReferenceCase cases[] = {
      {"add",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4, AddImm(9, 9, kSlotArea & 0xFFF));
       },
       kRefAt + 4, 0x1403600},
      {"ldr",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4, LdrX(0, 9, kSlotArea & 0xFFF));
       },
       kRefAt + 4, 0x1403600},
      {"str w",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4, StrW(0, 9, 0x604));
       },
       kRefAt + 4, 0x1403604},
      {"ldur through an add",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4, AddImm(9, 9, 0x5f0));
         w.elf.PutWord(kRefAt + 8, Ldur(0, 9, 0x10));
       },
       kRefAt + 8, 0x1403600},
      {"ldp through an add",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4, AddImm(9, 9, 0x5f0));
         w.elf.PutWord(kRefAt + 8, LdpX(0, 1, 9, 0x10));
       },
       kRefAt + 8, 0x1403600},
      {"adr",
       [](World& w) { w.elf.PutWord(kRefAt, Adr(kRefAt, kSlotArea + 8, 0)); },
       kRefAt, 0x1403608},
      {"mov copy",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4, kMovX10X9);
         w.elf.PutWord(kRefAt + 8, LdrX(0, 10, kSlotArea & 0xFFF));
       },
       kRefAt + 8, 0x1403600},
      {"shifted add",
       [](World& w) {
         PlantAdrp(w, kRefAt, 0x1402000, 9);
         w.elf.PutWord(kRefAt + 4, AddImmLsl12(9, 9, 1));
         w.elf.PutWord(kRefAt + 8, LdrX(0, 9, 0x600));
       },
       kRefAt + 8, 0x1403600},
      {"window edge, in",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4 * 32, AddImm(9, 9, 0x600));
       },
       kRefAt + 128, 0x1403600},
      {"16-byte width",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4, AddImm(9, 9, 0x5f8));
       },
       kRefAt + 4, 0x14035f8},
      {"vector ldr q",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4, 0x3DC18120u);  // ldr q0, [x9, #0x600]
       },
       kRefAt + 4, 0x1403600},
  };
  for (const ReferenceCase& c : cases) {
    World w;
    c.plant(w);
    const PatchResult r = w.Patch();
    ExpectRefusal(r, "slot_area_referenced", "", c.what);
    CHECK(r.summary["site"] == HexOf(c.site));
    CHECK(r.summary["target"] == HexOf(c.target));
  }
}

struct PlantCase {
  const char* what;
  void (*plant)(World&);
};

// Reaches of the scan that stop short of the slot area: the window's end, a branch, a load that overwrites the base, the
// zone's edges. Each input is patched normally.
TEST(AReferenceThatDoesNotReachTheSlotAreaIsNotRefused) {
  const PlantCase cases[] = {
      {"one word past the window",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4 * 33, AddImm(9, 9, 0x600));
       }},
      {"a branch ends the window",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4, kBranchPlus4);
         w.elf.PutWord(kRefAt + 8, AddImm(9, 9, 0x600));
       }},
      {"a load into the register ends the tracking",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4, LdrX(9, 0, 0));
         w.elf.PutWord(kRefAt + 8, AddImm(9, 9, 0x600));
       }},
      {"just below by a full access width",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4, AddImm(9, 9, 0x5f0));
       }},
      {"exactly at the zone's end",
       [](World& w) {
         PlantAdrp(w, kRefAt, kSlotArea, 9);
         w.elf.PutWord(kRefAt + 4, AddImm(9, 9, 0x618));
       }},
  };
  for (const PlantCase& c : cases) {
    World w;
    c.plant(w);
    const PatchResult r = w.Patch();
    if (!r.ok) std::fprintf(stderr, "  %s: refused %s %s\n", c.what, r.refusal.code.c_str(), r.refusal.detail.c_str());
    CHECK(r.ok);
  }
  World control;  // the ADRP+ADD of the credits fixture, already in every input: the scan accepts a normal pair
  CHECK(control.Patch().ok);
}

TEST(ARefusedReferenceExitsOneAndTheRecordNamesTheSiteAndTarget) {
  World w;
  PlantAdrp(w, kRefAt, kSlotArea, 9);
  w.elf.PutWord(kRefAt + 4, AddImm(9, 9, kSlotArea & 0xFFF));
  const std::string stock = g_scratch + "/ref_stock.so", out = g_scratch + "/ref_out.so", report = g_scratch + "/ref_report.json";
  WriteBytes(stock, w.elf.bytes);
  std::remove(out.c_str());
  std::ostringstream so, se;
  CHECK_EQ(RunCli({"--stock", stock, "--out", out, "--report", report}, w.spec, so, se), 1);
  CHECK(!Exists(out));
  CHECK(so.str().empty());
  const Json err = Json::parse(se.str());
  CHECK(err["reason"] == "slot_area_referenced");
  CHECK(err["site"] == "0x1400604");
  CHECK(err["target"] == "0x1403600");
  CHECK(err["slot_area"] == "0x1403600");
  CHECK(err["slot_area_end"] == "0x1403618");
  CHECK(Json::parse(ReadText(report))["summary"]["status"] == "refused");
  for (const std::string& f : {stock, report}) std::remove(f.c_str());
}

TEST(ASpecWithAGapPlacesTheSlotAreaPastIt) {
  World w;
  w.spec.slotAreaGap = 0x100;
  const PatchResult r = w.Patch();
  CHECK(r.ok);
  if (!r.ok) {
    std::fprintf(stderr, "  %s\n", r.summary.dump().c_str());
    return;
  }
  CHECK(r.summary["slot_area"] == "0x1403700");      // RoundUp(0x14035f8 + 0x100, 64)
  CHECK(r.summary["slot_area_end"] == "0x1403718");  // + 8 bytes per row, 3 rows
  // The two inline stubs decode to their slots, and the credits pairs load from row 2's slot.
  Stub stub{};
  CHECK(EncodeSlotStub(kJoinVa, 0x1403700, &stub));
  for (std::size_t i = 0; i < 3; ++i) CHECK_EQ(WordOf(r.output, kJoinVa + 4 * i), stub.words[i]);
  CHECK(EncodeSlotStub(kFailVa, 0x1403708, &stub));
  for (std::size_t i = 0; i < 3; ++i) CHECK_EQ(WordOf(r.output, kFailVa + 4 * i), stub.words[i]);
  std::uint32_t adrp = 0, ldr = 0;
  CHECK(EncodeAdrp(kAdrp0, 0x1403710, 8, &adrp));
  CHECK(EncodeLdrX(8, 8, 0x710, &ldr));
  CHECK_EQ(WordOf(r.output, kAdrp0), adrp);
  CHECK_EQ(WordOf(r.output, kAdd0), ldr);
  CHECK(EncodeAdrp(kAdrp1, 0x1403710, 12, &adrp));
  CHECK(EncodeLdrX(12, 12, 0x710, &ldr));
  CHECK_EQ(WordOf(r.output, kAdrp1), adrp);
  CHECK_EQ(WordOf(r.output, kAdd1), ldr);
  CHECK(r.summary["build_id"] == BuildIdHex(DeriveBuildId(StockId(), CanonicalTable(w.spec.rows, w.spec.rowCount, {}, 0x1403700))));
  std::uint64_t memsz = 0;
  std::memcpy(&memsz, r.output.data() + w.elf.PhdrMemszOffset(), 8);
  CHECK_EQ(memsz, static_cast<std::uint64_t>(0x1403718 - kDataVaddr));
  // The gap is part of what the pin means: the output is recognised under the gapped spec, and refused as another build without it.
  ExpectRefusal(PatchLibr15(r.output, w.spec, {}, ""), "already_patched", "", "the gapped output under the gapped spec");
  PatchSpec noGap = w.spec;
  noGap.slotAreaGap = 0;
  ExpectRefusal(PatchLibr15(r.output, noGap, {}, ""), "stock_build_id_mismatch", "", "the gapped output under a spec with no gap");
}

// The state words another build of this libr15 keeps in the slack, in miniature: two of them, where the reference build's
// are. With no gap the slot area is on them and the input is refused; with the gap they are clear and the input is patched.
TEST(AnInputsStateWordsInThePageSlackAreKeptClearByTheGap) {
  World w;
  PlantAdrp(w, kRefAt, kSlotArea, 9);
  w.elf.PutWord(kRefAt + 4, AddImm(9, 9, kSlotArea & 0xFFF));
  w.elf.PutWord(kRefAt + 8, 0xB9400128u);  // ldr w8, [x9]: the reference's own word
  PlantAdrp(w, kRefAt + 16, kSlotArea + 0x20, 9);
  w.elf.PutWord(kRefAt + 20, AddImm(9, 9, (kSlotArea + 0x20) & 0xFFF));
  w.elf.PutWord(kRefAt + 24, 0xF900012Au);  // str x10, [x9]: the reference's own word
  ExpectRefusal(w.Patch(), "slot_area_referenced", "", "the state words with no gap");
  w.spec.slotAreaGap = 0x100;
  const PatchResult r = w.Patch();
  CHECK(r.ok);
  if (!r.ok) {
    std::fprintf(stderr, "  %s\n", r.summary.dump().c_str());
    return;
  }
  CHECK(r.summary["slot_area"] == "0x1403700");
  for (std::uint64_t off = 0; off < 32; off += 4) CHECK_EQ(WordOf(r.output, kRefAt + off), w.elf.WordAt(kRefAt + off));
  static_assert(kSlotArea + 0x20 + 8 <= 0x1403700, "the second state word ends before the gapped slot area");
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
