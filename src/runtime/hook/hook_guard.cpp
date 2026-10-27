#include "runtime/hook/hook_guard.h"

#include <winsock2.h>
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
///
/// The bound is found, not set. Counting the install sites in src/ that reach Record() (every
/// PatchDetour/InstallBootDetour call, each row of the binary_bug_fixes hook table, and the raw
/// MinHook installs that call Record directly) gives 63 for a build with every optional hook
/// present: crash_recovery 9, mode_patches 8, binary_bug_fixes 7, party_invite_gate 7,
/// initialize 8 (6 InstallBootDetour + CSysDLL_GetSymbol + CSysDLL_Load), headless_graphics 4,
/// social_facade 4, dll_load_hook 4, coop_ai_trace 3, pnsrad_enabler 2, early_quit_lockout 2, menu_version_label 1,
/// and one each in xpid_patch, return_to_lobby, resource_override, builtin_filter. The
/// capacity is twice that counted 63 sites, so a new hook has room. If the table fills,
/// Record() counts the drop (never silent): VerifyAll() reports it at ERROR and
/// DroppedCount() exposes it.
constexpr int kMaxGuarded = 128;

struct GuardedSite {
  const void* target;
  const char* name;  // string literal from the call site; not owned
  unsigned char bytes[kSnapshotBytes];
  volatile LONG published;  // release-published only after all fields are filled
  volatile LONG reported;  // true once this site's overwrite ERROR has been logged —
                  // a collision that stays overwritten (nothing un-overwrites
                  // one) must not re-log the identical ERROR every ~30s
                  // periodic check for the rest of the process lifetime.
};

GuardedSite g_sites[kMaxGuarded];
volatile LONG g_reserved = 0;
volatile LONG g_recorded = 0;
volatile LONG g_dropped = 0;           // Record() calls that found the table full
volatile LONG g_dropReported = 0;       // g_dropped value VerifyAll() last reported
volatile LONG g_firstDropClaimed = 0;   // one writer fills the two fields below
volatile LONG g_firstDropPublished = 0;
const void* g_firstDropTarget = nullptr;
const char* g_firstDropName = nullptr;

#ifdef NEVR_TEST_HOOKS
void (*g_beforePublishForTest)() = nullptr;
#endif

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

namespace nevr_hook_guard {

namespace {

// `bytes` is the owner's record of what it wrote, or null to snapshot what is at `target` now.
void RecordImpl(const void* target, const char* name, const unsigned char* bytes) {
  if (target == nullptr) return;
  if (!Readable(target, kSnapshotBytes)) return;

  LONG idx = 0;
  for (;;) {
    idx = InterlockedCompareExchange(&g_reserved, 0, 0);
    if (idx >= kMaxGuarded) {
      // No Log() here, one line per dropped hook would repeat itself: the drop is counted
      // and VerifyAll() (tick, plugin loader: never under the lock) reports it.
      if (InterlockedCompareExchange(&g_firstDropClaimed, 1, 0) == 0) {
        g_firstDropTarget = target;
        g_firstDropName = name != nullptr ? name : "(unnamed)";
        InterlockedExchange(&g_firstDropPublished, 1);
      }
      InterlockedIncrement(&g_dropped);
      return;
    }
    if (InterlockedCompareExchange(&g_reserved, idx + 1, idx) == idx) break;
  }

  g_sites[idx].target = target;
  g_sites[idx].name = name != nullptr ? name : "(unnamed)";
  memcpy(g_sites[idx].bytes, bytes != nullptr ? bytes : static_cast<const unsigned char*>(target), kSnapshotBytes);
#ifdef NEVR_TEST_HOOKS
  if (g_beforePublishForTest != nullptr) g_beforePublishForTest();
#endif
  InterlockedExchange(&g_sites[idx].published, 1);
  InterlockedIncrement(&g_recorded);
}

}  // namespace

void Record(const void* target, const char* name) { RecordImpl(target, name, nullptr); }

void RecordBytes(const void* target, const char* name, const unsigned char (&bytes)[16]) {
  static_assert(sizeof(bytes) == kSnapshotBytes, "the host's hook record is the guard's snapshot size");
  RecordImpl(target, name, bytes);
}

bool IsOurDetour(const void* target) {
  if (target == nullptr || !Readable(target, kSnapshotBytes)) return false;
  const LONG n = InterlockedCompareExchange(&g_reserved, 0, 0);
  for (LONG i = 0; i < n && i < kMaxGuarded; i++) {
    if (InterlockedCompareExchange(&g_sites[i].published, 0, 0) == 0) continue;
    if (g_sites[i].target == target) return memcmp(target, g_sites[i].bytes, kSnapshotBytes) == 0;
  }
  return false;
}

int VerifyAll(const char* context) {
  const LONG n = InterlockedCompareExchange(&g_reserved, 0, 0);
  int mismatches = 0;
  int newlyReported = 0;

  for (LONG i = 0; i < n && i < kMaxGuarded; i++) {
    GuardedSite& s = g_sites[i];
    if (InterlockedCompareExchange(&s.published, 0, 0) == 0) continue;
    if (s.target == nullptr) continue;
    if (!Readable(s.target, kSnapshotBytes)) continue;

    unsigned char now[kSnapshotBytes];
    memcpy(now, s.target, kSnapshotBytes);
    if (memcmp(now, s.bytes, kSnapshotBytes) == 0) continue;

    mismatches++;
    // Edge-triggered: log this site's ERROR once, the first check that finds
    // it overwritten. Nothing un-overwrites a clobbered hook, so without this
    // gate the identical line repeats every ~30s periodic check forever.
    if (InterlockedCompareExchange(&s.reported, 1, 0) != 0) continue;
    newlyReported++;

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
        "already owns",
        s.name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.target)),
        context != nullptr ? context : "(unknown)", expected, actual);
  }

  // A full table means detours that nothing is watching. Reported at ERROR each time the
  // dropped count has grown since the last report, so it cannot pass as quiet.
  const LONG dropped = InterlockedCompareExchange(&g_dropped, 0, 0);
  const LONG lastReported = InterlockedExchange(&g_dropReported, dropped);
  if (dropped > lastReported) {
    const bool haveFirst = InterlockedCompareExchange(&g_firstDropPublished, 0, 0) != 0;
    Log(EchoVR::LogLevel::Error,
        "[NEVR.PATCH] hook guard table full cap=%d dropped=%ld first=%s va=0x%llX after=%s — "
        "those detours are NOT guarded against overwrite",
        kMaxGuarded, static_cast<long>(dropped), haveFirst ? g_firstDropName : "(unknown)",
        static_cast<unsigned long long>(haveFirst ? reinterpret_cast<uintptr_t>(g_firstDropTarget) : 0),
        context != nullptr ? context : "(unknown)");
  }

  // Aggregate line is edge-triggered too: only fires when THIS check found a
  // new collision, not on every periodic tick for as long as an old one
  // remains overwritten. `mismatches` (the return value) still reports the
  // full current count — only the logging cadence changed.
  if (newlyReported > 0) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.PATCH] hook guard: %d new overwrite(s) detected after=%s "
        "(%d of %ld guarded address(es) now overwritten, cumulative)",
        newlyReported, context != nullptr ? context : "(unknown)",
        mismatches, static_cast<long>(n));
  }
  return mismatches;
}

int RecordedCount() { return static_cast<int>(InterlockedCompareExchange(&g_recorded, 0, 0)); }

int DroppedCount() { return static_cast<int>(InterlockedCompareExchange(&g_dropped, 0, 0)); }

int Capacity() { return kMaxGuarded; }

#ifdef NEVR_TEST_HOOKS
void SetBeforePublishForTest(void (*callback)()) { g_beforePublishForTest = callback; }

void ResetForTest() {
  InterlockedExchange(&g_reserved, 0);
  InterlockedExchange(&g_recorded, 0);
  memset(g_sites, 0, sizeof(g_sites));
  InterlockedExchange(&g_dropped, 0);
  InterlockedExchange(&g_dropReported, 0);
  InterlockedExchange(&g_firstDropClaimed, 0);
  InterlockedExchange(&g_firstDropPublished, 0);
  g_firstDropTarget = nullptr;
  g_firstDropName = nullptr;
  g_beforePublishForTest = nullptr;
}
#endif

}  // namespace nevr_hook_guard
