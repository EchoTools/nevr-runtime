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

TEST(BridgeOwnerQueue, DataAndControlEventsShareFifoAndAllowReentrantPost) {
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
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
  EXPECT_NE(taskThread, std::this_thread::get_id());
}

TEST(BridgeOwnerQueue, FenceDoesNotOvertakeEarlierControlEvents) {
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
  EXPECT_EQ(order.front(), 1);
  EXPECT_EQ(order.back(), 2);
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

TEST(BridgeOwnerQueue, TerminalCloseUsesReservedSlotAndDoesNotOvertakeFifo) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());
  ASSERT_TRUE(owner.ReserveCloseSlot());
  std::promise<void> blockerStarted;
  std::promise<void> releaseBlocker;
  std::shared_future<void> releaseSignal = releaseBlocker.get_future().share();
  std::vector<int> order;
  std::mutex mutex;
  ASSERT_EQ(owner.PostData(0, [&] {
              blockerStarted.set_value();
              releaseSignal.wait();
              std::lock_guard<std::mutex> lock(mutex);
              order.push_back(0);
            }), BridgeControl::OwnerQueue::PostResult::Queued);
  blockerStarted.get_future().wait();
  for (size_t i = 0; i < BridgeControl::OwnerQueue::kMaxDataEvents; ++i) {
    ASSERT_EQ(owner.PostData(0, [&order, &mutex] {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(1);
              }), BridgeControl::OwnerQueue::PostResult::Queued);
  }
  uint64_t closeSequence = 0;
  EXPECT_EQ(owner.PostTerminalClose([&order, &mutex] {
              std::lock_guard<std::mutex> lock(mutex);
              order.push_back(2);
            }, &closeSequence), BridgeControl::OwnerQueue::PostResult::Queued);
  EXPECT_EQ(owner.PostData(0, [] {}), BridgeControl::OwnerQueue::PostResult::DataCountLimit);
  EXPECT_EQ(owner.PostTerminalClose([] {}), BridgeControl::OwnerQueue::PostResult::CloseCapacityLimit);
  releaseBlocker.set_value();
  ASSERT_TRUE(owner.StopAndJoin());
  ASSERT_EQ(order.size(), BridgeControl::OwnerQueue::kMaxDataEvents + 2);
  EXPECT_EQ(order.front(), 0);
  EXPECT_EQ(order.back(), 2);
  EXPECT_EQ(closeSequence, static_cast<uint64_t>(BridgeControl::OwnerQueue::kMaxDataEvents + 2));
  owner.ReleaseCloseSlot();
}

TEST(ConnectionLifetime, CloseAndEnqueueShareAdmissionLinearization) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());
  ASSERT_TRUE(owner.ReserveCloseSlot());
  auto lifetime = std::make_shared<BridgeControl::ConnectionLifetime>();
  auto lease = lifetime->TryAcquire();
  ASSERT_NE(lease, nullptr);
  std::promise<void> postPaused;
  std::promise<void> continuePost;
  std::shared_future<void> continueSignal = continuePost.get_future().share();
  std::atomic<bool> workRan{false};
  std::atomic<bool> accepted{true};
  std::thread callback([&] {
    postPaused.set_value();
    continueSignal.wait();
    accepted.store(lifetime->PostIfAccepting([&] {
      return owner.PostData(1, [&] { workRan.store(true, std::memory_order_release); }) ==
          BridgeControl::OwnerQueue::PostResult::Queued;
    }), std::memory_order_release);
  });
  postPaused.get_future().wait();
  const auto close = lifetime->CloseAndPost(false, [&](const auto& completion) {
    return owner.PostTerminalClose([&, completion] {
      completion->set_value(true);
    }) == BridgeControl::OwnerQueue::PostResult::Queued;
  });
  ASSERT_TRUE(close.first);
  EXPECT_TRUE(close.completion.get());
  continuePost.set_value();
  callback.join();
  EXPECT_FALSE(accepted.load(std::memory_order_acquire));
  lease.reset();
  EXPECT_TRUE(owner.StopAndJoin());
  EXPECT_FALSE(workRan.load(std::memory_order_acquire));
}

TEST(ConnectionLifetime, AdmittedResponseRunsBeforeOrdinaryCloseCleanup) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());
  ASSERT_TRUE(owner.ReserveCloseSlot());
  auto lifetime = std::make_shared<BridgeControl::ConnectionLifetime>();
  auto lease = lifetime->TryAcquire();
  ASSERT_NE(lease, nullptr);
  std::vector<int> order;
  std::mutex mutex;
  uint64_t responseSequence = 0;
  ASSERT_TRUE(lifetime->PostIfAccepting([&] {
    return owner.PostData(1, [&] {
      std::lock_guard<std::mutex> lock(mutex);
      order.push_back(1);
    }, &responseSequence) == BridgeControl::OwnerQueue::PostResult::Queued;
  }));
  std::promise<bool> closeResult;
  std::thread callback([&] {
    const auto close = lifetime->CloseAndPost(false, [&](const auto& completion) {
      return owner.PostTerminalClose([&order, &mutex, completion, &closeResult] {
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(2);
        completion->set_value(true);
        closeResult.set_value(true);
      }) == BridgeControl::OwnerQueue::PostResult::Queued;
    });
    EXPECT_TRUE(close.queued);
    EXPECT_TRUE(close.completion.get());
  });
  callback.join();
  lease.reset();
  EXPECT_TRUE(owner.StopAndJoin());
  EXPECT_EQ(order, (std::vector<int>{1, 2}));
  EXPECT_EQ(responseSequence, 1U);
  EXPECT_TRUE(closeResult.get_future().get());
}

TEST(ConnectionLifetime, DuplicateCloseSharesOneTerminalCompletion) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());
  ASSERT_TRUE(owner.ReserveCloseSlot());
  auto lifetime = std::make_shared<BridgeControl::ConnectionLifetime>();
  std::atomic<size_t> cleanupCount{0};
  const auto first = lifetime->CloseAndPost(false, [&](const auto& completion) {
    return owner.PostTerminalClose([&, completion] {
      cleanupCount.fetch_add(1, std::memory_order_relaxed);
      completion->set_value(true);
    }) == BridgeControl::OwnerQueue::PostResult::Queued;
  });
  const auto duplicate = lifetime->CloseAndPost(false, [&](const auto&) {
    ADD_FAILURE() << "duplicate close attempted a second terminal insert";
    return false;
  });
  EXPECT_TRUE(first.first);
  EXPECT_FALSE(duplicate.first);
  EXPECT_EQ(first.completion.get(), duplicate.completion.get());
  EXPECT_TRUE(owner.StopAndJoin());
  EXPECT_EQ(cleanupCount.load(std::memory_order_relaxed), 1U);
}

TEST(ConnectionLifetime, ReentrantRetirementAbortsQueuedBorrowedSocketWork) {
  BridgeControl::OwnerQueue owner;
  ASSERT_TRUE(owner.Start());
  ASSERT_TRUE(owner.ReserveCloseSlot());
  auto lifetime = std::make_shared<BridgeControl::ConnectionLifetime>();
  std::atomic<bool> socketAlive{true};
  std::atomic<size_t> socketTouches{0};
  std::promise<void> blockerStarted;
  std::promise<void> releaseBlocker;
  std::shared_future<void> releaseSignal = releaseBlocker.get_future().share();
  ASSERT_EQ(owner.PostData(0, [&] {
              blockerStarted.set_value();
              releaseSignal.wait();
            }), BridgeControl::OwnerQueue::PostResult::Queued);
  blockerStarted.get_future().wait();
  auto pendingLease = lifetime->TryAcquire();
  ASSERT_NE(pendingLease, nullptr);
  std::promise<void> closeReturned;
  ASSERT_EQ(owner.PostControl([&] {
    const auto close = lifetime->CloseAndPost(true, [&](const auto& completion) {
      return owner.PostTerminalClose([&, completion] {
        socketAlive.store(false, std::memory_order_release);
        completion->set_value(true);
      }) == BridgeControl::OwnerQueue::PostResult::Queued;
    });
    EXPECT_TRUE(close.first);
    // The owner reentrant path must return without waiting for its own FIFO.
    closeReturned.set_value();
  }), BridgeControl::OwnerQueue::PostResult::Queued);
  ASSERT_TRUE(lifetime->PostIfAccepting([&] {
    return owner.PostData(1, [&, pendingLease] {
      if (!lifetime->IsRetired() && socketAlive.load(std::memory_order_acquire)) {
        socketTouches.fetch_add(1, std::memory_order_relaxed);
      }
    }) == BridgeControl::OwnerQueue::PostResult::Queued;
  }));
  releaseBlocker.set_value();
  EXPECT_EQ(closeReturned.get_future().wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_TRUE(owner.StopAndJoin());
  EXPECT_FALSE(socketAlive.load(std::memory_order_acquire));
  EXPECT_EQ(socketTouches.load(std::memory_order_relaxed), 0U);
}
