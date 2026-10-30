// Host test of the `boosterpool` module (src/quest/plugins/q15t/src/boosterpool.cpp): a fake game library image (a sparse anonymous
// mapping whose offsets are the pinned build's ELF vaddrs, so the production slot and data addresses are used as they are), a
// fake AllocatePooledActor real function, a fake Append and a fake plain-name word. Real SafeRead, real got_swap, real /proc/self/maps.
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "boosterpool.h"
#include "got_swap.h"
#include "nevr/plugin_interface.h"
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

// the pinned build's ELF vaddrs (boosterpool.cpp; ReVault VA = ELF + 0x100000)
static const uintptr_t kSlot = 0x36cd1f8;       // GOT word of the AllocatePooledActor import
static const uintptr_t kFn = 0x18037c4;         // the real function
static const uintptr_t kAppend = 0xf31ea0;      // CSymbol64::Append (a PLT stub)
static const uintptr_t kPlainName = 0x37af868;  // DAT_038af868
static const size_t kImage = 0x3800000;

static uintptr_t g_base = 0;

// the 16-byte SActor the real function returns in x0:x1
struct ActorPair {
  uint64_t id, gen;
};
static const uint64_t kPlain = 0x1122334455667788ull;
static uint64_t Suffixed(uint64_t plain) { return plain ^ 0xabcdef5a5a5aull; }

static int g_real_calls = 0;
static uint64_t g_real_name = 0;
static void* g_real_st = nullptr;
static ActorPair FakeReal(void* st, uint64_t name) {
  ++g_real_calls;
  g_real_st = st;
  g_real_name = name;
  return ActorPair{name ^ 0x5a5a, 7};
}
// Append(name, "_lowspec", 1, 1) is the suffixed name; any other call returns a marker the checks would see
static uint64_t FakeAppend(uint64_t name, const char* suffix, uint32_t a, uint32_t b) {
  if (std::strcmp(suffix, "_lowspec") != 0 || a != 1 || b != 1) return 0xdeadull;
  return Suffixed(name);
}

using Fn = ActorPair (*)(void*, uint64_t);
static ActorPair CallSlot(void* st, uint64_t name) {
  return reinterpret_cast<Fn>(*reinterpret_cast<uintptr_t*>(g_base + kSlot))(st, name);
}

static uintptr_t& Word(uintptr_t vaddr) { return *reinterpret_cast<uintptr_t*>(g_base + vaddr); }

// clears the record and the fakes' call log; the GOT word is left as the caller set it
static void Reset() {
  g_lines.clear();
  g_real_calls = 0;
  g_real_name = 0;
  g_real_st = nullptr;
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);

  void* img = mmap(nullptr, kImage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (img == MAP_FAILED) {
    std::printf("mmap of the fake image failed\n");
    return 2;
  }
  g_base = reinterpret_cast<uintptr_t>(img);
  void* st = reinterpret_cast<void*>(0x5000);  // the game space's ST: passed through, never read
  *reinterpret_cast<uint64_t*>(g_base + kPlainName) = kPlain;

  // 1. install: the GOT word holds the wrapper, and the real target is base + the pinned function
  Reset();
  Word(kSlot) = g_base + kFn;
  Word(kSlot - 8) = 0x1111;  // neighbours, never touched
  Word(kSlot + 8) = 0x2222;
  q15t::InstallResult r = q15t::InstallBoosterPool(g_base);
  CHECK(r.status == q15t::SwapStatus::kOk, "install status %s", q15t::StatusName(r.status));
  CHECK(Word(kSlot) == q15t::BoosterPoolWrapperForTest(), "GOT word not swapped: 0x%lx", (unsigned long)Word(kSlot));
  CHECK(Word(kSlot - 8) == 0x1111 && Word(kSlot + 8) == 0x2222, "a neighbour of the GOT word was touched");
  uintptr_t real = 0, append = 0, plain_addr = 0;
  q15t::BoosterPoolTargetsForTest(&real, &append, &plain_addr);
  CHECK(real == g_base + kFn, "real target 0x%lx", (unsigned long)real);
  CHECK(append == g_base + kAppend && plain_addr == g_base + kPlainName, "append / plain targets 0x%lx 0x%lx", (unsigned long)append,
        (unsigned long)plain_addr);
  CHECK(HasLine("boosterpool installed", 1) && HasLine("fn_vaddr=0x18037c4"), "no install record");

  // the fakes stand in for the two callees from here on (the image holds zeros where the game code is)
  q15t::SetBoosterPoolTargetsForTest(reinterpret_cast<uintptr_t>(&FakeReal), reinterpret_cast<uintptr_t>(&FakeAppend),
                                     g_base + kPlainName);

  // 2. the plain name (the lowspec bit clear, or a caller that asks for the plain name): passed through unchanged
  Reset();
  uint64_t kept0 = q15t::BoosterPoolKeptCount();
  ActorPair a = CallSlot(st, kPlain);
  CHECK(g_real_calls == 1 && g_real_name == kPlain, "plain name: real got 0x%lx in %d calls", (unsigned long)g_real_name, g_real_calls);
  CHECK(g_real_st == st, "the ST argument was not passed through");
  CHECK(a.id == (kPlain ^ 0x5a5a) && a.gen == 7, "the actor pair was not returned as the real function gave it");
  CHECK(q15t::BoosterPoolKeptCount() == kept0, "a plain name counted as a substitution");

  // 3. the suffixed name the stock builds under lowspec: the real function gets the plain name instead, counted and logged
  Reset();
  kept0 = q15t::BoosterPoolKeptCount();
  CallSlot(st, Suffixed(kPlain));
  CHECK(g_real_calls == 1 && g_real_name == kPlain, "suffixed name: real got 0x%lx, want the plain name", (unsigned long)g_real_name);
  CHECK(q15t::BoosterPoolKeptCount() == kept0 + 1, "kept count did not move by one");
  CHECK(HasLine("boosterpool actor kept its plain name", 1) && HasLine(("n=" + std::to_string(kept0 + 1) + " ").c_str()),
        "no kept record for the substitution");

  // 4. some other name, even one that ends in _lowspec for another actor: untouched
  Reset();
  kept0 = q15t::BoosterPoolKeptCount();
  CallSlot(st, kPlain ^ 1);
  CHECK(g_real_calls == 1 && g_real_name == (kPlain ^ 1), "other name: real got 0x%lx", (unsigned long)g_real_name);
  CHECK(q15t::BoosterPoolKeptCount() == kept0, "other name counted as a substitution");

  // 5. the plain name word unreadable: the call passes through, counted as unreadable, never as a substitution
  Reset();
  q15t::SetBoosterPoolTargetsForTest(reinterpret_cast<uintptr_t>(&FakeReal), reinterpret_cast<uintptr_t>(&FakeAppend), 0x1000);
  const uint64_t unread0 = q15t::BoosterPoolUnreadableCount();
  kept0 = q15t::BoosterPoolKeptCount();
  CallSlot(st, Suffixed(kPlain));
  CHECK(g_real_calls == 1 && g_real_name == Suffixed(kPlain), "unreadable plain name: the call was changed");
  CHECK(q15t::BoosterPoolUnreadableCount() == unread0 + 1 && q15t::BoosterPoolKeptCount() == kept0, "unreadable count did not move by one");
  q15t::SetBoosterPoolTargetsForTest(reinterpret_cast<uintptr_t>(&FakeReal), reinterpret_cast<uintptr_t>(&FakeAppend),
                                     g_base + kPlainName);

  // 6. a second install is refused and writes nothing
  q15t::ResetBoosterPoolForTest();
  Word(kSlot) = g_base + kFn;
  r = q15t::InstallBoosterPool(g_base);
  CHECK(r.status == q15t::SwapStatus::kOk, "reinstall after reset: %s", q15t::StatusName(r.status));
  const uintptr_t wrapper = q15t::BoosterPoolWrapperForTest();
  r = q15t::InstallBoosterPool(g_base);
  CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "already_installed") == 0 && HasLine("boosterpool refused", 2),
        "second install was not refused");
  CHECK(Word(kSlot) == wrapper, "second install changed the GOT word");

  // 7. the GOT word holds something else: refused with the found and expected words, nothing written
  Reset();
  q15t::ResetBoosterPoolForTest();
  Word(kSlot) = g_base + kFn + 0x10;
  r = q15t::InstallBoosterPool(g_base);
  CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal, "unexpected word: status %s", q15t::StatusName(r.status));
  CHECK(Word(kSlot) == g_base + kFn + 0x10, "a refused install wrote the GOT word");
  CHECK(HasLine("boosterpool refused", 3) && HasLine("slot_vaddr=0x36cd1f8") && HasLine("status=unexpected_original"), "no refusal record");

  // 8. no base: refused before any write
  Reset();
  q15t::ResetBoosterPoolForTest();
  r = q15t::InstallBoosterPool(0);
  CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "no_base") == 0 && HasLine("boosterpool refused", 3), "base 0 accepted");

  if (g_fail == 0) std::printf("boosterpool_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
