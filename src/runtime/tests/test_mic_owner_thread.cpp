// GH #51: echovr.exe calls the mic provider's Create/Start/Stop/Destroy from
// whichever thread its component-system job (CR15NetVoipBroadcasterCS::
// UpdateGlobal, 0x140d7cfc0) happens to run on. MicCaptureLifecycle pins every
// transition to the thread that created the WASAPI resources (COM apartment +
// CoInitializeEx/CoUninitialize balance), so a direct call from the "wrong"
// game thread was rejected and capture never started. These tests pin the fix:
// MicOwnerThread marshals each call onto one dedicated thread.

#include "core/mic_lifecycle.h"
#include "core/mic_owner_thread.h"

#include <gtest/gtest.h>

#include <atomic>
#include <initializer_list>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

// A small integer per thread object, standing in for GetCurrentThreadId() so
// this test stays free of windows.h like the code under test. It comes from a
// counter, not from std::thread::id: the C library reuses the id of a joined
// thread, so two sequential game threads could share an id and a token (#424).
uint32_t ThreadToken() {
  static std::atomic<uint32_t> next{0};
  thread_local const uint32_t token = ++next;
  return token;
}

// Joins the owner thread when a test body returns early (a failed ASSERT):
// ~MicOwnerThread detaches a live thread by design, and a test object that
// dies while that thread waits on its members hangs the process instead of
// reporting the failure (#424).
struct OwnerGuard {
  explicit OwnerGuard(MicOwnerThread& owner) : owner_(owner) {}
  ~OwnerGuard() { owner_.Shutdown(); }
  OwnerGuard(const OwnerGuard&) = delete;
  OwnerGuard& operator=(const OwnerGuard&) = delete;

 private:
  MicOwnerThread& owner_;
};

struct RecordingAudio {
  std::mutex mutex;
  std::set<uint32_t> opThreads;
  uint32_t ops = 0;

  void Record() {
    std::lock_guard<std::mutex> lock(mutex);
    opThreads.insert(ThreadToken());
    ++ops;
  }
};

bool RecCreate(void* c) { static_cast<RecordingAudio*>(c)->Record(); return true; }
bool RecStartAudio(void* c) { static_cast<RecordingAudio*>(c)->Record(); return true; }
MicWorkerCreateResult RecCreateWorker(void* c) {
  static_cast<RecordingAudio*>(c)->Record();
  return MicWorkerCreateResult::Started;
}
bool RecRequestStop(void* c) { static_cast<RecordingAudio*>(c)->Record(); return true; }
MicWorkerWaitResult RecWaitWorker(void* c, uint32_t) {
  static_cast<RecordingAudio*>(c)->Record();
  return MicWorkerWaitResult::Signaled;
}
void RecCloseWorker(void* c) { static_cast<RecordingAudio*>(c)->Record(); }
bool RecStopAudio(void* c) { static_cast<RecordingAudio*>(c)->Record(); return true; }
void RecRelease(void* c) { static_cast<RecordingAudio*>(c)->Record(); }
void RecReset(void* c) { static_cast<RecordingAudio*>(c)->Record(); }

MicLifecycleOperations RecOps(RecordingAudio& audio) {
  return {&audio, RecCreate, RecStartAudio, RecCreateWorker, RecRequestStop, RecWaitWorker,
          RecCloseWorker, RecStopAudio, RecRelease, RecReset};
}

enum class Transition { Create, Start, Stop, Destroy };

// Mirrors the provider glue: the transition runs on the owner thread and
// identifies itself by the owner thread's id, never the game caller's.
struct TransitionCall {
  MicCaptureLifecycle* lifecycle;
  RecordingAudio* audio;
  Transition transition;
  bool result = false;
};

void RunTransition(void* context) {
  auto& call = *static_cast<TransitionCall*>(context);
  const uint32_t self = ThreadToken();
  const MicLifecycleOperations ops = RecOps(*call.audio);
  switch (call.transition) {
    case Transition::Create: call.result = call.lifecycle->Create(self, ops); break;
    case Transition::Start: call.result = call.lifecycle->Start(self, ops); break;
    case Transition::Stop: call.result = call.lifecycle->Stop(self, ops, 2000); break;
    case Transition::Destroy: call.result = call.lifecycle->Destroy(self, ops, 2000); break;
  }
}

// Issues one transition from a brand-new game thread, as the engine's job
// system does, and returns {marshalled ok, transition result, caller token}.
struct GameCallResult {
  bool ran;
  bool result;
  uint32_t callerToken;
};

GameCallResult CallFromNewThread(MicOwnerThread& owner, MicCaptureLifecycle& lifecycle,
                                 RecordingAudio& audio, Transition transition) {
  GameCallResult out{false, false, 0};
  std::thread game([&]() {
    out.callerToken = ThreadToken();
    TransitionCall call{&lifecycle, &audio, transition, false};
    out.ran = owner.Run(RunTransition, &call);
    out.result = call.result;
  });
  game.join();
  return out;
}

// Two threads that run one after the other get different tokens even when the
// OS hands the second the id of the first (what the next test relies on).
TEST(MicOwnerThread, SequentialThreadsGetDistinctTokens) {
  uint32_t first = 0;
  uint32_t second = 0;
  std::thread a([&]() { first = ThreadToken(); });
  a.join();
  std::thread b([&]() { second = ThreadToken(); });
  b.join();
  EXPECT_NE(first, second);
  EXPECT_NE(first, 0u);
  EXPECT_NE(second, 0u);
}

TEST(MicOwnerThread, GameCallPatternFromDifferentThreadsSucceedsOnOneOwnerThread) {
  MicOwnerThread owner;
  OwnerGuard guard(owner);
  MicCaptureLifecycle lifecycle;
  RecordingAudio audio;

  const GameCallResult create = CallFromNewThread(owner, lifecycle, audio, Transition::Create);
  const GameCallResult start = CallFromNewThread(owner, lifecycle, audio, Transition::Start);
  ASSERT_NE(create.callerToken, start.callerToken) << "test must use distinct game threads";
  EXPECT_TRUE(create.ran);
  EXPECT_TRUE(create.result);
  EXPECT_TRUE(start.ran);
  EXPECT_TRUE(start.result) << "MicStart from a non-creating game thread was rejected (GH #51)";
  EXPECT_EQ(lifecycle.State(), MicLifecycleState::Running);

  const GameCallResult stop = CallFromNewThread(owner, lifecycle, audio, Transition::Stop);
  EXPECT_TRUE(stop.result) << "MicStop from a non-creating game thread was rejected (GH #51)";
  EXPECT_EQ(lifecycle.State(), MicLifecycleState::Ready);

  const GameCallResult restart = CallFromNewThread(owner, lifecycle, audio, Transition::Start);
  EXPECT_TRUE(restart.result);
  const GameCallResult destroy = CallFromNewThread(owner, lifecycle, audio, Transition::Destroy);
  EXPECT_TRUE(destroy.result) << "MicDestroy from a non-creating game thread was rejected (GH #51)";
  EXPECT_EQ(lifecycle.State(), MicLifecycleState::Closed);

  // Every resource operation — including create and release, the COM
  // initialize/uninitialize pair in production — ran on exactly one thread,
  // and that thread is none of the game callers.
  ASSERT_EQ(audio.opThreads.size(), 1u);
  const uint32_t opThread = *audio.opThreads.begin();
  for (const GameCallResult& call : {create, start, stop, restart, destroy}) {
    EXPECT_NE(call.callerToken, opThread);
  }
  EXPECT_TRUE(owner.Shutdown());
  EXPECT_FALSE(owner.IsRunning());
}

struct Overlap {
  std::atomic<int> inside{0};
  std::atomic<bool> overlapped{false};
  uint64_t count = 0;  // deliberately non-atomic: only safe if serialized
  std::mutex threadsMutex;
  std::set<std::thread::id> threads;
};

void CountSerialized(void* context) {
  auto& o = *static_cast<Overlap*>(context);
  if (o.inside.fetch_add(1) != 0) o.overlapped.store(true);
  ++o.count;
  {
    std::lock_guard<std::mutex> lock(o.threadsMutex);
    o.threads.insert(std::this_thread::get_id());
  }
  o.inside.fetch_sub(1);
}

TEST(MicOwnerThread, ConcurrentCallersAreSerializedOntoOneThread) {
  MicOwnerThread owner;
  Overlap overlap;
  constexpr int kCallers = 8;
  constexpr int kCallsEach = 50;
  std::atomic<int> failures{0};
  std::vector<std::thread> callers;
  for (int i = 0; i < kCallers; ++i) {
    callers.emplace_back([&]() {
      for (int n = 0; n < kCallsEach; ++n) {
        if (!owner.Run(CountSerialized, &overlap)) failures.fetch_add(1);
      }
    });
  }
  for (std::thread& t : callers) t.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_FALSE(overlap.overlapped.load());
  EXPECT_EQ(overlap.count, static_cast<uint64_t>(kCallers * kCallsEach));
  EXPECT_EQ(overlap.threads.size(), 1u);
  EXPECT_TRUE(owner.Shutdown());
}

struct Reentry {
  MicOwnerThread* owner;
  bool innerRan = false;
  bool innerResult = false;
  std::thread::id outerThread;
  std::thread::id innerThread;
};

void InnerTask(void* context) {
  auto& r = *static_cast<Reentry*>(context);
  r.innerRan = true;
  r.innerThread = std::this_thread::get_id();
}

void OuterTask(void* context) {
  auto& r = *static_cast<Reentry*>(context);
  r.outerThread = std::this_thread::get_id();
  r.innerResult = r.owner->Run(InnerTask, &r);
}

TEST(MicOwnerThread, ReentrantRunFromOwnerThreadExecutesInlineWithoutDeadlock) {
  MicOwnerThread owner;
  Reentry reentry{&owner, false, false, std::thread::id{}, std::thread::id{}};
  EXPECT_TRUE(owner.Run(OuterTask, &reentry));
  EXPECT_TRUE(reentry.innerRan);
  EXPECT_TRUE(reentry.innerResult);
  EXPECT_EQ(reentry.innerThread, reentry.outerThread);
  EXPECT_TRUE(owner.Shutdown());
}

void ThrowingTask(void*) { throw std::runtime_error("injected owner-thread task failure"); }
void SetFlag(void* context) { *static_cast<bool*>(context) = true; }

TEST(MicOwnerThread, ThrowingTaskReportsFailureAndOwnerKeepsServing) {
  MicOwnerThread owner;
  EXPECT_FALSE(owner.Run(ThrowingTask, nullptr));
  bool ran = false;
  EXPECT_TRUE(owner.Run(SetFlag, &ran));
  EXPECT_TRUE(ran);
  EXPECT_TRUE(owner.Shutdown());
}

void RecordThread(void* context) { *static_cast<std::thread::id*>(context) = std::this_thread::get_id(); }

struct SelfShutdown {
  MicOwnerThread* owner;
  bool result = true;
};

void ShutdownFromOwner(void* context) {
  auto& s = *static_cast<SelfShutdown*>(context);
  s.result = s.owner->Shutdown();
}

TEST(MicOwnerThread, ShutdownJoinsAndNextRunStartsAFreshThread) {
  MicOwnerThread owner;
  OwnerGuard guard(owner);
  EXPECT_FALSE(owner.IsRunning());
  EXPECT_TRUE(owner.Shutdown()) << "Shutdown with no thread is a no-op success";

  std::thread::id first;
  ASSERT_TRUE(owner.Run(RecordThread, &first));
  EXPECT_TRUE(owner.IsRunning());
  EXPECT_NE(first, std::this_thread::get_id());

  SelfShutdown self{&owner, true};
  ASSERT_TRUE(owner.Run(ShutdownFromOwner, &self));
  EXPECT_FALSE(self.result) << "the owner thread cannot join itself";
  EXPECT_TRUE(owner.IsRunning());

  EXPECT_TRUE(owner.Shutdown());
  EXPECT_FALSE(owner.IsRunning());

  std::thread::id second;
  ASSERT_TRUE(owner.Run(RecordThread, &second));
  EXPECT_TRUE(owner.IsRunning());
  EXPECT_NE(second, std::this_thread::get_id());
  EXPECT_TRUE(owner.Shutdown());
}

bool ReadFlag(void* context) { return *static_cast<bool*>(context); }

TEST(MicOwnerThread, ShutdownWhenStopsOnlyWhenThePredicateHolds) {
  MicOwnerThread owner;
  OwnerGuard guard(owner);
  std::thread::id first;
  ASSERT_TRUE(owner.Run(RecordThread, &first));

  bool providerClosed = false;
  EXPECT_FALSE(owner.ShutdownWhen(ReadFlag, &providerClosed)) << "thread must survive while resources are live";
  EXPECT_TRUE(owner.IsRunning());
  std::thread::id stillFirst;
  ASSERT_TRUE(owner.Run(RecordThread, &stillFirst));
  EXPECT_EQ(stillFirst, first) << "a declined shutdown must not replace the owner thread";

  providerClosed = true;
  EXPECT_TRUE(owner.ShutdownWhen(ReadFlag, &providerClosed));
  EXPECT_FALSE(owner.IsRunning());
}

TEST(MicOwnerThread, NullTaskIsRefusedWithoutStartingAThread) {
  MicOwnerThread owner;
  EXPECT_FALSE(owner.Run(nullptr, nullptr));
  EXPECT_FALSE(owner.IsRunning());
}

}  // namespace
