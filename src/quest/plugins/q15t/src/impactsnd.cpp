#include "impactsnd.h"

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "got_swap.h"
#include "nevr/symbol_hash.h"
#include "plugin_log.h"
#include "safe_read.h"

// 1: build without writing anything to the game: D and the rows are read and logged, never written (the source build's
// `nowrite=1`). The wrappers and the tick still run.
#ifndef Q15T_IMPACTSND_NOWRITE
#define Q15T_IMPACTSND_NOWRITE 0
#endif

namespace q15t {
namespace {

// ELF vaddrs of the pinned build (b243509c08ce677aeb95fa348016949b3fc45230); disassembly VA = ELF + 0x100000.
constexpr std::uintptr_t kDefaultSymVa = 0x37af8d0;      // R13 D, .bss (the source build's impactsnd:74)
constexpr std::uintptr_t kSoundSettingsVa = 0x37af6a0;   // R12 kSoundSettings, .bss (the source build's impactsnd:75)
constexpr std::uintptr_t kSoundSettingsGotVa = 0x370fa70;  // R12 GLOB_DAT slot of kSoundSettings (readelf -rW)
constexpr std::uintptr_t kLoadSlotVa = 0x36fa3c0;        // R14 JUMP_SLOT of LoadSoundSettings (readelf -rW)
constexpr std::uintptr_t kLoadFnVa = 0x2121d04;          // R14 LoadSoundSettings (the source build's impactsnd:86)
constexpr std::uintptr_t kShutdownSlotVa = 0x36933e8;    // R17 CR15NetBulletCS::Shutdown vtable word, R_AARCH64_ABS64, RELRO (readelf -rW)
constexpr std::uintptr_t kShutdownFnVa = 0x21226f0;      // R17 CR15NetBulletCS::Shutdown (readelf -sW, disassembly 0x22226f0)
constexpr std::uint64_t kDefaultSym = kImpactDefaultSymbol;
constexpr std::uintptr_t kMapCount = 0x30;               // map+0x30 = row count (the source build's impactsnd:77)
constexpr std::uintptr_t kMapFlags = 0x38;               // bit 0 = sort pending (the source build's impactsnd:100)
constexpr std::uintptr_t kWhizzbyOff = 0x40;             // the second table (bullet_whizzby) sits 0x40 into kSoundSettings

// ---- R16, rebuilt: LoadSoundSettings in C++ (disassembly 0x2221d04, 808 bytes; the stock body is not copied, its steps are).
// The stock loader's row scratch {key, id} sits at sp+0x10 / sp+0x18, the same bytes as the path string it hands to the next
// CJson::Next (MakeTStrPrintF "bullet_impact|<gun>"), so after the first row the path reads a key, and each gun keeps one row.
// The rebuilt loader keeps the path and the row in separate storage, so every member of every gun is appended.
//
// The calls go to the stock loader's own direct-call targets (ELF = disassembly VA - 0x100000). Each is a GOT stub that jumps on
// to the function, the same as the stock `bl`. The first word of each stub is pinned (read from the disassembly, the stub's
// `adrp x16`), and the rebuild runs only when every one matches. Signatures: the disassembly decompiles of CJson::Next(char const*,
// void*) const, CJson::NameAt(void*) const, CJson::TString(void*, char*) const (called with a third argument, the fallback),
// CMemBlock::Resize(ulonglong, uint) and the stock call sites (x0 = this, the arguments in x1, x2) in 0x2221d04..0x2222028.
// CSymbol64 is the SDK's CSymbol64Hash (the module's kIds keys, checked against it by the tests); CSoundSymbol is replaced
// by WwiseEventId, because CalculateSymbolValue returns kStockId for every name on the Quest (RE-28).
struct RebuildCallee {
  const char* name;
  std::uintptr_t elf;
  std::uint32_t first_word;  // the stub's first instruction, little-endian
};
constexpr RebuildCallee kRebuildCallees[] = {
    {"CJson::CJson", 0xed7760, 0x90013f70u},
    {"CJson::~CJson", 0xedcae0, 0xb0013f50u},
    {"CJson::LoadFromProjectSourceDb", 0x0f00140, 0xf0013eb0u},
    {"CJson::Next", 0x0f28490, 0xf0013e10u},
    {"CJson::NameAt", 0x0f17d40, 0x90013e70u},
    {"CJson::TString", 0x0f237d0, 0xd0013e30u},
    {"CMemBlock::Resize", 0x0f3e400, 0x90013dd0u},
};
enum RebuildIdx : std::size_t { kIdxCtor, kIdxDtor, kIdxLoad, kIdxNext, kIdxNameAt, kIdxTString, kIdxResize };
using JsonCtorFn = void (*)(void* self);
using JsonDtorFn = void (*)(void* self);
using JsonLoadFn = int (*)(void* self, const char* path);
using JsonNextFn = void* (*)(void* self, const char* name, const void* from);
using JsonNameAtFn = const char* (*)(void* self, const void* node);
using JsonTStringFn = const char* (*)(void* self, const void* node, const char* fallback);
using MemBlockResizeFn = void (*)(void* self, std::uint32_t bytes, int keep);
constexpr const char* kImpactPath = "bullet_impact";
constexpr const char* kWhizzbyPath = "bullet_whizzby";
// the stock SPrintF("%s|%s", "/json/r14/config", "mp_sound_settings.json")
constexpr char kJsonFile[] = "/json/r14/config|mp_sound_settings.json";

// R15: the hit code that reads the count and D (the source build's impactsnd:78-80). Read-only guards: checked, never written.
struct CodeWord {
  std::uintptr_t va;
  std::uint32_t want;
};
constexpr CodeWord kCode[4] = {
    {0x21208b4, 0xf9401ac1},  // ldr x1, [x22, #0x30]
    {0x21208b8, 0xb4000541},  // cbz x1, <log>
    {0x21208bc, 0xf000b468},  // adrp x8, <D page>
    {0x21208c8, 0xf944691b},  // ldr x27, [x8, #0x8d0]
};

constexpr double kWaitS = 600.0;   // the count line is logged after this long even with no table (the source build's impactsnd:81)
constexpr double kPollS = 0.25;    // the tick reads the table at most this often
constexpr double kSettleS = 2.0;   // the count is logged once it is non-zero for this long
constexpr std::uint64_t kRowCount = 15;  // 5 guns x 3 members (the PC table)
constexpr std::uint64_t kPlayerRows = 5;  // what the stock loader builds on the Quest (only the first member of each gun)
constexpr std::uint32_t kStockId = 1;     // what CalculateSymbolValue returns on the Quest (the source build's impactsnd:101)
constexpr std::uint64_t kRowMax = 64;
constexpr unsigned kNoteLines = 12;     // non-writing chatter lines per process
constexpr unsigned kWriteLines = 200;   // lines of passes that wrote, per process
constexpr unsigned kThreadLines = 24;   // thread-identity lines per process

// the source build's impactsnd:103-119. key = CSymbol64(subkey) ^ CSymbol64(gun); id = FNV-1 32 of the lowercase event name
// (smg uses the _quark events).
constexpr ImpactSndRow kIds[] = {
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

// ---- state. Plain values are written at install (before the wrappers are swapped in) or only on the game thread; what the
// wrappers' threads and the tick share is atomic.
std::uintptr_t g_base = 0;
bool g_nowrite = Q15T_IMPACTSND_NOWRITE != 0;
std::atomic<bool> g_on{false};
std::atomic<bool> g_writers_ok{false};  // the Shutdown wrapper is in: the only state in which a row may be written
std::atomic<bool> g_rebuild{false};     // the loader wrapper runs the rebuilt LoadSoundSettings (R16), not the stock one
std::atomic<std::uintptr_t> g_real{0};
std::atomic<std::uintptr_t> g_real_shutdown{0};
std::atomic<std::uintptr_t> g_uns_done{0};  // the unsorted table already written (once per table): wrapper and tick
std::atomic<unsigned> g_note_lines{0};
std::atomic<unsigned> g_write_lines{0};
std::atomic<unsigned> g_thread_lines{0};
std::atomic<bool> g_ro_logged{false};
std::atomic<std::uint32_t> g_polls{0}, g_loads{0}, g_passes{0}, g_rows_written{0}, g_rows_left{0}, g_skipped{0}, g_shutdowns{0};
std::atomic<long> g_tid[3] = {{0}, {0}, {0}};  // the last thread seen in each role (tick, loader, shutdown)
double (*g_clock)() = nullptr;
void (*g_hook)(int) = nullptr;

// The table lock. Held across the real CR15NetBulletCS::Shutdown (which frees the table and clears the global), and across
// every row write. A plain POD (no constructor at load).
pthread_mutex_t g_table_mu = PTHREAD_MUTEX_INITIALIZER;
thread_local unsigned t_shutdown_depth = 0;  // this thread is inside the Shutdown wrapper (it holds the lock)

bool g_table_line_done = false;  // the count line was logged; from then on IdsTick
bool g_table_gone = false;
double g_t0 = 0, g_seen = 0, g_next = 0, g_wait_t0 = 0;
std::uintptr_t g_wait_data = 0;   // the unsorted table we are waiting on (0 = none)
bool g_wait_logged = false;
std::uintptr_t g_sorted_data = 0;  // the sorted table seen at the previous poll
std::uint64_t g_sorted_count = 0;
std::uintptr_t g_uns_data = 0;     // the unsorted table seen at the previous poll
std::uint64_t g_uns_count = 0;

double Now() {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
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
    Log(NEVR_LOG_WARNING, "impactsnd log cap reached: further lines of this kind are not written", f, 2);
  }
  return n <= cap;
}

// Which thread runs the tick, the loader wrapper and the Shutdown wrapper: one line the first time a role is seen on a thread
// (and again when it moves), naming the last thread seen in each role, so one headset run shows whether they share a thread.
enum Role { kRoleTick = 0, kRoleLoader = 1, kRoleShutdown = 2 };
void NoteThread(Role role) {
  const long tid = static_cast<long>(syscall(SYS_gettid));
  if (g_tid[role].exchange(tid, std::memory_order_relaxed) == tid) return;
  if (!Budget(&g_thread_lines, kThreadLines, "threads")) return;
  static const char* const kNames[3] = {"tick", "loader", "shutdown"};
  char t[24], a[24], b[24], c[24];
  long seen[3];
  for (int i = 0; i < 3; ++i) seen[i] = g_tid[i].load(std::memory_order_relaxed);
  std::snprintf(t, sizeof t, "%ld", tid);
  std::snprintf(a, sizeof a, "%ld", seen[0]);
  std::snprintf(b, sizeof b, "%ld", seen[1]);
  std::snprintf(c, sizeof c, "%ld", seen[2]);
  const NevrLogField f[] = {{"role", kNames[role]}, {"tid", t}, {"tick_tid", a}, {"loader_tid", b}, {"shutdown_tid", c}};
  Log(NEVR_LOG_INFO, "impactsnd thread (0 = role not seen yet)", f, 5);
}

void Refuse(const char* reason, std::uintptr_t va, std::uint64_t have, std::uint64_t want) {
  char a[24], h[24], w[24];
  const NevrLogField f[] = {{"reason", reason}, {"vaddr", Hex(a, sizeof a, va)}, {"read", Hex(h, sizeof h, have)},
                            {"pinned", Hex(w, sizeof w, want)}, {"written", "0"}};
  Log(NEVR_LOG_ERROR, "impactsnd OFF: nothing written", f, 5);
}

// map / data / count / flags, all readable, data != 0 and 8-aligned
bool ReadTable(std::uintptr_t* map, std::uintptr_t* data, std::uint64_t* count, std::uint32_t* flags) {
  return SafeRead(g_base + kSoundSettingsVa, map, sizeof *map) && *map != 0 && SafeRead(*map, data, sizeof *data) &&
         *data != 0 && (*data & 7) == 0 && SafeRead(*map + kMapCount, count, sizeof *count) &&
         SafeRead(*map + kMapFlags, flags, sizeof *flags);
}

std::uint32_t WantId(std::uint64_t key) {
  for (const auto& r : kIds)
    if (r.key == key) return r.id;
  return 0;
}

// one of kIds' ids (what only this module writes; the stock rows hold kStockId)
bool KnownId(std::uint32_t id) {
  for (const auto& r : kIds)
    if (r.id == id) return true;
  return false;
}

// a known row this module writes: it holds the stock id, or another known row's id (a store into an unsorted table that
// the first hit's sort moved under it lands in a neighbour row; the next pass puts the right id back)
bool NeedsWrite(std::uint32_t id, std::uint32_t want) { return want != 0 && id != want && (id == kStockId || KnownId(id)); }

bool AllKeysKnown(std::uintptr_t data, std::uint64_t count) {
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
    if (on) pthread_mutex_unlock(&g_table_mu);
  }
};

struct PassStats {
  unsigned known = 0, wrote = 0, right = 0, left = 0;
  bool log = true;
  char found[sizeof kIds / sizeof kIds[0] * 9 + 1] = "";
};

// The row loop. `ro`: read and count only. When it writes, the caller holds the table lock and has re-validated the table.
// One aligned u32 store at row+8, read back; keys, order and the pad are never touched.
Pass ScanRows(std::uintptr_t data, std::uint64_t count, bool ro, PassStats* st) {
  bool work = false;
  for (std::uint64_t i = 0; i < count && !work; ++i) {
    std::uint64_t key = 0;
    std::uint32_t id = 0;
    work = SafeRead(data + 16 * i, &key, sizeof key) && SafeRead(data + 16 * i + 8, &id, sizeof id) && NeedsWrite(id, WantId(key));
  }
  if (!work) return Pass::kNothing;
  if (ro && g_ro_logged.load(std::memory_order_relaxed)) {  // read-only: the read is logged once
    st->log = false;
    return Pass::kDone;
  }
  g_ro_logged.store(ro, std::memory_order_relaxed);
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

void SkipNote(const char* why, std::uintptr_t data, std::uint64_t count, const char* when) {
  g_skipped.fetch_add(1, std::memory_order_relaxed);
  if (!Budget(&g_note_lines, kNoteLines, "notes")) return;
  char d[24], c[24];
  std::snprintf(c, sizeof c, "%llu", static_cast<unsigned long long>(count));
  const NevrLogField f[] = {{"why", why}, {"when", when}, {"table", Hex(d, sizeof d, data)}, {"rows", c}};
  Log(NEVR_LOG_INFO, "impactsnd row pass skipped, nothing written", f, 4);
}

// One pass over the table (map, data, count) the caller validated. A write needs the table lock (try only: a thread that is in
// the Shutdown wrapper holds it while the table is freed, and the tick must never wait) and the global table pointer, the data
// pointer and the count read again under it, still the ones validated: otherwise nothing is written (kSkipped, the caller
// retries). The Shutdown wrapper takes the same lock, so while it is held the table cannot be freed. No logging under the lock.
Pass RowPass(std::uintptr_t map, std::uintptr_t data, std::uint64_t count, const char* when) {
  const bool ro = g_nowrite || !g_writers_ok.load(std::memory_order_acquire);
  PassStats st;
  Pass res = Pass::kNothing;
  const char* skip = nullptr;
  if (ro) {
    res = ScanRows(data, count, true, &st);
  } else {
    Hook(4);
    Held held;
    if (pthread_mutex_trylock(&g_table_mu) != 0) {
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
  g_rows_written.fetch_add(st.wrote, std::memory_order_relaxed);
  g_rows_left.fetch_add(st.left, std::memory_order_relaxed);
  if (Budget(&g_write_lines, kWriteLines, "ids")) {
    char t[24], r[24], k[24], w[24], ri[24], l[24];
    std::snprintf(r, sizeof r, "%llu", static_cast<unsigned long long>(count));
    std::snprintf(k, sizeof k, "%u", st.known);
    std::snprintf(w, sizeof w, "%u", st.wrote);
    std::snprintf(ri, sizeof ri, "%u", st.right);
    std::snprintf(l, sizeof l, "%u", st.left);
    const NevrLogField f[] = {{"when", when}, {"table", Hex(t, sizeof t, data)}, {"rows", r}, {"known", k},
                              {"rewrote", w}, {"right", ri},                      {"left", l}, {"found_ids", st.found}};
    Log(NEVR_LOG_INFO,
        g_nowrite ? "impactsnd ids (nowrite build: read, not written)"
        : ro      ? "impactsnd ids (row writes disabled, Shutdown not wrapped: read, not written)"
                  : "impactsnd ids",
        f, 8);
  }
  return Pass::kDone;
}

void NoTableNote(std::uintptr_t map, std::uintptr_t data, std::uint64_t count, const char* where) {
  if (!Budget(&g_note_lines, kNoteLines, "notes")) return;
  char m[24], d[24], c[24];
  std::snprintf(c, sizeof c, "%llu", static_cast<unsigned long long>(count));
  const NevrLogField f[] = {{"where", where}, {"map", Hex(m, sizeof m, map)}, {"data", Hex(d, sizeof d, data)}, {"count", c}};
  Log(NEVR_LOG_INFO, "impactsnd no table now (level change?); watching for the next one", f, 4);
}

// The table's word-patch state as the row count shows it: the stock loader builds 5 rows, the loader with the five R16 words
// builds 15.
const char* PatchState(std::uint64_t count) {
  if (count == 0) return "no_verdict";
  if (count == kRowCount) return "in_effect";
  if (count == kPlayerRows) return "absent";
  return "unexpected";
}

// Straight after the loader returns, on the loader's thread: the table is complete and (the stock loader) unsorted, no hit
// has used it yet. Same rule as the tick's early pass: an unsorted table is written only when every key is a known one.
void AfterLoad() {
  g_loads.fetch_add(1, std::memory_order_relaxed);
  std::uintptr_t map = 0, data = 0;
  std::uint64_t count = 0;
  std::uint32_t flags = 0;
  if (!ReadTable(&map, &data, &count, &flags) || count == 0 || count > kRowMax) {
    NoTableNote(map, data, count, "after LoadSoundSettings");
    return;
  }
  if (Budget(&g_note_lines, kNoteLines, "notes")) {
    char d[24], c[24], fl[24];
    std::snprintf(c, sizeof c, "%llu", static_cast<unsigned long long>(count));
    std::snprintf(fl, sizeof fl, "%08x", flags);
    const NevrLogField f[] = {{"table", Hex(d, sizeof d, data)}, {"rows", c}, {"flags", fl}, {"word_patch", PatchState(count)}};
    Log(NEVR_LOG_INFO, "impactsnd LoadSoundSettings returned", f, 4);
  }
  const bool unsorted = (flags & 1) != 0;
  if (unsorted && !AllKeysKnown(data, count)) return;  // the tick decides (it logs the skip)
  const Pass r = RowPass(map, data, count, unsorted ? "load/unsorted" : "load/sorted");
  if (unsorted && r != Pass::kSkipped) g_uns_done.store(data, std::memory_order_relaxed);
}

// The Wwise event id: FNV-1 32 of the lowercase name (the kIds ids). A null name is the empty name.
std::uint32_t WwiseEventId(const char* name) {
  std::uint32_t h = 2166136261u;
  for (const char* p = name != nullptr ? name : ""; *p != 0; ++p) {
    h *= 16777619u;
    const char c = (*p >= 'A' && *p <= 'Z') ? static_cast<char>(*p - 'A' + 'a') : *p;
    h ^= static_cast<std::uint8_t>(c);
  }
  return h;
}

template <typename Fn>
Fn Callee(std::size_t idx) {
  return reinterpret_cast<Fn>(g_base + kRebuildCallees[idx].elf);
}

// The first callee whose stub is not the pinned one (its name), or nullptr when all of them are.
const char* FirstMissingCallee(std::uintptr_t base) {
  for (const RebuildCallee& c : kRebuildCallees) {
    std::uint32_t w = 0;
    if (!SafeRead(base + c.elf, &w, sizeof w) || w != c.first_word) return c.name;
  }
  return nullptr;
}

// One row onto the table at `table` (kSoundSettings' pointee, or +0x40 for the bullet_whizzby table): the stock append, field for
// field. Words: data +0, growth +0x20, capacity +0x28, count +0x30; flags (u32) +0x38, bit 0 = sort pending. The table lock is
// held by the caller.
void AppendRow(std::uintptr_t table, std::uint64_t key, std::uint32_t id) {
  auto* words = reinterpret_cast<std::uint64_t*>(table);
  std::uint64_t cap = words[5];
  std::uint64_t count = words[6];
  if (cap <= count) {
    const std::uint64_t grow = std::max(cap, words[4]);
    Callee<MemBlockResizeFn>(kIdxResize)(reinterpret_cast<void*>(table), static_cast<std::uint32_t>((grow + cap) * 0x10), 1);
    count = words[6];
    words[5] = cap + grow;
  }
  words[6] = count + 1;
  std::uint8_t* row = reinterpret_cast<std::uint8_t*>(words[0] + count * 0x10);
  std::memcpy(row, &key, sizeof key);
  std::memcpy(row + 8, &id, sizeof id);
  *reinterpret_cast<std::uint32_t*>(table + 0x38) |= 1;
}

// The rebuilt LoadSoundSettings body, into the table at `map`. Returns the file's result (0 read and walked, else the
// LoadFromProjectSourceDb result, which the stock loader returns too); `rows` counts the appends. The table lock is held.
int RebuildSoundSettings(std::uintptr_t map, unsigned* rows) {
  alignas(16) unsigned char json[16];  // the stock frame holds the CJson in these 16 bytes (sp+0x210..0x21f)
  char file[sizeof kJsonFile];
  std::memcpy(file, kJsonFile, sizeof file);
  Callee<JsonCtorFn>(kIdxCtor)(json);
  const int rc = Callee<JsonLoadFn>(kIdxLoad)(json, file);
  if (rc == 0) {
    const auto next = Callee<JsonNextFn>(kIdxNext);
    const auto name_at = Callee<JsonNameAtFn>(kIdxNameAt);
    const auto tstring = Callee<JsonTStringFn>(kIdxTString);
    // bullet_impact: one entry per gun; each member of a gun is a row, key = CSymbol64(member) ^ CSymbol64(gun)
    for (void* gun_node = next(json, kImpactPath, nullptr); gun_node != nullptr; gun_node = next(json, kImpactPath, gun_node)) {
      const char* gun = name_at(json, gun_node);
      const std::uint64_t gun_sym = EchoVR::CSymbol64Hash(gun);
      char path[512];
      std::snprintf(path, sizeof path, "%s|%s", kImpactPath, gun);
      for (void* member = next(json, path, nullptr); member != nullptr; member = next(json, path, member)) {
        const std::uint64_t key = EchoVR::CSymbol64Hash(name_at(json, member)) ^ gun_sym;
        AppendRow(map, key, WwiseEventId(tstring(json, member, "")));
        ++*rows;
      }
    }
    // bullet_whizzby: one entry each, key = CSymbol64(name) with no XOR
    for (void* node = next(json, kWhizzbyPath, nullptr); node != nullptr; node = next(json, kWhizzbyPath, node)) {
      AppendRow(map + kWhizzbyOff, EchoVR::CSymbol64Hash(name_at(json, node)), WwiseEventId(tstring(json, node, "")));
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

// The rebuilt loader under the table lock: the table the global holds now, read under the lock, is the one built into. Nothing is
// logged under the lock.
RebuildResult RebuildUnderLock() {
  RebuildResult out;
  pthread_mutex_lock(&g_table_mu);
  Held held;
  held.on = true;
  std::uintptr_t map = 0;
  if (SafeRead(g_base + kSoundSettingsVa, &map, sizeof map) && map != 0) {
    out.map = map;
    out.rc = RebuildSoundSettings(map, &out.rows);
  }
  return out;
}

// The wrapper on LoadSoundSettings (R14): the loader takes no argument (it reads none before its first clobber of x0,
// disassembly 0x2221d1c..0x2221d20) and returns w0 = w19 (0x2222008), the caller's next call may see it in x0, so the whole
// return value is passed through. With the rebuild on (g_rebuild) the rebuilt loader runs in place of the stock one; otherwise
// the stock loader runs and the after-load pass reads what it built.
std::uint64_t LoadWrapper() {
  if (g_rebuild.load(std::memory_order_acquire)) {
    NoteThread(kRoleLoader);
    const RebuildResult r = RebuildUnderLock();
    if (Budget(&g_note_lines, kNoteLines, "notes")) {
      char d[24], c[24], j[24];
      std::snprintf(c, sizeof c, "%u", r.rows);
      std::snprintf(j, sizeof j, "%d", r.rc);
      const NevrLogField f[] = {{"table", Hex(d, sizeof d, r.map)}, {"rows", c}, {"json_result", j}, {"loader", "reconstructed"}};
      Log(NEVR_LOG_INFO, "impactsnd LoadSoundSettings rebuilt", f, 4);
    }
    AfterLoad();
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(r.rc));
  }
  const auto real = reinterpret_cast<std::uint64_t (*)()>(g_real.load(std::memory_order_acquire));
  if (real == nullptr) return 0;  // not reachable: the slot is swapped only after g_real is set
  const std::uint64_t r = real();
  NoteThread(kRoleLoader);
  AfterLoad();
  return r;
}

// The wrapper on CR15NetBulletCS::Shutdown (R17), a virtual method: x0 = this, read first (`mov x19, x0` at 0x2222700), and the
// method ends in a tail call (`b 0x010190d0` at 0x2222848) so x0 is its return. The real Shutdown frees kSoundSettings and
// clears the global when the last system leaves (disassembly 0x22227ec blr the allocator's free, 0x22227f8 `str xzr, [x24]`): it
// runs with the table lock held, so no row write is in flight while it does, and none starts until it has returned. A nested
// call on the same thread does not take the lock again.
std::uint64_t ShutdownWrapper(std::uint64_t self) {
  const auto real = reinterpret_cast<std::uint64_t (*)(std::uint64_t)>(g_real_shutdown.load(std::memory_order_acquire));
  if (real == nullptr) return 0;  // not reachable: the slot is swapped only after g_real_shutdown is set
  NoteThread(kRoleShutdown);
  const bool outer = t_shutdown_depth == 0;
  ++t_shutdown_depth;
  if (outer) pthread_mutex_lock(&g_table_mu);
  const std::uint64_t r = real(self);
  if (outer) pthread_mutex_unlock(&g_table_mu);
  --t_shutdown_depth;
  g_shutdowns.fetch_add(1, std::memory_order_relaxed);
  if (outer && Budget(&g_note_lines, kNoteLines, "notes")) {
    std::uintptr_t map = 0;
    SafeRead(g_base + kSoundSettingsVa, &map, sizeof map);
    char m[24];
    const NevrLogField f[] = {{"table_global_now", Hex(m, sizeof m, map)}};
    Log(NEVR_LOG_INFO, "impactsnd CR15NetBulletCS::Shutdown returned", f, 1);
  }
  return r;
}

// The tick's watch over every table instance of the process. A write needs the same data pointer and count reading the same
// on two consecutive polls (kPollS apart), and a re-read right before the rows (then the table lock and a third read in
// RowPass). An unsorted table is written once per table, only when every key is a known one; a sorted one whenever a known
// row needs it.
void IdsTick(double now) {
  std::uintptr_t map = 0, data = 0;
  std::uint64_t count = 0;
  std::uint32_t flags = 0;
  const bool read = ReadTable(&map, &data, &count, &flags);
  if (!read || count == 0 || count > kRowMax) {
    if (!g_table_gone) NoTableNote(map, data, count, "tick");
    g_table_gone = true;
    g_wait_data = 0;
    g_sorted_data = 0;
    g_uns_data = 0;
    g_uns_done.store(0, std::memory_order_relaxed);  // a new table at the same address is new
    return;
  }
  g_table_gone = false;
  if ((flags & 1) != 0) {  // sort still pending: the first hit sorts it
    g_sorted_data = 0;
    if (g_wait_data != data) {
      g_wait_data = data;
      g_wait_logged = false;
      g_wait_t0 = now;
    }
    if (!g_wait_logged && now - g_wait_t0 >= kWaitS) {
      g_wait_logged = true;
      if (Budget(&g_note_lines, kNoteLines, "notes")) {
        char d[24], c[24], w[24];
        std::snprintf(c, sizeof c, "%llu", static_cast<unsigned long long>(count));
        std::snprintf(w, sizeof w, "%.0f", now - g_wait_t0);
        const NevrLogField f[] = {{"table", Hex(d, sizeof d, data)}, {"rows", c}, {"unsorted_for_s", w}};
        Log(NEVR_LOG_INFO, "impactsnd table still unsorted; still watching", f, 3);
      }
    }
    const bool same = data == g_uns_data && count == g_uns_count;
    g_uns_data = data;
    g_uns_count = count;
    if (!same || g_uns_done.load(std::memory_order_relaxed) == data) return;
    Hook(1);
    std::uintptr_t map2 = 0, data2 = 0;
    std::uint64_t count2 = 0;
    std::uint32_t flags2 = 0;
    if (!ReadTable(&map2, &data2, &count2, &flags2) || map2 != map || data2 != data || count2 != count || (flags2 & 1) == 0)
      return;  // changed or sorted under us: the next polls decide
    if (AllKeysKnown(data, count)) {
      const Pass r = RowPass(map, data, count, "tick/unsorted");
      if (r != Pass::kSkipped) g_uns_done.store(data, std::memory_order_relaxed);
    } else {
      g_uns_done.store(data, std::memory_order_relaxed);
      if (Budget(&g_note_lines, kNoteLines, "notes")) {
        char d[24], c[24];
        std::snprintf(c, sizeof c, "%llu", static_cast<unsigned long long>(count));
        const NevrLogField f[] = {{"table", Hex(d, sizeof d, data)}, {"rows", c}};
        Log(NEVR_LOG_INFO, "impactsnd table unsorted with keys outside the 15: not written until it is sorted", f, 2);
      }
    }
    return;
  }
  g_wait_data = 0;
  g_uns_data = 0;
  const bool stable = data == g_sorted_data && count == g_sorted_count;
  g_sorted_data = data;
  g_sorted_count = count;
  if (!stable) return;
  Hook(1);
  std::uintptr_t map2 = 0, data2 = 0;
  std::uint64_t count2 = 0;
  std::uint32_t flags2 = 0;
  if (!ReadTable(&map2, &data2, &count2, &flags2) || map2 != map || data2 != data || count2 != count || (flags2 & 1) != 0)
    return;  // changed under us: the next polls decide again
  RowPass(map, data, count, "tick/sorted");
}

}  // namespace

const ImpactSndRow* ImpactSndRows(std::size_t* count) {
  *count = sizeof kIds / sizeof kIds[0];
  return kIds;
}

InstallResult InstallImpactSnd(std::uintptr_t base) {
  InstallResult out;
  out.status = SwapStatus::kUnexpectedOriginal;
  g_base = base;
  g_t0 = Now();
  // R15: the hit code the module's reasoning is about. A word that differs = not the pinned build: nothing is written.
  for (std::size_t i = 0; i < 4; ++i) {
    std::uint32_t have = 0;
    if (!SafeRead(base + kCode[i].va, &have, sizeof have) || have != kCode[i].want) {
      Refuse("hit code word differs", kCode[i].va, have, kCode[i].want);
      out.reason = "hit_code_word_mismatch";
      return out;
    }
  }
  // R12: the relocated GLOB_DAT word of kSoundSettings must point at the .bss object this module reads.
  std::uintptr_t got = 0;
  if (!SafeRead(base + kSoundSettingsGotVa, &got, sizeof got) || got != base + kSoundSettingsVa) {
    Refuse("kSoundSettings GOT word differs", kSoundSettingsGotVa, got, base + kSoundSettingsVa);
    out.reason = "ksoundsettings_got_mismatch";
    return out;
  }
  // R13: D.
  std::uint64_t was = 0;
  if (!SafeRead(base + kDefaultSymVa, &was, sizeof was)) {
    Refuse("default symbol unreadable", kDefaultSymVa, 0, kDefaultSym);
    out.reason = "default_symbol_unreadable";
    return out;
  }
  const bool write = was == 0 && !g_nowrite;
  const char* action = write ? "wrote" : was != 0 ? "left alone (not 0)" : "left alone (nowrite build)";
  std::uint64_t back = was;
  if (write) {
    Mapping m;  // .bss: base + vaddr, outside RELRO, no page-protection call; refuse rather than fault on a layout that is not the pinned one
    if (!FindMapping(base + kDefaultSymVa, &m) || m.perms[0] != 'r' || m.perms[1] != 'w' || m.perms[2] == 'x') {
      Refuse("default symbol page is not rw-", kDefaultSymVa, was, kDefaultSym);
      out.reason = "default_symbol_page_not_rw";
      return out;
    }
    __atomic_store_n(reinterpret_cast<std::uint64_t*>(base + kDefaultSymVa), kDefaultSym, __ATOMIC_RELEASE);
    Hook(3);
    if (!SafeRead(base + kDefaultSymVa, &back, sizeof back) || back != kDefaultSym) action = "wrote, READ-BACK DIFFERS";
  }
  {
    char a[24], w[24], b[24];
    const NevrLogField f[] = {{"default_vaddr", Hex(a, sizeof a, kDefaultSymVa)}, {"was", Hex(w, sizeof w, was)},
                              {"now", Hex(b, sizeof b, back)}, {"action", action}, {"nowrite_build", g_nowrite ? "1" : "0"}};
    Log(back == kDefaultSym || !write ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "impactsnd on", f, 5);
  }
  g_on.store(true, std::memory_order_release);
  // R17: the Shutdown vtable word (data page, RELRO: got_swap.h makes it writable for the one store and restores it). The wrapper
  // holds the table lock across the real Shutdown, so the row writes may run on any thread. A slot that is not the pinned one:
  // the writes are not proven safe, so they stay off (the module reads and logs only).
  const InstallResult rs = InstallImportHook("CR15NetBulletCS::Shutdown", base, kShutdownSlotVa,
                                             reinterpret_cast<std::uintptr_t>(&ShutdownWrapper), &g_real_shutdown, kShutdownFnVa);
  g_writers_ok.store(rs.status == SwapStatus::kOk, std::memory_order_release);
  {
    const NevrLogField f[] = {{"hook", "CR15NetBulletCS::Shutdown (vtable word)"}, {"status", StatusName(rs.status)}, {"reason", rs.reason},
                              {"row_writes", rs.status == SwapStatus::kOk ? "enabled" : "DISABLED (read and log only)"}};
    Log(rs.status == SwapStatus::kOk ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "impactsnd shutdown wrapper", f, 4);
  }
  // R16: the rebuilt loader writes the table under the same lock, so it runs only with the Shutdown wrapper in, every stub pinned,
  // and a write run (the nowrite build keeps the stock loader and the after-load pass, which only reads).
  const char* missing = FirstMissingCallee(base);
  const bool rebuild = missing == nullptr && rs.status == SwapStatus::kOk && !g_nowrite;
  g_rebuild.store(rebuild, std::memory_order_release);
  {
    const char* mode = rebuild ? "reconstructed (every member of every gun)" : missing != nullptr ? "stock (a callee stub is not pinned)"
                       : g_nowrite                                              ? "stock (nowrite build)"
                                                                                : "stock (row writes disabled)";
    const NevrLogField f[] = {{"mode", mode}, {"missing_callee", missing != nullptr ? missing : "none"}};
    Log(rebuild || missing == nullptr ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "impactsnd LoadSoundSettings rebuild", f, 2);
  }
  // R14: the after-load pass. Data page only (the slot's page is RELRO: got_swap.h makes it writable for the one store).
  const InstallResult r = InstallImportHook("CR15NetBulletCS::LoadSoundSettings", base, kLoadSlotVa,
                                            reinterpret_cast<std::uintptr_t>(&LoadWrapper), &g_real, kLoadFnVa);
  {
    const NevrLogField f[] = {{"hook", "CR15NetBulletCS::LoadSoundSettings"}, {"status", StatusName(r.status)}, {"reason", r.reason},
                              {"mode", r.status == SwapStatus::kOk ? "after-load pass + tick" : "tick only"}};
    Log(r.status == SwapStatus::kOk ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "impactsnd load wrapper", f, 4);
  }
  return rs.status != SwapStatus::kOk ? rs : r;
}

void ImpactSndTick(std::uintptr_t /*game*/) {
  if (!g_on.load(std::memory_order_acquire)) return;
  const double now = Now();
  if (now < g_next) return;
  g_next = now + kPollS;
  g_polls.fetch_add(1, std::memory_order_relaxed);
  NoteThread(kRoleTick);
  if (g_table_line_done) {
    IdsTick(now);
    return;
  }
  // the count line, once: what the table reads kSettleS after it is first non-zero, or kWaitS after install with what is there
  std::uintptr_t map = 0;
  std::uint64_t count = 0;
  const bool have_map = SafeRead(g_base + kSoundSettingsVa, &map, sizeof map) && map != 0;
  const bool have_count = have_map && SafeRead(map + kMapCount, &count, sizeof count);
  if (have_count && count != 0) {
    if (g_seen == 0) g_seen = now;
    if (now - g_seen < kSettleS) return;
  } else if (now - g_t0 < kWaitS) {
    return;
  }
  g_table_line_done = true;
  std::uint64_t d = 0;
  SafeRead(g_base + kDefaultSymVa, &d, sizeof d);
  char dc[24], c[24], s[24];
  std::snprintf(c, sizeof c, "%llu", static_cast<unsigned long long>(count));
  std::snprintf(s, sizeof s, "%.1f", now - g_t0);
  const NevrLogField f[] = {{"map", have_map ? "set" : "null"}, {"rows", c}, {"after_s", s},
                            {"default_symbol", Hex(dc, sizeof dc, d)}, {"word_patch", PatchState(count)}};
  const bool odd = count != 0 && count != kRowCount && count != kPlayerRows;
  Log(odd ? NEVR_LOG_WARNING : NEVR_LOG_INFO, "impactsnd table", f, 5);
}

ImpactSndStats GetImpactSndStats() {
  ImpactSndStats s;
  s.on = g_on.load();
  s.writers_ok = g_writers_ok.load();
  s.polls = g_polls.load();
  s.loads = g_loads.load();
  s.passes = g_passes.load();
  s.rows_written = g_rows_written.load();
  s.rows_left = g_rows_left.load();
  s.skipped = g_skipped.load();
  s.shutdowns = g_shutdowns.load();
  return s;
}

void SetImpactSndClockForTest(double (*clock)()) { g_clock = clock; }
void SetImpactSndNoWriteForTest(bool nowrite) { g_nowrite = nowrite; }
void SetImpactSndHookForTest(void (*hook)(int stage)) { g_hook = hook; }
void ResetImpactSndForTest() {
  g_base = 0;
  g_nowrite = Q15T_IMPACTSND_NOWRITE != 0;
  g_on.store(false);
  g_writers_ok.store(false);
  g_rebuild.store(false);
  g_real.store(0);
  g_real_shutdown.store(0);
  g_uns_done.store(0);
  g_note_lines.store(0);
  g_write_lines.store(0);
  g_thread_lines.store(0);
  g_ro_logged.store(false);
  g_polls.store(0);
  g_loads.store(0);
  g_passes.store(0);
  g_rows_written.store(0);
  g_rows_left.store(0);
  g_skipped.store(0);
  g_shutdowns.store(0);
  for (auto& t : g_tid) t.store(0);
  t_shutdown_depth = 0;
  g_table_line_done = g_table_gone = g_wait_logged = false;
  g_t0 = g_seen = g_next = g_wait_t0 = 0;
  g_wait_data = g_sorted_data = g_uns_data = 0;
  g_sorted_count = g_uns_count = 0;
  g_hook = nullptr;
}

}  // namespace q15t
