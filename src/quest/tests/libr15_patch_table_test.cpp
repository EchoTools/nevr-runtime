// Host test for the libr15 patch table and the patched build ID (src/quest/patch/libr15_patch_table.h,
// libr15_patch_identity.h; ADR 0021 sections 1, 4 and 7). The real-file checks (every row's stock words at its vaddr
// in the store libr15) are in the pinned recipe `just test-quest-patcher-pinned`; this test checks the table's own
// shape and the identity function on fixed inputs.
// Run: libr15_patch_table_test [name-filter]

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "mini_test.h"
#include "quest/patch/libr15_patch_identity.h"
#include "quest/patch/libr15_patch_table.h"
#include "quest/patch/stub_codec.h"

namespace {

using namespace nevr_quest::patch;

const Row* Find(const char* id) {
  for (std::size_t i = 0; i < kRowCount; ++i) {
    if (std::strcmp(kRows[i].id, id) == 0) return &kRows[i];
  }
  return nullptr;
}

TEST(TheTableHasTheSevenRowsOfTheAdrInOrder) {
  const char* ids[] = {"lobby_full.join", "lobby_full.failure", "lobby_full.success", "ui_texture.attach",
                       "splash_skip.widget_update", "menu_version.set_text", "credits.pairs"};
  CHECK_EQ(kRowCount, static_cast<std::size_t>(7));
  for (std::size_t i = 0; i < kRowCount; ++i) {
    CHECK(std::strcmp(kRows[i].id, ids[i]) == 0);
    CHECK_EQ(kRows[i].slot, i);
  }
  std::set<std::string> seen;
  for (std::size_t i = 0; i < kRowCount; ++i) CHECK(seen.insert(kRows[i].id).second);
}

TEST(TheVaddrIsTheReVaultAddressMinusTheBiasInCode) {
  // The ReVault addresses of ADR section 1, and the pinned vaddrs they give (the stock words were read at these).
  struct Expect { const char* id; std::uint64_t revault; std::uint64_t vaddr; };
  const Expect expects[] = {
      {"lobby_full.join", 0x1340ba8, 0x1240ba8},          {"lobby_full.failure", 0x1340cc8, 0x1240cc8},
      {"lobby_full.success", 0x134024c, 0x124024c},       {"ui_texture.attach", 0x19e75c4, 0x18e75c4},
      {"splash_skip.widget_update", 0x209a7a0, 0x1f9a7a0}, {"menu_version.set_text", 0x149111c, 0x139111c},
  };
  for (const Expect& e : expects) {
    const Row* row = Find(e.id);
    CHECK(row != nullptr);
    if (row == nullptr) continue;
    CHECK_EQ(row->sites[0].revaultVa, e.revault);
    CHECK_EQ(row->sites[0].Vaddr(), e.vaddr);
    CHECK(row->kind == RowKind::kInlineHook);
    CHECK_EQ(row->sites[0].wordCount, static_cast<std::uint8_t>(3));
  }
  const Row* credits = Find("credits.pairs");
  CHECK(credits != nullptr);
  if (credits == nullptr) return;
  const std::uint64_t pairs[] = {0x1ea1284, 0x1ea1290, 0x1ea1d84, 0x1ea1d88};
  CHECK(credits->kind == RowKind::kCreditsPair);
  CHECK_EQ(credits->siteCount, static_cast<std::uint8_t>(4));
  for (std::size_t i = 0; i < 4; ++i) {
    CHECK_EQ(credits->sites[i].revaultVa, pairs[i]);
    CHECK_EQ(credits->sites[i].Vaddr(), pairs[i] - kRevaultBias);
  }
  CHECK_EQ(kRevaultBias, static_cast<std::uint64_t>(0x100000));
}

TEST(TheSlotAreaIsSectionFourOfTheAdr) {
  CHECK_EQ(kStockImageEnd, static_cast<std::uint64_t>(0x37d2538));
  CHECK_EQ(kSlotAreaVaddr, static_cast<std::uint64_t>(0x37d2540));
  CHECK_EQ(kSlotAreaVaddr + kSlotBytes * kRowCount, static_cast<std::uint64_t>(0x37d2578));
  CHECK_EQ(kRows[0].SlotVaddr(), static_cast<std::uint64_t>(0x37d2540));
  CHECK_EQ(kRows[6].SlotVaddr(), static_cast<std::uint64_t>(0x37d2570));
  CHECK_EQ(RoundUp(0x37d2538, 64), static_cast<std::uint64_t>(0x37d2540));
  CHECK_EQ(RoundUp(0x37d2540, 64), static_cast<std::uint64_t>(0x37d2540));
}

TEST(TheJoinRowEncodesToTheAdrsReferenceStub) {
  const Row& join = kRows[0];
  Stub stub{};
  CHECK(EncodeSlotStub(join.sites[0].Vaddr(), join.SlotVaddr(), &stub));
  CHECK_EQ(stub.words[0], 0xd0012c90u);
  CHECK_EQ(stub.words[1], 0xf942a210u);
  CHECK_EQ(stub.words[2], 0xd61f0200u);
}

TEST(EveryInlineRowsStubDecodesBackToItsSlot) {
  for (std::size_t i = 0; i < kRowCount; ++i) {
    if (kRows[i].kind != RowKind::kInlineHook) continue;
    Stub stub{};
    CHECK(EncodeSlotStub(kRows[i].sites[0].Vaddr(), kRows[i].SlotVaddr(), &stub));
    std::uint64_t slot = 0;
    CHECK(DecodeSlotStub(stub.words, kRows[i].sites[0].Vaddr(), &slot));
    CHECK_EQ(slot, kRows[i].SlotVaddr());
  }
}

TEST(TheCreditsPairsAreAnAdrpAndAnAddOfTheSameRegisterOnTheBlobsPage) {
  const Row* credits = Find("credits.pairs");
  CHECK(credits != nullptr);
  if (credits == nullptr) return;
  std::uint64_t blob = 0;
  for (std::size_t pair = 0; pair < 2; ++pair) {
    const Site& adrp = credits->sites[2 * pair];
    const Site& add = credits->sites[2 * pair + 1];
    unsigned rd = 0;
    std::uint64_t page = 0;
    CHECK(DecodeAdrp(adrp.words[0], adrp.Vaddr(), &rd, &page));
    CHECK_EQ((add.words[0] & 0xFFC00000u), 0x91000000u);  // ADD Xd, Xn, #imm12 (no shift)
    CHECK_EQ((add.words[0] & 0x1F), rd);
    CHECK_EQ(((add.words[0] >> 5) & 0x1F), rd);
    const std::uint64_t target = page + ((add.words[0] >> 10) & 0xFFF);
    if (pair == 0) blob = target;
    CHECK_EQ(target, blob);
  }
  CHECK_EQ(blob, static_cast<std::uint64_t>(0x31a3de1));  // credits_text.h kBlobVaddr
}

BuildIdBytes StockId() {
  BuildIdBytes id{};
  CHECK(ParseBuildIdHex(kLibR15StockBuildId, &id));
  return id;
}

TEST(HexRoundTripsAndRefusesBadStrings) {
  BuildIdBytes id{};
  CHECK(ParseBuildIdHex("b243509c08ce677aeb95fa348016949b3fc45230", &id));
  CHECK_EQ(id[0], static_cast<std::uint8_t>(0xb2));
  CHECK_EQ(id[19], static_cast<std::uint8_t>(0x30));
  CHECK_EQ(BuildIdHex(id), std::string("b243509c08ce677aeb95fa348016949b3fc45230"));
  CHECK(!ParseBuildIdHex("b243509c08ce677aeb95fa348016949b3fc4523", &id));    // 39
  CHECK(!ParseBuildIdHex("b243509c08ce677aeb95fa348016949b3fc452300", &id));  // 41
  CHECK(!ParseBuildIdHex("B243509c08ce677aeb95fa348016949b3fc45230", &id));   // upper case
  CHECK(!ParseBuildIdHex("g243509c08ce677aeb95fa348016949b3fc45230", &id));  // the high digit of byte 0
  CHECK(!ParseBuildIdHex("bg43509c08ce677aeb95fa348016949b3fc45230", &id));  // the low digit of byte 0
  CHECK(!ParseBuildIdHex("b243509c08ce677aeb95fa348016949b3fc4523g", &id));  // the low digit of the last byte
}

TEST(ThePatchedIdIsAPureFunctionOfTheTableAndTheStockId) {
  const std::string a = PatchedBuildId();
  CHECK_EQ(a.size(), static_cast<std::size_t>(40));
  CHECK_EQ(a, PatchedBuildId());
  CHECK_EQ(a, PatchedBuildId(std::vector<WordPatchRow>{}));
  CHECK(a != kLibR15StockBuildId);
  // The same value from its parts, as the patcher computes it from the file's own note.
  CHECK_EQ(a, BuildIdHex(DeriveBuildId(StockId(), CanonicalTable(kRows, kRowCount, {}, kSlotAreaVaddr))));
}

TEST(ThePublicIdIsPinnedSoATableChangeIsAConsciousOne) {
  // Any change to a public row, a stock word, a slot or the layout moves this value. Update it in the commit that
  // changes the table, with the reason.
  CHECK_EQ(PatchedBuildId(), std::string("cf8333283f12e3bd50fd3bb387c3d85887a2b54a"));
}

TEST(AnyChangeToAFactTheFileDependsOnMovesTheId) {
  const BuildIdBytes stock = StockId();
  const std::string base = CanonicalTable(kRows, kRowCount, {}, kSlotAreaVaddr);
  const BuildIdBytes baseId = DeriveBuildId(stock, base);

  for (std::size_t i = 0; i < stock.size(); ++i) {  // every byte of the stock ID counts
    BuildIdBytes otherStock = stock;
    otherStock[i] ^= 1;
    CHECK(DeriveBuildId(otherStock, base) != baseId);
  }
  CHECK(DeriveBuildId(stock, CanonicalTable(kRows, kRowCount, {}, kSlotAreaVaddr + 64)) != baseId);
  CHECK(DeriveBuildId(stock, CanonicalTable(kRows, kRowCount - 1, {}, kSlotAreaVaddr)) != baseId);

  Row changed[kRowCount];
  for (std::size_t i = 0; i < kRowCount; ++i) changed[i] = kRows[i];
  changed[2].sites[0].words[1] ^= 0x10;  // a stock word
  CHECK(DeriveBuildId(stock, CanonicalTable(changed, kRowCount, {}, kSlotAreaVaddr)) != baseId);
  for (std::size_t i = 0; i < kRowCount; ++i) changed[i] = kRows[i];
  changed[4].sites[0].revaultVa += 4;  // a site address
  CHECK(DeriveBuildId(stock, CanonicalTable(changed, kRowCount, {}, kSlotAreaVaddr)) != baseId);
  for (std::size_t i = 0; i < kRowCount; ++i) changed[i] = kRows[i];
  changed[1].slot = 5;  // a slot
  CHECK(DeriveBuildId(stock, CanonicalTable(changed, kRowCount, {}, kSlotAreaVaddr)) != baseId);
  for (std::size_t i = 0; i < kRowCount; ++i) changed[i] = kRows[i];
  changed[0].id = "lobby_full.joinx";  // an id
  CHECK(DeriveBuildId(stock, CanonicalTable(changed, kRowCount, {}, kSlotAreaVaddr)) != baseId);
  // The handler and game-function names change nothing in the file, so they do not move it.
  for (std::size_t i = 0; i < kRowCount; ++i) changed[i] = kRows[i];
  changed[0].handler = "OnJoin2";
  changed[0].gameFunction = "Renamed";
  CHECK(DeriveBuildId(stock, CanonicalTable(changed, kRowCount, {}, kSlotAreaVaddr)) == baseId);
}

WordEdit Edit(std::uint32_t offset, WordBytes stock, WordBytes word) { return {offset, stock, word}; }

TEST(ExtraRowsChangeTheIdAndTheirOrderDoesNot) {
  const WordBytes nop{0x1f, 0x20, 0x03, 0xd5};
  const WordPatchRow a{"x.a", 0x1500000, {Edit(4, {0xe0, 0x03, 0x1f, 0xaa}, nop)}};
  const WordPatchRow b{"x.b", 0x1600000, {Edit(0, {0xff, 0x83, 0x00, 0xd1}, nop), Edit(8, {0xf3, 0x7b, 0x01, 0xa9}, nop)}};
  const std::string none = PatchedBuildId();
  const std::string one = PatchedBuildId({a});
  const std::string two = PatchedBuildId({a, b});
  CHECK(none != one);
  CHECK(one != two);
  CHECK_EQ(two, PatchedBuildId({b, a}));
  CHECK_EQ(one, PatchedBuildId({a}));
  // Each field of an extra row counts.
  WordPatchRow c = a;
  c.edits[0].word[3] ^= 1;
  CHECK(PatchedBuildId({c}) != one);
  c = a;
  c.edits[0].stock[0] ^= 1;
  CHECK(PatchedBuildId({c}) != one);
  c = a;
  c.edits[0].offset = 8;
  CHECK(PatchedBuildId({c}) != one);
  c = a;
  c.revaultVa += 4;
  CHECK(PatchedBuildId({c}) != one);
  c = a;
  c.id = "x.c";
  CHECK(PatchedBuildId({c}) != one);
  // The widths of the fixed fields: a difference only in the high bytes still counts.
  c = a;
  c.revaultVa += std::uint64_t{1} << 40;
  CHECK(PatchedBuildId({c}) != one);
  c = a;
  c.edits[0].offset = 4 + 256;
  CHECK(PatchedBuildId({c}) != one);
  c = a;
  c.id = "x.a2";
  CHECK(PatchedBuildId({c}) != one);
  // The binary record is framed: moving a word between two rows is a different file, not the same bytes.
  const WordPatchRow split1{"x.s", 0x1500000, {Edit(0, {1, 2, 3, 4}, nop), Edit(4, {5, 6, 7, 8}, nop)}};
  const WordPatchRow split2a{"x.s", 0x1500000, {Edit(0, {1, 2, 3, 4}, nop)}};
  const WordPatchRow split2b{"x.t", 0x1500004, {Edit(0, {5, 6, 7, 8}, nop)}};
  CHECK(PatchedBuildId({split1}) != PatchedBuildId({split2a, split2b}));
  // The two word forms: bytes in memory order and the little-endian value.
  CHECK_EQ(WordValue(nop), 0xd503201fu);
}

// The patched ID of a table with extra rows, pinned as a literal. The value was computed outside this code (a script
// that applies the serialisation below to the public table's canonical text and hashes it with SHA-256), so it checks
// the serialisation itself and not just that it is deterministic. The extra rows are sorted by (revault_va, id) and
// written after the public table as:
//   "extra|" + hex(row count) + "\n"          the count as 0x%llx, as in the public part
//   then, per row, binary and without separators:
//     u8 id length, the id bytes, u64 little-endian revault_va, u8 word count,
//     and per word: u16 little-endian offset, the 4 new_bytes, the 4 stock_bytes (each in memory order).
// The ID is the first 20 bytes of SHA-256("NEVR-LIBR15-PATCH-1" + the 20 stock ID bytes + the table text).
// Three rows: two share an address (the id breaks the tie, and one offset needs both bytes of its u16), one is later in
// memory and has two words (the word count and the offsets of a row); they are listed here out of order.
TEST(TheIdOfThreeInventedExtraRowsIsPinnedToTheByte) {
  const WordPatchRow late{"x.late", 0x1500040, {Edit(0, {0x11, 0x22, 0x33, 0x44}, {0x55, 0x66, 0x77, 0x88}), Edit(8, {0x99, 0xaa, 0xbb, 0xcc}, {0xdd, 0xee, 0xff, 0x00})}};
  const WordPatchRow tieB{"x.tie_b", 0x1500020, {Edit(4, {0x0a, 0x0b, 0x0c, 0x0d}, {0x01, 0x02, 0x03, 0x04})}};
  const WordPatchRow tieA{"x.tie_a", 0x1500020, {Edit(0x104, {0x0f, 0x0e, 0x0d, 0x0c}, {0x04, 0x03, 0x02, 0x01})}};
  const std::string pinned = "ccb7fde69c56a86c219885817fdc0f542e15928d";
  CHECK_EQ(PatchedBuildId({late, tieB, tieA}), pinned);
  CHECK_EQ(PatchedBuildId({tieA, late, tieB}), pinned);
  CHECK_EQ(PatchedBuildId({tieB, tieA, late}), pinned);
  // Each part of the pinned value, so a change to one serialisation rule is seen in its own value.
  CHECK_EQ(PatchedBuildId({late}), std::string("7b61f482609d1d762e566dfddb98dff72db27bcb"));
  CHECK_EQ(PatchedBuildId({tieB, tieA}), std::string("45831f0edfb0b2f31a334fffb03fc0fb805b8ded"));
}

}  // namespace

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
