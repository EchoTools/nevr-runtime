// Host test of ThreadWord (src/quest/plugins/q15t/src/thread_word.h): a value per thread in a pthread key, in place of thread_local.
// Each thread sees only its own value; a new thread starts at 0; before Create the word reads 0 and ignores Set; Create is idempotent.
#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "key_hog.h"
#include "thread_word.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                 \
  do {                                                   \
    if (!(cond)) {                                       \
      ++g_fail;                                          \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                          \
      std::printf("\n");                                 \
    }                                                    \
  } while (0)

static q15t::ThreadWord g_a, g_b, g_never, g_late;

int main() {
  // before Create: reads 0, Set is ignored (no hook can run before the module's install). A Set that went to the key the word holds
  // before it has one would land on key 0, which is a key of someone else when it already exists and this probe's when it does not.
  pthread_key_t probe;
  CHECK(pthread_key_create(&probe, nullptr) == 0, "no pthread key for the probe");
  pthread_setspecific(probe, reinterpret_cast<void*>(0x55));
  void* const key0_before = pthread_getspecific(0);
  g_a.Set(7);
  CHECK(g_a.Get() == 0, "a word that was never created answered %lu", static_cast<unsigned long>(g_a.Get()));
  CHECK(pthread_getspecific(0) == key0_before, "Set on a word that was never created changed key 0");

  CHECK(g_a.Create() && g_b.Create(), "Create failed");
  CHECK(g_a.Create(), "a second Create failed");
  CHECK(g_a.Get() == 0, "a created word starts at %lu, want 0", static_cast<unsigned long>(g_a.Get()));
  g_a.Set(0x1234);
  CHECK(g_a.Get() == 0x1234, "Set then Get on one thread: %lu", static_cast<unsigned long>(g_a.Get()));
  CHECK(g_b.Get() == 0, "two words share a key");
  g_a.Set(0xffffffffffffffffull);
  CHECK(g_a.Get() == 0xffffffffffffffffull, "the full pointer width is kept");
  g_a.Set(0x1234);
  g_a.Create();
  CHECK(g_a.Get() == 0x1234, "a second Create reset this thread's value");

  // another thread starts at 0, and neither thread's Set reaches the other
  std::atomic<std::uintptr_t> seen_at_start{99}, seen_after_own_set{0};
  std::atomic<bool> go_set{false}, own_set_done{false}, release{false};
  std::thread t([&] {
    seen_at_start = g_a.Get();
    while (!go_set) std::this_thread::yield();
    g_a.Set(0x5678);
    seen_after_own_set = g_a.Get();
    own_set_done = true;
    while (!release) std::this_thread::yield();
  });
  while (seen_at_start == 99) std::this_thread::yield();
  CHECK(seen_at_start == 0, "a new thread read %lu, want 0 (it saw this thread's value)", static_cast<unsigned long>(seen_at_start.load()));
  go_set = true;
  while (!own_set_done) std::this_thread::yield();
  CHECK(seen_after_own_set == 0x5678, "the other thread lost its own value (%lu)", static_cast<unsigned long>(seen_after_own_set.load()));
  CHECK(g_a.Get() == 0x1234, "the other thread's Set reached this thread (%lu)", static_cast<unsigned long>(g_a.Get()));
  release = true;
  t.join();

  // many threads at once, each with its own value, each reading it back after the others have written theirs
  constexpr int kThreads = 16;
  std::atomic<int> arrived{0}, bad{0};
  std::vector<std::thread> ts;
  for (int i = 0; i < kThreads; ++i) {
    ts.emplace_back([&, i] {
      g_a.Set(1000 + static_cast<std::uintptr_t>(i));
      arrived.fetch_add(1);
      while (arrived.load() < kThreads) std::this_thread::yield();
      for (int k = 0; k < 1000; ++k) {
        if (g_a.Get() != 1000 + static_cast<std::uintptr_t>(i)) bad.fetch_add(1);
      }
    });
  }
  for (auto& th : ts) th.join();
  CHECK(bad == 0, "%d reads saw another thread's value", bad.load());
  CHECK(g_a.Get() == 0x1234, "the threads' values reached this thread");

  CHECK(g_never.Get() == 0, "an untouched word is not 0");

  // no key left: Create says so, the word stays inert (reads 0, ignores Set), and a later Create, once a key is free, works
  {
    KeyHog hog;
    CHECK(!g_late.Create(), "Create succeeded with no pthread key left");
    g_late.Set(9);
    CHECK(g_late.Get() == 0, "a word whose Create failed answered %lu", static_cast<unsigned long>(g_late.Get()));
  }
  CHECK(g_late.Create(), "Create failed once keys were free again");
  g_late.Set(9);
  CHECK(g_late.Get() == 9, "a word created after a failed Create does not keep a value");
  if (g_fail == 0) std::printf("thread_word_test: all checks pass\n");
  else std::printf("thread_word_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
