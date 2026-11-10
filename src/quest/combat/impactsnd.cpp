#include "quest/combat/impactsnd.h"

#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "nevr/symbol_hash.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/impactsnd_targets.h"
#include "quest/combat/safe_read.h"
#include "quest/combat/thread_word.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::impactsnd {

namespace {

using sentinel::LogLevel;

constexpr std::uintptr_t kMapCount = 0x30;     // map+0x30 = row count
constexpr std::uintptr_t kMapFlags = 0x38;     // bit 0 = sort pending
constexpr std::uintptr_t kWhizzbyOff = 0x40;   // the second table (bullet_whizzby) sits 0x40 into kSoundSettings

// ---- The rebuilt loader: LoadSoundSettings in C++ (disassembly 0x2221d04, 808 bytes; the stock body is not copied, its steps are).
// The stock loader's row scratch {key, id} sits at sp+0x10 / sp+0x18, the same bytes as the path string it hands to the next CJson::Next
// (MakeTStrPrintF "bullet_impact|<gun>"), so after the first row the path reads a key, and each gun keeps one row. The rebuilt loader keeps the path
// and the row in separate storage, so every member of every gun is appended.
// The calls go to the stock loader's own direct-call targets (impactsnd_targets.h kRebuildCallees). Signatures: the disassembly decompiles of
// CJson::Next(char const*, void*) const, CJson::NameAt(void*) const, CJson::TString(void*, char*) const (called with a third argument, the fallback),
// CMemBlock::Resize(ulonglong, uint) and the stock call sites (x0 = this, the arguments in x1, x2) in 0x2221d04..0x2222028. CSymbol64 is the SDK's
// CSymbol64Hash; CSoundSymbol is replaced by WwiseEventId, because CalculateSymbolValue returns kStockId for every name on the Quest.
enum RebuildIdx : std::size_t { kIdxCtor, kIdxDtor, kIdxLoad, kIdxNext, kIdxNameAt, kIdxTString, kIdxResize };
using JsonCtorFn = void (*)(void* self);
using JsonDtorFn = void (*)(void* self);
using JsonLoadFn = int (*)(void* self, const char* path);
using JsonNextFn = void* (*)(void* self, const char* name, const void* from);
using JsonNameAtFn = const char* (*)(void* self, const void* node);
using JsonTStringFn = const char* (*)(void* self, const void* node, const char* fallback);
using MemBlockResizeFn = void (*)(void* self, std::uint64_t bytes, int keep);
constexpr const char* kImpactPath = "bullet_impact";
constexpr const char* kWhizzbyPath = "bullet_whizzby";
// the stock SPrintF("%s|%s", "/json/r14/config", "mp_sound_settings.json")
constexpr char kJsonFile[] = "/json/r14/config|mp_sound_settings.json";

constexpr double kWaitS = 600.0;   // the count line is logged after this long even with no table
constexpr double kPollS = 0.25;    // the tick reads the table at most this often
constexpr double kRecheckS = 1.0;  // while a table is in scope, the tick asks the levels again this often (an arena space can start beside a combat one)
constexpr double kSettleS = 2.0;   // the count is logged once it is non-zero for this long
constexpr std::uint64_t kRowCount = 15;   // 5 guns x 3 members (the PC table)
constexpr std::uint64_t kPlayerRows = 5;  // what the stock loader builds on the Quest (only the first member of each gun)
constexpr std::uint32_t kStockId = 1;     // what CalculateSymbolValue returns on the Quest
constexpr std::uint64_t kRowMax = 64;
constexpr unsigned kNoteLines = 12;     // non-writing chatter lines per process
constexpr unsigned kWriteLines = 200;   // lines of passes that wrote, per process
constexpr unsigned kThreadLines = 24;   // thread-identity lines per process
constexpr unsigned kScopeLines = 16;    // scope decisions per process (one per game-space load)

// key = CSymbol64(subkey) ^ CSymbol64(gun); id = FNV-1 32 of the lowercase event name (smg uses the _quark events).
constexpr Row kIds[] = {
    {0x6e044bbd445b5992ull, 0xd19cd68d},  // assault player  play_bullet_impact_player
    {0x00031201131c0906ull, 0x8ec36d51},  // assault barrier play_bullet_impact_shield
    {0x0005161500000000ull, 0x49a215e5},  // assault default play_bullet_impact_geo
    {0xa2ff10f8c1ba53a8ull, 0xd19cd68d},  // scout player
    {0xccf8494496fd033cull, 0x8ec36d51},  // scout barrier
    {0xccfe4d5085e10a3aull, 0x49a215e5},  // scout default
    {0x0000020302120006ull, 0xd19cd68d},  // rocket player
    {0x6e075bbf55555092ull, 0x8ec36d51},  // rocket barrier
    {0x6e015fab46495994ull, 0x49a215e5},  // rocket default
    {0xa2ff10f9cbb34ab9ull, 0xd19cd68d},  // rifle player
    {0xccf849459cf41a2dull, 0x8ec36d51},  // rifle barrier
    {0xccfe4d518fe8132bull, 0x49a215e5},  // rifle default
    {0x893a0780b3b9ede1ull, 0x8b9c3b62},  // smg player  play_bullet_impact_player_quark
    {0xe73d5e3ce4febd75ull, 0xd57a0ac6},  // smg barrier play_bullet_impact_shield_quark
    {0xe73b5a28f7e2b473ull, 0xc44e25da},  // smg default play_bullet_impact_geo_quark
};

using LoadSig = std::uint64_t();
using ShutdownSig = std::uint64_t(std::uint64_t self);
struct LoadTag {};
struct ShutdownTag {};
using LoadThunk = sentinel::CallbackThunk<LoadTag, LoadSig>;
using ShutdownThunk = sentinel::CallbackThunk<ShutdownTag, ShutdownSig>;

// ---- state. Plain values are written at install (before the handlers are hooked in) or only on the game thread; what the handlers' threads and the
// tick share is atomic.
sentinel::GotHook g_loadHook;
sentinel::GotHook g_shutdownHook;
std::uintptr_t g_base = 0;
bool g_write = true;                       // combat.impactsnd_write
std::atomic<bool> g_on{false};
std::atomic<bool> g_writersOk{false};      // the Shutdown handler is in: the only state in which a row may be written
std::atomic<bool> g_rebuild{false};        // the load handler runs the rebuilt LoadSoundSettings, not the stock one
std::atomic<std::uintptr_t> g_game{0};     // the CR15Game the tick last saw: what the load handler asks the levels of
std::atomic<bool> g_scoped{false};         // a table that a combat level loaded is live: the tick polls it
bool g_loadHooked = false;                 // the load handler is in: it marks a load. Without it the tick asks the levels itself, once a second
double g_nextScope = 0;                    // that second; and, while a table is in scope, the next time the tick asks whether a combat level is still loaded
std::atomic<bool> g_unscoping{false};      // no combat level is loaded any more: the stock id goes back into the table, then the scope ends
std::atomic<std::uintptr_t> g_rebuiltMap{0};  // the table the rebuilt loader filled: every row of it, the whizzby rows too, holds an id of ours
std::atomic<std::uintptr_t> g_unsDone{0};  // the unsorted table already written (once per table): handler and tick
std::atomic<unsigned> g_noteLines{0};
std::atomic<unsigned> g_writeLines{0};
std::atomic<unsigned> g_threadLines{0};
std::atomic<unsigned> g_scopeLines{0};
std::atomic<bool> g_roLogged{false};
std::atomic<std::uint32_t> g_polls{0}, g_loads{0}, g_scopedLoads{0}, g_passes{0}, g_rowsWritten{0}, g_rowsLeft{0}, g_skipped{0}, g_shutdowns{0};
std::atomic<std::uint32_t> g_rechecks{0}, g_unscoped{0}, g_rowsRestored{0};
std::atomic<long> g_tid[3] = {{0}, {0}, {0}};  // the last thread seen in each role (tick, loader, shutdown)
double (*g_clock)() = nullptr;
void (*g_hook)(int) = nullptr;

// The table lock. Held across the real CR15NetBulletCS::Shutdown (which frees the table and clears the global), and across every row write. A plain POD
// (no constructor at load).
pthread_mutex_t g_tableMu = PTHREAD_MUTEX_INITIALIZER;
ThreadWord g_tShutdownDepth;  // per thread, how deep this thread is inside the Shutdown handler (it holds the lock); a key, not thread_local: no allocation in a hooked call

bool g_tableLineDone = false;  // the count line was logged; from then on IdsTick
bool g_tableGone = false;
double g_t0 = 0, g_seen = 0, g_next = 0, g_waitT0 = 0;
std::uintptr_t g_waitData = 0;   // the unsorted table we are waiting on (0 = none)
bool g_waitLogged = false;
std::uintptr_t g_sortedData = 0;  // the sorted table seen at the previous poll
std::uint64_t g_sortedCount = 0;
std::uintptr_t g_unsData = 0;     // the unsorted table seen at the previous poll
std::uint64_t g_unsCount = 0;

double Now() noexcept {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

void Hook(int stage) noexcept {
  if (g_hook != nullptr) g_hook(stage);
}

// A line inside a per-process budget; the first line over it is one Warning that says so, then silence.
bool Budget(std::atomic<unsigned>* used, unsigned cap, const char* what) noexcept {
  const unsigned n = used->fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == cap + 1) {
    sentinel::LogFields(LogLevel::kWarn, "combat_impactsnd_log_cap",
                        {{"lines", what}, {"cap", cap}, {"msg", "further lines of this kind are not written; the counters in the module stats still count"}});
  }
  return n <= cap;
}

// Which thread runs the tick, the loader handler and the Shutdown handler: one line the first time a role is seen on a thread (and again when it
// moves), naming the last thread seen in each role, so one headset run shows whether they share a thread.
enum Role { kRoleTick = 0, kRoleLoader = 1, kRoleShutdown = 2 };
void NoteThread(Role role) noexcept {
  const long tid = static_cast<long>(syscall(SYS_gettid));
  if (g_tid[role].exchange(tid, std::memory_order_relaxed) == tid) return;
  if (!Budget(&g_threadLines, kThreadLines, "threads")) return;
  static const char* const kNames[3] = {"tick", "loader", "shutdown"};
  sentinel::LogFields(LogLevel::kInfo, "combat_impactsnd_thread",
                      {{"role", kNames[role]}, {"tid", tid}, {"tick_tid", g_tid[0].load(std::memory_order_relaxed)},
                       {"loader_tid", g_tid[1].load(std::memory_order_relaxed)}, {"shutdown_tid", g_tid[2].load(std::memory_order_relaxed)},
                       {"msg", "a tid of 0 means that role has not been seen yet"}});
}

ModuleResult Refuse(const char* reason, std::uintptr_t va, std::uint64_t have, std::uint64_t want) noexcept {
  char a[19], h[19], w[19];
  sentinel::LogFields(LogLevel::kError, "combat_impactsnd_refused",
                      {{"reason", reason}, {"vaddr", sentinel::HexString(a, va)}, {"read", sentinel::HexString(h, have)},
                       {"pinned", sentinel::HexString(w, want)}, {"written", 0},
                       {"msg", "impact sounds stay off: the table is not touched and the game keeps its own rows"}});
  return {false, reason};
}

// map / data / count / flags, all readable, data != 0 and 8-aligned
bool ReadTable(std::uintptr_t* map, std::uintptr_t* data, std::uint64_t* count, std::uint32_t* flags) noexcept {
  return SafeRead(g_base + kSoundSettingsVa, map, sizeof *map) && *map != 0 && SafeRead(*map, data, sizeof *data) && *data != 0 &&
         (*data & 7) == 0 && SafeRead(*map + kMapCount, count, sizeof *count) && SafeRead(*map + kMapFlags, flags, sizeof *flags);
}

std::uint32_t WantId(std::uint64_t key) noexcept {
  for (const auto& r : kIds)
    if (r.key == key) return r.id;
  return 0;
}

// one of kIds' ids (what only this module writes; the stock rows hold kStockId)
bool KnownId(std::uint32_t id) noexcept {
  for (const auto& r : kIds)
    if (r.id == id) return true;
  return false;
}

// a known row this module writes: it holds the stock id, or another known row's id (a store into an unsorted table that the first hit's sort moved under
// it lands in a neighbour row; the next pass puts the right id back)
bool NeedsWrite(std::uint32_t id, std::uint32_t want) noexcept { return want != 0 && id != want && (id == kStockId || KnownId(id)); }

bool AllKeysKnown(std::uintptr_t data, std::uint64_t count) noexcept {
  for (std::uint64_t i = 0; i < count; ++i) {
    std::uint64_t key = 0;
    if (!SafeRead(data + 16 * i, &key, sizeof key) || WantId(key) == 0) return false;
  }
  return true;
}

enum class Pass { kNothing, kDone, kSkipped };  // nothing to write / a pass ran / the table lock or the table was not ours

struct Held {  // the table lock, released at scope end
  bool on = false;
  ~Held() {
    if (on) pthread_mutex_unlock(&g_tableMu);
  }
};

struct PassStats {
  unsigned known = 0, wrote = 0, right = 0, left = 0;
  bool log = true;
  char found[sizeof kIds / sizeof kIds[0] * 9 + 1] = "";
};

// The row loop. `ro`: read and count only. When it writes, the caller holds the table lock and has re-validated the table. One aligned u32 store at
// row+8, read back; keys, order and the pad are never touched.
Pass ScanRows(std::uintptr_t data, std::uint64_t count, bool ro, PassStats* st) noexcept {
  bool work = false;
  for (std::uint64_t i = 0; i < count && !work; ++i) {
    std::uint64_t key = 0;
    std::uint32_t id = 0;
    work = SafeRead(data + 16 * i, &key, sizeof key) && SafeRead(data + 16 * i + 8, &id, sizeof id) && NeedsWrite(id, WantId(key));
  }
  if (!work) return Pass::kNothing;
  if (ro && g_roLogged.load(std::memory_order_relaxed)) {  // read-only: the read is logged once
    st->log = false;
    return Pass::kDone;
  }
  g_roLogged.store(ro, std::memory_order_relaxed);
  std::size_t at = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    const std::uintptr_t row = data + 16 * i;
    std::uint64_t key = 0;
    std::uint32_t id = 0;
    const std::uint32_t want = SafeRead(row, &key, sizeof key) && SafeRead(row + 8, &id, sizeof id) ? WantId(key) : 0;
    if (want == 0) continue;
    ++st->known;
    if (at + 10 <= sizeof st->found)
      at += static_cast<std::size_t>(std::snprintf(st->found + at, sizeof st->found - at, "%s%08x", at != 0 ? "," : "", id));
    if (id == want) {
      ++st->right;
      continue;
    }
    if (!NeedsWrite(id, want) || ro) {
      ++st->left;
      continue;
    }
    __atomic_store_n(reinterpret_cast<std::uint32_t*>(row + 8), want, __ATOMIC_RELEASE);
    Hook(2);
    std::uint32_t back = 0;
    if (SafeRead(row + 8, &back, sizeof back) && back == want) ++st->wrote;
    else ++st->left;
  }
  return Pass::kDone;
}

void SkipNote(const char* why, std::uintptr_t data, std::uint64_t count, const char* when) noexcept {
  g_skipped.fetch_add(1, std::memory_order_relaxed);
  if (!Budget(&g_noteLines, kNoteLines, "notes")) return;
  char d[19];
  sentinel::LogFields(LogLevel::kInfo, "combat_impactsnd_row_pass_skipped",
                      {{"why", why}, {"when", when}, {"table", sentinel::HexString(d, data)}, {"rows", count},
                       {"msg", "nothing was written in this pass; the next poll decides again"}});
}

// One pass over the table (map, data, count) the caller validated. A write needs the table lock (try only: a thread that is in the Shutdown handler holds
// it while the table is freed, and the tick must never wait) and the global table pointer, the data pointer and the count read again under it, still the
// ones validated: otherwise nothing is written (kSkipped, the caller retries). The Shutdown handler takes the same lock, so while it is held the table
// cannot be freed. No logging under the lock.
Pass RowPass(std::uintptr_t map, std::uintptr_t data, std::uint64_t count, const char* when) noexcept {
  const bool ro = !g_write || !g_writersOk.load(std::memory_order_acquire);
  PassStats st;
  Pass res = Pass::kNothing;
  const char* skip = nullptr;
  if (ro) {
    res = ScanRows(data, count, true, &st);
  } else {
    Hook(4);
    Held held;
    if (pthread_mutex_trylock(&g_tableMu) != 0) {
      skip = "table lock busy (bullet Shutdown running)";
    } else {
      held.on = true;
      std::uintptr_t m2 = 0, d2 = 0;
      std::uint64_t c2 = 0;
      if (!SafeRead(g_base + kSoundSettingsVa, &m2, sizeof m2) || m2 != map || !SafeRead(m2, &d2, sizeof d2) || d2 != data ||
          !SafeRead(m2 + kMapCount, &c2, sizeof c2) || c2 != count)
        skip = "table changed or freed before the write";
      else res = ScanRows(data, count, false, &st);
    }
  }
  if (skip != nullptr) {
    SkipNote(skip, data, count, when);
    return Pass::kSkipped;
  }
  if (res != Pass::kDone || !st.log) return res;
  g_passes.fetch_add(1, std::memory_order_relaxed);
  g_rowsWritten.fetch_add(st.wrote, std::memory_order_relaxed);
  g_rowsLeft.fetch_add(st.left, std::memory_order_relaxed);
  if (Budget(&g_writeLines, kWriteLines, "ids")) {
    char t[19];
    sentinel::LogFields(LogLevel::kInfo, "combat_impactsnd_ids",
                        {{"when", when}, {"table", sentinel::HexString(t, data)}, {"rows", count}, {"known", st.known}, {"rewrote", st.wrote},
                         {"right", st.right}, {"left", st.left}, {"found_ids", st.found},
                         {"mode", !g_write ? "read_only_switch" : ro ? "read_only_no_shutdown_handler" : "written"}});
  }
  return Pass::kDone;
}

void NoTableNote(std::uintptr_t map, std::uintptr_t data, std::uint64_t count, const char* where) noexcept {
  if (!Budget(&g_noteLines, kNoteLines, "notes")) return;
  char m[19], d[19];
  sentinel::LogFields(LogLevel::kInfo, "combat_impactsnd_no_table",
                      {{"where", where}, {"map", sentinel::HexString(m, map)}, {"data", sentinel::HexString(d, data)}, {"count", count},
                       {"msg", "no impact table now (a level change?); watching for the next one"}});
}

// The table's word-patch state as the row count shows it: the stock loader builds 5 rows, the rebuilt loader builds 15.
const char* PatchState(std::uint64_t count) noexcept {
  if (count == 0) return "no_verdict";
  if (count == kRowCount) return "in_effect";
  if (count == kPlayerRows) return "absent";
  return "unexpected";
}

// Straight after the loader returns, on the loader's thread: the table is complete and (the stock loader) unsorted, no hit has used it yet. Same rule as
// the tick's early pass: an unsorted table is written only when every key is a known one.
void AfterLoad() noexcept {
  std::uintptr_t map = 0, data = 0;
  std::uint64_t count = 0;
  std::uint32_t flags = 0;
  if (!ReadTable(&map, &data, &count, &flags) || count == 0 || count > kRowMax) {
    NoTableNote(map, data, count, "after LoadSoundSettings");
    return;
  }
  if (Budget(&g_noteLines, kNoteLines, "notes")) {
    char d[19], fl[16];
    std::snprintf(fl, sizeof fl, "%08x", flags);
    sentinel::LogFields(LogLevel::kInfo, "combat_impactsnd_load",
                        {{"table", sentinel::HexString(d, data)}, {"rows", count}, {"flags", fl}, {"word_patch", PatchState(count)}});
  }
  const bool unsorted = (flags & 1) != 0;
  if (unsorted && !AllKeysKnown(data, count)) return;  // the tick decides (it logs the skip)
  const Pass r = RowPass(map, data, count, unsorted ? "load/unsorted" : "load/sorted");
  if (unsorted && r != Pass::kSkipped) g_unsDone.store(data, std::memory_order_relaxed);
}

// The Wwise event id: FNV-1 32 of the lowercase name (the kIds ids). A null name is the empty name.
std::uint32_t WwiseEventId(const char* name) noexcept {
  std::uint32_t h = 2166136261u;
  for (const char* p = name != nullptr ? name : ""; *p != 0; ++p) {
    h *= 16777619u;
    const char c = (*p >= 'A' && *p <= 'Z') ? static_cast<char>(*p - 'A' + 'a') : *p;
    h ^= static_cast<std::uint8_t>(c);
  }
  return h;
}

void* g_callee[7] = {};  // tests: a host function in place of the stock loader's stub at this index (RebuildIdx)

template <typename Fn>
Fn Callee(std::size_t idx) noexcept {
  return reinterpret_cast<Fn>(g_callee[idx] != nullptr ? reinterpret_cast<std::uintptr_t>(g_callee[idx]) : g_base + kRebuildCallees[idx].elf);
}

// The first callee whose stub is not the pinned one (its name), or nullptr when all of them are.
const char* FirstMissingCallee(std::uintptr_t base) noexcept {
  for (const RebuildCallee& c : kRebuildCallees) {
    std::uint32_t w = 0;
    if (!SafeRead(base + c.elf, &w, sizeof w) || w != c.firstWord) return c.name;
  }
  return nullptr;
}

// One row onto the table at `table` (kSoundSettings' pointee, or +0x40 for the bullet_whizzby table): the stock append, field for field. Words: data +0,
// growth +0x20, capacity +0x28, count +0x30; flags (u32) +0x38, bit 0 = sort pending. The table lock is held by the caller.
void AppendRow(std::uintptr_t table, std::uint64_t key, std::uint32_t id) noexcept {
  auto* words = reinterpret_cast<std::uint64_t*>(table);
  std::uint64_t cap = words[5];
  std::uint64_t count = words[6];
  if (cap <= count) {
    const std::uint64_t grow = std::max(cap, words[4]);
    Callee<MemBlockResizeFn>(kIdxResize)(reinterpret_cast<void*>(table), (grow + cap) * 0x10, 1);
    cap = words[5];  // the stock code reads the capacity and the count again after the call (ldp x9, x8, [x26, #0x28])
    count = words[6];
    words[5] = cap + grow;
  }
  words[6] = count + 1;
  std::uint8_t* row = reinterpret_cast<std::uint8_t*>(words[0] + count * 0x10);
  std::memcpy(row, &key, sizeof key);
  std::memcpy(row + 8, &id, sizeof id);
  *reinterpret_cast<std::uint32_t*>(table + 0x38) |= 1;
}

// The rebuilt LoadSoundSettings body, into the table at `map`. Returns the file's result (0 read and walked, else the LoadFromProjectSourceDb result,
// which the stock loader returns too); `rows` counts the appends. The table lock is held.
int RebuildSoundSettings(std::uintptr_t map, unsigned* rows) noexcept {
  alignas(16) unsigned char json[16];  // the stock frame holds the CJson in these 16 bytes (sp+0x210..0x21f)
  char file[sizeof kJsonFile];
  std::memcpy(file, kJsonFile, sizeof file);
  Callee<JsonCtorFn>(kIdxCtor)(json);
  const int rc = Callee<JsonLoadFn>(kIdxLoad)(json, file);
  if (rc == 0) {
    const auto next = Callee<JsonNextFn>(kIdxNext);
    const auto nameAt = Callee<JsonNameAtFn>(kIdxNameAt);
    const auto tstring = Callee<JsonTStringFn>(kIdxTString);
    // bullet_impact: one entry per gun; each member of a gun is a row, key = CSymbol64(member) ^ CSymbol64(gun)
    for (void* gunNode = next(json, kImpactPath, nullptr); gunNode != nullptr; gunNode = next(json, kImpactPath, gunNode)) {
      const char* gun = nameAt(json, gunNode);
      const std::uint64_t gunSym = EchoVR::CSymbol64Hash(gun);
      char path[512];
      std::snprintf(path, sizeof path, "%s|%s", kImpactPath, gun);
      for (void* member = next(json, path, nullptr); member != nullptr; member = next(json, path, member)) {
        const std::uint64_t key = EchoVR::CSymbol64Hash(nameAt(json, member)) ^ gunSym;
        AppendRow(map, key, WwiseEventId(tstring(json, member, "")));
        ++*rows;
      }
    }
    // bullet_whizzby: one entry each, key = CSymbol64(name) with no XOR
    for (void* node = next(json, kWhizzbyPath, nullptr); node != nullptr; node = next(json, kWhizzbyPath, node)) {
      AppendRow(map + kWhizzbyOff, EchoVR::CSymbol64Hash(nameAt(json, node)), WwiseEventId(tstring(json, node, "")));
      ++*rows;
    }
  }
  Callee<JsonDtorFn>(kIdxDtor)(json);
  return rc;
}

struct RebuildResult {
  int rc = 0;
  unsigned rows = 0;
  std::uintptr_t map = 0;  // the table the rows went into (0: none was there)
};

// The rebuilt loader under the table lock: the table the global holds now, read under the lock, is the one built into. Nothing is logged under the lock.
// No object with a destructor lives across the game calls (callback_thunk.h rule 1).
RebuildResult RebuildUnderLock() noexcept {
  RebuildResult out;
  pthread_mutex_lock(&g_tableMu);
  std::uintptr_t map = 0;
  if (SafeRead(g_base + kSoundSettingsVa, &map, sizeof map) && map != 0) {
    out.map = map;
    out.rc = RebuildSoundSettings(map, &out.rows);
  }
  pthread_mutex_unlock(&g_tableMu);
  return out;
}

// What the loaded levels say about this load (combat_level.h). The combat lobby counts with the combat maps: nothing of arena's is in it. An Echo Arena
// level that is loaded wins, the lobbies arena players use are no class, and an unreadable game is no level: every one of those is the game's own loader.
enum class Scope { kCombat, kOtherLevel, kNoGame };
Scope DecideScope() noexcept {
  const std::uintptr_t game = g_game.load(std::memory_order_acquire);
  if (game == 0) return Scope::kNoGame;
  std::uint64_t level = 0;
  return level::Classify(game, /*lobby=*/true, &level, /*inFlight=*/true) == level::Kind::kCombatOrLobby ? Scope::kCombat : Scope::kOtherLevel;
}

Scope g_unscopeWhy = Scope::kOtherLevel;  // what the recheck that ended the scope saw (tick thread only)

void NoteScope(Scope scope, const char* loader) noexcept {
  if (!Budget(&g_scopeLines, kScopeLines, "scope")) return;
  sentinel::LogFields(LogLevel::kInfo, "combat_impactsnd_scope",
                      {{"where", "load"}, {"scope", scope == Scope::kCombat ? "combat" : scope == Scope::kOtherLevel ? "other_level" : "no_game"},
                       {"loader", loader},
                       {"msg", scope == Scope::kCombat ? "a combat level is loaded: this load's impact table is repaired"
                                                       : "no combat level is loaded: the game's own loader runs and the table is left as the game made it"}});
}

// The handler on LoadSoundSettings (the JUMP_SLOT): the loader takes no argument (it reads none before its first clobber of x0, disassembly
// 0x2221d1c..0x2221d20) and returns w0 = w19 (0x2222008), the caller's next call may see it in x0, so the whole return value is passed through. In a combat
// level with the rebuild on (g_rebuild) the rebuilt loader runs in place of the stock one; in a combat level otherwise the stock loader runs and the
// after-load pass reads what it built; in every other level the stock loader runs and nothing else.
std::uint64_t OnLoad(LoadThunk::Fn original) noexcept {
  g_loads.fetch_add(1, std::memory_order_relaxed);
  const Scope scope = DecideScope();
  if (scope != Scope::kCombat) {
    // This load is the game's own, and the table it fills is not a combat level's: the scope flag is cleared here, so the tick does not write ids into this
    // table. Through the game's one caller the flag is already clear: Initialize makes a new table and loads it only when kBulletSystemCount is 0
    // (disassembly 0x22217b8..0x22217bc, 0x2221840, 0x2221844), after the last Shutdown freed the old one (0x22227f8). The clear is the fail-closed answer
    // for a load that reaches this handler another way while a combat table lives.
    g_scoped.store(false, std::memory_order_release);
    g_unscoping.store(false, std::memory_order_release);
    g_rebuiltMap.store(0, std::memory_order_release);
    const std::uint64_t r = original();
    NoteScope(scope, "game");
    return r;
  }
  g_scopedLoads.fetch_add(1, std::memory_order_relaxed);
  if (g_rebuild.load(std::memory_order_acquire)) {
    NoteThread(kRoleLoader);
    const RebuildResult r = RebuildUnderLock();
    NoteScope(scope, "rebuilt");
    if (Budget(&g_noteLines, kNoteLines, "notes")) {
      char d[19];
      sentinel::LogFields(LogLevel::kInfo, "combat_impactsnd_rebuilt",
                          {{"table", sentinel::HexString(d, r.map)}, {"rows", r.rows}, {"json_result", r.rc}, {"loader", "reconstructed"}});
    }
    AfterLoad();
    g_rebuiltMap.store(r.map, std::memory_order_release);
    g_unscoping.store(false, std::memory_order_release);
    g_scoped.store(true, std::memory_order_release);
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(r.rc));
  }
  const std::uint64_t r = original();
  NoteThread(kRoleLoader);
  NoteScope(scope, "stock");
  AfterLoad();
  g_rebuiltMap.store(0, std::memory_order_release);
  g_unscoping.store(false, std::memory_order_release);
  g_scoped.store(true, std::memory_order_release);
  return r;
}

// The handler on CR15NetBulletCS::Shutdown (the vtable word), a virtual method: x0 = this, read first (`mov x19, x0` at 0x2222700), and the method ends in
// a tail call (`b 0x010190d0` at 0x2222848) so x0 is its return. The real Shutdown frees kSoundSettings and clears the global when the last system leaves
// (disassembly 0x22227ec blr the allocator's free, 0x22227f8 `str xzr, [x24]`): it runs with the table lock held, so no row write is in flight while it
// does, and none starts until it has returned. A nested call on the same thread does not take the lock again. When the global reads 0 afterwards the table
// is gone, and so is the scope.
std::uint64_t OnShutdown(ShutdownThunk::Fn original, std::uint64_t self) noexcept {
  if (g_scoped.load(std::memory_order_relaxed)) NoteThread(kRoleShutdown);  // no syscall in an arena teardown
  const std::uintptr_t depth = g_tShutdownDepth.Get();
  const bool outer = depth == 0;
  g_tShutdownDepth.Set(depth + 1);
  if (outer) pthread_mutex_lock(&g_tableMu);
  const std::uint64_t r = original(self);
  if (outer) pthread_mutex_unlock(&g_tableMu);
  g_tShutdownDepth.Set(depth);
  g_shutdowns.fetch_add(1, std::memory_order_relaxed);
  if (outer && g_scoped.load(std::memory_order_acquire)) {  // no table of a combat level: no read (an arena match makes none)
    std::uintptr_t map = 0;
    SafeRead(g_base + kSoundSettingsVa, &map, sizeof map);
    if (map == 0) {  // the table is gone: nothing is left to put back
      g_scoped.store(false, std::memory_order_release);
      g_unscoping.store(false, std::memory_order_release);
      g_rebuiltMap.store(0, std::memory_order_release);
    }
    if (Budget(&g_noteLines, kNoteLines, "notes")) {
      char m[19];
      sentinel::LogFields(LogLevel::kInfo, "combat_impactsnd_shutdown_returned", {{"table_global_now", sentinel::HexString(m, map)}});
    }
  }
  return r;
}

NEVR_HOOK_RECORD(kLoadRecord, LoadThunk, &OnLoad);
NEVR_HOOK_RECORD(kShutdownRecord, ShutdownThunk, &OnShutdown);

// The tick's watch over every table instance of the process. A write needs the same data pointer and count reading the same on two consecutive polls
// (kPollS apart), and a re-read right before the rows (then the table lock and a third read in RowPass). An unsorted table is written once per table, only
// when every key is a known one; a sorted one whenever a known row needs it.
void IdsTick(double now) noexcept {
  std::uintptr_t map = 0, data = 0;
  std::uint64_t count = 0;
  std::uint32_t flags = 0;
  const bool read = ReadTable(&map, &data, &count, &flags);
  if (!read || count == 0 || count > kRowMax) {
    if (!g_tableGone) NoTableNote(map, data, count, "tick");
    g_tableGone = true;
    g_waitData = 0;
    g_sortedData = 0;
    g_unsData = 0;
    g_unsDone.store(0, std::memory_order_relaxed);  // a new table at the same address is new
    return;
  }
  g_tableGone = false;
  if ((flags & 1) != 0) {  // sort still pending: the first hit sorts it
    g_sortedData = 0;
    if (g_waitData != data) {
      g_waitData = data;
      g_waitLogged = false;
      g_waitT0 = now;
    }
    if (!g_waitLogged && now - g_waitT0 >= kWaitS) {
      g_waitLogged = true;
      if (Budget(&g_noteLines, kNoteLines, "notes")) {
        char d[19];
        sentinel::LogFields(LogLevel::kInfo, "combat_impactsnd_unsorted_wait",
                            {{"table", sentinel::HexString(d, data)}, {"rows", count}, {"unsorted_for_s", static_cast<long long>(now - g_waitT0)},
                             {"msg", "the table is still unsorted (no hit has used it); still watching"}});
      }
    }
    const bool same = data == g_unsData && count == g_unsCount;
    g_unsData = data;
    g_unsCount = count;
    if (!same || g_unsDone.load(std::memory_order_relaxed) == data) return;
    Hook(1);
    std::uintptr_t map2 = 0, data2 = 0;
    std::uint64_t count2 = 0;
    std::uint32_t flags2 = 0;
    if (!ReadTable(&map2, &data2, &count2, &flags2) || map2 != map || data2 != data || count2 != count || (flags2 & 1) == 0)
      return;  // changed or sorted under us: the next polls decide
    if (AllKeysKnown(data, count)) {
      const Pass r = RowPass(map, data, count, "tick/unsorted");
      if (r != Pass::kSkipped) g_unsDone.store(data, std::memory_order_relaxed);
    } else {
      g_unsDone.store(data, std::memory_order_relaxed);
      if (Budget(&g_noteLines, kNoteLines, "notes")) {
        char d[19];
        sentinel::LogFields(LogLevel::kInfo, "combat_impactsnd_unsorted_foreign_keys",
                            {{"table", sentinel::HexString(d, data)}, {"rows", count},
                             {"msg", "the unsorted table holds keys outside the 15 known rows: it is not written until it is sorted"}});
      }
    }
    return;
  }
  g_waitData = 0;
  g_unsData = 0;
  const bool stable = data == g_sortedData && count == g_sortedCount;
  g_sortedData = data;
  g_sortedCount = count;
  if (!stable) return;
  Hook(1);
  std::uintptr_t map2 = 0, data2 = 0;
  std::uint64_t count2 = 0;
  std::uint32_t flags2 = 0;
  if (!ReadTable(&map2, &data2, &count2, &flags2) || map2 != map || data2 != data || count2 != count || (flags2 & 1) != 0)
    return;  // changed under us: the next polls decide again
  RowPass(map, data, count, "tick/sorted");
}

// The stock id back into one table's rows (data, count): every row when `all` (a table the rebuilt loader filled, where every id is ours), else the rows
// that hold one of kIds' ids (the ones this module wrote). Keys, order, count and pads are never touched: the game's hit code reads the table without our
// lock, so a row is changed only by one aligned u32 store. The caller holds the table lock and validated the table.
unsigned StockIds(std::uintptr_t data, std::uint64_t count, bool all) noexcept {
  unsigned put = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    std::uint32_t id = 0;
    if (!SafeRead(data + 16 * i + 8, &id, sizeof id) || id == kStockId || (!all && !KnownId(id))) continue;
    __atomic_store_n(reinterpret_cast<std::uint32_t*>(data + 16 * i + 8), kStockId, __ATOMIC_RELEASE);
    ++put;
  }
  return put;
}

// No combat level is loaded any more while a table of one is still live: the game made the table for the first bullet system and frees it with the last
// (docs/quest/combat/impactsnd.md, "Scope and arena cost"), so a game space that starts while a combat one lives, an Echo Arena level among them, shares
// it. The rows this module wrote get the stock id back, so every hit plays what the stock table plays (id 1), and the scope ends: the tick stops polling.
// The table keeps its rows and count (no row is added, removed or moved). A busy lock (a Shutdown is running) or a table that changed is tried again on
// the next frame. Returns whether the scope ended.
bool Unscope(Scope why) noexcept {
  std::uintptr_t map = 0, data = 0;
  std::uint64_t count = 0;
  std::uint32_t flags = 0;
  unsigned rows = 0, whizzby = 0;
  const bool table = ReadTable(&map, &data, &count, &flags) && count != 0 && count <= kRowMax;
  const bool writes = g_write && g_writersOk.load(std::memory_order_acquire);  // read-only: no id of ours was ever written
  if (table && writes) {
    Hook(5);
    if (pthread_mutex_trylock(&g_tableMu) != 0) return false;
    std::uintptr_t m2 = 0, d2 = 0;
    std::uint64_t c2 = 0;
    if (!SafeRead(g_base + kSoundSettingsVa, &m2, sizeof m2) || m2 != map || !SafeRead(m2, &d2, sizeof d2) || d2 != data ||
        !SafeRead(m2 + kMapCount, &c2, sizeof c2) || c2 != count) {
      pthread_mutex_unlock(&g_tableMu);
      return false;
    }
    const bool rebuilt = map == g_rebuiltMap.load(std::memory_order_acquire);
    rows = StockIds(data, count, rebuilt);
    std::uintptr_t wData = 0;
    std::uint64_t wCount = 0;
    if (rebuilt && SafeRead(map + kWhizzbyOff, &wData, sizeof wData) && wData != 0 && (wData & 7) == 0 &&
        SafeRead(map + kWhizzbyOff + kMapCount, &wCount, sizeof wCount) && wCount <= kRowMax)
      whizzby = StockIds(wData, wCount, true);
    pthread_mutex_unlock(&g_tableMu);
  }
  g_scoped.store(false, std::memory_order_release);  // the scope ends: the tick stops polling
  g_unscoping.store(false, std::memory_order_release);
  g_rebuiltMap.store(0, std::memory_order_release);
  g_unscoped.fetch_add(1, std::memory_order_relaxed);
  g_rowsRestored.fetch_add(rows + whizzby, std::memory_order_relaxed);
  if (Budget(&g_scopeLines, kScopeLines, "scope")) {
    char t[19];
    sentinel::LogFields(LogLevel::kInfo, "combat_impactsnd_unscoped",
                        {{"scope", why == Scope::kOtherLevel ? "other_level" : "no_game"}, {"table", sentinel::HexString(t, table ? data : 0)},
                         {"rows_stock", rows}, {"whizzby_stock", whizzby}, {"mode", !table ? "no_table" : writes ? "written" : "read_only"},
                         {"msg", "no combat level is loaded any more: every id this module put into the shared impact table is the stock id again, "
                                 "so the level now loaded plays what stock plays, and the tick stops polling the table"}});
  }
  return true;
}

ModuleResult InstallHandlers(sentinel::ImageLookup lookup, std::uintptr_t base) noexcept {
  // The Shutdown word first: the row writes are enabled only with it in, and the decals module chains above it.
  ShutdownThunk::Arm(kShutdownRecord);
  const sentinel::GotStatus shutdownStatus = sentinel::InstallThunk<ShutdownThunk>(g_shutdownHook, ShutdownTarget(base), lookup);
  const bool shutdownOk = shutdownStatus == sentinel::GotStatus::kOk;
  g_writersOk.store(shutdownOk, std::memory_order_release);
  sentinel::LogFields(shutdownOk ? LogLevel::kInfo : LogLevel::kError, "combat_impactsnd_hook",
                      {{"hook", "shutdown"}, {"status", sentinel::GotStatusName(shutdownStatus)}, {"slot", "vtable_word"},
                       {"row_writes", shutdownOk ? "enabled" : "disabled"},
                       {"msg", shutdownOk ? "the Shutdown handler holds the table lock while the game frees the table, so row writes are safe"
                                          : "without the Shutdown handler no row is written: the module reads and logs only"}});
  // The rebuilt loader writes the table under the same lock, so it runs only with the Shutdown handler in, every stub pinned, and writes switched on.
  const char* missing = FirstMissingCallee(base);
  const bool rebuild = missing == nullptr && shutdownOk && g_write;
  g_rebuild.store(rebuild, std::memory_order_release);
  sentinel::LogFields(rebuild || missing == nullptr ? LogLevel::kInfo : LogLevel::kError, "combat_impactsnd_rebuild",
                      {{"mode", rebuild ? "reconstructed" : "stock"},
                       {"why", rebuild ? "every_stub_pinned" : missing != nullptr ? "callee_stub_not_pinned" : !g_write ? "writes_switched_off" : "no_shutdown_handler"},
                       {"missing_callee", missing != nullptr ? missing : "none"}});
  LoadThunk::Arm(kLoadRecord);
  const sentinel::GotStatus loadStatus = sentinel::InstallThunk<LoadThunk>(g_loadHook, LoadTarget(base), lookup);
  const bool loadOk = loadStatus == sentinel::GotStatus::kOk;
  g_loadHooked = loadOk;
  sentinel::LogFields(loadOk ? LogLevel::kInfo : LogLevel::kError, "combat_impactsnd_hook",
                      {{"hook", "load"}, {"status", sentinel::GotStatusName(loadStatus)}, {"slot", "jump_slot"},
                       {"mode", loadOk ? "after_load_pass_and_tick" : "tick_only"}});
  const bool ok = shutdownOk && loadOk;
  const char* status = ok ? "installed" : !shutdownOk ? sentinel::GotStatusName(shutdownStatus) : sentinel::GotStatusName(loadStatus);
  sentinel::LogFields(ok ? LogLevel::kInfo : LogLevel::kError, "combat_impactsnd_install",
                      {{"result", ok ? "installed" : "refused"}, {"status", status}, {"shutdown", sentinel::GotStatusName(shutdownStatus)},
                       {"load", sentinel::GotStatusName(loadStatus)}, {"row_writes", shutdownOk && g_write ? "enabled" : "disabled"},
                       {"rebuild", rebuild ? "reconstructed" : "stock"}, {"switch", "combat.impactsnd"},
                       {"msg", ok ? "bullet impact sounds are repaired in combat levels only; an arena match keeps the game's own table"
                                  : "a hook was refused: the part of the module that depends on it is off and the game keeps its own behaviour there"}});
  return {ok, status};
}

}  // namespace

const Row* Rows(std::size_t* count) noexcept {
  *count = sizeof kIds / sizeof kIds[0];
  return kIds;
}

ModuleResult InstallAt(bool on, bool write, sentinel::ImageLookup lookup) noexcept {
  if (!on) {
    sentinel::LogFields(LogLevel::kInfo, "combat_impactsnd_install",
                        {{"result", "off"}, {"reason", "off_config"}, {"switch", "combat.impactsnd"},
                         {"msg", "combat.impactsnd is false: the game keeps its own impact table and sounds"}});
    return {false, "off_config", true};
  }
  g_write = write;
  sentinel::ElfImage image{};
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return Refuse("module_not_loaded", 0, 0, 0);
  const std::uintptr_t base = image.base;
  g_base = base;
  g_t0 = Now();
  // The hit code the module's reasoning is about. A word that differs = not the pinned build: nothing is written.
  for (const CodeWord& word : kCode) {
    std::uint32_t have = 0;
    if (!SafeRead(base + word.va, &have, sizeof have) || have != word.want) return Refuse("hit_code_word_mismatch", word.va, have, word.want);
  }
  // The relocated GLOB_DAT word of kSoundSettings must point at the .bss object this module reads.
  std::uintptr_t got = 0;
  if (!SafeRead(base + kSoundSettingsGotVa, &got, sizeof got) || got != base + kSoundSettingsVa)
    return Refuse("ksoundsettings_got_mismatch", kSoundSettingsGotVa, got, base + kSoundSettingsVa);
  // D: the library's own static initialiser stores it. This module reads it and writes nothing there: a D that reads 0 means the second lookup of
  // the hit code cannot match, which no row write repairs, so the module is refused.
  std::uint64_t d = 0;
  if (!SafeRead(base + kDefaultSymVa, &d, sizeof d)) return Refuse("default_symbol_unreadable", kDefaultSymVa, 0, kDefaultSym);
  {
    char a[19], v[19];
    sentinel::LogFields(LogLevel::kInfo, "combat_impactsnd_default_symbol",
                        {{"vaddr", sentinel::HexString(a, kDefaultSymVa)}, {"value", sentinel::HexString(v, d)}, {"matches", d == kDefaultSym ? 1 : 0},
                         {"msg", "D is read and never written"}});
  }
  if (d == 0) return Refuse("default_symbol_zero", kDefaultSymVa, d, kDefaultSym);
  // Every check is done: the Shutdown handler keeps its depth per thread in a pthread key (not thread_local: that allocates in a hooked call), made now so
  // that a module refused above holds no key. No key: nothing is hooked.
  if (const int keyError = g_tShutdownDepth.Create(); keyError != 0) {
    sentinel::LogFields(LogLevel::kError, "combat_impactsnd_refused",
                        {{"reason", "no_thread_key"}, {"error", keyError}, {"written", 0},
                         {"msg", "the Shutdown handler's per-thread depth needs a pthread key and the process has none left; impact sounds stay off"}});
    return {false, "no_thread_key"};
  }
  g_on.store(true, std::memory_order_release);
  const ModuleResult result = InstallHandlers(lookup, base);
  tick::Add(&Tick);  // the tick does nothing until a combat level loads a table
  return result;
}

ModuleResult Install(bool on, bool write) noexcept { return InstallAt(on, write, sentinel::FindLoadedImage); }

void Tick(std::uintptr_t game) noexcept {
  if (!g_on.load(std::memory_order_acquire)) return;
  if (g_game.load(std::memory_order_relaxed) != game) g_game.store(game, std::memory_order_release);
  if (!g_loadHooked && !g_scoped.load(std::memory_order_acquire)) {  // no load handler to mark a combat level's table: ask the levels, once a second
    const double at = Now();
    if (at >= g_nextScope) {
      g_nextScope = at + kRecheckS;
      if (DecideScope() == Scope::kCombat) g_scoped.store(true, std::memory_order_release);
    }
  }
  if (!g_scoped.load(std::memory_order_acquire)) return;  // an arena match, or no table of a combat level: one load per frame
  const double now = Now();
  // While a combat level's table is live, once a second: is a combat level still loaded? An arena space that starts while a combat one lives shares the
  // table (no load runs for it), so the answer is asked of the levels, not of the loads. When it is no, the stock ids go back and the scope ends (fail closed).
  if (!g_unscoping.load(std::memory_order_acquire) && now >= g_nextScope) {
    g_nextScope = now + kRecheckS;
    g_rechecks.fetch_add(1, std::memory_order_relaxed);
    const Scope scope = DecideScope();
    if (scope != Scope::kCombat) {
      g_unscopeWhy = scope;
      g_unscoping.store(true, std::memory_order_release);
    }
  }
  if (g_unscoping.load(std::memory_order_acquire)) {
    Unscope(g_unscopeWhy);  // a busy lock: the next frame tries again
    return;
  }
  if (now < g_next) return;
  g_next = now + kPollS;
  g_polls.fetch_add(1, std::memory_order_relaxed);
  NoteThread(kRoleTick);
  if (g_tableLineDone) {
    IdsTick(now);
    return;
  }
  // the count line, once: what the table reads kSettleS after it is first non-zero, or kWaitS after install with what is there
  std::uintptr_t map = 0;
  std::uint64_t count = 0;
  const bool haveMap = SafeRead(g_base + kSoundSettingsVa, &map, sizeof map) && map != 0;
  const bool haveCount = haveMap && SafeRead(map + kMapCount, &count, sizeof count);
  if (haveCount && count != 0) {
    if (g_seen == 0) g_seen = now;
    if (now - g_seen < kSettleS) return;
  } else if (now - g_t0 < kWaitS) {
    return;
  }
  g_tableLineDone = true;
  std::uint64_t d = 0;
  SafeRead(g_base + kDefaultSymVa, &d, sizeof d);
  char dc[19], s[24];
  std::snprintf(s, sizeof s, "%.1f", now - g_t0);
  const bool odd = count != 0 && count != kRowCount && count != kPlayerRows;
  sentinel::LogFields(odd ? LogLevel::kWarn : LogLevel::kInfo, "combat_impactsnd_table",
                      {{"map", haveMap ? "set" : "null"}, {"rows", count}, {"after_s", s}, {"default_symbol", sentinel::HexString(dc, d)},
                       {"word_patch", PatchState(count)}});
}

Stats GetStats() noexcept {
  Stats s;
  s.on = g_on.load();
  s.writersOk = g_writersOk.load();
  s.scoped = g_scoped.load();
  s.polls = g_polls.load();
  s.loads = g_loads.load();
  s.scopedLoads = g_scopedLoads.load();
  s.passes = g_passes.load();
  s.rowsWritten = g_rowsWritten.load();
  s.rowsLeft = g_rowsLeft.load();
  s.skipped = g_skipped.load();
  s.shutdowns = g_shutdowns.load();
  s.rechecks = g_rechecks.load();
  s.unscoped = g_unscoped.load();
  s.rowsRestored = g_rowsRestored.load();
  return s;
}

void SetClockForTest(double (*clock)()) noexcept { g_clock = clock; }
void SetGameForTest(std::uintptr_t game) noexcept { g_game.store(game, std::memory_order_release); }
void SetCalleeForTest(std::size_t index, void* fn) noexcept {
  if (index < sizeof g_callee / sizeof g_callee[0]) g_callee[index] = fn;
}
void SetScopedForTest(bool scoped) noexcept { g_scoped.store(scoped, std::memory_order_release); }
void SetHookForTest(void (*hook)(int stage)) noexcept { g_hook = hook; }

void ResetForTest() noexcept {
  g_loadHook.Forget();
  g_shutdownHook.Forget();
  LoadThunk::Reset();
  ShutdownThunk::Reset();
  g_base = 0;
  g_write = true;
  g_on.store(false);
  g_writersOk.store(false);
  g_rebuild.store(false);
  g_game.store(0);
  std::memset(g_callee, 0, sizeof g_callee);
  g_scoped.store(false);
  g_loadHooked = false;
  g_nextScope = 0;
  g_unscoping.store(false);
  g_rebuiltMap.store(0);
  g_unscopeWhy = Scope::kOtherLevel;
  g_rechecks.store(0);
  g_unscoped.store(0);
  g_rowsRestored.store(0);
  g_unsDone.store(0);
  g_noteLines.store(0);
  g_writeLines.store(0);
  g_threadLines.store(0);
  g_scopeLines.store(0);
  g_roLogged.store(false);
  g_polls.store(0);
  g_loads.store(0);
  g_scopedLoads.store(0);
  g_passes.store(0);
  g_rowsWritten.store(0);
  g_rowsLeft.store(0);
  g_skipped.store(0);
  g_shutdowns.store(0);
  for (auto& t : g_tid) t.store(0);
  g_tShutdownDepth.Set(0);
  g_tableLineDone = g_tableGone = g_waitLogged = false;
  g_t0 = g_seen = g_next = g_waitT0 = 0;
  g_waitData = g_sortedData = g_unsData = 0;
  g_sortedCount = g_unsCount = 0;
  g_hook = nullptr;
  g_clock = nullptr;
}

}  // namespace nevr_quest::combat::impactsnd
