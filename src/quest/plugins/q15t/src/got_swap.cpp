#include "got_swap.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace q15t {
namespace {

std::atomic_flag g_lock = ATOMIC_FLAG_INIT;

struct Lock {
  Lock() {
    while (g_lock.test_and_set(std::memory_order_acquire)) {
    }
  }
  ~Lock() { g_lock.clear(std::memory_order_release); }
};

}  // namespace

const char* StatusName(SwapStatus status) {
  switch (status) {
    case SwapStatus::kOk: return "ok";
    case SwapStatus::kNullSlot: return "null_slot";
    case SwapStatus::kNotAligned: return "not_aligned";
    case SwapStatus::kUnmapped: return "unmapped";
    case SwapStatus::kExecutablePage: return "executable_page";
    case SwapStatus::kUnexpectedOriginal: return "unexpected_original";
    case SwapStatus::kProtectFailed: return "protect_failed";
    case SwapStatus::kRestoreFailed: return "restore_failed";
    case SwapStatus::kMapsUnreadable: return "maps_unreadable";
  }
  return "unknown";
}

bool FindMapping(std::uintptr_t addr, Mapping* out) {
  FILE* f = std::fopen("/proc/self/maps", "re");
  if (f == nullptr) return false;
  char line[512];
  bool found = false;
  while (std::fgets(line, sizeof line, f) != nullptr) {
    unsigned long long lo = 0, hi = 0;
    char perms[8] = {0};
    int consumed = 0;
    if (std::sscanf(line, "%llx-%llx %7s %*s %*s %*s %n", &lo, &hi, perms, &consumed) < 3) continue;
    if (addr < lo || addr >= hi) continue;
    out->start = static_cast<std::uintptr_t>(lo);
    out->end = static_cast<std::uintptr_t>(hi);
    std::memcpy(out->perms, perms, 4);
    out->perms[4] = 0;
    out->path[0] = 0;
    if (consumed > 0) {
      const char* p = line + consumed;
      std::size_t n = std::strlen(p);
      while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == ' ')) --n;
      if (n >= sizeof out->path) n = sizeof out->path - 1;
      std::memcpy(out->path, p, n);
      out->path[n] = 0;
    }
    found = true;
    break;
  }
  std::fclose(f);
  return found;
}

SwapResult SwapWord(std::uintptr_t slot, std::uintptr_t expected, std::uintptr_t replacement) {
  SwapResult r;
  if (slot == 0 || expected == 0) return r;  // kNullSlot
  if (slot % sizeof(std::uintptr_t) != 0) {
    r.status = SwapStatus::kNotAligned;
    return r;
  }
  Lock lock;
  Mapping m;
  if (!FindMapping(slot, &m)) {
    r.status = SwapStatus::kUnmapped;
    return r;
  }
  if (m.perms[2] == 'x') {
    r.status = SwapStatus::kExecutablePage;
    return r;
  }
  if (m.perms[0] != 'r') {
    r.status = SwapStatus::kUnmapped;  // an unreadable page cannot hold a word we can compare
    return r;
  }
  auto* word = reinterpret_cast<std::atomic<std::uintptr_t>*>(slot);
  r.original = word->load(std::memory_order_acquire);
  if (r.original != expected) {
    r.status = SwapStatus::kUnexpectedOriginal;
    return r;
  }
  const bool was_writable = m.perms[1] == 'w';
  const std::uintptr_t page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
  const std::uintptr_t base = slot & ~(page - 1);
  if (!was_writable && mprotect(reinterpret_cast<void*>(base), page, PROT_READ | PROT_WRITE) != 0) {
    r.err = errno;
    r.status = SwapStatus::kProtectFailed;
    return r;
  }
  std::uintptr_t want = expected;
  const bool swapped = word->compare_exchange_strong(want, replacement, std::memory_order_acq_rel);
  if (!was_writable && mprotect(reinterpret_cast<void*>(base), page, PROT_READ) != 0) {
    r.err = errno;
    r.status = SwapStatus::kRestoreFailed;
    return r;
  }
  r.status = swapped ? SwapStatus::kOk : SwapStatus::kUnexpectedOriginal;
  if (!swapped) r.original = want;
  return r;
}

}  // namespace q15t
