// Host test of impactsnd (src/quest/plugins/q15t/src/impactsnd.cpp) against a fake game image: an anonymous mapping that holds the pinned ELF vaddrs
// (hit-code words, the kSoundSettings GLOB_DAT word, the .bss words D and kSoundSettings, the LoadSoundSettings JUMP_SLOT
// on a read-only page, a trampoline at the loader's address) and a fake impact table on the host heap laid out as the game's
// (map+0x00 data, +0x30 count, +0x38 flags; 16-byte rows {u64 key, u32 id, pad}). Real SafeRead, real mprotect, real
// /proc/self/maps, a fake clock.
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "nevr/plugin_interface.h"
#include "impactsnd.h"
#include "plugin_log.h"

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

struct Line {
  uint32_t level;
  std::string text;
};
static std::vector<Line> g_lines;
static std::mutex g_lines_mu;  // the module logs from several threads
static void FakeLog(const char* id, uint32_t level, const char* msg, const NevrLogField* f, uint32_t n) {
  std::string t = std::string("plugin=") + id + " " + msg;
  for (uint32_t i = 0; i < n; ++i) t += std::string(" ") + f[i].key + "=" + f[i].value;
  std::lock_guard<std::mutex> lock(g_lines_mu);
  g_lines.push_back({level, t});
}
static int CountLines(const char* needle, int level = -1) {
  int n = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++n;
  return n;
}
static bool HasLine(const char* needle, int level = -1) { return CountLines(needle, level) > 0; }

// ---- the pinned addresses, restated here (not taken from the module) ----
static const uintptr_t kDefaultVa = 0x37af8d0, kSettingsVa = 0x37af6a0, kSettingsGotVa = 0x370fa70, kSlotVa = 0x36fa3c0, kFnVa = 0x2121d04;
static const uintptr_t kShutSlotVa = 0x36933e8, kShutFnVa = 0x21226f0;  // CR15NetBulletCS::Shutdown: vtable word, function
static const uintptr_t kCodeVa[4] = {0x21208b4, 0x21208b8, 0x21208bc, 0x21208c8};
static const uint32_t kCodeWord[4] = {0xf9401ac1, 0xb4000541, 0xf000b468, 0xf944691b};
static const uint64_t kDefaultSym = 0x2fd38a995f615b9eull;
static const size_t kImgSize = 0x37b1000;
static const uint32_t kStock = 1;

// ---- the fake game image ----
static uintptr_t g_base = 0;
static void W64(uintptr_t va, uint64_t v) { *reinterpret_cast<uint64_t*>(g_base + va) = v; }
static uint64_t R64(uintptr_t va) { return *reinterpret_cast<uint64_t*>(g_base + va); }
static uint32_t R32(uintptr_t va) { return *reinterpret_cast<uint32_t*>(g_base + va); }

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

static const q15t::ImpactSndRow* Known(size_t* n) { return q15t::ImpactSndRows(n); }
static uint64_t& MapData() { return *reinterpret_cast<uint64_t*>(g_map); }
static uint64_t& MapCount() { return *reinterpret_cast<uint64_t*>(g_map + 0x30); }
static uint32_t& MapFlags() { return *reinterpret_cast<uint32_t*>(g_map + 0x38); }
static void Publish() { W64(kSettingsVa, reinterpret_cast<uintptr_t>(g_map)); }
static void Unpublish() { W64(kSettingsVa, 0); }
static void SetTable(const std::vector<size_t>& idx, uint32_t id, bool unsorted) {  // idx into kIds, in this order
  size_t n = 0;
  const q15t::ImpactSndRow* k = Known(&n);
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
  const q15t::ImpactSndRow* k = Known(&n);
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
    case kLoad15: SetTable(All(), kStock, true); Publish(); break;
    case kLoad5: SetTable(Players(), kStock, true); Publish(); break;
    case kLoad15Sorted: SetTable(All(), kStock, true); SortRows(); Publish(); break;
    case kLoadForeign:
      SetTable(All(), kStock, true);
      g_rows[g_n++] = {0xdeadbeefcafef00dull, kStock, kPad};
      MapCount() = g_n;
      Publish();
      break;
    case kLoadNothing: Unpublish(); break;
    case kLoadHuge: SetTable(All(), kStock, true); MapCount() = 1000; Publish(); break;
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
  q15t::ImpactSndTick(0);
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
  if (g_base != 0) munmap(reinterpret_cast<void*>(g_base), kImgSize);
  void* p = mmap(nullptr, kImgSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) std::abort();
  g_base = reinterpret_cast<uintptr_t>(p);
  for (int i = 0; i < 4; ++i) *reinterpret_cast<uint32_t*>(g_base + kCodeVa[i]) = kCodeWord[i];
  W64(kSettingsGotVa, g_base + kSettingsVa);
  W64(kDefaultVa, 0);
  W64(kSettingsVa, 0);
  W64(kSlotVa, g_base + kFnVa);
  W64(kShutSlotVa, g_base + kShutFnVa);
  // a trampoline at the loader's address: movabs rax, FakeLoader; jmp rax (the page is executable, never written afterwards)
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
  mprotect(reinterpret_cast<void*>((g_base + kSlotVa) & ~0xfffull), 0x1000, PROT_READ);      // RELRO
  mprotect(reinterpret_cast<void*>((g_base + kShutSlotVa) & ~0xfffull), 0x1000, PROT_READ);  // RELRO
}

static std::vector<uint32_t> g_code_before;
static void Fresh() {  // a new image, new table, a reset module; not installed
  g_lines.clear();
  q15t::ResetImpactSndForTest();
  q15t::SetImpactSndClockForTest(&Clock);
  BuildImage();
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
static q15t::InstallResult Install() { return q15t::InstallImpactSnd(g_base); }
static void Setup() {
  Fresh();
  const q15t::InstallResult r = Install();
  CHECK(r.status == q15t::SwapStatus::kOk, "setup install: %s %s", q15t::StatusName(r.status), r.reason);
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
  const q15t::ImpactSndRow* rows = Known(&n);
  CHECK(n == 15, "%zu rows", n);
  const char* guns[5] = {"assault", "scout", "rocket", "rifle", "smg"};
  const char* subs[3] = {"player", "barrier", "default"};
  const char* ev[3] = {"play_bullet_impact_player", "play_bullet_impact_shield", "play_bullet_impact_geo"};
  for (int g = 0; g < 5; ++g)
    for (int s = 0; s < 3; ++s) {
      std::string name = ev[s];
      if (g == 4) name += "_quark";  // smg uses the _quark events
      const q15t::ImpactSndRow& r = rows[g * 3 + s];
      const uint64_t key = EchoVR::CSymbol64Hash(subs[s]) ^ EchoVR::CSymbol64Hash(guns[g]);
      CHECK(r.key == key, "row %d (%s %s): key %016llx, CSymbol64 says %016llx", g * 3 + s, guns[g], subs[s], (unsigned long long)r.key,
            (unsigned long long)key);
      CHECK(r.id == Fnv1(name.c_str()), "row %d (%s): id %08x, FNV-1 32 of %s is %08x", g * 3 + s, guns[g], r.id, name.c_str(),
            Fnv1(name.c_str()));
    }
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 1; j < n; ++j) CHECK(rows[i].key != rows[j].key, "duplicate key rows %zu %zu", i, j);
  CHECK(q15t::kImpactDefaultSymbol == EchoVR::CSymbol64Hash("default") && q15t::kImpactDefaultSymbol == kDefaultSym,
        "default symbol %016llx", (unsigned long long)EchoVR::CSymbol64Hash("default"));
  CHECK(rows[0].id != kStock && rows[5].id != kStock, "a known id equals the stock id");
}

static void TestInstallWritesD() {
  Fresh();
  g_code_before.clear();
  for (int i = 0; i < 4; ++i) g_code_before.push_back(R32(kCodeVa[i]));
  const q15t::InstallResult r = Install();
  CHECK(r.status == q15t::SwapStatus::kOk, "install: %s %s", q15t::StatusName(r.status), r.reason);
  CHECK(R64(kDefaultVa) == kDefaultSym, "D reads %016llx", (unsigned long long)R64(kDefaultVa));
  CHECK(R64(kSlotVa) != g_base + kFnVa && R64(kSlotVa) != 0, "slot not swapped");
  q15t::Mapping m;
  CHECK(q15t::FindMapping(g_base + kSlotVa, &m) && std::strcmp(m.perms, "r--p") == 0, "slot page perms %s (RELRO not restored)", m.perms);
  for (int i = 0; i < 4; ++i) CHECK(R32(kCodeVa[i]) == g_code_before[i], "hit-code word %d changed", i);
  CHECK(R64(kSettingsGotVa) == g_base + kSettingsVa, "GOT word changed");
  CHECK(HasLine("impactsnd on") && HasLine("action=wrote") && HasLine("was=0") && HasLine("now=2fd38a995f615b9e"), "no D line");
  CHECK(HasLine("impactsnd load wrapper") && HasLine("status=ok", 1), "no wrapper line");
  CHECK(q15t::GetImpactSndStats().on && q15t::GetImpactSndStats().writers_ok, "not on / writers off");
  CHECK(R64(kShutSlotVa) != g_base + kShutFnVa && R64(kShutSlotVa) != 0, "Shutdown slot not swapped");
  CHECK(q15t::FindMapping(g_base + kShutSlotVa, &m) && std::strcmp(m.perms, "r--p") == 0, "Shutdown slot page perms %s (RELRO not restored)", m.perms);
  CHECK(HasLine("impactsnd shutdown wrapper") && HasLine("row_writes=enabled", 1), "no Shutdown wrapper line");
  // the module's own default bytes are not touched on a second look: D already set -> left alone
  Fresh();
  W64(kDefaultVa, 0x1111222233334444ull);
  Install();
  CHECK(R64(kDefaultVa) == 0x1111222233334444ull, "a non-zero D was overwritten");
  CHECK(HasLine("action=left alone (not 0)") && HasLine("was=1111222233334444"), "no left-alone line");
}

static void TestNowriteBuild() {
  Fresh();
  q15t::SetImpactSndNoWriteForTest(true);
  Install();
  CHECK(R64(kDefaultVa) == 0, "nowrite: D written");
  CHECK(HasLine("action=left alone (nowrite build)") && HasLine("nowrite_build=1"), "nowrite: no D line");
  CHECK(R64(kSlotVa) != g_base + kFnVa, "nowrite: the wrapper (an observer) must still be installed");
  // a table is found and logged, never written, and the line comes once
  SetTable(All(), kStock, false);
  Publish();
  Polls(20);
  CHECK(AllRight() == false && CountRight() == 0, "nowrite: ids written (%zu right)", CountRight());
  CHECK(CountLines("impactsnd ids (nowrite build") == 1, "nowrite: %d lines", CountLines("impactsnd ids (nowrite build"));
  CHECK(q15t::GetImpactSndStats().rows_written == 0, "nowrite: rows_written %u", q15t::GetImpactSndStats().rows_written);
  // and through the wrapper
  Fresh();
  q15t::SetImpactSndNoWriteForTest(true);
  Install();
  CallLoader();
  CHECK(CountRight() == 0 && g_loader_calls == 1 && g_n == 15, "nowrite wrapper: ids written / loader not called");
  CHECK(CountLines("impactsnd ids (nowrite build") == 1 && HasLine("when=load/unsorted"), "nowrite wrapper: no read line");
}

static void TestRefusals() {
  for (int w = 0; w < 4; ++w) {  // each of the four hit-code words, wrong in turn
    Fresh();
    *reinterpret_cast<uint32_t*>(g_base + kCodeVa[w]) = kCodeWord[w] ^ 0x1;
    const q15t::InstallResult r = Install();
    CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "hit_code_word_mismatch") == 0, "word %d: %s %s", w, q15t::StatusName(r.status),
          r.reason);
    CHECK(R64(kDefaultVa) == 0, "word %d: D written", w);
    CHECK(R64(kSlotVa) == g_base + kFnVa, "word %d: slot swapped", w);
    CHECK(HasLine("impactsnd OFF: nothing written", 3) && HasLine("written=0"), "word %d: not loud", w);
    SetTable(All(), kStock, false);
    Publish();
    Polls(8);
    CHECK(q15t::GetImpactSndStats().polls == 0 && !q15t::GetImpactSndStats().on && CountRight() == 0, "word %d: the tick ran", w);
  }
  // the code page unreadable: refused, no fault
  Fresh();
  mprotect(reinterpret_cast<void*>((g_base + kCodeVa[0]) & ~0xfffull), 0x1000, PROT_NONE);
  CHECK(Install().status != q15t::SwapStatus::kOk && R64(kDefaultVa) == 0, "unreadable code page not refused");
  // the kSoundSettings GLOB_DAT word does not point at the object this module reads
  Fresh();
  W64(kSettingsGotVa, g_base + kSettingsVa + 8);
  const q15t::InstallResult r = Install();
  CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "ksoundsettings_got_mismatch") == 0 && R64(kDefaultVa) == 0 &&
            R64(kSlotVa) == g_base + kFnVa,
        "GOT mismatch: %s %s", q15t::StatusName(r.status), r.reason);
  // D unreadable
  Fresh();
  mprotect(reinterpret_cast<void*>((g_base + kDefaultVa) & ~0xfffull), 0x1000, PROT_NONE);
  CHECK(std::strcmp(Install().reason, "default_symbol_unreadable") == 0, "unreadable D not refused");
  // D reads 0 but its page is read-only: refused, never faults
  Fresh();
  mprotect(reinterpret_cast<void*>((g_base + kDefaultVa) & ~0xfffull), 0x1000, PROT_READ);
  const q15t::InstallResult r2 = Install();
  CHECK(std::strcmp(r2.reason, "default_symbol_page_not_rw") == 0 && R64(kDefaultVa) == 0 && R64(kSlotVa) == g_base + kFnVa,
        "read-only D page: %s", r2.reason);
  // D reads 0 but its page is executable-writable: refused too
  Fresh();
  mprotect(reinterpret_cast<void*>((g_base + kDefaultVa) & ~0xfffull), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
  const q15t::InstallResult r3 = Install();
  CHECK(std::strcmp(r3.reason, "default_symbol_page_not_rw") == 0 && R64(kDefaultVa) == 0, "rwx D page written: %s", r3.reason);
}

static void TestSlotRefused() {
  Fresh();
  mprotect(reinterpret_cast<void*>((g_base + kSlotVa) & ~0xfffull), 0x1000, PROT_READ | PROT_WRITE);
  W64(kSlotVa, 0x4242420);  // not the loader
  mprotect(reinterpret_cast<void*>((g_base + kSlotVa) & ~0xfffull), 0x1000, PROT_READ);
  const q15t::InstallResult r = Install();
  CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal, "foreign slot: %s", q15t::StatusName(r.status));
  CHECK(R64(kSlotVa) == 0x4242420, "foreign slot word changed");
  CHECK(R64(kDefaultVa) == kDefaultSym, "tick-only mode must still write D");
  CHECK(HasLine("impactsnd load wrapper") && HasLine("mode=tick only", 3), "no ERROR for the refused wrapper");
  // tick-only mode works: sorted table, two stable polls
  SetTable(All(), kStock, false);
  Publish();
  Warm();
  Polls(3);
  CHECK(AllRight(), "tick-only: ids not written (%zu right)", CountRight());
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
  const q15t::ImpactSndRow* k = Known(&n);
  bool order = after.size() == 15;
  for (size_t i = 0; order && i < 15; ++i) order = after[i].key == k[i].key && after[i].pad == kPad;
  CHECK(order, "keys, order or pads changed by the write");
  CHECK(MapFlags() == 1u && MapCount() == 15, "flags/count touched");
  const q15t::ImpactSndStats s = q15t::GetImpactSndStats();
  CHECK(s.loads == 1 && s.passes == 1 && s.rows_written == 15 && s.rows_left == 0, "stats l%u p%u w%u r%u", s.loads, s.passes, s.rows_written,
        s.rows_left);
  CHECK(HasLine("impactsnd LoadSoundSettings returned") && HasLine("word_patch=in_effect") && HasLine("rows=15"), "no loader line");
  CHECK(HasLine("impactsnd ids") && HasLine("when=load/unsorted") && HasLine("rewrote=15") && HasLine("found_ids=00000001,00000001"), "no ids line");
  // the tick afterwards does not repeat the unsorted pass; after the first hit's sort nothing is left to write
  Polls(10);
  CHECK(q15t::GetImpactSndStats().passes == 1, "the tick repeated the unsorted pass: %u", q15t::GetImpactSndStats().passes);
  SortRows();
  Polls(10);
  CHECK(AllRight() && q15t::GetImpactSndStats().passes == 1 && CountLines("impactsnd ids") == 1, "sorted table: extra pass (%u)",
        q15t::GetImpactSndStats().passes);
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
  CHECK(CountRight() == 0 && q15t::GetImpactSndStats().passes == 0, "unsorted table with a key outside the 15 was written at load");
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
  CHECK(rv == kLoaderReturn && q15t::GetImpactSndStats().loads == 1 && q15t::GetImpactSndStats().passes == 0, "no table at load");
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
  q15t::ImpactSndTick(0);
  q15t::ImpactSndTick(0);
  g_now += 0.2;
  q15t::ImpactSndTick(0);
  CHECK(q15t::GetImpactSndStats().polls == 1, "polls %u within 0.25 s", q15t::GetImpactSndStats().polls);
  g_now += 0.05;
  q15t::ImpactSndTick(0);
  CHECK(q15t::GetImpactSndStats().polls == 2, "polls %u after 0.25 s", q15t::GetImpactSndStats().polls);
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
  const uint32_t passes = q15t::GetImpactSndStats().passes;
  Polls(8);
  CHECK(q15t::GetImpactSndStats().passes == passes, "unsorted table passed more than once");
  // the engine puts the stock ids back into the same table (a reload at the same address) after it was gone: written again
  Unpublish();
  Polls(2);
  SetTable(All(), kStock, true);
  Publish();
  Polls(3);
  CHECK(CountRight() == 15 && q15t::GetImpactSndStats().passes == passes + 1, "new table at the same address not rewritten (%zu right)", CountRight());
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
  CHECK(q15t::GetImpactSndStats().passes == 1 && CountLines("impactsnd ids") == 1, "a pass with nothing to write logged");
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
  CHECK(q15t::GetImpactSndStats().rows_left == 1, "left %u", q15t::GetImpactSndStats().rows_left);
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
  q15t::SetImpactSndNoWriteForTest(true);
  Install();
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
      q15t::SetImpactSndHookForTest(&Hk);
      g_hook_stage1 = 0;
      Poll();
      g_hook_mode = mode;
      Poll();  // stable: the hook changes the table before the re-read
      CHECK(g_hook_stage1 == 1, "unsorted %d mode %d: hook ran %d times", unsorted, mode, g_hook_stage1);
      CHECK(CountRight() == 0, "unsorted %d mode %d: written although the table changed under the poll (%zu)", unsorted, mode, CountRight());
      CHECK(q15t::GetImpactSndStats().skipped == 0 && q15t::GetImpactSndStats().passes == 0,
            "unsorted %d mode %d: the poll's own re-read must stop it before a pass starts (skipped %u)", unsorted, mode,
            q15t::GetImpactSndStats().skipped);
      // restore and let the polls settle: it is written eventually
      MapCount() = 15;
      if (unsorted) MapFlags() |= 1u;
      else MapFlags() &= ~1u;
      MapData() = reinterpret_cast<uint64_t>(g_rows);
      Publish();
      q15t::SetImpactSndHookForTest(nullptr);
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
  q15t::SetImpactSndHookForTest(&Hk);
  Polls(2);
  q15t::SetImpactSndHookForTest(nullptr);
  g_hook_armed2 = false;
  CHECK(g_hook_stage2 == 15, "stage 2 hook ran %d times", g_hook_stage2);
  const q15t::ImpactSndStats s = q15t::GetImpactSndStats();
  CHECK(s.rows_written == 14 && s.rows_left == 1, "lost store counted: written %u left %u", s.rows_written, s.rows_left);
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
  CHECK(q15t::GetImpactSndStats().passes == 210, "passes %u (counting goes on past the cap)", q15t::GetImpactSndStats().passes);
}

static void TestDReadBack() {
  Fresh();
  g_hook_d = 0xbad;
  q15t::SetImpactSndHookForTest(&Hk);
  Install();
  q15t::SetImpactSndHookForTest(nullptr);
  g_hook_d = 0;
  CHECK(HasLine("action=wrote, READ-BACK DIFFERS") && HasLine("impactsnd on", 3), "a lost D store is not an ERROR");
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
  CHECK(CountRight() == 15 && q15t::GetImpactSndStats().passes == 2, "sorted pass after the sort: %zu right, %u passes", CountRight(),
        q15t::GetImpactSndStats().passes);
  // the tick's own early pass is also once per table
  Setup();
  SetTable(All(), kStock, true);
  Publish();
  Warm();
  Polls(4);
  CHECK(CountRight() == 15, "tick early pass");
  for (size_t i = 0; i < g_n; ++i) g_rows[i].id = kStock;
  Polls(12);
  CHECK(CountRight() == 0 && q15t::GetImpactSndStats().passes == 1, "tick early pass repeated (%zu right)", CountRight());
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
  CHECK(CountRight() == 0 && q15t::GetImpactSndStats().passes == 0 && HasLine("impactsnd no table now") && HasLine("count=1000") &&
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
  const q15t::ImpactSndRow* k = Known(&n);
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
  q15t::SetImpactSndHookForTest(&HkRace);
  Poll();  // the pass starts; Shutdown starts on the other thread while it is writing
  b.join();
  q15t::SetImpactSndHookForTest(nullptr);
  CHECK(g_in_flight, "the race never started");
  CHECK(g_uaf == 0, "%d row stores after the table was freed", g_uaf.load());
  CHECK(g_freed && g_right_at_free == 15, "Shutdown ran before the pass finished: %zu of 15 right when it freed", g_right_at_free);
  CHECK(g_shut_calls == 1 && g_shut_self == 0x1234 && q15t::GetImpactSndStats().shutdowns == 1, "Shutdown calls %d", g_shut_calls.load());
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
      q15t::SetImpactSndHookForTest(&Hk);
      Poll();
      g_hook4_mode = mode;
      Poll();  // validated, stable, then the hook changes the world before the lock is taken
      q15t::SetImpactSndHookForTest(nullptr);
      CHECK(g_uaf == 0, "unsorted %d mode %d: stores into a freed table", unsorted, mode);
      CHECK(q15t::GetImpactSndStats().skipped == 1 && q15t::GetImpactSndStats().passes == 0 && HasLine("impactsnd row pass skipped"),
            "unsorted %d mode %d: skipped %u passes %u", unsorted, mode, q15t::GetImpactSndStats().skipped, q15t::GetImpactSndStats().passes);
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
  q15t::SetImpactSndHookForTest(&Hk);
  g_hook4_mode = 3;
  CallLoader();
  q15t::SetImpactSndHookForTest(nullptr);
  CHECK(CountRight() == 0 && q15t::GetImpactSndStats().skipped == 1, "loader pass over a changed table: %zu right", CountRight());
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
  CHECK(CountRight() == 0 && q15t::GetImpactSndStats().skipped == 1 && HasLine("table lock busy"), "written or not skipped while the lock was held");
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

static void TestShutdownSlotRefused() {
  Fresh();
  mprotect(reinterpret_cast<void*>((g_base + kShutSlotVa) & ~0xfffull), 0x1000, PROT_READ | PROT_WRITE);
  W64(kShutSlotVa, 0x4242420);  // not Shutdown
  mprotect(reinterpret_cast<void*>((g_base + kShutSlotVa) & ~0xfffull), 0x1000, PROT_READ);
  const q15t::InstallResult r = Install();
  CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal, "foreign Shutdown slot: %s", q15t::StatusName(r.status));
  CHECK(R64(kShutSlotVa) == 0x4242420, "foreign slot word changed");
  CHECK(HasLine("impactsnd shutdown wrapper") && HasLine("row_writes=DISABLED", 3), "no ERROR for the refused Shutdown wrapper");
  CHECK(!q15t::GetImpactSndStats().writers_ok && R64(kDefaultVa) == kDefaultSym, "writers_ok / D");
  // the module reads and logs, never writes: through the loader wrapper and through the tick
  CallLoader();
  CHECK(CountRight() == 0 && HasLine("impactsnd ids (row writes disabled"), "loader pass wrote with writers disabled (%zu)", CountRight());
  SetTable(All(), kStock, false);
  Publish();
  Polls(24);
  CHECK(CountRight() == 0 && q15t::GetImpactSndStats().rows_written == 0, "tick wrote with writers disabled (%zu)", CountRight());
  CHECK(CountLines("impactsnd ids (row writes disabled") == 1, "read-only line %d times", CountLines("impactsnd ids (row writes disabled"));
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
  // a hang (a lock that is never released, a joined thread that never returns) is a failure, not a stuck test run
  std::thread([] {
    for (int i = 0; i < 600; ++i) usleep(100000);
    std::printf("FAIL: impactsnd_test watchdog: 60 s without finishing (hang)\n");
    std::_Exit(2);
  }).detach();
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);

  TestConstants();
  TestInstallWritesD();
  TestNowriteBuild();
  TestRefusals();
  TestSlotRefused();
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
  TestDReadBack();
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
  TestShutdownNested();
  TestShutdownSlotRefused();
  TestThreadLines();

  if (g_fail == 0) std::printf("impactsnd_test: all checks passed\n");
  else std::printf("impactsnd_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
