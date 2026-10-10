// #352: the CDN tint map was freed while the Loadout hook could still be reading it. ReaderGate is
// what the hook holds while it uses the map and what the writer waits on before freeing.

#include "core/reader_gate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
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

  nevr::ReaderGate gate;
  std::vector<Canary> pool(kRepublishes + 1);
  std::atomic<Canary*> slot{&pool[0]};
  std::atomic<bool> stop{false};
  std::atomic<long> violations{0};
  std::atomic<int> ready{0};

  std::vector<std::thread> readers;
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

  for (int i = 1; i <= kRepublishes; i++) {
    Canary* old = slot.load(std::memory_order_relaxed);
    nevr::ReaderGate::Publish(slot, &pool[i]);
    ASSERT_TRUE(gate.WaitIdle(10s));
    old->freed.store(true, std::memory_order_relaxed);  // "delete old"
  }
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
