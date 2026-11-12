#include "quest/combat/sensorwd.h"

#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::sensorwd {

namespace {

namespace tgt = targets;

// ---- constants of the game and of the rule -------------------------------------------------------------------------------------
constexpr std::uint64_t kSphere = 0xac2d3f9f080ba45fULL;     // the sensor sphere's model record
constexpr std::uint64_t kTriggered = 0xfd8cf56544b904a3ULL;  // TriggeredPing (CSymbol64Hash("delegate_TriggeredPing"))
constexpr std::uint64_t kFinished = 0x7cc621b8bf3248c5ULL;   // FinishedPing (CSymbol64Hash("delegate_FinishedPing"))
constexpr std::int64_t kPairMs = 250;                        // a sphere enable this soon after a TriggeredPing belongs to that ping's actor
constexpr std::int64_t kSweepMs = 5000;                      // the tick re-reads every tracked sphere this often and drops the dead ones
constexpr unsigned kLogLines = 64;                           // tick and sweep action lines per process
constexpr unsigned kNoteLines = 12;                          // table-full, ping-lost and swept lines per process
constexpr unsigned kThreadLines = 24;                        // thread-identity lines per process
// CModelCS entry lookup, as OnComponentEnabled and OnComponentDisabled do it (disassembly 0x16b127c..)
constexpr std::uintptr_t kCsIndex = 0xd0;     // [cs+0xd0] -> u16 table; entry index = u16 at 4 * (h & 0xffff)
constexpr std::uintptr_t kCsEntries = 0x100;  // [cs+0x100] -> entry array, 16 bytes each
constexpr std::uintptr_t kEntModel = 8;       // entry+8: the CModel pointer
constexpr std::uintptr_t kModelName = 0xcc8;  // [model+0xcc8]: the record id

struct Slot {
  std::uintptr_t cs;    // the sphere's component (0: free)
  std::uint64_t h;
  std::uint64_t actA;   // the TriggeredPing actor pair paired with its first enable (0, 0: none)
  std::uint64_t actB;
  std::int64_t enMs;    // the last enable (the engine's or ours)
  bool enabled;         // enabled and timed
  bool forced;          // the tick turned it off; the component is still 'enabled' to the script
};
struct Ping {
  std::uint64_t a, b;
};

// ---- state -------------------------------------------------------------------------------------------------------------------------
// Everything below `g_mu` is touched by the handlers (whatever thread the engine calls them on) and by the tick (the game thread) and is read or written
// only with g_mu held. g_mu is held for short loops over plain fields only: never across a read of game memory, a call into the game or a log line; never nested.
std::atomic<bool> g_on{false};
std::atomic<bool> g_tickRegistered{false};
// The combat level answer as the tick last read it (combat_scope.h `InCombatMap`): only the tick writes it, to see the edge of the scope.
std::atomic<bool> g_wasInScope{false};
std::atomic<std::int64_t> g_nextSweepMs{0};
std::atomic<unsigned> g_logLines{0}, g_noteLines{0}, g_threadLines{0};
std::atomic<long> g_tid[4] = {{0}, {0}, {0}, {0}};  // the last thread seen in each role (tick, enable, disable, event)
std::atomic<std::uint8_t> g_firstSaid{0};            // bits: 1 sphere enable, 2 sphere disable, 4 ping
std::atomic<std::uint8_t> g_capSaid{0};              // bits per kind of capped line: 1 actions, 2 notes, 4 threads
std::atomic<std::uint32_t> g_enables{0}, g_disables{0}, g_pings{0}, g_finished{0}, g_wdOff{0}, g_wdBack{0}, g_tableFull{0}, g_stale{0}, g_pingsLost{0};
std::atomic<std::int64_t (*)() noexcept> g_clock{nullptr};
std::atomic<void (*)(int) noexcept> g_hook{nullptr};

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
Slot g_slots[kSlots];
Ping g_ping[kPings];  // TriggeredPing actor pairs the tick has not looked at yet
unsigned g_pingN = 0;
std::uint64_t g_lastA = 0, g_lastB = 0;  // the last TriggeredPing actor pair, and when (0, 0 before the first: pairs nothing)
std::int64_t g_lastMs = 0;

sentinel::GotHook g_hooks[3];  // enabled, disabled, event

void Lock() noexcept { pthread_mutex_lock(&g_mu); }
void Unlock() noexcept { pthread_mutex_unlock(&g_mu); }

std::int64_t Now() noexcept {
  const auto clock = g_clock.load(std::memory_order_acquire);
  if (clock != nullptr) return clock();
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void Hook(int stage) noexcept {
  const auto hook = g_hook.load(std::memory_order_acquire);
  if (hook != nullptr) hook(stage);
}

// The scope as a handler and the tick read it: the combat level answer (combat_scope.h `InCombatMap`) says a combat map is loaded and no Echo Arena
// level is, and its last look is at most scope::kStaleAfterMs (1 s) old. The first test is one relaxed load of the scope word, which is all the path of an arena match
// pays; the clock is read only on the path of a combat one.
bool InScope() noexcept { return scope::InCombatMap(); }

void ClearTable() noexcept {
  Lock();
  for (Slot& s : g_slots) s = Slot{0, 0, 0, 0, 0, false, false};
  g_pingN = 0;
  g_lastA = g_lastB = 0;
  g_lastMs = 0;
  Unlock();
}

// A line inside a per-process budget; the first line over it is one Warning that says so, then silence.
bool Budget(std::atomic<unsigned>* used, unsigned cap, std::uint8_t capBit, const char* what) noexcept {
  const unsigned n = used->fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > cap && !(g_capSaid.fetch_or(capBit, std::memory_order_acq_rel) & capBit)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_sensorwd_cap",
                        {{"lines", what}, {"cap", cap}, {"msg", "further lines of this kind are counted, not written"}});
  }
  return n <= cap;
}

// Which thread runs the tick and each handler: one line the first time a role is seen on a thread (and again when it moves), naming the last thread seen in each
// role, so one headset run shows which threads call what.
enum Role { kRoleTick = 0, kRoleEnable = 1, kRoleDisable = 2, kRoleEvent = 3 };
void NoteThread(Role role) noexcept {
  const long tid = static_cast<long>(syscall(SYS_gettid));
  if (g_tid[role].exchange(tid, std::memory_order_relaxed) == tid) return;
  if (!Budget(&g_threadLines, kThreadLines, 4, "threads")) return;
  static const char* const kNames[4] = {"tick", "enable", "disable", "event"};
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_sensorwd_thread",
                      {{"role", kNames[role]}, {"tid", tid}, {"tick_tid", g_tid[0].load(std::memory_order_relaxed)},
                       {"enable_tid", g_tid[1].load(std::memory_order_relaxed)}, {"disable_tid", g_tid[2].load(std::memory_order_relaxed)},
                       {"event_tid", g_tid[3].load(std::memory_order_relaxed)},
                       {"msg", "a role of the watchdog ran on this thread (0 = role not seen yet)"}});
}

void SayFirst(std::uint8_t bit, const char* what) noexcept {
  if (g_firstSaid.fetch_or(bit, std::memory_order_acq_rel) & bit) return;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_sensorwd_first",
                      {{"what", what}, {"msg", "the watchdog saw the game's scanner sphere in a combat map for the first time in this process"}});
}

// True when [cs+0xd0] / [cs+0x100] and the handle name a CModel whose record id is the sensor sphere's. Every read is a SafeRead.
bool IsSphere(std::uintptr_t cs, std::uint64_t h) noexcept {
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

Slot* Find(std::uintptr_t cs, std::uint64_t h, bool add) noexcept {  // g_mu held
  Slot* fresh = nullptr;
  for (Slot& s : g_slots) {
    if (s.cs == cs && s.h == h) return &s;
    if (s.cs == 0 && fresh == nullptr) fresh = &s;
  }
  if (add && fresh != nullptr) *fresh = Slot{cs, h, 0, 0, 0, false, false};
  return add ? fresh : nullptr;
}

void DropSlot(std::uintptr_t cs, std::uint64_t h) noexcept {
  Lock();
  Hook(14);
  if (Slot* s = Find(cs, h, false)) *s = Slot{0, 0, 0, 0, 0, false, false};
  Unlock();
}

// What a handler found, for the line it writes after the game's function has returned.
struct Seen {
  bool sphere = false;
  bool full = false;
  bool lost = false;
  std::uintptr_t cs = 0;
  std::uint64_t h = 0;
};

// The handlers' work on a model enable (en) or disable, before the game's function runs. Does nothing for a model that is not the sensor sphere.
Seen TrackModel(bool en, std::uintptr_t cs, std::uint64_t h) noexcept {
  Seen seen;
  seen.cs = cs;
  seen.h = h;
  if (!IsSphere(cs, h)) return seen;
  seen.sphere = true;
  const std::int64_t now = Now();
  Lock();
  Hook(11);
  if (en) {
    Slot* s = Find(cs, h, true);
    if (s != nullptr) {
      if (s->actA == 0 && s->actB == 0 && now - g_lastMs <= kPairMs) {
        s->actA = g_lastA;
        s->actB = g_lastB;
      }
      s->enMs = now;
      s->enabled = true;
      s->forced = false;
    } else {
      seen.full = true;
    }
  } else if (Slot* s = Find(cs, h, false)) {
    s->enabled = false;
    s->forced = false;
  }
  Unlock();
  (en ? g_enables : g_disables).fetch_add(1, std::memory_order_relaxed);
  if (seen.full) g_tableFull.fetch_add(1, std::memory_order_relaxed);
  return seen;
}

void ReportModel(bool en, const Seen& seen) noexcept {
  if (!seen.sphere) return;
  NoteThread(en ? kRoleEnable : kRoleDisable);
  SayFirst(en ? 1 : 2, en ? "sphere_enable" : "sphere_disable");
  if (seen.full && Budget(&g_noteLines, kNoteLines, 2, "notes")) {
    char c[19], hh[19];
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_sensorwd_note",
                        {{"what", "table_full"}, {"cs", sentinel::HexString(c, seen.cs)}, {"handle", sentinel::HexString(hh, seen.h)}, {"slots", kSlots},
                         {"msg", "no free slot for a sphere enable: this sphere is not watched"}});
  }
}

// TriggeredPing records its actor pair before the game's function runs (the script enables the sphere inside it, and the enable pairs by the last ping).
Seen TrackEvent(std::uint64_t symbol, std::uint64_t a, std::uint64_t b) noexcept {
  Seen seen;
  if (symbol == kTriggered) {
    seen.sphere = true;  // "saw a ping"
    const std::int64_t now = Now();
    Lock();
    Hook(13);
    g_lastA = a;
    g_lastB = b;
    g_lastMs = now;
    if (a != 0 || b != 0) {
      if (g_pingN < kPings) {
        g_ping[g_pingN++] = Ping{a, b};
      } else {
        seen.lost = true;
      }
    }
    Unlock();
    g_pings.fetch_add(1, std::memory_order_relaxed);
    if (seen.lost) g_pingsLost.fetch_add(1, std::memory_order_relaxed);
  } else if (symbol == kFinished) {
    g_finished.fetch_add(1, std::memory_order_relaxed);
  }
  return seen;
}

void ReportEvent(const Seen& seen, std::uint64_t a, std::uint64_t b) noexcept {
  if (!seen.sphere) return;
  NoteThread(kRoleEvent);
  SayFirst(4, "ping");
  if (seen.lost && Budget(&g_noteLines, kNoteLines, 2, "notes")) {
    char x[19], y[19];
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_sensorwd_note",
                        {{"what", "ping_lost"}, {"actor_a", sentinel::HexString(x, a)}, {"actor_b", sentinel::HexString(y, b)}, {"waiting", kPings},
                         {"msg", "TriggeredPing dropped: the tick has not looked at the pings already waiting"}});
  }
}

// ---- the three handlers: the scope first, then the plugin's work ----------------------------------------------------------------------

std::uint64_t OnEnabled(tgt::EnabledThunk::Fn original, void* component, std::uint64_t handle) noexcept {
  if (!InScope()) return original(component, handle);
  const Seen seen = TrackModel(true, reinterpret_cast<std::uintptr_t>(component), handle);
  const std::uint64_t result = original(component, handle);
  ReportModel(true, seen);
  return result;
}

std::uint64_t OnDisabled(tgt::DisabledThunk::Fn original, void* component, std::uint64_t handle) noexcept {
  if (!InScope()) return original(component, handle);
  const Seen seen = TrackModel(false, reinterpret_cast<std::uintptr_t>(component), handle);
  const std::uint64_t result = original(component, handle);
  ReportModel(false, seen);
  return result;
}

std::uint64_t OnEvent(tgt::EventThunk::Fn original, void* gamespace, std::uint64_t symbol, std::uint64_t a, std::uint64_t b, std::uint32_t flags) noexcept {
  if (!InScope()) return original(gamespace, symbol, a, b, flags);
  const Seen seen = TrackEvent(symbol, a, b);
  const std::uint64_t result = original(gamespace, symbol, a, b, flags);
  ReportEvent(seen, a, b);
  return result;
}

constexpr tgt::EnabledThunk::Handler kEnabledHandler = &OnEnabled;
constexpr tgt::DisabledThunk::Handler kDisabledHandler = &OnDisabled;
constexpr tgt::EventThunk::Handler kEventHandler = &OnEvent;
NEVR_HOOK_RECORD(kEnabledRecord, tgt::EnabledThunk, kEnabledHandler);
NEVR_HOOK_RECORD(kDisabledRecord, tgt::DisabledThunk, kDisabledHandler);
NEVR_HOOK_RECORD(kEventRecord, tgt::EventThunk, kEventHandler);

// ---- the tick ----------------------------------------------------------------------------------------------------------------------------

struct Act {
  std::uintptr_t cs;
  std::uint64_t h;
  std::int64_t age;
  bool on;
};

void LogAct(const Act& x, std::uint32_t n) noexcept {
  if (!Budget(&g_logLines, kLogLines, 1, "actions")) return;
  char c[19], h[19];
  if (x.on) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_sensorwd_back_on",
                        {{"cs", sentinel::HexString(c, x.cs)}, {"handle", sentinel::HexString(h, x.h)}, {"n", n},
                         {"msg", "a sphere that was turned off came back on at its actor's next ping"}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_sensorwd_off",
                        {{"cs", sentinel::HexString(c, x.cs)}, {"handle", sentinel::HexString(h, x.h)}, {"age_ms", x.age}, {"n", n},
                         {"msg", "a sphere still enabled 6000 ms after its enable was turned off on the game thread"}});
  }
}

// Re-read every tracked sphere and drop the ones that no longer read as the sphere (the component or the level is gone).
void Sweep() noexcept {
  struct Key {
    std::uintptr_t cs;
    std::uint64_t h;
  } keys[kSlots], dead[kSlots];
  std::size_t n = 0, nd = 0;
  Lock();
  Hook(15);
  for (const Slot& s : g_slots) {
    if (s.cs != 0) keys[n++] = Key{s.cs, s.h};
  }
  Unlock();
  for (std::size_t i = 0; i < n; ++i) {
    if (!IsSphere(keys[i].cs, keys[i].h)) dead[nd++] = keys[i];
  }
  for (std::size_t i = 0; i < nd; ++i) DropSlot(dead[i].cs, dead[i].h);
  if (nd == 0) return;
  g_stale.fetch_add(static_cast<std::uint32_t>(nd), std::memory_order_relaxed);
  if (!Budget(&g_noteLines, kNoteLines, 2, "notes")) return;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_sensorwd_note",
                      {{"what", "swept"}, {"dropped", nd}, {"tracked_before", n},
                       {"msg", "tracked spheres whose component no longer reads as the sphere were dropped"}});
}

// The scope as this tick reads it. combat_scope has no change callback, so the tick keeps the answer it read last and sees the edge itself: leaving the
// scope empties the table, so nothing stale survives into the next match. The scope row registers its refresh with the tick before this module's row
// does, so on every frame the answer read here is the one the refresh just made (or kept, between its looks); the level that decided it is on that
// row's `combat_scope_change` line.
void OnTick(std::uintptr_t) noexcept {
  const std::int64_t now = Now();
  const bool in = InScope();
  const bool was = g_wasInScope.exchange(in, std::memory_order_relaxed);
  if (in != was) {
    if (was) ClearTable();
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_sensorwd_scope",
                        {{"from", was ? 1 : 0}, {"to", in ? 1 : 0},
                         {"msg", in ? "a combat map is loaded: the watchdog is watching the scanner spheres"
                                    : "no combat map (or an Echo Arena level, or the combat level answer is stale): every handler passes through and the "
                                      "table is emptied"}});
  }
  if (!in) return;
  NoteThread(kRoleTick);
  Act act[kSlots];
  std::size_t n = 0;
  Lock();
  Hook(12);
  for (Slot& s : g_slots) {
    if (s.cs == 0) continue;
    bool back = false;
    if (s.forced) {  // a sphere with no actor pair (0, 0) matches no ping: (0, 0) pings are never queued
      for (unsigned i = 0; i < g_pingN; ++i) {
        if (g_ping[i].a == s.actA && g_ping[i].b == s.actB) back = true;
      }
    }
    if (back) {
      s.forced = false;
      s.enabled = true;
      s.enMs = now;
      act[n++] = Act{s.cs, s.h, 0, true};
    } else if (s.enabled && now - s.enMs >= kWatchdogMs) {
      act[n++] = Act{s.cs, s.h, now - s.enMs, false};
      s.forced = true;
      s.enabled = false;
    }
  }
  g_pingN = 0;
  Unlock();
  for (std::size_t i = 0; i < n; ++i) {
    const Act& x = act[i];
    if (!IsSphere(x.cs, x.h)) {  // the component is gone or recycled: the engine's function is not called on it
      DropSlot(x.cs, x.h);
      g_stale.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    // The game's own function, not through the vtable word: the handlers do not see this call.
    const tgt::EnabledThunk::Fn fn = x.on ? tgt::EnabledThunk::Original() : tgt::DisabledThunk::Original();
    if (fn == nullptr) continue;
    (void)fn(reinterpret_cast<void*>(x.cs), x.h);
    const std::uint32_t c = (x.on ? g_wdBack : g_wdOff).fetch_add(1, std::memory_order_relaxed) + 1;
    LogAct(x, c);
  }
  if (now >= g_nextSweepMs.load(std::memory_order_relaxed)) {
    g_nextSweepMs.store(now + kSweepMs, std::memory_order_relaxed);
    Sweep();
  }
}

const char* FirstPinMismatch(const unsigned char* base) noexcept {
  for (const tgt::Word& pin : tgt::kWords) {
    std::uint32_t found = 0;
    std::memcpy(&found, base + pin.vaddr, sizeof(found));
    if (found != pin.word) return "sensor_code";
  }
  return nullptr;
}

void LogInstall(sentinel::LogLevel level, const char* state, const char* reason, const char* enabled, const char* disabled, const char* event,
                const char* msg) noexcept {
  sentinel::LogFields(level, "combat_sensorwd_install",
                      {{"state", state}, {"reason", reason}, {"switch", "combat.sensorwd"}, {"scope", "combat_map"}, {"enabled", enabled},
                       {"disabled", disabled}, {"event_slot", event}, {"watchdog_ms", kWatchdogMs}, {"msg", msg}});
}

void WarnLegacyKillFile() noexcept {
  if (!legacy::Present("sensorwd.off")) return;
  sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_sensorwd_legacy_switch",
                      {{"file", "sensorwd.off"},
                       {"replaced_by", "combat.sensorwd: false"},
                       {"msg", "the plugin's kill file is no longer read and changes nothing; set the combat key in config.yaml instead"}});
}

constexpr const char* kRefusedMsg = "a scanner sphere stuck on is not turned off";

void ForgetHooks() noexcept {
  for (sentinel::GotHook& hook : g_hooks) hook.Forget();
  tgt::EnabledThunk::Reset();
  tgt::DisabledThunk::Reset();
  tgt::EventThunk::Reset();
}

// Puts a hook this install made back (the game's word again) and disarms its thunk; a word that cannot be put back keeps its entry, which calls the game's
// function (a disarmed thunk passes every call through).
void Undo(std::size_t index) noexcept {
  g_hooks[index].Remove();
  if (index == 0) tgt::EnabledThunk::Disarm();
  if (index == 1) tgt::DisabledThunk::Disarm();
  if (index == 2) tgt::EventThunk::Disarm();
}

}  // namespace

ModuleResult InstallAt(bool configOn, sentinel::ImageLookup lookup, const targets::Pins& pins) noexcept {
  WarnLegacyKillFile();
  if (!configOn) {
    LogInstall(sentinel::LogLevel::kInfo, "off", "off_config", "none", "none", "none",
               "combat.sensorwd is false: the game's scanner sphere is untouched");
    return {false, "off_config", true};  // off by its key: counted apart on the phase line, neither installed nor failed
  }
  const auto refuse = [](const char* reason, const char* enabled, const char* disabled, const char* event) noexcept -> ModuleResult {
    LogInstall(sentinel::LogLevel::kError, "refused", reason, enabled, disabled, event, kRefusedMsg);
    return {false, reason};
  };
  // The handlers act only while the combat level answer says a combat map is loaded: when that answer cannot change (the tick or the scope row's refresh
  // is not installed) the watchdog would never act, so nothing is hooked (combat_scope.h `Live`).
  if (!scope::Live()) return refuse("scope_not_live", "none", "none", "none");
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return refuse("module_not_loaded", "none", "none", "none");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return refuse("no_build_id", "none", "none", "none");
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return refuse("build_id_mismatch", "none", "none", "none");
  if (FirstPinMismatch(reinterpret_cast<const unsigned char*>(image.base)) != nullptr) return refuse("pin_mismatch", "none", "none", "none");
  // The event word first: it only records pings. All three or none: a hook that was made is removed when a later one is refused.
  tgt::EventThunk::Arm(kEventRecord);
  const sentinel::GotStatus event = sentinel::InstallThunk<tgt::EventThunk>(g_hooks[2], tgt::EventTarget(image.base, pins), lookup);
  if (event != sentinel::GotStatus::kOk) {
    if (!g_hooks[2].installed()) tgt::EventThunk::Disarm();
    return refuse("hook", "none", "none", sentinel::GotStatusName(event));
  }
  tgt::EnabledThunk::Arm(kEnabledRecord);
  const sentinel::GotStatus enabled = sentinel::InstallThunk<tgt::EnabledThunk>(g_hooks[0], tgt::EnabledTarget(image.base, pins), lookup);
  if (enabled != sentinel::GotStatus::kOk) {
    if (!g_hooks[0].installed()) tgt::EnabledThunk::Disarm();
    Undo(2);
    return refuse("hook", sentinel::GotStatusName(enabled), "none", sentinel::GotStatusName(event));
  }
  tgt::DisabledThunk::Arm(kDisabledRecord);
  const sentinel::GotStatus disabled = sentinel::InstallThunk<tgt::DisabledThunk>(g_hooks[1], tgt::DisabledTarget(image.base, pins), lookup);
  if (disabled != sentinel::GotStatus::kOk) {
    if (!g_hooks[1].installed()) tgt::DisabledThunk::Disarm();
    Undo(0);
    Undo(2);
    return refuse("hook", sentinel::GotStatusName(enabled), sentinel::GotStatusName(disabled), sentinel::GotStatusName(event));
  }
  // The tick last: a watchdog whose tick could never run installs nothing, so the three hooks are put back when the table is full.
  if (!g_tickRegistered.load(std::memory_order_acquire)) {
    if (!tick::Add(&Tick)) {
      Undo(1);
      Undo(0);
      Undo(2);
      return refuse("tick_table_full", sentinel::GotStatusName(enabled), sentinel::GotStatusName(disabled), sentinel::GotStatusName(event));
    }
    g_tickRegistered.store(true, std::memory_order_release);
  }
  g_on.store(true, std::memory_order_release);
  LogInstall(sentinel::LogLevel::kInfo, "on", "default", sentinel::GotStatusName(enabled), sentinel::GotStatusName(disabled),
             sentinel::GotStatusName(event),
             "a scanner sphere still enabled 6000 ms after its enable is turned off on the game thread, in combat maps only; an Echo Arena level passes through");
  return {true, "installed"};
}

ModuleResult Install(bool configOn) noexcept { return InstallAt(configOn, &sentinel::FindLoadedImage, tgt::kPins); }

void Tick(std::uintptr_t game) noexcept { OnTick(game); }

Stats CurrentStats() noexcept {
  Stats s;
  s.on = g_on.load();
  s.inScope = g_wasInScope.load();
  s.enables = g_enables.load();
  s.disables = g_disables.load();
  s.pings = g_pings.load();
  s.finished = g_finished.load();
  s.wdOff = g_wdOff.load();
  s.wdBack = g_wdBack.load();
  s.tableFull = g_tableFull.load();
  s.stale = g_stale.load();
  s.pingsLost = g_pingsLost.load();
  Lock();
  Hook(16);
  for (const Slot& x : g_slots) {
    if (x.cs == 0) continue;
    ++s.tracked;
    s.forced += x.forced ? 1 : 0;
    s.enabled += x.enabled ? 1 : 0;
  }
  Unlock();
  return s;
}

void SetClockForTest(std::int64_t (*clockMs)() noexcept) noexcept { g_clock.store(clockMs, std::memory_order_release); }
void SetHookForTest(void (*hook)(int) noexcept) noexcept { g_hook.store(hook, std::memory_order_release); }
bool LockHeldForTest() noexcept {
  if (pthread_mutex_trylock(&g_mu) != 0) return true;
  pthread_mutex_unlock(&g_mu);
  return false;
}
void SetLegacyDirForTest(const char* dir) noexcept { legacy::SetDirectoryForTest(dir); }

void ResetForTest() noexcept {
  ForgetHooks();
  g_on.store(false);
  g_tickRegistered.store(false);
  g_wasInScope.store(false);
  g_nextSweepMs.store(0);
  g_logLines.store(0);
  g_noteLines.store(0);
  g_threadLines.store(0);
  for (auto& t : g_tid) t.store(0);
  g_firstSaid.store(0);
  g_capSaid.store(0);
  g_enables.store(0);
  g_disables.store(0);
  g_pings.store(0);
  g_finished.store(0);
  g_wdOff.store(0);
  g_wdBack.store(0);
  g_tableFull.store(0);
  g_stale.store(0);
  g_pingsLost.store(0);
  ClearTable();
  g_hook.store(nullptr);
  g_clock.store(nullptr);
}

}  // namespace nevr_quest::combat::sensorwd
