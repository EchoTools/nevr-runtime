// Host test for the cargo resolver pins (src/core/cargo_resolver.*; docs/adr/0045-cdn-texture-delivery.md D3 "A texture's pair"): the
// shadow that answers a texture's index 0 answers its index 1, on that thread, however many sets are published between the two, and a
// thread that saw no cargo at index 0 sees none at index 1. Distinct threads have distinct pins, and a full pin table degrades to the
// published set without a crash.
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
constexpr std::uint64_t kThreadA = 1111, kThreadB = 2222;

void NothingPublishedSelectsNothing() {
  Resolver r;
  QCHECK(r.Select(kThreadA, kPoster, 0) == nullptr);
  QCHECK(r.Select(kThreadA, kPoster, 1) == nullptr);
  QCHECK(r.published() == nullptr);
}

void IndexZeroSelectsThePublishedShadow() {
  Resolver r;
  const auto s = OneShadow(5);
  r.Publish(s.get());
  QCHECK(r.Select(kThreadA, kPoster, 0) == s.get());
  QCHECK(r.Select(kThreadA, kPoster, 1) == s.get());
}

void ASetPublishedBetweenTheTwoReadsDoesNotSplitThePair() {
  Resolver r;
  const auto first = OneShadow(5), second = OneShadow(6);
  r.Publish(first.get());
  QCHECK(r.Select(kThreadA, kPoster, 0) == first.get());
  r.Publish(second.get());                                    // mounted between the descriptor and the pixels
  QCHECK(r.Select(kThreadA, kPoster, 1) == first.get());      // the pair stays whole
  QCHECK(r.Select(kThreadA, kPoster, 0) == second.get());     // the next load takes the new set
  QCHECK(r.Select(kThreadA, kPoster, 1) == second.get());
}

void AThreadThatSawNoCargoAtIndexZeroSeesNoneAtIndexOne() {
  Resolver r;
  const auto s = OneShadow(5);
  QCHECK(r.Select(kThreadA, kPoster, 0) == nullptr);
  r.Publish(s.get());                                         // mounted after the descriptor was read from stock
  QCHECK(r.Select(kThreadA, kPoster, 1) == nullptr);          // the pixels come from stock too
  QCHECK(r.Select(kThreadA, kPoster, 0) == s.get());
}

void EachThreadHasItsOwnPin() {
  Resolver r;
  const auto first = OneShadow(5), second = OneShadow(6);
  r.Publish(first.get());
  QCHECK(r.Select(kThreadA, kPoster, 0) == first.get());
  r.Publish(second.get());
  QCHECK(r.Select(kThreadB, kPoster, 0) == second.get());
  QCHECK(r.Select(kThreadA, kPoster, 1) == first.get());
  QCHECK(r.Select(kThreadB, kPoster, 1) == second.get());
  QCHECK(r.pins_in_use() == 2);
}

void APinIsForOneNameAndTheNextNameReplacesIt() {
  Resolver r;
  const auto first = OneShadow(5), second = OneShadow(6);
  r.Publish(first.get());
  r.Select(kThreadA, kPoster, 0);
  r.Publish(second.get());
  QCHECK(r.Select(kThreadA, kPoster + 1, 1) == second.get());   // index 1 of a name never read at index 0: the published set
  r.Select(kThreadA, 77, 0);                                    // another name's index 0 replaces the pin
  QCHECK(r.Select(kThreadA, kPoster, 1) == second.get());
}

void AFullPinTableFallsBackToThePublishedSetWithoutCrashing() {
  Resolver r;
  const auto s = OneShadow(5);
  r.Publish(s.get());
  for (std::uint64_t t = 1; t <= Resolver::kPinSlots * 4; ++t) {
    const ns::Shadow* const got = r.Select(t, kPoster, 0);
    QCHECK(got == s.get());
    QCHECK(r.Select(t, kPoster, 1) == s.get());
  }
  QCHECK(r.pins_in_use() <= Resolver::kPinSlots);
}

void ThreadKeyZeroIsAUsableThread() {
  Resolver r;
  const auto first = OneShadow(5), second = OneShadow(6);
  r.Publish(first.get());
  r.Select(0, kPoster, 0);
  r.Publish(second.get());
  QCHECK(r.Select(0, kPoster, 1) == first.get());
  QCHECK(r.pins_in_use() == 1);  // thread key 0 is stored as a used slot, not as a free one
}

void ManyThreadsPublishingAndReadingNeverSplitAPair() {
  Resolver r;
  const auto a = OneShadow(1), b = OneShadow(2);
  r.Publish(a.get());
  std::atomic<bool> stop{false};
  std::atomic<int> split{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 6; ++t) {
    readers.emplace_back([&r, &stop, &split, t] {
      const std::uint64_t key = 5000 + static_cast<std::uint64_t>(t);
      for (int i = 0; i < 20000 && !stop.load(); ++i) {
        const ns::Shadow* const zero = r.Select(key, kPoster, 0);
        const ns::Shadow* const one = r.Select(key, kPoster, 1);
        if (zero != one) split.fetch_add(1);
      }
    });
  }
  std::thread publisher([&r, &a, &b, &stop] {
    for (int i = 0; i < 5000 && !stop.load(); ++i) r.Publish(i % 2 == 0 ? a.get() : b.get());
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
  AFullPinTableFallsBackToThePublishedSetWithoutCrashing();
  ThreadKeyZeroIsAUsableThread();
  ManyThreadsPublishingAndReadingNeverSplitAPair();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cargo_resolver_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cargo_resolver_test: all checks passed\n");
  return 0;
}
