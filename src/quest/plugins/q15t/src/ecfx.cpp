#include "ecfx.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "ec.h"
#include "ec_core.h"
#include "ec_fld.h"
#include "ecfx_core.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {

const char kEcfxLib[] = "libac4e9374a146d3a4.so";

namespace {

using namespace ecfx;

constexpr char kEntrySym[4][32] = {"re33_ecent_ac4e93_17900", "re33_ecent_ac4e93_17700", "re33_ecent_ac4e93_17b00", "re33_ecent_ac4e93_17b60"};
constexpr const char* kKindName[4] = {"charge", "burst", "boom", "sound"};
constexpr int kLineCap = 96;
using EntryFn = void (*)(void* tp);

Entries g_entries;                              // set before the tick is registered; read on the tick only
Drain g_drain;                                  // the tick only
std::atomic<bool> g_gateOn{false};
std::uint64_t g_lastMs = 0;                     // the tick only
std::atomic_flag g_running = ATOMIC_FLAG_INIT;  // one drain at a time, whatever calls the tick
void* g_lib = nullptr;                          // our own reference to the wall lib, held for the process: it is never unmapped
std::atomic<int> g_lines{0};
bool g_attached = false;
bool g_noThread = false;
bool g_told_waiting = false;

std::uint64_t NowMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000;
}

// the wall's script data [[ctx]+0x68], read only through SafeRead; 0 = unreadable
std::uint64_t WallOf(std::uint64_t ctx) {
  std::uint64_t a = 0, w = 0;
  if (!SafeRead(static_cast<std::uintptr_t>(ctx), &a, sizeof a) || !a) return 0;
  if (!SafeRead(static_cast<std::uintptr_t>(a) + 0x68, &w, sizeof w)) return 0;
  return w;
}

// the address is still the named entry of the wall lib (dladdr: name and exact start), 16-aligned
bool Named(std::uint64_t fn, int kind) {
  Dl_info di{};
  if (!fn || (fn & 0xf) || !dladdr(reinterpret_cast<void*>(fn), &di) || !di.dli_sname || !di.dli_fname) return false;
  const char* slash = std::strrchr(di.dli_fname, '/');
  return !std::strcmp(di.dli_sname, kEntrySym[kind]) && reinterpret_cast<std::uint64_t>(di.dli_saddr) == fn &&
         !std::strcmp(slash ? slash + 1 : di.dli_fname, kEcfxLib);
}

std::uint32_t* Gate() { return reinterpret_cast<std::uint32_t*>(EcPage() + kGateOff); }
std::uint64_t* Queue() { return reinterpret_cast<std::uint64_t*>(EcPage() + kQueueOff); }

void TickOnce(std::uint64_t now) {
  if (g_lastMs && now - g_lastMs < kEveryMs) return;
  g_lastMs = now;
  Run run[kSlots];
  Counts c;
  const int n = g_drain.Plan(Queue(), g_entries, now, &WallOf, run, &c);
  for (int i = 0; i < n; ++i) {
    if (Named(run[i].fn, run[i].kind)) {
      reinterpret_cast<EntryFn>(run[i].fn)(reinterpret_cast<void*>(run[i].ctx));  // the entry checks tp itself
      if (g_lines.fetch_add(1, std::memory_order_relaxed) < kLineCap) {
        char tp[24];
        std::snprintf(tp, sizeof tp, "%p", reinterpret_cast<void*>(run[i].ctx));
        EcFld().S("kind", kKindName[run[i].kind]).S("tp", tp).Emit(NEVR_LOG_INFO, "neutronfx: ran");
      }
    } else if (g_lines.fetch_add(1, std::memory_order_relaxed) < kLineCap) {
      EcFld().S("kind", kKindName[run[i].kind]).X("entry", run[i].fn).Emit(NEVR_LOG_WARNING, "neutronfx: entry is no longer the wall lib's: not run");
    }
    Free(Queue(), run[i].slot);  // Q-1: after the entry returned
  }
  if ((c.boom || c.sound || c.refused || c.stale) && g_lines.fetch_add(1, std::memory_order_relaxed) < kLineCap)
    EcFld().N("boom_freed", c.boom).N("sound_repeats", c.sound).N("refused", c.refused).N("stale_freed", c.stale).Emit(NEVR_LOG_INFO, "neutronfx: drain");
  if (!g_gateOn.load(std::memory_order_relaxed)) {  // fail-closed: only after a drain ran on the tick
    g_gateOn.store(true, std::memory_order_relaxed);
    __atomic_store_n(Gate(), 1u, __ATOMIC_RELEASE);
    EcFld().N("tick_tid", static_cast<long long>(syscall(SYS_gettid))).Emit(NEVR_LOG_INFO, "neutronfx: gate +e20 = 1 (the wall lib now queues its NEUTRON effects)");
  }
}

// A single runner; a second caller (a re-entry from inside an entry, or another thread) returns at once
void RunTick(std::uint64_t now) {
  if (g_running.test_and_set(std::memory_order_acquire)) return;
  TickOnce(now);
  g_running.clear(std::memory_order_release);
}
void Tick() { RunTick(NowMs()); }

void* Main(void*) {
  for (;;) {  // the wall lib loads with the first EC level: look once a second, quietly
    const EcfxStatus s = EcfxAttach();
    if (s != EcfxStatus::kWaiting) return nullptr;
    sleep(1);
  }
}

}  // namespace

EcfxStatus EcfxAttach() {
  if (g_attached) return EcfxStatus::kOn;
  void* lib = dlopen(kEcfxLib, RTLD_NOW | RTLD_NOLOAD);
  if (!lib) {
    if (!g_told_waiting) {
      g_told_waiting = true;
      EcFld().S("lib", kEcfxLib).Emit(NEVR_LOG_INFO, "neutronfx: waiting for the wall lib (gate +e20 stays 0)");
    }
    return EcfxStatus::kWaiting;
  }
  Entries e;
  for (int k = 0; k < 4; ++k) {
    void* p = dlsym(lib, kEntrySym[k]);
    if (!p || !Named(reinterpret_cast<std::uint64_t>(p), k)) {
      char a[24];
      std::snprintf(a, sizeof a, "%p", p);
      EcFld().S("lib", kEcfxLib).S("entry", kEntrySym[k]).S("addr", a).Emit(NEVR_LOG_ERROR, "neutronfx: OFF: the wall lib has no such entry; gate +e20 stays 0");
      dlclose(lib);
      return EcfxStatus::kNoEntry;
    }
    e.entry[k] = reinterpret_cast<std::uint64_t>(p);
  }
  // The reference RTLD_NOLOAD took is KEPT (never dlclosed): the lib then stays mapped for the process, so a queued entry
  // address can never point at unmapped or reused memory, even if the engine closes its own handle; the dladdr check before
  // each call stays (name + exact address)
  g_lib = lib;
  g_entries = e;
  if (!EcOnNetTick(&Tick)) {
    EcFld().Emit(NEVR_LOG_ERROR, "neutronfx: OFF: no free net tick slot; gate +e20 stays 0");
    return EcfxStatus::kNoTickSlot;
  }
  g_attached = true;
  EcFld().S("lib", kEcfxLib).X("charge", e.entry[kCharge]).X("burst", e.entry[kBurst]).X("sound", e.entry[kSound])
      .Emit(NEVR_LOG_INFO, "neutronfx: on (boom never run); the gate opens at the first drain");
  return EcfxStatus::kOn;
}

// On with no switch file when the build says so (Q15T_ECFX_DEFAULT_ON, CMake default ON): a player install does not depend on a
// file in the data directory. The file still switches it on in a build that is not default-on.
#ifndef Q15T_ECFX_DEFAULT_ON
#define Q15T_ECFX_DEFAULT_ON 0
#endif

InstallResult InstallEcfx(std::uintptr_t /*base*/) {
  InstallResult r;
  if (!Q15T_ECFX_DEFAULT_ON && !EcDataFileExists("neutronfx.on")) {
    r.reason = "off";
    return r;
  }
  r.status = SwapStatus::kOk;
  r.reason = "waiting";
  if (g_noThread) return r;
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &attr, Main, nullptr) != 0) {
    EcFld().Emit(NEVR_LOG_ERROR, "neutronfx: OFF: thread not started; gate +e20 stays 0");
    r.status = SwapStatus::kNullSlot;
    r.reason = "thread";
  }
  pthread_attr_destroy(&attr);
  return r;
}

void EcfxTickForTest(std::uint64_t now_ms) { RunTick(now_ms); }
void EcfxResetForTest() {
  g_entries = Entries();
  g_drain = Drain();
  g_gateOn = false;
  g_lastMs = 0;
  g_attached = false;
  g_told_waiting = false;
  g_lines = 0;
}
bool EcfxGateOpenForTest() { return __atomic_load_n(Gate(), __ATOMIC_ACQUIRE) != 0; }
void SetEcfxNoThreadForTest(bool on) { g_noThread = on; }
void EcfxSetEntryForTest(int kind, std::uint64_t fn) { g_entries.entry[kind] = fn; }

}  // namespace q15t
