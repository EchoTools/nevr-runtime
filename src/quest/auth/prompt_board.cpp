#include "quest/auth/prompt_board.h"

#include <pthread.h>

#include <atomic>

namespace nevr::quest_auth::prompt_board {

namespace {

// Sequence lock: even = stable, odd = a writer is inside. The bytes are atomics read and written
// relaxed, with the fences that make a consistent read detectable (the C++ seqlock pattern): the
// writer's release fence after the odd store and release store of the even value, the reader's
// acquire load of the sequence before the bytes and acquire fence after them. A reader that
// overlaps a writer sees a changed or odd sequence and retries; it never returns torn text.
// On arm64 those orderings are real instructions (ldar/stlr/dmb); an x86 host is TSO and cannot
// show a weakened order, so the host test checks torn reads, not the fences.
std::atomic<std::uint64_t> g_sequence{0};
std::atomic<std::uint32_t> g_length{0};  // 0: nothing published
std::atomic<std::uint8_t> g_mode{0};
std::atomic<char> g_text[kCapacity];
std::atomic<std::uint32_t> g_titleLength{0};  // 0: no title
std::atomic<char> g_title[kMaxTitleChars];
pthread_mutex_t g_writer = PTHREAD_MUTEX_INITIALIZER;

inline void CpuRelax() noexcept {
#if defined(__aarch64__)
  __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

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

bool Publish(const char* text, std::size_t len, Mode mode, const char* title) noexcept {
  if (text == nullptr || len == 0 || len > kCapacity) return false;
  for (std::size_t i = 0; i < len; ++i) {
    if (text[i] == '\0') return false;
  }
  std::size_t titleLen = 0;
  if (title != nullptr) {
    while (titleLen <= kMaxTitleChars && title[titleLen] != '\0') ++titleLen;
    if (titleLen == 0 || titleLen > kMaxTitleChars) return false;
  }
  Exclusive([text, len, mode, title, titleLen] {
    // The title is rewritten whole as well: a title of an earlier publish must not stay under a text that has none.
    for (std::size_t i = 0; i < kMaxTitleChars; ++i) g_title[i].store(i < titleLen ? title[i] : '\0', std::memory_order_relaxed);
    g_titleLength.store(static_cast<std::uint32_t>(titleLen), std::memory_order_relaxed);
    // The whole buffer is rewritten, so nothing of a longer earlier text (an older code) stays.
    for (std::size_t i = 0; i < kCapacity; ++i) g_text[i].store(i < len ? text[i] : '\0', std::memory_order_relaxed);
    g_length.store(static_cast<std::uint32_t>(len), std::memory_order_relaxed);
    g_mode.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
  });
  return true;
}

bool Withdraw() noexcept {
  bool had = false;
  Exclusive([&had] {
    had = g_length.load(std::memory_order_relaxed) != 0;
    // The text may hold a device code: overwrite it, not just forget its length (atomic stores are
    // not removed as dead stores).
    for (std::size_t i = 0; i < kCapacity; ++i) g_text[i].store('\0', std::memory_order_relaxed);
    for (std::size_t i = 0; i < kMaxTitleChars; ++i) g_title[i].store('\0', std::memory_order_relaxed);
    g_length.store(0, std::memory_order_relaxed);
    g_titleLength.store(0, std::memory_order_relaxed);
    g_mode.store(0, std::memory_order_relaxed);
  });
  return had;
}

ReadResult Read(char* out, std::size_t cap, Mode* mode, std::uint64_t* version) noexcept {
  if (out == nullptr || cap == 0) return ReadResult::kEmpty;
  out[0] = '\0';
  if (cap < kCapacity + 1) return ReadResult::kEmpty;
  for (int attempt = 0; attempt < kReadAttempts; ++attempt) {
    if (attempt != 0) CpuRelax();
    const std::uint64_t before = g_sequence.load(std::memory_order_acquire);
    if ((before & 1U) != 0) continue;  // a writer is inside
    const std::uint32_t len = g_length.load(std::memory_order_relaxed);
    const std::uint8_t m = g_mode.load(std::memory_order_relaxed);
    if (len > kCapacity) continue;  // cannot happen with a stable sequence; read again
    for (std::uint32_t i = 0; i < len; ++i) out[i] = g_text[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g_sequence.load(std::memory_order_relaxed) != before) continue;
    out[len] = '\0';
    if (version != nullptr) *version = before / 2;
    if (len == 0) return ReadResult::kEmpty;
    if (mode != nullptr) *mode = m == static_cast<std::uint8_t>(Mode::kNotice) ? Mode::kNotice : Mode::kPrompt;
    return ReadResult::kCopied;
  }
  out[0] = '\0';
  return ReadResult::kBusy;
}

bool ReadTitle(char* out, std::size_t cap) noexcept {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  if (cap < kMaxTitleChars + 1) return false;
  for (int attempt = 0; attempt < kReadAttempts; ++attempt) {
    if (attempt != 0) CpuRelax();
    const std::uint64_t before = g_sequence.load(std::memory_order_acquire);
    if ((before & 1U) != 0) continue;  // a writer is inside
    const std::uint32_t len = g_titleLength.load(std::memory_order_relaxed);
    if (len > kMaxTitleChars) continue;  // cannot happen with a stable sequence; read again
    for (std::uint32_t i = 0; i < len; ++i) out[i] = g_title[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g_sequence.load(std::memory_order_relaxed) != before) continue;
    out[len] = '\0';
    return len != 0;
  }
  out[0] = '\0';
  return false;
}

std::uint64_t Version() noexcept { return g_sequence.load(std::memory_order_acquire) / 2; }

std::size_t NonZeroTextBytesForTest() noexcept {
  std::size_t n = 0;
  for (std::size_t i = 0; i < kCapacity; ++i) n += g_text[i].load(std::memory_order_relaxed) != '\0' ? 1 : 0;
  for (std::size_t i = 0; i < kMaxTitleChars; ++i) n += g_title[i].load(std::memory_order_relaxed) != '\0' ? 1 : 0;
  return n;
}

void BeginWriteForTest() noexcept {
  pthread_mutex_lock(&g_writer);
  g_sequence.fetch_add(1, std::memory_order_acq_rel);
}

void EndWriteForTest() noexcept {
  g_sequence.fetch_add(1, std::memory_order_acq_rel);
  pthread_mutex_unlock(&g_writer);
}

}  // namespace nevr::quest_auth::prompt_board
