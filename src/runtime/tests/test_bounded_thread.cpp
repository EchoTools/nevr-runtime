// #340: a client exit ended with ExitProcess(3) because a namespace-scope std::thread was still
// joinable when the DLL's atexit chain destroyed it (std::terminate). BoundedThread is the type
// that replaced it; these tests pin the three ways a thread can be left at shutdown.

#include "core/bounded_thread.h"

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
