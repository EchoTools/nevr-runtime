#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include "runtime/compat/bridge_control_owner.h"

TEST(ConnectionLifetime, CloseRejectsNewLeasesAndWaitsForExistingLease) {
  auto lifetime = std::make_shared<BridgeControl::ConnectionLifetime>();
  auto lease = lifetime->TryAcquire();
  ASSERT_NE(lease, nullptr);

  lifetime->CloseAdmission();
  EXPECT_FALSE(lifetime->IsOpen());
  EXPECT_EQ(lifetime->TryAcquire(), nullptr);

  std::promise<void> waitStarted;
  std::promise<void> waitFinished;
  std::future<void> finished = waitFinished.get_future();
  std::thread waiter([&] {
    waitStarted.set_value();
    lifetime->WaitForLeases();
    waitFinished.set_value();
  });
  waitStarted.get_future().wait();
  EXPECT_EQ(finished.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
  lease.reset();
  EXPECT_EQ(finished.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  waiter.join();
}

TEST(BridgeOwnerQueue, ThreadStartFailureIsRetryable) {
  BridgeControl::OwnerQueue owner([](BridgeControl::OwnerQueue::Task) -> std::thread {
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    return std::thread{};
  });

  EXPECT_FALSE(owner.Start());
  EXPECT_EQ(owner.PostControl([] {}), BridgeControl::OwnerQueue::PostResult::Stopped);
  EXPECT_TRUE(owner.StopAndJoin());
}

TEST(BridgeOwnerQueue, ControlLanePreemptsDataAndAllowsReentrantPost) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());

  std::mutex mutex;
  std::condition_variable condition;
  std::vector<int> order;
  bool reentrantQueued = false;
  std::thread::id taskThread;
  ASSERT_EQ(owner.PostData(1, [&] {
              taskThread = std::this_thread::get_id();
              {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(1);
              }
              EXPECT_TRUE(owner.IsOwnerThread());
              EXPECT_EQ(owner.PostControl([&] {
                          std::lock_guard<std::mutex> lock(mutex);
                          order.push_back(3);
                          reentrantQueued = true;
                          condition.notify_all();
                        }),
                        BridgeControl::OwnerQueue::PostResult::Queued);
            }), BridgeControl::OwnerQueue::PostResult::Queued);
  ASSERT_EQ(owner.PostData(1, [&] {
              std::lock_guard<std::mutex> lock(mutex);
              order.push_back(2);
            }), BridgeControl::OwnerQueue::PostResult::Queued);
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(1), [&] { return reentrantQueued; }));
  }
  EXPECT_TRUE(owner.StopAndJoin());
  EXPECT_EQ(order, (std::vector<int>{1, 3, 2}));
  EXPECT_NE(taskThread, std::this_thread::get_id());
}

TEST(BridgeOwnerQueue, CloseFenceBypassesFullReservedControlLane) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());
  std::promise<void> blockerStarted;
  std::promise<void> releaseBlocker;
  std::shared_future<void> releaseSignal = releaseBlocker.get_future().share();
  ASSERT_EQ(owner.PostData(0, [&] {
              blockerStarted.set_value();
              releaseSignal.wait();
            }), BridgeControl::OwnerQueue::PostResult::Queued);
  blockerStarted.get_future().wait();

  std::mutex mutex;
  std::vector<int> order;
  std::promise<void> controlsDrained;
  for (size_t index = 0; index < BridgeControl::OwnerQueue::kReservedControlEvents; ++index) {
    ASSERT_EQ(owner.PostControl([&order, &mutex, index, &controlsDrained] {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(1);
                if (index + 1 == BridgeControl::OwnerQueue::kReservedControlEvents) {
                  controlsDrained.set_value();
                }
              }), BridgeControl::OwnerQueue::PostResult::Queued);
  }
  EXPECT_EQ(owner.PostControl([] {}), BridgeControl::OwnerQueue::PostResult::ControlCountLimit);

  std::promise<void> fenceCompleted;
  ASSERT_TRUE(owner.PostFence([&] {
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back(2);
    fenceCompleted.set_value();
  }));
  releaseBlocker.set_value();
  EXPECT_EQ(fenceCompleted.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_EQ(controlsDrained.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
  std::promise<void> controlAfterFence;
  ASSERT_EQ(owner.PostControl([&controlAfterFence] { controlAfterFence.set_value(); }),
            BridgeControl::OwnerQueue::PostResult::Queued);
  EXPECT_EQ(controlAfterFence.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_TRUE(owner.StopAndJoin());
  ASSERT_FALSE(order.empty());
  EXPECT_EQ(order.front(), 2);
}

TEST(BridgeOwnerQueue, DataLimitsPreserveReservedControlCapacityAndDrainOnStop) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());

  std::promise<void> taskStarted;
  std::promise<void> releaseTask;
  std::shared_future<void> releaseSignal = releaseTask.get_future().share();
  std::atomic<size_t> ran{0};
  ASSERT_EQ(owner.PostData(0, [&] {
              taskStarted.set_value();
              releaseSignal.wait();
              ran.fetch_add(1, std::memory_order_relaxed);
            }), BridgeControl::OwnerQueue::PostResult::Queued);
  taskStarted.get_future().wait();

  for (size_t i = 0; i < BridgeControl::OwnerQueue::kMaxDataEvents; ++i) {
    ASSERT_EQ(owner.PostData(0, [&] { ran.fetch_add(1, std::memory_order_relaxed); }),
              BridgeControl::OwnerQueue::PostResult::Queued);
  }
  EXPECT_EQ(owner.PostData(0, [] {}), BridgeControl::OwnerQueue::PostResult::DataCountLimit);
  for (size_t i = 0; i < BridgeControl::OwnerQueue::kReservedControlEvents; ++i) {
    ASSERT_EQ(owner.PostControl([&] { ran.fetch_add(1, std::memory_order_relaxed); }),
              BridgeControl::OwnerQueue::PostResult::Queued);
  }
  EXPECT_EQ(owner.PostControl([] {}), BridgeControl::OwnerQueue::PostResult::ControlCountLimit);
  releaseTask.set_value();

  EXPECT_TRUE(owner.StopAndJoin());
  EXPECT_EQ(ran.load(std::memory_order_relaxed), 1U + BridgeControl::OwnerQueue::kMaxDataEvents +
                                                     BridgeControl::OwnerQueue::kReservedControlEvents);
}

TEST(BridgeOwnerQueue, PayloadByteLimitRejectsOversizeAndAggregateOverflow) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());
  std::promise<void> taskStarted;
  std::promise<void> releaseTask;
  std::shared_future<void> releaseSignal = releaseTask.get_future().share();
  ASSERT_EQ(owner.PostData(0, [&] {
              taskStarted.set_value();
              releaseSignal.wait();
            }), BridgeControl::OwnerQueue::PostResult::Queued);
  taskStarted.get_future().wait();

  EXPECT_EQ(owner.PostData(BridgeControl::OwnerQueue::kMaxDataBytes + 1, [] {}),
            BridgeControl::OwnerQueue::PostResult::DataByteLimit);
  EXPECT_EQ(owner.PostData(BridgeControl::OwnerQueue::kMaxDataBytes, [] {}),
            BridgeControl::OwnerQueue::PostResult::Queued);
  EXPECT_EQ(owner.PostData(1, [] {}), BridgeControl::OwnerQueue::PostResult::DataByteLimit);
  releaseTask.set_value();
  EXPECT_TRUE(owner.StopAndJoin());
}

TEST(BridgeOwnerQueue, StopCannotJoinFromItsOwnerThread) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());
  std::promise<bool> joinResult;
  ASSERT_EQ(owner.PostControl([&] { joinResult.set_value(owner.StopAndJoin()); }),
            BridgeControl::OwnerQueue::PostResult::Queued);
  EXPECT_FALSE(joinResult.get_future().get());
  EXPECT_TRUE(owner.StopAndJoin());
}

TEST(BridgeOwnerQueue, CloseFenceCompletesWhileCallbackWaitsForOwner) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());
  std::promise<void> blockerStarted;
  std::promise<void> releaseBlocker;
  std::shared_future<void> releaseSignal = releaseBlocker.get_future().share();
  ASSERT_EQ(owner.PostData(0, [&] {
              blockerStarted.set_value();
              releaseSignal.wait();
            }), BridgeControl::OwnerQueue::PostResult::Queued);
  blockerStarted.get_future().wait();

  std::atomic<bool> callbackStarted{false};
  std::atomic<bool> closeProcessed{false};
  std::thread callback([&] {
    callbackStarted.store(true, std::memory_order_release);
    EXPECT_TRUE(owner.InvokeControlAndWait([&] { closeProcessed.store(true, std::memory_order_release); }));
  });
  while (!callbackStarted.load(std::memory_order_acquire)) std::this_thread::yield();
  releaseBlocker.set_value();

  callback.join();
  EXPECT_TRUE(closeProcessed.load(std::memory_order_acquire));
  EXPECT_TRUE(owner.StopAndJoin());
}

TEST(BridgeOwnerQueue, CloseRacingInFlightCallbackSkipsStaleWorkBeforeLeaseDrain) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());
  auto lifetime = std::make_shared<BridgeControl::ConnectionLifetime>();
  auto lease = lifetime->TryAcquire();
  ASSERT_NE(lease, nullptr);

  std::promise<void> workStarted;
  std::promise<void> releaseWork;
  std::shared_future<void> releaseSignal = releaseWork.get_future().share();
  std::atomic<bool> staleWorkUsedTarget{false};
  ASSERT_EQ(owner.PostData(1, [lifetime, lease, &workStarted, releaseSignal, &staleWorkUsedTarget] {
              workStarted.set_value();
              releaseSignal.wait();
              if (lifetime->IsOpen()) staleWorkUsedTarget.store(true, std::memory_order_release);
            }), BridgeControl::OwnerQueue::PostResult::Queued);
  workStarted.get_future().wait();

  std::promise<void> closeCompleted;
  std::promise<void> admissionClosed;
  std::thread callback([&] {
    lifetime->CloseAdmission();
    admissionClosed.set_value();
    EXPECT_TRUE(owner.InvokeControlAndWait([&] { closeCompleted.set_value(); }));
    lease.reset();
    lifetime->WaitForLeases();
  });
  admissionClosed.get_future().wait();
  releaseWork.set_value();
  EXPECT_EQ(closeCompleted.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
  callback.join();
  EXPECT_FALSE(staleWorkUsedTarget.load(std::memory_order_acquire));
  EXPECT_TRUE(owner.StopAndJoin());
}

TEST(BridgeOwnerQueue, ReentrantOwnerCloseSkipsAlreadyQueuedGameSocketWork) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());
  auto lifetime = std::make_shared<BridgeControl::ConnectionLifetime>();
  auto closeLease = lifetime->TryAcquire();
  auto queuedLease = lifetime->TryAcquire();
  ASSERT_NE(closeLease, nullptr);
  ASSERT_NE(queuedLease, nullptr);

  std::promise<void> blockerStarted;
  std::promise<void> releaseBlocker;
  std::shared_future<void> releaseSignal = releaseBlocker.get_future().share();
  ASSERT_EQ(owner.PostData(0, [&] {
              blockerStarted.set_value();
              releaseSignal.wait();
            }), BridgeControl::OwnerQueue::PostResult::Queued);
  blockerStarted.get_future().wait();

  std::promise<void> closeRan;
  std::atomic<bool> staleSocketTouched{false};
  ASSERT_EQ(owner.PostData(1, [lifetime, queuedLease, &staleSocketTouched] {
              if (lifetime->IsOpen()) staleSocketTouched.store(true, std::memory_order_release);
            }), BridgeControl::OwnerQueue::PostResult::Queued);
  ASSERT_EQ(owner.PostControl([&owner, lifetime, closeLease, &closeRan] {
              lifetime->CloseAdmission();
              EXPECT_TRUE(owner.InvokeControlAndWait([&] { closeRan.set_value(); }));
            }), BridgeControl::OwnerQueue::PostResult::Queued);
  releaseBlocker.set_value();

  EXPECT_EQ(closeRan.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_TRUE(owner.StopAndJoin());
  closeLease.reset();
  queuedLease.reset();
  EXPECT_FALSE(staleSocketTouched.load(std::memory_order_acquire));
}

TEST(BridgeConnectionLifetime, RejectionFenceCloseCallbackStillCleansPairOnce) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());
  auto lifetime = std::make_shared<BridgeControl::ConnectionLifetime>();
  auto rejectionLease = lifetime->TryAcquire();
  ASSERT_NE(rejectionLease, nullptr);
  lifetime->RequestClose();
  EXPECT_FALSE(lifetime->TryAcquire());

  std::atomic<bool> pairPresent{true};
  std::promise<void> closeHandled;
  ASSERT_TRUE(owner.PostFence([&owner, lifetime, rejectionLease, &pairPresent, &closeHandled] {
    // This models ixwebsocket's reentrant Close callback after the queue-limit
    // fence calls WebSocket::close. It must still reach pair cleanup although
    // RequestClose has already rejected regular callbacks.
    auto closeLease = lifetime->TryAcquireClose();
    EXPECT_NE(closeLease, nullptr);
    if (closeLease) {
      EXPECT_TRUE(owner.InvokeControlAndWait([&pairPresent] {
        pairPresent.store(false, std::memory_order_release);
      }));
      closeLease.reset();
    }
    EXPECT_FALSE(lifetime->TryAcquireClose());
    closeHandled.set_value();
  }));
  rejectionLease.reset();

  EXPECT_EQ(closeHandled.get_future().wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  lifetime->WaitForLeases();
  EXPECT_FALSE(pairPresent.load(std::memory_order_acquire));
  EXPECT_TRUE(owner.StopAndJoin());
}
