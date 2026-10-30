// Host test of tablestep (src/quest/plugins/q15t/src/tablestep.cpp, issue #1552): the real install path against a fake game-library
// image. The pin word at ELF 0x1907130 is the stock `mov w8,#0x20`; the slot holds base + 0x190707c; the fake CreateInstance
// builds a 0xb0-byte object whose table step at +0x98 is the stock 0x20, as the stock store leaves it.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "pbhud_image.h"
#include "plugin_log.h"
#include "tablestep.h"

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
  if (std::getenv("PBHUD_LOG")) std::printf("LOG level=%u %s\n", level, t.c_str());  // PBHUD_LOG=1: every record as it is emitted
  g_lines.push_back({level, t});
}
static int Count(const char* needle, int level = -1) {
  int n = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++n;
  return n;
}

static const uintptr_t kCreate = 0x190707c;
static const uintptr_t kPin = 0x1907130;
static const uint32_t kPinWord = 0x52800408;  // mov w8,#0x20
static const uintptr_t kSlots[1] = {0x365ddd0};
static const uintptr_t kObjStep = 0x98;
static const uintptr_t kObjSize = 0xb0;

// The stock CreateInstance: a fresh object with the stock step 0x20 at +0x98. The first argument is unused, as in the stock.
static unsigned g_stock_calls = 0;
static uint64_t g_stock_step = 0x20;  // what the stock store writes (a test may change it to mimic a different build)
static void* FakeCreate(void* unused, void* init) {
  (void)unused;
  (void)init;
  ++g_stock_calls;
  auto* obj = static_cast<uint8_t*>(std::calloc(1, kObjSize));
  std::memcpy(obj + kObjStep, &g_stock_step, sizeof g_stock_step);
  return obj;
}
using CreateFn = void* (*)(void*, void*);

static void Fresh(pbhud::Image& img) {
  g_lines.clear();
  g_stock_calls = 0;
  g_stock_step = 0x20;
  q15t::ResetTableStepForTest();
  img.Real(kCreate, reinterpret_cast<void*>(&FakeCreate));
  img.Word32(kPin) = kPinWord;
  for (uintptr_t s : kSlots) img.Slot(s, kCreate);
}

static uint64_t StepOf(void* obj) {
  uint64_t v = 0;
  std::memcpy(&v, static_cast<uint8_t*>(obj) + kObjStep, sizeof v);
  return v;
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);

  // 1. the slot is swapped; a call through it gives the object step 1 and the stock call count moves
  {
    pbhud::Image img;
    Fresh(img);
    q15t::InstallResult ir = q15t::InstallTableStep(img.base);
    CHECK(ir.status == q15t::SwapStatus::kOk, "install status %s", q15t::StatusName(ir.status));
    for (uintptr_t s : kSlots) CHECK(img.Get(s) != img.base + kCreate, "slot 0x%llx not swapped", (unsigned long long)s);
    CHECK(Count("tablestep: slot swapped", 1) == 1, "one INFO slot line expected");
    CHECK(Count("tablestep: on", 1) == 1 && Count("slots=1") == 1, "no summary line with slots=1");
    for (int i = 0; i < 1; ++i) {
      CreateFn f = reinterpret_cast<CreateFn>(img.Get(kSlots[i]));
      void* obj = f(nullptr, nullptr);
      CHECK(obj != nullptr, "slot %d returned null", i);
      CHECK(StepOf(obj) == 1, "slot %d: step %llu, want 1", i, (unsigned long long)StepOf(obj));
      std::free(obj);
    }
    CHECK(g_stock_calls == 1, "stock calls %u, want 1", g_stock_calls);
    auto st = q15t::GetTableStepStats();
    CHECK(st.rewritten == 1 && st.unexpected == 0, "stats %u/%u, want 1/0", st.rewritten, st.unexpected);
    CHECK(Count("tablestep: first shader-set table step set", 1) == 1, "first-step INFO logged more or less than once");
  }

  // 2. the stock value is not 0x20 (a different build): left alone, counted, logged once
  {
    pbhud::Image img;
    Fresh(img);
    q15t::InstallTableStep(img.base);
    g_stock_step = 0x40;
    CreateFn f = reinterpret_cast<CreateFn>(img.Get(kSlots[0]));
    void* obj = f(nullptr, nullptr);
    CHECK(StepOf(obj) == 0x40, "step changed though it was not the stock value: %llu", (unsigned long long)StepOf(obj));
    std::free(obj);
    obj = f(nullptr, nullptr);
    std::free(obj);
    auto st = q15t::GetTableStepStats();
    CHECK(st.rewritten == 0 && st.unexpected == 2, "stats %u/%u, want 0/2", st.rewritten, st.unexpected);
    CHECK(Count("table step is not the stock value", 2) == 1, "the not-stock WARNING logged other than once");
  }

  // 3. the code pin does not read back: the module is refused whole; no slot is touched
  {
    pbhud::Image img;
    Fresh(img);
    img.Word32(kPin) = 0x52800028;  // mov w8,#1 (the replacement, not the stock word)
    q15t::InstallResult ir = q15t::InstallTableStep(img.base);
    CHECK(ir.status == q15t::SwapStatus::kUnexpectedOriginal && std::strcmp(ir.reason, "word_pin_mismatch") == 0,
          "pin mismatch not refused: %s / %s", q15t::StatusName(ir.status), ir.reason);
    for (uintptr_t s : kSlots) CHECK(img.Get(s) == img.base + kCreate, "slot 0x%llx touched by a refused module", (unsigned long long)s);
    CHECK(Count("code word pin mismatch", 3) == 1, "no ERROR pin line");
  }

  // 4. the slot holds something else: refused, the module is OFF and the word is left as it was
  {
    pbhud::Image img;
    Fresh(img);
    img.SetSlot(kSlots[0], img.base + 0x1234);  // not the stock address
    q15t::InstallResult ir = q15t::InstallTableStep(img.base);
    CHECK(ir.status == q15t::SwapStatus::kUnexpectedOriginal, "a slot that is not the stock address: %s", q15t::StatusName(ir.status));
    CHECK(img.Get(kSlots[0]) == img.base + 0x1234, "refused slot was written");
    CHECK(Count("slot refused, its calls stay stock", 2) == 1, "no WARNING for the refused slot");
    CHECK(Count("tablestep: on", 1) == 0, "an 'on' line though no slot was installed");
  }

  // 5. every slot refused (the one slot), an unaligned word: OFF, the reason is the first refusal's status
  {
    pbhud::Image img;
    Fresh(img);
    for (uintptr_t s : kSlots) img.SetSlot(s, img.base + 0x1234);
    q15t::InstallResult ir = q15t::InstallTableStep(img.base);
    CHECK(ir.status == q15t::SwapStatus::kUnexpectedOriginal, "all refused: status %s", q15t::StatusName(ir.status));
    CHECK(Count("tablestep: OFF; no slot installed", 3) == 1, "no OFF ERROR line");
  }

  // 6. no game base
  {
    q15t::InstallResult ir = q15t::InstallTableStep(0);
    CHECK(std::strcmp(ir.reason, "no_base") == 0, "no_base not reported");
    CHECK(Count("tablestep not installed: no game base", 3) == 1, "no ERROR for the missing base");
  }

  if (g_fail == 0) std::printf("tablestep_test: OK\n");
  return g_fail == 0 ? 0 : 1;
}
