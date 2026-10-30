// Host test of the `levelname` module (src/quest/plugins/q15t/src/levelname.cpp): a fake game library image (a sparse anonymous mapping whose
// offsets are the pinned build's ELF vaddrs, so the production slot addresses are used as they are), four fake vtable
// words, fake real/plain callees. Real SafeRead, real got_swap, real /proc/self/maps.
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "got_swap.h"
#include "levelname.h"
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
static bool HasLine(const char* needle, int level = -1) {
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) return true;
  return false;
}

// the pinned build's ELF vaddrs (docs/quest/q15t/modules/levelname.md)
static const uintptr_t kFn = 0x1202ecc, kPlainStub = 0xf133d0;
static const uintptr_t kSlot[4] = {0x362ef90, 0x3630e58, 0x3631278, 0x36316f8};
static const uint64_t kGlobalMp = 0x3F9915D3001DC28Eull;
static const size_t kImage = 0x3800000;
static const size_t kFlag = 0x7ad8;

static uintptr_t g_base = 0;
static size_t g_page = 0;

// fake callees: the real function appends "_lowspec" (modelled as a xor), the plain one returns the name
static int g_real_calls = 0, g_plain_calls = 0;
static void *g_real_game = nullptr, *g_plain_game = nullptr;
static uint64_t g_real_name = 0, g_plain_name = 0;
static uint64_t FakeReal(void* game, uint64_t name) {
  ++g_real_calls;
  g_real_game = game;
  g_real_name = name;
  return name ^ 0x00000000000a5a5aull;
}
static uint64_t FakePlain(void* game, uint64_t name) {
  ++g_plain_calls;
  g_plain_game = game;
  g_plain_name = name;
  return name;
}

alignas(16) static unsigned char g_game[0x8000];
static void SetFlags(uint32_t v, size_t off = kFlag) {
  std::memset(g_game, 0, sizeof g_game);
  std::memcpy(g_game + off, &v, 4);
}

static uintptr_t& Word(uintptr_t vaddr) { return *reinterpret_cast<uintptr_t*>(g_base + vaddr); }

static void Prepare() {
  g_lines.clear();
  q15t::ResetLevelNameForTest();
  for (uintptr_t s : kSlot) {
    Word(s) = g_base + kFn;
    Word(s - 8) = 0x1111;  // neighbours, never touched
    Word(s + 8) = 0x2222;
  }
  g_real_calls = g_plain_calls = 0;
  g_real_game = g_plain_game = nullptr;
  g_real_name = g_plain_name = 0;
  SetFlags(0x80000000u);
}
static void UseFakes() {
  q15t::SetLevelNameTargetsForTest(reinterpret_cast<uintptr_t>(&FakeReal), reinterpret_cast<uintptr_t>(&FakePlain));
}

using Fn = uint64_t (*)(void*, uint64_t);
static uint64_t CallSlot(int i, void* game, uint64_t name) { return reinterpret_cast<Fn>(Word(kSlot[i]))(game, name); }

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);

  g_page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  void* img = mmap(nullptr, kImage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (img == MAP_FAILED) {
    std::printf("mmap of the fake image failed\n");
    return 2;
  }
  g_base = reinterpret_cast<uintptr_t>(img);

  // 1. install: all four vtable words (and only those) now hold the wrapper; the callee addresses are base + the vaddrs
  Prepare();
  q15t::InstallResult r = q15t::InstallLevelName(g_base);
  CHECK(r.status == q15t::SwapStatus::kOk, "install status %s", q15t::StatusName(r.status));
  const uintptr_t wrapper = q15t::LevelNameWrapperForTest();
  for (int i = 0; i < 4; ++i) {
    CHECK(Word(kSlot[i]) == wrapper, "slot %d (vaddr 0x%lx) not swapped: 0x%lx", i, (unsigned long)kSlot[i], (unsigned long)Word(kSlot[i]));
    CHECK(Word(kSlot[i] - 8) == 0x1111 && Word(kSlot[i] + 8) == 0x2222, "slot %d neighbour touched", i);
  }
  uintptr_t real = 0, plain = 0;
  q15t::LevelNameTargetsForTest(&real, &plain);
  CHECK(real == g_base + kFn, "real target 0x%lx", (unsigned long)real);
  CHECK(plain == g_base + kPlainStub, "plain target 0x%lx", (unsigned long)plain);
  CHECK(HasLine("levelname installed", 1) && HasLine("slots=4"), "no install record");
  std::printf("record: %s\n", g_lines.empty() ? "(none)" : g_lines[0].text.c_str());
  if (g_fail != 0) {  // the words are not the wrapper: calling through them would run the fake image, stop here
    std::printf("levelname_test: install failed, remaining checks skipped\n");
    return 1;
  }

  // 2. the wrapper's rule, through each of the four slots
  UseFakes();
  for (int i = 0; i < 4; ++i) {
    g_real_calls = g_plain_calls = 0;
    SetFlags(0x80000000u);
    uint64_t a = CallSlot(i, g_game, kGlobalMp);  // global_mp + lowspec: the plain name
    CHECK(a == kGlobalMp && g_plain_calls == 1 && g_real_calls == 0, "slot %d: global_mp lowspec: ans 0x%llx plain %d real %d", i,
          (unsigned long long)a, g_plain_calls, g_real_calls);
    CHECK(g_plain_game == g_game && g_plain_name == kGlobalMp, "plain callee got other args");
    g_real_calls = g_plain_calls = 0;
    const uint64_t other = 0x9a3717b576c23967ull;  // the cut global_mp's own name: any other level
    a = CallSlot(i, g_game, other);
    CHECK(a == (other ^ 0x0a5a5aull) && g_real_calls == 1 && g_plain_calls == 0, "slot %d: other level not left to the real function", i);
    CHECK(g_real_game == g_game && g_real_name == other, "real callee got other args");
    g_real_calls = g_plain_calls = 0;
    SetFlags(0);  // lowspec clear: the real function (it answers plain by itself)
    a = CallSlot(i, g_game, kGlobalMp);
    CHECK(g_real_calls == 1 && g_plain_calls == 0, "slot %d: lowspec clear took the plain path", i);
  }

  // 3. each part of the rule alone
  g_real_calls = g_plain_calls = 0;
  SetFlags(0x7fffffffu);  // every bit but 31
  CallSlot(0, g_game, kGlobalMp);
  CHECK(g_real_calls == 1 && g_plain_calls == 0, "a bit other than 31 counted as lowspec");
  g_real_calls = g_plain_calls = 0;
  SetFlags(0x80000000u, kFlag - 4);  // bit 31 of the neighbour word only
  CallSlot(0, g_game, kGlobalMp);
  SetFlags(0x80000000u, kFlag + 4);
  CallSlot(0, g_game, kGlobalMp);
  CHECK(g_real_calls == 2 && g_plain_calls == 0, "the flag is read from the wrong offset");
  g_real_calls = g_plain_calls = 0;
  SetFlags(0x80000000u);
  CallSlot(0, g_game, kGlobalMp ^ 1);  // a near miss of the key
  CallSlot(0, g_game, kGlobalMp ^ 0x8000000000000000ull);
  CallSlot(0, g_game, 0);
  CHECK(g_real_calls == 3 && g_plain_calls == 0, "a name that is not global_mp took the plain path");
  g_real_calls = g_plain_calls = 0;
  SetFlags(0xffffffffu);
  CallSlot(0, g_game, kGlobalMp);
  CHECK(g_plain_calls == 1 && g_real_calls == 0, "all bits set: not lowspec");

  // 4. an unreadable game object never faults: the real function answers
  g_real_calls = g_plain_calls = 0;
  uint64_t a = CallSlot(1, nullptr, kGlobalMp);
  CHECK(g_real_calls == 1 && g_plain_calls == 0 && a == (kGlobalMp ^ 0x0a5a5aull), "null game: real not called");
  g_real_calls = g_plain_calls = 0;
  a = CallSlot(1, reinterpret_cast<void*>(0x7ffffffff000ull), kGlobalMp);  // an address in no mapping of this process
  CHECK(g_real_calls == 1 && g_plain_calls == 0, "unmapped game: real not called");

  // 5. counters and records: one INFO line per global_mp substitution (the first 16), none for other levels
  Prepare();
  r = q15t::InstallLevelName(g_base);
  UseFakes();
  g_lines.clear();
  for (int i = 0; i < 1000; ++i) CallSlot(i & 3, g_game, 0x1234 + i);
  CHECK(g_lines.empty(), "an ordinary level logged (%zu lines)", g_lines.size());
  CHECK(q15t::LevelNameKeptPlainCount() == 0, "counter moved for ordinary levels");
  CallSlot(0, g_game, kGlobalMp);
  CHECK(q15t::LevelNameKeptPlainCount() == 1, "counter %llu", (unsigned long long)q15t::LevelNameKeptPlainCount());
  CHECK(HasLine("levelname global_mp kept its plain name", 1) && HasLine("name=0x3f9915d3001dc28e") && HasLine("n=1 "), "no record for the first global_mp");
  std::printf("record: %s\n", g_lines.empty() ? "(none)" : g_lines[0].text.c_str());
  for (int i = 0; i < 40; ++i) CallSlot(2, g_game, kGlobalMp);
  CHECK(g_lines.size() == 16, "log not bounded to the first 16: %zu lines", g_lines.size());
  CHECK(q15t::LevelNameKeptPlainCount() == 41, "counter after 41: %llu", (unsigned long long)q15t::LevelNameKeptPlainCount());
  SetFlags(0);
  const size_t before = g_lines.size();
  CallSlot(0, g_game, kGlobalMp);
  CHECK(g_lines.size() == before && q15t::LevelNameKeptPlainCount() == 41, "lowspec clear counted as a substitution");

  // 6. a word that is not the pinned function: refused, ERROR, and the words already swapped are put back
  for (int bad = 0; bad < 4; ++bad) {
    Prepare();
    Word(kSlot[bad]) = g_base + kFn + 4;  // some other function
    r = q15t::InstallLevelName(g_base);
    CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal, "bad slot %d: status %s", bad, q15t::StatusName(r.status));
    for (int i = 0; i < 4; ++i) {
      const uintptr_t want = i == bad ? g_base + kFn + 4 : g_base + kFn;
      CHECK(Word(kSlot[i]) == want, "bad slot %d: slot %d left as 0x%lx (not rolled back or touched)", bad, i, (unsigned long)Word(kSlot[i]));
    }
    char needle[40];
    std::snprintf(needle, sizeof needle, "slot_index=%d ", bad);
    CHECK(HasLine("levelname refused", 3) && HasLine(needle) && HasLine("why=unexpected_original"), "bad slot %d: no ERROR record", bad);
    char rb[40];
    std::snprintf(rb, sizeof rb, "rolled_back=%d", bad);
    CHECK(HasLine(rb) && !HasLine("levelname installed"), "bad slot %d: rollback count not reported", bad);
    if (bad == 3) std::printf("record: %s\n", g_lines.empty() ? "(none)" : g_lines.back().text.c_str());
    // the refused install leaves the retry possible once the word is right
    Word(kSlot[bad]) = g_base + kFn;
    r = q15t::InstallLevelName(g_base);
    CHECK(r.status == q15t::SwapStatus::kOk, "bad slot %d: install after the word is right: %s", bad, q15t::StatusName(r.status));
  }

  // 7. other refusals: no base, twice, a base whose slots are in no mapping
  Prepare();
  r = q15t::InstallLevelName(0);
  CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "no_base") == 0 && HasLine("levelname refused", 3), "base 0 accepted");
  r = q15t::InstallLevelName(0x10000);
  CHECK(r.status == q15t::SwapStatus::kUnmapped && HasLine("why=unmapped", 3), "unmapped slots: %s", q15t::StatusName(r.status));
  r = q15t::InstallLevelName(g_base);
  CHECK(r.status == q15t::SwapStatus::kOk, "install %s", q15t::StatusName(r.status));
  g_lines.clear();
  r = q15t::InstallLevelName(g_base);
  CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "already_installed") == 0 && HasLine("levelname refused", 2),
        "second install not refused");
  CHECK(Word(kSlot[0]) == wrapper && Word(kSlot[3]) == wrapper, "second install disturbed the words");

  // 8. a read-only vtable page (RELRO after relocation): swapped, and read-only again afterwards
  Prepare();
  for (uintptr_t s : kSlot) mprotect(reinterpret_cast<void*>((g_base + s) & ~(g_page - 1)), g_page, PROT_READ);
  r = q15t::InstallLevelName(g_base);
  CHECK(r.status == q15t::SwapStatus::kOk, "read-only pages: %s", q15t::StatusName(r.status));
  CHECK(Word(kSlot[0]) == wrapper && Word(kSlot[3]) == wrapper, "read-only pages not swapped");
  for (uintptr_t s : kSlot) {
    q15t::Mapping m;
    CHECK(q15t::FindMapping(g_base + s, &m) && m.perms[1] != 'w', "page of 0x%lx left writable", (unsigned long)s);
  }
  for (uintptr_t s : kSlot) mprotect(reinterpret_cast<void*>((g_base + s) & ~(g_page - 1)), g_page, PROT_READ | PROT_WRITE);

  // 9. an executable page is never written (a code word is not this module's to touch)
  Prepare();
  mprotect(reinterpret_cast<void*>((g_base + kSlot[3]) & ~(g_page - 1)), g_page, PROT_READ | PROT_EXEC);
  r = q15t::InstallLevelName(g_base);
  CHECK(r.status == q15t::SwapStatus::kExecutablePage, "executable page: %s", q15t::StatusName(r.status));
  CHECK(Word(kSlot[0]) == g_base + kFn && Word(kSlot[1]) == g_base + kFn && Word(kSlot[2]) == g_base + kFn, "executable page: earlier words not rolled back");
  mprotect(reinterpret_cast<void*>((g_base + kSlot[3]) & ~(g_page - 1)), g_page, PROT_READ | PROT_WRITE);

  munmap(img, kImage);
  if (g_fail == 0) std::printf("levelname_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
