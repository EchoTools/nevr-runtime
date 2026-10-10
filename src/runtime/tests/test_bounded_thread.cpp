// #340: a client exit ended with ExitProcess(3) because a namespace-scope std::thread was still
// joinable when the DLL's atexit chain destroyed it (std::terminate). BoundedThread is the type
// that replaced it; these tests pin the three ways a thread can be left at shutdown.

#include "core/bounded_thread.h"
#include "core/transfer_abort.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

using namespace std::chrono_literals;

namespace {

// Blocks until released, like a download stuck in a network call.
struct Gate {
  std::atomic<bool> release{false};
  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};
  void Run() {
    started = true;
    while (!release) std::this_thread::sleep_for(1ms);
    finished = true;
  }
};

}  // namespace

TEST(BoundedThread, DestroyingAWorkerThatIsStillRunningDoesNotTerminate) {
  Gate gate;
  {
    nevr::BoundedThread worker;
    worker.Start([&gate] { gate.Run(); });
    while (!gate.started) std::this_thread::sleep_for(1ms);
    // Scope exit with a joinable thread: a plain std::thread calls std::terminate here.
  }
  gate.release = true;
  while (!gate.finished) std::this_thread::sleep_for(1ms);
  SUCCEED();
}

TEST(BoundedThread, JoinForJoinsAThreadThatFinishesInTime) {
  std::atomic<bool> ran{false};
  nevr::BoundedThread worker;
  worker.Start([&ran] { ran = true; });
  EXPECT_TRUE(worker.JoinFor(5s));
  EXPECT_TRUE(ran);
  EXPECT_FALSE(worker.Joinable());
}

TEST(BoundedThread, JoinForGivesUpOnAThreadThatDoesNotFinish) {
  Gate gate;
  nevr::BoundedThread worker;
  worker.Start([&gate] { gate.Run(); });
  while (!gate.started) std::this_thread::sleep_for(1ms);
  const auto begin = std::chrono::steady_clock::now();
  EXPECT_FALSE(worker.JoinFor(50ms));
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s);
  EXPECT_FALSE(worker.Joinable());  // detached, so the destructor has nothing left to abort on
  gate.release = true;
  while (!gate.finished) std::this_thread::sleep_for(1ms);
}

TEST(BoundedThread, JoinForWithNoThreadSucceeds) {
  nevr::BoundedThread worker;
  EXPECT_TRUE(worker.JoinFor(0ms));
}

// A fake transfer the way libcurl runs one: it blocks, and only its progress callback can stop it.
// `data_alive` stands for the manifest/tint maps the fetch thread iterates.
namespace {
struct FakeFetch {
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> data_alive{true};
  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};
  std::atomic<int> reads_of_freed_data{0};

  void Run() {
    started = true;
    // curl_easy_perform: loop until the progress callback returns non-zero.
    while (nevr::AbortTransferWhenRequested(&stop_requested) == 0) {
      if (!data_alive) reads_of_freed_data++;
      std::this_thread::sleep_for(1ms);
    }
    finished = true;
  }
};
}  // namespace

TEST(BoundedThread, TimedOutStopLeavesTheThreadsDataAllocatedUntilItsTransferAborts) {
  FakeFetch fetch;
  nevr::BoundedThread worker;
  worker.Start([&fetch] { fetch.Run(); });
  while (!fetch.started) std::this_thread::sleep_for(1ms);

  // Shutdown(): request stop, bounded join, free the data only through JoinForThenRelease.
  // The fake transfer is stalled in a call that has not reached its next callback yet, so the stop
  // flag is not what ends it inside the bound: simulate that by requesting the stop after the join.
  const bool joined = worker.JoinForThenRelease(50ms, [&fetch] { fetch.data_alive = false; });
  EXPECT_FALSE(joined);
  EXPECT_TRUE(fetch.data_alive) << "data was released while the fetch thread was still running";

  // The callback is what ends the transfer once the flag is up.
  fetch.stop_requested = true;
  for (int i = 0; i < 2000 && !fetch.finished; i++) std::this_thread::sleep_for(1ms);
  EXPECT_TRUE(fetch.finished) << "the abort callback did not end the transfer";
  EXPECT_EQ(fetch.reads_of_freed_data.load(), 0);
}

TEST(BoundedThread, FinishedThreadReleasesItsData) {
  FakeFetch fetch;
  fetch.stop_requested = true;  // the callback aborts at once
  nevr::BoundedThread worker;
  worker.Start([&fetch] { fetch.Run(); });
  EXPECT_TRUE(worker.JoinForThenRelease(5s, [&fetch] { fetch.data_alive = false; }));
  EXPECT_FALSE(fetch.data_alive);
}

TEST(TransferAbort, AbortsOnlyWhenRequested) {
  std::atomic<bool> requested{false};
  EXPECT_EQ(nevr::AbortTransferWhenRequested(&requested), 0);
  requested = true;
  EXPECT_NE(nevr::AbortTransferWhenRequested(&requested), 0);
  EXPECT_EQ(nevr::AbortTransferWhenRequested(nullptr), 0);
}
