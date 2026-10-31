// #1420: the netgun entries of the four unfinished weapons. The rule is pure (runtime/patch/netgun_bullet_ids.h); these tests
// build a synthetic resource in memory, with no game bytes, in the layout the header names. The pin is injected by the
// caller's digest string only where the rule takes one; the stock digest itself is checked as a constant.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/patch/netgun_bullet_ids.h"

namespace {

namespace nb = nevr_netgun_bullet_ids;

constexpr uint64_t kOtherBullet = 0xA000000000000001ULL;

void WriteU64(std::vector<uint8_t>& b, size_t at, uint64_t v) {
  for (int i = 0; i < 8; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

uint64_t ReadU64(const std::vector<uint8_t>& b, size_t at) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | b[at + i];
  return v;
}

size_t EntryAt(size_t i) { return nb::kHeaderBytes + i * nb::kEntryBytes; }

// A resource with the stock layout: 120 entries in eight kinds of 15, the assault's bullet on entries 0-14, a distinct
// bullet on each of the other entries, and filler bytes that no rule touches.
std::vector<uint8_t> Synthetic(uint64_t assaultBullet = nb::kAssaultBulletId) {
  std::vector<uint8_t> b(nb::kTotalBytes);
  for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>(0x5A ^ (i * 7));
  WriteU64(b, 8, nb::kEntries * nb::kEntryBytes);
  WriteU64(b, 40, nb::kEntries);
  WriteU64(b, 48, nb::kEntries);
  for (size_t i = 0; i < nb::kEntries; ++i) {
    const size_t e = EntryAt(i);
    WriteU64(b, e + nb::kKindOffset, 0xC000 + i / nb::kGroupEntries);
    const uint64_t bullet = i <= nb::kAssaultLast ? assaultBullet : kOtherBullet + i;
    WriteU64(b, e + nb::kBulletOffset, bullet);
  }
  return b;
}

// The stock bullet ids of the four kinds are distinct from the assault's, so a rewrite is visible.
TEST(NetgunBulletIds, StockDigestsAreThePinnedValues) {
  EXPECT_EQ(std::string(nb::kStockSha256), "c28109544de935ec480f9c7666914971ba5c17b659e8e2b74dbea59a24373c37");
  EXPECT_EQ(std::string(nb::kPluginFileSha256), "30d60a0ba49bfc6ce353289ed362fad5f34cfd91dc4c826d99d9d482ccf037e1");
  EXPECT_EQ(nb::kTotalBytes, 19256u);
  EXPECT_EQ(nb::kNameCount, 5u);
}

TEST(NetgunBulletIds, PinnedBytesGetTheAssaultIdOnTheFourKindsOnly) {
  const std::vector<uint8_t> stock = Synthetic();
  const nb::Result r = nb::Patch(stock.data(), stock.size(), nb::kStockSha256);
  ASSERT_EQ(r.verdict, nb::Verdict::Patched) << r.reason;
  EXPECT_EQ(r.rewritten, 60u);
  EXPECT_EQ(r.assaultBulletId, nb::kAssaultBulletId);
  ASSERT_EQ(r.bytes.size(), stock.size());

  for (size_t i = 0; i < nb::kEntries; ++i) {
    const uint64_t want = i >= nb::kRewriteFirst ? nb::kAssaultBulletId : ReadU64(stock, EntryAt(i) + nb::kBulletOffset);
    EXPECT_EQ(ReadU64(r.bytes, EntryAt(i) + nb::kBulletOffset), want) << "entry " << i;
  }
}

TEST(NetgunBulletIds, OnlyTheBulletIdChanges) {
  const std::vector<uint8_t> stock = Synthetic();
  const nb::Result r = nb::Patch(stock.data(), stock.size(), nb::kStockSha256);
  ASSERT_EQ(r.verdict, nb::Verdict::Patched);
  for (size_t i = 0; i < stock.size(); ++i) {
    bool inBullet = false;
    for (size_t e = nb::kRewriteFirst; e < nb::kEntries && !inBullet; ++e) {
      const size_t at = EntryAt(e) + nb::kBulletOffset;
      inBullet = i >= at && i < at + 8;
    }
    if (!inBullet) {
      EXPECT_EQ(r.bytes[i], stock[i]) << "byte " << i;
    }
  }
}

TEST(NetgunBulletIds, AnotherDigestIsLeftAlone) {
  const std::vector<uint8_t> stock = Synthetic();
  const nb::Result r = nb::Patch(stock.data(), stock.size(), nb::kPluginFileSha256);
  EXPECT_EQ(r.verdict, nb::Verdict::NotStock);
  EXPECT_TRUE(r.bytes.empty());
}

TEST(NetgunBulletIds, AlreadyRewrittenBufferIsNotThePin) {
  std::vector<uint8_t> stock = Synthetic();
  const nb::Result once = nb::Patch(stock.data(), stock.size(), nb::kStockSha256);
  ASSERT_EQ(once.verdict, nb::Verdict::Patched);
  // Its digest is the plugin file's, not the stock pin, so the rule does not run again.
  const nb::Result twice = nb::Patch(once.bytes.data(), once.bytes.size(), nb::kPluginFileSha256);
  EXPECT_EQ(twice.verdict, nb::Verdict::NotStock);
}

TEST(NetgunBulletIds, WrongSizeIsRefused) {
  std::vector<uint8_t> stock = Synthetic();
  stock.pop_back();
  const nb::Result r = nb::Patch(stock.data(), stock.size(), nb::kStockSha256);
  EXPECT_EQ(r.verdict, nb::Verdict::Refused);
  EXPECT_EQ(r.reason, "size");
  EXPECT_TRUE(r.bytes.empty());
}

TEST(NetgunBulletIds, WrongDataSizeIsRefused) {
  std::vector<uint8_t> stock = Synthetic();
  WriteU64(stock, 8, 19199);
  const nb::Result r = nb::Patch(stock.data(), stock.size(), nb::kStockSha256);
  EXPECT_EQ(r.verdict, nb::Verdict::Refused);
  EXPECT_EQ(r.reason, "data_size");
}

TEST(NetgunBulletIds, WrongCapacityIsRefused) {
  std::vector<uint8_t> stock = Synthetic();
  WriteU64(stock, 40, 121);
  const nb::Result r = nb::Patch(stock.data(), stock.size(), nb::kStockSha256);
  EXPECT_EQ(r.verdict, nb::Verdict::Refused);
  EXPECT_EQ(r.reason, "capacity");
}

TEST(NetgunBulletIds, WrongCountIsRefused) {
  std::vector<uint8_t> stock = Synthetic();
  WriteU64(stock, 48, 119);
  const nb::Result r = nb::Patch(stock.data(), stock.size(), nb::kStockSha256);
  EXPECT_EQ(r.verdict, nb::Verdict::Refused);
  EXPECT_EQ(r.reason, "count");
}

TEST(NetgunBulletIds, AssaultIdsThatDifferAreRefused) {
  std::vector<uint8_t> stock = Synthetic();
  WriteU64(stock, EntryAt(7) + nb::kBulletOffset, kOtherBullet);
  const nb::Result r = nb::Patch(stock.data(), stock.size(), nb::kStockSha256);
  EXPECT_EQ(r.verdict, nb::Verdict::Refused);
  EXPECT_EQ(r.reason, "assault_ids_differ");
}

TEST(NetgunBulletIds, NoAssaultBulletIsRefused) {
  const std::vector<uint8_t> stock = Synthetic(0);
  const nb::Result r = nb::Patch(stock.data(), stock.size(), nb::kStockSha256);
  EXPECT_EQ(r.verdict, nb::Verdict::Refused);
  EXPECT_EQ(r.reason, "assault_bullet_missing");
}

TEST(NetgunBulletIds, KindsThatSplitAGroupAreRefused) {
  std::vector<uint8_t> stock = Synthetic();
  WriteU64(stock, EntryAt(70) + nb::kKindOffset, 0xDEAD);
  const nb::Result r = nb::Patch(stock.data(), stock.size(), nb::kStockSha256);
  EXPECT_EQ(r.verdict, nb::Verdict::Refused);
  EXPECT_EQ(r.reason, "kind_groups_differ");
}

TEST(NetgunBulletIds, NullBufferIsNotPatched) {
  const nb::Result r = nb::Patch(nullptr, 0, nb::kStockSha256);
  EXPECT_EQ(r.verdict, nb::Verdict::Refused);
  EXPECT_STREQ(nb::VerdictName(r.verdict), "refused");
}

}  // namespace
