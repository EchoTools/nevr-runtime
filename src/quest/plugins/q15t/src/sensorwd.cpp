#include "sensorwd.h"

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <ctime>

#include "got_swap.h"
#include "module_switch.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

// ---- constants from the source build (the source build's fx, the block "fxsensor.on / sensorwd.on (FX-B T-07 ...)") ----
constexpr std::uint64_t kSphere = 0xac2d3f9f080ba45full;     // the sensor sphere's model record
constexpr std::uint64_t kTriggered = 0xfd8cf56544b904a3ull;  // TriggeredPing
constexpr std::uint64_t kFinished = 0x7cc621b8bf3248c5ull;   // FinishedPing (+4.0 s after its TriggeredPing)
constexpr std::int64_t kWdMs = 6000;      // still enabled this long after its enable = stuck
constexpr std::int64_t kPairMs = 250;     // a sphere enable this soon after a TriggeredPing belongs to that ping's actor
constexpr int kSlots = 32;                // spheres tracked (players + bots of one match, with room)
constexpr unsigned kPings = 8;            // TriggeredPings waiting for the tick
constexpr std::int64_t kSweepMs = 5000;   // the tick re-reads every tracked sphere this often and drops the dead ones
constexpr unsigned kLogLines = 64;        // tick / sweep action lines per process
constexpr unsigned kNoteLines = 12;       // table-full / ping-lost chatter lines per process
constexpr unsigned kThreadLines = 24;     // thread-identity lines per process
// CModelCS entry lookup, as OnComponentEnabled / OnComponentDisabled do it (disassembly 0x16b127c..) and the source reads it
constexpr std::uintptr_t kCsIndex = 0xd0;      // [cs+0xd0] -> u16 table; entry index = u16 at 4 * (h & 0xffff)
constexpr std::uintptr_t kCsEntries = 0x100;   // [cs+0x100] -> entry array, 16 bytes each
constexpr std::uintptr_t kEntModel = 8;        // entry+8: the CModel pointer (entry+4 is the visible flag the stock functions pass on)
constexpr std::uintptr_t kModelName = 0xcc8;   // [model+0xcc8]: the record id (source only, unverified)

struct Slot {
  std::uintptr_t cs;       // the sphere's component (0: free)
  std::uint64_t h;
  std::uint64_t act_a;     // the TriggeredPing actor pair paired with its first enable (0, 0: none)
  std::uint64_t act_b;
  std::int64_t en_ms;      // the last enable (the engine's or ours)
  bool enabled;            // enabled and timed
  bool forced;             // the tick turned it off; the component is still 'enabled' to the script
};
struct Ping {
  std::uint64_t a, b;
};

// ---- state. Everything below `g_mu` is touched by the hooks (whatever thread the engine calls them on) and by the tick (the game
// thread) and is read or written only with g_mu held. g_mu is held for short loops over plain fields only: never across SafeRead,
// a call into the game, or Log; never nested.
std::atomic<bool> g_on{false};
std::atomic<std::uintptr_t> g_real_en{0}, g_real_dis{0}, g_real_event{0};
std::atomic<unsigned> g_log_lines{0}, g_note_lines{0}, g_thread_lines{0};
std::atomic<long> g_tid[4] = {{0}, {0}, {0}, {0}};  // the last thread seen in each role (tick, enable, disable, event)
std::atomic<std::int64_t> g_next_sweep{0};
std::atomic<std::uint32_t> g_enables{0}, g_disables{0}, g_pings{0}, g_finished{0}, g_wd_off{0}, g_wd_back{0}, g_table_full{0},
    g_stale{0}, g_pings_lost{0};
std::int64_t (*g_clock)() = nullptr;
void (*g_hook)(int) = nullptr;

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;  // a plain POD (no constructor at load)
Slot g_slots[kSlots];
Ping g_ping[kPings];                  // TriggeredPing actor pairs the tick has not looked at yet
unsigned g_ping_n = 0;
std::uint64_t g_last_a = 0, g_last_b = 0;  // the last TriggeredPing actor pair and when (0, 0 before the first: pairs nothing)
std::int64_t g_last_ms = 0;

void Lock() { pthread_mutex_lock(&g_mu); }
void Unlock() { pthread_mutex_unlock(&g_mu); }

std::int64_t Now() {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void Hook(int stage) {
  if (g_hook != nullptr) g_hook(stage);
}

const char* Hex(char* buf, std::size_t n, unsigned long long v) {
  std::snprintf(buf, n, "%llx", v);
  return buf;
}

// A line inside a per-process budget; the first line over it is one WARNING that says so, then silence.
bool Budget(std::atomic<unsigned>* used, unsigned cap, const char* what) {
  const unsigned n = used->fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == cap + 1) {
    char c[16];
    std::snprintf(c, sizeof c, "%u", cap);
    const NevrLogField f[] = {{"lines", what}, {"cap", c}};
    Log(NEVR_LOG_WARNING, "sensorwd log cap reached: further lines of this kind are not written", f, 2);
  }
  return n <= cap;
}

// Which thread runs the tick and each wrapper: one line the first time a role is seen on a thread (and again when it moves),
// naming the last thread seen in each role, so one headset run shows which threads call what.
enum Role { kRoleTick = 0, kRoleEnable = 1, kRoleDisable = 2, kRoleEvent = 3 };
void NoteThread(Role role) {
  static thread_local long t_tid = 0;
  if (t_tid == 0) t_tid = static_cast<long>(syscall(SYS_gettid));
  const long tid = t_tid;
  if (g_tid[role].exchange(tid, std::memory_order_relaxed) == tid) return;
  if (!Budget(&g_thread_lines, kThreadLines, "threads")) return;
  static const char* const kNames[4] = {"tick", "enable", "disable", "event"};
  char t[24], a[24], b[24], c[24], d[24];
  long seen[4];
  for (int i = 0; i < 4; ++i) seen[i] = g_tid[i].load(std::memory_order_relaxed);
  std::snprintf(t, sizeof t, "%ld", tid);
  std::snprintf(a, sizeof a, "%ld", seen[0]);
  std::snprintf(b, sizeof b, "%ld", seen[1]);
  std::snprintf(c, sizeof c, "%ld", seen[2]);
  std::snprintf(d, sizeof d, "%ld", seen[3]);
  const NevrLogField f[] = {{"role", kNames[role]}, {"tid", t},          {"tick_tid", a},
                            {"enable_tid", b},      {"disable_tid", c}, {"event_tid", d}};
  Log(NEVR_LOG_INFO, "sensorwd thread (0 = role not seen yet)", f, 6);
}

// True when [cs+0xd0] / [cs+0x100] and the handle name a CModel whose record id is the sensor sphere's. Every read is a SafeRead.
bool IsSphere(std::uintptr_t cs, std::uint64_t h) {
  std::uintptr_t tbl = 0, ents = 0, model = 0;
  std::uint16_t idx = 0;
  std::uint64_t name = 0;
  if (!(SafeRead(cs + kCsIndex, &tbl, sizeof tbl) && SafeRead(tbl + 4 * (h & 0xffff), &idx, sizeof idx) &&
        SafeRead(cs + kCsEntries, &ents, sizeof ents) &&
        SafeRead(ents + 16 * static_cast<std::uintptr_t>(idx) + kEntModel, &model, sizeof model) &&
        SafeRead(model + kModelName, &name, sizeof name)))  // a null model: 0xcc8 is unmapped, the read fails
    return false;
  return name == kSphere;
}

Slot* Find(std::uintptr_t cs, std::uint64_t h, bool add) {  // g_mu held
  Slot* fresh = nullptr;
  for (Slot& s : g_slots) {
    if (s.cs == cs && s.h == h) return &s;
    if (s.cs == 0 && fresh == nullptr) fresh = &s;
  }
  if (add && fresh != nullptr) *fresh = Slot{cs, h, 0, 0, 0, false, false};
  return add ? fresh : nullptr;
}

void DropSlot(std::uintptr_t cs, std::uint64_t h) {
  Lock();
  Hook(14);
  if (Slot* s = Find(cs, h, false)) *s = Slot{0, 0, 0, 0, 0, false, false};
  Unlock();
}

void FullNote(std::uintptr_t cs, std::uint64_t h) {
  if (!Budget(&g_note_lines, kNoteLines, "notes")) return;
  char c[24], hh[24];
  const NevrLogField f[] = {{"cs", Hex(c, sizeof c, cs)}, {"h", Hex(hh, sizeof hh, h)}, {"slots", "32"}};
  Log(NEVR_LOG_WARNING, "sensorwd: no free slot for a sphere enable: this sphere is not watched", f, 3);
}

// The wrappers' work on a sphere enable (en) or disable. Does nothing for a model that is not the sensor sphere.
void OnModel(bool en, std::uintptr_t cs, std::uint64_t h) {
  if (!IsSphere(cs, h)) return;
  NoteThread(en ? kRoleEnable : kRoleDisable);
  const std::int64_t now = Now();
  bool full = false;
  Lock();
  Hook(11);
  if (en) {
    Slot* s = Find(cs, h, true);
    if (s != nullptr) {
      if (s->act_a == 0 && s->act_b == 0 && now - g_last_ms <= kPairMs) {
        s->act_a = g_last_a;
        s->act_b = g_last_b;
      }
      s->en_ms = now;
      s->enabled = true;
      s->forced = false;
    } else {
      full = true;
    }
  } else if (Slot* s = Find(cs, h, false)) {
    s->enabled = false;
    s->forced = false;
  }
  Unlock();
  (en ? g_enables : g_disables).fetch_add(1, std::memory_order_relaxed);
  if (full) {
    g_table_full.fetch_add(1, std::memory_order_relaxed);
    FullNote(cs, h);
  }
}

using Fn8 = std::uintptr_t (*)(std::uintptr_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                               std::uint64_t);

// The three wrappers are plain C++ (no naked thunk): the functions they stand in for take integers in x0..x7 only (CModelCS this +
// handle; CncaGameSpace this + CSymbol64 + SActor + CSymbol64 + unsigned), no stack argument, no float argument and no x8 result
// pointer (the stock bodies, docs/quest/q15t/modules/sensorwd.md), so passing x0..x7 on as eight integers is the whole calling convention.
__attribute__((noinline)) std::uintptr_t EnableWrapper(std::uintptr_t cs, std::uint64_t h, std::uint64_t a2, std::uint64_t a3,
                                                       std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7) {
  if (g_on.load(std::memory_order_acquire)) OnModel(true, cs, h);
  const auto real = reinterpret_cast<Fn8>(g_real_en.load(std::memory_order_acquire));
  return real != nullptr ? real(cs, h, a2, a3, a4, a5, a6, a7) : 0;
}

__attribute__((noinline)) std::uintptr_t DisableWrapper(std::uintptr_t cs, std::uint64_t h, std::uint64_t a2, std::uint64_t a3,
                                                        std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7) {
  if (g_on.load(std::memory_order_acquire)) OnModel(false, cs, h);
  const auto real = reinterpret_cast<Fn8>(g_real_dis.load(std::memory_order_acquire));
  return real != nullptr ? real(cs, h, a2, a3, a4, a5, a6, a7) : 0;
}

// SendComponentEventGlobal(gs, sym, a, b, j): high fan-in, so the call path is two compares and counters; TriggeredPing records its
// actor pair (a, b) under the lock.
__attribute__((noinline)) std::uintptr_t EventWrapper(std::uintptr_t gs, std::uint64_t sym, std::uint64_t a, std::uint64_t b,
                                                      std::uint64_t j, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7) {
  if (sym == kTriggered && g_on.load(std::memory_order_acquire)) {
    NoteThread(kRoleEvent);
    const std::int64_t now = Now();
    bool lost = false;
    Lock();
    Hook(13);
    g_last_a = a;
    g_last_b = b;
    g_last_ms = now;
    if (a != 0 || b != 0) {
      if (g_ping_n < kPings)
        g_ping[g_ping_n++] = Ping{a, b};
      else
        lost = true;
    }
    Unlock();
    g_pings.fetch_add(1, std::memory_order_relaxed);
    if (lost) {
      g_pings_lost.fetch_add(1, std::memory_order_relaxed);
      if (Budget(&g_note_lines, kNoteLines, "notes")) {
        char x[24], y[24];
        const NevrLogField f[] = {{"actor_a", Hex(x, sizeof x, a)}, {"actor_b", Hex(y, sizeof y, b)}, {"waiting", "8"}};
        Log(NEVR_LOG_WARNING, "sensorwd: TriggeredPing dropped, 8 are already waiting for the tick", f, 3);
      }
    }
  } else if (sym == kFinished && g_on.load(std::memory_order_acquire)) {
    g_finished.fetch_add(1, std::memory_order_relaxed);
  }
  const auto real = reinterpret_cast<Fn8>(g_real_event.load(std::memory_order_acquire));
  return real != nullptr ? real(gs, sym, a, b, j, a5, a6, a7) : 0;
}

struct Act {
  std::uintptr_t cs;
  std::uint64_t h;
  std::int64_t age;
  bool on;
};

void LogAct(const Act& x, std::int64_t now, std::uint32_t n) {
  if (!Budget(&g_log_lines, kLogLines, "actions")) return;
  char c[24], h[24], age[24], num[16], t[24];
  std::snprintf(age, sizeof age, "%lld", static_cast<long long>(x.age));
  std::snprintf(num, sizeof num, "%u", n);
  std::snprintf(t, sizeof t, "%lld", static_cast<long long>(now));
  const NevrLogField f[] = {{"cs", Hex(c, sizeof c, x.cs)}, {"h", Hex(h, sizeof h, x.h)}, {"age_ms", age}, {"n", num}, {"t_ms", t}};
  Log(NEVR_LOG_INFO, x.on ? "sensorwd back on: its actor pinged again" : "sensorwd off: sphere still enabled 6000 ms after its enable", f,
      5);
}

// Re-read every tracked sphere and drop the ones that no longer read as the sphere (the component or the level is gone).
void Sweep() {
  struct Key {
    std::uintptr_t cs;
    std::uint64_t h;
  } keys[kSlots], dead[kSlots];
  int n = 0, nd = 0;
  Lock();
  Hook(15);
  for (const Slot& s : g_slots)
    if (s.cs != 0) keys[n++] = Key{s.cs, s.h};
  Unlock();
  for (int i = 0; i < n; ++i)
    if (!IsSphere(keys[i].cs, keys[i].h)) dead[nd++] = keys[i];
  for (int i = 0; i < nd; ++i) DropSlot(dead[i].cs, dead[i].h);
  if (nd == 0) return;
  g_stale.fetch_add(static_cast<std::uint32_t>(nd), std::memory_order_relaxed);
  if (!Budget(&g_note_lines, kNoteLines, "notes")) return;
  char dn[16], tn[16];
  std::snprintf(dn, sizeof dn, "%d", nd);
  std::snprintf(tn, sizeof tn, "%d", n);
  const NevrLogField f[] = {{"dropped", dn}, {"tracked_before", tn}};
  Log(NEVR_LOG_INFO, "sensorwd: tracked spheres whose component no longer reads as the sphere were dropped", f, 2);
}

// One checked swap of a data word to a wrapper; `real_out` holds the stock function afterwards (cleared again by a failed swap).
InstallResult Swap(const char* what, std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t fn_vaddr, std::uintptr_t wrapper,
                   std::atomic<std::uintptr_t>* real_out) {
  const InstallResult r = InstallImportHook(what, base, slot_vaddr, wrapper, real_out, fn_vaddr);
  char slot[24], real[24];
  std::snprintf(slot, sizeof slot, "0x%llx", static_cast<unsigned long long>(slot_vaddr));
  std::snprintf(real, sizeof real, "0x%llx", static_cast<unsigned long long>(fn_vaddr));
  if (r.status == SwapStatus::kOk) {
    const NevrLogField f[] = {{"hook", what}, {"slot_vaddr", slot}, {"real_vaddr", real}, {"status", StatusName(r.status)}};
    Log(NEVR_LOG_INFO, "sensorwd word swapped", f, 4);
  } else {
    LogSlotRefusal("sensorwd refused: a word could not be swapped, nothing else is changed", base, slot_vaddr, fn_vaddr, r);
  }
  return r;
}

// Put a swapped word back to the stock function (only if it still holds the wrapper). Loud when it cannot.
void PutBack(const char* what, std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t fn_vaddr, std::uintptr_t wrapper) {
  const SwapResult r = SwapWord(base + slot_vaddr, wrapper, base + fn_vaddr);
  char slot[24];
  std::snprintf(slot, sizeof slot, "0x%llx", static_cast<unsigned long long>(slot_vaddr));
  const NevrLogField f[] = {{"hook", what}, {"slot_vaddr", slot}, {"status", StatusName(r.status)}};
  const bool ok = r.status == SwapStatus::kOk;
  Log(ok ? NEVR_LOG_WARNING : NEVR_LOG_ERROR, ok ? "sensorwd: word put back to the stock function" : "sensorwd: word could not be put back",
      f, 3);
}

// The word at base + slot_vaddr must read base + fn_vaddr. Else one ERROR line naming pin (the slot's ELF vaddr) / found / expected.
bool HoldsStock(const char* message, std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t fn_vaddr, InstallResult* out,
                const char* reason) {
  std::uintptr_t word = 0;
  const bool ok = SafeRead(base + slot_vaddr, &word, sizeof word);
  if (ok && word == base + fn_vaddr) return true;
  out->status = ok ? SwapStatus::kUnexpectedOriginal : SwapStatus::kUnmapped;
  out->reason = reason;
  LogSlotRefusal(message, base, slot_vaddr, fn_vaddr, *out);
  return false;
}

constexpr const char* kMsgEnable = "sensorwd refused: CModelCS OnComponentEnabled vtable word is not the stock function, nothing changed";
constexpr const char* kMsgDisable = "sensorwd refused: CModelCS OnComponentDisabled vtable word is not the stock function, nothing changed";
constexpr const char* kMsgEvent = "sensorwd refused: SendComponentEventGlobal slot is not the stock function, nothing changed";
constexpr const char* kHookEvent = "CncaGameSpace::SendComponentEventGlobal (JUMP_SLOT)";
constexpr const char* kHookEnable = "CModelCS::OnComponentEnabled (vtable word)";
constexpr const char* kHookDisable = "CModelCS::OnComponentDisabled (vtable word)";

void PutBackEvent(std::uintptr_t base, const SensorWdAddrs& a) {
  PutBack(kHookEvent, base, a.event_slot, a.event_fn, reinterpret_cast<std::uintptr_t>(&EventWrapper));
}
void PutBackEnable(std::uintptr_t base, const SensorWdAddrs& a) {
  PutBack(kHookEnable, base, a.en_slot, a.en_fn, reinterpret_cast<std::uintptr_t>(&EnableWrapper));
}

}  // namespace

bool SensorWdWordPinsMatch(std::uintptr_t base) {
  return CheckWordPins("sensorwd", base, kSensorWdWordPins, kSensorWdWordPinCount);
}

InstallResult InstallSensorWdAt(std::uintptr_t base, const SensorWdAddrs& a) {
  InstallResult out;
  if (base == 0) {
    out.reason = "no_base";
    return out;
  }
  // the three words must hold the stock functions before anything is written
  if (!HoldsStock(kMsgEnable, base, a.en_slot, a.en_fn, &out, "vtable_word_not_stock")) return out;
  if (!HoldsStock(kMsgDisable, base, a.dis_slot, a.dis_fn, &out, "vtable_word_not_stock")) return out;
  if (!HoldsStock(kMsgEvent, base, a.event_slot, a.event_fn, &out, "event_slot_not_stock")) return out;
  g_on.store(true, std::memory_order_release);
  const auto fail = [&](const InstallResult& r) {
    g_on.store(false, std::memory_order_release);
    return r;
  };
  // the event slot first: it only records pings
  const InstallResult re = Swap(kHookEvent, base, a.event_slot, a.event_fn, reinterpret_cast<std::uintptr_t>(&EventWrapper), &g_real_event);
  if (re.status != SwapStatus::kOk) return fail(re);
  Hook(1);
  const InstallResult ren = Swap(kHookEnable, base, a.en_slot, a.en_fn, reinterpret_cast<std::uintptr_t>(&EnableWrapper), &g_real_en);
  if (ren.status != SwapStatus::kOk) {
    PutBackEvent(base, a);
    return fail(ren);
  }
  Hook(2);
  const InstallResult rd = Swap(kHookDisable, base, a.dis_slot, a.dis_fn, reinterpret_cast<std::uintptr_t>(&DisableWrapper), &g_real_dis);
  if (rd.status != SwapStatus::kOk) {
    PutBackEnable(base, a); PutBackEvent(base, a);
    return fail(rd);
  }
  Hook(3);
  {
    char es[24], ds[24], vs[24], wd[16];
    std::snprintf(es, sizeof es, "0x%llx", static_cast<unsigned long long>(a.en_slot));
    std::snprintf(ds, sizeof ds, "0x%llx", static_cast<unsigned long long>(a.dis_slot));
    std::snprintf(vs, sizeof vs, "0x%llx", static_cast<unsigned long long>(a.event_slot));
    std::snprintf(wd, sizeof wd, "%lld", static_cast<long long>(kWdMs));
    const NevrLogField f[] = {{"enable_slot", es}, {"disable_slot", ds}, {"event_slot", vs}, {"watchdog_ms", wd}, {"kill_file", "sensorwd.off"}};
    Log(NEVR_LOG_INFO, "sensorwd on: a sphere still enabled 6000 ms after its enable is turned off on the game thread", f, 5);
  }
  out.status = SwapStatus::kOk;
  out.reason = "";
  return out;
}

InstallResult InstallSensorWd(std::uintptr_t base) {
  InstallResult r;
  if (ModuleSwitchPresent("sensorwd.off")) {
    const NevrLogField f[] = {{"why", "sensorwd.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "sensorwd off", f, 1);
    r.reason = "off_file";
    return r;
  }
  if (!SensorWdWordPinsMatch(base)) {
    r.reason = "word_pin_mismatch";
    return r;
  }
  return InstallSensorWdAt(base, kSensorWdPinned);
}

void SensorWdTick(std::uintptr_t /*game*/) {
  if (!g_on.load(std::memory_order_acquire)) return;
  NoteThread(kRoleTick);
  const std::int64_t now = Now();
  Act act[kSlots];
  int n = 0;
  Lock();
  Hook(12);
  for (Slot& s : g_slots) {
    if (s.cs == 0) continue;
    bool back = false;
    if (s.forced)  // a sphere with no actor pair (0, 0) matches no ping: (0, 0) pings are never queued
      for (unsigned i = 0; i < g_ping_n; ++i)
        if (g_ping[i].a == s.act_a && g_ping[i].b == s.act_b) back = true;
    if (back) {
      s.forced = false;
      s.enabled = true;
      s.en_ms = now;
      act[n++] = Act{s.cs, s.h, 0, true};
    } else if (s.enabled && now - s.en_ms >= kWdMs) {
      act[n++] = Act{s.cs, s.h, now - s.en_ms, false};
      s.forced = true;
      s.enabled = false;
    }
  }
  g_ping_n = 0;
  Unlock();
  for (int i = 0; i < n; ++i) {
    const Act& x = act[i];
    if (!IsSphere(x.cs, x.h)) {  // the component is gone or recycled: the engine's function is not called on it
      DropSlot(x.cs, x.h);
      g_stale.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const auto fn = reinterpret_cast<Fn8>((x.on ? g_real_en : g_real_dis).load(std::memory_order_acquire));
    if (fn == nullptr) continue;
    fn(x.cs, x.h, 0, 0, 0, 0, 0, 0);  // the stock function itself, not through the vtable word: the wrappers do not see it
    const std::uint32_t c = (x.on ? g_wd_back : g_wd_off).fetch_add(1, std::memory_order_relaxed) + 1;
    LogAct(x, now, c);
  }
  if (now >= g_next_sweep.load(std::memory_order_relaxed)) {
    g_next_sweep.store(now + kSweepMs, std::memory_order_relaxed);
    Sweep();
  }
}

SensorWdStats GetSensorWdStats() {
  SensorWdStats s;
  s.on = g_on.load();
  s.enables = g_enables.load();
  s.disables = g_disables.load();
  s.pings = g_pings.load();
  s.finished = g_finished.load();
  s.wd_off = g_wd_off.load();
  s.wd_back = g_wd_back.load();
  s.table_full = g_table_full.load();
  s.stale = g_stale.load();
  s.pings_lost = g_pings_lost.load();
  Lock();
  Hook(16);
  for (const Slot& x : g_slots) {
    if (x.cs == 0) continue;
    ++s.tracked;
    s.forced += x.forced;
    s.enabled += x.enabled;
  }
  Unlock();
  return s;
}

void SetSensorWdClockForTest(std::int64_t (*clock_ms)()) { g_clock = clock_ms; }
void SetSensorWdHookForTest(void (*hook)(int stage)) { g_hook = hook; }
void SetSensorWdOnForTest(bool on) { g_on.store(on); }
bool SensorWdLockHeldForTest() {
  if (pthread_mutex_trylock(&g_mu) != 0) return true;
  pthread_mutex_unlock(&g_mu);
  return false;
}
void ResetSensorWdForTest() {
  g_on.store(false);
  g_real_en.store(0);
  g_real_dis.store(0);
  g_real_event.store(0);
  g_log_lines.store(0);
  g_note_lines.store(0);
  g_thread_lines.store(0);
  for (auto& t : g_tid) t.store(0);
  g_next_sweep.store(0);
  g_enables.store(0);
  g_disables.store(0);
  g_pings.store(0);
  g_finished.store(0);
  g_wd_off.store(0);
  g_wd_back.store(0);
  g_table_full.store(0);
  g_stale.store(0);
  g_pings_lost.store(0);
  Lock();
  for (Slot& s : g_slots) s = Slot{0, 0, 0, 0, 0, false, false};
  g_ping_n = 0;
  g_last_a = g_last_b = 0;
  g_last_ms = 0;
  Unlock();
  g_hook = nullptr;
}

}  // namespace q15t
