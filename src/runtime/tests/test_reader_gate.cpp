// #352: the CDN tint map was freed while the Loadout hook could still be reading it. ReaderGate is
// what the hook holds while it uses the map and what the writer waits on before freeing.

#include "core/reader_gate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

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
