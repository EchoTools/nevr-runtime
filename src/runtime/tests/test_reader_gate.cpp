// #352: the CDN tint map was freed while the Loadout hook could still be reading it. ReaderGate is
// what the hook holds while it uses the map and what the writer waits on before freeing.

#include "core/reader_gate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// A map that records a read after it was freed (the real one would be heap corruption).
struct Map {
  std::atomic<bool> freed{false};
  std::atomic<int> reads_after_free{0};
  void Read() {
    if (freed) reads_after_free++;
  }
};

}  // namespace

TEST(ReaderGate, WriterDoesNotFreeWhileAReaderIsInsideTheGate) {
  nevr::ReaderGate gate;
  Map map;
  std::atomic<Map*> published{&map};
  std::atomic<bool> inside{false};
  std::atomic<bool> leave{false};

  std::thread reader([&] {
    nevr::ReaderGate::Scope scope(gate);
    Map* m = published.load();  // loaded before the writer published nullptr
    inside = true;
    while (!leave) std::this_thread::sleep_for(1ms);
    if (m) m->Read();  // still in use after the writer started to shut down
  });
  while (!inside) std::this_thread::sleep_for(1ms);

  // Shutdown path: publish nullptr, wait for readers, free.
  published.store(nullptr);
  const bool idle = gate.WaitIdle(50ms);
  if (idle) map.freed = true;
  EXPECT_FALSE(idle) << "WaitIdle returned while a reader was still inside the gate";
  EXPECT_FALSE(map.freed);

  leave = true;
  reader.join();
  EXPECT_EQ(map.reads_after_free.load(), 0);
  EXPECT_TRUE(gate.WaitIdle(1s));
  map.freed = true;
}

TEST(ReaderGate, WaitIdleReturnsOnceTheLastReaderLeaves) {
  nevr::ReaderGate gate;
  std::atomic<bool> inside{false};
  std::thread reader([&] {
    nevr::ReaderGate::Scope scope(gate);
    inside = true;
    std::this_thread::sleep_for(30ms);
  });
  while (!inside) std::this_thread::sleep_for(1ms);
  EXPECT_TRUE(gate.WaitIdle(5s));
  reader.join();
}

TEST(ReaderGate, IdleGateReturnsAtOnce) {
  nevr::ReaderGate gate;
  EXPECT_TRUE(gate.WaitIdle(0ms));
}

// #469: WaitIdle waits for the readers that were inside when it was called, not for a count of zero.
// Here the count is never zero while the writer waits: one thread always holds a Scope and hands over to
// the next one before it lets go of the last. A gate that waits for a zero count reports "not idle" for
// the whole timeout although the reader that was inside at the call left at the first hand-over.
TEST(ReaderGate, WaitIdleIsNotStarvedByReadersThatOverlapEachOther) {
  nevr::ReaderGate gate;
  std::atomic<bool> writer_done{false};
  std::atomic<bool> idle{false};
  std::atomic<bool> writer_started{false};

  auto first = std::make_unique<nevr::ReaderGate::Scope>(gate);
  std::thread writer([&] {
    writer_started = true;
    idle = gate.WaitIdle(2s);
    writer_done = true;
  });
  const auto give_up = std::chrono::steady_clock::now() + 10s;
  while (!writer_done && std::chrono::steady_clock::now() < give_up) {
    auto next = std::make_unique<nevr::ReaderGate::Scope>(gate);  // entered before the previous one leaves
    first = std::move(next);
    EXPECT_GE(gate.Readers(), 1);
  }
  first.reset();
  writer.join();
  EXPECT_TRUE(writer_started.load());
  EXPECT_TRUE(idle.load()) << "WaitIdle did not return although every reader inside at the call had left: "
                              "a stream of overlapping readers starved the writer";
}

// A reader that outlives a timed-out wait must still hold the next wait back: the object it uses was
// published before the second Publish and may be freed only once it has left.
TEST(ReaderGate, ATimedOutWaitIsNotForgottenByTheNextOne) {
  nevr::ReaderGate gate;
  std::atomic<bool> inside{false};
  std::atomic<bool> leave{false};
  std::thread reader([&] {
    nevr::ReaderGate::Scope scope(gate);
    inside = true;
    while (!leave) std::this_thread::sleep_for(1ms);
  });
  while (!inside) std::this_thread::sleep_for(1ms);

  EXPECT_FALSE(gate.WaitIdle(20ms));
  EXPECT_FALSE(gate.WaitIdle(20ms)) << "the reader that the first wait timed out on is still inside";
  EXPECT_FALSE(gate.WaitIdle(20ms));
  EXPECT_EQ(gate.Readers(), 1);

  leave = true;
  reader.join();
  EXPECT_TRUE(gate.WaitIdle(2s));
  EXPECT_EQ(gate.Readers(), 0);
}

// A reader that read the epoch, was overtaken by a whole WaitIdle and only then raised the counter of
// the epoch it read must not stay registered there: the next WaitIdle would wait on the other counter
// and call the gate idle while this reader uses the object the first one published.
TEST(ReaderGate, AReaderOvertakenBetweenReadingTheEpochAndRegisteringIsWaitedFor) {
  nevr::ReaderGate gate;
  struct Obj {
    int v;
  } first{1}, second{2}, third{3};
  std::atomic<Obj*> slot{&first};
  std::atomic<bool> stalled{false};
  std::atomic<bool> resume{false};
  std::atomic<bool> inside{false};
  std::atomic<bool> leave{false};
  std::atomic<int> seen{0};

  std::thread reader([&] {
    bool once = true;
    nevr::ReaderGate::Scope scope(gate, [&] {
      if (!once) return;  // the retry after the epoch moved is not held
      once = false;
      stalled = true;
      while (!resume) std::this_thread::sleep_for(1ms);
    });
    seen = nevr::ReaderGate::Load(slot)->v;
    inside = true;
    while (!leave) std::this_thread::sleep_for(1ms);
  });
  while (!stalled) std::this_thread::sleep_for(1ms);

  nevr::ReaderGate::Publish(slot, &second);
  EXPECT_TRUE(gate.WaitIdle(1s)) << "nobody is registered yet";
  resume = true;
  while (!inside) std::this_thread::sleep_for(1ms);
  EXPECT_EQ(seen.load(), 2) << "a reader that registers after the writer's wait loads the replacement";

  nevr::ReaderGate::Publish(slot, &third);
  EXPECT_FALSE(gate.WaitIdle(50ms)) << "WaitIdle called the gate idle while a reader is inside it";
  EXPECT_EQ(gate.Readers(), 1);

  leave = true;
  reader.join();
  EXPECT_TRUE(gate.WaitIdle(1s));
}

// A reader that enters after the writer started waiting is not waited for.
TEST(ReaderGate, ALaterReaderDoesNotHoldTheWriterBack) {
  nevr::ReaderGate gate;
  std::atomic<bool> inside{false};
  std::atomic<bool> leave{false};
  std::thread late;
  std::thread early([&] {
    nevr::ReaderGate::Scope scope(gate);
    inside = true;
    while (!leave) std::this_thread::sleep_for(1ms);
  });
  while (!inside) std::this_thread::sleep_for(1ms);

  std::atomic<bool> idle{false};
  std::atomic<bool> writer_done{false};
  std::thread writer([&] {
    idle = gate.WaitIdle(5s);
    writer_done = true;
  });
  // Readers keep arriving and leaving after the early one; none of them is what the writer waits for.
  std::atomic<bool> stop_late{false};
  late = std::thread([&] {
    while (!stop_late) {
      nevr::ReaderGate::Scope scope(gate);
      std::this_thread::yield();
    }
  });
  std::this_thread::sleep_for(20ms);
  leave = true;
  early.join();
  const auto give_up = std::chrono::steady_clock::now() + 10s;
  while (!writer_done && std::chrono::steady_clock::now() < give_up) std::this_thread::sleep_for(1ms);
  stop_late = true;
  late.join();
  writer.join();
  EXPECT_TRUE(idle.load()) << "the writer waited for readers that entered after it started to wait";
}

// The publish-vs-enter race: readers hammer enter / load / use / exit while the writer republishes
// and "frees" the old object. A freed object is only flagged (never returned to the allocator) so a
// violation is counted instead of corrupting the test process. The handshake is a store/load pattern
// on x86 as well as elsewhere: with the pointer store and the counter operations weaker than seq_cst
// the writer can read the counter as 0 while a reader goes on to load the old pointer.
namespace {
struct Canary {
  std::atomic<bool> freed{false};
};
}  // namespace

TEST(ReaderGate, RepublishNeverFreesAnObjectAReaderIsStillUsing) {
  constexpr int kReaders = 6;
  constexpr int kRepublishes = 30000;
  // The race needs pressure, not a fixed count: the run ends at kRepublishes or at kBudget, whichever
  // comes first, because on a machine with fewer cores than readers each republish waits for the
  // descheduled readers inside the gate to run again (a 4 vCPU runner: ~1.6 ms each). It fails by name
  // below kFloor, which only a wedged writer misses; that a stream of readers cannot starve the writer
  // is pinned by WaitIdleIsNotStarvedByReadersThatOverlapEachOther (#469).
  constexpr auto kBudget = 15s;
  constexpr int kFloor = 200;

  nevr::ReaderGate gate;
  std::vector<Canary> pool(kRepublishes + 1);
  std::atomic<Canary*> slot{&pool[0]};
  std::atomic<bool> stop{false};
  std::atomic<long> violations{0};
  std::atomic<int> ready{0};

  std::vector<std::thread> readers;
  // The readers are joined on every way out of the test, including a failed ASSERT.
  struct Joiner {
    std::atomic<bool>& stop;
    std::vector<std::thread>& threads;
    ~Joiner() {
      stop = true;
      for (auto& t : threads) {
        if (t.joinable()) t.join();
      }
    }
  } joiner{stop, readers};

  for (int r = 0; r < kReaders; r++) {
    readers.emplace_back([&] {
      ready++;
      while (!stop.load(std::memory_order_relaxed)) {
        {
          nevr::ReaderGate::Scope scope(gate);
          Canary* c = nevr::ReaderGate::Load(slot);
          if (c != nullptr) {
            // Use it for a moment, as the hook does between the load and the last dereference.
            for (int spin = 0; spin < 4; spin++) {
              if (c->freed.load(std::memory_order_relaxed)) violations++;
            }
          }
        }
      }
    });
  }
  while (ready < kReaders) std::this_thread::yield();

  const auto started = std::chrono::steady_clock::now();
  int done = 0;
  for (int i = 1; i <= kRepublishes; i++) {
    Canary* old = slot.load(std::memory_order_relaxed);
    nevr::ReaderGate::Publish(slot, &pool[i]);
    ASSERT_TRUE(gate.WaitIdle(10s)) << "WaitIdle timed out at republish " << i << " with "
                                    << gate.Readers() << " readers inside";
    old->freed.store(true, std::memory_order_relaxed);  // "delete old"
    done = i;
    if (std::chrono::steady_clock::now() - started > kBudget) break;
  }
  ASSERT_GE(done, kFloor) << "only " << done << " republishes in " << kBudget.count() << " s with "
                          << kReaders << " readers: the writer is starved by the reader stream (#469)";
  stop = true;
  for (auto& t : readers) t.join();

  EXPECT_EQ(violations.load(), 0) << "an object was freed while a reader was inside the gate using it";
}

TEST(ReaderGate, ReadersCountsScopes) {
  nevr::ReaderGate gate;
  EXPECT_EQ(gate.Readers(), 0);
  {
    nevr::ReaderGate::Scope a(gate);
    nevr::ReaderGate::Scope b(gate);
    EXPECT_EQ(gate.Readers(), 2);
  }
  EXPECT_EQ(gate.Readers(), 0);
}
