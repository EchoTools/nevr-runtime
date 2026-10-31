// Host test of the `heaps` module (src/quest/plugins/q15t/src/heaps.cpp) against a fake game library: one anonymous mapping standing in for libr15's
// load image, with the four rodata sizes, the four GLOB_DAT words (made read-only like RELRO) and the four reader
// instructions at their pinned ELF vaddrs. The test's own table below is the independent copy of the pins (readelf -rW lines
// 89726 / 98594 / 92082 / 94611 of the source build's readelf note, not in this tree); the module's pins must agree with it. Real got_swap, real
// mprotect, real /proc/self/maps, real SafeRead.
#include <dirent.h>
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
#include "heaps.h"
#include "module_switch.h"
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
static int Count(const char* needle, int level = -1) {
  int c = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++c;
  return c;
}

static const uint64_t kMiB = 1ull << 20;
struct Expect {
  const char* id;
  uintptr_t got, ro, reader;
  uint32_t reader_word;
  uint64_t stock, target;
};
// R06 R07 R08 R09: GLOB_DAT slot -> rodata symbol value (readelf), reader = disassembly VA - 0x100000
static const Expect kExpect[4] = {
    {"R06", 0x370b720, 0x2b9caa0, 0xf78590, 0xf9400101, 206 * kMiB, 320 * kMiB},
    {"R07", 0x371d020, 0x2b9ca90, 0xf78524, 0xf9400101, 400 * kMiB, 640 * kMiB},
    {"R08", 0x3710188, 0x2b9ca98, 0xf78614, 0xf9400101, 160 * kMiB, 512 * kMiB},
    {"R09", 0x3715198, 0x2b9e1c0, 0x11ffdec, 0xf9400100, 40 * kMiB, 128 * kMiB},
};
static const size_t kLibSize = 0x3800000;

static uintptr_t NewLib() {
  void* p = mmap(nullptr, kLibSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) std::abort();
  const uintptr_t base = reinterpret_cast<uintptr_t>(p);
  for (const Expect& e : kExpect) {
    *reinterpret_cast<uint64_t*>(base + e.ro) = e.stock;
    *reinterpret_cast<uintptr_t*>(base + e.got) = base + e.ro;
    *reinterpret_cast<uint32_t*>(base + e.reader) = e.reader_word;
  }
  return base;
}
static void Protect(uintptr_t base) {  // every page that holds one of the words: read-only (RELRO / rodata / text)
  for (const Expect& e : kExpect)
    for (uintptr_t a : {e.got, e.ro, e.reader}) mprotect(reinterpret_cast<void*>((base + a) & ~uintptr_t(4095)), 4096, PROT_READ);
}
static void Poke64(uintptr_t addr, uint64_t v) {  // change a word on a protected page, then protect it again
  void* pg = reinterpret_cast<void*>(addr & ~uintptr_t(4095));
  mprotect(pg, 4096, PROT_READ | PROT_WRITE);
  *reinterpret_cast<uint64_t*>(addr) = v;
  mprotect(pg, 4096, PROT_READ);
}
static void Poke32(uintptr_t addr, uint32_t v) {
  void* pg = reinterpret_cast<void*>(addr & ~uintptr_t(4095));
  mprotect(pg, 4096, PROT_READ | PROT_WRITE);
  *reinterpret_cast<uint32_t*>(addr) = v;
  mprotect(pg, 4096, PROT_READ);
}
static uint64_t Rodata(uintptr_t base, int i) { return *reinterpret_cast<uint64_t*>(base + kExpect[i].ro); }
static uintptr_t SlotWord(uintptr_t base, int i) { return *reinterpret_cast<uintptr_t*>(base + kExpect[i].got); }
// what the game does: ldr x8,[GOT]; ldr x1,[x8]
static uint64_t GameRead(uintptr_t base, int i) { return *reinterpret_cast<uint64_t*>(SlotWord(base, i)); }
static bool Writable(uintptr_t addr) {
  q15t::Mapping m;
  return q15t::FindMapping(addr, &m) && m.perms[1] == 'w';
}
static void Drop(uintptr_t base) { munmap(reinterpret_cast<void*>(base), kLibSize); }

static std::string g_dir;
static void ClearDir() {
  DIR* d = opendir(g_dir.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d))
    if (e->d_name[0] != '.') unlink((g_dir + "/" + e->d_name).c_str());
  closedir(d);
}
static void Touch(const char* name) {
  FILE* f = std::fopen((g_dir + "/" + name).c_str(), "w");
  if (f != nullptr) std::fclose(f);
}

static void Fresh(const char* device) {
  g_lines.clear();
  ClearDir();
  q15t::ResetHeapsForTest();
  q15t::SetHeapsDeviceForTest(device);
}

int main() {
  char tmpl[] = "/tmp/q15t-heaps-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  g_dir = tmpl;
  q15t::SetModuleSwitchDirForTest(g_dir.c_str());
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);

  // 0. the module's pins are the readelf pins
  int n = 0;
  const q15t::HeapRowPin* pins = q15t::HeapRowPins(&n);
  CHECK(n == 4, "row count %d", n);
  for (int i = 0; i < n && i < 4; ++i) {
    const Expect& e = kExpect[i];
    CHECK(std::strcmp(pins[i].id, e.id) == 0 && pins[i].got == e.got && pins[i].ro == e.ro && pins[i].reader == e.reader &&
              pins[i].reader_word == e.reader_word && pins[i].stock == e.stock && pins[i].target == e.target,
          "pin %s differs from the readelf table", e.id);
  }

  // 1. a Quest 3 (not "hollywood"): all four slots swapped, the game reads the new sizes, nothing writable is left behind
  Fresh("eureka");
  uintptr_t base = NewLib();
  Protect(base);
  q15t::InstallResult r = q15t::InstallHeaps(base);
  CHECK(r.status == q15t::SwapStatus::kOk, "install: %s %s", q15t::StatusName(r.status), r.reason);
  for (int i = 0; i < 4; ++i) {
    CHECK(GameRead(base, i) == kExpect[i].target, "%s: the game reads %llu MiB, want %llu", kExpect[i].id,
          (unsigned long long)(GameRead(base, i) >> 20), (unsigned long long)(kExpect[i].target >> 20));
    CHECK(Rodata(base, i) == kExpect[i].stock, "%s: the rodata word was touched", kExpect[i].id);
    CHECK(SlotWord(base, i) != base + kExpect[i].ro, "%s: slot not swapped", kExpect[i].id);
    CHECK(!Writable(base + kExpect[i].got), "%s: the slot page was left writable", kExpect[i].id);
  }
  q15t::HeapsStats st = q15t::GetHeapsStats();
  CHECK(st.rows == 4 && st.swapped == 4 && st.refused == 0, "stats %d/%d/%d", st.rows, st.swapped, st.refused);
  CHECK(Count("heaps row swapped", 1) == 4, "want 4 INFO swap lines, got %d", Count("heaps row swapped", 1));
  CHECK(Count("row=R06") >= 1 && Count("value_mib=320") == 1 && Count("value_mib=640") == 1 && Count("value_mib=512") == 1 &&
            Count("value_mib=128") == 1,
        "swap lines do not carry the values");
  CHECK(Count("observed=0x14000000") == 1 && Count("observed=0x28000000") == 1, "read-back values not logged");
  CHECK(Count("device=eureka") == 4, "device not logged");
  Drop(base);

  // 2. a Quest 2 ("hollywood"): the System heap is 512 MiB, the others unchanged
  Fresh("hollywood");
  base = NewLib();
  Protect(base);
  r = q15t::InstallHeaps(base);
  CHECK(r.status == q15t::SwapStatus::kOk, "hollywood install: %s", q15t::StatusName(r.status));
  CHECK(GameRead(base, 1) == 512 * kMiB, "hollywood System heap %llu MiB", (unsigned long long)(GameRead(base, 1) >> 20));
  CHECK(GameRead(base, 0) == 320 * kMiB && GameRead(base, 2) == 512 * kMiB && GameRead(base, 3) == 128 * kMiB, "other rows changed");
  Drop(base);

  // 2b. no device property at all: the non-Quest-2 value
  Fresh("");
  base = NewLib();
  Protect(base);
  q15t::InstallHeaps(base);
  CHECK(GameRead(base, 1) == 640 * kMiB, "unknown device System heap %llu MiB", (unsigned long long)(GameRead(base, 1) >> 20));
  Drop(base);

  // 3. a slot that does not hold base + its rodata address (another hook owns it): that row is refused and left alone, the
  // other three are swapped
  Fresh("eureka");
  base = NewLib();
  Protect(base);
  Poke64(base + kExpect[2].got, 0x1234560);
  r = q15t::InstallHeaps(base);
  CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal && std::strcmp(r.reason, "R08") == 0, "foreign slot: %s %s",
        q15t::StatusName(r.status), r.reason);
  CHECK(SlotWord(base, 2) == 0x1234560, "the foreign slot word was changed");
  CHECK(GameRead(base, 0) == 320 * kMiB && GameRead(base, 1) == 640 * kMiB && GameRead(base, 3) == 128 * kMiB,
        "one refusal stopped the other rows");
  CHECK(Count("heaps row refused", 3) == 1 && Count("row=R08") >= 1, "no ERROR line for the refused row");
  st = q15t::GetHeapsStats();
  CHECK(st.swapped == 3 && st.refused == 1, "stats %d/%d", st.swapped, st.refused);
  Drop(base);

  // 4. a libr15 whose rodata word is not the stock size (an already-patched file): refused with a WARNING, slot untouched
  Fresh("eureka");
  base = NewLib();
  Protect(base);
  Poke64(base + kExpect[3].ro, 128 * kMiB);
  r = q15t::InstallHeaps(base);
  CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "R09") == 0, "non-stock rodata: %s", r.reason);
  CHECK(SlotWord(base, 3) == base + kExpect[3].ro, "slot of a non-stock row was swapped");
  CHECK(Count("rodata word is not the stock size", 2) == 1 && Count("observed=0x8000000") == 1, "no WARNING with the value");
  Drop(base);

  // 5. the reader is not the stock `ldr xN,[x8]`: refused with a WARNING, slot untouched
  Fresh("eureka");
  base = NewLib();
  Protect(base);
  Poke32(base + kExpect[0].reader, 0xd503201f);  // nop
  r = q15t::InstallHeaps(base);
  CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "R06") == 0, "reader word: %s", r.reason);
  CHECK(SlotWord(base, 0) == base + kExpect[0].ro, "slot swapped although the reader differs");
  CHECK(Count("reader word is not the stock load", 2) == 1, "no WARNING for the reader word");
  CHECK(GameRead(base, 1) == 640 * kMiB, "the other rows were not installed");
  Drop(base);

  // 6. a slot on an executable page is never written
  Fresh("eureka");
  base = NewLib();
  Protect(base);
  mprotect(reinterpret_cast<void*>((base + kExpect[0].got) & ~uintptr_t(4095)), 4096, PROT_READ | PROT_EXEC);
  r = q15t::InstallHeaps(base);
  CHECK(r.status == q15t::SwapStatus::kExecutablePage, "exec page: %s", q15t::StatusName(r.status));
  CHECK(SlotWord(base, 0) == base + kExpect[0].ro, "an executable page was written");
  CHECK(Count("executable_page", 3) == 1, "no ERROR naming executable_page");
  Drop(base);

  // 7. heaps.off: nothing is swapped, one line
  Fresh("eureka");
  Touch("heaps.off");
  base = NewLib();
  Protect(base);
  r = q15t::InstallHeaps(base);
  CHECK(r.status == q15t::SwapStatus::kOk && std::strcmp(r.reason, "heaps_off") == 0, "heaps.off: %s", r.reason);
  for (int i = 0; i < 4; ++i) CHECK(SlotWord(base, i) == base + kExpect[i].ro, "%s swapped although heaps.off", kExpect[i].id);
  CHECK(Count("heaps off", 1) == 1, "no INFO line for heaps.off");
  Drop(base);

  // 8. memsys.off (the source build's switch for the System heap): only R07 stays stock
  Fresh("eureka");
  Touch("memsys.off");
  base = NewLib();
  Protect(base);
  r = q15t::InstallHeaps(base);
  CHECK(r.status == q15t::SwapStatus::kOk, "memsys.off: %s", q15t::StatusName(r.status));
  CHECK(SlotWord(base, 1) == base + kExpect[1].ro && GameRead(base, 1) == 400 * kMiB, "System heap swapped although memsys.off");
  CHECK(GameRead(base, 0) == 320 * kMiB && GameRead(base, 2) == 512 * kMiB && GameRead(base, 3) == 128 * kMiB, "other rows not swapped");
  CHECK(Count("heaps row off", 1) == 1, "no INFO line for the skipped row");
  Drop(base);

  // 9. no base
  Fresh("eureka");
  r = q15t::InstallHeaps(0);
  CHECK(r.status == q15t::SwapStatus::kNullSlot && std::strcmp(r.reason, "no_base") == 0 && Count("no game base", 3) == 1, "base 0");

  // 10. a second install: the slots hold the plugin's own pointers, so every row is refused (no chaining), nothing changes
  Fresh("eureka");
  base = NewLib();
  Protect(base);
  q15t::InstallHeaps(base);
  const uintptr_t first0 = SlotWord(base, 0);
  g_lines.clear();
  r = q15t::InstallHeaps(base);
  CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal, "second install: %s", q15t::StatusName(r.status));
  CHECK(SlotWord(base, 0) == first0 && GameRead(base, 0) == 320 * kMiB, "second install changed a slot");
  CHECK(Count("heaps row refused", 3) == 4, "second install: want 4 ERROR lines, got %d", Count("heaps row refused", 3));
  Drop(base);

  ClearDir();
  rmdir(g_dir.c_str());
  if (g_fail == 0) std::printf("heaps_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
