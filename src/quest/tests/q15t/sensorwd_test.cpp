// Host test of sensorwd (src/quest/plugins/q15t/src/sensorwd.cpp) against a fake game image: an anonymous mapping that holds the pinned ELF vaddrs (the
// code words the module pins, the two CModelCS vtable words and the SendComponentEventGlobal JUMP_SLOT on read-only pages) and, for the
// behaviour tests, x86 trampolines at "function" addresses that jump to host functions, plus a fake CModelCS table laid out as the
// game's (cs+0xd0 -> u16 index table at 4 * (h & 0xffff), cs+0x100 -> 16-byte entries, +4 visible, +8 model, model+0xcc8 record id).
// The wrappers are called through the swapped words, as the game does. Real SafeRead, real mprotect, real /proc/self/maps, a fake clock.
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nevr/symbol_hash.h"
#include "nevr/plugin_interface.h"
#include "module_switch.h"
#include "pin_check.h"
#include "plugin_log.h"
#include "sensorwd.h"

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

using u64 = uint64_t;

// ---- log capture ----
struct Line {
  uint32_t level;
  std::string text;
};
static std::vector<Line> g_lines;
static std::mutex g_lines_mu;
static std::atomic<int> g_log_under_lock{0};
static bool g_probe_log = false;  // single-threaded sections: a Log call with the state lock held is a defect
static void FakeLog(const char* id, uint32_t level, const char* msg, const NevrLogField* f, uint32_t n) {
  if (g_probe_log && q15t::SensorWdLockHeldForTest()) ++g_log_under_lock;
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
static int ErrorLines() {
  int n = 0;
  for (const auto& l : g_lines) n += l.level == NEVR_LOG_ERROR;
  return n;
}

// ---- the pinned addresses, restated here (not taken from the module) ----
static const uintptr_t kEnSlot = 0x36495c8, kDisSlot = 0x36495d0, kEvSlot = 0x36fcae8;
static const uintptr_t kEnFn = 0x15b127c, kDisFn = 0x15b12a8, kEvFn = 0x180735c;
static const uintptr_t kTEnFn = 0x300000, kTDisFn = 0x300100, kTEvFn = 0x300200;  // trampolines (behaviour tests)
static const uintptr_t kImgSize = 0x3700000;
static const u64 kSphere = 0xac2d3f9f080ba45full, kTriggered = 0xfd8cf56544b904a3ull, kFinished = 0x7cc621b8bf3248c5ull;
static const u64 kOtherModel = 0x1122334455667788ull, kOtherSym = 0x99aabbccddeeff00ull;

static uintptr_t g_base = 0;
static void W64(uintptr_t va, u64 v) { *reinterpret_cast<u64*>(g_base + va) = v; }
static u64 R64(uintptr_t va) { return *reinterpret_cast<u64*>(g_base + va); }
static uint32_t R32(uintptr_t va) { return *reinterpret_cast<uint32_t*>(g_base + va); }

// ---- the fake CModelCS table ----
struct Entry {
  uint32_t pad;
  uint32_t vis;  // +4
  uintptr_t model;  // +8
};
struct Cs {
  uintptr_t w[0x120 / 8];  // +0xd0 index table, +0x100 entries; the words around them point at a zero table
  uint16_t idx[256];
  Entry ent[64];
};
struct Model {
  uint8_t b[0xd00];  // +0xcc8: the record id
};
static const int kNumCs = 3, kNumH = 40;
alignas(16) static Cs g_cs[kNumCs];
alignas(16) static Model g_models[kNumCs * 64];
alignas(16) static uint8_t g_zero[0x4000];
static uintptr_t CsAddr(int c) { return reinterpret_cast<uintptr_t>(&g_cs[c]); }
// handle i of component c: a generation and junk above bit 16 (the index is the low 16 bits); entry number i + 5 (not i)
static u64 Handle(int i, u64 gen = 3) { return (u64)i | (gen << 16) | (0xabcdull << 32); }
static void SetKind(int c, int i, u64 name) {
  Model& m = g_models[c * 64 + i];
  std::memset(&m, 0, sizeof m);
  std::memcpy(m.b + 0xcc8, &name, 8);
  const int e = i + 5;
  g_cs[c].idx[2 * i] = (uint16_t)e;
  g_cs[c].ent[e].model = reinterpret_cast<uintptr_t>(&m);
}
static void SetNullModel(int c, int i) { g_cs[c].ent[i + 5].model = 0; }
static void World() {
  std::memset(g_cs, 0, sizeof g_cs);
  std::memset(g_zero, 0, sizeof g_zero);
  for (int c = 0; c < kNumCs; ++c) {
    for (int k = 0; k < 0x120 / 8; ++k) g_cs[c].w[k] = reinterpret_cast<uintptr_t>(g_zero);  // decoys for a wrong offset
    g_cs[c].w[0xd0 / 8] = reinterpret_cast<uintptr_t>(g_cs[c].idx);
    g_cs[c].w[0x100 / 8] = reinterpret_cast<uintptr_t>(g_cs[c].ent);
    for (int i = 0; i < kNumH; ++i) SetKind(c, i, kSphere);
    for (int e = 0; e < 64; ++e) {  // entries that no handle reaches hold another model
      if (e >= 5 && e < 5 + kNumH) continue;
      g_cs[c].ent[e].model = reinterpret_cast<uintptr_t>(&g_models[c * 64 + 63]);
    }
    std::memset(&g_models[c * 64 + 63], 0, sizeof(Model));
    uint64_t other = kOtherModel;
    std::memcpy(g_models[c * 64 + 63].b + 0xcc8, &other, 8);
  }
}
static int Vis(int c, int i) { return (int)__atomic_load_n(&g_cs[c].ent[i + 5].vis, __ATOMIC_RELAXED); }

// ---- the fake engine functions the trampolines jump to ----
enum Which { kEn = 1, kDis = 2, kEv = 3 };
struct Call {
  int which;
  uintptr_t cs;
  u64 h, a2, a3, a4, a5, a6, a7;
};
static std::vector<Call> g_calls;
static std::mutex g_calls_mu;
static std::atomic<int> g_marked_en{0}, g_marked_dis{0}, g_lock_in_real{0};
static bool g_probe_real = false;  // single-threaded sections: a call into the game with the state lock held is a defect
static const u64 kMark = 0x77;
static const uintptr_t kRetEn = 0xe0e0e0e0, kRetDis = 0xd0d0d0d0, kRetEv = 0xe7e7e7e7;
static void Record(int w, uintptr_t cs, u64 h, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  if (g_probe_real && q15t::SensorWdLockHeldForTest()) ++g_lock_in_real;
  if (a2 == kMark) (w == kEn ? g_marked_en : g_marked_dis).fetch_add(1);
  std::lock_guard<std::mutex> lock(g_calls_mu);
  g_calls.push_back({w, cs, h, a2, a3, a4, a5, a6, a7});
}
static Entry* EntryOf(uintptr_t cs, u64 h) {
  for (int c = 0; c < kNumCs; ++c)
    if (CsAddr(c) == cs) return &g_cs[c].ent[g_cs[c].idx[2 * (h & 0xffff)]];
  return nullptr;
}
extern "C" __attribute__((noinline)) uintptr_t FakeEnable(uintptr_t cs, u64 h, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  Record(kEn, cs, h, a2, a3, a4, a5, a6, a7);
  if (Entry* e = EntryOf(cs, h)) __atomic_store_n(&e->vis, 1u, __ATOMIC_RELAXED);
  return kRetEn;
}
extern "C" __attribute__((noinline)) uintptr_t FakeDisable(uintptr_t cs, u64 h, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  Record(kDis, cs, h, a2, a3, a4, a5, a6, a7);
  if (Entry* e = EntryOf(cs, h)) __atomic_store_n(&e->vis, 0u, __ATOMIC_RELAXED);
  return kRetDis;
}
extern "C" __attribute__((noinline)) uintptr_t FakeEvent(uintptr_t gs, u64 sym, u64 a, u64 b, u64 j, u64 a5, u64 a6, u64 a7) {
  Record(kEv, gs, sym, a, b, j, a5, a6, a7);
  return kRetEv;
}
using Fn8 = uintptr_t (*)(uintptr_t, u64, u64, u64, u64, u64, u64, u64);

// ---- the fake image ----
enum Mode { kPinned, kTramp };
static void Tramp(uintptr_t va, const void* target) {  // movabs rax, target; jmp rax (the page is executable, never written afterwards)
  uint8_t code[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
  const u64 t = reinterpret_cast<u64>(target);
  std::memcpy(code + 2, &t, 8);
  std::memcpy(reinterpret_cast<void*>(g_base + va), code, sizeof code);
}
static void BuildImage(Mode mode) {
  if (g_base != 0) munmap(reinterpret_cast<void*>(g_base), kImgSize);
  void* p = mmap(nullptr, kImgSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) std::abort();
  g_base = reinterpret_cast<uintptr_t>(p);
  if (mode == kPinned) {
    for (size_t i = 0; i < q15t::kSensorWdWordPinCount; ++i)
      *reinterpret_cast<uint32_t*>(g_base + q15t::kSensorWdWordPins[i].va) = q15t::kSensorWdWordPins[i].want;
    W64(kEnSlot, g_base + kEnFn);
    W64(kDisSlot, g_base + kDisFn);
    W64(kEvSlot, g_base + kEvFn);
  } else {
    Tramp(kTEnFn, reinterpret_cast<const void*>(&FakeEnable));
    Tramp(kTDisFn, reinterpret_cast<const void*>(&FakeDisable));
    Tramp(kTEvFn, reinterpret_cast<const void*>(&FakeEvent));
    mprotect(reinterpret_cast<void*>((g_base + kTEnFn) & ~0xfffull), 0x1000, PROT_READ | PROT_EXEC);
    mprotect(reinterpret_cast<void*>((g_base + kTDisFn) & ~0xfffull), 0x1000, PROT_READ | PROT_EXEC);
    mprotect(reinterpret_cast<void*>((g_base + kTEvFn) & ~0xfffull), 0x1000, PROT_READ | PROT_EXEC);
    W64(kEnSlot, g_base + kTEnFn);
    W64(kDisSlot, g_base + kTDisFn);
    W64(kEvSlot, g_base + kTEvFn);
  }
  mprotect(reinterpret_cast<void*>((g_base + kEnSlot) & ~0xfffull), 0x1000, PROT_READ);  // RELRO (both vtable words share the page)
  mprotect(reinterpret_cast<void*>((g_base + kEvSlot) & ~0xfffull), 0x1000, PROT_READ);  // the JUMP_SLOT page
}
static q15t::SensorWdAddrs TAddrs() { return {kEnSlot, kTEnFn, kDisSlot, kTDisFn, kEvSlot, kTEvFn}; }

static void Poke(uintptr_t va, u64 v) {  // a write into a read-only (RELRO) page of the fake image, as another party would do
  const uintptr_t pg = (g_base + va) & ~0xfffull;
  mprotect(reinterpret_cast<void*>(pg), 0x1000, PROT_READ | PROT_WRITE);
  W64(va, v);
  mprotect(reinterpret_cast<void*>(pg), 0x1000, PROT_READ);
}

// ---- clock ----
static std::atomic<int64_t> g_ms{100000};
static int64_t Clock() { return g_ms.load(); }
static void Advance(int64_t ms) { g_ms += ms; }

static void Fresh(Mode mode) {  // a new image, new world, a reset module; not installed
  g_lines.clear();
  g_calls.clear();
  g_marked_en = g_marked_dis = g_lock_in_real = g_log_under_lock = 0;
  q15t::ResetSensorWdForTest();
  q15t::SetSensorWdClockForTest(&Clock);
  g_ms = 100000;
  World();
  BuildImage(mode);
}
static q15t::InstallResult InstallT() { return q15t::InstallSensorWdAt(g_base, TAddrs()); }
static void Setup() {
  Fresh(kTramp);
  const q15t::InstallResult r = InstallT();
  CHECK(r.status == q15t::SwapStatus::kOk, "setup install: %s %s", q15t::StatusName(r.status), r.reason);
  g_calls.clear();
  g_lines.clear();
  g_probe_log = g_probe_real = true;
}

// ---- calls as the game makes them: through the swapped words ----
static uintptr_t En(int c, int i, u64 a2 = 0x22, u64 a3 = 0x33, u64 a4 = 0x44, u64 a5 = 0x55, u64 a6 = 0x66, u64 a7 = 0x77) {
  return reinterpret_cast<Fn8>(R64(kEnSlot))(CsAddr(c), Handle(i), a2, a3, a4, a5, a6, a7);
}
static uintptr_t Dis(int c, int i, u64 a2 = 0x22, u64 a3 = 0x33, u64 a4 = 0x44, u64 a5 = 0x55, u64 a6 = 0x66, u64 a7 = 0x77) {
  return reinterpret_cast<Fn8>(R64(kDisSlot))(CsAddr(c), Handle(i), a2, a3, a4, a5, a6, a7);
}
static uintptr_t Ev(u64 sym, u64 a, u64 b, u64 j = 1) {
  return reinterpret_cast<Fn8>(R64(kEvSlot))(0x6a6a, sym, a, b, j, 0x55, 0x66, 0x77);
}
static void Tick() {
  q15t::SensorWdTick(0);
  CHECK(!q15t::SensorWdLockHeldForTest(), "the state lock is still held after a tick");
}
static int TickCalls(int which) {  // calls the module made itself: every argument after the handle is zero
  int n = 0;
  for (const auto& c : g_calls)
    n += c.which == which && c.a2 == 0 && c.a3 == 0 && c.a4 == 0 && c.a5 == 0 && c.a6 == 0 && c.a7 == 0;
  return n;
}
static int Calls(int which) {
  int n = 0;
  for (const auto& c : g_calls) n += c.which == which;
  return n;
}
static q15t::SensorWdStats St() { return q15t::GetSensorWdStats(); }

// ---- snapshots, for "nothing was written" ----
struct Snap {
  std::vector<uint8_t> pages[2];
  u64 words[3];
  std::vector<uint32_t> pins;
  char perms[2][5];
};
static Snap Take() {
  Snap s;
  const uintptr_t pg[2] = {(g_base + kEnSlot) & ~0xfffull, (g_base + kEvSlot) & ~0xfffull};
  for (int i = 0; i < 2; ++i) {
    s.pages[i].assign(reinterpret_cast<uint8_t*>(pg[i]), reinterpret_cast<uint8_t*>(pg[i]) + 0x1000);
    q15t::Mapping m;
    if (q15t::FindMapping(pg[i], &m)) std::memcpy(s.perms[i], m.perms, 5);
    else std::memset(s.perms[i], 0, 5);
  }
  s.words[0] = R64(kEnSlot);
  s.words[1] = R64(kDisSlot);
  s.words[2] = R64(kEvSlot);
  for (size_t i = 0; i < q15t::kSensorWdWordPinCount; ++i) s.pins.push_back(R32(q15t::kSensorWdWordPins[i].va));
  return s;
}
static bool Same(const Snap& a, const Snap& b) {
  return a.pages[0] == b.pages[0] && a.pages[1] == b.pages[1] && std::memcmp(a.words, b.words, sizeof a.words) == 0 && a.pins == b.pins &&
         std::memcmp(a.perms, b.perms, sizeof a.perms) == 0;
}

// ======================================================================================================================
static void TestConstants() {
  // the two event ids are CSymbol64 of the delegate names (checked here against the hash, not against the module)
  CHECK(EchoVR::CSymbol64Hash("delegate_TriggeredPing") == kTriggered, "TriggeredPing id %016llx",
        (unsigned long long)EchoVR::CSymbol64Hash("delegate_TriggeredPing"));
  CHECK(EchoVR::CSymbol64Hash("delegate_FinishedPing") == kFinished, "FinishedPing id %016llx",
        (unsigned long long)EchoVR::CSymbol64Hash("delegate_FinishedPing"));
  const q15t::SensorWdAddrs& p = q15t::kSensorWdPinned;
  CHECK(p.en_slot == kEnSlot && p.en_fn == kEnFn && p.dis_slot == kDisSlot && p.dis_fn == kDisFn && p.event_slot == kEvSlot &&
            p.event_fn == kEvFn,
        "pinned addresses changed");
  // disassembly VA = ELF + 0x100000 (the functions as the disassembly names them: 16b127c / 16b12a8 / 190735c)
  CHECK(p.en_fn + 0x100000 == 0x16b127c && p.dis_fn + 0x100000 == 0x16b12a8 && p.event_fn + 0x100000 == 0x190735c, "disassembly VAs");
  CHECK(p.dis_slot == p.en_slot + 8, "the two vtable words are adjacent");
  // every pin lies inside one of the three functions it was read from
  for (size_t i = 0; i < q15t::kSensorWdWordPinCount; ++i) {
    const uintptr_t va = q15t::kSensorWdWordPins[i].va;
    const bool in_en = va >= kEnFn && va < kEnFn + 44, in_dis = va >= kDisFn && va < kDisFn + 44, in_ev = va >= kEvFn && va < kEvFn + 48;
    CHECK(in_en || in_dis || in_ev, "pin %zu (0x%llx) is in none of the three functions", i, (unsigned long long)va);
  }
  auto count = [](const char* needle, int level) { return CountLines(needle, level); };
  auto clear = []() { g_lines.clear(); };
  g_fail += pincheck::Run("sensorwd", q15t::kSensorWdWordPins, q15t::kSensorWdWordPinCount, 34, 0x8b1c277349b4443full,
                          [](uintptr_t b) { return q15t::SensorWdWordPinsMatch(b); }, count, clear);
}

static void TestInstallPinnedImage() {
  Fresh(kPinned);
  const Snap before = Take();
  const q15t::InstallResult r = q15t::InstallSensorWd(g_base);
  CHECK(r.status == q15t::SwapStatus::kOk, "install: %s %s", q15t::StatusName(r.status), r.reason);
  const Snap after = Take();
  CHECK(R64(kEnSlot) != g_base + kEnFn && R64(kEnSlot) != 0, "enable word not swapped");
  CHECK(R64(kDisSlot) != g_base + kDisFn && R64(kDisSlot) != 0, "disable word not swapped");
  CHECK(R64(kEvSlot) != g_base + kEvFn && R64(kEvSlot) != 0, "event slot not swapped");
  CHECK(R64(kEnSlot) != R64(kDisSlot) && R64(kEnSlot) != R64(kEvSlot) && R64(kDisSlot) != R64(kEvSlot), "two words got the same wrapper");
  CHECK(after.pins == before.pins, "a code word changed");
  CHECK(std::strcmp(after.perms[0], "r--p") == 0 && std::strcmp(after.perms[1], "r--p") == 0, "RELRO not restored: %s %s", after.perms[0],
        after.perms[1]);
  CHECK(HasLine("sensorwd on", 1) && HasLine("enable_slot=0x36495c8") && HasLine("disable_slot=0x36495d0") &&
            HasLine("event_slot=0x36fcae8") && HasLine("watchdog_ms=6000") && HasLine("kill_file=sensorwd.off"),
        "no install line naming the three words");
  CHECK(CountLines("sensorwd word swapped", 1) == 3 && CountLines("status=ok") >= 3, "swap lines: %d", CountLines("sensorwd word swapped", 1));
  CHECK(ErrorLines() == 0, "install logged an error");
  CHECK(St().on, "stats.on");
  // everything else on the two pages is unchanged: only the three words differ
  int diff = 0;
  for (int pg = 0; pg < 2; ++pg)
    for (size_t i = 0; i < 0x1000; ++i) diff += before.pages[pg][i] != after.pages[pg][i];
  CHECK(diff > 0 && diff <= 24, "%d bytes changed on the two pages", diff);
}

static void TestRefusals() {
  // a different build: all-zero text (the pins read zero), nothing written
  Fresh(kPinned);
  std::memset(reinterpret_cast<void*>((g_base + kEnFn) & ~0xfffull), 0, 0x1000);
  {
    const Snap b = Take();
    g_lines.clear();
    const q15t::InstallResult r = q15t::InstallSensorWd(g_base);
    CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "word_pin_mismatch") == 0, "other build: %s %s", q15t::StatusName(r.status), r.reason);
    CHECK(Same(b, Take()), "other build: something was written");
    CHECK(CountLines("code word pin mismatch", 3) == 1 && ErrorLines() == 1, "other build: not exactly one ERROR line (%d)", ErrorLines());
    CHECK(!St().on, "other build: on");
  }
  // one changed pin in each of the three functions: refused, named, nothing written
  const uintptr_t pin_at[3] = {kEnFn + 8, kDisFn + 12, kEvFn + 16};
  for (int k = 0; k < 3; ++k) {
    Fresh(kPinned);
    *reinterpret_cast<uint32_t*>(g_base + pin_at[k]) ^= 0x100;
    const Snap b = Take();
    g_lines.clear();
    const q15t::InstallResult r = q15t::InstallSensorWd(g_base);
    CHECK(std::strcmp(r.reason, "word_pin_mismatch") == 0 && r.status != q15t::SwapStatus::kOk, "pin %d: %s", k, r.reason);
    char need[64];
    std::snprintf(need, sizeof need, "pin=0x%llx found=", (unsigned long long)pin_at[k]);
    CHECK(CountLines(need, 3) == 1 && ErrorLines() == 1, "pin %d: ERROR line missing or not alone", k);
    CHECK(Same(b, Take()), "pin %d: something was written", k);
  }
  // the enable vtable word is not the stock function (another build's vtable)
  {
    Fresh(kPinned);
    Poke(kEnSlot, g_base + kEnFn + 4);
    const Snap b = Take();
    g_lines.clear();
    const q15t::InstallResult r = q15t::InstallSensorWd(g_base);
    CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal && std::strcmp(r.reason, "vtable_word_not_stock") == 0, "enable word: %s %s",
          q15t::StatusName(r.status), r.reason);
    char need[160];
    std::snprintf(need, sizeof need, "pin=0x36495c8 found=0x%llx expected=0x%llx", (unsigned long long)(g_base + kEnFn + 4),
                  (unsigned long long)(g_base + kEnFn));
    CHECK(CountLines("sensorwd refused", 3) == 1 && CountLines(need, 3) == 1 && ErrorLines() == 1, "enable word: refusal not named");
    CHECK(Same(b, Take()), "enable word: something was written");
    CHECK(!St().on, "enable word: on");
  }
  // the disable word is not (the enable word is: still nothing may be written, in particular not the enable word)
  {
    Fresh(kPinned);
    Poke(kDisSlot, g_base + kDisFn + 4);
    const Snap b = Take();
    g_lines.clear();
    const q15t::InstallResult r = q15t::InstallSensorWd(g_base);
    CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal && std::strcmp(r.reason, "vtable_word_not_stock") == 0, "disable word: %s %s",
          q15t::StatusName(r.status), r.reason);
    char need[160];
    std::snprintf(need, sizeof need, "pin=0x36495d0 found=0x%llx expected=0x%llx", (unsigned long long)(g_base + kDisFn + 4),
                  (unsigned long long)(g_base + kDisFn));
    CHECK(CountLines("sensorwd refused", 3) == 1 && CountLines(need, 3) == 1 && ErrorLines() == 1, "disable word: refusal not named");
    CHECK(Same(b, Take()), "disable word: the enable word or the event slot was swapped before the check");
  }
  // the event slot is not the stock function
  {
    Fresh(kPinned);
    Poke(kEvSlot, 0x4242420);
    const Snap b = Take();
    g_lines.clear();
    const q15t::InstallResult r = q15t::InstallSensorWd(g_base);
    CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal && std::strcmp(r.reason, "event_slot_not_stock") == 0, "event slot: %s %s",
          q15t::StatusName(r.status), r.reason);
    char need[160];
    std::snprintf(need, sizeof need, "pin=0x36fcae8 found=0x4242420 expected=0x%llx", (unsigned long long)(g_base + kEvFn));
    CHECK(CountLines("sensorwd refused", 3) == 1 && CountLines(need, 3) == 1 && ErrorLines() == 1, "event slot: refusal not named");
    CHECK(Same(b, Take()), "event slot: something was written (the vtable words are swapped before the check)");
  }
  // a word that cannot be read at all
  {
    Fresh(kTramp);
    q15t::SensorWdAddrs a = TAddrs();
    a.dis_slot = 0xffffffffff000ull;  // far beyond any mapping
    g_lines.clear();
    const q15t::InstallResult r = q15t::InstallSensorWdAt(g_base, a);
    CHECK(r.status == q15t::SwapStatus::kUnmapped && std::strcmp(r.reason, "vtable_word_not_stock") == 0, "unreadable: %s %s", q15t::StatusName(r.status), r.reason);
    CHECK(CountLines("found=unreadable", 3) == 1 && ErrorLines() == 1, "unreadable word not named");
    CHECK(R64(kEnSlot) == g_base + kTEnFn, "unreadable: the enable word was written");
  }
  // no base
  {
    Fresh(kTramp);
    const q15t::InstallResult r = q15t::InstallSensorWdAt(0, TAddrs());
    CHECK(std::strcmp(r.reason, "no_base") == 0 && r.status != q15t::SwapStatus::kOk, "no base: %s", r.reason);
  }
  // the kill file
  {
    char tmpl[] = "/tmp/q15t-sensorwd-XXXXXX";
    if (mkdtemp(tmpl) == nullptr) std::abort();
    const std::string dir = tmpl;
    q15t::SetModuleSwitchDirForTest(dir.c_str());
    // other modules' kill files do not switch this one off
    FILE* o = std::fopen((dir + "/fxtrail.off").c_str(), "w");
    std::fclose(o);
    Fresh(kPinned);
    q15t::InstallResult r = q15t::InstallSensorWd(g_base);
    CHECK(r.status == q15t::SwapStatus::kOk, "another module's kill file switched sensorwd off: %s", r.reason);
    unlink((dir + "/fxtrail.off").c_str());
    // sensorwd.off: refused, nothing written, one INFO line, no ERROR
    FILE* m = std::fopen((dir + "/sensorwd.off").c_str(), "w");
    std::fclose(m);
    Fresh(kPinned);
    const Snap b = Take();
    g_lines.clear();
    r = q15t::InstallSensorWd(g_base);
    CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "off_file") == 0, "kill file ignored: %s %s", q15t::StatusName(r.status), r.reason);
    CHECK(Same(b, Take()), "kill file: something was written");
    CHECK(CountLines("sensorwd off", 1) == 1 && HasLine("sensorwd.off present") && ErrorLines() == 0, "kill file: log lines");
    CHECK(!St().on, "kill file: on");
    // the tick does nothing and the words are still stock
    Tick();
    unlink((dir + "/sensorwd.off").c_str());
    rmdir(dir.c_str());
    q15t::SetModuleSwitchDirForTest("");
  }
  // the tick after a refusal and before any install does nothing
  {
    Fresh(kPinned);
    g_lines.clear();
    Tick();
    CHECK(g_lines.empty() && !St().on, "tick without an install logged or acted");
  }
}

static void TestInstallFailures() {
  // the event slot's page is executable: the swap is refused, the vtable words are untouched
  {
    Fresh(kTramp);
    mprotect(reinterpret_cast<void*>((g_base + kEvSlot) & ~0xfffull), 0x1000, PROT_READ | PROT_EXEC);
    const Snap b = Take();
    g_lines.clear();
    const q15t::InstallResult r = InstallT();
    CHECK(r.status == q15t::SwapStatus::kExecutablePage, "executable event page: %s", q15t::StatusName(r.status));
    CHECK(R64(kEnSlot) == g_base + kTEnFn && R64(kDisSlot) == g_base + kTDisFn && R64(kEvSlot) == b.words[2], "words changed after the refused swap");
    CHECK(CountLines("sensorwd refused", 3) == 1 && ErrorLines() == 1 && !St().on, "event swap failure: ERROR or on-state");
    Tick();
  }
  // the enable word changes between the check and its swap: the event slot is put back
  static uintptr_t captured_event = 0, captured_enable = 0;
  {
    Fresh(kTramp);
    captured_event = captured_enable = 0;
    q15t::SetSensorWdHookForTest([](int stage) {
      if (stage == 1) {
        captured_event = R64(kEvSlot);
        Poke(kEnSlot, 0x5151510);
      }
    });
    g_lines.clear();
    const q15t::InstallResult r = InstallT();
    CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal, "enable raced: %s", q15t::StatusName(r.status));
    CHECK(R64(kEvSlot) == g_base + kTEvFn, "enable swap failed and the event slot was not put back");
    CHECK(R64(kEnSlot) == 0x5151510 && R64(kDisSlot) == g_base + kTDisFn, "someone else's word was overwritten");
    CHECK(HasLine("word put back to the stock function", 2) && ErrorLines() == 1 && !St().on, "put-back / error lines: %d errors", ErrorLines());
    // the wrapper this install had put in the event slot is inert now: it passes the call through and tracks nothing
    g_calls.clear();
    CHECK(captured_event != 0, "no wrapper captured");
    const uintptr_t ret = reinterpret_cast<Fn8>(captured_event)(1, kTriggered, 5, 6, 7, 8, 9, 10);
    CHECK(ret == kRetEv && Calls(kEv) == 1 && St().pings == 0, "the put-back wrapper still acted (ret %lx)", (unsigned long)ret);
    q15t::SetSensorWdHookForTest(nullptr);
  }
  // the disable word changes: both earlier swaps are put back
  {
    Fresh(kTramp);
    captured_event = captured_enable = 0;
    q15t::SetSensorWdHookForTest([](int stage) {
      if (stage == 1) captured_event = R64(kEvSlot);
      if (stage == 2) {
        captured_enable = R64(kEnSlot);
        Poke(kDisSlot, 0x5151510);
      }
    });
    g_lines.clear();
    const q15t::InstallResult r = InstallT();
    CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal, "disable raced: %s", q15t::StatusName(r.status));
    CHECK(R64(kEvSlot) == g_base + kTEvFn, "disable swap failed and the event slot was not put back");
    CHECK(R64(kEnSlot) == g_base + kTEnFn, "disable swap failed and the enable word was not put back");
    CHECK(R64(kDisSlot) == 0x5151510, "someone else's word was overwritten");
    CHECK(CountLines("word put back to the stock function", 2) == 2 && ErrorLines() == 1 && !St().on, "put-back lines: %d errors", ErrorLines());
    // both wrappers that were in are inert: they pass through, track nothing
    g_calls.clear();
    const uintptr_t ret = reinterpret_cast<Fn8>(captured_enable)(CsAddr(0), Handle(0), 1, 2, 3, 4, 5, 6);
    CHECK(ret == kRetEn && Calls(kEn) == 1 && St().enables == 0 && St().tracked == 0, "the put-back enable wrapper still tracked");
    reinterpret_cast<Fn8>(captured_event)(1, kTriggered, 5, 6, 7, 8, 9, 10);
    CHECK(St().pings == 0, "the put-back event wrapper still counted");
    q15t::SetSensorWdHookForTest(nullptr);
  }
  // a put-back that cannot be done is an ERROR
  {
    Fresh(kTramp);
    q15t::SetSensorWdHookForTest([](int stage) {
      if (stage == 2) {
        Poke(kDisSlot, 0x5151510);
        Poke(kEvSlot, 0x6161610);  // someone else took the event slot meanwhile
      }
    });
    g_lines.clear();
    InstallT();
    CHECK(CountLines("word could not be put back", 3) == 1 && R64(kEvSlot) == 0x6161610, "failed put-back not loud / not left alone");
    q15t::SetSensorWdHookForTest(nullptr);
  }
}

static void TestPassThrough() {
  Setup();
  // sphere enable / disable: the real function gets x0..x7 exactly, its value comes back
  uintptr_t r = En(0, 2, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7);
  CHECK(r == kRetEn, "enable return %lx", (unsigned long)r);
  CHECK(g_calls.size() == 1 && g_calls[0].which == kEn && g_calls[0].cs == CsAddr(0) && g_calls[0].h == Handle(2) && g_calls[0].a2 == 0xa2 &&
            g_calls[0].a3 == 0xa3 && g_calls[0].a4 == 0xa4 && g_calls[0].a5 == 0xa5 && g_calls[0].a6 == 0xa6 && g_calls[0].a7 == 0xa7,
        "enable arguments changed");
  g_calls.clear();
  r = Dis(0, 2, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7);
  CHECK(r == kRetDis, "disable return %lx", (unsigned long)r);
  CHECK(g_calls.size() == 1 && g_calls[0].which == kDis && g_calls[0].cs == CsAddr(0) && g_calls[0].h == Handle(2) && g_calls[0].a2 == 0xb2 &&
            g_calls[0].a3 == 0xb3 && g_calls[0].a4 == 0xb4 && g_calls[0].a5 == 0xb5 && g_calls[0].a6 == 0xb6 && g_calls[0].a7 == 0xb7,
        "disable arguments changed");
  // the event: TriggeredPing, FinishedPing and a foreign symbol all reach the real function unchanged
  const u64 syms[3] = {kTriggered, kFinished, kOtherSym};
  for (int k = 0; k < 3; ++k) {
    g_calls.clear();
    r = reinterpret_cast<Fn8>(R64(kEvSlot))(0x1a1a, syms[k], 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7);
    CHECK(r == kRetEv, "event %d return %lx", k, (unsigned long)r);
    CHECK(g_calls.size() == 1 && g_calls[0].which == kEv && g_calls[0].cs == 0x1a1a && g_calls[0].h == syms[k] && g_calls[0].a2 == 0xc2 &&
              g_calls[0].a3 == 0xc3 && g_calls[0].a4 == 0xc4 && g_calls[0].a5 == 0xc5 && g_calls[0].a6 == 0xc6 && g_calls[0].a7 == 0xc7,
          "event %d arguments changed", k);
  }
  CHECK(St().pings == 1 && St().finished == 1, "ping / finished counts %u %u", St().pings, St().finished);
  CHECK(g_lock_in_real == 0 && g_log_under_lock == 0, "the lock was held across a game call (%d) or a log call (%d)", (int)g_lock_in_real,
        (int)g_log_under_lock);
  // a component that cannot be read (garbage pointer): the call still reaches the real function, nothing is tracked, no fault
  g_calls.clear();
  const q15t::SensorWdStats s1 = St();  // the one tracked sphere of the first call above
  CHECK(s1.enables == 1 && s1.disables == 1 && s1.tracked == 1, "first calls: %u %u %u", s1.enables, s1.disables, s1.tracked);
  r = reinterpret_cast<Fn8>(R64(kEnSlot))(0x10, 0x5, 1, 2, 3, 4, 5, 6);
  CHECK(r == kRetEn && Calls(kEn) == 1 && St().enables == 1 && St().tracked == 1, "unreadable component");
  r = reinterpret_cast<Fn8>(R64(kDisSlot))(0xdead0000dead0000ull, 0x5, 1, 2, 3, 4, 5, 6);
  CHECK(r == kRetDis && Calls(kDis) == 1 && St().disables == 1, "unreadable component (disable)");
  // a null model pointer in the entry
  SetNullModel(0, 3);
  g_calls.clear();
  r = En(0, 3);
  CHECK(r == kRetEn && Calls(kEn) == 1 && St().enables == 1 && St().tracked == 1, "null model");
  // the module switched off (a rolled-back install): the wrappers only pass the call on
  g_calls.clear();
  q15t::SetSensorWdOnForTest(false);
  const q15t::SensorWdStats s0 = St();
  En(0, 4);
  Dis(0, 4);
  Ev(kTriggered, 1, 2);
  Ev(kFinished, 1, 2);
  CHECK(Calls(kEn) == 1 && Calls(kDis) == 1 && Calls(kEv) == 2, "off: calls not passed on");
  CHECK(St().enables == s0.enables && St().disables == s0.disables && St().pings == s0.pings && St().finished == s0.finished &&
            St().tracked == s0.tracked,
        "off: the wrappers still counted or tracked");
  q15t::SetSensorWdOnForTest(true);
}

static void TestNonSensorModels() {
  Setup();
  SetKind(0, 1, kOtherModel);
  SetKind(0, 2, 0);
  En(0, 1);
  En(0, 2);
  Dis(0, 1);
  Dis(0, 2);
  CHECK(Calls(kEn) == 2 && Calls(kDis) == 2, "real functions not called for other models");
  CHECK(St().enables == 0 && St().disables == 0 && St().tracked == 0, "other models were counted or tracked (%u %u %u)", St().enables,
        St().disables, St().tracked);
  Advance(100000);
  Tick();
  Advance(100000);
  Tick();
  CHECK(TickCalls(kEn) == 0 && TickCalls(kDis) == 0 && St().wd_off == 0, "the tick touched a model that is not the sphere");
  CHECK(CountLines("sensorwd off:", 1) == 0, "an off line for a non-sensor model");
}

static void TestEnableTimedAndOff() {
  Setup();
  En(0, 0);
  CHECK(St().enables == 1 && St().tracked == 1 && St().enabled == 1, "enable not tracked: %u %u %u", St().enables, St().tracked, St().enabled);
  CHECK(Vis(0, 0) == 1, "fake enable did not run");
  Advance(5999);
  Tick();
  CHECK(TickCalls(kDis) == 0 && Vis(0, 0) == 1, "turned off at 5999 ms");
  Advance(1);
  Tick();
  CHECK(TickCalls(kDis) == 1, "not turned off at 6000 ms (%d)", TickCalls(kDis));
  CHECK(Vis(0, 0) == 0, "the sphere is still visible after the tick");
  bool found = false;
  for (const auto& c : g_calls)
    found |= c.which == kDis && c.cs == CsAddr(0) && c.h == Handle(0) && c.a2 == 0 && c.a3 == 0 && c.a4 == 0 && c.a5 == 0 && c.a6 == 0 && c.a7 == 0;
  CHECK(found, "the tick called disable with other arguments than (cs, h, 0 x6)");
  CHECK(St().wd_off == 1 && St().forced == 1 && St().enabled == 0, "stats after the off: %u %u %u", St().wd_off, St().forced, St().enabled);
  CHECK(CountLines("sensorwd off: sphere still enabled 6000 ms after its enable", 1) == 1 && HasLine("age_ms=6000") && HasLine("n=1"), "off line");
  char cs[40];
  std::snprintf(cs, sizeof cs, "cs=%lx h=%llx", (unsigned long)CsAddr(0), (unsigned long long)Handle(0));
  CHECK(HasLine(cs), "the off line does not name cs and h");
  // once off, a later tick does not call again
  Advance(60000);
  Tick();
  Tick();
  CHECK(TickCalls(kDis) == 1 && St().wd_off == 1, "turned off twice");
  CHECK(g_lock_in_real == 0 && g_log_under_lock == 0, "lock held across a game call (%d) or a log call (%d)", (int)g_lock_in_real, (int)g_log_under_lock);
}

static void TestDisableClears() {
  Setup();
  En(0, 0);
  Advance(1000);
  Dis(0, 0);
  CHECK(St().disables == 1 && St().enabled == 0 && St().tracked == 1, "disable not tracked: %u %u %u", St().disables, St().enabled, St().tracked);
  Advance(20000);
  Tick();
  CHECK(TickCalls(kDis) == 0 && St().wd_off == 0, "a sphere that was disabled was turned off again");
  // the engine's own re-enable restarts the timer
  En(0, 1);
  Advance(5000);
  En(0, 1);
  Advance(1000);
  Tick();
  CHECK(TickCalls(kDis) == 0, "the second enable did not restart the 6 s");
  Advance(5000);
  Tick();
  CHECK(TickCalls(kDis) == 1, "not turned off 6 s after the last enable");
  // a disable of a sphere nobody enabled takes no slot
  Setup();
  Dis(1, 7);
  CHECK(St().tracked == 0 && St().disables == 1, "disable of an untracked sphere: tracked %u, counted %u", St().tracked, St().disables);
}

static void TestForcedBackOn() {
  Setup();
  const u64 A = 0x1001, B = 0x2002;
  Ev(kTriggered, A, B);
  Advance(100);
  En(0, 0);  // both spheres are enabled within 250 ms of the ping: both belong to actor (A, B)
  En(1, 0);
  Advance(6000);
  Tick();
  CHECK(TickCalls(kDis) == 2, "both spheres should be turned off (%d)", TickCalls(kDis));
  g_calls.clear();
  // a ping of another actor: nothing comes back
  Ev(kTriggered, 0x1111, 0x2222);
  Tick();
  CHECK(TickCalls(kEn) == 0, "a ping of another actor turned a sphere on");
  // only the first half of the pair
  Ev(kTriggered, A, 0x2223);
  Tick();
  CHECK(TickCalls(kEn) == 0, "a ping that shares only actor a turned a sphere on");
  Ev(kTriggered, 0x1112, B);
  Tick();
  CHECK(TickCalls(kEn) == 0, "a ping that shares only actor b turned a sphere on");
  // the same actor pings again: the forced spheres of that actor come back on, once each
  Advance(5000);
  Ev(kTriggered, A, B);
  Tick();
  CHECK(TickCalls(kEn) == 2, "forced spheres not turned back on (%d)", TickCalls(kEn));
  CHECK(Vis(0, 0) == 1 && Vis(1, 0) == 1, "the spheres are not visible again");
  bool args_ok = true;
  for (const auto& c : g_calls)
    if (c.which == kEn) args_ok &= c.a2 == 0 && c.a3 == 0 && c.a4 == 0 && c.a5 == 0 && c.a6 == 0 && c.a7 == 0 && (c.cs == CsAddr(0) || c.cs == CsAddr(1));
  CHECK(args_ok, "back-on call arguments");
  CHECK(St().wd_back == 2 && St().forced == 0 && St().enabled == 2, "stats after back-on: %u %u %u", St().wd_back, St().forced, St().enabled);
  CHECK(CountLines("sensorwd back on: its actor pinged again", 1) == 2, "back-on lines: %d", CountLines("sensorwd back on", 1));
  // the ping was consumed: the next tick does not repeat it
  Tick();
  CHECK(TickCalls(kEn) == 2, "the ping was used twice");
  // and they are watched again: 5999 ms later nothing, at 6000 ms off again
  g_calls.clear();
  Advance(5999);
  Tick();
  CHECK(TickCalls(kDis) == 0, "turned off too early after the back-on (%d)", TickCalls(kDis));
  Advance(1);
  Tick();
  CHECK(TickCalls(kDis) == 2, "not watched again after the back-on (%d)", TickCalls(kDis));
  // a ping for a sphere that is not forced calls nothing
  g_calls.clear();
  En(2, 0);
  Ev(kTriggered, 0x7, 0x8);
  Tick();
  CHECK(TickCalls(kEn) == 0, "an enabled sphere was enabled again by the tick");
  // the engine enables a forced sphere itself: it is a live sphere again, timed from that enable
  Setup();
  Ev(kTriggered, A, B);
  En(0, 0);
  Advance(6000);
  Tick();
  CHECK(TickCalls(kDis) == 1, "setup: off");
  Advance(1000);
  En(0, 0);
  g_calls.clear();
  Ev(kTriggered, A, B);  // it is live again: its actor's ping has nothing to bring back
  Tick();
  CHECK(TickCalls(kEn) == 0 && St().wd_back == 0 && St().forced == 0, "a ping enabled a sphere the engine had already re-enabled");
  Advance(6000);
  Tick();
  CHECK(TickCalls(kDis) == 1, "a sphere the engine re-enabled was not watched again");
  // the engine disables a forced sphere itself: a later ping of its actor does not turn it on
  Setup();
  Ev(kTriggered, A, B);
  En(0, 0);
  Advance(6000);
  Tick();
  Dis(0, 0);
  g_calls.clear();
  Ev(kTriggered, A, B);
  Tick();
  CHECK(TickCalls(kEn) == 0 && St().forced == 0, "a sphere the engine disabled was turned on by its actor's ping");
}

static void TestPairingWindow() {
  Setup();
  // sphere (0,0) enabled 250 ms after the ping: paired; sphere (0,1) 251 ms after: not
  Ev(kTriggered, 0xa1, 0xb1);
  Advance(250);
  En(0, 0);
  Advance(1);
  En(0, 1);
  Advance(6000);
  Tick();
  CHECK(TickCalls(kDis) == 2, "both turned off");
  g_calls.clear();
  Ev(kTriggered, 0xa1, 0xb1);
  Tick();
  CHECK(TickCalls(kEn) == 1, "only the sphere enabled within 250 ms of the ping belongs to it (%d)", TickCalls(kEn));
  bool first = false;
  for (const auto& c : g_calls) first |= c.which == kEn && c.h == Handle(0);
  CHECK(first, "the wrong sphere came back");
  // a sphere that was paired once keeps its first actor
  Setup();
  Ev(kTriggered, 0xa1, 0);  // an actor pair whose second half is 0 is still a pair
  En(0, 0);
  Dis(0, 0);
  Advance(1000);
  Ev(kTriggered, 0xa2, 0xb2);
  En(0, 0);  // its second enable: still actor 1
  Advance(6000);
  Tick();
  g_calls.clear();
  Ev(kTriggered, 0xa2, 0xb2);
  Tick();
  CHECK(TickCalls(kEn) == 0, "a sphere changed its actor on its second enable");
  Ev(kTriggered, 0xa1, 0);
  Tick();
  CHECK(TickCalls(kEn) == 1, "a sphere lost its first actor (or a ping with b == 0 was not queued)");
  // a sphere enabled with no ping at all has no actor: a ping of actor (0, 0) does not bring it back, and a real one does not either
  Setup();
  En(0, 0);
  Advance(6000);
  Tick();
  g_calls.clear();
  Ev(kTriggered, 0, 0);
  Ev(kTriggered, 0x5, 0x6);
  Tick();
  CHECK(TickCalls(kEn) == 0 && St().pings == 2, "an unpaired sphere was brought back (%d)", TickCalls(kEn));
}

static void TestPingQueue() {
  Setup();
  // 3 forced spheres of 3 actors, 3 pings in one frame: all three come back
  for (int c = 0; c < 3; ++c) {
    Ev(kTriggered, 0x100 + c, 0x200 + c);
    Advance(10);
    En(c, 0);
    Advance(300);
  }
  Advance(6000);
  Tick();
  CHECK(TickCalls(kDis) == 3, "off x3 (%d)", TickCalls(kDis));
  g_calls.clear();
  for (int c = 0; c < 3; ++c) Ev(kTriggered, 0x100 + c, 0x200 + c);
  Tick();
  CHECK(TickCalls(kEn) == 3, "three pings in one frame: %d spheres came back", TickCalls(kEn));
  // pings of actor (0, 0) are not queued: they take no room
  Setup();
  for (int k = 0; k < 9; ++k) Ev(kTriggered, 0, 0);
  Ev(kTriggered, 0x9, 0xa);
  CHECK(St().pings == 10 && St().pings_lost == 0, "zero-actor pings took queue room: lost %u", St().pings_lost);
  // more pings than the queue holds: the extra ones are counted and warned about, once
  Setup();
  for (int k = 0; k < 10; ++k) Ev(kTriggered, 0x300 + k, 0x400 + k);
  CHECK(St().pings == 10 && St().pings_lost == 2, "pings %u lost %u", St().pings, St().pings_lost);
  CHECK(CountLines("TriggeredPing dropped", 2) == 2, "lost-ping warnings: %d", CountLines("TriggeredPing dropped"));
  Tick();
  Ev(kTriggered, 0x500, 0x600);
  CHECK(St().pings_lost == 2, "the queue was not emptied by the tick");
  // FinishedPing changes nothing
  Setup();
  En(0, 0);
  Ev(kFinished, 0x1, 0x2);
  CHECK(St().finished == 1 && St().pings == 0 && St().enabled == 1, "FinishedPing: %u %u %u", St().finished, St().pings, St().enabled);
  Advance(6000);
  Tick();
  CHECK(TickCalls(kDis) == 1, "a FinishedPing event stopped the watch");
  // a foreign event is not counted
  Ev(kOtherSym, 1, 2);
  CHECK(St().pings == 0 && St().finished == 1, "a foreign event was counted");
}

static void TestStaleAndSweep() {
  Setup();
  En(0, 0);
  En(0, 1);
  CHECK(St().tracked == 2, "tracked %u", St().tracked);
  // the component of sphere 0 is recycled for another model before the tick: the module does not call into it
  SetKind(0, 0, kOtherModel);
  Advance(6000);
  Tick();
  CHECK(TickCalls(kDis) == 1 && Vis(0, 1) == 0, "tick calls: %d", TickCalls(kDis));
  bool called0 = false;
  for (const auto& c : g_calls) called0 |= c.which == kDis && c.h == Handle(0) && c.a2 == 0;
  CHECK(!called0, "the tick called into a component that is no longer the sphere");
  CHECK(St().stale == 1 && St().tracked == 1, "stale %u tracked %u", St().stale, St().tracked);
  // the sweep: spheres that were disabled (nothing for the tick to act on) are dropped when their component stops being one, every 5 s
  Setup();
  En(0, 5);
  Dis(0, 5);
  En(0, 6);
  Dis(0, 6);
  Tick();  // the first tick sweeps
  CHECK(St().tracked == 2 && St().stale == 0, "first sweep dropped live spheres");
  CHECK(CountLines("tracked spheres whose component") == 0, "a sweep that dropped nothing logged");
  SetKind(0, 5, kOtherModel);
  Advance(4999);
  Tick();
  CHECK(St().tracked == 2, "swept before 5 s (tracked %u)", St().tracked);
  Advance(1);
  Tick();
  CHECK(St().tracked == 1 && St().stale == 1, "sweep: tracked %u stale %u", St().tracked, St().stale);
  CHECK(CountLines("tracked spheres whose component no longer reads as the sphere were dropped", 1) == 1 && HasLine("dropped=1") &&
            HasLine("tracked_before=2"),
        "sweep line");
  Advance(5000);
  Tick();
  CHECK(St().tracked == 1 && St().stale == 1, "a live sphere was dropped by the sweep");
}

static void TestTableFull() {
  Setup();
  for (int i = 0; i < 32; ++i) En(0, i);
  CHECK(St().tracked == 32 && St().table_full == 0, "32 spheres: tracked %u full %u", St().tracked, St().table_full);
  En(0, 32);
  En(0, 33);
  CHECK(St().tracked == 32 && St().table_full == 2 && St().enables == 34, "33rd and 34th: tracked %u full %u enables %u", St().tracked, St().table_full, St().enables);
  CHECK(CountLines("no free slot for a sphere enable", 2) == 2, "full warnings %d", CountLines("no free slot"));
  CHECK(Calls(kEn) == 34, "an enable was not passed on when the table was full");
  // the same sphere enabled again is the same slot
  En(0, 3);
  CHECK(St().tracked == 32 && St().table_full == 2, "re-enable took a second slot");
  // 10 components die: the sweep frees their slots, the next enable is tracked
  for (int i = 0; i < 10; ++i) SetKind(0, i, kOtherModel);
  Tick();
  CHECK(St().tracked == 22 && St().stale == 10, "after the sweep: tracked %u stale %u", St().tracked, St().stale);
  En(0, 32);
  CHECK(St().tracked == 23 && St().table_full == 2, "freed slot not reused");
}

static void TestLogBudgets() {
  Setup();
  // 70 off actions: 64 lines and one cap warning
  for (int k = 0; k < 70; ++k) {
    En(0, 0);
    Advance(6000);
    Tick();
    Dis(0, 0);  // clears the forced state for the next round
  }
  CHECK(St().wd_off == 70, "wd_off %u", St().wd_off);
  CHECK(CountLines("sensorwd off: sphere still enabled", 1) == 64, "off lines %d", CountLines("sensorwd off: sphere still enabled"));
  CHECK(CountLines("sensorwd log cap reached", 2) == 1 && HasLine("lines=actions") && HasLine("cap=64"), "cap warning lines %d", CountLines("log cap reached"));
  // thread lines: one per role and thread, capped at 24
  Setup();
  En(0, 0);
  Dis(0, 0);
  Ev(kTriggered, 1, 2);
  Tick();
  Tick();
  En(0, 1);
  char tid[24];
  std::snprintf(tid, sizeof tid, "%ld", (long)syscall(SYS_gettid));
  CHECK(CountLines("sensorwd thread", 1) == 4, "thread lines %d", CountLines("sensorwd thread"));
  const char* roles[4] = {"role=enable", "role=disable", "role=event", "role=tick"};
  for (const char* r : roles) CHECK(CountLines(r) == 1, "%s lines %d", r, CountLines(r));
  CHECK(HasLine((std::string("role=tick tid=") + tid).c_str()) && HasLine((std::string("role=event tid=") + tid + " tick_tid=0 enable_tid=" + tid).c_str()),
        "thread line fields");
  // the same role on another thread is one more line, naming both
  std::thread t([] { En(1, 0); });
  t.join();
  CHECK(CountLines("role=enable", 1) == 2, "second enable thread not noted (%d)", CountLines("role=enable"));
  for (int k = 0; k < 40; ++k) {  // the role moves between two threads 40 times: capped
    std::thread u([] { En(2, 1); });
    u.join();
    En(2, 2);
  }
  CHECK(CountLines("sensorwd thread", 1) == 24, "thread line cap: %d", CountLines("sensorwd thread", 1));
  CHECK(CountLines("sensorwd log cap reached", 2) == 1 && HasLine("lines=threads") && HasLine("cap=24"), "thread cap warning");
  // table-full chatter is capped at 12
  Setup();
  for (int i = 0; i < 32; ++i) En(0, i);
  for (int i = 0; i < 20; ++i) En(1, i);
  CHECK(CountLines("no free slot", 2) == 12, "full chatter %d", CountLines("no free slot"));
  CHECK(CountLines("sensorwd log cap reached", 2) == 1 && HasLine("lines=notes") && HasLine("cap=12"), "notes cap warning");
}

static int g_held[32];
static int g_hits[32];
static void ProbeHook(int stage) {
  if (stage < 11) return;
  ++g_hits[stage];
  if (q15t::SensorWdLockHeldForTest()) ++g_held[stage];
}
static void TestLockDiscipline() {
  Setup();
  std::memset(g_held, 0, sizeof g_held);
  std::memset(g_hits, 0, sizeof g_hits);
  q15t::SetSensorWdHookForTest(&ProbeHook);
  En(0, 0);            // 11
  Dis(0, 0);           // 11
  Ev(kTriggered, 1, 2);  // 13
  Advance(7000);
  En(0, 1);
  Advance(6000);
  SetKind(0, 2, kSphere);
  En(0, 2);
  SetKind(0, 2, kOtherModel);
  Tick();              // 12, the first tick's sweep 15 and the dead sphere 14
  (void)St();          // 16
  const int stages[6] = {11, 12, 13, 14, 15, 16};
  for (int s : stages) {
    CHECK(g_hits[s] > 0, "stage %d never reached", s);
    CHECK(g_held[s] == g_hits[s], "stage %d: the state lock was held %d of %d times", s, g_held[s], g_hits[s]);
  }
  CHECK(!q15t::SensorWdLockHeldForTest(), "the lock is held after the calls");
  CHECK(g_lock_in_real == 0 && g_log_under_lock == 0, "lock held across a game call (%d) or a log call (%d)", (int)g_lock_in_real, (int)g_log_under_lock);
  q15t::SetSensorWdHookForTest(nullptr);
}

static void TestConcurrency() {
  Setup();
  g_probe_log = g_probe_real = false;  // other threads hold the lock legitimately
  const int kThreads = 4, kIters = 300, kPerThread = 7;
  std::atomic<bool> go{false};
  std::atomic<int> pings{0}, en_calls{0}, dis_calls{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t)
    ts.emplace_back([&, t] {
      while (!go) std::this_thread::yield();
      const int c = t % 3;
      for (int k = 0; k < kIters; ++k) {
        const int i = t * kPerThread + k % kPerThread;
        En(c, i, kMark);
        ++en_calls;
        if (k % 3 == 0) {
          Ev(kTriggered, 0x1000 + t, 0x2000 + t);
          ++pings;
        }
        if (k % 2 == 1) {  // the others stay enabled: the ticker turns them off and the pings bring them back
          Dis(c, i, kMark);
          ++dis_calls;
        }
      }
    });
  std::thread ticker([&] {
    while (!go) std::this_thread::yield();
    for (int k = 0; k < 400 && !stop; ++k) {
      g_ms += 700;
      q15t::SensorWdTick(0);
    }
  });
  go = true;
  for (auto& t : ts) t.join();
  stop = true;
  ticker.join();
  const q15t::SensorWdStats s = St();
  CHECK(g_marked_en == en_calls.load() && g_marked_dis == dis_calls.load(), "real calls: en %d of %d, dis %d of %d", (int)g_marked_en,
        en_calls.load(), (int)g_marked_dis, dis_calls.load());
  CHECK(s.enables == (uint32_t)en_calls.load() && s.disables == (uint32_t)dis_calls.load(), "counted: en %u dis %u", s.enables, s.disables);
  CHECK(s.pings == (uint32_t)pings.load(), "pings %u of %d", s.pings, pings.load());
  CHECK(s.table_full == 0, "table full %u", s.table_full);
  int tick_calls = 0;
  for (const auto& c : g_calls) tick_calls += (c.which == kEn || c.which == kDis) && c.a2 == 0 && c.a7 == 0;
  CHECK((uint32_t)tick_calls == s.wd_off + s.wd_back, "tick calls %d vs counters %u + %u", tick_calls, s.wd_off, s.wd_back);
  CHECK(!q15t::SensorWdLockHeldForTest(), "lock held at the end");
}

static void TestLogRecord() {
  // one real-looking run, printed for the report: install, a stuck sphere, its actor's next ping
  Fresh(kTramp);
  InstallT();
  g_probe_log = false;
  Ev(kTriggered, 0xA1, 0xB1);
  Advance(120);
  En(0, 0);
  Advance(4000);
  Ev(kFinished, 0xA1, 0xB1);
  Advance(2100);
  Tick();
  Advance(30000);
  Ev(kTriggered, 0xA1, 0xB1);
  Tick();
  std::printf("--- log record of one run (level text) ---\n");
  for (const auto& l : g_lines) std::printf("L%u %s\n", l.level, l.text.c_str());
  std::printf("--- end of log record ---\n");
  CHECK(CountLines("sensorwd on", 1) == 1 && CountLines("sensorwd off: sphere", 1) == 1 && CountLines("sensorwd back on", 1) == 1, "run record");
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  TestConstants();
  TestInstallPinnedImage();
  TestRefusals();
  TestInstallFailures();
  TestPassThrough();
  TestNonSensorModels();
  TestEnableTimedAndOff();
  TestDisableClears();
  TestForcedBackOn();
  TestPairingWindow();
  TestPingQueue();
  TestStaleAndSweep();
  TestTableFull();
  TestLogBudgets();
  TestLockDiscipline();
  TestConcurrency();
  TestLogRecord();
  if (g_fail == 0) std::printf("sensorwd_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
