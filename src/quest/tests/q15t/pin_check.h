// Host-test helper for a module's code-word pins (src/quest/plugins/q15t/src/word_pins.h): a fake text mapping carrying the pinned words at base + va, and
// the checks every module's pin table must pass. Used by the module tests; no engine, real SafeRead.
#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "word_pins.h"

namespace pincheck {

struct Text {
  std::uint8_t* mem = nullptr;
  std::size_t len = 0;
  std::uintptr_t base() const { return reinterpret_cast<std::uintptr_t>(mem); }
  std::uint32_t* word(std::uintptr_t va) const { return reinterpret_cast<std::uint32_t*>(mem + va); }
};

inline Text Make(const q15t::WordPin* pins, std::size_t n) {
  std::uintptr_t top = 0;
  for (std::size_t i = 0; i < n; ++i) top = pins[i].va > top ? pins[i].va : top;
  Text t;
  t.len = (top + 4 + 0xfff) & ~static_cast<std::uintptr_t>(0xfff);
  void* m = mmap(nullptr, t.len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (m == MAP_FAILED) return t;
  t.mem = static_cast<std::uint8_t*>(m);
  for (std::size_t i = 0; i < n; ++i) *t.word(pins[i].va) = pins[i].want;
  return t;
}
inline void Free(Text* t) {
  if (t->mem != nullptr) munmap(t->mem, t->len);
  t->mem = nullptr;
}

// FNV-1a over every (va, want) of the table: pins the table's content in the test, so a changed word is a test failure.
inline std::uint64_t Fingerprint(const q15t::WordPin* pins, std::size_t n) {
  std::uint64_t h = 1469598103934665603ull;
  auto mix = [&h](std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
      h ^= (v >> (8 * i)) & 0xff;
      h *= 1099511628211ull;
    }
  };
  for (std::size_t i = 0; i < n; ++i) {
    mix(pins[i].va, 8);
    mix(pins[i].want, 4);
  }
  return h;
}

// match(base) -> bool; count(needle, level) -> number of captured log lines containing needle at that level (-1: any); clear().
// Returns the number of failed checks (each printed).
template <class Match, class Count, class Clear>
int Run(const char* mod, const q15t::WordPin* pins, std::size_t n, std::size_t want_n, std::uint64_t golden, Match match, Count count,
        Clear clear) {
  int fail = 0;
  auto bad = [&](const char* what, std::size_t i) {
    ++fail;
    std::printf("FAIL pin_check %s: %s (pin %zu)\n", mod, what, i);
  };
  if (n != want_n) bad("pin count changed", n);
  if (Fingerprint(pins, n) != golden) {
    ++fail;
    std::printf("FAIL pin_check %s: table fingerprint 0x%016llx, expected 0x%016llx\n", mod,
                static_cast<unsigned long long>(Fingerprint(pins, n)), static_cast<unsigned long long>(golden));
  }
  Text t = Make(pins, n);
  if (t.mem == nullptr) {
    bad("no fake text", 0);
    return fail;
  }
  clear();
  if (!match(t.base())) bad("matching text refused", 0);
  if (count("code word pin mismatch", -1) != 0) bad("matching text logged a mismatch", 0);
  for (std::size_t i = 0; i < n; ++i) {
    clear();
    std::uint32_t* w = t.word(pins[i].va);
    const std::uint32_t good = *w;
    const std::uint32_t flipped = good ^ 0x00010000u;
    *w = flipped;
    if (match(t.base())) bad("a changed word was accepted", i);
    char need[96];
    std::snprintf(need, sizeof need, "pin=0x%llx found=0x%08x expected=0x%08x", static_cast<unsigned long long>(pins[i].va), flipped,
                  good);
    if (count(need, 3) != 1) bad("the mismatch did not name pin / found / expected at ERROR", i);
    if (count("code word pin mismatch", 3) != 1) bad("not exactly one ERROR line", i);
    *w = good;
  }
  clear();
  if (!match(t.base())) bad("restored text refused", 0);
  const std::uintptr_t base = t.base();
  Free(&t);  // unmapped: every read fails
  clear();
  if (match(base)) bad("unreadable text accepted", 0);
  char need[64];
  std::snprintf(need, sizeof need, "pin=0x%llx found=unreadable", static_cast<unsigned long long>(pins[0].va));
  if (count(need, 3) != 1) bad("unreadable pin not named at ERROR", 0);
  return fail;
}

}  // namespace pincheck
