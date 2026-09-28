/* SYNTHESIS -- custom tool code, not from binary */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <thread>

#include "runtime/server/telemetry_snapshot_store.h"

namespace {

struct SnapshotFixture {
  uint64_t generation = 0;
  uint64_t inverse = 0;
};

bool Publish(TelemetrySnapshotStore<SnapshotFixture>& store, uint64_t generation) {
  return store.CaptureIfActive([generation](SnapshotFixture& snapshot) {
    snapshot.generation = generation;
    snapshot.inverse = ~generation;
    return true;
  });
}

TEST(TelemetrySnapshotStore, ZeroSnapshotsAndRateLimitedCaptureDoNotPublish) {
  TelemetrySnapshotStore<SnapshotFixture> store;
  EXPECT_FALSE(store.TryAcquireRead(0));
  store.Activate();
  bool captureCalled = false;
  EXPECT_FALSE(store.CaptureIfActive([&captureCalled](SnapshotFixture&) {
    captureCalled = true;
    return false;
  }));
  EXPECT_TRUE(captureCalled);
  EXPECT_FALSE(store.HasPublished());
  EXPECT_FALSE(store.TryAcquireRead(0));
}

TEST(TelemetrySnapshotStore, ReaderLeaseProtectsSnapshotAndCountsOnlyOverwrittenGenerations) {
  TelemetrySnapshotStore<SnapshotFixture> store;
  store.Activate();
  ASSERT_TRUE(Publish(store, 1));

  std::mutex gateMutex;
  std::condition_variable gateCv;
  uint32_t pausedStage = 0;
  uint32_t resumeStage = 0;
  SnapshotFixture previousCopy{};
  uint64_t readSequence = 0;
  std::atomic<bool> readerConsistent{true};
  std::thread readerThread([&]() {
    auto reader = store.TryAcquireRead(0);
    if (!reader || reader.sequence() != 1U) {
      readerConsistent.store(false, std::memory_order_release);
      return;
    }
    readSequence = reader.sequence();
    for (uint32_t stage = 1; stage <= 3; ++stage) {
      std::unique_lock<std::mutex> lock(gateMutex);
      pausedStage = stage;
      gateCv.notify_all();
      gateCv.wait(lock, [&resumeStage, stage]() { return resumeStage >= stage; });
      if (reader.snapshot().generation != 1U || reader.snapshot().inverse != ~uint64_t{1}) {
        readerConsistent.store(false, std::memory_order_release);
      }
    }
    previousCopy = reader.snapshot();
  });

  auto awaitStage = [&](uint32_t expected) {
    std::unique_lock<std::mutex> lock(gateMutex);
    return gateCv.wait_for(lock, std::chrono::seconds(2), [&pausedStage, expected]() {
      return pausedStage >= expected;
    });
  };
  auto resume = [&](uint32_t stage) {
    {
      std::lock_guard<std::mutex> lock(gateMutex);
      resumeStage = stage;
    }
    gateCv.notify_all();
  };

  const bool headerPause = awaitStage(1);  // header serialization pause
  EXPECT_TRUE(headerPause);
  EXPECT_TRUE(Publish(store, 2));
  resume(1);
  const bool framePause = awaitStage(2);  // frame serialization pause
  EXPECT_TRUE(framePause);
  EXPECT_TRUE(Publish(store, 3));
  EXPECT_TRUE(Publish(store, 4));
  resume(2);
  const bool previousCopyPause = awaitStage(3);  // previous-snapshot copy pause
  EXPECT_TRUE(previousCopyPause);
  EXPECT_TRUE(Publish(store, 5));
  resume(3);
  readerThread.join();

  EXPECT_TRUE(headerPause && framePause && previousCopyPause);
  EXPECT_TRUE(readerConsistent.load(std::memory_order_acquire));
  EXPECT_EQ(readSequence, 1U);
  EXPECT_EQ(previousCopy.generation, 1U);
  EXPECT_EQ(store.skippedGenerations(), 3U);

  auto latest = store.TryAcquireRead(readSequence);
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest.sequence(), 5U);
  EXPECT_EQ(latest.snapshot().generation, 5U);
  EXPECT_EQ(latest.snapshot().inverse, ~uint64_t{5});
  latest.Release();
  EXPECT_EQ(store.skippedGenerations(), 3U);
}

TEST(TelemetrySnapshotStore, HeaderEpochRetriesFailureAndLateConnectionUsesLatestLease) {
  TelemetrySnapshotStore<SnapshotFixture> store;
  TelemetryHeaderEpoch header;
  store.Activate();
  ASSERT_TRUE(Publish(store, 12));

  // A connection that arrives after the startup wait can lease the latest
  // snapshot even when its sequence was already consumed before reconnect.
  const uint64_t lateConnectionEpoch = header.Require();
  auto lateConnection = store.TryAcquireRead(12, header.Required());
  ASSERT_TRUE(lateConnection);
  EXPECT_EQ(lateConnection.snapshot().generation, 12U);
  const uint64_t lateSequence = lateConnection.sequence();
  lateConnection.Release();

  // Failed header send does not mark the epoch; same-generation retry is legal.
  EXPECT_TRUE(header.Required());
  EXPECT_FALSE(header.MarkSent(lateConnectionEpoch - 1));
  auto retry = store.TryAcquireRead(lateSequence, header.Required());
  ASSERT_TRUE(retry);
  EXPECT_EQ(retry.sequence(), lateSequence);
  EXPECT_TRUE(header.MarkSent(lateConnectionEpoch));
  EXPECT_FALSE(header.Required());
  retry.Release();

  const uint64_t reconnectEpoch = header.Require();
  EXPECT_TRUE(header.Required());
  EXPECT_FALSE(header.MarkSent(lateConnectionEpoch));
  EXPECT_TRUE(header.MarkSent(reconnectEpoch));
  EXPECT_FALSE(header.Required());
}

TEST(TelemetrySnapshotStore, StopQuiescesAnInProgressCaptureBeforeReset) {
  TelemetrySnapshotStore<SnapshotFixture> store;
  store.Activate();
  ASSERT_TRUE(Publish(store, 1));

  std::mutex gateMutex;
  std::condition_variable gateCv;
  bool captureEntered = false;
  bool allowCaptureFinish = false;
  bool captureResult = false;
  std::thread producer([&]() {
    captureResult = store.CaptureIfActive([&](SnapshotFixture& snapshot) {
      std::unique_lock<std::mutex> lock(gateMutex);
      captureEntered = true;
      gateCv.notify_all();
      gateCv.wait(lock, [&allowCaptureFinish]() { return allowCaptureFinish; });
      snapshot.generation = 2;
      snapshot.inverse = ~uint64_t{2};
      return true;
    });
  });
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    ASSERT_TRUE(gateCv.wait_for(lock, std::chrono::seconds(2), [&captureEntered]() { return captureEntered; }));
  }

  std::atomic<bool> stopFinished{false};
  std::thread stopper([&]() {
    store.DeactivateAndQuiesce();
    stopFinished.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(stopFinished.load(std::memory_order_acquire));
  EXPECT_TRUE(store.HasPublished());
  {
    std::lock_guard<std::mutex> lock(gateMutex);
    allowCaptureFinish = true;
  }
  gateCv.notify_all();
  producer.join();
  stopper.join();

  EXPECT_TRUE(captureResult);
  EXPECT_TRUE(stopFinished.load(std::memory_order_acquire));
  EXPECT_FALSE(Publish(store, 3));
  EXPECT_TRUE(store.Reset());
  EXPECT_FALSE(store.HasPublished());
}

TEST(TelemetrySnapshotStore, StopWaitsForReaderLeaseBeforeRestarting) {
  TelemetrySnapshotStore<SnapshotFixture> store;
  store.Activate();
  ASSERT_TRUE(Publish(store, 1));
  auto reader = store.TryAcquireRead(0);
  ASSERT_TRUE(reader);

  store.DeactivateAndQuiesce();
  EXPECT_FALSE(store.Reset());
  EXPECT_EQ(reader.snapshot().generation, 1U);
  reader.Release();
  ASSERT_TRUE(store.Reset());

  store.Activate();
  ASSERT_TRUE(Publish(store, 7));
  auto restarted = store.TryAcquireRead(0);
  ASSERT_TRUE(restarted);
  EXPECT_EQ(restarted.snapshot().generation, 7U);
}

TEST(TelemetrySnapshotStore, RapidStartStopCyclesClearAllSlotRoles) {
  TelemetrySnapshotStore<SnapshotFixture> store;
  for (uint64_t generation = 1; generation <= 100; ++generation) {
    store.Activate();
    ASSERT_TRUE(Publish(store, generation));
    auto reader = store.TryAcquireRead(0);
    ASSERT_TRUE(reader);
    EXPECT_EQ(reader.snapshot().generation, generation);
    reader.Release();
    store.DeactivateAndQuiesce();
    ASSERT_TRUE(store.Reset());
    EXPECT_FALSE(store.HasPublished());
  }
}

TEST(TelemetrySnapshotStore, ThreadStartFailureRollsBackCaptureActivation) {
  TelemetrySnapshotStore<SnapshotFixture> store;
  store.Activate();
  bool rolledBack = false;
  const bool started = StartTelemetryWorker(
      []() { return false; },
      [&]() {
        store.DeactivateAndQuiesce();
        rolledBack = store.Reset();
      });
  EXPECT_FALSE(started);
  EXPECT_TRUE(rolledBack);
  EXPECT_FALSE(Publish(store, 1));
}

TEST(TelemetrySnapshotStore, ThreadStartExceptionUsesSameRollbackPath) {
  TelemetrySnapshotStore<SnapshotFixture> store;
  store.Activate();
  bool rolledBack = false;
  const bool started = StartTelemetryWorker(
      []() -> bool { throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again)); },
      [&]() {
        store.DeactivateAndQuiesce();
        rolledBack = store.Reset();
      });
  EXPECT_FALSE(started);
  EXPECT_TRUE(rolledBack);
  EXPECT_FALSE(Publish(store, 1));
}

TEST(TelemetrySnapshotStore, CaptureAndReaderCanRunOnSeparateThreadsWithoutTornSlots) {
  TelemetrySnapshotStore<SnapshotFixture> store;
  store.Activate();
  std::atomic<bool> producerDone{false};
  std::thread producer([&]() {
    for (uint64_t generation = 1; generation <= 2000; ++generation) {
      store.CaptureIfActive([generation](SnapshotFixture& snapshot) {
        snapshot.generation = generation;
        std::this_thread::yield();
        snapshot.inverse = ~generation;
        return true;
      });
    }
    producerDone.store(true, std::memory_order_release);
  });

  uint64_t lastSequence = 0;
  uint64_t reads = 0;
  for (;;) {
    auto reader = store.TryAcquireRead(lastSequence);
    if (!reader) {
      if (producerDone.load(std::memory_order_acquire)) break;
      std::this_thread::yield();
      continue;
    }
    const SnapshotFixture copy = reader.snapshot();
    EXPECT_EQ(copy.inverse, ~copy.generation);
    lastSequence = reader.sequence();
    ++reads;
    reader.Release();
  }
  producer.join();
  EXPECT_GT(reads, 0U);
}

}  // namespace
