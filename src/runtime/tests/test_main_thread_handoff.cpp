// GH #44: the graceful-shutdown thread hands EndSession + Unregister to the game
// thread through MainThreadHandoff instead of touching the main-thread-only
// callback registry itself. These tests pin the hand-off contract.
//
// The "game thread" below is the test's own thread calling Service() once per
// simulated frame, the way GameServerLib::Update() does.

#include "runtime/server/main_thread_handoff.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using nevr_game_server::MainThreadHandoff;
using namespace std::chrono_literals;

namespace {

// Calls Service() once per simulated 1 ms frame until it runs a task or the
// deadline passes. Returns whether a task ran.
bool RunFramesUntilServiced(MainThreadHandoff& handoff, std::chrono::milliseconds deadline) {
  const auto end = std::chrono::steady_clock::now() + deadline;
  while (std::chrono::steady_clock::now() < end) {
    if (handoff.Service()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

// Simulated frames until a request is pending (or the deadline passes).
bool RunFramesUntilPending(const MainThreadHandoff& handoff, std::chrono::milliseconds deadline) {
  const auto end = std::chrono::steady_clock::now() + deadline;
  while (std::chrono::steady_clock::now() < end) {
    if (handoff.HasPending()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

}  // namespace

// The defect itself: before #44 the shutdown thread ran Unregister on its own
// thread. The task must run on the thread that calls Service(), not the requester.
TEST(MainThreadHandoff, TaskRunsOnTheServicingThreadNotTheRequester) {
  MainThreadHandoff handoff;
  const std::thread::id gameThread = std::this_thread::get_id();
  std::thread::id ranOn;
  std::thread::id requesterThread;

  std::future<MainThreadHandoff::Outcome> outcome = std::async(std::launch::async, [&]() {
    requesterThread = std::this_thread::get_id();
    return handoff.RunOnServicingThread([&]() { ranOn = std::this_thread::get_id(); }, 10s);
  });

  ASSERT_TRUE(RunFramesUntilServiced(handoff, 10s)) << "the request never reached the game thread";
  EXPECT_EQ(outcome.get(), MainThreadHandoff::Outcome::kRan);
  EXPECT_EQ(ranOn, gameThread) << "the task ran off the servicing (game) thread";
  EXPECT_NE(ranOn, requesterThread) << "the task ran on the requesting (shutdown) thread";
}

TEST(MainThreadHandoff, ServiceIsANoOpWhenNothingIsPending) {
  MainThreadHandoff handoff;
  EXPECT_FALSE(handoff.HasPending());
  EXPECT_FALSE(handoff.Service());
}

// The game thread can stop calling Update(). The requester must get its thread
// back, and the withdrawn task must never run later behind the fallback's back.
TEST(MainThreadHandoff, UnservicedRequestTimesOutAndIsNeverRunLater) {
  MainThreadHandoff handoff;
  std::atomic<int> runs{0};

  EXPECT_EQ(handoff.RunOnServicingThread([&]() { ++runs; }, 20ms), MainThreadHandoff::Outcome::kTimedOut);
  EXPECT_FALSE(handoff.HasPending());
  EXPECT_FALSE(handoff.Service()) << "a timed-out request was still serviceable";
  EXPECT_EQ(runs.load(), 0);
}

// Once the game thread has started the task the requester must not give up on
// it — giving up would run the fallback concurrently with the task.
TEST(MainThreadHandoff, RequesterWaitsPastItsTimeoutForATaskAlreadyRunning) {
  MainThreadHandoff handoff;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::promise<void> started;

  std::future<MainThreadHandoff::Outcome> outcome = std::async(std::launch::async, [&]() {
    return handoff.RunOnServicingThread(
        [&]() {
          started.set_value();
          released.wait();
        },
        20ms);
  });

  // Game thread: wait for the request, then run it on a helper "frame" so this
  // thread can hold the task open past the requester's 20 ms timeout.
  if (!RunFramesUntilPending(handoff, 10s)) {
    // Release before failing: a task that somehow already started (e.g. run
    // inline on the requester) would otherwise block the future's destructor
    // and turn this failure into a hang.
    release.set_value();
    FAIL() << "the request never became pending for the game thread";
  }
  std::thread frame([&]() { handoff.Service(); });
  started.get_future().wait();
  std::this_thread::sleep_for(100ms);  // well past the requester's timeout
  EXPECT_EQ(outcome.wait_for(0ms), std::future_status::timeout)
      << "the requester returned while its task was still running";
  release.set_value();
  frame.join();

  EXPECT_EQ(outcome.get(), MainThreadHandoff::Outcome::kRan);
}

TEST(MainThreadHandoff, CancelWithdrawsAPendingRequest) {
  MainThreadHandoff handoff;
  std::atomic<int> runs{0};

  std::future<MainThreadHandoff::Outcome> outcome = std::async(std::launch::async, [&]() {
    return handoff.RunOnServicingThread([&]() { ++runs; }, 10s);
  });

  ASSERT_TRUE(RunFramesUntilPending(handoff, 10s));
  handoff.Cancel();
  EXPECT_EQ(outcome.get(), MainThreadHandoff::Outcome::kCancelled);
  EXPECT_FALSE(handoff.Service());
  EXPECT_EQ(runs.load(), 0);
}

// Service() is called from a game vtable entry point; an exception must not
// escape into the game, and the requester must learn the task failed.
TEST(MainThreadHandoff, ThrowingTaskIsContainedAndReported) {
  MainThreadHandoff handoff;

  std::future<MainThreadHandoff::Outcome> outcome = std::async(std::launch::async, [&]() {
    return handoff.RunOnServicingThread([]() { throw std::runtime_error("unregister failed"); }, 10s);
  });

  bool serviced = false;
  EXPECT_NO_THROW(serviced = RunFramesUntilServiced(handoff, 10s));
  EXPECT_TRUE(serviced);
  EXPECT_EQ(outcome.get(), MainThreadHandoff::Outcome::kTaskThrew);
}

TEST(MainThreadHandoff, SecondConcurrentRequestIsRefusedAndTheFirstStillRuns) {
  MainThreadHandoff handoff;
  std::atomic<int> firstRuns{0};
  std::atomic<int> secondRuns{0};

  std::future<MainThreadHandoff::Outcome> first = std::async(std::launch::async, [&]() {
    return handoff.RunOnServicingThread([&]() { ++firstRuns; }, 10s);
  });
  ASSERT_TRUE(RunFramesUntilPending(handoff, 10s));

  EXPECT_EQ(handoff.RunOnServicingThread([&]() { ++secondRuns; }, 10s), MainThreadHandoff::Outcome::kBusy);

  ASSERT_TRUE(RunFramesUntilServiced(handoff, 10s));
  EXPECT_EQ(first.get(), MainThreadHandoff::Outcome::kRan);
  EXPECT_EQ(firstRuns.load(), 1);
  EXPECT_EQ(secondRuns.load(), 0);
}

TEST(MainThreadHandoff, IsReusableAfterARequestCompletes) {
  MainThreadHandoff handoff;
  for (int round = 0; round < 3; ++round) {
    std::atomic<int> runs{0};
    std::future<MainThreadHandoff::Outcome> outcome = std::async(std::launch::async, [&]() {
      return handoff.RunOnServicingThread([&]() { ++runs; }, 10s);
    });
    ASSERT_TRUE(RunFramesUntilServiced(handoff, 10s)) << "round " << round;
    EXPECT_EQ(outcome.get(), MainThreadHandoff::Outcome::kRan) << "round " << round;
    EXPECT_EQ(runs.load(), 1) << "round " << round;
  }
}

TEST(MainThreadHandoff, OutcomeNamesAreStableLogTokens) {
  EXPECT_STREQ(nevr_game_server::MainThreadHandoffOutcomeName(MainThreadHandoff::Outcome::kRan), "ran");
  EXPECT_STREQ(nevr_game_server::MainThreadHandoffOutcomeName(MainThreadHandoff::Outcome::kTaskThrew), "task_threw");
  EXPECT_STREQ(nevr_game_server::MainThreadHandoffOutcomeName(MainThreadHandoff::Outcome::kTimedOut), "timed_out");
  EXPECT_STREQ(nevr_game_server::MainThreadHandoffOutcomeName(MainThreadHandoff::Outcome::kCancelled), "cancelled");
  EXPECT_STREQ(nevr_game_server::MainThreadHandoffOutcomeName(MainThreadHandoff::Outcome::kBusy), "busy");
}
