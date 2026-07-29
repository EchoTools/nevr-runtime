#include "hook_guard.h"

#include <windows.h>

#include <cstring>

#include "core/logging.h"

namespace {

/// Bytes compared per address. A MinHook detour writes a 5-byte JMP rel32 (or a
/// 14-byte absolute JMP when the target is out of rel32 range), so 16 covers both
/// forms plus a couple of following bytes.
constexpr int kSnapshotBytes = 16;

/// Fixed capacity — no heap. Recording happens during boot, some of it under the
/// loader lock, and this is a diagnostic that must never be the reason a boot fails.
constexpr int kMaxGuarded = 64;

struct GuardedSite {
  const void* target;
  const char* name;  // string literal from the call site; not owned
  unsigned char bytes[kSnapshotBytes];
};

GuardedSite g_sites[kMaxGuarded];
volatile LONG g_count = 0;
bool g_overflowed = false;

/// Committed+readable check that does not use IsBadReadPtr (deprecated, and
/// unreliable under Wine).
bool Readable(const void* addr, size_t len) {
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
  const auto start = static_cast<const unsigned char*>(mbi.BaseAddress);
  const auto want_end = static_cast<const unsigned char*>(addr) + len;
  return want_end <= start + mbi.RegionSize;
}

void FormatBytes(const unsigned char* src, int n, char* out, size_t out_cap) {
  size_t used = 0;
  for (int i = 0; i < n && used + 3 < out_cap; i++) {
    const int written = snprintf(out + used, out_cap - used, "%02x", src[i]);
    if (written <= 0) break;
    used += static_cast<size_t>(written);
  }
  out[used < out_cap ? used : out_cap - 1] = '\0';
}

}  // namespace

namespace HookGuard {

void Record(const void* target, const char* name) {
  if (target == nullptr) return;
  if (!Readable(target, kSnapshotBytes)) return;

  const LONG idx = InterlockedIncrement(&g_count) - 1;
  if (idx >= kMaxGuarded) {
    InterlockedDecrement(&g_count);
    if (!g_overflowed) {
      g_overflowed = true;
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] hook guard table full cap=%d — further detours unguarded",
          kMaxGuarded);
    }
    return;
  }

  g_sites[idx].target = target;
  g_sites[idx].name = name != nullptr ? name : "(unnamed)";
  memcpy(g_sites[idx].bytes, target, kSnapshotBytes);
}

int VerifyAll(const char* context) {
  const LONG n = g_count;
  int mismatches = 0;

  for (LONG i = 0; i < n && i < kMaxGuarded; i++) {
    const GuardedSite& s = g_sites[i];
    if (s.target == nullptr) continue;
    if (!Readable(s.target, kSnapshotBytes)) continue;

    unsigned char now[kSnapshotBytes];
    memcpy(now, s.target, kSnapshotBytes);
    if (memcmp(now, s.bytes, kSnapshotBytes) == 0) continue;

    char expected[kSnapshotBytes * 2 + 1];
    char actual[kSnapshotBytes * 2 + 1];
    FormatBytes(s.bytes, kSnapshotBytes, expected, sizeof(expected));
    FormatBytes(now, kSnapshotBytes, actual, sizeof(actual));

    // ERROR, not Warning: an operator MUST act. Two MinHook instances on one
    // address means the second trampolined off our JMP stub, and which of the
    // two hooks actually runs is not determined by either of them.
    Log(EchoVR::LogLevel::Error,
        "[NEVR.PATCH] hook overwritten name=%s va=0x%llX after=%s "
        "expected=%s actual=%s — another module detoured an address gamepatches "
        "already owns (N84)",
        s.name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.target)),
        context != nullptr ? context : "(unknown)", expected, actual);
    mismatches++;
  }

  if (mismatches > 0) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.PATCH] hook guard: %d of %ld guarded address(es) overwritten after=%s",
        mismatches, static_cast<long>(n), context != nullptr ? context : "(unknown)");
  }
  return mismatches;
}

int RecordedCount() { return static_cast<int>(g_count); }

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  InterlockedExchange(&g_count, 0);
  memset(g_sites, 0, sizeof(g_sites));
  g_overflowed = false;
}
#endif

}  // namespace HookGuard
