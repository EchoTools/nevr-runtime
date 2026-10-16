// #504: the loader notification records pnsrad.dll and pnsradmatchmaking.dll loads; the patches are applied
// by whoever drains the record outside the loader lock. These pin the record/drain contract.

#include <gtest/gtest.h>

#include "runtime/patch/pnsrad_load_queue.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using nevr_pnsrad_load::Classify;
using nevr_pnsrad_load::Module;
using nevr_pnsrad_load::PendingLoads;

std::optional<Module> ClassifyWide(const std::wstring& name) { return Classify(name.data(), name.size()); }

std::vector<std::pair<Module, std::uintptr_t>> DrainAll(PendingLoads& queue) {
  std::vector<std::pair<Module, std::uintptr_t>> out;
  queue.Drain([&](Module module, std::uintptr_t base) { out.emplace_back(module, base); });
  return out;
}

}  // namespace

TEST(PnsradLoadQueue, ClassifiesTheTwoModulesByWholeNameAnyCase) {
  EXPECT_EQ(ClassifyWide(L"pnsrad.dll"), Module::Pnsrad);
  EXPECT_EQ(ClassifyWide(L"PNSRAD.DLL"), Module::Pnsrad);
  EXPECT_EQ(ClassifyWide(L"pnsradmatchmaking.dll"), Module::Matchmaking);
  EXPECT_EQ(ClassifyWide(L"PnsRadMatchmaking.DLL"), Module::Matchmaking);
  EXPECT_FALSE(ClassifyWide(L"pnsrad.dll.bak").has_value());
  EXPECT_FALSE(ClassifyWide(L"pnsrad").has_value());
  EXPECT_FALSE(ClassifyWide(L"pnsradgameserver.dll").has_value());
  EXPECT_FALSE(ClassifyWide(L"xpnsrad.dll").has_value());
  EXPECT_FALSE(Classify(static_cast<const wchar_t*>(nullptr), 10).has_value());
}

// The notification writes nothing: a load is applied only when the record is drained, and only once.
TEST(PnsradLoadQueue, ARecordIsAppliedOnlyWhenDrainedAndOnlyOnce) {
  PendingLoads queue;
  ASSERT_TRUE(queue.Note(Module::Pnsrad, 0x180000000ULL));
  const auto first = DrainAll(queue);
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(first[0].first, Module::Pnsrad);
  EXPECT_EQ(first[0].second, 0x180000000ULL);
  EXPECT_TRUE(DrainAll(queue).empty()) << "a drained record was handed over a second time";
}

// pnsradmatchmaking.dll reloads once per lobby: each load is its own record, so each image is patched.
TEST(PnsradLoadQueue, EachReloadIsItsOwnRecord) {
  PendingLoads queue;
  ASSERT_TRUE(queue.Note(Module::Matchmaking, 0x7ff100000000ULL));
  EXPECT_EQ(DrainAll(queue).size(), 1u);
  ASSERT_TRUE(queue.Note(Module::Matchmaking, 0x7ff200000000ULL));
  ASSERT_TRUE(queue.Note(Module::Matchmaking, 0x7ff300000000ULL));
  const auto reloads = DrainAll(queue);
  ASSERT_EQ(reloads.size(), 2u);
  EXPECT_NE(reloads[0].second, reloads[1].second);
  for (const auto& r : reloads) EXPECT_EQ(r.first, Module::Matchmaking);
}

TEST(PnsradLoadQueue, AFullQueueCountsTheLoadsItCouldNotRecord) {
  PendingLoads queue;
  for (std::size_t i = 0; i < PendingLoads::kCapacity; ++i) {
    ASSERT_TRUE(queue.Note(Module::Matchmaking, 0x1000 * (i + 1)));
  }
  EXPECT_FALSE(queue.Note(Module::Pnsrad, 0xdead0000));
  EXPECT_FALSE(queue.Note(Module::Pnsrad, 0xbeef0000));
  EXPECT_EQ(queue.TakeDropped(), 2u);
  EXPECT_EQ(queue.TakeDropped(), 0u);
  EXPECT_EQ(DrainAll(queue).size(), PendingLoads::kCapacity);
  EXPECT_TRUE(queue.Note(Module::Pnsrad, 0xdead0000)) << "drained slots are reusable";
}

// The frame tick only takes a record a whole tick old, so a LoadLibrary still returning on another thread keeps
// its own record (and its patch lands before that call returns).
TEST(PnsradLoadQueue, TheTickTakesARecordOnlyAfterAWholeTick) {
  PendingLoads queue;
  ASSERT_TRUE(queue.Note(Module::Pnsrad, 0x180000000ULL));
  int applied = 0;
  EXPECT_EQ(queue.DrainNotedBefore(queue.AdvanceEpoch(), [&](Module, std::uintptr_t) { ++applied; }), 0u);
  EXPECT_EQ(applied, 0) << "the tick took a record noted during the tick it just ended";
  EXPECT_EQ(queue.DrainNotedBefore(queue.AdvanceEpoch(), [&](Module, std::uintptr_t) { ++applied; }), 1u);
  EXPECT_EQ(applied, 1);
}

TEST(PnsradLoadQueue, ConcurrentNotesAndDrainsHandEachRecordOverExactlyOnce) {
  PendingLoads queue;
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 2000;
  std::atomic<int> producersDone{0};
  std::vector<std::atomic<int>> seen(kProducers * kPerProducer + 1);
  std::atomic<std::uint64_t> drained{0};

  // Each consumer drains until every producer is done, then once more for what was noted after its last pass.
  auto consume = [&] {
    auto count = [&](Module, std::uintptr_t base) { seen[base].fetch_add(1); };
    while (producersDone.load() < kProducers) drained += queue.Drain(count);
    drained += queue.Drain(count);
  };

  std::vector<std::thread> threads;
  for (int p = 0; p < kProducers; ++p) {
    threads.emplace_back([&, p] {
      for (int i = 1; i <= kPerProducer; ++i) {
        const std::uintptr_t id = static_cast<std::uintptr_t>(p * kPerProducer + i);
        while (!queue.Note(Module::Matchmaking, id)) std::this_thread::yield();
      }
      producersDone.fetch_add(1);
    });
  }
  std::thread consumerA(consume);
  std::thread consumerB(consume);
  for (auto& t : threads) t.join();
  consumerA.join();
  consumerB.join();
  drained += queue.Drain([&](Module, std::uintptr_t base) { seen[base].fetch_add(1); });

  EXPECT_EQ(drained.load(), static_cast<std::uint64_t>(kProducers * kPerProducer));
  for (int id = 1; id <= kProducers * kPerProducer; ++id) {
    ASSERT_EQ(seen[id].load(), 1) << "record " << id;
  }
}
