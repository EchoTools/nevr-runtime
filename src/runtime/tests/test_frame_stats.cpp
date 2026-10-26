// #954: the pure half of the PC frame-time probe (runtime/frame/frame_stats.h): the config switch, the
// loop-head return-address predicate, the ring, the ovrPerfStats extraction, the window math and the two log
// lines. The hook and the log call (frame_stats.cpp) are not linked here; the live run proves those.

#include "runtime/frame/frame_stats.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace fs = nevr_frame_stats;

namespace {

// Feeds a collector the loop-head readings whose gaps are `intervals` (the first reading only sets the baseline).
void FeedIntervals(fs::Collector& c, const std::vector<uint32_t>& intervals, uint64_t startUs = 1000000) {
  uint64_t t = startUs;
  c.AddFrame(t, 0);
  for (uint32_t i : intervals) {
    t += i;
    c.AddFrame(t, 0);
  }
}

std::unique_ptr<fs::Collector> NewCollector() { return std::make_unique<fs::Collector>(); }

// An ovrPerfStats with `count` entries and a sentinel in every word; entry i gets appDropped=100+i, compositorDropped=200+i,
// appCpu=(i+1) ms, appGpu=(i+1)*2 ms.
std::vector<uint8_t> MakeStats(int32_t count) {
  std::vector<uint8_t> buf(0x17C, 0);
  std::memcpy(buf.data() + 0x168, &count, sizeof(count));  // FrameStatsCount, literal: the SDK layout, not our constant
  for (int32_t i = 0; i < 5; ++i) {
    uint8_t* entry = buf.data() + static_cast<std::size_t>(i) * 0x48;  // literal stride
    // Every word of the entry holds a sentinel first, so a field read from the wrong offset returns a value
    // that is not the expected one (a zeroed neighbour would let an off-by-one-word offset pass).
    for (uint32_t w = 0; w < 0x48 / 4; ++w) {
      const uint32_t sentinel = 0x01000000u + static_cast<uint32_t>(i) * 0x100u + w;
      std::memcpy(entry + w * 4, &sentinel, sizeof(sentinel));
    }
    const int32_t appDropped = 100 + i;
    const int32_t compDropped = 200 + i;
    const float cpu = static_cast<float>(i + 1) * 0.001f;
    const float gpu = static_cast<float>(i + 1) * 0.002f;
    std::memcpy(entry + 0x08, &appDropped, sizeof(appDropped));
    std::memcpy(entry + 0x20, &compDropped, sizeof(compDropped));
    std::memcpy(entry + 0x14, &cpu, sizeof(cpu));
    std::memcpy(entry + 0x18, &gpu, sizeof(gpu));
  }
  return buf;
}

}  // namespace

// ---- the switch ------------------------------------------------------------------------------------------

TEST(FrameStatsMode, AbsentKeyIsOffAndRecognised) {
  bool recognised = false;
  EXPECT_EQ(fs::Mode::Off, fs::ParseMode(std::nullopt, &recognised));
  EXPECT_TRUE(recognised);
}

TEST(FrameStatsMode, TrueSpellingsAreFullFalseSpellingsAreOffPerfIsPerfOnly) {
  for (const char* v : {"true", "True", "YES", "on", "1"}) {
    bool recognised = false;
    EXPECT_EQ(fs::Mode::Full, fs::ParseMode(std::string(v), &recognised)) << v;
    EXPECT_TRUE(recognised) << v;
  }
  for (const char* v : {"false", "False", "no", "OFF", "0"}) {
    bool recognised = false;
    EXPECT_EQ(fs::Mode::Off, fs::ParseMode(std::string(v), &recognised)) << v;
    EXPECT_TRUE(recognised) << v;
  }
  bool recognised = false;
  EXPECT_EQ(fs::Mode::PerfOnly, fs::ParseMode(std::string("PERF"), &recognised));
  EXPECT_TRUE(recognised);
}

TEST(FrameStatsMode, AnythingElseIsOffAndNotRecognised) {
  for (const char* v : {"maybe", "", "fulll", "2"}) {
    bool recognised = true;
    EXPECT_EQ(fs::Mode::Off, fs::ParseMode(std::string(v), &recognised)) << v;
    EXPECT_FALSE(recognised) << v;
  }
  EXPECT_EQ(fs::Mode::Off, fs::ParseMode(std::string("maybe"), nullptr));  // the out-parameter is optional
}

TEST(FrameStatsMode, NamesAreTheLoggedSpellings) {
  EXPECT_STREQ("off", fs::ModeName(fs::Mode::Off));
  EXPECT_STREQ("full", fs::ModeName(fs::Mode::Full));
  EXPECT_STREQ("perf", fs::ModeName(fs::Mode::PerfOnly));
}

// ---- the loop head ---------------------------------------------------------------------------------------

TEST(FrameStatsLoopHead, OnlyTheInstructionAfterTheLoopHeadCallMatches) {
  const uintptr_t base = 0x7ff600000000;
  EXPECT_TRUE(fs::IsLoopHeadReturn(base + 0x50BD4C, base));
  EXPECT_FALSE(fs::IsLoopHeadReturn(base + 0x50BD4B, base));
  EXPECT_FALSE(fs::IsLoopHeadReturn(base + 0x50BD4D, base));
  EXPECT_FALSE(fs::IsLoopHeadReturn(base + 0x50BD47, base));  // the call itself, not its return address
  EXPECT_FALSE(fs::IsLoopHeadReturn(0x50BD4C, base));         // an RVA is not an address
}

TEST(FrameStatsLoopHead, AnotherCallerOfGetTimeMicrosecondsDoesNotMatch) {
  const uintptr_t base = 0x140000000;
  // The hook is entered from many places; the physics step and the network code are not the loop head.
  for (uintptr_t rva : {uintptr_t{0xCD510}, uintptr_t{0x1B8650}, uintptr_t{0xF80ED0}, uintptr_t{0x50BD00}}) {
    EXPECT_FALSE(fs::IsLoopHeadReturn(base + rva, base)) << std::hex << rva;
  }
}

TEST(FrameStatsLoopHead, ABaseOfZeroNeverMatches) {
  EXPECT_FALSE(fs::IsLoopHeadReturn(nevr_patch_addresses::FRAME_LOOP_HEAD_RETURN, 0));
}

TEST(FrameStatsLoopHead, TheCallBytesNameGetTimeMicrosecondsThroughTheirDisplacement) {
  // E8 rel32: the target is the address after the instruction plus the signed displacement.
  int32_t rel = 0;
  std::memcpy(&rel, fs::kLoopHeadCallBytes + 1, sizeof(rel));
  const int64_t returnVa = 0x140000000LL + static_cast<int64_t>(nevr_patch_addresses::FRAME_LOOP_HEAD_RETURN);
  EXPECT_EQ(0xE8, fs::kLoopHeadCallBytes[0]);
  EXPECT_EQ(0x140000000LL + static_cast<int64_t>(nevr_patch_addresses::GET_TIME_MICROSECONDS), returnVa + rel);
  EXPECT_EQ(nevr_patch_addresses::FRAME_LOOP_HEAD_RETURN - 5, fs::kLoopHeadCallRva);
}

TEST(FrameStatsLoopHead, TheCallSiteCheckAcceptsTheBytesAndRejectsAnyOtherOrNull) {
  uint8_t site[5];
  std::memcpy(site, fs::kLoopHeadCallBytes, sizeof(site));
  EXPECT_TRUE(fs::LoopHeadCallMatches(site));
  for (std::size_t i = 0; i < sizeof(site); ++i) {
    uint8_t changed[5];
    std::memcpy(changed, site, sizeof(changed));
    changed[i] ^= 0x01;
    EXPECT_FALSE(fs::LoopHeadCallMatches(changed)) << "byte " << i;
  }
  EXPECT_FALSE(fs::LoopHeadCallMatches(nullptr));
}

// ---- the ring --------------------------------------------------------------------------------------------

TEST(FrameStatsRing, ReturnsWhatWasPushedInOrderAcrossAWrap) {
  fs::SpscRing<uint32_t, 4> ring;
  uint32_t next = 0, expect = 0, v = 0;
  for (int round = 0; round < 10; ++round) {  // 10 x 3 pushes wraps a 4-slot ring several times
    for (int i = 0; i < 3; ++i) EXPECT_TRUE(ring.Push(next++));
    for (int i = 0; i < 3; ++i) {
      ASSERT_TRUE(ring.Pop(v));
      EXPECT_EQ(expect++, v);
    }
  }
  EXPECT_FALSE(ring.Pop(v));
  EXPECT_EQ(0u, ring.Dropped());
}

TEST(FrameStatsRing, AFullRingDropsTheNewEntryAndCountsIt) {
  fs::SpscRing<uint32_t, 4> ring;
  for (uint32_t i = 0; i < 4; ++i) EXPECT_TRUE(ring.Push(i));
  EXPECT_FALSE(ring.Push(100));
  EXPECT_FALSE(ring.Push(101));
  EXPECT_EQ(2u, ring.Dropped());
  uint32_t v = 0;
  for (uint32_t i = 0; i < 4; ++i) {  // the oldest four survive
    ASSERT_TRUE(ring.Pop(v));
    EXPECT_EQ(i, v);
  }
  EXPECT_FALSE(ring.Pop(v));
  EXPECT_TRUE(ring.Push(7));  // room again after a pop
}

TEST(FrameStatsRing, ASlotsDroppedCountSeparatesReadingsEitherSideOfALoss) {
  fs::SpscRing<fs::FrameSlot, 2> ring;
  EXPECT_TRUE(ring.Push(fs::FrameSlot{10, ring.Dropped()}));
  EXPECT_TRUE(ring.Push(fs::FrameSlot{20, ring.Dropped()}));
  EXPECT_FALSE(ring.Push(fs::FrameSlot{30, ring.Dropped()}));  // lost
  fs::FrameSlot slot;
  ASSERT_TRUE(ring.Pop(slot));
  EXPECT_TRUE(ring.Push(fs::FrameSlot{40, ring.Dropped()}));
  ASSERT_TRUE(ring.Pop(slot));
  EXPECT_EQ(20u, slot.us);
  EXPECT_EQ(0u, slot.dropped);
  ASSERT_TRUE(ring.Pop(slot));
  EXPECT_EQ(40u, slot.us);
  EXPECT_EQ(1u, slot.dropped);  // one entry was lost before this one
}

TEST(FrameStatsRing, AWriterThreadAndAReaderThreadLoseNothingUnaccounted) {
  fs::SpscRing<uint32_t, 64> ring;
  constexpr uint32_t kPushes = 200000;
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (uint32_t i = 1; i <= kPushes; ++i) ring.Push(i);
    done.store(true, std::memory_order_release);
  });
  uint32_t last = 0, v = 0;
  uint64_t popped = 0;
  bool inOrder = true;
  for (;;) {
    if (ring.Pop(v)) {
      inOrder = inOrder && v > last;  // a full ring leaves gaps; it never reorders
      last = v;
      ++popped;
      continue;
    }
    if (done.load(std::memory_order_acquire)) {
      while (ring.Pop(v)) {  // whatever landed between the last empty Pop and the flag
        inOrder = inOrder && v > last;
        last = v;
        ++popped;
      }
      break;
    }
    std::this_thread::yield();
  }
  writer.join();
  EXPECT_TRUE(inOrder);
  EXPECT_EQ(static_cast<uint64_t>(kPushes), popped + ring.Dropped());
}

// ---- ovrPerfStats ----------------------------------------------------------------------------------------

TEST(FrameStatsPerf, TheLayoutIsFiveEntriesAndACountInside0x17c) {
  EXPECT_EQ(0x48u, fs::kPerfEntryStride);
  EXPECT_EQ(5u * fs::kPerfEntryStride, 0x168u);  // the count follows the five entries
  EXPECT_EQ(0x168u, fs::kPerfCountOffset);
  EXPECT_LT(fs::kPerfCountOffset + sizeof(int32_t), fs::kPerfStatsSize);
  EXPECT_EQ(0x17Cu, fs::kPerfStatsSize);
  EXPECT_LT(fs::kPerfCompositorDroppedOffset + sizeof(int32_t), fs::kPerfEntryStride);
}

TEST(FrameStatsPerf, EntriesComeOutOldestFirstWithTheirOwnFields) {
  const std::vector<uint8_t> stats = MakeStats(3);
  fs::PerfSample out[fs::kPerfMaxEntries];
  std::size_t rejected = 0;
  ASSERT_EQ(3u, fs::ExtractPerfSamples(stats.data(), out, &rejected));
  EXPECT_EQ(0u, rejected);
  // Entry 0 is the newest, so the oldest of the three is entry 2.
  EXPECT_EQ(102, out[0].appDropped);
  EXPECT_EQ(202, out[0].compositorDropped);
  EXPECT_FLOAT_EQ(0.003f, out[0].appCpuS);
  EXPECT_FLOAT_EQ(0.006f, out[0].appGpuS);
  EXPECT_EQ(100, out[2].appDropped);
  EXPECT_EQ(200, out[2].compositorDropped);
  EXPECT_FLOAT_EQ(0.001f, out[2].appCpuS);
  EXPECT_FLOAT_EQ(0.002f, out[2].appGpuS);
}

TEST(FrameStatsPerf, ZeroEntriesCopiesNothing) {
  const std::vector<uint8_t> stats = MakeStats(0);
  fs::PerfSample out[fs::kPerfMaxEntries];
  std::size_t rejected = 0;
  EXPECT_EQ(0u, fs::ExtractPerfSamples(stats.data(), out, &rejected));
  EXPECT_EQ(0u, rejected);
}

TEST(FrameStatsPerf, AllFiveEntriesAreCopied) {
  const std::vector<uint8_t> stats = MakeStats(5);
  fs::PerfSample out[fs::kPerfMaxEntries];
  ASSERT_EQ(5u, fs::ExtractPerfSamples(stats.data(), out, nullptr));
  EXPECT_EQ(104, out[0].appDropped);
  EXPECT_EQ(100, out[4].appDropped);
}

TEST(FrameStatsPerf, ACountOutsideZeroToFiveIsRejectedNotRead) {
  for (int32_t count : {-1, 6, 1000000}) {
    const std::vector<uint8_t> stats = MakeStats(count);
    fs::PerfSample out[fs::kPerfMaxEntries];
    std::size_t rejected = 0;
    EXPECT_EQ(0u, fs::ExtractPerfSamples(stats.data(), out, &rejected)) << count;
    EXPECT_EQ(1u, rejected) << count;
  }
}

TEST(FrameStatsPerf, AnEntryWithANegativeOrNonFiniteTimeIsRejected) {
  std::vector<uint8_t> stats = MakeStats(3);
  const float negative = -0.001f;
  const float nan = std::nanf("");
  std::memcpy(stats.data() + 0 * fs::kPerfEntryStride + fs::kPerfAppCpuOffset, &negative, sizeof(negative));
  std::memcpy(stats.data() + 1 * fs::kPerfEntryStride + fs::kPerfAppGpuOffset, &nan, sizeof(nan));
  fs::PerfSample out[fs::kPerfMaxEntries];
  std::size_t rejected = 0;
  ASSERT_EQ(1u, fs::ExtractPerfSamples(stats.data(), out, &rejected));
  EXPECT_EQ(2u, rejected);
  EXPECT_EQ(102, out[0].appDropped);  // only entry 2 survived
}

TEST(FrameStatsPerf, ANullBufferOrOutputCopiesNothing) {
  fs::PerfSample out[fs::kPerfMaxEntries];
  EXPECT_EQ(0u, fs::ExtractPerfSamples(nullptr, out, nullptr));
  const std::vector<uint8_t> stats = MakeStats(2);
  EXPECT_EQ(0u, fs::ExtractPerfSamples(stats.data(), nullptr, nullptr));
}

// ---- the window ------------------------------------------------------------------------------------------

TEST(FrameStatsWindow, MedianP95AndMaxOfAHundredKnownIntervals) {
  auto c = NewCollector();
  std::vector<uint32_t> intervals;
  for (uint32_t i = 100; i >= 1; --i) intervals.push_back(i * 100);  // 10000, 9900 ... 100, unsorted on purpose
  FeedIntervals(*c, intervals);
  const fs::WindowStats w = c->Finish(10000);
  EXPECT_EQ(100u, w.frames);
  EXPECT_EQ(5000u, w.medianUs);  // nearest rank: ceil(0.50 x 100) = 50th value
  EXPECT_EQ(9500u, w.p95Us);     // ceil(0.95 x 100) = 95th value
  EXPECT_EQ(10000u, w.maxUs);
  EXPECT_EQ(0u, w.overruns);
  EXPECT_EQ(10000u, w.elapsedMs);
}

TEST(FrameStatsWindow, AnEvenCountTakesTheLowerMiddleAndP95IsTheTop) {
  auto c = NewCollector();
  FeedIntervals(*c, {40, 10, 30, 20});
  const fs::WindowStats w = c->Finish(0);
  EXPECT_EQ(4u, w.frames);
  EXPECT_EQ(20u, w.medianUs);  // ceil(0.50 x 4) = 2nd
  EXPECT_EQ(40u, w.p95Us);     // ceil(0.95 x 4) = 4th
}

TEST(FrameStatsWindow, OneIntervalIsItsOwnMedianP95AndMax) {
  auto c = NewCollector();
  FeedIntervals(*c, {8333});
  const fs::WindowStats w = c->Finish(0);
  EXPECT_EQ(1u, w.frames);
  EXPECT_EQ(8333u, w.medianUs);
  EXPECT_EQ(8333u, w.p95Us);
  EXPECT_EQ(8333u, w.maxUs);
  EXPECT_EQ(0u, w.over1p5x);
}

TEST(FrameStatsWindow, TheFirstReadingOnlySetsTheBaseline) {
  auto c = NewCollector();
  c->AddFrame(5000, 0);
  const fs::WindowStats w = c->Finish(0);
  EXPECT_EQ(0u, w.frames);
  EXPECT_EQ(0u, w.medianUs);
  EXPECT_EQ(0u, w.maxUs);
}

TEST(FrameStatsWindow, OverOnePointFiveTimesTheMedianIsStrict) {
  auto c = NewCollector();
  // Sorted: 10 10 10 10 10 15 16. The median is 10, so the line is 15: 15 is not over it, 16 is.
  FeedIntervals(*c, {10, 16, 10, 15, 10, 10, 10});
  const fs::WindowStats w = c->Finish(0);
  EXPECT_EQ(10u, w.medianUs);
  EXPECT_EQ(1u, w.over1p5x);
}

TEST(FrameStatsWindow, OverOnePointFiveUsesIntegerArithmeticOnAnOddMedian) {
  auto c = NewCollector();
  // Median 7: 1.5 x 7 = 10.5. 10 is under it, 11 is over it.
  FeedIntervals(*c, {7, 7, 7, 10, 11});
  const fs::WindowStats w = c->Finish(0);
  EXPECT_EQ(7u, w.medianUs);
  EXPECT_EQ(1u, w.over1p5x);
}

TEST(FrameStatsWindow, AGapInTheRingBreaksTheBaselineAndCountsTheLostReadings) {
  auto c = NewCollector();
  c->AddFrame(1000, 0);
  c->AddFrame(1010, 0);  // 10
  c->AddFrame(1020, 0);  // 10
  c->AddFrame(900000, 3);  // three readings were lost before this one: no interval across the gap
  c->AddFrame(900010, 3);  // 10
  const fs::WindowStats w = c->Finish(0);
  EXPECT_EQ(3u, w.frames);
  EXPECT_EQ(10u, w.maxUs);
  EXPECT_EQ(3u, w.overruns);
}

TEST(FrameStatsWindow, ADropCountBelowTheOneAlreadySeenIsIgnored) {
  auto c = NewCollector();
  c->AddFrame(1000, 0);
  c->AddFrame(2000, 3);  // 3 lost: baseline only
  c->AddFrame(2010, 1);  // pushed before two of those drops were counted: neither counts nor breaks
  const fs::WindowStats w = c->Finish(0);
  EXPECT_EQ(1u, w.frames);
  EXPECT_EQ(10u, w.maxUs);
  EXPECT_EQ(3u, w.overruns);
}

TEST(FrameStatsWindow, AClockThatStepsBackwardIsAZeroIntervalNotAHugeOne) {
  auto c = NewCollector();
  c->AddFrame(5000, 0);
  c->AddFrame(4000, 0);
  const fs::WindowStats w = c->Finish(0);
  EXPECT_EQ(1u, w.frames);
  EXPECT_EQ(0u, w.maxUs);
}

TEST(FrameStatsWindow, AnIntervalAboveThirtyTwoBitsIsClamped) {
  auto c = NewCollector();
  c->AddFrame(1, 0);
  c->AddFrame(1 + 0x200000000ULL, 0);
  const fs::WindowStats w = c->Finish(0);
  EXPECT_EQ(UINT32_MAX, w.maxUs);
}

TEST(FrameStatsWindow, IntervalsBeyondTheWindowsCapacityAreCountedAsOverruns) {
  auto c = NewCollector();
  const std::size_t extra = 5;
  std::vector<uint32_t> intervals(fs::kMaxWindowIntervals + extra, 1000);
  FeedIntervals(*c, intervals);
  const fs::WindowStats w = c->Finish(0);
  EXPECT_EQ(fs::kMaxWindowIntervals, w.frames);
  EXPECT_EQ(extra, w.overruns);
  EXPECT_EQ(1000u, w.medianUs);
}

TEST(FrameStatsWindow, FinishStartsTheNextWindowEmptyButKeepsTheBaseline) {
  auto c = NewCollector();
  FeedIntervals(*c, {10, 20}, 1000);  // last reading at 1030
  EXPECT_EQ(2u, c->Finish(0).frames);
  c->AddFrame(1040, 0);  // 10 after the previous window's last reading
  const fs::WindowStats second = c->Finish(0);
  EXPECT_EQ(1u, second.frames);
  EXPECT_EQ(10u, second.maxUs);
  EXPECT_EQ(0u, second.overruns);
  EXPECT_EQ(0u, c->Finish(0).frames);
}

TEST(FrameStatsWindow, ACollectorWithNoPerfSamplesReportsNoPerfFields) {
  auto c = NewCollector();
  FeedIntervals(*c, {10});
  const fs::WindowStats w = c->Finish(0);
  EXPECT_FALSE(w.hasPerf);
  EXPECT_EQ(0u, w.perfSamples);
}

TEST(FrameStatsWindow, CpuAndGpuTimesAreMeanAndMaxInMicroseconds) {
  auto c = NewCollector();
  c->AddPerf(fs::PerfSample{0, 0, 0.002f, 0.001f});
  c->AddPerf(fs::PerfSample{0, 0, 0.004f, 0.009f});
  const fs::WindowStats w = c->Finish(0);
  ASSERT_TRUE(w.hasPerf);
  EXPECT_EQ(2u, w.perfSamples);
  EXPECT_EQ(3000u, w.appCpuMeanUs);
  EXPECT_EQ(4000u, w.appCpuMaxUs);
  EXPECT_EQ(5000u, w.appGpuMeanUs);
  EXPECT_EQ(9000u, w.appGpuMaxUs);
}

TEST(FrameStatsWindow, DroppedCountsAreWindowDeltasOfTheCumulativeCounters) {
  auto c = NewCollector();
  c->AddPerf(fs::PerfSample{5, 50, 0.001f, 0.001f});  // the first sample is the baseline
  c->AddPerf(fs::PerfSample{7, 50, 0.001f, 0.001f});
  c->AddPerf(fs::PerfSample{7, 53, 0.001f, 0.001f});
  const fs::WindowStats first = c->Finish(0);
  EXPECT_EQ(2u, first.appDropped);
  EXPECT_EQ(3u, first.compositorDropped);
  c->AddPerf(fs::PerfSample{9, 53, 0.001f, 0.001f});  // counted once, in this window, against the last one seen
  const fs::WindowStats second = c->Finish(0);
  EXPECT_EQ(2u, second.appDropped);
  EXPECT_EQ(0u, second.compositorDropped);
}

TEST(FrameStatsWindow, ACounterThatGoesBackwardRebasesAndCountsNothingForThatStep) {
  auto c = NewCollector();
  c->AddPerf(fs::PerfSample{9, 9, 0.001f, 0.001f});
  c->AddPerf(fs::PerfSample{1, 1, 0.001f, 0.001f});  // the session was reset (ovr_ResetPerfStats)
  c->AddPerf(fs::PerfSample{4, 4, 0.001f, 0.001f});
  const fs::WindowStats w = c->Finish(0);
  EXPECT_EQ(3u, w.appDropped);
  EXPECT_EQ(3u, w.compositorDropped);
}

TEST(FrameStatsWindow, LostCompositorEntriesCountAsOverruns) {
  auto c = NewCollector();
  c->AddPerfLost(4);
  FeedIntervals(*c, {10});
  EXPECT_EQ(4u, c->Finish(0).overruns);
}

// ---- the lines -------------------------------------------------------------------------------------------

TEST(FrameStatsLine, TheWindowLineWithoutAVrSessionIsTheSpecifiedFields) {
  fs::WindowStats w;
  w.frames = 598;
  w.medianUs = 16667;
  w.p95Us = 18200;
  w.maxUs = 41000;
  w.over1p5x = 3;
  w.overruns = 0;
  w.elapsedMs = 10003;
  char line[512];
  const int n = fs::FormatWindowLine(line, sizeof(line), w);
  EXPECT_STREQ(
      "[NEVR.PERF] frame_stats window_s=10 frames=598 median_us=16667 p95_us=18200 max_us=41000 over_1p5x=3 "
      "overruns=0 elapsed_ms=10003",
      line);
  EXPECT_EQ(static_cast<int>(std::strlen(line)), n);
}

TEST(FrameStatsLine, TheWindowLineWithAVrSessionAddsTheCompositorFieldsBeforeElapsed) {
  fs::WindowStats w;
  w.frames = 720;
  w.medianUs = 11111;
  w.p95Us = 12000;
  w.maxUs = 30000;
  w.over1p5x = 1;
  w.overruns = 2;
  w.elapsedMs = 10001;
  w.hasPerf = true;
  w.perfSamples = 715;
  w.appDropped = 4;
  w.compositorDropped = 1;
  w.appCpuMeanUs = 5200;
  w.appCpuMaxUs = 9100;
  w.appGpuMeanUs = 6100;
  w.appGpuMaxUs = 10400;
  char line[512];
  fs::FormatWindowLine(line, sizeof(line), w);
  EXPECT_STREQ(
      "[NEVR.PERF] frame_stats window_s=10 frames=720 median_us=11111 p95_us=12000 max_us=30000 over_1p5x=1 "
      "overruns=2 app_dropped=4 compositor_dropped=1 app_cpu_us_mean=5200 app_cpu_us_max=9100 "
      "app_gpu_us_mean=6100 app_gpu_us_max=10400 perf_samples=715 elapsed_ms=10001",
      line);
}

TEST(FrameStatsLine, ABufferThatIsTooSmallIsTruncatedAndTerminatedNotOverrun) {
  fs::WindowStats w;
  w.hasPerf = true;
  char line[40];
  std::memset(line, 'x', sizeof(line));
  const int n = fs::FormatWindowLine(line, sizeof(line), w);
  EXPECT_GE(n, static_cast<int>(sizeof(line)));  // snprintf's would-have-written length
  EXPECT_EQ('\0', line[sizeof(line) - 1]);
  EXPECT_EQ(sizeof(line) - 1, std::strlen(line));
}

TEST(FrameStatsLine, TheStartLineNamesTheModeTheHostTheBuildAndTheInstallResults) {
  char line[512];
  fs::FormatStartLine(line, sizeof(line), fs::Mode::Full, true, "client", "v5.6.0-3-gabc1234", "armed", "installed");
  EXPECT_STREQ(
      "[NEVR.PERF] frame_stats start mode=full recognised=yes host=client build=v5.6.0-3-gabc1234 "
      "frame_ring=armed perf_hook=installed",
      line);
  fs::FormatStartLine(line, sizeof(line), fs::Mode::Off, false, "server", "dev", "off", "skipped_server");
  EXPECT_STREQ(
      "[NEVR.PERF] frame_stats start mode=off recognised=no host=server build=dev frame_ring=off "
      "perf_hook=skipped_server",
      line);
}

// ---- Poll's drain and window clock (the logic Poll runs each tick, without the game) ----

TEST(FrameStatsDrain, EverythingBothRingsHoldReachesTheCollectorInOrder) {
  fs::SpscRing<fs::FrameSlot, 8> frames;
  fs::SpscRing<fs::PerfSample, 8> perf;
  std::atomic<uint64_t> rejected{0};
  fs::DrainState state;
  fs::Collector collector;

  for (uint64_t t = 1000000; t <= 1000000 + 4 * 11000; t += 11000) frames.Push(fs::FrameSlot{t, frames.Dropped()});
  fs::PerfSample sample;
  sample.appCpuS = 0.005f;
  sample.appGpuS = 0.006f;
  perf.Push(sample);
  perf.Push(sample);

  fs::Drain(frames, perf, rejected, state, collector);
  const fs::WindowStats w = collector.Finish(10000);
  EXPECT_EQ(4u, w.frames);  // 5 readings = 4 intervals
  EXPECT_EQ(11000u, w.medianUs);
  EXPECT_TRUE(w.hasPerf);
  EXPECT_EQ(2u, w.perfSamples);
  EXPECT_EQ(0u, w.overruns);
}

TEST(FrameStatsDrain, ALossOnThePerfPathIsCountedOnceNotOnEveryDrain) {
  fs::SpscRing<fs::FrameSlot, 8> frames;
  fs::SpscRing<fs::PerfSample, 2> perf;  // holds 2
  std::atomic<uint64_t> rejected{0};
  fs::DrainState state;
  fs::Collector collector;

  fs::PerfSample sample;
  for (int i = 0; i < 5; ++i) perf.Push(sample);  // 2 kept, 3 dropped
  rejected.store(4);                              // 4 entries the extractor refused
  fs::Drain(frames, perf, rejected, state, collector);
  EXPECT_EQ(7u, collector.Finish(1).overruns);

  fs::Drain(frames, perf, rejected, state, collector);  // nothing new lost
  EXPECT_EQ(0u, collector.Finish(1).overruns);

  rejected.store(5);
  fs::Drain(frames, perf, rejected, state, collector);  // one more
  EXPECT_EQ(1u, collector.Finish(1).overruns);
}

TEST(FrameStatsDrain, ALostLoopHeadReadingBreaksTheIntervalAcrossTheGap) {
  fs::SpscRing<fs::FrameSlot, 2> frames;  // holds 2
  fs::SpscRing<fs::PerfSample, 4> perf;
  std::atomic<uint64_t> rejected{0};
  fs::DrainState state;
  fs::Collector collector;

  frames.Push(fs::FrameSlot{1000000, frames.Dropped()});
  frames.Push(fs::FrameSlot{1011000, frames.Dropped()});
  frames.Push(fs::FrameSlot{1022000, frames.Dropped()});  // ring full: dropped
  fs::Drain(frames, perf, rejected, state, collector);
  frames.Push(fs::FrameSlot{1033000, frames.Dropped()});  // carries dropped=1
  fs::Drain(frames, perf, rejected, state, collector);
  const fs::WindowStats w = collector.Finish(10000);
  EXPECT_EQ(1u, w.frames);  // only 1000000 -> 1011000; nothing spans the gap
  EXPECT_EQ(1u, w.overruns);
}

TEST(FrameStatsClock, TheFirstTickOnlyStartsTheWindow) {
  fs::WindowClock clock;
  uint64_t ms = 99;
  EXPECT_FALSE(clock.Tick(5000000, &ms));
  EXPECT_EQ(99u, ms);
}

TEST(FrameStatsClock, AWindowEndsAtTenSecondsExactlyAndNotBefore) {
  fs::WindowClock clock;
  uint64_t ms = 0;
  ASSERT_FALSE(clock.Tick(5000000, &ms));
  EXPECT_FALSE(clock.Tick(5000000 + fs::kWindowUs - 1, &ms));
  EXPECT_TRUE(clock.Tick(5000000 + fs::kWindowUs, &ms));
  EXPECT_EQ(10000u, ms);
}

TEST(FrameStatsClock, ALateTickReportsItsRealLengthAndStartsTheNextWindowThere) {
  fs::WindowClock clock;
  uint64_t ms = 0;
  ASSERT_FALSE(clock.Tick(1000000, &ms));
  ASSERT_TRUE(clock.Tick(1000000 + 10500000, &ms));
  EXPECT_EQ(10500u, ms);
  EXPECT_FALSE(clock.Tick(1000000 + 10500000 + fs::kWindowUs - 1, &ms));
  EXPECT_TRUE(clock.Tick(1000000 + 10500000 + fs::kWindowUs, &ms));
  EXPECT_EQ(10000u, ms);
}

TEST(FrameStatsClock, ATimeThatStepsBackwardNeitherEndsTheWindowNorWrapsIt) {
  fs::WindowClock clock;
  uint64_t ms = 0;
  ASSERT_FALSE(clock.Tick(50000000, &ms));
  EXPECT_FALSE(clock.Tick(40000000, &ms));
}

// ---- the gaps the first mutation round found ----------------------------------------------------------------

TEST(FrameStatsMode, UppercaseSpellingsWithAnAreRecognised) {
  bool recognised = false;
  EXPECT_EQ(fs::Mode::Off, fs::ParseMode(std::string("FALSE"), &recognised));
  EXPECT_TRUE(recognised);
  recognised = false;
  EXPECT_EQ(fs::Mode::PerfOnly, fs::ParseMode(std::string("Perf"), &recognised));
  EXPECT_TRUE(recognised);
}

namespace {
// A value whose copy-assignment pushes into the ring it is being stored in: a second writer arriving while the
// first is inside Push, forced deterministically.
struct Reentrant {
  static inline fs::SpscRing<Reentrant, 4>* ring = nullptr;
  static inline bool secondPushed = true;
  int v = 0;
  Reentrant() = default;
  Reentrant(const Reentrant&) = default;
  Reentrant& operator=(const Reentrant& other) {
    v = other.v;
    if (ring != nullptr && other.v == 1) {
      Reentrant inner;
      inner.v = 2;
      secondPushed = ring->Push(inner);
    }
    return *this;
  }
};
}  // namespace

TEST(FrameStatsRing, ASecondWriterInsideAPushIsDroppedAndCountedNotStored) {
  fs::SpscRing<Reentrant, 4> ring;
  Reentrant::ring = &ring;
  Reentrant::secondPushed = true;
  Reentrant first;
  first.v = 1;
  EXPECT_TRUE(ring.Push(first));
  Reentrant::ring = nullptr;
  EXPECT_FALSE(Reentrant::secondPushed);
  EXPECT_EQ(1u, ring.Dropped());
  Reentrant out;
  ASSERT_TRUE(ring.Pop(out));
  EXPECT_EQ(1, out.v);
  EXPECT_FALSE(ring.Pop(out));  // the second writer's entry is not in the ring
}

TEST(FrameStatsPercentile, NearestRankIsTheCeilingOfPctTimesNOver100) {
  uint32_t v[101];
  for (uint32_t i = 0; i < 101; ++i) v[i] = i * 10;
  EXPECT_EQ(0u, fs::Percentile(v, 0, 50));      // no values: zero
  EXPECT_EQ(10u, fs::Percentile(v, 101, 1));    // ceil(1.01) = 2 -> the second value
  EXPECT_EQ(0u, fs::Percentile(v, 101, 0));     // rank 0 is clamped up to the first value
  EXPECT_EQ(1000u, fs::Percentile(v, 101, 150));  // beyond 100 % is clamped to the last value
  EXPECT_EQ(500u, fs::Percentile(v, 101, 50));
}

TEST(FrameStatsPerfExtract, ACpuTimeThatIsNotFiniteOrNegativeIsRejectedLikeAGpuOne) {
  for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -0.001f}) {
    auto cpuBad = MakeStats(1);
    std::memcpy(cpuBad.data() + fs::kPerfAppCpuOffset, &bad, sizeof(bad));
    fs::PerfSample out[5];
    std::size_t rejected = 0;
    EXPECT_EQ(0u, fs::ExtractPerfSamples(cpuBad.data(), out, &rejected)) << "cpu " << bad;
    EXPECT_EQ(1u, rejected) << "cpu " << bad;

    auto gpuBad = MakeStats(1);
    std::memcpy(gpuBad.data() + fs::kPerfAppGpuOffset, &bad, sizeof(bad));
    rejected = 0;
    EXPECT_EQ(0u, fs::ExtractPerfSamples(gpuBad.data(), out, &rejected)) << "gpu " << bad;
    EXPECT_EQ(1u, rejected) << "gpu " << bad;
  }
}

TEST(FrameStatsWindow, ANewWindowWithoutCompositorSamplesReportsNoneEvenAfterOneWithSome) {
  fs::Collector c;
  fs::PerfSample s;
  s.appCpuS = 0.005f;
  s.appGpuS = 0.006f;
  c.AddPerf(s);
  EXPECT_TRUE(c.Finish(1).hasPerf);
  const fs::WindowStats second = c.Finish(1);
  EXPECT_FALSE(second.hasPerf);
  EXPECT_EQ(0u, second.perfSamples);
}

TEST(FrameStatsWindow, CpuAndGpuSumsAndMaximaStartOverInEachWindow) {
  fs::Collector c;
  fs::PerfSample big;
  big.appCpuS = 0.008f;
  big.appGpuS = 0.009f;
  c.AddPerf(big);
  c.Finish(1);
  fs::PerfSample small;
  small.appCpuS = 0.001f;
  small.appGpuS = 0.002f;
  c.AddPerf(small);
  const fs::WindowStats w = c.Finish(1);
  EXPECT_EQ(1000u, w.appCpuMeanUs);
  EXPECT_EQ(1000u, w.appCpuMaxUs);
  EXPECT_EQ(2000u, w.appGpuMeanUs);
  EXPECT_EQ(2000u, w.appGpuMaxUs);
}

TEST(FrameStatsWindow, TimesAtTheEdgesOfTheRangeRoundAndClamp) {
  fs::Collector c;
  fs::PerfSample zero;  // 0 s: zero microseconds, not one
  c.AddPerf(zero);
  fs::WindowStats w = c.Finish(1);
  EXPECT_EQ(0u, w.appCpuMeanUs);
  EXPECT_EQ(0u, w.appCpuMaxUs);
  EXPECT_EQ(0u, w.appGpuMaxUs);

  fs::PerfSample sub;  // 0.8 us rounds to 1
  sub.appCpuS = 8e-7f;
  c.AddPerf(sub);
  w = c.Finish(1);
  EXPECT_EQ(1u, w.appCpuMaxUs);

  fs::PerfSample huge;  // 5000 s is beyond uint32 microseconds: clamped, not wrapped to zero
  huge.appCpuS = 5000.0f;
  c.AddPerf(huge);
  w = c.Finish(1);
  EXPECT_EQ(UINT32_MAX, w.appCpuMaxUs);
}

TEST(FrameStatsLine, ATruncatedWindowLineNeverWritesPastTheCapacityAndReturnsWhatItCutOff) {
  fs::WindowStats w;
  w.hasPerf = true;
  char full[512];
  const int fullLen = fs::FormatWindowLine(full, sizeof(full), w);
  ASSERT_GT(fullLen, 0);
  const std::string whole(full);
  const std::size_t firstPart = whole.find(" app_dropped=");
  const std::size_t withPerf = whole.find(" elapsed_ms=");
  ASSERT_NE(std::string::npos, firstPart);
  ASSERT_NE(std::string::npos, withPerf);

  // cap 40: cut inside the first part; cap 120: the first part fits and the compositor part is cut.
  for (std::size_t cap : {std::size_t{40}, std::size_t{120}}) {
    std::vector<char> buf(cap + 64, 'x');
    const int n = fs::FormatWindowLine(buf.data(), cap, w);
    for (std::size_t i = cap; i < buf.size(); ++i) ASSERT_EQ('x', buf[i]) << "cap " << cap << " byte " << i;
    EXPECT_LT(std::strlen(buf.data()), cap);
    EXPECT_EQ(whole.substr(0, std::strlen(buf.data())), std::string(buf.data())) << "a prefix of the full line";
    // The result is the length of the part that did not fit: no later part is formatted after the cut.
    EXPECT_EQ(static_cast<int>(cap == 40 ? firstPart : withPerf), n) << "cap " << cap;
  }
}

TEST(FrameStatsPerfExtract, TheOffsetConstantsAreTheSdkLayout) {
  EXPECT_EQ(0x48u, fs::kPerfEntryStride);
  EXPECT_EQ(5u, fs::kPerfMaxEntries);
  EXPECT_EQ(0x168u, fs::kPerfCountOffset);
  EXPECT_EQ(0x08u, fs::kPerfAppDroppedOffset);
  EXPECT_EQ(0x14u, fs::kPerfAppCpuOffset);
  EXPECT_EQ(0x18u, fs::kPerfAppGpuOffset);
  EXPECT_EQ(0x20u, fs::kPerfCompositorDroppedOffset);
  EXPECT_EQ(0x17Cu, fs::kPerfStatsSize);
  EXPECT_EQ(fs::kPerfStatsSize, fs::kPerfMaxEntries * fs::kPerfEntryStride + 0x14u);  // + the five trailing words
}

// ---- what the hooks and Configure decide ------------------------------------------------------------------

TEST(FrameStatsDecisions, TheRingIsArmedOnlyInFullModeWithTheCallSiteConfirmed) {
  EXPECT_TRUE(fs::ArmsFrameRing(fs::Mode::Full, true));
  EXPECT_FALSE(fs::ArmsFrameRing(fs::Mode::Full, false));
  EXPECT_FALSE(fs::ArmsFrameRing(fs::Mode::PerfOnly, true));
  EXPECT_FALSE(fs::ArmsFrameRing(fs::Mode::Off, true));
  EXPECT_STREQ("armed", fs::FrameRingState(fs::Mode::Full, true));
  EXPECT_STREQ("site_mismatch", fs::FrameRingState(fs::Mode::Full, false));
  EXPECT_STREQ("off", fs::FrameRingState(fs::Mode::PerfOnly, true));
  EXPECT_STREQ("off", fs::FrameRingState(fs::Mode::Off, false));
}

TEST(FrameStatsDecisions, ThePerfCopyKeepsDataInFullAndPerfModesNotOff) {
  EXPECT_TRUE(fs::KeepsPerfStats(fs::Mode::Full));
  EXPECT_TRUE(fs::KeepsPerfStats(fs::Mode::PerfOnly));
  EXPECT_FALSE(fs::KeepsPerfStats(fs::Mode::Off));
}

TEST(FrameStatsDecisions, ALoopHeadReadingIsRecordedOnlyWhenArmedAndTheClockGaveATime) {
  EXPECT_TRUE(fs::ShouldRecordLoopHead(1, true));
  EXPECT_FALSE(fs::ShouldRecordLoopHead(0, true));
  EXPECT_FALSE(fs::ShouldRecordLoopHead(5000000, false));
}

TEST(FrameStatsDecisions, ARecordedLoopHeadReadingCarriesTheTimeAndTheRingsDropCount) {
  fs::SpscRing<fs::FrameSlot, 2> ring;
  fs::RecordLoopHead(ring, 111);
  fs::RecordLoopHead(ring, 222);
  fs::RecordLoopHead(ring, 333);  // full: dropped
  fs::RecordLoopHead(ring, 444);  // full: dropped
  fs::FrameSlot a, b, c;
  ASSERT_TRUE(ring.Pop(a));
  ASSERT_TRUE(ring.Pop(b));
  EXPECT_FALSE(ring.Pop(c));
  EXPECT_EQ(111u, a.us);
  EXPECT_EQ(0u, a.dropped);
  EXPECT_EQ(222u, b.us);
  EXPECT_EQ(0u, b.dropped);
  fs::RecordLoopHead(ring, 555);
  ASSERT_TRUE(ring.Pop(c));
  EXPECT_EQ(555u, c.us);
  EXPECT_EQ(2u, c.dropped);
}

TEST(FrameStatsDecisions, ThePerfResultIsCopiedOnlyOnSuccessWithABufferWhileTheProbeIsOn) {
  auto stats = MakeStats(1);
  EXPECT_TRUE(fs::ShouldCopyPerf(0, stats.data(), true));
  EXPECT_TRUE(fs::ShouldCopyPerf(7, stats.data(), true));
  EXPECT_FALSE(fs::ShouldCopyPerf(-1, stats.data(), true));
  EXPECT_FALSE(fs::ShouldCopyPerf(-1004, stats.data(), true));  // ovr_GetPerfStats with no session
  EXPECT_FALSE(fs::ShouldCopyPerf(0, nullptr, true));
  EXPECT_FALSE(fs::ShouldCopyPerf(0, stats.data(), false));
}

TEST(FrameStatsDecisions, CapturingAPerfResultPushesTheEntriesOldestFirstAndCountsTheRefusedOnes) {
  fs::SpscRing<fs::PerfSample, 8> ring;
  std::atomic<uint64_t> rejected{0};
  auto stats = MakeStats(3);
  fs::CapturePerf(stats.data(), ring, rejected);
  EXPECT_EQ(0u, rejected.load());
  fs::PerfSample s;
  ASSERT_TRUE(ring.Pop(s));
  EXPECT_EQ(102, s.appDropped);  // entry 2 is the oldest
  ASSERT_TRUE(ring.Pop(s));
  EXPECT_EQ(101, s.appDropped);
  ASSERT_TRUE(ring.Pop(s));
  EXPECT_EQ(100, s.appDropped);
  EXPECT_FALSE(ring.Pop(s));

  auto bad = MakeStats(2);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(bad.data() + 0x48 + 0x14, &nan, sizeof(nan));  // entry 1's cpu time
  fs::CapturePerf(bad.data(), ring, rejected);
  EXPECT_EQ(1u, rejected.load());
  ASSERT_TRUE(ring.Pop(s));
  EXPECT_EQ(100, s.appDropped);  // only entry 0 survived
  EXPECT_FALSE(ring.Pop(s));

  auto outOfRange = MakeStats(9);
  fs::CapturePerf(outOfRange.data(), ring, rejected);
  EXPECT_EQ(2u, rejected.load());
  EXPECT_FALSE(ring.Pop(s));

  auto twoBad = MakeStats(3);
  std::memcpy(twoBad.data() + 0x14, &nan, sizeof(nan));         // entry 0's cpu time
  std::memcpy(twoBad.data() + 2 * 0x48 + 0x18, &nan, sizeof(nan));  // entry 2's gpu time
  fs::CapturePerf(twoBad.data(), ring, rejected);
  EXPECT_EQ(4u, rejected.load());  // two more, counted by how many, not once per call
  ASSERT_TRUE(ring.Pop(s));
  EXPECT_EQ(101, s.appDropped);  // only entry 1 survived
  EXPECT_FALSE(ring.Pop(s));
}
