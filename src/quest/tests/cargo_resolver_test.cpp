// Host test for the cargo resolver pins (src/core/cargo_resolver.*; docs/adr/0045-cdn-texture-delivery.md D3 "A texture's pair"): the
// shadow that answers a texture's index 0 answers its index 1, on that thread, however many sets are published between the two, and a
// thread that saw no cargo at index 0 sees none at index 1. Distinct threads have distinct pins, kept in thread-local storage like the
// game's own per-thread state, so no number of threads can fill a table.
//
// Built and run by `just test-quest-data`.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "core/cargo_resolver.h"
#include "quest/tests/test_check.h"

namespace {

namespace ns = nevr_shadow;
using nevr_cargo::Resolver;

std::unique_ptr<ns::Shadow> OneShadow(std::int64_t handle) {
  ns::Input in;
  in.handles = {handle};
  in.chunks = {{0, 0, 10, 20}};
  in.records = {{0x489bb35d53ca50e9ULL, 0x3fa6d25bc24a7b01ULL, 0, 0, 20, 16, 0}};
  std::unique_ptr<ns::Shadow> s;
  QCHECK(ns::Shadow::Build(in, &s) == ns::Status::kOk);
  return s;
}

constexpr std::uint64_t kPoster = 0x3fa6d25bc24a7b01ULL;
constexpr std::uint64_t kOtherName = 77;
constexpr int kManyThreads = 2000;
constexpr int kReaders = 6;
constexpr int kReaderRounds = 20000;
constexpr int kPublishRounds = 5000;

void NothingPublishedSelectsNothing() {
  Resolver r;
  QCHECK(r.Select(kPoster, 0) == nullptr);
  QCHECK(r.Select(kPoster, 1) == nullptr);
  QCHECK(r.published() == nullptr);
}

void IndexZeroSelectsThePublishedShadow() {
  Resolver r;
  const auto s = OneShadow(5);
  r.Publish(s.get());
  QCHECK(r.Select(kPoster, 0) == s.get());
  QCHECK(r.Select(kPoster, 1) == s.get());
}

void ASetPublishedBetweenTheTwoReadsDoesNotSplitThePair() {
  Resolver r;
  const auto first = OneShadow(5), second = OneShadow(6);
  r.Publish(first.get());
  QCHECK(r.Select(kPoster, 0) == first.get());
  r.Publish(second.get());                              // mounted between the descriptor and the pixels
  QCHECK(r.Select(kPoster, 1) == first.get());          // the pair stays whole
  QCHECK(r.Select(kPoster, 0) == second.get());         // the next load takes the new set
  QCHECK(r.Select(kPoster, 1) == second.get());
}

void AThreadThatSawNoCargoAtIndexZeroSeesNoneAtIndexOne() {
  Resolver r;
  const auto s = OneShadow(5);
  QCHECK(r.Select(kPoster, 0) == nullptr);
  r.Publish(s.get());                                   // mounted after the descriptor was read from stock
  QCHECK(r.Select(kPoster, 1) == nullptr);              // the pixels come from stock too
  QCHECK(r.Select(kPoster, 0) == s.get());
}

void EachThreadHasItsOwnPin() {
  Resolver r;
  const auto first = OneShadow(5), second = OneShadow(6);
  r.Publish(first.get());
  QCHECK(r.Select(kPoster, 0) == first.get());          // this thread reads index 0 of the poster
  r.Publish(second.get());
  const ns::Shadow* other0 = nullptr;
  const ns::Shadow* other1 = nullptr;
  std::thread other([&] {                               // another thread reads the same name after the publish
    other0 = r.Select(kPoster, 0);
    other1 = r.Select(kPoster, 1);
  });
  other.join();
  QCHECK(other0 == second.get() && other1 == second.get());
  QCHECK(r.Select(kPoster, 1) == first.get());          // this thread's pin is untouched by the other's
}

void APinIsForOneNameAndTheNextNameReplacesIt() {
  Resolver r;
  const auto first = OneShadow(5), second = OneShadow(6);
  r.Publish(first.get());
  r.Select(kPoster, 0);
  r.Publish(second.get());
  QCHECK(r.Select(kPoster + 1, 1) == second.get());     // index 1 of a name never read at index 0: the published set
  r.Select(kOtherName, 0);                              // another name's index 0 replaces the pin
  QCHECK(r.Select(kPoster, 1) == second.get());
}

void ThePinOfOneResolverIsNoPinOfAnother() {
  Resolver a;
  Resolver b;
  const auto first = OneShadow(5), second = OneShadow(6);
  a.Publish(first.get());
  b.Publish(second.get());
  QCHECK(a.Select(kPoster, 0) == first.get());
  QCHECK(b.Select(kPoster, 1) == second.get());         // b has no pin of its own for this name: the pin of a is not its pin
}

void AResetForgetsThePublishedSetAndEveryPin() {
  Resolver r;
  const auto s = OneShadow(5);
  r.Publish(s.get());
  QCHECK(r.Select(kPoster, 0) == s.get());
  r.Reset();
  QCHECK(r.published() == nullptr);
  QCHECK(r.Select(kPoster, 1) == nullptr);              // the pin of the earlier epoch is gone
}

// The game keeps per-thread state in thread-local storage (FillResourceFileRequest echovr.exe 0x140fa1fe0: LookupSlot 0x140fa2155, TlsGetValue
// through CPool::GetOrCreate 0x140fa2171, AllocState 0x140fa218d), not in a table of a fixed number of slots; neither does the pin. Two thousand threads alive at once,
// more than any table could have held, each keep the shadow that answered their own index 0.
void MoreThreadsThanAnyTableEachKeepTheirOwnPin() {
  Resolver r;
  const auto a = OneShadow(1), b = OneShadow(2);
  r.Publish(a.get());
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kManyThreads; ++t) {
    threads.emplace_back([&] {
      r.Select(kPoster, 0);
      ready.fetch_add(1);
      while (!go.load()) std::this_thread::yield();
      if (r.Select(kPoster, 1) != a.get()) wrong.fetch_add(1);
    });
  }
  while (ready.load() < kManyThreads) std::this_thread::yield();
  r.Publish(b.get());  // mounted while every thread is between its two reads
  go.store(true);
  for (std::thread& th : threads) th.join();
  QCHECK(wrong.load() == 0);
}

void ManyThreadsPublishingAndReadingNeverSplitAPair() {
  Resolver r;
  const auto a = OneShadow(1), b = OneShadow(2);
  r.Publish(a.get());
  std::atomic<bool> stop{false};
  std::atomic<int> split{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < kReaders; ++t) {
    readers.emplace_back([&r, &stop, &split] {
      for (int i = 0; i < kReaderRounds && !stop.load(); ++i) {
        const ns::Shadow* const zero = r.Select(kPoster, 0);
        const ns::Shadow* const one = r.Select(kPoster, 1);
        if (zero != one) split.fetch_add(1);
      }
    });
  }
  std::thread publisher([&r, &a, &b, &stop] {
    for (int i = 0; i < kPublishRounds && !stop.load(); ++i) r.Publish(i % 2 == 0 ? a.get() : b.get());
  });
  publisher.join();
  stop = true;
  for (std::thread& th : readers) th.join();
  QCHECK(split.load() == 0);
}

}  // namespace

int main() {
  NothingPublishedSelectsNothing();
  IndexZeroSelectsThePublishedShadow();
  ASetPublishedBetweenTheTwoReadsDoesNotSplitThePair();
  AThreadThatSawNoCargoAtIndexZeroSeesNoneAtIndexOne();
  EachThreadHasItsOwnPin();
  APinIsForOneNameAndTheNextNameReplacesIt();
  ThePinOfOneResolverIsNoPinOfAnother();
  AResetForgetsThePublishedSetAndEveryPin();
  MoreThreadsThanAnyTableEachKeepTheirOwnPin();
  ManyThreadsPublishingAndReadingNeverSplitAPair();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cargo_resolver_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cargo_resolver_test: all checks passed\n");
  return 0;
}
