// Host test of fxtrail (src/quest/plugins/q15t/src/fxtrail.cpp): the wrapper installed by the real installer on a fake JUMP_SLOT page, called through
// the slot, against a fake CParticleEffectCS laid out as EffectTime reads it. Real SafeRead, real mprotect, real files.
#include <sys/mman.h>
#include <unistd.h>

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "fxtrail.h"
#include "pin_check.h"
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
static void FakeLog(const char* id, uint32_t level, const char* msg, const NevrLogField* f, uint32_t n) {
  std::string t = std::string("plugin=") + id + " " + msg;
  for (uint32_t i = 0; i < n; ++i) t += std::string(" ") + f[i].key + "=" + f[i].value;
  g_lines.push_back({level, t});
}
static int CountLines(const char* needle, int level = -1) {
  int c = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++c;
  return c;
}

// ---- the fake effect component system ----
alignas(16) static uintptr_t g_cs[0x120 / 8];  // +0xc8 index table, +0xf8 data
alignas(16) static uint16_t g_index[8];
struct Data {
  uint8_t b[0x18];
};
alignas(16) static Data g_data[6];

static int g_real_calls = 0;
static const void* g_real_cs = nullptr;
static uint64_t g_real_h = 0;
extern "C" __attribute__((noinline)) float RealEffectTime(const void* cs, uint64_t h) {
  ++g_real_calls;
  g_real_cs = cs;
  g_real_h = h;
  return 0.5f;
}
extern "C" __attribute__((noinline)) float OtherFn(const void*, uint64_t) { return 9.0f; }
using Fn = float (*)(const void*, uint64_t);

static void SetInst(int data_slot, uintptr_t inst) { std::memcpy(g_data[data_slot].b + 0x10, &inst, sizeof inst); }

// handle index 2: the u16 at byte offset 4*2 of the index table maps to data[3]; every other data entry gets `others`, data[3] gets `mine`
static void World(uintptr_t mine, uintptr_t others) {
  std::memset(g_cs, 0, sizeof g_cs);
  std::memset(g_index, 0, sizeof g_index);
  std::memset(g_data, 0, sizeof g_data);
  g_cs[0xc8 / 8] = reinterpret_cast<uintptr_t>(g_index);
  g_cs[0xf8 / 8] = reinterpret_cast<uintptr_t>(g_data);
  g_index[4] = 3;  // u16 at byte 4 * 2
  for (int i = 0; i < 6; ++i) SetInst(i, others);
  SetInst(3, mine);
  g_real_calls = 0;
}

static const uint64_t kHandle = 2ull | (7ull << 16) | (0xabcdull << 32);  // index 2, a generation and junk above bit 16

struct Slot {
  uintptr_t* w;
  uintptr_t base;
};
static Slot MakePage(uintptr_t word) {
  void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) std::abort();
  auto* w = static_cast<uintptr_t*>(p);
  w[4] = word;  // slot at 0x20
  mprotect(p, 4096, PROT_READ);
  return {w, reinterpret_cast<uintptr_t>(p)};
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  char tmpl[] = "/tmp/q15t-fxtrail-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  const std::string dir = tmpl;
  q15t::SetFxTrailDirForTest(dir.c_str());

  // the pinned addresses (R0_readelf_r.txt line 126983; disassembly 0x17647e4 - 0x100000)
  CHECK(q15t::kFxTrailSlot == 0x36e7b98 && q15t::kFxTrailReal == 0x16647e4, "pinned addresses changed");

  Slot s = MakePage(reinterpret_cast<uintptr_t>(&RealEffectTime));
  const auto r = q15t::InstallFxTrailAt(s.base, 0x20, reinterpret_cast<uintptr_t>(&RealEffectTime) - s.base);
  CHECK(r.status == q15t::SwapStatus::kOk, "install: %s", q15t::StatusName(r.status));
  CHECK(s.w[4] != reinterpret_cast<uintptr_t>(&RealEffectTime), "slot not replaced");
  CHECK(CountLines("fxtrail on", 1) == 1, "no INFO install line");
  const Fn hook = reinterpret_cast<Fn>(s.w[4]);

  // 1. an effect with no instance: the real function runs, the answer is FLT_MAX, the call is counted
  World(0, 0x1234);
  float t = hook(g_cs, kHandle);
  CHECK(t == FLT_MAX, "null instance: %g", t);
  CHECK(g_real_calls == 1 && g_real_cs == g_cs && g_real_h == kHandle, "real not called once with the same arguments");
  CHECK(q15t::FxTrailEnded() == 1, "ended %llu", (unsigned long long)q15t::FxTrailEnded());

  // 2. an effect with an instance: the engine's value, untouched (every other data entry is empty: a wrong stride is seen)
  World(0x1234, 0);
  t = hook(g_cs, kHandle);
  CHECK(t == 0.5f, "instance present: %g", t);
  CHECK(g_real_calls == 1 && q15t::FxTrailEnded() == 1, "counted an effect that exists");

  // 3. counters only on the call path: 5 ended trails log at 1, 2 and 4 only
  q15t::ResetFxTrailForTest();
  Slot s2 = MakePage(reinterpret_cast<uintptr_t>(&RealEffectTime));
  g_lines.clear();
  q15t::InstallFxTrailAt(s2.base, 0x20, reinterpret_cast<uintptr_t>(&RealEffectTime) - s2.base);
  const Fn hook2 = reinterpret_cast<Fn>(s2.w[4]);
  World(0, 0x1234);
  for (int i = 0; i < 5; ++i) hook2(g_cs, kHandle);
  CHECK(q15t::FxTrailEnded() == 5, "ended %llu", (unsigned long long)q15t::FxTrailEnded());
  CHECK(CountLines("fxtrail null-effect trail ended") == 3, "log lines %d", CountLines("fxtrail null-effect trail ended"));
  CHECK(CountLines("ended=1") == 1 && CountLines("ended=2") == 1 && CountLines("ended=4") == 1 && CountLines("ended=3") == 0,
        "log at powers of two only");

  // 4. any read that fails: the real value, never FLT_MAX
  q15t::ResetFxTrailForTest();
  Slot s3 = MakePage(reinterpret_cast<uintptr_t>(&RealEffectTime));
  q15t::InstallFxTrailAt(s3.base, 0x20, reinterpret_cast<uintptr_t>(&RealEffectTime) - s3.base);
  const Fn hook3 = reinterpret_cast<Fn>(s3.w[4]);
  World(0, 0);
  t = hook3(reinterpret_cast<const void*>(0x20), kHandle);  // cs itself unreadable
  CHECK(t == 0.5f, "unreadable cs: %g", t);
  t = hook3(nullptr, kHandle);
  CHECK(t == 0.5f, "null cs: %g", t);
  World(0, 0);
  g_cs[0xc8 / 8] = 0x30;  // index table unreadable
  t = hook3(g_cs, kHandle);
  CHECK(t == 0.5f, "unreadable index table: %g", t);
  World(0, 0);
  g_cs[0xf8 / 8] = 0x30;  // data array unreadable
  t = hook3(g_cs, kHandle);
  CHECK(t == 0.5f, "unreadable data array: %g", t);
  CHECK(q15t::FxTrailEnded() == 0, "a failed read was counted as an ended trail");

  // 5. a slot that does not hold the pinned function is refused and left alone, with an ERROR line
  g_lines.clear();
  Slot bad = MakePage(0x4242420);
  const auto rb = q15t::InstallFxTrailAt(bad.base, 0x20, reinterpret_cast<uintptr_t>(&RealEffectTime) - bad.base);
  CHECK(rb.status == q15t::SwapStatus::kUnexpectedOriginal && bad.w[4] == 0x4242420, "foreign word: %s", q15t::StatusName(rb.status));
  {
    char named[128];
    std::snprintf(named, sizeof named, "pin=0x20 found=0x4242420 expected=0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&RealEffectTime)));
    CHECK(CountLines("fxtrail refused", 3) == 1 && CountLines(named) == 1, "refusal not loud / does not name pin, found, expected");
  }
  // a word that points at real executable code, but not at the pinned function, is refused too (no by-name acceptance)
  Slot other = MakePage(reinterpret_cast<uintptr_t>(&OtherFn));
  const auto rc = q15t::InstallFxTrailAt(other.base, 0x20, reinterpret_cast<uintptr_t>(&RealEffectTime) - other.base);
  CHECK(rc.status == q15t::SwapStatus::kUnexpectedOriginal && other.w[4] == reinterpret_cast<uintptr_t>(&OtherFn),
        "other function's word accepted: %s", q15t::StatusName(rc.status));

  // 6. fxtrail.off in the data directory: nothing swapped, one INFO line
  g_lines.clear();
  FILE* m = std::fopen((dir + "/fxtrail.off").c_str(), "w");
  std::fclose(m);
  Slot off = MakePage(reinterpret_cast<uintptr_t>(&RealEffectTime));
  const auto ro = q15t::InstallFxTrailAt(off.base, 0x20, reinterpret_cast<uintptr_t>(&RealEffectTime) - off.base);
  CHECK(ro.status != q15t::SwapStatus::kOk && std::strcmp(ro.reason, "off_file") == 0, "off file ignored: %s", q15t::StatusName(ro.status));
  CHECK(off.w[4] == reinterpret_cast<uintptr_t>(&RealEffectTime), "slot swapped although fxtrail.off exists");
  CHECK(CountLines("fxtrail off", 1) == 1, "no INFO off line");
  unlink((dir + "/fxtrail.off").c_str());

  rmdir(dir.c_str());
  // ---- code-word pins: every word read, a changed or unreadable one refused loudly, no file hash anywhere ----
  {
    auto count = [](const char* needle, int level) { return CountLines(needle, level); };
    auto clear = []() { g_lines.clear(); };
    g_fail += pincheck::Run("fxtrail", q15t::kFxTrailWordPins, q15t::kFxTrailWordPinCount, 11, 0xb3d4bde0243f2605ull,
                            [](std::uintptr_t b) { return q15t::FxTrailWordPinsMatch(b); }, count, clear);
    // the real installer refuses on a changed word, before anything else (reason word_pin_mismatch, one ERROR line, nothing swapped)
    pincheck::Text t = pincheck::Make(q15t::kFxTrailWordPins, q15t::kFxTrailWordPinCount);
    *t.word(q15t::kFxTrailWordPins[0].va) ^= 1u;
    g_lines.clear();
    const auto pr = q15t::InstallFxTrail(t.base());
    CHECK(std::strcmp(pr.reason, "word_pin_mismatch") == 0 && CountLines("code word pin mismatch", 3) == 1, "installer ignored a changed word");
    CHECK(CountLines("fxtrail on", -1) == 0, "installer went on after a changed word");
    pincheck::Free(&t);
  }

  if (g_fail == 0) std::printf("fxtrail_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
