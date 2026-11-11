// NEUTRON's effects drain (patch/ecfx_rules.h, #2349). The callers are runtime/combat/dronefix.cpp (PC: WallFxDue, RunWallFx) and
// quest/combat/ecfx.cpp (Quest: TickOnce); the queue itself, the reads of a wall and the call of an entry need the game and are proven by a
// live run. The Quest host test (quest/tests/ecfx_test.cpp) compiles this same header.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "core/burst_then_count.h"
#include "runtime/patch/ecfx_rules.h"

namespace {

namespace fx = nevr_ecfx;

constexpr uint64_t kE0 = 0x10000, kE1 = 0x10010, kE2 = 0x10020, kE3 = 0x10030;  // 16-aligned, as the defer stub writes them
constexpr uint64_t kUnreadable = 0x40;                                           // a node whose wall cannot be read

uint64_t WallOf(uint64_t ctx) { return ctx == kUnreadable ? 0 : 0x9000 + ctx; }

class EcfxDrain : public ::testing::Test {
 protected:
  alignas(16) uint64_t q[2 * fx::kSlots] = {};
  fx::Drain d;
  fx::Counts c;
  std::vector<fx::Run> runs;
  bool heldDuringRun = true;  // every slot still held (node and word) while its entry ran

  void Put(int slot, uint64_t ctx, uint64_t word) {
    q[2 * slot] = ctx;
    q[2 * slot + 1] = word;
  }
  fx::Env Env(const fx::Entries* entries = nullptr) {
    fx::Env env;
    env.entries = entries;
    env.wallOf = &WallOf;
    return env;
  }
  // A drain whose entries are recorded, not called; `during` runs inside each entry (what an effect that queues would do).
  template <class During>
  int Drain(uint64_t now, const fx::Env& env, During during) {
    c = fx::Counts();
    runs.clear();
    return d.Plan(q, env, now, [&](const fx::Run& r) {
      heldDuringRun = heldDuringRun && q[2 * r.slot] == r.ctx && q[2 * r.slot + 1] != 0;
      runs.push_back(r);
      during(r);
    }, &c);
  }
  int Drain(uint64_t now, const fx::Env& env) {
    return Drain(now, env, [](const fx::Run&) {});
  }
  int Drain(uint64_t now) { return Drain(now, Env()); }
  bool Empty(int slot) const { return q[2 * slot] == 0 && q[2 * slot + 1] == 0; }
};

TEST(EcfxRules, TheConstantsAreV200s) {
  EXPECT_EQ(fx::kSlots, 6);
  EXPECT_EQ(fx::kBoom, 2);
  EXPECT_EQ(fx::kSound, 3);
  EXPECT_EQ(fx::kStaleMs, 500u);
  EXPECT_EQ(fx::kSoundMs, 3000u);
  EXPECT_EQ(fx::kSoundWalls, 8);
}

TEST_F(EcfxDrain, AnEmptyQueueRunsNothing) {
  EXPECT_EQ(Drain(1000), 0);
  EXPECT_EQ(c.run, 0);
}

TEST_F(EcfxDrain, ANodesLowestKindRunsAndItsOtherSlotWaitsForTheNextDrain) {
  Put(0, 0x100, kE1 | fx::kBurst);
  Put(1, 0x100, kE0 | fx::kCharge);
  ASSERT_EQ(Drain(1000), 1);
  EXPECT_EQ(runs[0].ctx, 0x100u);
  EXPECT_EQ(runs[0].fn, kE0);
  EXPECT_EQ(runs[0].kind, fx::kCharge);
  EXPECT_EQ(runs[0].slot, 1);
  EXPECT_TRUE(heldDuringRun) << "a slot is freed only after its entry returned";
  EXPECT_TRUE(Empty(1));
  EXPECT_EQ(q[0], 0x100u);
  EXPECT_EQ(q[1], kE1 | fx::kBurst);
  ASSERT_EQ(Drain(1010), 1);
  EXPECT_EQ(runs[0].kind, fx::kBurst);
  EXPECT_TRUE(Empty(0));
}

TEST_F(EcfxDrain, EveryNodeRunsInOneDrain) {
  Put(0, 0x100, kE0);
  Put(1, 0x200, kE1 | fx::kBurst);
  Put(2, 0x300, kE0);
  EXPECT_EQ(Drain(1000), 3);
  EXPECT_TRUE(Empty(0) && Empty(1) && Empty(2));
}

TEST_F(EcfxDrain, TheBoomIsFreedAndNeverRun) {
  Put(0, 0x100, kE2 | fx::kBoom);
  EXPECT_EQ(Drain(1000), 0);
  EXPECT_EQ(c.boom, 1);
  EXPECT_TRUE(Empty(0));
}

TEST_F(EcfxDrain, ASlotStillBeingWrittenIsLeftAlone) {
  Put(0, 0x100, 0);
  EXPECT_EQ(Drain(1000), 0);
  EXPECT_EQ(c.inflight, 1);
  EXPECT_EQ(q[0], 0x100u);
  q[1] = kE0;  // the writer completes it
  EXPECT_EQ(Drain(1010), 1);
  // a slot of the same node still being written is not a candidate for the lowest kind
  Put(0, 0x100, kE1 | fx::kBurst);
  Put(1, 0x100, 0);
  ASSERT_EQ(Drain(1020), 1);
  EXPECT_EQ(runs[0].slot, 0);
  EXPECT_EQ(q[2], 0x100u);
}

TEST_F(EcfxDrain, AQueueNotDrainedForMoreThan500msIsFreedUnrun) {
  Put(0, 0x100, kE0);
  Put(1, 0x200, kE1 | fx::kBurst);
  Put(2, 0x300, 0);
  EXPECT_EQ(Drain(5000), 2) << "the first drain is never stale";
  Put(0, 0x100, kE0);
  Put(1, 0x200, kE1 | fx::kBurst);
  EXPECT_EQ(Drain(5500), 2) << "exactly 500 ms is not stale";
  Put(0, 0x100, kE0);
  Put(1, 0x200, kE1 | fx::kBurst);
  EXPECT_EQ(Drain(6001), 0);
  EXPECT_EQ(c.stale, 2);
  EXPECT_TRUE(Empty(0) && Empty(1));
  EXPECT_EQ(q[4], 0x300u) << "a slot still being written stays";
  Put(0, 0x100, kE0);
  EXPECT_EQ(Drain(6011), 1) << "the drain after a stale one runs again";
  EXPECT_EQ(c.stale, 0);
}

TEST_F(EcfxDrain, OneSoundPerWallPer3000ms) {
  Put(0, 0x100, kE3 | fx::kSound);
  EXPECT_EQ(Drain(1000), 1);
  Put(0, 0x100, kE3 | fx::kSound);
  EXPECT_EQ(Drain(1400), 0);
  EXPECT_EQ(c.sound, 1);
  EXPECT_TRUE(Empty(0));
  for (uint64_t t = 1800; t < 3999; t += 400) Drain(t);  // drains in between: a gap over 500 ms would make the queue stale
  Put(0, 0x100, kE3 | fx::kSound);
  EXPECT_EQ(Drain(3999), 0) << "2999 ms after the last is a repeat";
  Put(0, 0x100, kE3 | fx::kSound);
  EXPECT_EQ(Drain(4000), 1) << "3000 ms after the last plays again";
  Put(0, 0x200, kE3 | fx::kSound);
  EXPECT_EQ(Drain(4010), 1) << "another wall is not a repeat";
}

TEST(EcfxRules, TheNinthWallReplacesTheOldest) {
  fx::Sounds s;
  for (uint64_t w = 1; w <= 8; ++w) EXPECT_FALSE(s.Again(w, 10000 + w));
  EXPECT_FALSE(s.Again(9, 10100));
  EXPECT_FALSE(s.Again(1, 10101)) << "the oldest wall was forgotten";
  EXPECT_TRUE(s.Again(9, 10102));
}

// EC marks the node as run before its boom check (EC 445, then 450): a boom uses up its node's turn this drain.
TEST_F(EcfxDrain, ABoomUsesUpItsNodesTurn) {
  Put(0, 0x100, kE2 | fx::kBoom);
  Put(1, 0x100, kE3 | fx::kSound);
  EXPECT_EQ(Drain(1000), 0);
  EXPECT_EQ(c.boom, 1);
  EXPECT_TRUE(Empty(0));
  EXPECT_EQ(q[2], 0x100u) << "the node's sound waits for the next drain";
  EXPECT_EQ(Drain(1010), 1);
  EXPECT_EQ(runs[0].kind, fx::kSound);
}

// The lowest kind is looked for in the slots after this one (EC 442, j = i + 1): an earlier slot of the same node, written by a
// running entry after the drain passed it, waits for the next drain.
TEST_F(EcfxDrain, TheLowestKindIsLookedForAfterTheSlotOnly) {
  Put(2, 0x100, kE0);
  int queued = 0;
  const int n = Drain(1000, Env(), [&](const fx::Run& r) {
    if (r.ctx != 0x100 || queued++) return;
    Put(0, 0x200, kE0 | fx::kCharge);  // a passed slot, node 0x200's charge
    Put(4, 0x200, kE1 | fx::kBurst);   // a later slot, node 0x200's burst
  });
  ASSERT_EQ(n, 2);
  EXPECT_EQ(runs[1].slot, 4);
  EXPECT_EQ(runs[1].kind, fx::kBurst);
  EXPECT_EQ(q[0], 0x200u) << "the passed slot waits";
  EXPECT_EQ(q[1], kE0 | fx::kCharge);
}

// D2: a word the defer stub never writes is freed and not run.
TEST_F(EcfxDrain, AWordWithNoEntryIsFreedAndNotRun) {
  Put(0, 0x100, fx::kBurst);
  EXPECT_EQ(Drain(1000), 0);
  EXPECT_EQ(c.refused, 1);
  EXPECT_TRUE(Empty(0));
}

TEST_F(EcfxDrain, AWordWithBit2Or3SetIsFreedAndNotRun) {
  Put(0, 0x100, kE0 | 4);
  Put(1, 0x200, kE1 | 8 | fx::kBurst);
  EXPECT_EQ(Drain(1000), 0);
  EXPECT_EQ(c.refused, 2);
  EXPECT_TRUE(Empty(0) && Empty(1));
}

// D3: a wall of 0 is WallOf's read-failure value. The clock is past 3000 ms, where an empty remembered slot (D4) cannot hide it.
TEST_F(EcfxDrain, ASoundWhoseWallCannotBeReadIsFreedAndNotRun) {
  Put(0, kUnreadable, kE3 | fx::kSound);
  EXPECT_EQ(Drain(10000), 0);
  EXPECT_EQ(c.sound, 1);
  EXPECT_EQ(c.nowall, 1);
  EXPECT_TRUE(Empty(0));
  Put(0, 0x100, kE3 | fx::kSound);
  fx::Env none;  // no wall read at all
  EXPECT_EQ(Drain(10010, none), 0);
  EXPECT_EQ(c.nowall, 1);
}

// D4: an empty remembered slot matches no wall.
TEST(EcfxRules, AnEmptyRememberedSlotIsNotARepeat) {
  fx::Sounds s;
  EXPECT_FALSE(s.Again(0, 1000));
}

// D1: the exact-entry check is an Env input.
TEST_F(EcfxDrain, WithTheEntriesKnownOnlyTheExactEntryForItsKindRuns) {
  fx::Entries e;
  e.entry[0] = kE0;
  e.entry[1] = kE1;
  e.entry[2] = kE2;
  e.entry[3] = kE3;
  Put(0, 0x100, 0x20000 | fx::kCharge);  // not the entry
  Put(1, 0x200, kE1 | fx::kCharge);      // the entry of another kind
  Put(2, 0x300, kE0 | fx::kCharge);      // the right one
  ASSERT_EQ(Drain(1000, Env(&e)), 1);
  EXPECT_EQ(runs[0].ctx, 0x300u);
  EXPECT_EQ(c.refused, 2);
  fx::Entries unknown;  // a kind whose entry is unknown never runs
  Put(0, 0x100, kE0);
  EXPECT_EQ(Drain(1010, Env(&unknown)), 0);
  EXPECT_EQ(c.refused, 1);
}

TEST_F(EcfxDrain, WithTheCheckOffAnyEntryRuns) {
  Put(0, 0x100, 0x20000 | fx::kCharge);
  Put(1, 0x200, kE1 | fx::kCharge);
  EXPECT_EQ(Drain(1000), 2);
  EXPECT_EQ(c.refused, 0);
}

// D5: each entry runs when the drain reaches its slot (EC's order), so an effect a running entry queues into a later slot, for a node not
// yet run this drain, runs this drain; one queued into a slot already passed, or for a node that already ran, waits for the next.
TEST_F(EcfxDrain, AnEffectQueuedByARunningEntryIntoALaterSlotRunsThisDrain) {
  Put(1, 0x100, kE0);
  int queued = 0;
  const int n = Drain(1000, Env(), [&](const fx::Run& r) {
    if (r.ctx != 0x100 || queued++) return;
    Put(3, 0x200, kE1 | fx::kBurst);  // a later slot, another node
    Put(0, 0x300, kE0);               // a slot already passed
    Put(4, 0x100, kE1 | fx::kBurst);  // a node that already ran
  });
  ASSERT_EQ(n, 2);
  EXPECT_EQ(runs[1].ctx, 0x200u);
  EXPECT_TRUE(Empty(3));
  EXPECT_EQ(q[0], 0x300u);
  EXPECT_EQ(q[8], 0x100u);
  EXPECT_EQ(Drain(1010), 2);
}

// The Warning budget the PC drain and the follow update's faults use (core/burst_then_count.h): the first `burst` each, then one that says
// the later ones are counted, then the count at most every 10 s, and once none has come for 10 s.
TEST(BurstThenCount, TheFirstBurstAreEachWrittenThenOneSaysTheRestAreCounted) {
  nevr::BurstThenCount b{3, 10000};
  for (uint64_t i = 1; i <= 3; ++i) {
    const auto l = b.Event(1000 + i);
    EXPECT_EQ(l.verdict, nevr::BurstThenCount::Verdict::kWrite);
    EXPECT_EQ(l.total, i);
  }
  const auto capped = b.Event(1004);
  EXPECT_EQ(capped.verdict, nevr::BurstThenCount::Verdict::kCapped);
  EXPECT_EQ(capped.total, 4u);
  EXPECT_FALSE(b.Pending());
  EXPECT_EQ(b.Event(1005).verdict, nevr::BurstThenCount::Verdict::kCounted);
  EXPECT_TRUE(b.Pending());
}

TEST(BurstThenCount, TheCountGoesOutOnTheFirstEventTenSecondsAfterTheLastLine) {
  nevr::BurstThenCount b{1, 10000};
  b.Event(1000);                                                       // written
  b.Event(1001);                                                       // capped: the last line at 1001
  EXPECT_EQ(b.Event(11000).verdict, nevr::BurstThenCount::Verdict::kCounted);  // 9999 ms after the last line
  const auto l = b.Event(11001);
  EXPECT_EQ(l.verdict, nevr::BurstThenCount::Verdict::kCount);
  EXPECT_EQ(l.total, 4u);
  EXPECT_EQ(l.sinceLine, 2u);
  EXPECT_FALSE(b.Pending());
}

TEST(BurstThenCount, AFlushWritesThePendingCountAfterTenSecondsOfQuiet) {
  nevr::BurstThenCount b{1, 10000};
  b.Event(1000);
  b.Event(1001);
  b.Event(2000);
  b.Event(3000);
  EXPECT_EQ(b.Flush(12999).verdict, nevr::BurstThenCount::Verdict::kCounted) << "9999 ms of quiet";
  const auto l = b.Flush(13000);
  EXPECT_EQ(l.verdict, nevr::BurstThenCount::Verdict::kCount);
  EXPECT_EQ(l.total, 4u);
  EXPECT_EQ(l.sinceLine, 2u);
  EXPECT_EQ(b.Flush(30000).verdict, nevr::BurstThenCount::Verdict::kCounted) << "nothing pending: nothing to write";
}

TEST(BurstThenCount, NothingIsFlushedWhileEveryEventWasWritten) {
  nevr::BurstThenCount b{16, 10000};
  b.Event(1000);
  EXPECT_FALSE(b.Pending());
  EXPECT_EQ(b.Flush(50000).verdict, nevr::BurstThenCount::Verdict::kCounted);
}

}  // namespace
