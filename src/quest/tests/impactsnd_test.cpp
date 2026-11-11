// Host test of the impact-sound module (src/quest/combat/impactsnd.cpp, #2183; moved from the q15t plugin's impactsnd): the real handlers through the
// two thunks, installed by the real GotHook into the JUMP_SLOT of LoadSoundSettings and the vtable word (ABS64) of CR15NetBulletCS::Shutdown of a libr15
// image built in memory that holds the pinned ELF vaddrs (the hit-code words, the kSoundSettings GLOB_DAT word, the .bss words D and kSoundSettings, a
// trampoline at each function's address) and a fake impact table on the host heap laid out as the game's (map+0x00 data, +0x30 count, +0x38 flags;
// 16-byte rows {u64 key, u32 id, pad}). Real SafeRead, a fake clock, and a fake CR15Game (the two gamespace tables, read by process_vm_readv as in
// production) that says which level is loaded.
//
// What moved with the module is every assertion of the plugin's test about the table, the loaders, the tick and the lock. What changed is the
// fixture (the manager's slots, the structured lines), the install checks (D is read and never written; the nowrite build option is the switch
// `combat.impactsnd_write`), and what is new: the scope. Arena shares the APK, so the load handler runs the game's own loader and the tick sleeps
// unless a combat level is loaded.
//
// Built and run by `just test-quest-combat`.
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/safe_read.h"
#include "quest/combat/impactsnd.h"
#include "quest/combat/impactsnd_targets.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/key_hog.h"

namespace impactsnd = nevr_quest::combat::impactsnd;

// What a path costs, counted: every SafeRead and every level scan goes through a counter behind its test seam (safe_read.h, combat_level.h),
// installed for the whole run at the top of main().
static std::atomic<std::uint64_t> g_reads{0}, g_scans{0};
static void CountScan() { g_scans.fetch_add(1, std::memory_order_relaxed); }
static std::uint64_t ReadCount() { return g_reads.load(std::memory_order_relaxed); }
static std::uint64_t ScanCount() { return g_scans.load(std::memory_order_relaxed); }
static void StartCounting() {
  nevr_quest::combat::SetReadCounterForTest(&g_reads);
  nevr_quest::combat::level::SetScanSeamForTest(&CountScan);
}

static int g_fail = 0;
#define CHECK(cond, ...)                                \
  do {                                                  \
    if (!(cond)) {                                      \
      ++g_fail;                                         \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                         \
      std::printf("\n");                                \
    }                                                   \
  } while (0)

// ---- the log: every line the module writes is one JSON object; a needle written as the plugin's `key=value` tokens is matched against its fields ----
struct Line {
  int level;  // 0 debug, 1 info, 2 warning, 3 error
  std::string text;
};
static std::vector<Line> g_lines;
static std::mutex g_lines_mu;  // the module logs from several threads
static void Capture(sentinel::LogLevel level, const char* line) {
  const int l = level == sentinel::LogLevel::kInfo ? 1 : level == sentinel::LogLevel::kWarn ? 2 : level == sentinel::LogLevel::kError ? 3 : 0;
  std::lock_guard<std::mutex> lock(g_lines_mu);
  g_lines.push_back({l, line});
}
// The event words of the plugin's lines and the events that replace them.
static std::string Needle(const char* needle) {
  static const struct {
    const char* from;
    const char* to;
  } kEvents[] = {
      {"impactsnd table still unsorted", "combat_impactsnd_unsorted_wait"},
      {"impactsnd table unsorted with keys outside the 15", "combat_impactsnd_unsorted_foreign_keys"},
      {"impactsnd table", "combat_impactsnd_table"},
      {"impactsnd ids", "combat_impactsnd_ids"},
      {"impactsnd no table now", "combat_impactsnd_no_table"},
      {"impactsnd row pass skipped", "combat_impactsnd_row_pass_skipped"},
      {"impactsnd LoadSoundSettings returned", "combat_impactsnd_load"},
      {"impactsnd LoadSoundSettings rebuilt", "combat_impactsnd_rebuilt"},
      {"impactsnd CR15NetBulletCS::Shutdown returned", "combat_impactsnd_shutdown_returned"},
      {"impactsnd thread", "combat_impactsnd_thread"},
      {"impactsnd OFF: nothing written", "combat_impactsnd_refused"},
      {"impactsnd load wrapper", "combat_impactsnd_hook\",\"hook\":\"load"},
      {"impactsnd shutdown wrapper", "combat_impactsnd_hook\",\"hook\":\"shutdown"},
      {"impactsnd on", "combat_impactsnd_default_symbol"},
      {"log cap reached", "combat_impactsnd_log_cap"},
  };
  std::string s = needle;
  for (const auto& e : kEvents) {
    if (s.compare(0, std::strlen(e.from), e.from) == 0) return std::string("\"event\":\"") + e.to + "\"" + s.substr(std::strlen(e.from));
  }
  if (s.find('=') == std::string::npos) return s;
  // `key=value` tokens; a word without '=' after one continues its value ("where=after LoadSoundSettings")
  std::vector<std::pair<std::string, std::string>> kv;
  size_t at = 0;
  while (at < s.size()) {
    size_t end = s.find(' ', at);
    if (end == std::string::npos) end = s.size();
    const std::string tok = s.substr(at, end - at);
    at = end + 1;
    const size_t eq = tok.find('=');
    if (eq != std::string::npos) kv.emplace_back(tok.substr(0, eq), tok.substr(eq + 1));
    else if (!kv.empty()) kv.back().second += " " + tok;
  }
  std::string out;
  for (size_t k = 0; k < kv.size(); ++k) {
    const std::string& key = kv[k].first;
    std::string val = kv[k].second;
    const bool last = k + 1 == kv.size();  // the last string value is a prefix: its closing quote is left off
    const bool digits = !val.empty() && val.find_first_not_of("0123456789") == std::string::npos && (val == "0" || val[0] != '0');
    const bool hex = (key == "table_global_now" || key == "table" || key == "data" || key == "map" || key == "default_symbol" || key == "vaddr" || key == "read" ||
                      key == "pinned" || key == "value") &&
                     !val.empty() && val.find_first_not_of("0123456789abcdef") == std::string::npos;
    if (!out.empty()) out += ',';
    if (hex) {
      while (val.size() < 16) val = "0" + val;
      out += "\"" + key + "\":\"0x" + val + (last ? "" : "\"");
    } else if (digits) {
      out += "\"" + key + "\":" + val;
    } else {
      out += "\"" + key + "\":\"" + val + (last ? "" : "\"");
    }
  }
  return out;
}
static int CountLines(const char* needle, int level = -1) {
  const std::string n = Needle(needle);
  int c = 0;
  std::lock_guard<std::mutex> lock(g_lines_mu);
  for (const auto& l : g_lines)
    if (l.text.find(n) != std::string::npos && (level < 0 || l.level == level)) ++c;
  return c;
}
static bool HasLine(const char* needle, int level = -1) { return CountLines(needle, level) > 0; }

// ---- the pinned addresses, restated here (not taken from the module) ----
static const uintptr_t kDefaultVa = 0x37af8d0, kSettingsVa = 0x37af6a0, kSettingsGotVa = 0x370fa70, kSlotVa = 0x36fa3c0, kFnVa = 0x2121d04;
static const uintptr_t kShutSlotVa = 0x36933e8, kShutFnVa = 0x21226f0;  // CR15NetBulletCS::Shutdown: vtable word, function
static const uintptr_t kCodeVa[4] = {0x21208b4, 0x21208b8, 0x21208bc, 0x21208c8};
static const uint32_t kCodeWord[4] = {0xf9401ac1, 0xb4000541, 0xf000b468, 0xf944691b};
static const uint64_t kDefaultSym = 0x2fd38a995f615b9eull;
static const uint32_t kStock = 1;

// ---- the fake game image ----
static combat_test::Image g_image;
static uintptr_t g_base = 0;
static void W64(uintptr_t va, uint64_t v) { *reinterpret_cast<uint64_t*>(g_base + va) = v; }
static uint64_t R64(uintptr_t va) { return *reinterpret_cast<uint64_t*>(g_base + va); }
static uint32_t R32(uintptr_t va) { return *reinterpret_cast<uint32_t*>(g_base + va); }
static bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}

// ---- the fake CR15Game: the two gamespace tables as the module reads them (offsets spelled here, not taken from the header) ----
constexpr std::size_t kTable0 = 0x1f48, kCount0 = 0x1f78, kGamespaceOwner = 0x8, kGamespaceLevel = 0x58;
constexpr std::size_t kFiberRows = 0x1ed8, kFiberCount = 0x1f08, kFiberGamespace = 0x30;
struct Gamespace {
  std::uintptr_t words[0x60 / 8 + 1] = {};
};
struct Game {
  std::uintptr_t words[0x2000 / 8] = {};
  std::uintptr_t table0[8][2] = {};
  Gamespace spaces[8];
  std::uintptr_t fiberRows[4] = {};  // the init-fiber table: gamespaces still loading
  std::uintptr_t fibers[4][8] = {};
  Gamespace flight[4];
};
static Game g_game;
static uintptr_t GameAddress() { return reinterpret_cast<std::uintptr_t>(g_game.words); }
// A gamespace still loading: its init fiber is in the table at game + 0x1ed8 and it is in neither of the two loaded tables (CR15NetBulletCS::Initialize, the
// caller of LoadSoundSettings, runs while the level's resources are built, before the gamespace enters the loaded tables).
static void PutInFlight(const char* name, int row = 0) {
  Gamespace& gs = g_game.flight[row];
  gs.words[kGamespaceOwner / 8] = GameAddress();
  gs.words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(EchoVR::CSymbol64Hash(name));
  g_game.fibers[row][kFiberGamespace / 8] = reinterpret_cast<std::uintptr_t>(&gs);
  g_game.fiberRows[row] = reinterpret_cast<std::uintptr_t>(g_game.fibers[row]);
  g_game.words[kFiberRows / 8] = reinterpret_cast<std::uintptr_t>(g_game.fiberRows);
  std::uintptr_t& count = g_game.words[kFiberCount / 8];
  if (static_cast<std::uintptr_t>(row) + 1 > count) count = static_cast<std::uintptr_t>(row) + 1;
}
static void LoadLevels(std::initializer_list<const char*> names) {
  g_game = Game{};
  int row = 0;
  for (const char* name : names) {
    Gamespace& gs = g_game.spaces[row];
    gs.words[kGamespaceOwner / 8] = GameAddress();
    gs.words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(EchoVR::CSymbol64Hash(name));
    g_game.table0[row][0] = 0x5555;
    g_game.table0[row][1] = reinterpret_cast<std::uintptr_t>(&gs);
    ++row;
  }
  g_game.words[kTable0 / 8] = reinterpret_cast<std::uintptr_t>(g_game.table0);
  g_game.words[kCount0 / 8] = static_cast<std::uintptr_t>(row);
}

// ---- the fake table ----
struct Row {
  uint64_t key;
  uint32_t id;
  uint32_t pad;
};
static const uint32_t kPad = 0xa5a5a5a5;
alignas(16) static Row g_rows[128];
alignas(16) static uint8_t g_map[0x40];
alignas(16) static uint8_t g_map2[0x40];
static size_t g_n = 0;

static const impactsnd::Row* Known(size_t* n) { return impactsnd::Rows(n); }
static uint64_t& MapData() { return *reinterpret_cast<uint64_t*>(g_map); }
static uint64_t& MapCount() { return *reinterpret_cast<uint64_t*>(g_map + 0x30); }
static uint32_t& MapFlags() { return *reinterpret_cast<uint32_t*>(g_map + 0x38); }
static void PublishRaw() { W64(kSettingsVa, reinterpret_cast<uintptr_t>(g_map)); }
// the table appears as a combat level's load makes it: the tick's watch starts (the arena tests publish with PublishRaw and mark nothing)
static void Publish() {
  PublishRaw();
  impactsnd::SetScopedForTest(true);
}
static void Unpublish() { W64(kSettingsVa, 0); }
static void SetTable(const std::vector<size_t>& idx, uint32_t id, bool unsorted) {  // idx into kIds, in this order
  size_t n = 0;
  const impactsnd::Row* k = Known(&n);
  g_n = idx.size();
  for (size_t i = 0; i < idx.size(); ++i) g_rows[i] = {k[idx[i]].key, id, kPad};
  MapData() = reinterpret_cast<uint64_t>(g_rows);
  MapCount() = g_n;
  MapFlags() = unsorted ? 1u : 0u;
}
static std::vector<size_t> All() {
  std::vector<size_t> v;
  for (size_t i = 0; i < 15; ++i) v.push_back(i);
  return v;
}
static std::vector<size_t> Players() { return {0, 3, 6, 9, 12}; }
static void SortRows() {  // what the first hit does: moves whole 16-byte rows by key, clears the pending bit
  std::sort(g_rows, g_rows + g_n, [](const Row& a, const Row& b) { return a.key < b.key; });
  MapFlags() &= ~1u;
}
static uint32_t IdOfKey(uint64_t key) {
  for (size_t i = 0; i < g_n; ++i)
    if (g_rows[i].key == key) return g_rows[i].id;
  return 0xffffffffu;
}
static uint32_t WantOf(uint64_t key) {
  size_t n = 0;
  const impactsnd::Row* k = Known(&n);
  for (size_t i = 0; i < n; ++i)
    if (k[i].key == key) return k[i].id;
  return 0;
}
static bool AllRight() {  // every known row holds its Wwise id
  for (size_t i = 0; i < g_n; ++i) {
    const uint32_t w = WantOf(g_rows[i].key);
    if (w != 0 && g_rows[i].id != w) return false;
  }
  return true;
}
static size_t CountRight() {
  size_t c = 0;
  for (size_t i = 0; i < g_n; ++i) c += WantOf(g_rows[i].key) != 0 && g_rows[i].id == WantOf(g_rows[i].key);
  return c;
}
static bool KeysAndPadsIntact(const std::vector<Row>& before) {
  if (before.size() != g_n) return false;
  for (size_t i = 0; i < g_n; ++i)
    if (before[i].key != g_rows[i].key || before[i].pad != g_rows[i].pad) return false;
  return true;
}
static std::vector<Row> Snap() { return std::vector<Row>(g_rows, g_rows + g_n); }

// the loader the trampoline at base+kFnVa jumps to: builds the table the way the Quest's stock loader does
enum LoadMode { kLoad15, kLoad5, kLoad15Sorted, kLoadForeign, kLoadNothing, kLoadHuge };
static LoadMode g_load_mode = kLoad15;
static int g_loader_calls = 0;
static const uint64_t kLoaderReturn = 0x1234abcdull;
static uint64_t FakeLoader() {
  ++g_loader_calls;
  switch (g_load_mode) {
    case kLoad15: SetTable(All(), kStock, true); PublishRaw(); break;
    case kLoad5: SetTable(Players(), kStock, true); PublishRaw(); break;
    case kLoad15Sorted: SetTable(All(), kStock, true); SortRows(); PublishRaw(); break;
    case kLoadForeign:
      SetTable(All(), kStock, true);
      g_rows[g_n++] = {0xdeadbeefcafef00dull, kStock, kPad};
      MapCount() = g_n;
      PublishRaw();
      break;
    case kLoadNothing: Unpublish(); break;
    case kLoadHuge: SetTable(All(), kStock, true); MapCount() = 1000; PublishRaw(); break;
  }
  return kLoaderReturn;
}

// the Shutdown the trampoline at base+kShutFnVa jumps to: frees the table (poisons its rows) and clears the global, as the real one does
enum ShutMode { kShutFree, kShutHold, kShutNested };
static int g_shut_mode = kShutFree;
static std::atomic<int> g_shut_calls{0}, g_uaf{0};
static std::atomic<bool> g_freed{false}, g_in_shutdown{false}, g_hold_release{false}, g_nested_done{false};
static uint64_t g_shut_self = 0;
static size_t g_right_at_free = 0;
static const uint64_t kShutXor = 0x5a;
static uint64_t CallShutdown(uint64_t self) { return reinterpret_cast<uint64_t (*)(uint64_t)>(R64(kShutSlotVa))(self); }
static uint64_t FakeShutdown(uint64_t self) {
  ++g_shut_calls;
  g_shut_self = self;
  if (g_shut_mode == kShutHold) {  // the table stays; the lock is held for up to 600 ms
    g_in_shutdown = true;
    for (int i = 0; i < 120 && !g_hold_release; ++i) usleep(5000);
    return self ^ kShutXor;
  }
  if (g_shut_mode == kShutNested) {  // a Shutdown that reaches the wrapper again on the same thread
    g_shut_mode = kShutFree;
    CallShutdown(self);
    g_nested_done = true;
    return self ^ kShutXor;
  }
  g_right_at_free = CountRight();
  g_freed = true;
  Unpublish();
  std::memset(g_rows, 0xdd, sizeof g_rows);
  return self ^ kShutXor;
}
static bool PoisonIntact() {
  for (size_t i = 0; i < g_n; ++i)
    if (g_rows[i].id != 0xddddddddu) return false;
  return true;
}

static double g_now = 100.0;
static double Clock() { return g_now; }
static void Poll() {  // one tick at the current fake time, then 0.25 s later
  impactsnd::Tick(GameAddress());
  g_now += 0.25;
}
static void Polls(int n) {
  for (int i = 0; i < n; ++i) Poll();
}
// The tick logs the table count line first (kSettleS after the table is first non-zero) and only then watches the rows
// (the source build's order). Warm() runs the polls up to and including that line; the next poll is the watch's first.
static void Warm() {
  Polls(9);
  CHECK(CountLines("impactsnd table") == 1, "warm-up: count line %d times", CountLines("impactsnd table"));
}

static void BuildImage() {
  const std::vector<combat_test::Reloc> relocs = {
      {"_ZN10NRadEngine8NRadGame15CR15NetBulletCS17LoadSoundSettingsEv", kSlotVa, false, false, kFnVa},
      {"_ZN10NRadEngine8NRadGame15CR15NetBulletCS8ShutdownEv", kShutSlotVa, false, false, kShutFnVa, true},
  };
  g_image.Build(relocs);
  g_base = g_image.Base();
  for (int i = 0; i < 4; ++i) *reinterpret_cast<uint32_t*>(g_base + kCodeVa[i]) = kCodeWord[i];
  W64(kSettingsGotVa, g_base + kSettingsVa);
  W64(kDefaultVa, kDefaultSym);
  W64(kSettingsVa, 0);
  W64(kSlotVa, g_base + kFnVa);
  W64(kShutSlotVa, g_base + kShutFnVa);
  // a trampoline at each function's address: movabs rax, <fake>; jmp rax (the page is made executable and never written afterwards)
  uint8_t code[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
  const uint64_t target = reinterpret_cast<uint64_t>(&FakeLoader);
  std::memcpy(code + 2, &target, 8);
  std::memcpy(reinterpret_cast<void*>(g_base + kFnVa), code, sizeof code);
  mprotect(reinterpret_cast<void*>((g_base + kFnVa) & ~0xfffull), 0x1000, PROT_READ | PROT_EXEC);
  uint8_t code2[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
  const uint64_t target2 = reinterpret_cast<uint64_t>(&FakeShutdown);
  std::memcpy(code2 + 2, &target2, 8);
  std::memcpy(reinterpret_cast<void*>(g_base + kShutFnVa), code2, sizeof code2);
  mprotect(reinterpret_cast<void*>((g_base + kShutFnVa) & ~0xfffull), 0x1000, PROT_READ | PROT_EXEC);
}

static std::vector<uint32_t> g_code_before;
static void Fresh() {  // a new image, new table, a reset module, a combat level loaded; not installed
  g_lines.clear();
  impactsnd::ResetForTest();
  sentinel::SetLogSink(&Capture);
  impactsnd::SetClockForTest(&Clock);
  BuildImage();
  LoadLevels({"mpl_combat_dyson"});
  impactsnd::SetGameForTest(GameAddress());
  std::memset(g_rows, 0, sizeof g_rows);
  std::memset(g_map, 0, sizeof g_map);
  g_n = 0;
  g_now = 100.0;
  g_loader_calls = 0;
  g_load_mode = kLoad15;
  g_shut_mode = kShutFree;
  g_shut_calls = 0;
  g_uaf = 0;
  g_freed = g_in_shutdown = g_hold_release = g_nested_done = false;
  g_shut_self = 0;
  g_right_at_free = 0;
}
static nevr_quest::combat::ModuleResult Install(bool write = true) { return impactsnd::InstallAt(true, write, &Lookup); }
static void Setup() {  // installed, and a table a combat level loaded is live (the tests of the tick's watch start there)
  Fresh();
  const nevr_quest::combat::ModuleResult r = Install();
  CHECK(r.installed, "setup install: %s", r.status);
  impactsnd::SetScopedForTest(true);
}
typedef uint64_t (*LoaderFn)();
static uint64_t CallLoader() { return reinterpret_cast<LoaderFn>(R64(kSlotVa))(); }

// ---- FNV-1 32, restated independently of the module ----
static uint32_t Fnv1(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s != 0; ++s) {
    h *= 16777619u;
    h ^= static_cast<uint8_t>(*s);
  }
  return h;
}

static void TestConstants() {
  // the FNV-1 32 helper against published vectors (FNV test suite): "" -> 811c9dc5, "a" -> 050c5d7e, "foobar" -> 31f0b262
  CHECK(Fnv1("") == 0x811c9dc5u && Fnv1("a") == 0x050c5d7eu && Fnv1("foobar") == 0x31f0b262u, "FNV-1 helper wrong: %08x %08x %08x",
        Fnv1(""), Fnv1("a"), Fnv1("foobar"));
  size_t n = 0;
  const impactsnd::Row* rows = Known(&n);
  CHECK(n == 15, "%zu rows", n);
  const char* guns[5] = {"assault", "scout", "rocket", "rifle", "smg"};
  const char* subs[3] = {"player", "barrier", "default"};
  const char* ev[3] = {"play_bullet_impact_player", "play_bullet_impact_shield", "play_bullet_impact_geo"};
  for (int g = 0; g < 5; ++g)
    for (int s = 0; s < 3; ++s) {
      std::string name = ev[s];
      if (g == 4) name += "_quark";  // smg uses the _quark events
      const impactsnd::Row& r = rows[g * 3 + s];
      const uint64_t key = EchoVR::CSymbol64Hash(subs[s]) ^ EchoVR::CSymbol64Hash(guns[g]);
      CHECK(r.key == key, "row %d (%s %s): key %016llx, CSymbol64 says %016llx", g * 3 + s, guns[g], subs[s], (unsigned long long)r.key,
            (unsigned long long)key);
      CHECK(r.id == Fnv1(name.c_str()), "row %d (%s): id %08x, FNV-1 32 of %s is %08x", g * 3 + s, guns[g], r.id, name.c_str(),
            Fnv1(name.c_str()));
    }
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 1; j < n; ++j) CHECK(rows[i].key != rows[j].key, "duplicate key rows %zu %zu", i, j);
  CHECK(impactsnd::kDefaultSym == EchoVR::CSymbol64Hash("default") && impactsnd::kDefaultSym == kDefaultSym,
        "default symbol %016llx", (unsigned long long)EchoVR::CSymbol64Hash("default"));
  CHECK(rows[0].id != kStock && rows[5].id != kStock, "a known id equals the stock id");
}

// ---- the install: what it checks, what it hooks, what it never writes ----

static void TestInstall() {
  Fresh();
  g_code_before.clear();
  for (int i = 0; i < 4; ++i) g_code_before.push_back(R32(kCodeVa[i]));
  const nevr_quest::combat::ModuleResult r = Install();
  CHECK(r.installed && std::strcmp(r.status, "installed") == 0 && !r.off, "install: %s", r.status);
  CHECK(R64(kDefaultVa) == kDefaultSym, "D reads %016llx", (unsigned long long)R64(kDefaultVa));
  CHECK(R64(kSlotVa) != g_base + kFnVa && R64(kSlotVa) != 0, "load slot not hooked");
  CHECK(R64(kShutSlotVa) != g_base + kShutFnVa && R64(kShutSlotVa) != 0, "Shutdown slot not hooked");
  for (int i = 0; i < 4; ++i) CHECK(R32(kCodeVa[i]) == g_code_before[i], "hit-code word %d changed", i);
  CHECK(R64(kSettingsGotVa) == g_base + kSettingsVa, "GOT word changed");
  CHECK(HasLine("impactsnd on") && HasLine("matches=1") && HasLine("value=2fd38a995f615b9e"), "no D line");
  CHECK(HasLine("impactsnd load wrapper") && HasLine("status=ok", 1), "no load hook line");
  CHECK(impactsnd::GetStats().on && impactsnd::GetStats().writersOk, "not on / writers off");
  CHECK(HasLine("impactsnd shutdown wrapper") && HasLine("row_writes=enabled", 1), "no Shutdown hook line");
  CHECK(CountLines("\"event\":\"combat_impactsnd_install\"", 1) == 1 && HasLine("result=installed") && HasLine("row_writes=enabled") && HasLine("rebuild=stock"),
        "no install line (rebuild is stock here: the fake image pins none of the loader's callee stubs)");
  // a D that is not the "default" symbol but is not 0 is left alone, and the line says it does not match
  Fresh();
  W64(kDefaultVa, 0x1111222233334444ull);
  Install();
  CHECK(R64(kDefaultVa) == 0x1111222233334444ull, "a non-zero D was overwritten");
  CHECK(HasLine("impactsnd on") && HasLine("matches=0") && HasLine("value=1111222233334444"), "no D line");
}

// combat.impactsnd false: nothing is touched, nothing is hooked, the module answers off and says why
static void TestOffByTheSwitch() {
  Fresh();
  const nevr_quest::combat::ModuleResult r = impactsnd::InstallAt(false, true, &Lookup);
  CHECK(!r.installed && std::strcmp(r.status, "off_config") == 0, "off: %s", r.status);
  CHECK(R64(kSlotVa) == g_base + kFnVa && R64(kShutSlotVa) == g_base + kShutFnVa && !impactsnd::GetStats().on, "off: a slot was hooked");
  CHECK(HasLine("result=off") && HasLine("reason=off_config"), "off: no line");
  SetTable(All(), kStock, false);
  Publish();
  Polls(12);
  CHECK(CountRight() == 0 && impactsnd::GetStats().polls == 0, "off: the tick ran");
}

// combat.impactsnd_write false: the module reads and logs the table and writes nothing to it; the loader hook and the tick still run
static void TestWriteSwitchOff() {
  Fresh();
  const nevr_quest::combat::ModuleResult r = Install(false);
  CHECK(r.installed, "read-only install: %s", r.status);
  CHECK(HasLine("rebuild=stock") && HasLine("row_writes=disabled"), "read-only: the rebuilt loader or the writes are on");
  CHECK(R64(kSlotVa) != g_base + kFnVa, "read-only: the load hook (an observer) must still be installed");
  impactsnd::SetScopedForTest(true);
  // a table is found and logged, never written, and the line comes once
  SetTable(All(), kStock, false);
  Publish();
  Polls(20);
  CHECK(AllRight() == false && CountRight() == 0, "read-only: ids written (%zu right)", CountRight());
  CHECK(CountLines("mode=read_only_switch") == 1, "read-only: %d lines", CountLines("mode=read_only_switch"));
  CHECK(impactsnd::GetStats().rowsWritten == 0, "read-only: rowsWritten %u", impactsnd::GetStats().rowsWritten);
  // and through the load handler
  Fresh();
  Install(false);
  CallLoader();
  CHECK(CountRight() == 0 && g_loader_calls == 1 && g_n == 15, "read-only handler: ids written / loader not called");
  CHECK(CountLines("mode=read_only_switch") == 1 && HasLine("when=load/unsorted"), "read-only handler: no read line");
}

// No pthread key left for the Shutdown handler's per-thread depth: the install refuses before it hooks anything. Runs before any successful
// install, because a key made by one stays for the process.
static void TestNoThreadKey() {
  // a module refused for a word that is not the pinned one has made no key: the install below, with no key left, must still be refused
  Fresh();
  *reinterpret_cast<uint32_t*>(g_base + kCodeVa[0]) = kCodeWord[0] ^ 0x1;
  CHECK(std::strcmp(Install().status, "hit_code_word_mismatch") == 0, "the pin mismatch was not refused");
  Fresh();
  KeyHog hog;
  const nevr_quest::combat::ModuleResult r = Install();
  CHECK(!r.installed && std::strcmp(r.status, "no_thread_key") == 0, "no key: %s", r.status);
  CHECK(R64(kSlotVa) == g_base + kFnVa && R64(kShutSlotVa) == g_base + kShutFnVa, "no key: a slot was hooked");
  const std::string code = "error=" + std::to_string(EAGAIN);  // what pthread_key_create answers when the process has no key left
  CHECK(HasLine("impactsnd OFF: nothing written", 3) && HasLine("reason=no_thread_key") && HasLine(code.c_str()), "no key: not loud, or no error code");
  for (const auto& l : g_lines)
    CHECK(l.text.find("combat_impactsnd_refused") == std::string::npos || (l.text.find("\"vaddr\"") == std::string::npos && l.text.find("\"pinned\"") == std::string::npos),
          "no key: the line carries address fields that mean nothing here");
  SetTable(All(), kStock, false);
  Publish();
  Polls(8);
  CHECK(impactsnd::GetStats().polls == 0 && !impactsnd::GetStats().on && CountRight() == 0, "no key: the tick ran");
}

static void TestRefusals() {
  for (int w = 0; w < 4; ++w) {  // each of the four hit-code words, wrong in turn
    Fresh();
    *reinterpret_cast<uint32_t*>(g_base + kCodeVa[w]) = kCodeWord[w] ^ 0x1;
    const nevr_quest::combat::ModuleResult r = Install();
    CHECK(!r.installed && std::strcmp(r.status, "hit_code_word_mismatch") == 0, "word %d: %s", w, r.status);
    CHECK(R64(kSlotVa) == g_base + kFnVa && R64(kShutSlotVa) == g_base + kShutFnVa, "word %d: slot hooked", w);
    CHECK(HasLine("impactsnd OFF: nothing written", 3) && HasLine("written=0"), "word %d: not loud", w);
    SetTable(All(), kStock, false);
    Publish();
    Polls(8);
    CHECK(impactsnd::GetStats().polls == 0 && !impactsnd::GetStats().on && CountRight() == 0, "word %d: the tick ran", w);
  }
  // the code page unreadable: refused, no fault
  Fresh();
  mprotect(reinterpret_cast<void*>((g_base + kCodeVa[0]) & ~0xfffull), 0x1000, PROT_NONE);
  CHECK(!Install().installed && R64(kSlotVa) == g_base + kFnVa, "unreadable code page not refused");
  mprotect(reinterpret_cast<void*>((g_base + kCodeVa[0]) & ~0xfffull), 0x1000, PROT_READ | PROT_WRITE);
  // the kSoundSettings GLOB_DAT word does not point at the object this module reads
  Fresh();
  W64(kSettingsGotVa, g_base + kSettingsVa + 8);
  const nevr_quest::combat::ModuleResult r = Install();
  CHECK(!r.installed && std::strcmp(r.status, "ksoundsettings_got_mismatch") == 0 && R64(kSlotVa) == g_base + kFnVa, "GOT mismatch: %s", r.status);
  // D unreadable
  Fresh();
  mprotect(reinterpret_cast<void*>((g_base + kDefaultVa) & ~0xfffull), 0x1000, PROT_NONE);
  CHECK(std::strcmp(Install().status, "default_symbol_unreadable") == 0, "unreadable D not refused");
  mprotect(reinterpret_cast<void*>((g_base + kDefaultVa) & ~0xfffull), 0x1000, PROT_READ | PROT_WRITE);
  // D reads 0: the hit code's second lookup cannot match and no row write repairs it, so the module is refused, and D is not written
  Fresh();
  W64(kDefaultVa, 0);
  const nevr_quest::combat::ModuleResult r2 = Install();
  CHECK(!r2.installed && std::strcmp(r2.status, "default_symbol_zero") == 0 && R64(kDefaultVa) == 0 && R64(kSlotVa) == g_base + kFnVa, "D 0: %s", r2.status);
  // no libr15 loaded
  Fresh();
  CHECK(std::strcmp(impactsnd::InstallAt(true, true, nullptr).status, "module_not_loaded") == 0, "no lookup not refused");
}

// The load slot holds something that is not the loader: refused, left alone; the Shutdown hook (installed first) stays, and the tick still
// repairs a combat level's table.
static void TestSlotRefused() {
  Fresh();
  W64(kSlotVa, 0x4242420);  // not the loader
  const nevr_quest::combat::ModuleResult r = Install();
  CHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0, "foreign slot: %s", r.status);
  CHECK(R64(kSlotVa) == 0x4242420, "foreign slot word changed");
  CHECK(R64(kDefaultVa) == kDefaultSym, "D changed");
  CHECK(HasLine("impactsnd load wrapper") && HasLine("mode=tick_only", 3), "no ERROR for the refused load hook");
  // tick-only mode works: with no load handler to mark a combat level's table the tick asks the levels itself; sorted table, two stable polls
  SetTable(All(), kStock, false);
  PublishRaw();
  Polls(12);
  CHECK(AllRight(), "tick-only: ids not written (%zu right)", CountRight());
  // ... and an arena match in the same mode is left alone
  Fresh();
  LoadLevels({"mpl_arena_a"});
  impactsnd::SetGameForTest(GameAddress());
  W64(kSlotVa, 0x4242420);
  Install();
  SetTable(All(), kStock, false);
  PublishRaw();
  Polls(40);
  CHECK(CountRight() == 0 && impactsnd::GetStats().polls == 0, "tick-only mode in an arena match wrote (%zu right, %u polls)", CountRight(), impactsnd::GetStats().polls);
}

static void TestShutdownSlotRefused() {
  Fresh();
  W64(kShutSlotVa, 0x4242420);  // not Shutdown
  const nevr_quest::combat::ModuleResult r = Install();
  CHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0, "foreign Shutdown slot: %s", r.status);
  CHECK(R64(kShutSlotVa) == 0x4242420, "foreign slot word changed");
  CHECK(CountLines("impactsnd shutdown wrapper", 3) == 1 && HasLine("row_writes=disabled", 3), "no ERROR for the refused Shutdown hook");
  CHECK(!impactsnd::GetStats().writersOk && R64(kDefaultVa) == kDefaultSym, "writersOk / D");
  // the module reads and logs, never writes: through the load handler and through the tick
  impactsnd::SetScopedForTest(true);
  CallLoader();
  CHECK(CountRight() == 0 && HasLine("mode=read_only_no_shutdown_handler"), "loader pass wrote with writers disabled (%zu)", CountRight());
  SetTable(All(), kStock, false);
  Publish();
  Polls(24);
  CHECK(CountRight() == 0 && impactsnd::GetStats().rowsWritten == 0, "tick wrote with writers disabled (%zu)", CountRight());
  CHECK(CountLines("mode=read_only_no_shutdown_handler") == 1, "read-only line %d times", CountLines("mode=read_only_no_shutdown_handler"));
}

// ---- the scope: arena shares the APK, so only a combat level is repaired ----

static const char* const kNotCombat[] = {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a", "mpl_lobby_b", "mpl_lobby_b2", "mpl_lobby_b_arena",
                                         "mpl_lobby_b_social"};

// A load in a level that is not a combat one runs the game's loader and nothing else: the table keeps the game's own rows and ids.
static void TestArenaLoadIsTheGamesOwn() {
  for (const char* level : kNotCombat) {
    Fresh();
    LoadLevels({level});
    Install();
    g_load_mode = kLoad15;
    const uint64_t rv = CallLoader();
    CHECK(rv == kLoaderReturn && g_loader_calls == 1, "%s: the game's loader result %llx, calls %d", level, (unsigned long long)rv, g_loader_calls);
    CHECK(CountRight() == 0 && g_n == 15 && MapFlags() == 1u, "%s: the table was touched (%zu right)", level, CountRight());
    const impactsnd::Stats s = impactsnd::GetStats();
    CHECK(s.loads == 1 && s.scopedLoads == 0 && s.passes == 0 && !s.scoped, "%s: stats l%u s%u p%u scoped %d", level, s.loads, s.scopedLoads, s.passes, s.scoped);
    CHECK(HasLine("\"event\":\"combat_impactsnd_scope\"") && HasLine("scope=other_level") && HasLine("loader=game"), "%s: no scope line", level);
    CHECK(!HasLine("\"event\":\"combat_impactsnd_load\"") && !HasLine("\"event\":\"combat_impactsnd_ids\""), "%s: a combat line in an arena load", level);
    Polls(40);  // the tick sleeps: no poll, no read of the table
    CHECK(impactsnd::GetStats().polls == 0 && CountRight() == 0, "%s: the tick polled (%u)", level, impactsnd::GetStats().polls);
    SetTable(All(), kStock, false);
    Polls(40);
    CHECK(impactsnd::GetStats().polls == 0 && CountRight() == 0, "%s: the tick wrote a table nobody marked", level);
  }
}

// The levels that are combat's: the four maps and the combat lobby.
static void TestCombatLevelsAreRepaired() {
  for (const char* level : {"mpl_combat_dyson", "mpl_combat_combustion", "mpl_combat_fission", "mpl_combat_gauss", "mpl_lobby_b_combat"}) {
    Fresh();
    LoadLevels({level});
    impactsnd::SetGameForTest(GameAddress());
    Install();
    CallLoader();
    CHECK(AllRight() && CountRight() == 15, "%s: %zu of 15 right", level, CountRight());
    const impactsnd::Stats s = impactsnd::GetStats();
    CHECK(s.loads == 1 && s.scopedLoads == 1 && s.scoped, "%s: stats l%u s%u scoped %d", level, s.loads, s.scopedLoads, s.scoped);
    CHECK(HasLine("scope=combat") && HasLine("loader=stock"), "%s: no scope line", level);
  }
}

// An Echo Arena level that is loaded wins over a combat level still loaded during a transition; no game seen yet, and a game that cannot be read, are no level.
static void TestScopeFailsClosed() {
  Fresh();
  LoadLevels({"mpl_combat_dyson", "mpl_arena_a"});
  impactsnd::SetGameForTest(GameAddress());
  Install();
  CallLoader();
  CHECK(CountRight() == 0 && !impactsnd::GetStats().scoped && HasLine("scope=other_level"), "a transition with an arena level was repaired");
  Fresh();
  Install();
  impactsnd::SetGameForTest(0);  // the tick has not run yet
  CallLoader();
  CHECK(CountRight() == 0 && !impactsnd::GetStats().scoped && HasLine("scope=no_game") && g_loader_calls == 1, "no game seen: repaired, or the loader skipped");
  Fresh();
  Install();
  impactsnd::SetGameForTest(0x1000);  // an address nothing is mapped at
  CallLoader();
  CHECK(CountRight() == 0 && !impactsnd::GetStats().scoped && HasLine("scope=other_level") && g_loader_calls == 1, "an unreadable game was repaired");
}


// In tick-only mode (no load handler) the tick asks the levels once a second, not on every poll.
static void TestTickOnlyAsksOnceASecond() {
  Fresh();
  LoadLevels({"mpl_arena_a"});
  impactsnd::SetGameForTest(GameAddress());
  W64(kSlotVa, 0x4242420);
  Install();
  SetTable(All(), kStock, false);
  PublishRaw();
  impactsnd::Tick(GameAddress());  // t = 100: asked, an arena level
  LoadLevels({"mpl_combat_dyson"});
  g_now = 100.25;
  impactsnd::Tick(GameAddress());  // not asked again yet
  CHECK(!impactsnd::GetStats().scoped, "the levels were asked twice inside the second");
  g_now = 101.0;
  impactsnd::Tick(GameAddress());
  CHECK(impactsnd::GetStats().scoped, "the levels were not asked after the second");
}

// One scope line a load, up to the budget, then one Warning.
static void TestScopeLineBudget() {
  Fresh();
  LoadLevels({"mpl_arena_a"});
  Install();
  for (int i = 0; i < 20; ++i) CallLoader();
  CHECK(CountLines("\"event\":\"combat_impactsnd_scope\"") == 16, "%d scope lines (cap 16)", CountLines("\"event\":\"combat_impactsnd_scope\""));
  CHECK(CountLines("log cap reached", 2) == 1 && HasLine("lines=scope"), "scope cap not announced once");
}

// ---- the rebuilt loader (R16): the callees are host functions here (the stock stubs are arm64 code); the pinned stub words are in the image ----
alignas(16) static uint8_t g_bigMap[0x80];  // kSoundSettings' pointee: the impact table at +0 and the whizzby table at +0x40
static uint8_t g_nodes[64];
static const char* const kGuns[5] = {"assault", "scout", "rocket", "rifle", "smg"};
static const char* const kMembers[3] = {"player", "barrier", "default"};
static const char* const kWhiz[4] = {"whizzby_a", "whizzby_b", "whizzby_c", "whizzby_d"};
static int g_jsonRc = 0;
static int g_dtorCalls = 0, g_loadCalls = 0;
// node layout: g_nodes[0..4] guns, [8 + 3 * g + m] members, [32 + w] whizzby
static const void* Node(int i) { return &g_nodes[i]; }
static int NodeOf(const void* p) { return p == nullptr ? -1 : static_cast<int>(static_cast<const uint8_t*>(p) - g_nodes); }
static void JCtor(void*) {}
static void JDtor(void*) { ++g_dtorCalls; }
static int JLoad(void*, const char* path) {
  ++g_loadCalls;
  return std::strcmp(path, "/json/r14/config|mp_sound_settings.json") == 0 ? g_jsonRc : 99;
}
static void* JNext(void*, const char* name, const void* from) {
  const int at = NodeOf(from);
  if (std::strcmp(name, "bullet_impact") == 0) {
    const int n = from == nullptr ? 0 : at + 1;
    return n < 5 ? const_cast<void*>(Node(n)) : nullptr;
  }
  if (std::strcmp(name, "bullet_whizzby") == 0) {
    const int n = from == nullptr ? 0 : at - 32 + 1;
    return n < 4 ? const_cast<void*>(Node(32 + n)) : nullptr;
  }
  for (int g = 0; g < 5; ++g) {
    char want[64];
    std::snprintf(want, sizeof want, "bullet_impact|%s", kGuns[g]);
    if (std::strcmp(name, want) != 0) continue;
    const int n = from == nullptr ? 0 : at - (8 + 3 * g) + 1;
    return n < 3 ? const_cast<void*>(Node(8 + 3 * g + n)) : nullptr;
  }
  return nullptr;
}
static const char* JNameAt(void*, const void* node) {
  const int i = NodeOf(node);
  if (i < 5) return kGuns[i];
  if (i >= 32) return kWhiz[i - 32];
  return kMembers[(i - 8) % 3];
}
static const char* JTString(void*, const void* node, const char*) {
  static char buf[8][64];
  static int k = 0;
  const int i = NodeOf(node);
  char* out = buf[k++ & 7];
  if (i >= 32) {
    std::snprintf(out, 64, "play_whizzby");
    return out;
  }
  const int g = (i - 8) / 3, m = (i - 8) % 3;
  std::snprintf(out, 64, "play_bullet_impact_%s%s", m == 0 ? "player" : m == 1 ? "shield" : "geo", g == 4 ? "_quark" : "");
  return out;
}
static void JResize(void* table, uint32_t bytes, int) {
  auto* w = static_cast<uint64_t*>(table);
  uint8_t* fresh = static_cast<uint8_t*>(std::calloc(1, bytes));
  if (w[0] != 0) std::memcpy(fresh, reinterpret_cast<void*>(w[0]), w[6] * 16);  // leaks the old block: a test
  w[0] = reinterpret_cast<uint64_t>(fresh);
}
static std::vector<std::pair<uint64_t, uint32_t>> RowsOf(uintptr_t table) {
  const auto* w = reinterpret_cast<const uint64_t*>(table);
  std::vector<std::pair<uint64_t, uint32_t>> out;
  for (uint64_t i = 0; i < w[6]; ++i) {
    const uint8_t* row = reinterpret_cast<const uint8_t*>(w[0]) + 16 * i;
    uint64_t k;
    uint32_t id;
    std::memcpy(&k, row, 8);
    std::memcpy(&id, row + 8, 4);
    out.emplace_back(k, id);
  }
  return out;
}
static void SetupRebuild() {
  Fresh();
  std::memset(g_bigMap, 0, sizeof g_bigMap);
  auto* w = reinterpret_cast<uint64_t*>(g_bigMap);
  w[4] = 4;  // growth
  reinterpret_cast<uint64_t*>(g_bigMap + 0x40)[4] = 4;
  g_jsonRc = 0;
  g_dtorCalls = g_loadCalls = 0;
  using nevr_quest::combat::impactsnd::kRebuildCallees;
  for (const auto& c : kRebuildCallees) *reinterpret_cast<uint32_t*>(g_base + c.elf) = c.firstWord;
  void* fns[7] = {reinterpret_cast<void*>(&JCtor), reinterpret_cast<void*>(&JDtor), reinterpret_cast<void*>(&JLoad), reinterpret_cast<void*>(&JNext),
                  reinterpret_cast<void*>(&JNameAt), reinterpret_cast<void*>(&JTString), reinterpret_cast<void*>(&JResize)};
  for (size_t i = 0; i < 7; ++i) impactsnd::SetCalleeForTest(i, fns[i]);
}
static void PublishBig() { W64(kSettingsVa, reinterpret_cast<uint64_t>(g_bigMap)); }

// In a combat level the rebuilt loader runs in place of the game's: all 15 rows and the 4 whizzby rows, right ids, the scope marked.
static void TestRebuiltLoaderInACombatLevel() {
  SetupRebuild();
  CHECK(Install().installed && HasLine("rebuild=reconstructed"), "the rebuild is not on with every stub pinned and the Shutdown handler in");
  PublishBig();  // CR15NetBulletCS::Initialize stores the table before it calls the loader
  const uint64_t rv = CallLoader();
  CHECK(rv == 0 && g_loader_calls == 0, "the game's loader ran (%d) or the result %llx", g_loader_calls, (unsigned long long)rv);
  const auto rows = RowsOf(reinterpret_cast<uintptr_t>(g_bigMap));
  size_t n = 0;
  const impactsnd::Row* known = Known(&n);
  bool right = rows.size() == 15;
  for (size_t i = 0; right && i < 15; ++i) {
    right = false;
    for (size_t j = 0; j < n; ++j) right = right || (known[j].key == rows[i].first && known[j].id == rows[i].second);
    if (!right) break;
    right = true;
  }
  CHECK(right, "the rebuilt table has the wrong rows (%zu)", rows.size());
  CHECK(RowsOf(reinterpret_cast<uintptr_t>(g_bigMap) + 0x40).size() == 4, "the whizzby table has %zu rows", RowsOf(reinterpret_cast<uintptr_t>(g_bigMap) + 0x40).size());
  CHECK(g_loadCalls == 1 && g_dtorCalls == 1, "the JSON was loaded %d times, destroyed %d", g_loadCalls, g_dtorCalls);
  CHECK(impactsnd::GetStats().scoped && impactsnd::GetStats().scopedLoads == 1, "the scope was not marked by the rebuilt load");
  CHECK(HasLine("\"event\":\"combat_impactsnd_rebuilt\"") && HasLine("rows=19") && HasLine("json_result=0") && HasLine("scope=combat") && HasLine("loader=rebuilt"),
        "no rebuilt line");
  // the JSON file missing: its result is the loader's result, no row is made
  SetupRebuild();
  Install();
  PublishBig();
  g_jsonRc = 7;
  CHECK(CallLoader() == 7 && RowsOf(reinterpret_cast<uintptr_t>(g_bigMap)).empty() && g_dtorCalls == 1, "a missing JSON file");
}

// In every other level the game's own loader runs and the rebuilt one does not.
static void TestRebuiltLoaderNotInArena() {
  SetupRebuild();
  LoadLevels({"mpl_arena_a"});
  impactsnd::SetGameForTest(GameAddress());
  Install();
  PublishBig();
  const uint64_t rv = CallLoader();
  CHECK(rv == kLoaderReturn && g_loader_calls == 1 && g_loadCalls == 0, "arena: the game's loader %d, the JSON loads %d", g_loader_calls, g_loadCalls);
  CHECK(RowsOf(reinterpret_cast<uintptr_t>(g_bigMap)).empty() || g_loader_calls == 1, "arena rows");
  CHECK(!impactsnd::GetStats().scoped, "arena: scoped");
}

// The rebuild is off with the write switch off, without the Shutdown handler, and with a stub that is not the pinned one.
static void TestRebuiltLoaderNeedsItsConditions() {
  SetupRebuild();
  Install(false);
  CHECK(HasLine("rebuild=stock"), "write switch off: the rebuild is on");
  CallLoader();
  CHECK(g_loader_calls == 1 && g_loadCalls == 0, "write switch off: the rebuilt loader ran");
  SetupRebuild();
  W64(kShutSlotVa, 0x4242420);
  Install();
  CHECK(HasLine("rebuild=stock"), "no Shutdown handler: the rebuild is on");
  CallLoader();
  CHECK(g_loader_calls == 1 && g_loadCalls == 0, "no Shutdown handler: the rebuilt loader ran");
  SetupRebuild();
  *reinterpret_cast<uint32_t*>(g_base + nevr_quest::combat::impactsnd::kRebuildCallees[3].elf) ^= 1;
  Install();
  CHECK(HasLine("rebuild=stock") && HasLine("missing_callee=CJson::Next"), "a stub that is not pinned: the rebuild is on");
  CallLoader();
  CHECK(g_loader_calls == 1 && g_loadCalls == 0, "an unpinned stub: the rebuilt loader ran");
}

// The loader runs while the level is still loading: the gamespace is in the init-fiber table and not yet in the loaded tables (the real order). A combat map
// that is loading is repaired, an arena level that is loading is not, and a combat map loading while an arena lobby is loaded is repaired too.
static void TestScopeSeesALevelStillLoading() {
  Fresh();
  LoadLevels({});
  PutInFlight("mpl_combat_dyson");
  impactsnd::SetGameForTest(GameAddress());
  Install();
  CallLoader();
  CHECK(impactsnd::GetStats().scoped && impactsnd::GetStats().scopedLoads == 1 && AllRight() && HasLine("scope=combat"), "a combat map that is loading was not repaired");
  Fresh();
  LoadLevels({"mpl_lobby_b"});
  PutInFlight("mpl_combat_gauss");
  impactsnd::SetGameForTest(GameAddress());
  Install();
  CallLoader();
  CHECK(impactsnd::GetStats().scoped, "a combat map loading beside an arena lobby was not repaired");
  Fresh();
  LoadLevels({});
  PutInFlight("mpl_arena_a");
  impactsnd::SetGameForTest(GameAddress());
  Install();
  CallLoader();
  CHECK(!impactsnd::GetStats().scoped && CountRight() == 0 && HasLine("scope=other_level"), "an arena level that is loading was repaired");
  Fresh();
  LoadLevels({"mpl_combat_dyson"});
  PutInFlight("mpl_arena_b");
  impactsnd::SetGameForTest(GameAddress());
  Install();
  CallLoader();
  CHECK(!impactsnd::GetStats().scoped && CountRight() == 0, "an arena level loading beside a combat map was repaired");
}

// An arena match pays nothing, by count (not only by what the game receives): the tick, the Shutdown handler and the quiet frames make no read and no
// scan; a load makes the one scan that decides it (the decision once per level change) and nothing besides it.
static void TestArenaPaysNothingByCount() {
  Fresh();
  LoadLevels({"mpl_arena_a"});
  impactsnd::SetGameForTest(GameAddress());
  Install();
  g_load_mode = kLoadNothing;
  const uint64_t reads0 = ReadCount(), scans0 = ScanCount();
  for (int i = 0; i < 300; ++i) impactsnd::Tick(GameAddress());
  CHECK(ReadCount() == reads0 && ScanCount() == scans0,
        "300 arena frames made %llu reads and %llu scans", (unsigned long long)(ReadCount() - reads0),
        (unsigned long long)(ScanCount() - scans0));
  for (int i = 0; i < 5; ++i) CallShutdown(1);
  CHECK(CountLines("role=shutdown") == 0, "an arena teardown noted its thread (a gettid syscall)");
  CHECK(ReadCount() == reads0 && ScanCount() == scans0,
        "5 arena Shutdowns made %llu reads and %llu scans", (unsigned long long)(ReadCount() - reads0),
        (unsigned long long)(ScanCount() - scans0));
  // one load: exactly the reads of the one scan that decides it
  std::uint64_t level = 0;
  const uint64_t r1 = ReadCount();
  nevr_quest::combat::level::Classify(GameAddress(), true, &level, true);
  const uint64_t readsOfOneDecision = ReadCount() - r1;
  const uint64_t reads1 = ReadCount(), scans1 = ScanCount();
  CallLoader();
  CHECK(ScanCount() - scans1 == 1, "an arena load made %llu scans", (unsigned long long)(ScanCount() - scans1));
  CHECK(ReadCount() - reads1 == readsOfOneDecision, "an arena load made %llu reads, its one decision costs %llu",
        (unsigned long long)(ReadCount() - reads1), (unsigned long long)readsOfOneDecision);
  // and after a combat level's table is live, the Shutdown that frees it reads the global once to end the scope
  LoadLevels({"mpl_combat_dyson"});
  impactsnd::SetGameForTest(GameAddress());
  g_load_mode = kLoad15;
  CallLoader();
  const uint64_t reads2 = ReadCount();
  CallShutdown(1);
  CHECK(ReadCount() > reads2 && !impactsnd::GetStats().scoped, "the Shutdown of a combat table did not end the scope by a read");
}

// A non-combat load reaches the handler while a combat table lives (the global stays set). The game's own caller does not do this (Initialize loads only
// when no bullet system lives, docs/quest/combat/impactsnd.md "Scope and arena cost"); the handler fails closed: the scope ends with that load, the tick does
// not poll, and writes no id into the table the load filled.
static void TestAnArenaLoadEndsTheScopeOfAnEarlierCombatLoad() {
  Fresh();
  Install();
  CallLoader();  // a combat load: scoped
  CHECK(impactsnd::GetStats().scoped, "the combat load did not mark the scope");
  g_shut_mode = kShutHold;  // a Shutdown that leaves the table, as when another bullet system still lives
  g_hold_release = true;
  CallShutdown(1);
  CHECK(impactsnd::GetStats().scoped, "a Shutdown that left the table cleared the scope");
  LoadLevels({"mpl_arena_a"});
  impactsnd::SetGameForTest(GameAddress());
  g_shut_mode = kShutFree;
  g_load_mode = kLoad5;  // the arena lobby's stock load: five rows, every id the stock one, unsorted
  CallLoader();
  CHECK(!impactsnd::GetStats().scoped && HasLine("scope=other_level"), "the arena load left the earlier combat scope on");
  SortRows();  // the first hit's sort
  const uint32_t polls0 = impactsnd::GetStats().polls;
  Polls(40);
  CHECK(impactsnd::GetStats().polls == polls0 && CountRight() == 0, "the tick polled the arena table (%u polls) or wrote ids into it (%zu right)",
        impactsnd::GetStats().polls - polls0, CountRight());
}

// An arena space that starts while a combat one lives shares the combat level's table: the game makes it for the first bullet system and frees it with the
// last, and runs no load for the second (docs/quest/combat/impactsnd.md, "Scope and arena cost"). The tick asks the levels once a second while the table is in
// scope; within that second of the arena level appearing every row this module wrote holds the stock id again, one record says so, and the tick stops polling.
static void TestAnArenaSpaceBesideACombatTableGetsTheStockIds() {
  Fresh();
  Install();
  CallLoader();  // the combat load: the stock loader's 15 rows, repaired after the load
  CHECK(impactsnd::GetStats().scoped && AllRight(), "the combat load was not repaired");
  Polls(8);  // two seconds of the combat match: still in scope, still repaired
  CHECK(impactsnd::GetStats().scoped && AllRight() && impactsnd::GetStats().unscoped == 0, "the combat match lost its scope");
  const std::vector<Row> before = Snap();
  LoadLevels({"mpl_combat_dyson", "mpl_arena_a"});  // the arena space starts; no load runs, the table stays
  g_rows[3].id = 0x777;                              // a row whose id is not one of ours: never touched
  Polls(4);                                          // one second
  CHECK(!impactsnd::GetStats().scoped, "the scope outlived the combat level's place: an arena level is loaded");
  size_t stock = 0;
  for (size_t i = 0; i < g_n; ++i) stock += g_rows[i].id == kStock;
  CHECK(stock == 14 && g_rows[3].id == 0x777 && KeysAndPadsIntact(before) && g_n == 15 && MapCount() == 15,
        "after the arena space started: %zu of 15 rows hold the stock id, the foreign id %x", stock, g_rows[3].id);
  CHECK(CountLines("\"event\":\"combat_impactsnd_unscoped\"", 1) == 1 && HasLine("\"rows_stock\":14,\"whizzby_stock\":0,\"mode\":\"written\""),
        "no one record of the scope's end");
  CHECK(impactsnd::GetStats().unscoped == 1 && impactsnd::GetStats().rowsRestored == 14, "stats: unscoped %u, restored %u", impactsnd::GetStats().unscoped,
        impactsnd::GetStats().rowsRestored);
  // the tick stops: no poll, no read and no scan in 40 more frames of the arena match
  const uint32_t polls0 = impactsnd::GetStats().polls;
  const uint64_t reads0 = ReadCount(), scans0 = ScanCount();
  Polls(40);
  CHECK(impactsnd::GetStats().polls == polls0 && ReadCount() == reads0 && ScanCount() == scans0, "the tick went on after the scope ended: %u polls, %llu reads, %llu scans",
        impactsnd::GetStats().polls - polls0, (unsigned long long)(ReadCount() - reads0), (unsigned long long)(ScanCount() - scans0));
  CHECK(stock == 14 && CountRight() == 0, "a poll wrote an id again");
  // the combat space leaves first and no level of combat's is loaded: the same end (the other Scope)
  Fresh();
  Install();
  CallLoader();
  Poll();
  LoadLevels({"mpl_lobby_b2"});
  Polls(4);
  CHECK(!impactsnd::GetStats().scoped && CountRight() == 0 && HasLine("scope=other_level"), "the scope outlived every combat level");
}

// The same with a table the rebuilt loader filled: every id in it is ours, the whizzby rows' too, so every row gets the stock id.
static void TestAnArenaSpaceBesideARebuiltTableGetsTheStockIds() {
  SetupRebuild();
  Install();
  PublishBig();
  CallLoader();
  CHECK(impactsnd::GetStats().scoped && RowsOf(reinterpret_cast<uintptr_t>(g_bigMap) + 0x40).size() == 4, "the rebuilt load");
  LoadLevels({"mpl_combat_dyson", "mpl_arena_b"});
  Polls(4);
  bool stock = true;
  for (const auto& row : RowsOf(reinterpret_cast<uintptr_t>(g_bigMap))) stock = stock && row.second == kStock;
  for (const auto& row : RowsOf(reinterpret_cast<uintptr_t>(g_bigMap) + 0x40)) stock = stock && row.second == kStock;
  CHECK(stock && RowsOf(reinterpret_cast<uintptr_t>(g_bigMap)).size() == 15 && !impactsnd::GetStats().scoped, "a rebuilt table kept an id of ours");
  CHECK(HasLine("\"rows_stock\":15,\"whizzby_stock\":4,\"mode\":\"written\""), "the record does not count the 15 rows and the 4 whizzby rows");
}

// The table lock is held (a Shutdown runs): the tick does not wait; the stock ids go back on the frame after it is free, and only then does the scope end.
static void TestTheStockIdsWaitForTheTableLock() {
  Fresh();
  Install();
  CallLoader();
  Poll();
  g_shut_mode = kShutHold;
  std::thread b([] { CallShutdown(0x55); });
  for (int i = 0; i < 400 && !g_in_shutdown; ++i) usleep(2000);
  CHECK(g_in_shutdown, "the holding Shutdown never started");
  LoadLevels({"mpl_combat_dyson", "mpl_arena_a"});
  const auto t0 = std::chrono::steady_clock::now();
  Polls(4);
  const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 150 && AllRight() && impactsnd::GetStats().scoped, "with the lock held: waited %ld ms, or wrote, or ended the scope", ms);
  g_hold_release = true;
  b.join();
  g_shut_mode = kShutFree;
  Poll();
  CHECK(!impactsnd::GetStats().scoped && CountRight() == 0, "the stock ids did not go back once the lock was free (%zu right)", CountRight());
}

// The table changes between the read and the lock (a row was added under us): nothing is written to it on that frame; the next frame reads it again.
static int g_unscopeHooks = 0;
static void HkUnscope(int stage) {
  if (stage == 5 && g_unscopeHooks++ == 0) MapCount() = 14;
}
static void TestTheStockIdsWaitForAStableTable() {
  Fresh();
  Install();
  CallLoader();
  Poll();
  LoadLevels({"mpl_combat_dyson", "mpl_arena_a"});
  g_unscopeHooks = 0;
  g_now = 101.0;  // the second's ask is due
  impactsnd::SetHookForTest(&HkUnscope);
  impactsnd::Tick(GameAddress());
  CHECK(g_unscopeHooks == 1 && AllRight() && impactsnd::GetStats().scoped, "a table that changed under the read got the stock ids, or the scope ended");
  impactsnd::Tick(GameAddress());
  impactsnd::SetHookForTest(nullptr);
  size_t stock = 0;
  for (size_t i = 0; i < 14; ++i) stock += g_rows[i].id == kStock;
  CHECK(!impactsnd::GetStats().scoped && stock == 14 && g_rows[14].id != kStock, "the next frame did not put the stock ids into the 14 rows it read (%zu)", stock);
}

// With the write switch off nothing of ours is in the table: the scope ends and nothing is written.
static void TestAReadOnlyScopeEndsWithoutAWrite() {
  Fresh();
  Install(false);
  CallLoader();
  CHECK(impactsnd::GetStats().scoped && CountRight() == 0, "read-only: written");
  LoadLevels({"mpl_combat_dyson", "mpl_arena_a"});
  Polls(4);
  CHECK(!impactsnd::GetStats().scoped && HasLine("\"rows_stock\":0,\"whizzby_stock\":0,\"mode\":\"read_only\""), "read-only: the scope did not end, or wrote");
}

// What the once-a-second ask costs, by count: in a combat match exactly one scan a second (10 in 10 s of frames); in an arena match none (the ask runs only
// while a combat level's table is in scope).
static void TestTheRecheckIsOneScanASecond() {
  Fresh();
  Install();
  CallLoader();
  const uint64_t scans0 = ScanCount();
  const uint32_t rechecks0 = impactsnd::GetStats().rechecks;
  for (int i = 0; i < 400; ++i) {  // 10 s of frames at 40 Hz
    impactsnd::Tick(GameAddress());
    g_now += 0.025;
  }
  CHECK(ScanCount() - scans0 == 10 && impactsnd::GetStats().rechecks - rechecks0 == 10, "10 s of a combat match: %llu scans, %u rechecks (want 10)",
        (unsigned long long)(ScanCount() - scans0), impactsnd::GetStats().rechecks - rechecks0);
  CHECK(impactsnd::GetStats().scoped && AllRight(), "the combat match lost its ids");
  Fresh();
  LoadLevels({"mpl_arena_a"});
  impactsnd::SetGameForTest(GameAddress());
  Install();
  const uint64_t scans1 = ScanCount();
  for (int i = 0; i < 400; ++i) {
    impactsnd::Tick(GameAddress());
    g_now += 0.025;
  }
  CHECK(ScanCount() == scans1 && impactsnd::GetStats().rechecks == 0, "10 s of an arena match: %llu scans", (unsigned long long)(ScanCount() - scans1));
}

// The tick hands the module the game; the scope is marked by a combat load and cleared when the game frees the table.
static void TestScopeFollowsTheTable() {
  Fresh();
  Install();
  impactsnd::SetGameForTest(0);
  impactsnd::Tick(GameAddress());  // the tick records the game
  CallLoader();
  CHECK(impactsnd::GetStats().scoped && AllRight(), "the game the tick recorded was not used");
  Polls(12);
  CHECK(impactsnd::GetStats().polls > 0, "a combat table is not watched");
  const uint32_t before = impactsnd::GetStats().polls;
  CallShutdown(1);  // the game frees the table and clears the global
  CHECK(!impactsnd::GetStats().scoped, "the scope outlived the table");
  Polls(12);
  CHECK(impactsnd::GetStats().polls == before, "the tick polled after the table was freed (%u -> %u)", before, impactsnd::GetStats().polls);
  // a Shutdown that leaves the global set (another system still holds the table) keeps the scope
  CallLoader();
  CHECK(impactsnd::GetStats().scoped, "a second combat load did not mark the scope");
  g_shut_mode = kShutHold;
  g_hold_release = true;
  CallShutdown(2);
  CHECK(impactsnd::GetStats().scoped, "a Shutdown that left the table cleared the scope");
}

static void TestWrapperUnsorted15() {
  Setup();
  g_load_mode = kLoad15;
  const uint64_t rv = CallLoader();
  CHECK(rv == kLoaderReturn, "return value %llx", (unsigned long long)rv);
  CHECK(g_loader_calls == 1, "loader called %d times", g_loader_calls);
  CHECK(AllRight() && CountRight() == 15, "after the load: %zu of 15 right", CountRight());
  std::vector<Row> after = Snap();
  size_t n = 0;
  const impactsnd::Row* k = Known(&n);
  bool order = after.size() == 15;
  for (size_t i = 0; order && i < 15; ++i) order = after[i].key == k[i].key && after[i].pad == kPad;
  CHECK(order, "keys, order or pads changed by the write");
  CHECK(MapFlags() == 1u && MapCount() == 15, "flags/count touched");
  const impactsnd::Stats s = impactsnd::GetStats();
  CHECK(s.loads == 1 && s.passes == 1 && s.rowsWritten == 15 && s.rowsLeft == 0, "stats l%u p%u w%u r%u", s.loads, s.passes, s.rowsWritten,
        s.rowsLeft);
  CHECK(HasLine("impactsnd LoadSoundSettings returned") && HasLine("word_patch=in_effect") && HasLine("rows=15"), "no loader line");
  CHECK(HasLine("impactsnd ids") && HasLine("when=load/unsorted") && HasLine("rewrote=15") && HasLine("found_ids=00000001,00000001"), "no ids line");
  // the tick afterwards does not repeat the unsorted pass; after the first hit's sort nothing is left to write
  Polls(10);
  CHECK(impactsnd::GetStats().passes == 1, "the tick repeated the unsorted pass: %u", impactsnd::GetStats().passes);
  SortRows();
  Polls(10);
  CHECK(AllRight() && impactsnd::GetStats().passes == 1 && CountLines("impactsnd ids") == 1, "sorted table: extra pass (%u)",
        impactsnd::GetStats().passes);
  // the table count line, from the tick: 15 rows = the word patch is in effect
  CHECK(HasLine("impactsnd table") && HasLine("word_patch=in_effect") && HasLine("default_symbol=2fd38a995f615b9e"), "no table line");
  CHECK(CountLines("impactsnd table") == 1, "table line %d times", CountLines("impactsnd table"));
}

static void TestWrapper5Rows() {  // the loader without the R16 words: only the five `player` rows
  Setup();
  g_load_mode = kLoad5;
  CallLoader();
  CHECK(g_n == 5 && CountRight() == 5, "5 rows: %zu right", CountRight());
  CHECK(HasLine("rows=5") && HasLine("word_patch=absent"), "no word_patch=absent line");
  Polls(12);
  CHECK(HasLine("impactsnd table") && HasLine("rows=5") && CountLines("word_patch=absent") == 2, "table line for 5 rows");
  CHECK(CountLines("impactsnd table", 2) == 0, "5 rows must not be a WARNING");
}

static void TestWrapperSortedAtLoad() {
  Setup();
  g_load_mode = kLoad15Sorted;
  CallLoader();
  CHECK(AllRight() && HasLine("when=load/sorted"), "sorted table at load: %zu right", CountRight());
}

static void TestWrapperForeignKey() {
  Setup();
  g_load_mode = kLoadForeign;
  CallLoader();
  CHECK(CountRight() == 0 && impactsnd::GetStats().passes == 0, "unsorted table with a key outside the 15 was written at load");
  Polls(12);
  CHECK(CountRight() == 0 && CountLines("keys outside the 15") == 1, "unsorted foreign table written by the tick / note %d times", CountLines("keys outside the 15"));
  // the first hit sorts it: now the known rows are written, the foreign row never
  SortRows();
  Polls(4);
  CHECK(CountRight() == 15 && IdOfKey(0xdeadbeefcafef00dull) == kStock, "sorted: %zu right, foreign id %08x", CountRight(),
        IdOfKey(0xdeadbeefcafef00dull));
  CHECK(KeysAndPadsIntact(Snap()), "snapshot");
}

static void TestWrapperNoTable() {
  Setup();
  g_load_mode = kLoadNothing;
  const uint64_t rv = CallLoader();
  CHECK(rv == kLoaderReturn && impactsnd::GetStats().loads == 1 && impactsnd::GetStats().passes == 0, "no table at load");
  CHECK(HasLine("impactsnd no table now") && HasLine("where=after LoadSoundSettings"), "no note");
}

static void TestTickCountLine() {
  Setup();
  Polls(8);
  CHECK(!HasLine("impactsnd table"), "count line before any table");
  SetTable(All(), kStock, false);
  Publish();
  Polls(7);  // first non-zero poll + 6 more = 1.5 s
  CHECK(!HasLine("impactsnd table"), "count line before the 2 s settle");
  Polls(3);
  CHECK(CountLines("impactsnd table") == 1, "count line %d times", CountLines("impactsnd table"));
  CHECK(HasLine("rows=15") && HasLine("map=set"), "count line fields");
  Polls(20);
  CHECK(CountLines("impactsnd table") == 1, "count line repeated");

  Setup();  // the table never appears: one line after 600 s with what is there
  g_now = 699.0;
  Poll();
  CHECK(!HasLine("impactsnd table"), "600 s line early");
  g_now = 700.0;
  Poll();
  CHECK(HasLine("impactsnd table") && HasLine("map=null") && HasLine("rows=0") && HasLine("word_patch=no_verdict"), "600 s line");

  Setup();  // an odd count is a WARNING
  SetTable({0, 1, 2, 3}, kStock, false);
  Publish();
  Polls(12);
  CHECK(HasLine("impactsnd table", 2) && HasLine("word_patch=unexpected"), "odd count not a WARNING");
}

static void TestPollGate() {
  Setup();
  SetTable(All(), kStock, false);
  Publish();
  impactsnd::Tick(GameAddress());
  impactsnd::Tick(GameAddress());
  g_now += 0.2;
  impactsnd::Tick(GameAddress());
  CHECK(impactsnd::GetStats().polls == 1, "polls %u within 0.25 s", impactsnd::GetStats().polls);
  g_now += 0.05;
  impactsnd::Tick(GameAddress());
  CHECK(impactsnd::GetStats().polls == 2, "polls %u after 0.25 s", impactsnd::GetStats().polls);
}

static void TestTickUnsorted() {
  Setup();
  SetTable(All(), kStock, true);
  Publish();
  Warm();
  Poll();
  CHECK(CountRight() == 0, "written on the first poll");
  Poll();
  CHECK(CountRight() == 15, "unsorted table not written on the second poll: %zu", CountRight());
  CHECK(HasLine("when=tick/unsorted"), "no tick/unsorted line");
  const uint32_t passes = impactsnd::GetStats().passes;
  Polls(8);
  CHECK(impactsnd::GetStats().passes == passes, "unsorted table passed more than once");
  // the engine puts the stock ids back into the same table (a reload at the same address) after it was gone: written again
  Unpublish();
  Polls(2);
  SetTable(All(), kStock, true);
  Publish();
  Polls(3);
  CHECK(CountRight() == 15 && impactsnd::GetStats().passes == passes + 1, "new table at the same address not rewritten (%zu right)", CountRight());
}

static void TestTickSorted() {
  Setup();
  SetTable(All(), kStock, false);
  Publish();
  Warm();
  Poll();
  CHECK(CountRight() == 0, "sorted table written on the first poll");
  Poll();
  CHECK(CountRight() == 15, "sorted table not written on the second poll");
  CHECK(HasLine("when=tick/sorted"), "no tick/sorted line");
  const std::vector<Row> before = Snap();
  Polls(6);
  CHECK(impactsnd::GetStats().passes == 1 && CountLines("impactsnd ids") == 1, "a pass with nothing to write logged");
  CHECK(KeysAndPadsIntact(before), "snapshot");
}

static void TestSortRaceFixups() {
  Setup();
  SetTable(All(), kStock, false);
  Publish();
  Warm();
  Polls(3);
  CHECK(AllRight(), "setup");
  // another thread's store landed in a neighbour row: row 0 holds row 1's id, row 4 holds the stock id again, and row 7 holds an
  // id that is none of ours (not this module's to touch)
  g_rows[0].id = g_rows[1].id;
  g_rows[4].id = kStock;
  g_rows[7].id = 0x777;
  const uint32_t want0 = WantOf(g_rows[0].key), want4 = WantOf(g_rows[4].key);
  Polls(4);
  CHECK(g_rows[0].id == want0 && g_rows[4].id == want4, "fix-ups: row0 %08x row4 %08x", g_rows[0].id, g_rows[4].id);
  CHECK(g_rows[7].id == 0x777, "a foreign id was overwritten: %08x", g_rows[7].id);
  CHECK(impactsnd::GetStats().rowsLeft == 1, "left %u", impactsnd::GetStats().rowsLeft);
}

static void TestRowsOutsideTable() {
  Setup();  // an unknown key in a sorted table is never written, its id stays
  SetTable(All(), kStock, false);
  g_rows[15] = {0x1234567890abcdefull, 0x9999, kPad};
  g_rows[16] = {0xfedcba0987654321ull, kStock, kPad};
  g_n = 17;
  MapCount() = 17;
  Publish();
  Warm();
  Polls(4);
  CHECK(CountRight() == 15 && g_rows[15].id == 0x9999 && g_rows[16].id == kStock, "foreign rows: %08x %08x", g_rows[15].id, g_rows[16].id);
}

static void TestTableShapes() {
  Setup();  // count over the cap: not a table
  SetTable(All(), kStock, false);
  MapCount() = 1000;
  Publish();
  Warm();
  Polls(4);
  CHECK(CountRight() == 0 && HasLine("impactsnd no table now") && HasLine("count=1000"), "count 1000: %d notes", CountLines("no table now"));
  Setup();  // a data pointer that is not 8-aligned
  SetTable(All(), kStock, false);
  MapData() = reinterpret_cast<uint64_t>(g_rows) + 4;
  Publish();
  Warm();
  Polls(4);
  CHECK(CountRight() == 0 && HasLine("impactsnd no table now"), "misaligned data pointer used");
  Setup();  // a data pointer into unmapped memory: no fault
  SetTable(All(), kStock, false);
  MapData() = 0x1234567000ull;
  Publish();
  Warm();
  Polls(8);
  CHECK(CountRight() == 0, "unmapped table written?");
  Setup();  // the map pointer itself is wild
  W64(kSettingsVa, 0x10);
  Polls(8);
  CHECK(CountRight() == 0 && g_lines.size() > 0, "wild map");
}

static void TestNowriteTick() {
  Fresh();
  Install(false);
  impactsnd::SetScopedForTest(true);
  SetTable(All(), kStock, true);
  Publish();
  Polls(24);
  CHECK(CountRight() == 0 && CountLines("impactsnd ids") == 1 && HasLine("found_ids=00000001"), "nowrite tick");
}

// the table changes between the first read and the re-read: no write on that poll
static int g_hook_stage1 = 0, g_hook_mode = 0, g_hook_stage2 = 0;
static bool g_hook_armed2 = false;
static int g_hook4_mode = 0;
alignas(16) static Row g_rows2[128];
static uint64_t g_hook_d = 0;
static void Hk(int stage) {
  if (stage == 1) {
    ++g_hook_stage1;
    if (g_hook_mode == 1) MapCount() = 14;                  // count changes
    if (g_hook_mode == 2) MapFlags() ^= 1u;                 // the sort bit flips (sorted<->unsorted)
    if (g_hook_mode == 3) MapData() += 16;                  // data pointer moves
    if (g_hook_mode == 4) Unpublish();                      // the table is freed
    if (g_hook_mode == 5) {                                 // the map object is replaced by another with the same contents
      std::memcpy(g_map2, g_map, sizeof g_map);
      W64(kSettingsVa, reinterpret_cast<uint64_t>(g_map2));
    }
    g_hook_mode = 0;
  }
  if (stage == 3 && g_hook_d != 0) W64(kDefaultVa, g_hook_d);  // something else stores into D before the read-back
  if (stage == 2 && g_freed) ++g_uaf;  // a row store after the table was freed
  if (stage == 4 && g_hook4_mode != 0) {
    const int mode = g_hook4_mode;
    g_hook4_mode = 0;
    if (mode == 1) CallShutdown(0x77);  // Shutdown runs between the pass's validation and its lock
    if (mode == 2) {                    // another table is published (a new map object, new rows)
      std::memcpy(g_map2, g_map, sizeof g_map);
      *reinterpret_cast<uint64_t*>(g_map2) = reinterpret_cast<uint64_t>(g_rows2);
      W64(kSettingsVa, reinterpret_cast<uint64_t>(g_map2));
    }
    if (mode == 3) MapCount() = 14;
    if (mode == 4) MapData() += 16;
    if (mode == 5) {                    // the same data and count under a new map object
      std::memcpy(g_map2, g_map, sizeof g_map);
      W64(kSettingsVa, reinterpret_cast<uint64_t>(g_map2));
    }
  }
  if (stage == 2 && g_hook_armed2) {
    ++g_hook_stage2;
    if (g_hook_stage2 == 1) g_rows[0].id = g_rows[1].id == kStock ? 0xd57a0ac6u : g_rows[1].id;  // another store lands in row 0 first
  }
}
static void TestChangedUnderUs() {
  for (int unsorted = 0; unsorted < 2; ++unsorted)
    for (int mode = 1; mode <= 5; ++mode) {
      Setup();
      SetTable(All(), kStock, unsorted != 0);
      Publish();
      Warm();
      impactsnd::SetHookForTest(&Hk);
      g_hook_stage1 = 0;
      Poll();
      g_hook_mode = mode;
      Poll();  // stable: the hook changes the table before the re-read
      CHECK(g_hook_stage1 == 1, "unsorted %d mode %d: hook ran %d times", unsorted, mode, g_hook_stage1);
      CHECK(CountRight() == 0, "unsorted %d mode %d: written although the table changed under the poll (%zu)", unsorted, mode, CountRight());
      CHECK(impactsnd::GetStats().skipped == 0 && impactsnd::GetStats().passes == 0,
            "unsorted %d mode %d: the poll's own re-read must stop it before a pass starts (skipped %u)", unsorted, mode,
            impactsnd::GetStats().skipped);
      // restore and let the polls settle: it is written eventually
      MapCount() = 15;
      if (unsorted) MapFlags() |= 1u;
      else MapFlags() &= ~1u;
      MapData() = reinterpret_cast<uint64_t>(g_rows);
      Publish();
      impactsnd::SetHookForTest(nullptr);
      Polls(5);
      CHECK(CountRight() == 15, "unsorted %d mode %d: not written after the table settled (%zu)", unsorted, mode, CountRight());
    }
}
static void TestReadBack() {
  Setup();
  SetTable(All(), kStock, false);
  Publish();
  Warm();
  g_hook_stage2 = 0;
  g_hook_armed2 = true;
  impactsnd::SetHookForTest(&Hk);
  Polls(2);
  impactsnd::SetHookForTest(nullptr);
  g_hook_armed2 = false;
  CHECK(g_hook_stage2 == 15, "stage 2 hook ran %d times", g_hook_stage2);
  const impactsnd::Stats s = impactsnd::GetStats();
  CHECK(s.rowsWritten == 14 && s.rowsLeft == 1, "lost store counted: written %u left %u", s.rowsWritten, s.rowsLeft);
  CHECK(HasLine("rewrote=14") && HasLine("left=1"), "lost store not in the line");
  Polls(4);  // the next sorted pass puts it right (a known id sitting in the wrong row)
  CHECK(AllRight(), "the lost store was not repaired: %zu right", CountRight());
}

static void TestLogCaps() {
  Setup();
  SetTable(All(), kStock, false);
  for (int i = 0; i < 20; ++i) {  // 20 times the table goes away and comes back
    Publish();
    Polls(2);
    Unpublish();
    Polls(2);
  }
  CHECK(CountLines("impactsnd no table now") == 12, "%d note lines (cap 12)", CountLines("impactsnd no table now"));
  CHECK(CountLines("log cap reached", 2) == 1 && HasLine("lines=notes"), "note cap not announced once");

  Setup();
  SetTable(All(), kStock, false);
  Publish();
  Warm();
  for (int i = 0; i < 210; ++i) {  // 210 passes that wrote
    SetTable(All(), kStock, false);
    Publish();
    Polls(2);
  }
  CHECK(CountLines("impactsnd ids") == 200, "%d write lines (cap 200)", CountLines("impactsnd ids"));
  CHECK(CountLines("log cap reached", 2) == 1 && HasLine("lines=ids"), "write cap not announced once");
  CHECK(impactsnd::GetStats().passes == 210, "passes %u (counting goes on past the cap)", impactsnd::GetStats().passes);
}

static void TestCountZeroTableWaits() {
  Setup();  // the map object exists but its count is 0: no count line, however long
  SetTable({}, kStock, true);
  MapCount() = 0;
  Publish();
  Polls(30);
  CHECK(!HasLine("impactsnd table"), "count line for an empty table");
  SetTable(All(), kStock, true);
  Polls(12);
  CHECK(CountLines("impactsnd table") == 1 && HasLine("rows=15"), "no count line once the table filled");
}

static void TestUnsortedDoneOnce() {
  // the loader's pass is the table's one unsorted write: the tick does not write an unsorted table again
  Setup();
  CallLoader();
  CHECK(CountRight() == 15, "loader pass");
  for (size_t i = 0; i < g_n; ++i) g_rows[i].id = kStock;  // something puts the stock ids back while it is still unsorted
  Polls(24);
  CHECK(CountRight() == 0, "the tick rewrote an unsorted table the loader pass had written (%zu right)", CountRight());
  SortRows();  // the first hit sorts it: the sorted pass repairs it
  Polls(4);
  CHECK(CountRight() == 15 && impactsnd::GetStats().passes == 2, "sorted pass after the sort: %zu right, %u passes", CountRight(),
        impactsnd::GetStats().passes);
  // the tick's own early pass is also once per table
  Setup();
  SetTable(All(), kStock, true);
  Publish();
  Warm();
  Polls(4);
  CHECK(CountRight() == 15, "tick early pass");
  for (size_t i = 0; i < g_n; ++i) g_rows[i].id = kStock;
  Polls(12);
  CHECK(CountRight() == 0 && impactsnd::GetStats().passes == 1, "tick early pass repeated (%zu right)", CountRight());
}

static void TestTableGoneOnce() {
  Setup();
  SetTable(All(), kStock, false);
  Publish();
  Warm();
  Unpublish();
  Polls(12);
  CHECK(CountLines("impactsnd no table now") == 1 && HasLine("where=tick"), "%d no-table notes for one disappearance",
        CountLines("impactsnd no table now"));
}

static void TestWrapperHuge() {
  Setup();
  g_load_mode = kLoadHuge;
  CallLoader();
  CHECK(CountRight() == 0 && impactsnd::GetStats().passes == 0 && HasLine("impactsnd no table now") && HasLine("count=1000") &&
            HasLine("where=after LoadSoundSettings"),
        "a count over the cap at load");
}

static void TestStillUnsorted() {  // an unsorted table with keys outside the 15 that is never sorted: one line after 600 s
  Setup();
  SetTable(All(), kStock, true);
  g_rows[15] = {0x1234567890abcdefull, kStock, kPad};
  g_n = 16;
  MapCount() = 16;
  Publish();
  Warm();
  Polls(4);
  CHECK(!HasLine("still unsorted"), "unsorted line early");
  g_now += 600.0;
  Polls(8);
  CHECK(CountLines("impactsnd table still unsorted") == 1, "still-unsorted line %d times", CountLines("impactsnd table still unsorted"));
  Polls(8);
  CHECK(CountLines("impactsnd table still unsorted") == 1, "still-unsorted line repeated");
  // a different table (another data pointer) starts its own 600 s wait
  std::memcpy(g_rows + 32, g_rows, sizeof(Row) * 16);
  MapData() = reinterpret_cast<uint64_t>(g_rows + 32);
  Polls(8);
  CHECK(CountLines("impactsnd table still unsorted") == 1, "the new table's wait is not its own");
  g_now += 600.0;
  Polls(8);
  CHECK(CountLines("impactsnd table still unsorted") == 2, "no still-unsorted line for the second table (%d)",
        CountLines("impactsnd table still unsorted"));
}

static void TestCountChangesBetweenPolls() {
  for (int unsorted = 0; unsorted < 2; ++unsorted) {
    Setup();
    SetTable(All(), kStock, unsorted != 0);
    Publish();
    Warm();
    Poll();  // first sight of 15 rows
    MapCount() = 14;
    Poll();  // same data, another count: not the same table state, no write
    CHECK(CountRight() == 0, "unsorted %d: written although the count changed between polls", unsorted);
    Poll();
    CHECK(CountRight() == 14, "unsorted %d: not written once the count held for two polls (%zu)", unsorted, CountRight());
  }
}

static size_t CountRight2() {
  size_t c = 0;
  for (size_t i = 0; i < 15; ++i) c += WantOf(g_rows2[i].key) != 0 && g_rows2[i].id == WantOf(g_rows2[i].key);
  return c;
}
static void SetRows2() {
  size_t n = 0;
  const impactsnd::Row* k = Known(&n);
  for (size_t i = 0; i < 15; ++i) g_rows2[i] = {k[i].key, kStock, kPad};
}

// ---- the table lock: a Shutdown that frees the table while a row pass is mid-flight (two threads) ----
static std::atomic<bool> g_in_flight{false}, g_b_started{false};
static void HkRace(int stage) {
  if (stage != 2) return;
  if (g_freed) ++g_uaf;  // a row store after the free
  if (!g_in_flight.exchange(true)) {  // the first store: let the other thread start Shutdown, and give it time to free
    for (int i = 0; i < 400 && !g_b_started; ++i) usleep(5000);
    usleep(150000);
  }
}
static void TestShutdownRace() {
  Setup();
  SetTable(All(), kStock, false);
  Publish();
  Warm();
  Poll();  // first sight of the table
  g_in_flight = g_b_started = false;
  std::thread b([] {
    for (int i = 0; i < 400 && !g_in_flight; ++i) usleep(5000);
    g_b_started = true;
    CallShutdown(0x1234);
  });
  impactsnd::SetHookForTest(&HkRace);
  Poll();  // the pass starts; Shutdown starts on the other thread while it is writing
  b.join();
  impactsnd::SetHookForTest(nullptr);
  CHECK(g_in_flight, "the race never started");
  CHECK(g_uaf == 0, "%d row stores after the table was freed", g_uaf.load());
  CHECK(g_freed && g_right_at_free == 15, "Shutdown ran before the pass finished: %zu of 15 right when it freed", g_right_at_free);
  CHECK(g_shut_calls == 1 && g_shut_self == 0x1234 && impactsnd::GetStats().shutdowns == 1, "Shutdown calls %d", g_shut_calls.load());
  CHECK(HasLine("impactsnd CR15NetBulletCS::Shutdown returned") && HasLine("table_global_now=0"), "no Shutdown line");
}

static void TestShutdownPassThrough() {
  Setup();
  const uint64_t rv = CallShutdown(0xabcdef);
  CHECK(g_shut_calls == 1 && g_shut_self == 0xabcdef, "the real Shutdown did not get its argument: %d %llx", g_shut_calls.load(),
        (unsigned long long)g_shut_self);
  CHECK(rv == (0xabcdefull ^ kShutXor), "return value %llx not passed through", (unsigned long long)rv);
}

static void TestBetweenValidationAndLock() {
  for (int unsorted = 0; unsorted < 2; ++unsorted)
    for (int mode = 1; mode <= 5; ++mode) {
      Setup();
      SetTable(All(), kStock, unsorted != 0);
      SetRows2();
      Publish();
      Warm();
      impactsnd::SetHookForTest(&Hk);
      Poll();
      g_hook4_mode = mode;
      Poll();  // validated, stable, then the hook changes the world before the lock is taken
      impactsnd::SetHookForTest(nullptr);
      CHECK(g_uaf == 0, "unsorted %d mode %d: stores into a freed table", unsorted, mode);
      CHECK(impactsnd::GetStats().skipped == 1 && impactsnd::GetStats().passes == 0 && HasLine("impactsnd row pass skipped"),
            "unsorted %d mode %d: skipped %u passes %u", unsorted, mode, impactsnd::GetStats().skipped, impactsnd::GetStats().passes);
      if (mode == 1) {
        CHECK(PoisonIntact() && g_freed, "unsorted %d mode 1: the freed table was written", unsorted);
        continue;
      }
      if (mode == 2) {
        CHECK(CountRight() == 0, "unsorted %d mode 2: the old table was written (%zu)", unsorted, CountRight());
        *reinterpret_cast<uint32_t*>(g_map2 + 0x38) = unsorted != 0 ? 1u : 0u;
        Polls(6);
        CHECK(CountRight2() == 15 && CountRight() == 0, "unsorted %d mode 2: new table %zu old %zu", unsorted, CountRight2(), CountRight());
        continue;
      }
      CHECK(CountRight() == 0, "unsorted %d mode %d: written although the table changed (%zu)", unsorted, mode, CountRight());
      // the world is put back: the pass was not counted as done, it runs again
      MapCount() = 15;
      MapData() = reinterpret_cast<uint64_t>(g_rows);
      Publish();
      Polls(6);
      CHECK(CountRight() == 15, "unsorted %d mode %d: never written after the skip (%zu)", unsorted, mode, CountRight());
    }
  // the loader's pass is skipped the same way, and is not marked done either
  Setup();
  g_load_mode = kLoad15;
  impactsnd::SetHookForTest(&Hk);
  g_hook4_mode = 3;
  CallLoader();
  impactsnd::SetHookForTest(nullptr);
  CHECK(CountRight() == 0 && impactsnd::GetStats().skipped == 1, "loader pass over a changed table: %zu right", CountRight());
  MapCount() = 15;
  Polls(24);
  CHECK(CountRight() == 15, "the tick did not take over after the loader's pass was skipped (%zu)", CountRight());
}

static void TestLockBusy() {  // a Shutdown holds the lock: the tick does not wait and writes nothing
  Setup();
  SetTable(All(), kStock, false);
  Publish();
  Warm();
  Poll();
  g_shut_mode = kShutHold;
  std::thread b([] { CallShutdown(0x55); });
  for (int i = 0; i < 400 && !g_in_shutdown; ++i) usleep(2000);
  CHECK(g_in_shutdown, "the holding Shutdown never started");
  const auto t0 = std::chrono::steady_clock::now();
  Poll();
  const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 150, "the tick waited %ld ms for the lock", ms);
  CHECK(CountRight() == 0 && impactsnd::GetStats().skipped == 1 && HasLine("table lock busy"), "written or not skipped while the lock was held");
  g_hold_release = true;
  b.join();
  g_shut_mode = kShutFree;
  Polls(4);
  CHECK(CountRight() == 15, "not written once the lock was free (%zu)", CountRight());
}

static void TestShutdownNested() {
  Setup();
  g_shut_mode = kShutNested;
  std::thread b([] { CallShutdown(0x66); });
  for (int i = 0; i < 400 && !g_nested_done; ++i) usleep(5000);
  if (!g_nested_done) {  // a self-deadlock: report and leave (the thread cannot be joined)
    std::printf("FAIL %s:%d: nested Shutdown deadlocked\n", __FILE__, __LINE__);
    std::printf("impactsnd_test: 1 failure(s)\n");
    std::_Exit(1);
  }
  b.join();
  CHECK(g_shut_calls == 2, "nested calls %d", g_shut_calls.load());
  // and the lock is free again afterwards
  SetTable(All(), kStock, false);
  Publish();
  Warm();
  Polls(3);
  CHECK(CountRight() == 15, "the lock stayed held after a nested Shutdown (%zu)", CountRight());
}

// The depth that tells a nested Shutdown from a first one is per thread, and a Shutdown that returned leaves none behind: after one
// Shutdown has completed on a thread, that thread's next one takes the lock and holds it, and a Shutdown on a second thread is a first
// one there. It waits for the holder; it does not run beside it.
static void TestShutdownDepthIsPerThread() {
  Setup();
  std::thread holder([] {
    CallShutdown(0x44);  // completes (kShutFree), call 1
    g_shut_mode = kShutHold;
    CallShutdown(0x55);  // call 2: holds the lock until released
  });
  for (int i = 0; i < 400 && !g_in_shutdown; ++i) usleep(2000);
  CHECK(g_in_shutdown, "the holding Shutdown never started");
  g_shut_mode = kShutFree;  // the second thread's Shutdown frees the table at once if it gets in
  std::atomic<bool> second_done{false};
  std::thread second([&] {
    CallShutdown(0x66);
    second_done = true;
  });
  for (int i = 0; i < 60 && g_shut_calls == 2; ++i) usleep(2000);  // an event with a deadline: the second Shutdown reaching the game
  CHECK(g_shut_calls == 2 && !second_done, "a Shutdown on a second thread ran beside the first, which held the lock (%d calls)", g_shut_calls.load());
  g_hold_release = true;
  holder.join();
  second.join();
  CHECK(g_shut_calls == 3 && second_done, "the second Shutdown never ran once the lock was free (%d calls)", g_shut_calls.load());
}

static long Gettid() { return static_cast<long>(syscall(SYS_gettid)); }
static void TestThreadLines() {
  Setup();
  SetTable(All(), kStock, false);
  Publish();
  Warm();  // the tick polls on this thread
  CallLoader();
  const long me = Gettid();
  long other = 0;
  std::thread b([&] {
    other = Gettid();
    CallShutdown(1);
    CallShutdown(2);  // the same thread again: no second line
  });
  b.join();
  char want[96];
  std::snprintf(want, sizeof want, "role=tick tid=%ld", me);
  CHECK(CountLines(want) == 1, "tick thread line: %d", CountLines(want));
  std::snprintf(want, sizeof want, "role=loader tid=%ld", me);
  CHECK(CountLines(want) == 1, "loader thread line: %d", CountLines(want));
  std::snprintf(want, sizeof want, "role=shutdown tid=%ld tick_tid=%ld loader_tid=%ld shutdown_tid=%ld", other, me, me, other);
  CHECK(CountLines(want) == 1 && CountLines("role=shutdown") == 1, "shutdown thread line: %d (%d)", CountLines(want), CountLines("role=shutdown"));
  CHECK(other != me && other != 0, "test threads");
  // a thread-line budget: 30 live threads, 24 lines and one WARNING
  Setup();
  g_shut_mode = kShutHold;  // the table stays, so the scope stays: a Shutdown that frees it ends the scope, and an arena teardown notes no thread
  g_hold_release = true;
  SetTable(All(), kStock, false);
  Publish();
  std::atomic<bool> go{false};
  std::vector<std::thread> ts;
  for (int i = 0; i < 30; ++i)
    ts.emplace_back([&] {
      while (!go) usleep(500);
      CallShutdown(3);
    });
  go = true;
  for (auto& t : ts) t.join();
  CHECK(CountLines("impactsnd thread") == 24, "%d thread lines (cap 24)", CountLines("impactsnd thread"));
  CHECK(CountLines("lines=threads", 2) == 1, "thread cap not announced once: %d", CountLines("lines=threads", 2));
}

int main() {
  StartCounting();
  // a hang (a lock that is never released, a joined thread that never returns) is a failure, not a stuck test run
  std::thread([] {
    for (int i = 0; i < 600; ++i) usleep(100000);
    std::printf("FAIL: impactsnd_test watchdog: 60 s without finishing (hang)\n");
    std::_Exit(2);
  }).detach();
  sentinel::SetLogSink(&Capture);

  TestNoThreadKey();
  TestConstants();
  TestInstall();
  TestOffByTheSwitch();
  TestWriteSwitchOff();
  TestRefusals();
  TestSlotRefused();
  TestShutdownSlotRefused();
  TestArenaLoadIsTheGamesOwn();
  TestCombatLevelsAreRepaired();
  TestScopeFailsClosed();
  TestScopeFollowsTheTable();
  TestAnArenaLoadEndsTheScopeOfAnEarlierCombatLoad();
  TestScopeSeesALevelStillLoading();
  TestArenaPaysNothingByCount();
  TestTickOnlyAsksOnceASecond();
  TestScopeLineBudget();
  TestRebuiltLoaderInACombatLevel();
  TestRebuiltLoaderNotInArena();
  TestRebuiltLoaderNeedsItsConditions();
  TestWrapperUnsorted15();
  TestWrapper5Rows();
  TestWrapperSortedAtLoad();
  TestWrapperForeignKey();
  TestWrapperNoTable();
  TestTickCountLine();
  TestPollGate();
  TestTickUnsorted();
  TestTickSorted();
  TestSortRaceFixups();
  TestRowsOutsideTable();
  TestTableShapes();
  TestNowriteTick();
  TestChangedUnderUs();
  TestReadBack();
  TestLogCaps();
  TestCountZeroTableWaits();
  TestUnsortedDoneOnce();
  TestTableGoneOnce();
  TestWrapperHuge();
  TestStillUnsorted();
  TestCountChangesBetweenPolls();
  TestShutdownRace();
  TestShutdownPassThrough();
  TestBetweenValidationAndLock();
  TestLockBusy();
  TestAnArenaSpaceBesideACombatTableGetsTheStockIds();
  TestAnArenaSpaceBesideARebuiltTableGetsTheStockIds();
  TestTheStockIdsWaitForTheTableLock();
  TestTheStockIdsWaitForAStableTable();
  TestAReadOnlyScopeEndsWithoutAWrite();
  TestTheRecheckIsOneScanASecond();
  TestShutdownNested();
  TestShutdownDepthIsPerThread();
  TestThreadLines();

  if (g_fail == 0) std::printf("impactsnd_test: all checks passed\n");
  else std::printf("impactsnd_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
