#include "quest/auth/prompt_board.h"

#include <pthread.h>

#include <atomic>

namespace nevr::quest_auth::prompt_board {

namespace {

// Sequence lock: even = stable, odd = a writer is inside. The bytes are atomics read and written
// relaxed, with the fences that make a consistent read detectable (the C++ seqlock pattern), so
// a reader that overlaps a writer sees a changed sequence and retries; it never reads torn text
// it then uses.
std::atomic<std::uint64_t> g_sequence{0};
std::atomic<std::uint32_t> g_length{0};  // 0: nothing published
std::atomic<char> g_text[kCapacity];
pthread_mutex_t g_writer = PTHREAD_MUTEX_INITIALIZER;

constexpr int kReadAttempts = 4;

// Runs `write` between the two sequence bumps, under the writer mutex.
template <typename Write>
void Exclusive(Write write) noexcept {
  pthread_mutex_lock(&g_writer);
  const std::uint64_t s = g_sequence.load(std::memory_order_relaxed);
  g_sequence.store(s + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  write();
  g_sequence.store(s + 2, std::memory_order_release);
  pthread_mutex_unlock(&g_writer);
}

}  // namespace

bool Publish(const char* text, std::size_t len) noexcept {
  if (text == nullptr || len == 0 || len > kCapacity) return false;
  for (std::size_t i = 0; i < len; ++i) {
    if (text[i] == '\0') return false;
  }
  Exclusive([text, len] {
    for (std::size_t i = 0; i < len; ++i) g_text[i].store(text[i], std::memory_order_relaxed);
    g_length.store(static_cast<std::uint32_t>(len), std::memory_order_relaxed);
  });
  return true;
}

bool Withdraw() noexcept {
  bool had = false;
  Exclusive([&had] {
    had = g_length.load(std::memory_order_relaxed) != 0;
    g_length.store(0, std::memory_order_relaxed);
  });
  return had;
}

bool Copy(char* out, std::size_t cap) noexcept {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  if (cap < kCapacity + 1) return false;
  for (int attempt = 0; attempt < kReadAttempts; ++attempt) {
    const std::uint64_t before = g_sequence.load(std::memory_order_acquire);
    if ((before & 1U) != 0) continue;  // a writer is inside
    const std::uint32_t len = g_length.load(std::memory_order_relaxed);
    if (len > kCapacity) continue;  // cannot happen with a stable sequence; read again
    for (std::uint32_t i = 0; i < len; ++i) out[i] = g_text[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g_sequence.load(std::memory_order_relaxed) != before) continue;
    out[len] = '\0';
    return len != 0;
  }
  out[0] = '\0';
  return false;
}

std::uint64_t Version() noexcept { return g_sequence.load(std::memory_order_acquire) / 2; }

}  // namespace nevr::quest_auth::prompt_board
