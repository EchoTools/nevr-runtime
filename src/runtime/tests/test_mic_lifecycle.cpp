#include "core/mic_lifecycle.h"
#include "core/mic_capture_drain.h"
#include "core/mic_dsp.h"
#include "runtime/patch/mic_policy.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

struct FakeAudio {
  bool createOk = true;
  bool startOk = true;
  bool workerCreateOk = true;
  bool failedWorkerExists = false;
  bool signalOk = true;
  bool stopOk = true;
  bool clientInvalidated = false;  // AUDCLNT_E_DEVICE_INVALIDATED: Start fails until the client is re-acquired
  bool recoverOk = true;
  uint32_t recoverCalls = 0;
  MicWorkerWaitResult waitResult = MicWorkerWaitResult::Signaled;
  uint32_t createCalls = 0;
  uint32_t startCalls = 0;
  uint32_t workerCreateCalls = 0;
  uint32_t signalCalls = 0;
  uint32_t waitCalls = 0;
  uint32_t closeCalls = 0;
  uint32_t stopCalls = 0;
  uint32_t releaseCalls = 0;
  uint32_t resetCalls = 0;
  bool blockWait = false;
  bool waitEntered = false;
  bool allowWaitToFinish = false;
  bool reenterStart = false;
  bool reentrantStartResult = true;
  uint32_t ownerThread = 41;
  MicCaptureLifecycle* lifecycle = nullptr;
  std::mutex waitMutex;
  std::condition_variable waitCondition;
};

MicLifecycleOperations Ops(FakeAudio& fake);

bool CreateResources(void* context) {
  auto& fake = *static_cast<FakeAudio*>(context);
  ++fake.createCalls;
  return fake.createOk;
}

bool StartAudio(void* context) {
  auto& fake = *static_cast<FakeAudio*>(context);
  ++fake.startCalls;
  if (fake.clientInvalidated) return false;
  if (fake.reenterStart && fake.lifecycle != nullptr) {
    fake.reentrantStartResult = fake.lifecycle->Start(fake.ownerThread, Ops(fake));
  }
  return fake.startOk;
}

MicWorkerCreateResult CreateWorker(void* context) {
  auto& fake = *static_cast<FakeAudio*>(context);
  ++fake.workerCreateCalls;
  if (fake.workerCreateOk) return MicWorkerCreateResult::Started;
  return fake.failedWorkerExists ? MicWorkerCreateResult::FailedWithWorker
                                 : MicWorkerCreateResult::FailedWithoutWorker;
}

bool RequestStop(void* context) {
  auto& fake = *static_cast<FakeAudio*>(context);
  ++fake.signalCalls;
  return fake.signalOk;
}

MicWorkerWaitResult WaitWorker(void* context, uint32_t timeoutMs) {
  auto& fake = *static_cast<FakeAudio*>(context);
  ++fake.waitCalls;
  EXPECT_EQ(timeoutMs, 2000u);
  if (fake.blockWait) {
    std::unique_lock<std::mutex> lock(fake.waitMutex);
    fake.waitEntered = true;
    fake.waitCondition.notify_all();
    fake.waitCondition.wait(lock, [&fake]() { return fake.allowWaitToFinish; });
  }
  return fake.waitResult;
}

void CloseWorker(void* context) { ++static_cast<FakeAudio*>(context)->closeCalls; }

bool StopAudio(void* context) {
  auto& fake = *static_cast<FakeAudio*>(context);
  ++fake.stopCalls;
  return fake.stopOk;
}

void ReleaseResources(void* context) { ++static_cast<FakeAudio*>(context)->releaseCalls; }

void ResetStream(void* context) { ++static_cast<FakeAudio*>(context)->resetCalls; }

bool RecoverAudio(void* context) {
  auto& fake = *static_cast<FakeAudio*>(context);
  ++fake.recoverCalls;
  if (fake.recoverOk) fake.clientInvalidated = false;
  return fake.recoverOk;
}

MicLifecycleOperations Ops(FakeAudio& fake) {
  return {&fake, CreateResources, StartAudio, CreateWorker, RequestStop, WaitWorker,
          CloseWorker, StopAudio, ReleaseResources, ResetStream, RecoverAudio};
}

struct FakeDrain {
  bool isCancelled = false;
  bool queryOk = true;
  bool acquireOk = true;
  bool releaseOk = true;
  bool throwDuringProcess = false;
  uint32_t cancelAfterProcess = 0;
  uint32_t queries = 0;
  uint32_t acquired = 0;
  uint32_t processed = 0;
  uint32_t released = 0;
};

bool DrainCancelled(void* context) { return static_cast<FakeDrain*>(context)->isCancelled; }

bool DrainNext(void* context, uint32_t* frames) {
  auto& fake = *static_cast<FakeDrain*>(context);
  ++fake.queries;
  if (!fake.queryOk) return false;
  *frames = 16;
  return true;
}

bool DrainAcquire(void* context, const void** data, uint32_t* frames, uint32_t* flags) {
  auto& fake = *static_cast<FakeDrain*>(context);
  if (!fake.acquireOk) return false;
  ++fake.acquired;
  *data = &fake;
  *frames = 16;
  *flags = 0;
  return true;
}

void DrainProcess(void* context, const void*, uint32_t, uint32_t) {
  auto& fake = *static_cast<FakeDrain*>(context);
  ++fake.processed;
  if (fake.throwDuringProcess) throw std::runtime_error("injected packet processing failure");
  if (fake.cancelAfterProcess != 0 && fake.processed == fake.cancelAfterProcess) fake.isCancelled = true;
}

bool DrainRelease(void* context, uint32_t frames) {
  auto& fake = *static_cast<FakeDrain*>(context);
  EXPECT_EQ(frames, 16u);
  ++fake.released;
  return fake.releaseOk;
}

MicCaptureDrainOperations DrainOps(FakeDrain& fake) {
  return {&fake, DrainCancelled, DrainNext, DrainAcquire, DrainProcess, DrainRelease};
}

class MicCaptureLifecycleTest : public ::testing::Test {
 protected:
  bool Create() { return lifecycle_.Create(kOwnerThread, Ops(fake_)); }
  bool Start() { return lifecycle_.Start(kOwnerThread, Ops(fake_)); }
  bool Stop() { return lifecycle_.Stop(kOwnerThread, Ops(fake_), 2000); }
  bool Destroy() { return lifecycle_.Destroy(kOwnerThread, Ops(fake_), 2000); }

  static constexpr uint32_t kOwnerThread = 41;
  FakeAudio fake_;
  MicCaptureLifecycle lifecycle_;
};

TEST_F(MicCaptureLifecycleTest, CreateThreadFailureRollsAudioBackAndAllowsRetry) {
  ASSERT_TRUE(Create());
  const uint32_t resetsAfterCreate = fake_.resetCalls;
  fake_.workerCreateOk = false;
  EXPECT_FALSE(Start());
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Ready);
  EXPECT_FALSE(lifecycle_.HasWorker());
  EXPECT_EQ(fake_.stopCalls, 1u);
  // One reset when the capture started (a new stream) and one when the failed start rolled back.
  EXPECT_EQ(fake_.resetCalls, resetsAfterCreate + 2u);

  fake_.workerCreateOk = true;
  EXPECT_TRUE(Start());
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Running);
  EXPECT_TRUE(Stop());
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Ready);
  EXPECT_TRUE(Destroy());
}

TEST_F(MicCaptureLifecycleTest, FailedRollbackStopFaultsWithoutWorkerUntilDestroyRetry) {
  ASSERT_TRUE(Create());
  fake_.workerCreateOk = false;
  fake_.stopOk = false;
  EXPECT_FALSE(Start());
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::FaultedNoWorker);
  EXPECT_FALSE(lifecycle_.HasWorker());
  EXPECT_FALSE(Start());

  fake_.stopOk = true;
  EXPECT_TRUE(Destroy());
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Closed);
  EXPECT_EQ(fake_.releaseCalls, 1u);
}

TEST_F(MicCaptureLifecycleTest, WorkerSetupFailureJoinsCreatedThreadBeforeRollback) {
  ASSERT_TRUE(Create());
  fake_.workerCreateOk = false;
  fake_.failedWorkerExists = true;
  EXPECT_FALSE(Start());
  EXPECT_EQ(fake_.signalCalls, 1u);
  EXPECT_EQ(fake_.waitCalls, 1u);
  EXPECT_EQ(fake_.closeCalls, 1u);
  EXPECT_EQ(fake_.stopCalls, 1u);
  EXPECT_FALSE(lifecycle_.HasWorker());
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Ready);
  EXPECT_TRUE(Destroy());
}

TEST_F(MicCaptureLifecycleTest, TimeoutAndWaitFailureRetainWorkerResourcesUntilRetryJoins) {
  ASSERT_TRUE(Create());
  ASSERT_TRUE(Start());
  fake_.waitResult = MicWorkerWaitResult::Timeout;
  EXPECT_FALSE(Stop());
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::FaultedWorker);
  EXPECT_TRUE(lifecycle_.HasWorker());
  EXPECT_EQ(fake_.closeCalls, 0u);
  EXPECT_EQ(fake_.stopCalls, 0u);
  EXPECT_EQ(fake_.releaseCalls, 0u);
  EXPECT_FALSE(Start());

  fake_.waitResult = MicWorkerWaitResult::Failed;
  EXPECT_FALSE(Destroy());
  EXPECT_TRUE(lifecycle_.HasWorker());
  EXPECT_EQ(fake_.closeCalls, 0u);
  EXPECT_EQ(fake_.releaseCalls, 0u);

  fake_.waitResult = MicWorkerWaitResult::Signaled;
  EXPECT_TRUE(Destroy());
  EXPECT_FALSE(lifecycle_.HasWorker());
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Closed);
  EXPECT_EQ(fake_.closeCalls, 1u);
  EXPECT_EQ(fake_.releaseCalls, 1u);
}

TEST_F(MicCaptureLifecycleTest, FailedWakeStillAttemptsBoundedJoin) {
  ASSERT_TRUE(Create());
  ASSERT_TRUE(Start());
  fake_.signalOk = false;
  EXPECT_TRUE(Stop());
  EXPECT_EQ(fake_.signalCalls, 1u);
  EXPECT_EQ(fake_.waitCalls, 1u);
  EXPECT_EQ(fake_.closeCalls, 1u);
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Ready);
  EXPECT_TRUE(Destroy());
}

TEST_F(MicCaptureLifecycleTest, StopFailureAfterConfirmedJoinBlocksRestartButAllowsDestroyRetry) {
  ASSERT_TRUE(Create());
  ASSERT_TRUE(Start());
  fake_.stopOk = false;
  EXPECT_FALSE(Stop());
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::FaultedNoWorker);
  EXPECT_FALSE(lifecycle_.HasWorker());
  EXPECT_FALSE(Start());

  fake_.stopOk = true;
  EXPECT_TRUE(Destroy());
  EXPECT_EQ(fake_.releaseCalls, 1u);
}

TEST_F(MicCaptureLifecycleTest, RejectsWrongThreadWithoutReleasingOwnerResources) {
  ASSERT_TRUE(Create());
  EXPECT_FALSE(lifecycle_.Start(kOwnerThread + 1, Ops(fake_)));
  EXPECT_FALSE(lifecycle_.Stop(kOwnerThread + 1, Ops(fake_), 2000));
  EXPECT_FALSE(lifecycle_.Destroy(kOwnerThread + 1, Ops(fake_), 2000));
  EXPECT_EQ(fake_.startCalls, 0u);
  EXPECT_EQ(fake_.releaseCalls, 0u);
  EXPECT_TRUE(Destroy());
}

TEST_F(MicCaptureLifecycleTest, ConcurrentStartWaitsForStopTransitionToFinish) {
  ASSERT_TRUE(Create());
  ASSERT_TRUE(Start());
  fake_.blockWait = true;
  std::atomic<bool> stopResult{false};
  std::atomic<bool> startFinished{false};
  std::thread stopper([this, &stopResult]() { stopResult.store(Stop()); });
  {
    std::unique_lock<std::mutex> lock(fake_.waitMutex);
    fake_.waitCondition.wait(lock, [this]() { return fake_.waitEntered; });
  }
  std::thread starter([this, &startFinished]() {
    (void)Start();
    startFinished.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(startFinished.load());
  {
    std::lock_guard<std::mutex> lock(fake_.waitMutex);
    fake_.allowWaitToFinish = true;
  }
  fake_.waitCondition.notify_all();
  stopper.join();
  starter.join();
  EXPECT_TRUE(stopResult.load());
  EXPECT_TRUE(startFinished.load());
  EXPECT_EQ(fake_.startCalls, 2u);
  EXPECT_TRUE(Stop());
  EXPECT_TRUE(Destroy());
}

TEST_F(MicCaptureLifecycleTest, ReentrantLifecycleCallIsRejectedWithoutDeadlock) {
  fake_.lifecycle = &lifecycle_;
  fake_.reenterStart = true;
  ASSERT_TRUE(Create());
  EXPECT_TRUE(Start());
  EXPECT_FALSE(fake_.reentrantStartResult);
  EXPECT_EQ(fake_.workerCreateCalls, 1u);
  EXPECT_TRUE(Stop());
  EXPECT_TRUE(Destroy());
}

// #95: the game polls MicAvailable/MicRead while capture is stopped. That must not latch the
// "reader is listening" state of the next capture, or its start-of-capture backlog is never dropped.
namespace {
struct RingStream {
  MicRingBuffer ring{9600};
};
void RingResetStream(void* context) { static_cast<RingStream*>(context)->ring.Reset(); }
bool RingStartAudio(void*) { return true; }
MicWorkerCreateResult RingCreateWorker(void*) { return MicWorkerCreateResult::Started; }
bool RingCreateResources(void*) { return true; }
bool RingRequestStop(void*) { return true; }
MicWorkerWaitResult RingWaitWorker(void*, uint32_t) { return MicWorkerWaitResult::Signaled; }
void RingCloseWorker(void*) {}
bool RingStopAudio(void*) { return true; }
void RingReleaseResources(void*) {}

MicLifecycleOperations RingOps(RingStream& stream) {
  return {&stream,          RingCreateResources, RingStartAudio,        RingCreateWorker,
          RingRequestStop, RingWaitWorker,      RingCloseWorker,       RingStopAudio,
          RingReleaseResources, RingResetStream};
}
}  // namespace

// #399: the two WASAPI codes that mean the client is dead for good (AUDCLNT_ERR(0x4) and AUDCLNT_ERR(0x26),
// FACILITY_AUDCLNT 0x889), and no other code is taken for it.
TEST(MicHresult, OnlyTheInvalidatedCodesMeanTheClientIsDead) {
  EXPECT_TRUE(MicHresultMeansDeviceInvalidated(static_cast<int32_t>(0x88890004u)));  // DEVICE_INVALIDATED
  EXPECT_TRUE(MicHresultMeansDeviceInvalidated(static_cast<int32_t>(0x88890026u)));  // RESOURCES_INVALIDATED
  EXPECT_FALSE(MicHresultMeansDeviceInvalidated(0));                                  // S_OK
  EXPECT_FALSE(MicHresultMeansDeviceInvalidated(1));                                  // S_FALSE
  EXPECT_FALSE(MicHresultMeansDeviceInvalidated(static_cast<int32_t>(0x80004005u)));  // E_FAIL
  EXPECT_FALSE(MicHresultMeansDeviceInvalidated(static_cast<int32_t>(0x88890003u)));  // NOT_INITIALIZED
  EXPECT_FALSE(MicHresultMeansDeviceInvalidated(static_cast<int32_t>(0x88890005u)));  // NOT_STOPPED
  EXPECT_FALSE(MicHresultMeansDeviceInvalidated(static_cast<int32_t>(0x88890010u)));  // SERVICE_NOT_RUNNING
  EXPECT_FALSE(MicHresultMeansDeviceInvalidated(static_cast<int32_t>(0x00000004u)));  // the low word alone
}

// #399: the capture device was invalidated (0x88890004); the dead IAudioClient can never Start again, so the
// restart re-acquires the default endpoint and capture resumes.
TEST_F(MicCaptureLifecycleTest, StartOnAnInvalidatedClientReacquiresTheEndpointAndRuns) {
  ASSERT_TRUE(Create());
  fake_.clientInvalidated = true;
  EXPECT_TRUE(Start());
  EXPECT_EQ(fake_.recoverCalls, 1u);
  EXPECT_EQ(fake_.startCalls, 2u);
  EXPECT_EQ(fake_.workerCreateCalls, 1u);
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Running);
}

TEST_F(MicCaptureLifecycleTest, StartThatCannotReacquireStaysReadyAndTheNextStartTriesAgain) {
  ASSERT_TRUE(Create());
  fake_.clientInvalidated = true;
  fake_.recoverOk = false;
  EXPECT_FALSE(Start());
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Ready);
  EXPECT_EQ(fake_.workerCreateCalls, 0u);
  fake_.recoverOk = true;  // a capture device is plugged in again
  EXPECT_TRUE(Start());
  EXPECT_EQ(fake_.recoverCalls, 2u);
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Running);
}

TEST_F(MicCaptureLifecycleTest, RecoverWhileRunningJoinsTheWorkerReacquiresAndStartsANewOne) {
  ASSERT_TRUE(Create());
  ASSERT_TRUE(Start());
  fake_.clientInvalidated = true;  // the device went away under the running worker
  const uint32_t stopsBefore = fake_.stopCalls;
  EXPECT_TRUE(lifecycle_.Recover(kOwnerThread, Ops(fake_), 2000));
  EXPECT_EQ(fake_.closeCalls, 1u);                // the old worker was joined and closed
  EXPECT_EQ(fake_.stopCalls, stopsBefore);        // the dead client is not stopped, it is replaced
  EXPECT_EQ(fake_.recoverCalls, 1u);
  EXPECT_EQ(fake_.workerCreateCalls, 2u);
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Running);
  EXPECT_TRUE(Stop());                            // and the new capture stops normally
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Ready);
}

TEST_F(MicCaptureLifecycleTest, FailedRecoverLeavesTheProviderReadyForTheGamesNextStart) {
  ASSERT_TRUE(Create());
  ASSERT_TRUE(Start());
  fake_.clientInvalidated = true;
  fake_.recoverOk = false;
  const uint32_t stopsBefore = fake_.stopCalls;
  EXPECT_FALSE(lifecycle_.Recover(kOwnerThread, Ops(fake_), 2000));
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Ready);
  EXPECT_FALSE(lifecycle_.HasWorker());
  EXPECT_TRUE(Stop());  // the game's Stop is harmless, and its Start re-acquires
  EXPECT_EQ(fake_.stopCalls, stopsBefore) << "a client that was released must not be stopped again";
  fake_.recoverOk = true;
  EXPECT_TRUE(Start());
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Running);
}

TEST_F(MicCaptureLifecycleTest, RecoverIsRefusedOnAClosedProviderAndFromAnotherThread) {
  EXPECT_FALSE(lifecycle_.Recover(kOwnerThread, Ops(fake_), 2000));
  ASSERT_TRUE(Create());
  ASSERT_TRUE(Start());
  EXPECT_FALSE(lifecycle_.Recover(kOwnerThread + 1, Ops(fake_), 2000));
  EXPECT_EQ(fake_.recoverCalls, 0u);
  EXPECT_EQ(lifecycle_.State(), MicLifecycleState::Running);
}

TEST(MicCaptureLifecycleStream, ReaderCallWhileStoppedDoesNotSkipTheNextCapturesBacklogDrop) {
  RingStream stream;
  MicCaptureLifecycle lifecycle;
  const MicLifecycleOperations ops = RingOps(stream);
  ASSERT_TRUE(lifecycle.Create(7, ops));

  // Audio is waiting while capture is stopped (left from a previous stream); the game asks for it, drains it.
  const int16_t leftover[50] = {};
  stream.ring.Push(leftover, 50);
  EXPECT_EQ(stream.ring.NoteReaderActive(), 50u);
  stream.ring.Push(leftover, 50);
  int16_t out[50];
  EXPECT_EQ(stream.ring.Pop(out, 50), 50u);
  EXPECT_TRUE(stream.ring.ReaderActive());

  ASSERT_TRUE(lifecycle.Start(7, ops));
  EXPECT_FALSE(stream.ring.ReaderActive()) << "a new capture starts with no consumer";

  // The capture worker fills the ring before the game's first call of this capture.
  const int16_t backlog[1000] = {};
  stream.ring.Push(backlog, 1000);
  EXPECT_EQ(stream.ring.NoteReaderActive(), 1000u) << "the first call that finds audio must drop the backlog";
}

TEST(MicCaptureLifecycleStream, EveryRestartRearmsTheBacklogDrop) {
  RingStream stream;
  MicCaptureLifecycle lifecycle;
  const MicLifecycleOperations ops = RingOps(stream);
  ASSERT_TRUE(lifecycle.Create(7, ops));
  ASSERT_TRUE(lifecycle.Start(7, ops));
  const int16_t first[10] = {};
  stream.ring.Push(first, 10);
  EXPECT_EQ(stream.ring.NoteReaderActive(), 10u);
  ASSERT_TRUE(lifecycle.Stop(7, ops, 2000));
  ASSERT_TRUE(lifecycle.Start(7, ops));
  const int16_t backlog[300] = {};
  stream.ring.Push(backlog, 300);
  EXPECT_EQ(stream.ring.NoteReaderActive(), 300u);
}

// The game's real order, from the two client runs (nevr-2026-10-10T12-00-22.946 and T12-21-07.530): MicStart,
// MicAvailable polls from that moment on (the ring empty, then holding audio) and the first MicRead only
// after the ring has overflowed. The overflows before that first read are silent (ReaderActive false is what
// the provider's warning waits for); the backlog is dropped at the first MicRead; after that read the game is
// a consumer and a stall would warn.
TEST(MicCaptureLifecycleStream, TheGamesSparseStartupPollsDoNotArmTheOverflowWarning) {
  RingStream stream;
  MicCaptureLifecycle lifecycle;
  const MicLifecycleOperations ops = RingOps(stream);
  ASSERT_TRUE(lifecycle.Create(7, ops));
  ASSERT_TRUE(lifecycle.Start(7, ops));

  EXPECT_EQ(stream.ring.Available(), 0u);  // MicAvailable right after MicStart: nothing yet
  EXPECT_FALSE(stream.ring.ReaderActive());

  const int16_t chunk[1000] = {};
  for (int i = 0; i < 12; ++i) stream.ring.Push(chunk, 1000);  // 12000 > 9600, nobody consuming
  EXPECT_FALSE(stream.ring.ReaderActive());

  EXPECT_EQ(stream.ring.Available(), 9600u);  // later polls see the newest 200 ms; a poll drains nothing
  EXPECT_FALSE(stream.ring.ReaderActive()) << "a poll that drained nothing is not a consumer";

  // The first MicRead: the provider drops the backlog, then pops. The drain returns what arrived since.
  EXPECT_EQ(stream.ring.NoteReaderActive(), 9600u);
  stream.ring.Push(chunk, 1000);
  int16_t out[1000];
  EXPECT_EQ(stream.ring.Pop(out, 1000), 1000u);
  EXPECT_TRUE(stream.ring.ReaderActive());
}

TEST(MicCaptureDrain, CancellationDuringContinuousPacketDrainReleasesEveryAcquiredPacket) {
  FakeDrain fake;
  fake.cancelAfterProcess = 2;
  const MicCaptureDrainStatus status = DrainMicCapturePackets(DrainOps(fake));
  EXPECT_EQ(status, MicCaptureDrainStatus::Cancelled);
  EXPECT_EQ(fake.acquired, 2u);
  EXPECT_EQ(fake.processed, 2u);
  EXPECT_EQ(fake.released, 2u);
  EXPECT_EQ(fake.queries, 2u);
}

TEST(MicCaptureDrain, AcquisitionFailureDoesNotReleaseUnacquiredPacket) {
  FakeDrain fake;
  fake.acquireOk = false;
  EXPECT_EQ(DrainMicCapturePackets(DrainOps(fake)), MicCaptureDrainStatus::BufferAcquireFailed);
  EXPECT_EQ(fake.acquired, 0u);
  EXPECT_EQ(fake.released, 0u);
}

TEST(MicCaptureDrain, ReleaseIsAttemptedOnceEvenWhenItFails) {
  FakeDrain fake;
  fake.releaseOk = false;
  EXPECT_EQ(DrainMicCapturePackets(DrainOps(fake)), MicCaptureDrainStatus::BufferReleaseFailed);
  EXPECT_EQ(fake.acquired, 1u);
  EXPECT_EQ(fake.processed, 1u);
  EXPECT_EQ(fake.released, 1u);
}

TEST(MicCaptureDrain, ProcessingExceptionStillReleasesBufferOnce) {
  FakeDrain fake;
  fake.throwDuringProcess = true;
  EXPECT_EQ(DrainMicCapturePackets(DrainOps(fake)), MicCaptureDrainStatus::BufferProcessFailed);
  EXPECT_EQ(fake.acquired, 1u);
  EXPECT_EQ(fake.processed, 1u);
  EXPECT_EQ(fake.released, 1u);
}

TEST(MicComBalance, SuccessfulSOkAndSFalseRequireUninitializeButChangedModeDoesNot) {
  EXPECT_TRUE(MicComInitializationRequiresUninitialize(0));
  EXPECT_TRUE(MicComInitializationRequiresUninitialize(1));
  EXPECT_FALSE(MicComInitializationRequiresUninitialize(static_cast<int32_t>(0x80010106u)));
}

}  // namespace

// #402: the WASAPI provider answers the game's Mic* lookups under Wine/Proton only; on native Windows the
// game's own mic path stays, and the boot line says which.
TEST(MicProviderPolicy, InstalledUnderWineAndNotOnNativeWindows) {
  EXPECT_TRUE(nevr_mic_policy::ShouldInstallProvider(/*isWine=*/true));
  EXPECT_FALSE(nevr_mic_policy::ShouldInstallProvider(/*isWine=*/false));
}

TEST(MicProviderPolicy, BootLineNamesTheDecisionAndNeverClaimsAProviderThatIsNotInstalled) {
  EXPECT_STREQ(nevr_mic_policy::BootLine(true), "[NEVR.MIC] provider installed: Wine\n");
  EXPECT_STREQ(nevr_mic_policy::BootLine(false), "[NEVR.MIC] provider not installed: native Windows\n");
}
