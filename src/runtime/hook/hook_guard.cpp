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
/// The bound is found, not set. A `git grep -nE` over src/ for calls to PatchDetour, InstallBootDetour,
/// hook_guard::Record and RecordBytes (outside src/runtime/tests and hook/patching.h, comment lines
/// dropped) finds 70 call lines: each is a place an install reaches Record(). A call line can install
/// several records (a loop over a table, or a wrapper called once per row or per engine hook: the
/// binary_bug_fixes hook table, loading_tips' detour list, plugin_assets'
/// two engine hooks), so the capacity is about twice the line count. The table holds live records, not call
/// lines: a retired record's slot is reused. If the table fills, Record() counts the drop (never silent):
/// VerifyAll() reports it at ERROR and DroppedCount() exposes it.
constexpr int kMaxGuarded = 128;

/// One slot. Writers change a slot only under g_writerLock, with `seq` odd, and readers skip a
/// slot whose `seq` is odd. `gen` is the record's generation (unique per published record), or 0
/// when the slot is free. Readers take no lock: a seqlock read copies the slot only when `seq` is
/// even and unchanged across the copy, so a reader never sees half a record.
struct GuardedSite {
  volatile LONG seq;
  volatile LONG gen;
  volatile LONG reportedGen;  // generation whose overwrite ERROR has been logged — a collision
                              // that stays overwritten (nothing un-overwrites one) must not
                              // re-log the identical ERROR every ~30s periodic check.
  const void* target;
  const char* name;  // string literal from the call site; not owned
  unsigned char bytes[kSnapshotBytes];
};

struct Snapshot {
  const void* target;
  const char* name;
  unsigned char bytes[kSnapshotBytes];
  LONG gen;
  LONG seq;  // the slot's seq when it was copied: even, and unchanged across the copy
};

GuardedSite g_sites[kMaxGuarded];

// Serializes Record, RecordBytes, Retire and ResetForTest. An SRW lock, not a spin lock: a
// Record can run under the loader lock during boot, and an uncontended acquire calls no loader
// code. Nothing holds it while logging.
SRWLOCK g_writerLock = SRWLOCK_INIT;

volatile LONG g_generation = 0;   // last generation handed out
volatile LONG g_slotsUsed = 0;    // one past the highest slot index ever claimed: readers scan up to here
volatile LONG g_recorded = 0;     // live records
volatile LONG g_dropped = 0;           // Record() calls that found the table full
volatile LONG g_dropReported = 0;       // g_dropped value VerifyAll() last reported
volatile LONG g_firstDropClaimed = 0;   // one writer fills the two fields below
volatile LONG g_firstDropPublished = 0;
const void* g_firstDropTarget = nullptr;
const char* g_firstDropName = nullptr;

#ifdef NEVR_TEST_HOOKS
void (*g_beforePublishForTest)() = nullptr;
/// Runs in VerifyAll right after a slot is copied, before the target's bytes are read.
void (*g_afterReadForTest)() = nullptr;
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

LONG SlotsInUse() { return InterlockedCompareExchange(&g_slotsUsed, 0, 0); }

/// Copies slot `i` when it holds a record and no writer is changing it. Lock-free: see GuardedSite.
bool ReadSlot(LONG i, Snapshot& out) {
  GuardedSite& s = g_sites[i];
  const LONG before = InterlockedCompareExchange(&s.seq, 0, 0);
  if ((before & 1) != 0) return false;
  const LONG gen = InterlockedCompareExchange(&s.gen, 0, 0);
  if (gen == 0) return false;
  out.target = s.target;
  out.name = s.name;
  memcpy(out.bytes, s.bytes, kSnapshotBytes);
  out.gen = gen;
  out.seq = before;
  return InterlockedCompareExchange(&s.seq, 0, 0) == before;
}

/// True when this call is the first to report the record of generation `gen` in slot `i`. A slot
/// reused by a later record has a new generation, so the new record's first overwrite is reported.
bool ClaimReport(LONG i, LONG gen) {
  GuardedSite& s = g_sites[i];
  for (;;) {
    const LONG prev = InterlockedCompareExchange(&s.reportedGen, 0, 0);
    if (prev == gen) return false;
    if (InterlockedCompareExchange(&s.reportedGen, gen, prev) == prev) return true;
  }
}

}  // namespace

namespace nevr_hook_guard {

namespace {

// `bytes` is the owner's record of what it wrote, or null to snapshot what is at `target` now.
void RecordImpl(const void* target, const char* name, const unsigned char* bytes) {
  if (target == nullptr) return;
  if (!Readable(target, kSnapshotBytes)) return;

  AcquireSRWLockExclusive(&g_writerLock);
  int idx = -1;
  for (int i = 0; i < kMaxGuarded; i++) {
    if (InterlockedCompareExchange(&g_sites[i].gen, 0, 0) == 0) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    ReleaseSRWLockExclusive(&g_writerLock);
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

  GuardedSite& s = g_sites[idx];
  const LONG gen = InterlockedIncrement(&g_generation);
  InterlockedIncrement(&s.seq);  // odd: readers skip the slot until it is published
  s.target = target;
  s.name = name != nullptr ? name : "(unnamed)";
  memcpy(s.bytes, bytes != nullptr ? bytes : static_cast<const unsigned char*>(target), kSnapshotBytes);
#ifdef NEVR_TEST_HOOKS
  if (g_beforePublishForTest != nullptr) g_beforePublishForTest();
#endif
  InterlockedExchange(&s.gen, gen);
  InterlockedIncrement(&g_recorded);
  if (idx + 1 > g_slotsUsed) InterlockedExchange(&g_slotsUsed, idx + 1);
  InterlockedIncrement(&s.seq);  // even: the slot is stable
  ReleaseSRWLockExclusive(&g_writerLock);
}

}  // namespace

void Record(const void* target, const char* name) { RecordImpl(target, name, nullptr); }

void RecordBytes(const void* target, const char* name, const unsigned char (&bytes)[16]) {
  static_assert(sizeof(bytes) == kSnapshotBytes, "the host's hook record is the guard's snapshot size");
  RecordImpl(target, name, bytes);
}

bool IsOurDetour(const void* target) {
  if (target == nullptr || !Readable(target, kSnapshotBytes)) return false;
  const LONG used = SlotsInUse();
  for (LONG i = 0; i < used; i++) {
    Snapshot snap{};
    if (!ReadSlot(i, snap) || snap.target != target) continue;
    if (memcmp(target, snap.bytes, kSnapshotBytes) == 0) return true;
  }
  return false;
}

int Retire(const void* target) {
  int retired = 0;
  const char* firstName = nullptr;
  if (target != nullptr) {
    AcquireSRWLockExclusive(&g_writerLock);
    const LONG used = SlotsInUse();
    for (LONG i = 0; i < used; i++) {
      GuardedSite& s = g_sites[i];
      if (InterlockedCompareExchange(&s.gen, 0, 0) == 0 || s.target != target) continue;
      if (firstName == nullptr) firstName = s.name;
      InterlockedIncrement(&s.seq);  // odd: readers skip the slot while it is freed
      InterlockedExchange(&s.gen, 0);
      InterlockedIncrement(&s.seq);  // even
      InterlockedDecrement(&g_recorded);
      retired++;
    }
    ReleaseSRWLockExclusive(&g_writerLock);
  }
  const unsigned long long va = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target));
  if (retired == 0) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] hook retire refused va=0x%llX reason=not_recorded; no guard record was there to forget",
        va);
    return 0;
  }
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] hook retired name=%s va=0x%llX records=%d", firstName, va, retired);
  return retired;
}

int VerifyAll(const char* context) {
  const LONG used = SlotsInUse();
  int mismatches = 0;
  int newlyReported = 0;

  for (LONG i = 0; i < used; i++) {
    Snapshot snap{};
    if (!ReadSlot(i, snap)) continue;
#ifdef NEVR_TEST_HOOKS
    if (g_afterReadForTest != nullptr) g_afterReadForTest();
#endif
    if (snap.target == nullptr) continue;
    if (!Readable(snap.target, kSnapshotBytes)) continue;

    unsigned char now[kSnapshotBytes];
    memcpy(now, snap.target, kSnapshotBytes);
    if (memcmp(now, snap.bytes, kSnapshotBytes) == 0) continue;

    // The bytes differ, and a Retire explains that: it changes the slot (seq, then gen 0) before MH_RemoveHook writes the
    // game's bytes back, so a copy taken across that is not an overwrite. A Record can reuse the slot the same way. A slot
    // whose seq or gen moved since the copy is skipped; an overwrite that is still there is found by the next pass.
    if (InterlockedCompareExchange(&g_sites[i].seq, 0, 0) != snap.seq ||
        InterlockedCompareExchange(&g_sites[i].gen, 0, 0) != snap.gen) {
      continue;
    }
    mismatches++;
    // Edge-triggered: log this record's ERROR once, the first check that finds
    // it overwritten. Nothing un-overwrites a clobbered hook, so without this
    // gate the identical line repeats every ~30s periodic check forever.
    if (!ClaimReport(i, snap.gen)) continue;
    newlyReported++;

    char expected[kSnapshotBytes * 2 + 1];
    char actual[kSnapshotBytes * 2 + 1];
    FormatBytes(snap.bytes, kSnapshotBytes, expected, sizeof(expected));
    FormatBytes(now, kSnapshotBytes, actual, sizeof(actual));

    // ERROR, not Warning: an operator MUST act. Two MinHook instances on one
    // address means the second trampolined off our JMP stub, and which of the
    // two hooks actually runs is not determined by either of them.
    Log(EchoVR::LogLevel::Error,
        "[NEVR.PATCH] hook overwritten name=%s va=0x%llX after=%s "
        "expected=%s actual=%s — another module detoured an address gamepatches "
        "already owns",
        snap.name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(snap.target)),
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
        mismatches, static_cast<long>(RecordedCount()));
  }
  return mismatches;
}

int RecordedCount() { return static_cast<int>(InterlockedCompareExchange(&g_recorded, 0, 0)); }

int DroppedCount() { return static_cast<int>(InterlockedCompareExchange(&g_dropped, 0, 0)); }

int Capacity() { return kMaxGuarded; }

#ifdef NEVR_TEST_HOOKS
void SetBeforePublishForTest(void (*callback)()) { g_beforePublishForTest = callback; }
void SetAfterReadForTest(void (*callback)()) { g_afterReadForTest = callback; }

void ResetForTest() {
  AcquireSRWLockExclusive(&g_writerLock);
  for (GuardedSite& s : g_sites) {
    InterlockedExchange(&s.gen, 0);
    InterlockedExchange(&s.reportedGen, 0);
  }
  InterlockedExchange(&g_recorded, 0);
  InterlockedExchange(&g_slotsUsed, 0);
  InterlockedExchange(&g_dropped, 0);
  InterlockedExchange(&g_dropReported, 0);
  InterlockedExchange(&g_firstDropClaimed, 0);
  InterlockedExchange(&g_firstDropPublished, 0);
  g_firstDropTarget = nullptr;
  g_firstDropName = nullptr;
  g_beforePublishForTest = nullptr;
  g_afterReadForTest = nullptr;
  ReleaseSRWLockExclusive(&g_writerLock);
}
#endif

}  // namespace nevr_hook_guard
