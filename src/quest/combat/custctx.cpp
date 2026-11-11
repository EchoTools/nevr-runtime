#include "quest/combat/custctx.h"

#include <dlfcn.h>
#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "quest/combat/combat_scope.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/line_budget.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::custctx {

namespace {

constexpr unsigned kMax = 256, kDropLines = 16, kTotalEvery = 50, kTotalLines = 16, kSpares = 4;
constexpr unsigned kLoadsEvery = 32, kLoadsLines = 32;

sentinel::GotHook g_loadHook;
sentinel::GotHook g_destroyHook;
sentinel::GotHook g_growHook;

std::uintptr_t g_base = 0;  // set before the first slot changes, read-only after
Pins g_pins = kPins;
Fns g_fns{};
std::atomic<unsigned> g_drops{0}, g_dropLines{0}, g_dropTotals{0};
std::atomic<unsigned> g_fails{0}, g_failLines{0}, g_failTotals{0}, g_spareNext{0};
std::atomic<unsigned> g_loads{0}, g_loadsLines{0}, g_refused{0}, g_refusedLines{0}, g_refusedTotals{0}, g_untracked{0};
std::atomic<bool> g_fullLogged{false};
void* g_spares[kSpares];  // set at install, before the first slot changes: elements for when the memalign fails too
std::atomic<int (*)(void**, std::size_t, std::size_t)> g_memalign{nullptr};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const void* (*)(const char*)> g_symbolLookup{nullptr};

// Resize as the loaded libr15 exports it (production), or what the test's lookup answers.
const void* ExportedResize() noexcept {
  const auto lookup = g_symbolLookup.load(std::memory_order_acquire);
  if (lookup != nullptr) return lookup(kResizeSymbol);
  void* const handle = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  if (handle == nullptr) return nullptr;
  const void* const symbol = dlsym(handle, kResizeSymbol);
  dlclose(handle);
  return symbol;
}

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
struct Entry {
  const void* ctx;
  std::uint32_t inflight;
  std::uint8_t live, dying;
};
Entry g_tab[kMax];  // under g_mu; ctx == nullptr = free
// The contexts whose load found the table full: their destroy is dropped with a why of its own (an address in no table is otherwise a stale second destroy).
// Under g_mu. More than kOverflow of them at once are counted (`untracked`) and their destroys give the stale why.
constexpr unsigned kOverflow = 64;
const void* g_over[kOverflow];

constexpr const char* kInstalledConsequence =
    "in a combat map or lobby loads are refused on a nearly full layout pool, a destroy runs only for a live context with no load in flight, and a draw table that "
    "cannot grow is made empty";
constexpr const char* kRefusedConsequence =
    "the cosmetics renderer runs as the game wrote it: a destroy racing a load, a draw table that cannot grow and a full layout pool can crash the game";

int Memalign(void** out, std::size_t align, std::size_t size) noexcept {
  const auto fn = g_memalign.load(std::memory_order_acquire);
  return fn != nullptr ? fn(out, align, size) : posix_memalign(out, align, size);
}

Entry* Find(const void* ctx) noexcept {
  for (Entry& e : g_tab) {
    if (e.ctx == ctx) return &e;
  }
  return nullptr;
}

Entry* FindOrAdd(const void* ctx) noexcept {
  Entry* freeSlot = nullptr;
  for (Entry& e : g_tab) {
    if (e.ctx == ctx) return &e;
    if (freeSlot == nullptr && e.ctx == nullptr) freeSlot = &e;
  }
  if (freeSlot != nullptr) *freeSlot = Entry{ctx, 0, 0, 0};
  return freeSlot;
}

// An entry nobody needs any more is freed. (No in-flight test: every LoadMeshData entry sets live, so a load in flight has live != 0.)
void Release(Entry* e) noexcept {
  if (e != nullptr && e->live == 0 && e->dying == 0) e->ctx = nullptr;
}

// The overflow list, under g_mu. Remember: this context's load found the table full. Forget: true when it was in the list (and is not any more).
void RememberOverflow(const void* ctx) noexcept {
  const void** freeSlot = nullptr;
  for (const void*& o : g_over) {
    if (o == ctx) return;
    if (o == nullptr && freeSlot == nullptr) freeSlot = &o;
  }
  if (freeSlot != nullptr) *freeSlot = ctx;
}
bool ForgetOverflow(const void* ctx) noexcept {
  for (const void*& o : g_over) {
    if (o == ctx) {
      o = nullptr;
      return true;
    }
  }
  return false;
}

void LogStats(bool final) noexcept {
  sentinel::LogFields(final ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug, "combat_custctx_stats",
                      {{"loads", g_loads.load(std::memory_order_relaxed)},
                       {"loads_refused", g_refused.load(std::memory_order_relaxed)},
                       {"destroys_dropped", g_drops.load(std::memory_order_relaxed)},
                       {"grow_failures", g_fails.load(std::memory_order_relaxed)},
                       {"untracked", g_untracked.load(std::memory_order_relaxed)},
                       {"final", final ? 1 : 0},
                       {"consequence", "loads is every LoadMeshData call, loads_refused those refused for a nearly full layout pool, destroys_dropped the "
                                       "destroys not passed on, grow_failures the draw tables made empty and untracked the loads that found the 256-context "
                                       "table full (their destroys are dropped); their rates over the run's time are the call rates of the three guarded "
                                       "functions"}});
}

void Full() noexcept {
  if (!g_fullLogged.exchange(true, std::memory_order_relaxed)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_custctx_full",
                        {{"max", kMax}, {"consequence", "more contexts than the table tracks (256): an overflow context's load goes to the game's function but is not tracked, so its destroy is dropped "
                                      "and the game's destroy is not run for it; counted as `untracked` in combat_custctx_stats, logged once"}});
  }
}

// The layout pool's free slots (a racy hint: the pool only changes on the render and load threads). Any unreadable word: not low.
bool RtLow(std::uint64_t* used, std::uint32_t* cap, bool* read) noexcept {
  bool ok = true;
  const auto pool = SafeRead<std::uintptr_t>(g_base + g_pins.rtPool, &ok);  // a null or unreadable pool pointer fails the reads below too
  const auto c = SafeRead<std::uint32_t>(pool + kPoolCapacityOffset, &ok);
  const auto fr = SafeRead<std::uint64_t>(pool + kPoolFreeOffset, &ok);
  if (!ok) return false;
  *read = true;
  *cap = c;
  *used = c - fr;
  return fr < kLayoutMargin;
}

// Whether the pool is low, in a combat map or the combat lobby only: the answer is believed only while its look is fresh (combat_scope.h `LookIsFresh`: a tick
// that stopped makes a true answer false after kStaleAfterMs). Not inlined into the handler: its reads would give the handler's arena path a frame.
// `*read` is set when the pool's words were read (the loads line says so; `used` and `cap` mean something only then).
[[gnu::noinline]] bool PoolIsLowInScope(std::uint64_t* used, std::uint32_t* cap, bool* read) noexcept {
  if (!scope::LookIsFresh()) return false;
  return RtLow(used, cap, read);
}

std::uint64_t LoadHandler(LoadThunk::Fn original, void* self, void* ctx, void* model) noexcept {
  bool tracked = false;
  pthread_mutex_lock(&g_mu);
  if (Entry* e = FindOrAdd(ctx)) {
    e->live = 1;
    e->dying = 0;
    ++e->inflight;
    tracked = true;
    ForgetOverflow(ctx);
  } else {
    RememberOverflow(ctx);
  }
  pthread_mutex_unlock(&g_mu);
  if (!tracked) {
    g_untracked.fetch_add(1, std::memory_order_relaxed);
    Full();
  }
  std::uint64_t used = 0;
  std::uint32_t cap = 0;
  bool poolRead = false;
  // The pool refusal acts only while a combat map or the combat lobby is loaded; in an arena match the load reaches the game's function as it is,
  // and no pool word is read. (The trap fires only when no slot is free; the margin below is not read from the game.)
  const bool low = scope::SeenCombatOrLobby() && PoolIsLowInScope(&used, &cap, &poolRead);
  const unsigned loads = g_loads.fetch_add(1, std::memory_order_relaxed) + 1;
  if (loads % kLoadsEvery == 0 &&
      LineBudget(&g_loadsLines, kLoadsLines, "custctx", "loads", "no further loads lines are written; the stats line still counts every load")) {
    if (poolRead) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_custctx_loads",
                          {{"loads", loads}, {"pool_read", 1}, {"layouts_used", used}, {"layouts_capacity", cap}});
    } else {  // the pool was not read for this load (not a combat level, or a stale answer): there is no usage to report
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_custctx_loads",
                          {{"loads", loads}, {"pool_read", 0},
                           {"msg", "the layout pool was not read for this load (no combat level is loaded, or its answer is stale), so no usage is reported"}});
    }
  }
  // A full pool is a deliberate trap (WriteLog + brk) in CGRenderTargetState::Initialize on this load's new mesh entries: fail the load
  // instead, as LoadMeshData does when a resource is missing (CreateContext then destroys the empty context, answers NULL).
  std::uint64_t result = 1;
  if (low) {
    const unsigned n = g_refused.fetch_add(1, std::memory_order_relaxed) + 1;
    if (LineBudget(&g_refusedLines, kDropLines, "custctx", "load_refused", "further refusals are written every 50th, up to 16 of those; the stats line counts every one") ||
        (n % kTotalEvery == 0 && LineBudget(&g_refusedTotals, kTotalLines, "custctx", "load_refused_every_50th", "no further refusal lines are written; the stats line still counts every one"))) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_custctx_load_refused",
                          {{"layouts_used", used},
                           {"layouts_capacity", cap},
                           {"refused", n},
                           {"consequence", "the render target layouts are nearly full, so this player's cosmetics are skipped instead of trapping the game"}});
    }
  } else {
    result = original(self, ctx, model);
  }
  if (tracked) {
    pthread_mutex_lock(&g_mu);
    Entry* e = Find(ctx);
    if (e != nullptr && e->inflight != 0) --e->inflight;
    pthread_mutex_unlock(&g_mu);
  }
  if ((loads & (kStatsEvery - 1)) == 0) LogStats(false);
  return result;
}

void Drop(const void* ctx, const char* why) noexcept {
  const unsigned n = g_drops.fetch_add(1, std::memory_order_relaxed) + 1;
  if (LineBudget(&g_dropLines, kDropLines, "custctx", "dropped", "further drops are summarised every 50th, up to 16 of those; the stats line counts every one")) {
    char c[19];
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_custctx_dropped",
                        {{"ctx", sentinel::HexString(c, reinterpret_cast<std::uintptr_t>(ctx))},
                         {"why", why},
                         {"dropped", n},
                         {"consequence", "the game's destroy was not run for this context"}});
  } else if (n % kTotalEvery == 0 &&
             LineBudget(&g_dropTotals, kTotalLines, "custctx", "drops_total", "no further drop totals are written; the stats line still counts every drop")) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_custctx_drops_total", {{"dropped", n}});
  }
}

std::uint64_t DestroyHandler(DestroyThunk::Fn original, void* self, void* ctx) noexcept {
  if (ctx == nullptr) return original(self, ctx);  // the stock function returns at once
  const char* why = nullptr;
  bool tracked = false;
  pthread_mutex_lock(&g_mu);
  Entry* e = Find(ctx);
  if (e == nullptr) {
    why = ForgetOverflow(ctx) ? "untracked: its load found the table full (256 contexts)" : "not live: no context holds this address";
  } else if (e->inflight != 0) {
    why = "its LoadMeshData is running";
  } else if (e->dying != 0) {
    why = "already being destroyed";
  } else if (e->live == 0) {
    why = "not live: already destroyed";
  } else {
    e->live = 0;
    e->dying = 1;
    tracked = true;
  }
  pthread_mutex_unlock(&g_mu);
  if (why != nullptr) {
    Drop(ctx, why);
    return 0;
  }
  const std::uint64_t result = original(self, ctx);
  if (tracked) {
    pthread_mutex_lock(&g_mu);
    if (Entry* d = Find(ctx)) {
      d->dying = 0;
      Release(d);
    }
    pthread_mutex_unlock(&g_mu);
  }
  return result;
}

// A table that would be written through NULL: made empty; Grow gets an element that is in no table.
void* Spare(GrowThunk::Fn original, DrawTable* t, const DrawTable& was, std::uint64_t add, std::uint64_t wanted, const char* why) noexcept {
  t->data = nullptr;
  t->size = 0;
  t->capacity = 0;
  t->count = 0;
  const unsigned n = g_fails.fetch_add(1, std::memory_order_relaxed) + 1;
  if (LineBudget(&g_failLines, kDropLines, "custctx", "draw_failure", "further draw failures are summarised every 50th, up to 16 of those; the stats line counts every one")) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_custctx_draw_failure",
                        {{"entries", add},
                         {"bytes", wanted},
                         {"in_use", was.count},
                         {"why", why},
                         {"failure", n},
                         {"consequence", "no memory for cosmetic draw entries: this player's cosmetics are skipped"}});
  } else if (n % kTotalEvery == 0 &&
             LineBudget(&g_failTotals, kTotalLines, "custctx", "draw_failures_total", "no further draw failure totals are written; the stats line still counts every failure")) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_custctx_draw_failures_total", {{"failures", n}});
  }
  // Grow itself builds the element in a one-slot table of ours; the slot is never freed (a resource load may still point into it), the table
  // header only lives for this call (a stack local).
  DrawTable one{};
  void* slot = nullptr;
  if (Memalign(&slot, 64, kDrawSize) != 0) slot = nullptr;
  if (slot == nullptr) slot = g_spares[g_spareNext.fetch_add(1, std::memory_order_relaxed) % kSpares];  // all set at install
  one.data = static_cast<std::uint8_t*>(slot);
  one.size = kDrawSize;
  one.allocator = was.allocator;
  one.step = 1;
  one.capacity = 1;
  return original(&one);
}

void* GrowHandler(GrowThunk::Fn original, DrawTable* t) noexcept {
  DrawTable was{};
  if (!SafeRead(reinterpret_cast<std::uintptr_t>(t), &was, sizeof(was))) return original(t);  // doubt: the real function decides
  if (was.count >= was.capacity) {
    const std::uint64_t add = was.step < was.capacity ? was.capacity : was.step;  // as Grow picks it (disassembly 0x1248964/68)
    const std::uint64_t wanted = (was.capacity + add) * kDrawSize;
    if (add == 0) return Spare(original, t, was, add, wanted, "step and capacity 0");
    g_fns.resize(t, wanted, 1);
    bool ok = true;
    const auto data = SafeRead<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(t) + offsetof(DrawTable, data), &ok);
    if (!ok || data == 0) return Spare(original, t, was, add, wanted, "Resize left the data NULL");
    t->capacity = was.capacity + add;
  } else if (was.data == nullptr) {
    return Spare(original, t, was, 0, 0, "room left but the data is NULL: a torn-down table");
  }
  return original(t);  // count < capacity and data set: Grow only adds the element
}

NEVR_HOOK_RECORD(kLoadRecord, LoadThunk, &LoadHandler);
NEVR_HOOK_RECORD(kDestroyRecord, DestroyThunk, &DestroyHandler);
NEVR_HOOK_RECORD(kGrowRecord, GrowThunk, &GrowHandler);

ModuleResult Refused(const char* token, const pinned::Failure* word = nullptr) noexcept {
  if (word != nullptr) {
    char at[19], haveText[19], wantText[19];
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_custctx_install",
                        {{"result", "refused"}, {"reason", token}, {"pin", word->what}, {"vaddr", sentinel::HexString(at, word->vaddr)},
                         {"found", word->readable ? sentinel::HexString(haveText, word->have) : "unreadable"},
                         {"expected", sentinel::HexString(wantText, word->want)}, {"consequence", kRefusedConsequence}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_custctx_install",
                        {{"result", "refused"}, {"reason", token}, {"consequence", kRefusedConsequence}});
  }
  return {false, token};
}

void FreeSpares() noexcept {
  for (void*& s : g_spares) {
    std::free(s);
    s = nullptr;
  }
}

// One slot's hook, armed first (the manager publishes the thunk's original before the slot changes).
template <typename Thunk, typename Record>
sentinel::GotStatus InstallOne(sentinel::GotHook& hook, const sentinel::GotTarget& target, const Record& record, sentinel::ImageLookup lookup) noexcept {
  Thunk::Arm(record);
  return sentinel::InstallThunk<Thunk>(hook, target, lookup);  // a refused slot keeps the game's call as it is; no entry is reachable through it
}

// A hook that was in is taken out again (the manager restores the word it replaced) and its thunk disarmed.
template <typename Thunk>
sentinel::GotStatus RemoveOne(sentinel::GotHook& hook) noexcept {
  const sentinel::GotStatus status = hook.Remove();
  Thunk::Disarm();
  return status;
}

ModuleResult RefuseSlot(const char* which, sentinel::GotStatus status, const char* rollback) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kError, "combat_custctx_install",
                      {{"result", "refused"}, {"reason", "slot_refused"}, {"which", which}, {"hook", sentinel::GotStatusName(status)},
                       {"rollback", rollback}, {"consequence", kRefusedConsequence}});
  return {false, sentinel::GotStatusName(status)};
}

}  // namespace

bool ResizeMatches(std::uintptr_t base, const Pins& pins, const void* resize, Fns* out) noexcept {
  if (reinterpret_cast<std::uintptr_t>(resize) != base + pins.fnResize) return false;
  out->resize = reinterpret_cast<void (*)(void*, std::uint64_t, std::uint32_t)>(const_cast<void*>(resize));
  return true;
}

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const Fns* fns, bool on) noexcept {
  // The old plugin's switch file changes nothing now; one that is there is told so.
  if (legacy::Present("custctx.off")) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_custctx_legacy_switch",
                        {{"key", "combat.custctx"}, {"file", "custctx.off"},
                         {"consequence", "the file is no longer read; set combat.custctx: false in config.yaml to turn the module off"}});
  }
  if (!on) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_custctx_install",
                        {{"result", "off"}, {"reason", "off_config"},
                         {"consequence", "combat.custctx is false: the cosmetics renderer runs as the game wrote it"}});
    return {false, "off_config", true};
  }
  if (g_loadHook.installed() || g_destroyHook.installed() || g_growHook.installed()) return Refused("already_installed");
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return Refused("module_not_loaded");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id");
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch");
  pinned::Failure word;
  if (pinned::FirstFailure(image.base, &kWordPins, 1, &word)) return Refused("pinned_word_mismatch", &word);
  Fns resolved{};
  if (fns != nullptr) {
    resolved = *fns;
  } else if (!ResizeMatches(image.base, pins, ExportedResize(), &resolved)) {
    resolved = Fns{};
  }
  if (resolved.resize == nullptr) return Refused("export_missing");
  for (void*& s : g_spares) {
    if (Memalign(&s, 64, kDrawSize) != 0) s = nullptr;
  }
  for (void* s : g_spares) {
    if (s == nullptr) {  // Spare must never hand Grow a NULL slot: without every spare, install nothing
      FreeSpares();
      return Refused("spare_alloc_failed");
    }
  }
  g_fns = resolved;
  g_pins = pins;
  g_base = image.base;  // published before the first slot changes

  sentinel::GotStatus status = InstallOne<LoadThunk>(g_loadHook, LoadTarget(image.base, pins), kLoadRecord, lookup);
  if (status != sentinel::GotStatus::kOk) return RefuseSlot("LoadMeshData", status, "none");
  status = InstallOne<DestroyThunk>(g_destroyHook, DestroyTarget(image.base, pins), kDestroyRecord, lookup);
  if (status != sentinel::GotStatus::kOk) {  // all or none
    const sentinel::GotStatus back = RemoveOne<LoadThunk>(g_loadHook);
    return RefuseSlot("DestroyContext", status, sentinel::GotStatusName(back));
  }
  status = InstallOne<GrowThunk>(g_growHook, GrowTarget(image.base, pins), kGrowRecord, lookup);
  if (status != sentinel::GotStatus::kOk) {
    const sentinel::GotStatus backDestroy = RemoveOne<DestroyThunk>(g_destroyHook);
    const sentinel::GotStatus backLoad = RemoveOne<LoadThunk>(g_loadHook);
    return RefuseSlot("Grow", status, backDestroy != sentinel::GotStatus::kOk ? sentinel::GotStatusName(backDestroy) : sentinel::GotStatusName(backLoad));
  }
  // The refusal needs the combat level answer; when it cannot change (the tick or the scope row's refresh is not installed) the refusal never acts, and the two
  // other guards, which act in every level, still do: the install says so.
  const bool scoped = scope::Live();
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_custctx_install",
                      {{"result", "installed"}, {"reason", "ok"}, {"load", "ok"}, {"destroy", "ok"}, {"grow", "ok"},
                       {"pool_refusal", scoped ? "combat_map_or_lobby" : "off_scope_not_live"}, {"layout_margin", kLayoutMargin}, {"consequence", kInstalledConsequence}});
  return {true, "installed"};
}

ModuleResult Install(bool on) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, kPins, nullptr, on);
}

void NoteEnd() noexcept {
  if (g_loadHook.installed() && g_destroyHook.installed() && g_growHook.installed()) LogStats(true);
}

Stats CurrentStats() noexcept {
  return {g_loads.load(std::memory_order_relaxed), g_refused.load(std::memory_order_relaxed), g_drops.load(std::memory_order_relaxed),
          g_fails.load(std::memory_order_relaxed)};
}

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }
void SetSymbolLookupForTest(const void* (*lookup)(const char* symbol)) noexcept { g_symbolLookup.store(lookup, std::memory_order_release); }
void SetMemalignForTest(int (*memalign)(void**, std::size_t, std::size_t)) noexcept { g_memalign.store(memalign, std::memory_order_release); }

void ResetForTest() noexcept {
  g_loadHook.Forget();
  g_destroyHook.Forget();
  g_growHook.Forget();
  LoadThunk::Reset();
  DestroyThunk::Reset();
  GrowThunk::Reset();
  pthread_mutex_lock(&g_mu);
  std::memset(g_tab, 0, sizeof(g_tab));
  pthread_mutex_unlock(&g_mu);
  g_drops = g_dropLines = g_dropTotals = 0;
  g_fails = g_failLines = g_failTotals = g_spareNext = 0;
  g_loads = g_loadsLines = g_refused = g_refusedLines = g_refusedTotals = g_untracked = 0;
  std::memset(g_over, 0, sizeof(g_over));
  g_fullLogged = false;
  FreeSpares();
  g_fns = Fns{};
  g_pins = kPins;
  g_base = 0;
  g_memalign.store(nullptr);
  g_lookup.store(nullptr);
  g_symbolLookup.store(nullptr);
}

}  // namespace nevr_quest::combat::custctx
