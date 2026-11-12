// #1420: the magnum, smg, Chain and rifle bullets get the scout bullet's particle effects. netgun_bullets.h is the
// byte logic and the pin decision; the fixture (netgun_bullets_fixture.h) writes both resources with literal offsets
// and ids and writes the expected results natively, so these tests compare the production rewrite with resources
// built with the effects already in them.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "runtime/patch/netgun_bullets.h"
#include "runtime/tests/netgun_bullets_fixture.h"

namespace {

using namespace netgun_fixture;
using nevr_netgun_bullets::Decide;
using nevr_netgun_bullets::PatchBullets;
using nevr_netgun_bullets::PatchParticles;
using nevr_netgun_bullets::Resource;
using nevr_netgun_bullets::Status;
using nevr_netgun_bullets::Verdict;

nevr_netgun_bullets::Result Particles_(const std::vector<uint8_t>& b) { return PatchParticles(b.data(), b.size()); }
nevr_netgun_bullets::Result Bullets_(const std::vector<uint8_t>& b) { return PatchBullets(b.data(), b.size()); }

// ── The names the transforms answer for ─────────────────────────────────────────────────────────────────────────

TEST(NetgunBulletNames, TheTypesAreTheCSymbol64OfTheClassNames) {
  // CSymbol64 of the class names, as tools/echomod/radhash.py computes them.
  EXPECT_EQ(nevr_netgun_bullets::kParticleType, 0x8ee04a8fa8b1cdb0ULL);
  EXPECT_EQ(nevr_netgun_bullets::kBulletType, 0x16bafee3754402a4ULL);
}

TEST(NetgunBulletNames, TheFiveLevelsAreTheOnesThatCarryTheNetGuns) {
  ASSERT_EQ(std::size(nevr_netgun_bullets::kLevels), 5u);
  const std::pair<const char*, uint64_t> want[] = {
      {"mpl_combat_combustion", 0x42670f2bed45703cULL}, {"mpl_combat_dyson", 0x43e2da7914642604ULL},
      {"mpl_combat_fission", 0xdf5ca7b7dfa383d4ULL},    {"mpl_combat_gauss", 0x43e2da7a0c623a19ULL},
      {"mpl_lobby_b_combat", 0xcb9977f7fc2b4526ULL},
  };
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_STREQ(nevr_netgun_bullets::kLevels[i].name, want[i].first);
    EXPECT_EQ(nevr_netgun_bullets::kLevels[i].hash, want[i].second) << want[i].first;
  }
}

// ── CParticleEffectCR ───────────────────────────────────────────────────────────────────────────────────────────

TEST(NetgunBulletParticles, TheFourGetTheScoutsRowsAfterTheLastRow) {
  const nevr_netgun_bullets::Result r = Particles_(Particles(StockRows()));
  ASSERT_EQ(r.verdict, Verdict::Patched) << nevr_netgun_bullets::StatusName(r.status);
  EXPECT_EQ(r.status, Status::Ok);
  EXPECT_EQ(r.entriesBefore, 11u);
  EXPECT_EQ(r.entriesAfter, 20u);
  EXPECT_EQ(r.bytes, PatchedParticles());
}

TEST(NetgunBulletParticles, EveryStockRowKeepsItsIndex) {
  // mpl_combat_gauss: five rows of a door actor follow the bullets' rows. They stay at 11 to 15, where a server, which
  // runs no transform, has them; the new rows are 16 to 24.
  const nevr_netgun_bullets::Result r = Particles_(Particles(StockRows(5)));
  ASSERT_EQ(r.verdict, Verdict::Patched) << nevr_netgun_bullets::StatusName(r.status);
  EXPECT_EQ(r.entriesBefore, 16u);
  EXPECT_EQ(r.entriesAfter, 25u);
  EXPECT_EQ(r.bytes, PatchedParticles(5));
  for (size_t i = 11; i < 16; ++i) EXPECT_EQ(Get64(r.bytes, 56 + i * 152 + 8), kDoor) << "row " << i;
  EXPECT_EQ(Get64(r.bytes, 56 + 16 * 152 + 8), kMagnum);
  EXPECT_EQ(Get64(r.bytes, 56 + 24 * 152 + 8), kRifle);
}

TEST(NetgunBulletParticles, TheHeaderCountsTheNewRows) {
  const std::vector<uint8_t> out = Particles_(Particles(StockRows())).bytes;
  ASSERT_EQ(out.size(), 56u + 20u * 152u);
  EXPECT_EQ(Get64(out, 8), 20u * 152u);
  EXPECT_EQ(Get64(out, 40), 20u);
  EXPECT_EQ(Get64(out, 48), 20u);
}

TEST(NetgunBulletParticles, EachNewRowIsTheScoutsWithOnlyItsOwnerAndPoolChanged) {
  const std::vector<uint8_t> in = Particles(StockRows());
  const std::vector<uint8_t> out = Particles_(in).bytes;
  ASSERT_EQ(out.size(), 56u + 20u * 152u);
  const uint64_t owners[] = {kMagnum, kMagnum, kMagnum, kSmg, kSmg, kChain, kChain, kRifle, kRifle};
  const uint16_t pools[] = {65, 65, 65, 200, 200, 150, 150, 500, 500};
  const size_t donors[] = {4, 5, 6, 4, 5, 4, 5, 4, 5};  // the scout's trail, impact, explosion rows
  for (size_t i = 0; i < 9; ++i) {
    const size_t row = 56 + (11 + i) * 152, donor = 56 + donors[i] * 152;
    EXPECT_EQ(Get64(out, row + 8), owners[i]) << "row " << 11 + i;
    EXPECT_EQ(Get16(out, row + 18), pools[i]) << "row " << 11 + i;
    for (size_t b = 0; b < 152; ++b) {
      if ((b >= 8 && b < 16) || b == 18 || b == 19) continue;
      ASSERT_EQ(out[row + b], in[donor + b]) << "row " << 11 + i << " byte " << b;
    }
  }
}

TEST(NetgunBulletParticles, TheRowsItWasGivenAreKeptByteForByte) {
  const std::vector<uint8_t> in = Particles(StockRows(5));
  const std::vector<uint8_t> out = Particles_(in).bytes;
  ASSERT_EQ(out.size(), in.size() + 9 * 152);
  for (size_t b = 56; b < in.size(); ++b) ASSERT_EQ(out[b], in[b]) << b;
  for (size_t b = 0; b < 56; ++b) {
    if ((b >= 8 && b < 16) || (b >= 40 && b < 56)) continue;
    ASSERT_EQ(out[b], in[b]) << "header byte " << b;
  }
}

TEST(NetgunBulletParticles, TheRowsGoAtTheEndWhateverOwnsTheLastRow) {
  // A door row first and one between the scout's and the rocket's: every given row keeps its place.
  std::vector<Row> rows = StockRows();
  rows.insert(rows.begin() + 7, Row{0x1000, kDoor, 0xffff, 40});
  rows.insert(rows.begin(), Row{0x1001, kDoor, 0xffff, 41});
  const std::vector<uint8_t> in = Particles(rows);
  const std::vector<uint8_t> out = Particles_(in).bytes;
  ASSERT_EQ(out.size(), 56u + 22u * 152u);
  for (size_t b = 56; b < in.size(); ++b) ASSERT_EQ(out[b], in[b]) << b;
  EXPECT_EQ(Get64(out, 56 + 12 * 152 + 8), kRocket);
  EXPECT_EQ(Get64(out, 56 + 13 * 152 + 8), kMagnum);
  EXPECT_EQ(Get64(out, 56 + 21 * 152 + 8), kRifle);
}

TEST(NetgunBulletParticles, AResourceThatAlreadyHasARowOfTheFourIsRefused) {
  const nevr_netgun_bullets::Result again = Particles_(PatchedParticles());
  EXPECT_EQ(again.verdict, Verdict::Refused);
  EXPECT_EQ(again.status, Status::AlreadyPresent);
  EXPECT_TRUE(again.bytes.empty());
  for (uint64_t owner : {kMagnum, kSmg, kChain, kRifle}) {
    std::vector<Row> rows = StockRows();
    rows.push_back({kTrail, owner, 1, 50});
    EXPECT_EQ(Particles_(Particles(rows)).status, Status::AlreadyPresent) << std::hex << owner;
    // The first row is read too.
    rows = StockRows();
    rows.insert(rows.begin(), Row{kTrail, owner, 1, 50});
    EXPECT_EQ(Particles_(Particles(rows)).status, Status::AlreadyPresent) << std::hex << owner << " first";
  }
}

TEST(NetgunBulletParticles, AScoutWithoutOneOfItsThreeRowsOrWithTwoIsRefused) {
  for (size_t drop : {4u, 5u, 6u}) {
    std::vector<Row> rows = StockRows();
    rows.erase(rows.begin() + drop);
    EXPECT_EQ(Particles_(Particles(rows)).status, Status::NoDonor) << "without row " << drop;
  }
  for (uint64_t name : {kTrail, kImpact, kExplosion}) {
    std::vector<Row> rows = StockRows();
    rows.push_back({name, kScout, 75, 60});
    EXPECT_EQ(Particles_(Particles(rows)).status, Status::NoDonor) << std::hex << name;
  }
  // The scout's row names on another actor are not the scout's rows.
  const std::vector<Row> door = {
      {kTrail, kDoor, 0xffff, 1}, {kImpact, kDoor, 0xffff, 2}, {kExplosion, kDoor, 0xffff, 3}};
  EXPECT_EQ(Particles_(Particles(door)).status, Status::NoDonor);
}

// ── The header both rewrites read ───────────────────────────────────────────────────────────────────────────────

struct HeaderCase {
  const char* what;
  size_t at;
  uint64_t value;
  size_t width;
  Status want;
};

TEST(NetgunBulletHeader, AHeaderNotAsOnDiskIsRefusedByBothRewrites) {
  const HeaderCase cases[] = {
      {"count above capacity", 48, 12, 8, Status::BadHeader},
      {"capacity above count", 40, 12, 8, Status::BadHeader},
      {"data size off by one", 8, 11 * 152 + 1, 8, Status::BadHeader},
      {"flags 0", 28, 0, 4, Status::BadHeader},
      {"flags 3", 28, 3, 4, Status::BadHeader},
  };
  for (const HeaderCase& c : cases) {
    std::vector<uint8_t> p = Particles(StockRows());
    if (c.width == 8) Put64(p, c.at, c.value); else Put32(p, c.at, static_cast<uint32_t>(c.value));
    EXPECT_EQ(Particles_(p).status, c.want) << c.what;
  }
  std::vector<uint8_t> b = Bullets(StockEntries());
  Put64(b, 8, 9 * 136 + 8);
  EXPECT_EQ(Bullets_(b).status, Status::BadHeader);
  b = Bullets(StockEntries());
  Put32(b, 28, 0);
  EXPECT_EQ(Bullets_(b).status, Status::BadHeader);
}

TEST(NetgunBulletHeader, ACountThatIsZeroOrSaysMoreThanTheBytesIsRefused) {
  std::vector<uint8_t> p = Particles(StockRows());
  Put64(p, 40, 0);
  Put64(p, 48, 0);
  EXPECT_EQ(Particles_(p).status, Status::BadHeader);
  // A header with no entries and no data is consistent in itself, and is still not a stock resource.
  EXPECT_EQ(Particles_(Particles({})).status, Status::BadHeader);
  EXPECT_EQ(Bullets_(Bullets({})).status, Status::BadHeader);
  p = Particles(StockRows());
  Put64(p, 40, 12);
  Put64(p, 48, 12);
  Put64(p, 8, 12 * 152);
  EXPECT_EQ(Particles_(p).status, Status::Truncated);
  p = Particles(StockRows());
  Put64(p, 40, 0x4000000000000000ULL);
  Put64(p, 48, 0x4000000000000000ULL);
  EXPECT_EQ(Particles_(p).status, Status::Truncated);
}

TEST(NetgunBulletHeader, TrailingBytesShortBuffersAndNullAreRefused) {
  std::vector<uint8_t> p = Particles(StockRows());
  p.push_back(0);
  EXPECT_EQ(Particles_(p).status, Status::BadHeader);
  p = Particles(StockRows());
  EXPECT_EQ(PatchParticles(p.data(), 55).status, Status::Truncated);
  EXPECT_EQ(PatchParticles(p.data(), p.size() - 1).status, Status::Truncated);
  EXPECT_EQ(PatchParticles(nullptr, 0).status, Status::Truncated);
  EXPECT_EQ(PatchParticles(nullptr, 4096).status, Status::Truncated);
  EXPECT_EQ(PatchBullets(nullptr, 0).status, Status::Truncated);
  EXPECT_EQ(PatchBullets(nullptr, 4096).status, Status::Truncated);
  const std::vector<uint8_t> b = Bullets(StockEntries());
  EXPECT_EQ(PatchBullets(b.data(), b.size() - 1).status, Status::Truncated);
}

// ── CR15NetBulletCR ─────────────────────────────────────────────────────────────────────────────────────────────

TEST(NetgunBulletSlots, TheFourNameTheScoutsImpactAndTrailAndTheMagnumItsExplosion) {
  const nevr_netgun_bullets::Result r = Bullets_(Bullets(StockEntries()));
  ASSERT_EQ(r.verdict, Verdict::Patched) << nevr_netgun_bullets::StatusName(r.status);
  EXPECT_EQ(r.status, Status::Ok);
  EXPECT_EQ(r.entriesBefore, 9u);
  EXPECT_EQ(r.entriesAfter, 9u);
  EXPECT_EQ(r.bytes, PatchedBullets());
}

TEST(NetgunBulletSlots, TheScoutAndEachOfTheFourAreFoundAsTheFirstEntry) {
  for (size_t first : {2u, 4u, 5u, 6u, 7u}) {
    std::vector<Entry> e = StockEntries();
    std::rotate(e.begin(), e.begin() + static_cast<std::ptrdiff_t>(first),
                e.begin() + static_cast<std::ptrdiff_t>(first) + 1);
    const nevr_netgun_bullets::Result r = Bullets_(Bullets(e));
    ASSERT_EQ(r.verdict, Verdict::Patched)
        << "entry " << first << " first: " << nevr_netgun_bullets::StatusName(r.status);
    EXPECT_EQ(r.bytes, PatchedBullets(e)) << "entry " << first << " first";
  }
}

TEST(NetgunBulletSlots, OnlyTheNineSlotsChange) {
  const std::vector<uint8_t> in = Bullets(StockEntries());
  const std::vector<uint8_t> out = Bullets_(in).bytes;
  ASSERT_EQ(out.size(), in.size());
  size_t changed = 0;
  for (size_t b = 0; b < in.size(); ++b) changed += in[b] != out[b];
  EXPECT_EQ(changed, 9u * 8u);
}

TEST(NetgunBulletSlots, AMissingOrDoubledScoutOrScoutSlotsThatDifferAreRefused) {
  std::vector<Entry> e = StockEntries();
  e.erase(e.begin() + 2);
  EXPECT_EQ(Bullets_(Bullets(e)).status, Status::NoDonor);
  e = StockEntries();
  e.push_back(e[2]);
  EXPECT_EQ(Bullets_(Bullets(e)).status, Status::NoDonor);
  for (size_t s = 0; s < 3; ++s) {
    e = StockEntries();
    e[2].slots[s] = kNone;
    EXPECT_EQ(Bullets_(Bullets(e)).status, Status::NoDonor) << "scout slot " << s;
  }
}

TEST(NetgunBulletSlots, AMissingOrDoubledEntryOfTheFourOrAnotherPoolIsRefused) {
  for (size_t i = 4; i < 8; ++i) {
    std::vector<Entry> e = StockEntries();
    e.erase(e.begin() + static_cast<std::ptrdiff_t>(i));
    EXPECT_EQ(Bullets_(Bullets(e)).status, Status::MissingBullet) << "without entry " << i;
    e = StockEntries();
    e.push_back(e[i]);
    EXPECT_EQ(Bullets_(Bullets(e)).status, Status::MissingBullet) << "entry " << i << " twice";
    e = StockEntries();
    e[i].pool = static_cast<uint16_t>(e[i].pool + 1);
    EXPECT_EQ(Bullets_(Bullets(e)).status, Status::MissingBullet) << "entry " << i << " pool";
    // A missing entry is refused for being missing, not for a pool read from where no entry is: here the header's
    // bytes at +18 hold the missing bullet's pool.
    const uint16_t pool = StockEntries()[i].pool;
    e = StockEntries();
    e.erase(e.begin() + static_cast<std::ptrdiff_t>(i));
    std::vector<uint8_t> b = Bullets(e);
    Put16(b, 18, pool);
    EXPECT_EQ(Bullets_(b).status, Status::MissingBullet) << "without entry " << i << ", its pool in the header";
  }
}

TEST(NetgunBulletSlots, AnEntryOfTheFourThatAlreadyNamesAnEffectIsRefused) {
  const nevr_netgun_bullets::Result again = Bullets_(PatchedBullets());
  EXPECT_EQ(again.verdict, Verdict::Refused);
  EXPECT_EQ(again.status, Status::AlreadyPresent);
  EXPECT_TRUE(again.bytes.empty());
  for (size_t s = 0; s < 6; ++s) {
    std::vector<Entry> e = StockEntries();
    e[7].slots[s] = 0x1234;
    EXPECT_EQ(Bullets_(Bullets(e)).status, Status::AlreadyPresent) << "rifle slot " << s;
  }
}

// ── The pin decision ────────────────────────────────────────────────────────────────────────────────────────────

TEST(NetgunBulletDecide, OnlyAPinnedBufferIsRewritten) {
  const std::vector<uint8_t> p = Particles(StockRows());
  const nevr_netgun_bullets::Result miss = Decide(Resource::Particles, p.data(), p.size(), "aa", {"bb", "cc"});
  EXPECT_EQ(miss.verdict, Verdict::NotStock);
  EXPECT_TRUE(miss.bytes.empty());
  // Any pin of the list, the first as well as the last.
  for (const char* sha : {"bb", "cc"}) {
    const nevr_netgun_bullets::Result hit = Decide(Resource::Particles, p.data(), p.size(), sha, {"bb", "cc"});
    EXPECT_EQ(hit.verdict, Verdict::Patched) << sha;
    EXPECT_EQ(hit.bytes, PatchedParticles()) << sha;
  }
  const std::vector<uint8_t> b = Bullets(StockEntries());
  EXPECT_EQ(Decide(Resource::Bullets, b.data(), b.size(), "bb", {"bb"}).bytes, PatchedBullets());
}

TEST(NetgunBulletDecide, APinnedBufferWithoutTheStockLayoutIsRefusedAndNothingIsServed) {
  const std::vector<uint8_t> p = Particles(StockRows());
  const nevr_netgun_bullets::Result r = Decide(Resource::Bullets, p.data(), p.size(), "aa", {"aa"});
  EXPECT_EQ(r.verdict, Verdict::Refused);
  EXPECT_TRUE(r.bytes.empty());
}

TEST(NetgunBulletDecide, ThePinsAreTheStockDigests) {
  const std::string particle = "7130808e89f3aa9400cb09fb7a30ca85c9a5a0be363557a647974ebd00283b8c";
  const std::string gauss = "9f03344fa5e5ea183ffed2b42186a1fa6b7e214953dcbfa1c0308389e9f05750";
  const std::string bullet = "611f0ab4d6ada7f275ab1aaf974a08209d3c6e78e1a81b40c88fa11d9a215e6b";
  EXPECT_EQ(nevr_netgun_bullets::StockPins(Resource::Particles), (std::vector<std::string>{particle, gauss}));
  EXPECT_EQ(nevr_netgun_bullets::StockPins(Resource::Bullets), (std::vector<std::string>{bullet}));
}

TEST(NetgunBulletDecide, TheNamesALogLineCarriesAreStable) {
  EXPECT_STREQ(nevr_netgun_bullets::ResourceName(Resource::Particles), "CParticleEffectCR");
  EXPECT_STREQ(nevr_netgun_bullets::ResourceName(Resource::Bullets), "CR15NetBulletCR");
  const std::pair<Status, const char*> statuses[] = {
      {Status::Ok, "ok"},
      {Status::Truncated, "truncated"},
      {Status::BadHeader, "bad_header"},
      {Status::NoDonor, "no_donor"},
      {Status::AlreadyPresent, "already_present"},
      {Status::MissingBullet, "missing_bullet"},
  };
  for (const auto& s : statuses) EXPECT_STREQ(nevr_netgun_bullets::StatusName(s.first), s.second);
}

}  // namespace
