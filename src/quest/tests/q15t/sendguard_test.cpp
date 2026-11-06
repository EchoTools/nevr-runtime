// Host test of sendguard (src/quest/plugins/q15t/src/sendguard.cpp): the real installer on a libr15 image built in memory, and the
// wrapper called the way the game calls it, through the data word. #1603 (the module shipped with no test), #2179.
//
// The image is one anonymous mapping the size of the highest word the module reads. It holds the 13 code words the module pins (spelled
// here, not taken from the source, so a wrong word there fails here), the slot at ELF 0x36f7928 holding base + 0x2725a80 (the
// JUMP_SLOT of CSendEventNode::Enter), and the first slot, ELF 0x356a6b0, as the stock library has it: in a read-only executable page,
// holding the eight bytes of an unwind record (`d0b31bff dc010000`: the FDE's pc_begin and range), not a pointer.
//
// Enter itself. The module calls the word it swapped, which holds base + 0x2725a80. Those bytes are pinned AArch64 words; the host
// runs x86-64, so the first word `d101c3ff` is the bytes `ff c3 01 d1` (inc ebx; add ecx, edx), the next eight are free, and this
// test puts `dec ebx; jmp [rip+disp]` there, to the model of Enter in this file. The jump's pointer sits after the pinned words, in the
// same page. The `inc ebx` and `dec ebx` pair leaves the caller's callee-saved register as it was.
//
// The lookup. The module reads the page that holds the game's CncaGameSpace::GameSpaceMT stub; it is only the address of that stub, so
// after the install the test replaces the module's lookup with a model through SetGameSpaceLookupForTest.
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "module_switch.h"
#include "nevr/plugin_interface.h"
#include "plugin_log.h"
#include "sendguard.h"

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
    if (l.text.find(needle) != std::string::npos && (level < 0 || static_cast<int>(l.level) == level)) ++c;
  return c;
}
static int CountLevel(int level) {
  int c = 0;
  for (const auto& l : g_lines)
    if (static_cast<int>(l.level) == level) ++c;
  return c;
}

// ---- the layout, as this test spells it ----
constexpr size_t kImageSize = 0x3800000;
constexpr uintptr_t kEnter = 0x2725a80;
constexpr uintptr_t kStub = 0xef5650;
constexpr uintptr_t kSlotFde = 0x356a6b0;   // inside .eh_frame in the stock library
constexpr uintptr_t kSlotJump = 0x36f7928;  // the JUMP_SLOT of Enter
constexpr uintptr_t kPointerAt = kEnter + 0x200;
struct Pin {
  uintptr_t va;
  uint32_t word;
};
constexpr Pin kPins[] = {
    {0x2725a80, 0xd101c3ff}, {0x2725a90, 0xf9400008}, {0x2725a98, 0xf9400021}, {0x2725aa0, 0xf9400109}, {0x2725aa8, 0xf9401929},
    {0x2725aac, 0xf9400129}, {0x2725ab0, 0xf9404133}, {0x2725abc, 0x979f3ee5}, {0x2725b78, 0xf9402e69}, {0xef5650, 0xb0013ef0},
    {0xef5654, 0xf9410a11},  {0xef5658, 0x91084210},  {0xef565c, 0xd61f0220},
};
constexpr size_t kPinCount = sizeof kPins / sizeof kPins[0];
constexpr unsigned char kFdeBytes[8] = {0xd0, 0xb3, 0x1b, 0xff, 0xdc, 0x01, 0x00, 0x00};

// ---- the models ----
static int g_enter_calls = 0;
static void* g_enter_ctx = nullptr;
static void* g_enter_data = nullptr;
extern "C" __attribute__((noinline)) void ModelEnter(void* ctx, void* data) {
  ++g_enter_calls;
  g_enter_ctx = ctx;
  g_enter_data = data;
}

static int g_lookup_calls = 0;
static void* g_lookup_space = nullptr;
static uint64_t g_lookup_level = 0;
static void* g_lookup_answer = nullptr;
static void* ModelLookup(void* space, uint64_t level) {
  ++g_lookup_calls;
  g_lookup_space = space;
  g_lookup_level = level;
  return g_lookup_answer;
}

// The node's execution context: ctx -> [0] -> [0] -> [+0x30] -> [0] -> [+0x80] = the current game space.
static void* const kSpace = reinterpret_cast<void*>(0x5150);
static uintptr_t g_a[2], g_b[8], g_c[2], g_d[0x20];
static uintptr_t g_ctx[2];
static void BuildContext() {
  std::memset(g_a, 0, sizeof g_a);
  std::memset(g_b, 0, sizeof g_b);
  std::memset(g_c, 0, sizeof g_c);
  std::memset(g_d, 0, sizeof g_d);
  std::memset(g_ctx, 0, sizeof g_ctx);
  g_ctx[0] = reinterpret_cast<uintptr_t>(g_a);
  g_a[0] = reinterpret_cast<uintptr_t>(g_b);
  g_b[0x30 / 8] = reinterpret_cast<uintptr_t>(g_c);
  g_c[0] = reinterpret_cast<uintptr_t>(g_d);
  g_d[0x80 / 8] = reinterpret_cast<uintptr_t>(kSpace);
}

struct Image {
  unsigned char* mem = nullptr;
  uintptr_t base = 0;
};
static void Put32(unsigned char* m, uintptr_t va, uint32_t w) { std::memcpy(m + va, &w, sizeof w); }
static uintptr_t* Word(const Image& im, uintptr_t va) { return reinterpret_cast<uintptr_t*>(im.mem + va); }

// flipped >= 0: that pin's word differs from the stock one.
static Image Build(int flipped = -1) {
  Image im;
  void* p = mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) std::abort();
  im.mem = static_cast<unsigned char*>(p);
  im.base = reinterpret_cast<uintptr_t>(p);
  for (size_t i = 0; i < kPinCount; ++i) Put32(im.mem, kPins[i].va, kPins[i].word ^ (static_cast<int>(i) == flipped ? 0x20u : 0u));
  // Enter's body on the host: dec ebx; jmp qword ptr [rip + disp32] to the pointer after the pinned words.
  unsigned char* e = im.mem + kEnter;
  e[4] = 0xff;
  e[5] = 0xcb;
  e[6] = 0xff;
  e[7] = 0x25;
  const int32_t disp = static_cast<int32_t>(kPointerAt - (kEnter + 12));
  std::memcpy(e + 8, &disp, sizeof disp);
  const uintptr_t target = reinterpret_cast<uintptr_t>(&ModelEnter);
  std::memcpy(im.mem + kPointerAt, &target, sizeof target);
  *Word(im, kSlotJump) = im.base + kEnter;
  // Enter's page is the one executable page besides the unwind record's; the slot's page is data, as in the stock library.
  if (mprotect(im.mem + (kEnter & ~uintptr_t{0xfff}), 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) std::abort();
  std::memcpy(im.mem + kSlotFde, kFdeBytes, sizeof kFdeBytes);
  if (mprotect(im.mem + (kSlotFde & ~uintptr_t{0xfff}), 4096, PROT_READ | PROT_EXEC) != 0) std::abort();
  return im;
}
static void Drop(Image& im) { munmap(im.mem, kImageSize); }

using EnterFn = void (*)(void*, void*);
static void Reset() {
  q15t::ResetSendGuardForTest();
  g_lines.clear();
  g_enter_calls = g_lookup_calls = 0;
  g_enter_ctx = g_enter_data = g_lookup_space = nullptr;
  g_lookup_level = 0;
  g_lookup_answer = nullptr;
  BuildContext();
}
static void CallThrough(const Image& im, void* ctx, void* data) { reinterpret_cast<EnterFn>(*Word(im, kSlotJump))(ctx, data); }

int main() {
  NevrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  char tmpl[] = "/tmp/q15t-sendguard-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  const std::string dir = tmpl;
  q15t::SetModuleSwitchDirForTest(dir.c_str());

  // 1. The install on the stock layout: the pinned words hold, the JUMP_SLOT is swapped to the wrapper, and the first slot is refused
  // and left as it is.
  {
    Reset();
    Image im = Build();
    const q15t::InstallResult r = q15t::InstallSendGuard(im.base);
    CHECK(r.status == q15t::SwapStatus::kOk, "install status %s", q15t::StatusName(r.status));
    CHECK(*Word(im, kSlotJump) != im.base + kEnter, "the JUMP_SLOT still holds Enter");
    CHECK(std::memcmp(im.mem + kSlotFde, kFdeBytes, sizeof kFdeBytes) == 0, "the unwind record's bytes changed");
    CHECK(CountLines("sendguard: slot swapped", 2) == 0 && CountLines("sendguard: slot swapped", 1) == 1, "swapped lines");
    CHECK(CountLines("sendguard: slot refused", 2) == 1 && CountLines("slot_elf_vaddr=0x356a6b0") == 1,
          "the first slot is refused at Warning, naming its address (%d)", CountLines("sendguard: slot refused", 2));
    CHECK(CountLines("status=executable_page") == 1, "the refusal does not name the executable page");
    CHECK(CountLines("sendguard: on", 1) == 1 && CountLines("slots=1 of=2") == 1, "the aggregate line counts one slot of two");
    Drop(im);
  }

  // 2. A named level that is not loaded: the lookup answers null, the send is dropped, the stock function is not called, one Info
  // line names the keys; the lookup was asked the space the context chain leads to and the level key.
  {
    Reset();
    Image im = Build();
    CHECK(q15t::InstallSendGuard(im.base).status == q15t::SwapStatus::kOk, "install");
    q15t::SetGameSpaceLookupForTest(&ModelLookup);
    g_lines.clear();
    uint64_t data[3] = {0x77, 0x88, 0x99};
    g_lookup_answer = nullptr;
    CallThrough(im, g_ctx, data);
    CHECK(g_enter_calls == 0, "the stock function ran for a level that is not loaded");
    CHECK(g_lookup_calls == 1 && g_lookup_space == kSpace && g_lookup_level == 0x77, "lookup asked %p / 0x%llx", g_lookup_space,
          static_cast<unsigned long long>(g_lookup_level));
    CHECK(q15t::GetSendGuardStats().dropped == 1 && q15t::GetSendGuardStats().passed == 0, "stats");
    CHECK(CountLines("sendguard: send dropped", 1) == 1 && CountLines("level_key=0x77 event_key=0x88 actor_key=0x99") == 1, "drop line");
    Drop(im);
  }

  // 3. A level that is loaded: the stock function runs with the node's own two arguments.
  {
    Reset();
    Image im = Build();
    CHECK(q15t::InstallSendGuard(im.base).status == q15t::SwapStatus::kOk, "install");
    q15t::SetGameSpaceLookupForTest(&ModelLookup);
    uint64_t data[3] = {0x77, 0x88, 0x99};
    g_lookup_answer = reinterpret_cast<void*>(0xbeef);
    CallThrough(im, g_ctx, data);
    CHECK(g_enter_calls == 1 && g_enter_ctx == g_ctx && g_enter_data == data, "the stock function's arguments changed");
    CHECK(q15t::GetSendGuardStats().dropped == 0 && q15t::GetSendGuardStats().passed == 1, "stats");
    Drop(im);
  }

  // 4. No level named (-1: send within the current space): no lookup, the stock function runs.
  {
    Reset();
    Image im = Build();
    CHECK(q15t::InstallSendGuard(im.base).status == q15t::SwapStatus::kOk, "install");
    q15t::SetGameSpaceLookupForTest(&ModelLookup);
    uint64_t data[3] = {~0ull, 1, 2};
    CallThrough(im, g_ctx, data);
    CHECK(g_lookup_calls == 0 && g_enter_calls == 1, "lookup %d enter %d", g_lookup_calls, g_enter_calls);
    Drop(im);
  }

  // 5. A null link anywhere on the chain to the current space leaves the stock function to do what it does; so does a null context
  // or a null node. The lookup is not asked.
  {
    Reset();
    Image im = Build();
    CHECK(q15t::InstallSendGuard(im.base).status == q15t::SwapStatus::kOk, "install");
    q15t::SetGameSpaceLookupForTest(&ModelLookup);
    uint64_t data[3] = {0x77, 1, 2};
    g_c[0] = 0;
    CallThrough(im, g_ctx, data);
    CHECK(g_enter_calls == 1 && g_lookup_calls == 0, "null link: enter %d lookup %d", g_enter_calls, g_lookup_calls);
    BuildContext();
    CallThrough(im, nullptr, data);
    CHECK(g_enter_calls == 2 && g_lookup_calls == 0, "null context");
    CallThrough(im, g_ctx, nullptr);
    CHECK(g_enter_calls == 3 && g_lookup_calls == 0, "null node");
    Drop(im);
  }

  // 6. Only the first eight drops are written; every drop is counted.
  {
    Reset();
    Image im = Build();
    CHECK(q15t::InstallSendGuard(im.base).status == q15t::SwapStatus::kOk, "install");
    q15t::SetGameSpaceLookupForTest(&ModelLookup);
    g_lines.clear();
    uint64_t data[3] = {0x77, 1, 2};
    for (int i = 0; i < 12; ++i) CallThrough(im, g_ctx, data);
    CHECK(q15t::GetSendGuardStats().dropped == 12, "dropped %u", q15t::GetSendGuardStats().dropped);
    CHECK(CountLines("sendguard: send dropped") == 8, "drop lines %d", CountLines("sendguard: send dropped"));
    Drop(im);
  }

  // 7. A pinned word that differs refuses the module before any slot is touched, with one Error line.
  for (size_t i = 0; i < kPinCount; ++i) {
    Reset();
    Image im = Build(static_cast<int>(i));
    const q15t::InstallResult r = q15t::InstallSendGuard(im.base);
    CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal && std::string(r.reason) == "word_pin_mismatch", "pin %zu: %s / %s", i,
          q15t::StatusName(r.status), r.reason);
    CHECK(*Word(im, kSlotJump) == im.base + kEnter, "pin %zu: the slot was swapped", i);
    CHECK(CountLevel(3) == 1 && CountLines("code word pin mismatch", 3) == 1, "pin %zu: error lines", i);
    Drop(im);
  }

  // 8. The slot holds something else than Enter (another runtime's word): refused alone; with no slot installed the module is off, at Error.
  {
    Reset();
    Image im = Build();
    *Word(im, kSlotJump) = im.base + 0x1234;
    const q15t::InstallResult r = q15t::InstallSendGuard(im.base);
    CHECK(r.status != q15t::SwapStatus::kOk, "install over a foreign word: %s", q15t::StatusName(r.status));
    CHECK(*Word(im, kSlotJump) == im.base + 0x1234, "a foreign word was replaced");
    CHECK(CountLines("sendguard: OFF; no slot installed", 3) == 1, "off line");
    Drop(im);
  }

  // 9. No game base: refused with an Error line; nothing is touched.
  {
    Reset();
    const q15t::InstallResult r = q15t::InstallSendGuard(0);
    CHECK(r.status != q15t::SwapStatus::kOk && std::string(r.reason) == "no_base", "no base: %s", r.reason);
    CHECK(CountLines("sendguard not installed: no game base", 3) == 1, "no-base line");
  }

  // 10. The off file: nothing is changed; the result reports what happened.
  {
    Reset();
    Image im = Build();
    const std::string off = dir + "/sendguard.off";
    FILE* f = std::fopen(off.c_str(), "w");
    if (f != nullptr) std::fclose(f);
    const q15t::InstallResult r = q15t::InstallSendGuard(im.base);
    CHECK(*Word(im, kSlotJump) == im.base + kEnter, "the off file did not stop the swap");
    CHECK(std::string(r.reason) == "sendguard_off", "off reason %s", r.reason);
    CHECK(CountLines("sendguard off: sendguard.off present", 1) == 1, "off line");
    CHECK(CountLines("sendguard: slot") == 0, "a slot was tried");
    unlink(off.c_str());
    Drop(im);
  }

  rmdir(dir.c_str());
  if (g_fail != 0) {
    std::printf("sendguard_test: %d FAILED\n", g_fail);
    return 1;
  }
  std::printf("sendguard_test: OK\n");
  return 0;
}
