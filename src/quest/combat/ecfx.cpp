#include "quest/combat/ecfx.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/ec.h"
#include "quest/combat/ecfx_core.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/ec_page.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::ecfx {

const char kWallLib[] = "libac4e9374a146d3a4.so";

namespace {

using namespace core;
using sentinel::LogLevel;

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
std::atomic<bool> g_installed{false};

std::uint64_t NowMs() noexcept {
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
bool Named(std::uint64_t fn, int kind) noexcept {
  Dl_info di{};
  if (!fn || (fn & 0xf) || !dladdr(reinterpret_cast<void*>(fn), &di) || !di.dli_sname || !di.dli_fname) return false;
  const char* slash = std::strrchr(di.dli_fname, '/');
  return !std::strcmp(di.dli_sname, kEntrySym[kind]) && reinterpret_cast<std::uint64_t>(di.dli_saddr) == fn &&
         !std::strcmp(slash ? slash + 1 : di.dli_fname, kWallLib);
}

std::uint32_t* Gate() noexcept { return reinterpret_cast<std::uint32_t*>(sentinel::EcPage() + kGateOff); }
std::uint64_t* Queue() noexcept { return reinterpret_cast<std::uint64_t*>(sentinel::EcPage() + kQueueOff); }

void TickOnce(std::uint64_t now) noexcept {
  if (g_lastMs && now - g_lastMs < kEveryMs) return;
  g_lastMs = now;
  Run run[kSlots];
  Counts c;
  const int n = g_drain.Plan(Queue(), g_entries, now, &WallOf, run, &c);
  for (int i = 0; i < n; ++i) {
    if (Named(run[i].fn, run[i].kind)) {
      reinterpret_cast<EntryFn>(run[i].fn)(reinterpret_cast<void*>(run[i].ctx));  // the entry checks tp itself
      if (g_lines.fetch_add(1, std::memory_order_relaxed) < kLineCap) {
        char tp[19];
        sentinel::LogFields(LogLevel::kInfo, "combat_ecfx_ran", {{"kind", kKindName[run[i].kind]}, {"tp", sentinel::HexString(tp, run[i].ctx)}});
      }
    } else if (g_lines.fetch_add(1, std::memory_order_relaxed) < kLineCap) {
      char entry[19];
      sentinel::LogFields(LogLevel::kWarn, "combat_ecfx_ran",
                          {{"kind", kKindName[run[i].kind]}, {"entry", sentinel::HexString(entry, run[i].fn)}, {"status", "not_run"},
                           {"msg", "the queued entry is no longer the wall library's: the effect is not run"}});
    }
    Free(Queue(), run[i].slot);  // Q-1: after the entry returned
  }
  if ((c.boom || c.sound || c.refused || c.stale) && g_lines.fetch_add(1, std::memory_order_relaxed) < kLineCap)
    sentinel::LogFields(LogLevel::kInfo, "combat_ecfx_drain",
                        {{"boom_freed", c.boom}, {"sound_repeats", c.sound}, {"refused", c.refused}, {"stale_freed", c.stale}});
  if (!g_gateOn.load(std::memory_order_relaxed)) {  // fail-closed: only after a drain ran on the tick
    g_gateOn.store(true, std::memory_order_relaxed);
    __atomic_store_n(Gate(), 1u, __ATOMIC_RELEASE);
    sentinel::LogFields(LogLevel::kInfo, "combat_ecfx_gate",
                        {{"tick_tid", static_cast<long long>(syscall(SYS_gettid))}, {"msg", "gate +e20 = 1: the wall library now queues its NEUTRON effects"}});
  }
}

// A single runner; a second caller (a re-entry from inside an entry, or another thread) returns at once
void RunTick(std::uint64_t now) noexcept {
  if (g_running.test_and_set(std::memory_order_acquire)) return;
  TickOnce(now);
  g_running.clear(std::memory_order_release);
}
void Tick() noexcept { RunTick(NowMs()); }

void* Main(void*) {
  for (;;) {  // the wall lib loads with the first EC level: look once a second, quietly
    const Status s = Attach();
    if (s != Status::kWaiting) return nullptr;
    sleep(1);
  }
}

}  // namespace

Status Attach() noexcept {
  if (g_attached) return Status::kOn;
  void* lib = dlopen(kWallLib, RTLD_NOW | RTLD_NOLOAD);
  if (!lib) {
    if (!g_told_waiting) {
      g_told_waiting = true;
      sentinel::LogFields(LogLevel::kInfo, "combat_ecfx_waiting", {{"lib", kWallLib}, {"msg", "waiting for the wall library: gate +e20 stays 0"}});
    }
    return Status::kWaiting;
  }
  Entries e;
  for (int k = 0; k < 4; ++k) {
    void* p = dlsym(lib, kEntrySym[k]);
    if (!p || !Named(reinterpret_cast<std::uint64_t>(p), k)) {
      char a[19];
      sentinel::LogFields(LogLevel::kError, "combat_ecfx_attach",
                          {{"status", "no_entry"}, {"lib", kWallLib}, {"entry", kEntrySym[k]}, {"addr", sentinel::HexString(a, reinterpret_cast<std::uintptr_t>(p))},
                           {"msg", "the wall library has no such entry: NEUTRON effects stay off, gate +e20 stays 0"}});
      dlclose(lib);
      return Status::kNoEntry;
    }
    e.entry[k] = reinterpret_cast<std::uint64_t>(p);
  }
  // The reference RTLD_NOLOAD took is KEPT (never dlclosed): the lib then stays mapped for the process, so a queued entry
  // address can never point at unmapped or reused memory, even if the engine closes its own handle; the dladdr check before
  // each call stays (name + exact address)
  g_lib = lib;
  g_entries = e;
  if (!ec::OnNetTick(&Tick)) {
    sentinel::LogFields(LogLevel::kError, "combat_ecfx_attach",
                        {{"status", "no_tick_slot"}, {"msg", "the EC client has no free net tick slot: NEUTRON effects stay off, gate +e20 stays 0"}});
    return Status::kNoTickSlot;
  }
  g_attached = true;
  char charge[19], burst[19], sound[19];
  sentinel::LogFields(LogLevel::kInfo, "combat_ecfx_attach",
                      {{"status", "on"}, {"lib", kWallLib}, {"charge", sentinel::HexString(charge, e.entry[kCharge])},
                       {"burst", sentinel::HexString(burst, e.entry[kBurst])}, {"sound", sentinel::HexString(sound, e.entry[kSound])},
                       {"msg", "NEUTRON effects on (boom never run); the gate opens at the first drain"}});
  return Status::kOn;
}

ModuleResult Install(bool on) noexcept {
  if (legacy::Present("neutronfx.on")) {
    sentinel::LogFields(LogLevel::kWarn, "combat_ecfx_legacy_switch",
                        {{"file", "neutronfx.on"}, {"replaced_by", "combat.ecfx"},
                         {"msg", "the plugin's switch file is no longer read and changes nothing; set the combat key in config.yaml instead"}});
  }
  const char* token = "waiting";
  if (!on) {
    token = "off";
  } else if (g_installed.exchange(true)) {
    token = "already_ran";
  } else if (!ec::Installed()) {
    token = "needs_ec";
  } else if (!g_noThread) {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, Main, nullptr) != 0) token = "thread";
    pthread_attr_destroy(&attr);
  }
  const bool ok = std::strcmp(token, "waiting") == 0;
  sentinel::LogFields(ok || std::strcmp(token, "off") == 0 ? LogLevel::kInfo : LogLevel::kError, "combat_ecfx_install",
                      {{"result", token},
                       {"consequence", ok ? "NEUTRON effects attach when the wall library loads; the gate opens at the first drain on a combat map"
                                          : "NEUTRON effects stay off: gate +e20 stays 0 and the wall library runs its effects in its own node"}});
  return {ok, token, !on};  // off by its key: counted apart on the phase line, neither installed nor failed
}

void TickForTest(std::uint64_t now_ms) noexcept { RunTick(now_ms); }
void ResetForTest() noexcept {
  g_entries = Entries();
  g_drain = Drain();
  g_gateOn = false;
  g_lastMs = 0;
  g_attached = false;
  g_told_waiting = false;
  g_lines = 0;
  g_installed = false;
}
bool GateOpenForTest() noexcept { return __atomic_load_n(Gate(), __ATOMIC_ACQUIRE) != 0; }
void SetNoThreadForTest(bool on) noexcept { g_noThread = on; }
void SetEntryForTest(int kind, std::uint64_t fn) noexcept { g_entries.entry[kind] = fn; }

}  // namespace nevr_quest::combat::ecfx
