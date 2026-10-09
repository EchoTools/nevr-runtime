// Host test for the Quest GOT backend and the typed callback thunks.
//
// Two kinds of image are hooked through the production Install/Remove:
//   - real shared objects built by the justfile (got_fixture_*.cpp) and loaded
//     with dlopen, covering JUMP_SLOT and GLOB_DAT, RELRO and writable GOT pages,
//     lazy binding, concurrency and unload;
//   - images built in memory (SynthImage), whose layout the test controls: the
//     malformed, ambiguous and unsupported tables, link-time-relative dynamic
//     pointers (the Bionic shape) and a slot page that refuses mprotect.
// Nothing here restores memory by hand to fake a success; the only direct slot
// writes are the ones that stage a hostile state for a negative case.
//
// Built with -fno-exceptions, like every translation unit that includes
// callback_thunk.h. The one place an exception is thrown is thunk_exception_fixture.cpp,
// built with exceptions, so an exception crosses a thunk frame the way a game exception
// would.
//
// Run: got_hook_test <fixture-dir>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#define NEVR_TEST_HAVE_NLOHMANN 1
#endif

#include "callback_thunk.h"
#include "got_hook.h"
#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "pinned_targets.h"
#include "quest/tests/test_check.h"

namespace {

using namespace sentinel;

// ---- log capture ------------------------------------------------------------

std::mutex g_captureMutex;

struct Line {
  LogLevel level;
  std::string text;
};
std::vector<Line>& Captured() {
  static std::vector<Line> lines;
  return lines;
}
// A strict check for the flat objects hook_log.cpp emits: {"k":"string"|number|true,...}.
bool ValidFlatJson(const std::string& t) {
  std::size_t i = 0;
  auto str = [&]() {
    if (i >= t.size() || t[i] != '"') return false;
    for (++i; i < t.size() && t[i] != '"'; ++i) {
      const unsigned char c = static_cast<unsigned char>(t[i]);
      if (c < 0x20 || c >= 0x80) return false;
      if (c == '\\') {
        ++i;
        if (i >= t.size()) return false;
        if (t[i] == 'u') {
          for (int k = 0; k < 4; ++k) {
            ++i;
            if (i >= t.size() || !std::isxdigit(static_cast<unsigned char>(t[i]))) return false;
          }
        } else if (std::strchr("\"\\/bfnrt", t[i]) == nullptr) {
          return false;
        }
      }
    }
    if (i >= t.size()) return false;
    ++i;
    return true;
  };
  if (t.empty() || t[i++] != '{') return false;
  for (bool first = true;; first = false) {
    if (!first) {
      if (i >= t.size() || t[i++] != ',') return false;
    }
    if (!str() || i >= t.size() || t[i++] != ':') return false;
    if (i < t.size() && t[i] == '"') {
      if (!str()) return false;
    } else if (t.compare(i, 4, "true") == 0) {
      i += 4;
    } else {
      const std::size_t start = i;
      if (i < t.size() && t[i] == '-') ++i;
      while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i;
      if (i == start || (i == start + 1 && t[start] == '-')) return false;
    }
    if (i < t.size() && t[i] == '}') return i + 1 == t.size();
  }
}

void SilentSink(LogLevel, const char*) {}

int g_invalidLines = 0;
int g_nlohmannParsed = 0;
void CaptureSink(LogLevel level, const char* line) {
  const std::lock_guard<std::mutex> guard(g_captureMutex);
  if (!ValidFlatJson(line)) {
    ++g_invalidLines;
    std::fprintf(stderr, "invalid JSON log line: %s\n", line);
  }
#ifdef NEVR_TEST_HAVE_NLOHMANN
  {
    const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("event") ||
        !parsed.contains("level") || !parsed.contains("ts_ms")) {
      ++g_invalidLines;
      std::fprintf(stderr, "nlohmann rejected log line: %s\n", line);
    } else {
      ++g_nlohmannParsed;
    }
  }
#endif
  Captured().push_back({level, line});
}

std::size_t Count(LogLevel level, const std::string& needle) {
  const std::lock_guard<std::mutex> guard(g_captureMutex);
  std::size_t n = 0;
  for (const Line& l : Captured()) {
    if (l.level == level && l.text.find(needle) != std::string::npos) ++n;
  }
  return n;
}
std::size_t Errors() {
  const std::lock_guard<std::mutex> guard(g_captureMutex);
  std::size_t n = 0;
  for (const Line& l : Captured()) n += l.level == LogLevel::kError ? 1 : 0;
  return n;
}

// ---- helpers ----------------------------------------------------------------

std::string g_dir;

std::string PagePerms(const void* addr) {
  std::ifstream maps("/proc/self/maps");
  std::string line;
  const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(addr);
  while (std::getline(maps, line)) {
    unsigned long long lo = 0, hi = 0;
    char perms[8] = {};
    if (std::sscanf(line.c_str(), "%llx-%llx %7s", &lo, &hi, perms) == 3 && a >= lo && a < hi) {
      return perms;
    }
  }
  return "";
}

void ForceWrite(void** slot, void* value) {
  const std::uintptr_t page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
  void* const start = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(slot) & ~(page - 1));
  mprotect(start, page, PROT_READ | PROT_WRITE);
  *slot = value;
  mprotect(start, page, PROT_READ);
}

int AddImpl(int a, int b) { return a + b; }
int SubImpl(int a, int b) { return a - b; }

}  // namespace

// The raw install is private to GotHook; tests (and only tests) reach it through this class.
namespace sentinel {
struct GotHookTestAccess {
  static GotStatus Install(GotHook& hook, const GotTarget& target, void* hookFn, void** originalOut,
                           ImageLookup lookup) {
    return hook.Install(target, hookFn, originalOut, lookup);
  }
};
}  // namespace sentinel

namespace {

GotStatus Install(GotHook& hook, const GotTarget& target, void* hookFn, void** originalOut,
                  ImageLookup lookup = FindLoadedImage) {
  return GotHookTestAccess::Install(hook, target, hookFn, originalOut, lookup);
}

struct AddTag {};
struct SubTag {};
using AddThunk = CallbackThunk<AddTag, int(int, int)>;
using SubThunk = CallbackThunk<SubTag, int(int, int)>;

int AddPlus100(AddThunk::Fn original, int a, int b) noexcept { return original(a, b) + 100; }
NEVR_HOOK_RECORD(kAddPlus100, AddThunk, &AddPlus100);
int SubPlus100(SubThunk::Fn original, int a, int b) noexcept { return original(a, b) + 100; }
NEVR_HOOK_RECORD(kSubPlus100, SubThunk, &SubPlus100);

struct Module {
  void* handle = nullptr;
  int (*callAdd)(int, int) = nullptr;
  int (*callSub)(int, int) = nullptr;
  void* realAdd = nullptr;
  void* realSub = nullptr;
  std::string soname;
};

Module Open(const char* soname) {
  Module m;
  m.soname = soname;
  m.handle = dlopen((g_dir + "/" + soname).c_str(), RTLD_NOW | RTLD_LOCAL);
  QCHECK(m.handle != nullptr);
  if (m.handle == nullptr) return m;
  m.callAdd = reinterpret_cast<int (*)(int, int)>(dlsym(m.handle, "fx_call_add"));
  m.callSub = reinterpret_cast<int (*)(int, int)>(dlsym(m.handle, "fx_call_sub"));
  m.realAdd = dlsym(m.handle, "fx_add");
  m.realSub = dlsym(m.handle, "fx_sub");
  QCHECK(m.callAdd != nullptr && m.callSub != nullptr && m.realAdd != nullptr && m.realSub != nullptr);
  return m;
}

void Prepare() {
  Captured().clear();
  AddThunk::Reset();
  SubThunk::Reset();
}

SlotResolution Resolve(const Module& m, const char* symbol, RelocKind kind) {
  ElfImage image;
  QCHECK(FindLoadedImage(m.soname.c_str(), &image));
  return ResolveSlot(image, GotTarget{m.soname.c_str(), symbol, kind}, kNativeRelocs);
}

// ---- real shared objects ----------------------------------------------------

void JumpSlotRoundTrip(const char* soname, bool expectRelro) {
  Prepare();
  Module m = Open(soname);
  QCHECK(m.callAdd(2, 3) == 5);
  ElfImage image;
  QCHECK(FindLoadedImage(soname, &image));
  char buildId[64] = {};
  QCHECK(ReadBuildId(image, buildId, sizeof(buildId)));
  QCHECK(std::strlen(buildId) == 40);

  GotTarget target{soname, "fx_add", RelocKind::kJumpSlot, buildId};
  const SlotResolution r = ResolveSlot(image, target, kNativeRelocs);
  QCHECK_STATUS(r.status, GotStatus::kOk);
  QCHECK(r.restoreReadOnly == expectRelro);
  const std::string before = PagePerms(r.slot);
  QCHECK(before == (expectRelro ? "r--p" : "rw-p"));
  QCHECK(*r.slot == m.realAdd);

  AddThunk::Arm(kAddPlus100);
  GotHook hook;
  QCHECK_STATUS(Install(hook, target, AddThunk::EntryAddress(), AddThunk::OriginalOut()),
                GotStatus::kOk);
  QCHECK(hook.installed());
  QCHECK(AddThunk::Original() == reinterpret_cast<AddThunk::Fn>(m.realAdd));
  QCHECK(*r.slot == AddThunk::EntryAddress());
  QCHECK(PagePerms(r.slot) == before);
  QCHECK(m.callAdd(2, 3) == 105);
  QCHECK(AddThunk::Calls() == 1);
  QCHECK(Count(LogLevel::kInfo, "\"op\":\"install\",\"status\":\"ok\"") == 1);
  QCHECK(Errors() == 0);

  QCHECK_STATUS(hook.Remove(), GotStatus::kOk);
  QCHECK(!hook.installed());
  QCHECK(*r.slot == m.realAdd);
  QCHECK(PagePerms(r.slot) == before);
  QCHECK(m.callAdd(2, 3) == 5);
  QCHECK(Count(LogLevel::kInfo, "\"op\":\"remove\",\"status\":\"ok\"") == 1);
  QCHECK_STATUS(hook.Remove(), GotStatus::kNotInstalled);
  dlclose(m.handle);
}

void GlobDatRoundTrip(const char* soname) {
  Prepare();
  Module m = Open(soname);
  QCHECK(m.callSub(5, 3) == 2);
  const SlotResolution r = Resolve(m, "fx_sub", RelocKind::kGlobDat);
  QCHECK_STATUS(r.status, GotStatus::kOk);
  QCHECK(*r.slot == m.realSub);
  SubThunk::Arm(kSubPlus100);
  GotHook hook;
  GotTarget target{soname, "fx_sub", RelocKind::kGlobDat};
  QCHECK_STATUS(Install(hook, target, SubThunk::EntryAddress(), SubThunk::OriginalOut()),
                GotStatus::kOk);
  QCHECK(m.callSub(5, 3) == 102);
  QCHECK_STATUS(hook.Remove(), GotStatus::kOk);
  QCHECK(m.callSub(5, 3) == 2);
  dlclose(m.handle);
}

// The lazy-linked fixture is opened with RTLD_NOW, so glibc binds every slot at
// load and the slot holds the real function. This therefore tests the DT_FLAGS
// refusal (the module is not marked BIND_NOW), not a live resolver stub.
void LazyLinkedModuleRefused() {
  Prepare();
  Module m = Open("libgotfx_consumer_lazy.so");
  const SlotResolution probe = Resolve(m, "fx_add", RelocKind::kJumpSlot);
  QCHECK_STATUS(probe.status, GotStatus::kLazyBinding);
  GotHook hook;
  void* sentinelValue = reinterpret_cast<void*>(0x1234);
  void* out = sentinelValue;
  GotTarget target{"libgotfx_consumer_lazy.so", "fx_add", RelocKind::kJumpSlot};
  QCHECK_STATUS(Install(hook, target, AddThunk::EntryAddress(), &out), GotStatus::kLazyBinding);
  QCHECK(out == sentinelValue);
  QCHECK(Errors() == 1 && Count(LogLevel::kError, "\"status\":\"lazy_binding\"") == 1);
  QCHECK(m.callAdd(2, 3) == 5);
  // GLOB_DAT slots are resolved at load time even in a lazy module.
  GotTarget glob{"libgotfx_consumer_lazy.so", "fx_sub", RelocKind::kGlobDat};
  SubThunk::Arm(kSubPlus100);
  GotHook globHook;
  QCHECK_STATUS(Install(globHook, glob, SubThunk::EntryAddress(), SubThunk::OriginalOut()),
                GotStatus::kOk);
  QCHECK(m.callSub(5, 3) == 102);
  QCHECK_STATUS(globHook.Remove(), GotStatus::kOk);
  dlclose(m.handle);
}

struct NegativeCase {
  const char* name;
  GotTarget target;
  GotStatus expected;
};

void NegativeCases() {
  Prepare();
  Module m = Open("libgotfx_consumer_now.so");
  const char* so = "libgotfx_consumer_now.so";
  const SlotResolution addSlot = Resolve(m, "fx_add", RelocKind::kJumpSlot);
  QCHECK_STATUS(addSlot.status, GotStatus::kOk);
  int other = 0;
  GotTarget wrongOriginal{so, "fx_add", RelocKind::kJumpSlot};
  wrongOriginal.expectedOriginal = &other;
  GotTarget wrongBuildId{so, "fx_add", RelocKind::kJumpSlot,
                         "0000000000000000000000000000000000000000"};
  GotTarget wrongVaddr{so, "fx_add", RelocKind::kJumpSlot, nullptr, addSlot.slotVaddr + 8};

  const NegativeCase cases[] = {
      {"module absent", {"libnope.so", "fx_add", RelocKind::kJumpSlot}, GotStatus::kModuleNotLoaded},
      {"partial module name", {"consumer_now.so", "fx_add", RelocKind::kJumpSlot},
       GotStatus::kModuleNotLoaded},
      {"symbol absent", {so, "fx_nope", RelocKind::kJumpSlot}, GotStatus::kSymbolNotFound},
      {"call symbol asked as GLOB_DAT", {so, "fx_add", RelocKind::kGlobDat},
       GotStatus::kWrongRelocationType},
      {"address symbol asked as JUMP_SLOT", {so, "fx_sub", RelocKind::kJumpSlot},
       GotStatus::kWrongRelocationType},
      {"build id differs", wrongBuildId, GotStatus::kBuildIdMismatch},
      {"pinned address is not the slot", wrongVaddr, GotStatus::kSlotOffsetMismatch},
      {"slot holds another value", wrongOriginal, GotStatus::kOriginalMismatch},
  };
  for (const NegativeCase& c : cases) {
    Captured().clear();
    void* const before = *addSlot.slot;
    void* sentinelValue = reinterpret_cast<void*>(0x4242);
    void* out = sentinelValue;
    GotHook hook;
    const GotStatus status = Install(hook, c.target, AddThunk::EntryAddress(), &out);
    if (status != c.expected) std::fprintf(stderr, "case: %s\n", c.name);
    QCHECK_STATUS(status, c.expected);
    QCHECK(!hook.installed());
    QCHECK(*addSlot.slot == before);
    QCHECK(out == sentinelValue);
    QCHECK(Errors() == 1);
    QCHECK(Count(LogLevel::kError, std::string("\"status\":\"") + GotStatusName(c.expected) + "\"") == 1);
  }
  // build-id mismatch names both ids
  Captured().clear();
  GotHook hook;
  void* out = nullptr;
  QCHECK_STATUS(Install(hook, wrongBuildId, AddThunk::EntryAddress(), &out), GotStatus::kBuildIdMismatch);
  QCHECK(Count(LogLevel::kError, "\"expected_build_id\":\"0000000000000000000000000000000000000000\",\"actual_build_id\":\"") == 1);

  GotHook nullFn;
  QCHECK_STATUS(Install(nullFn, GotTarget{so, "fx_add", RelocKind::kJumpSlot}, nullptr, &out),
                GotStatus::kBadArgument);

  // The right pin and the right expected original install.
  GotTarget exact{so, "fx_add", RelocKind::kJumpSlot, nullptr, addSlot.slotVaddr, m.realAdd};
  AddThunk::Arm(kAddPlus100);
  GotHook good;
  QCHECK_STATUS(Install(good, exact, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);
  QCHECK_STATUS(good.Remove(), GotStatus::kOk);
  dlclose(m.handle);
}

void DuplicateAndChained() {
  Prepare();
  Module m = Open("libgotfx_consumer_now.so");
  const char* so = "libgotfx_consumer_now.so";
  GotTarget target{so, "fx_add", RelocKind::kJumpSlot};
  const SlotResolution r = Resolve(m, "fx_add", RelocKind::kJumpSlot);
  AddThunk::Arm(kAddPlus100);
  GotHook first;
  QCHECK_STATUS(Install(first, target, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);

  QCHECK_STATUS(Install(first, target, AddThunk::EntryAddress(), AddThunk::OriginalOut()),
                GotStatus::kAlreadyInstalled);
  GotHook second;
  void* out = nullptr;
  QCHECK_STATUS(Install(second, target, AddThunk::EntryAddress(), &out), GotStatus::kAlreadyInstalled);
  QCHECK(out == nullptr);
  // A different hook function reaches the slot registry rather than the value check.
  QCHECK_STATUS(Install(second, target, reinterpret_cast<void*>(&AddImpl), &out),
                GotStatus::kAlreadyInstalled);
  QCHECK(out == nullptr);
  QCHECK(m.callAdd(2, 3) == 105);

  // Someone else overwrote the slot: Remove refuses to write over them.
  ForceWrite(r.slot, m.realSub);
  Captured().clear();
  QCHECK_STATUS(first.Remove(), GotStatus::kSlotChanged);
  QCHECK(first.installed());
  QCHECK(*r.slot == m.realSub);
  QCHECK(Count(LogLevel::kError, "\"op\":\"remove\",\"status\":\"slot_changed\"") == 1);
  ForceWrite(r.slot, AddThunk::EntryAddress());
  QCHECK_STATUS(first.Remove(), GotStatus::kOk);
  QCHECK(*r.slot == m.realAdd);
  // The slot is free again for a new handle.
  QCHECK_STATUS(Install(second, target, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);
  QCHECK_STATUS(second.Remove(), GotStatus::kOk);
  dlclose(m.handle);
}

void ModuleUnloaded() {
  Prepare();
  Module m = Open("libgotfx_consumer_now.so");
  GotTarget target{"libgotfx_consumer_now.so", "fx_add", RelocKind::kJumpSlot};
  AddThunk::Arm(kAddPlus100);
  GotHook hook;
  QCHECK_STATUS(Install(hook, target, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);
  QCHECK(dlclose(m.handle) == 0);
  Captured().clear();
  QCHECK_STATUS(hook.Remove(), GotStatus::kModuleChanged);
  QCHECK(!hook.installed());
  QCHECK(Count(LogLevel::kError, "\"op\":\"remove\",\"status\":\"module_changed\"") == 1);
}

void ConcurrentCallers() {
  Prepare();
  Module m = Open("libgotfx_consumer_now.so");
  GotTarget target{"libgotfx_consumer_now.so", "fx_add", RelocKind::kJumpSlot};
  AddThunk::Arm(kAddPlus100);
  std::atomic<bool> stop{false};
  std::atomic<long> bad{0};
  std::atomic<long> calls{0};
  std::thread caller([&] {
    while (!stop.load()) {
      const int v = m.callAdd(2, 3);
      if (v != 5 && v != 105) bad.fetch_add(1);
      calls.fetch_add(1);
    }
  });
  GotHook hook;
  for (int i = 0; i < 300; ++i) {
    QCHECK_STATUS(Install(hook, target, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);
    QCHECK_STATUS(hook.Remove(), GotStatus::kOk);
  }
  stop.store(true);
  caller.join();
  QCHECK(bad.load() == 0);
  QCHECK(calls.load() > 0);
  QCHECK(AddThunk::Faults() == 0);
  dlclose(m.handle);
}

// ---- in-memory images -------------------------------------------------------

constexpr std::size_t kPage = 4096;
constexpr std::size_t kImageSize = 4 * kPage;
// Layout, vaddr == offset:  [0x0000,0x1000) R-X: tables   [0x1000,0x4000) RW
//   RELRO [0x1000,0x3000)   dynamic 0x1800   RELRO slots 0x2000..   plain slots 0x3008..
constexpr std::uint64_t kStrtab = 0x100, kSymtab = 0x200, kJmprel = 0x400, kRela = 0x600;
constexpr std::uint64_t kDynamic = 0x1800;
constexpr std::uint64_t kRelroSlot = 0x2000, kPlainSlot = 0x3008;

struct Rel {
  bool inJmprel;
  std::uint64_t offset;
  std::uint32_t type;
  std::uint32_t sym;  // 1 = alpha, 2 = beta
  std::int64_t addend;
};

struct SynthSpec {
  RelocNumbers nums = kNativeRelocs;
  std::vector<Rel> rels;
  bool bindNow = true;
  bool useFlags1 = false;
  bool absolutePtrs = false;
  bool withDynamic = true;
  std::uint64_t pltrelType = DT_RELA;
  std::uint64_t relaEnt = sizeof(Elf64_Rela);
  std::uint64_t strsz = 16;
  std::uint64_t jmprelSizeAdjust = 0;
  bool readOnlyBacking = false;  // map the image from a read-only shared file mapping
};

struct SynthImage {
  void* mem = nullptr;
  std::vector<Elf64_Phdr> phdrs;
  ElfImage image;
  ~SynthImage() {
    if (mem != nullptr) munmap(mem, kImageSize);
  }
};

SynthImage* g_synth = nullptr;
bool SynthLookup(const char*, ElfImage* out) {
  if (g_synth == nullptr) return false;
  *out = g_synth->image;
  return true;
}

void Build(const SynthSpec& spec, void* originalValue, SynthImage* out) {
  std::vector<std::uint8_t> bytes(kImageSize, 0);
  auto put = [&bytes](std::uint64_t at, const void* src, std::size_t n) {
    std::memcpy(bytes.data() + at, src, n);
  };
  const char strings[] = "\0alpha\0beta\0";
  put(kStrtab, strings, sizeof(strings));
  Elf64_Sym syms[3] = {};
  syms[1].st_name = 1;
  syms[2].st_name = 7;
  put(kSymtab, syms, sizeof(syms));

  std::vector<Elf64_Rela> jmprel, rela;
  for (const Rel& r : spec.rels) {
    Elf64_Rela e{};
    e.r_offset = r.offset;
    e.r_info = ELF64_R_INFO(static_cast<Elf64_Xword>(r.sym), r.type);
    e.r_addend = r.addend;
    (r.inJmprel ? jmprel : rela).push_back(e);
  }
  if (!jmprel.empty()) put(kJmprel, jmprel.data(), jmprel.size() * sizeof(Elf64_Rela));
  if (!rela.empty()) put(kRela, rela.data(), rela.size() * sizeof(Elf64_Rela));

  std::vector<Elf64_Dyn> dyn;
  auto add = [&dyn](Elf64_Sxword tag, std::uint64_t value) {
    Elf64_Dyn d{};
    d.d_tag = tag;
    d.d_un.d_val = value;
    dyn.push_back(d);
  };

  // glibc-style images carry relocated (absolute) dynamic pointers, which need the
  // mapping address, so an anonymous mapping is reserved first. A read-only file
  // mapping is filled before it is mapped and therefore uses vaddrs.
  void* mem = MAP_FAILED;
  if (!spec.readOnlyBacking) {
    mem = mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    QCHECK(mem != MAP_FAILED);
  }
  const std::uint64_t adj =
      (spec.absolutePtrs && !spec.readOnlyBacking) ? reinterpret_cast<std::uint64_t>(mem) : 0;

  if (spec.withDynamic) {
    add(DT_STRTAB, adj + kStrtab);
    add(DT_SYMTAB, adj + kSymtab);
    add(DT_STRSZ, spec.strsz);
    if (!jmprel.empty()) {
      add(DT_JMPREL, adj + kJmprel);
      add(DT_PLTRELSZ, jmprel.size() * sizeof(Elf64_Rela) + spec.jmprelSizeAdjust);
      add(DT_PLTREL, spec.pltrelType);
    }
    if (!rela.empty()) {
      add(DT_RELA, adj + kRela);
      add(DT_RELASZ, rela.size() * sizeof(Elf64_Rela));
    }
    add(DT_RELAENT, spec.relaEnt);
    if (spec.bindNow) add(spec.useFlags1 ? DT_FLAGS_1 : DT_FLAGS, spec.useFlags1 ? DF_1_NOW : DF_BIND_NOW);
    add(DT_NULL, 0);
    put(kDynamic, dyn.data(), dyn.size() * sizeof(Elf64_Dyn));
  }
  // Slots start holding the same recognisable value.
  for (std::uint64_t slot = kRelroSlot; slot < kRelroSlot + 0x400; slot += 8) {
    put(slot, &originalValue, sizeof(originalValue));
  }
  put(kPlainSlot, &originalValue, sizeof(originalValue));

  if (spec.readOnlyBacking) {
    // Written through a read-write descriptor, mapped through a read-only one:
    // the mapping then cannot be given PROT_WRITE (mprotect fails with EACCES).
    // Next to the fixture libraries, which the caller placed in a scratch build tree.
    std::string pathText = g_dir + "/synthXXXXXX";
    char* const path = pathText.data();
    const int wfd = mkstemp(path);
    QCHECK(wfd >= 0);
    QCHECK(pwrite(wfd, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size()));
    close(wfd);
    const int rfd = open(path, O_RDONLY);
    QCHECK(rfd >= 0);
    unlink(path);
    mem = mmap(nullptr, kImageSize, PROT_READ, MAP_SHARED, rfd, 0);
    QCHECK(mem != MAP_FAILED);
    close(rfd);
  } else {
    std::memcpy(mem, bytes.data(), bytes.size());
  }
  out->mem = mem;

  auto phdr = [](std::uint32_t type, std::uint32_t flags, std::uint64_t vaddr, std::uint64_t size) {
    Elf64_Phdr p{};
    p.p_type = type;
    p.p_flags = flags;
    p.p_vaddr = vaddr;
    p.p_memsz = size;
    p.p_filesz = size;
    return p;
  };
  out->phdrs.clear();
  out->phdrs.push_back(phdr(PT_LOAD, PF_R | PF_X, 0, kPage));
  out->phdrs.push_back(phdr(PT_LOAD, PF_R | PF_W, kPage, 3 * kPage));
  if (spec.withDynamic) out->phdrs.push_back(phdr(PT_DYNAMIC, PF_R | PF_W, kDynamic, 0x100));
  out->phdrs.push_back(phdr(PT_GNU_RELRO, PF_R, kPage, 2 * kPage));
  out->image.base = reinterpret_cast<std::uintptr_t>(mem);
  out->image.phdr = out->phdrs.data();
  out->image.phnum = out->phdrs.size();
  std::strcpy(out->image.name, "synthetic.so");
}

GotTarget SynthTarget(RelocKind kind = RelocKind::kJumpSlot, const char* symbol = "alpha") {
  return GotTarget{"synthetic.so", symbol, kind};
}

SlotResolution ResolveSynth(const SynthSpec& spec, const GotTarget& target) {
  SynthImage img;
  Build(spec, reinterpret_cast<void*>(&AddImpl), &img);
  return ResolveSlot(img.image, target, spec.nums);
}

SynthSpec OneRel(std::uint64_t offset = kRelroSlot, std::int64_t addend = 0) {
  SynthSpec s;
  s.rels.push_back({true, offset, s.nums.jumpSlot, 1, addend});
  return s;
}

void SyntheticResolution() {
  const GotTarget jump = SynthTarget();

  for (bool absolute : {false, true}) {
    SynthSpec s = OneRel();
    s.absolutePtrs = absolute;
    const SlotResolution r = ResolveSynth(s, jump);
    QCHECK_STATUS(r.status, GotStatus::kOk);
    QCHECK(r.slotVaddr == kRelroSlot);
    QCHECK(r.restoreReadOnly);
  }
  {
    SynthSpec s = OneRel(kPlainSlot);
    const SlotResolution r = ResolveSynth(s, jump);
    QCHECK_STATUS(r.status, GotStatus::kOk);
    QCHECK(!r.restoreReadOnly);
  }
  {  // AArch64 numbering resolves on any host, and the host's numbering does not see it.
    SynthSpec s;
    s.nums = kAarch64Relocs;
    s.rels.push_back({true, kRelroSlot, kAarch64Relocs.jumpSlot, 1, 0});
    s.rels.push_back({false, kRelroSlot + 8, kAarch64Relocs.globDat, 2, 0});
    QCHECK_STATUS(ResolveSynth(s, jump).status, GotStatus::kOk);
    GotTarget glob = SynthTarget(RelocKind::kGlobDat, "beta");
    const SlotResolution g = ResolveSynth(s, glob);
    QCHECK_STATUS(g.status, GotStatus::kOk);
    QCHECK(g.slotVaddr == kRelroSlot + 8);
    SynthImage img;
    Build(s, reinterpret_cast<void*>(&AddImpl), &img);
    QCHECK_STATUS(ResolveSlot(img.image, jump, kNativeRelocs).status, GotStatus::kSymbolNotFound);
  }
  {  // two slots for one symbol need a pin; the same slot in both tables is one slot
    SynthSpec s = OneRel();
    s.rels.push_back({false, kRelroSlot + 8, s.nums.jumpSlot, 1, 0});
    QCHECK_STATUS(ResolveSynth(s, jump).status, GotStatus::kAmbiguousRelocation);
    GotTarget pinned = jump;
    pinned.slotVaddr = kRelroSlot + 8;
    const SlotResolution r = ResolveSynth(s, pinned);
    QCHECK_STATUS(r.status, GotStatus::kOk);
    QCHECK(r.slotVaddr == kRelroSlot + 8);
    pinned.slotVaddr = kRelroSlot + 16;
    QCHECK_STATUS(ResolveSynth(s, pinned).status, GotStatus::kSlotOffsetMismatch);

    SynthSpec dup = OneRel();
    dup.rels.push_back({false, kRelroSlot, dup.nums.jumpSlot, 1, 0});
    QCHECK_STATUS(ResolveSynth(dup, jump).status, GotStatus::kOk);
  }
  QCHECK_STATUS(ResolveSynth(OneRel(kRelroSlot, 8), jump).status, GotStatus::kUnsupportedAddend);
  QCHECK_STATUS(ResolveSynth(OneRel(kRelroSlot + 4), jump).status, GotStatus::kSlotMisaligned);
  QCHECK_STATUS(ResolveSynth(OneRel(0x500), jump).status, GotStatus::kSlotOutsideImage);
  QCHECK_STATUS(ResolveSynth(OneRel(0x9000), jump).status, GotStatus::kSlotOutsideImage);
  QCHECK_STATUS(ResolveSynth(OneRel(0x4000 - 4), jump).status, GotStatus::kSlotMisaligned);
  QCHECK_STATUS(ResolveSynth(OneRel(0x4000 - 8), jump).status, GotStatus::kOk);
  {
    SynthSpec s = OneRel();
    s.bindNow = false;
    QCHECK_STATUS(ResolveSynth(s, jump).status, GotStatus::kLazyBinding);
    s.rels.push_back({false, kRelroSlot + 8, s.nums.globDat, 2, 0});
    QCHECK_STATUS(ResolveSynth(s, SynthTarget(RelocKind::kGlobDat, "beta")).status, GotStatus::kOk);
    SynthSpec flags1 = OneRel();
    flags1.useFlags1 = true;
    QCHECK_STATUS(ResolveSynth(flags1, jump).status, GotStatus::kOk);
  }
  {
    SynthSpec s = OneRel();
    s.relaEnt = 16;
    QCHECK_STATUS(ResolveSynth(s, jump).status, GotStatus::kMalformedDynamic);
    s = OneRel();
    s.pltrelType = DT_REL;
    QCHECK_STATUS(ResolveSynth(s, jump).status, GotStatus::kMalformedDynamic);
    s = OneRel();
    s.jmprelSizeAdjust = 5;
    QCHECK_STATUS(ResolveSynth(s, jump).status, GotStatus::kMalformedDynamic);
    s = OneRel();
    s.strsz = 0;
    QCHECK_STATUS(ResolveSynth(s, jump).status, GotStatus::kMalformedDynamic);
    s = OneRel();
    s.rels[0].sym = 0x100000;  // symbol index far outside the image
    QCHECK_STATUS(ResolveSynth(s, jump).status, GotStatus::kMalformedDynamic);
    s = OneRel();
    s.strsz = 3;  // "alpha" starts past the end of the string table
    QCHECK_STATUS(ResolveSynth(s, jump).status, GotStatus::kSymbolNotFound);
    s = OneRel();
    s.withDynamic = false;
    QCHECK_STATUS(ResolveSynth(s, jump).status, GotStatus::kNoDynamicSegment);
  }
}

void SyntheticInstall() {
  Prepare();
  // Writable image, RELRO slot and plain slot, installed and removed through the
  // production API with the lookup injected.
  for (std::uint64_t slotVaddr : {kRelroSlot, kPlainSlot}) {
    SynthSpec spec = OneRel(slotVaddr);
    SynthImage img;
    Build(spec, reinterpret_cast<void*>(&AddImpl), &img);
    g_synth = &img;
    void** slot = reinterpret_cast<void**>(img.image.base + slotVaddr);
    // The loader makes RELRO read-only after relocation; do the same to the image.
    if (slotVaddr == kRelroSlot) QCHECK(mprotect(img.mem, 3 * kPage, PROT_READ) == 0);
    const std::string perms = PagePerms(slot);
    QCHECK(perms == (slotVaddr == kRelroSlot ? "r--p" : "rw-p"));
    GotTarget target = SynthTarget();
    target.expectedOriginal = reinterpret_cast<void*>(&AddImpl);
    GotHook hook;
    void* out = nullptr;
    QCHECK_STATUS(Install(hook, target, reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                  GotStatus::kOk);
    QCHECK(out == reinterpret_cast<void*>(&AddImpl));
    QCHECK(*slot == reinterpret_cast<void*>(&SubImpl));
    QCHECK(PagePerms(slot) == perms);
    QCHECK_STATUS(hook.Remove(), GotStatus::kOk);
    QCHECK(*slot == reinterpret_cast<void*>(&AddImpl));
    QCHECK(PagePerms(slot) == perms);

    // The module moves: Remove must not write into the old mapping.
    QCHECK_STATUS(Install(hook, target, reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                  GotStatus::kOk);
    g_synth = nullptr;
    Captured().clear();
    QCHECK_STATUS(hook.Remove(), GotStatus::kModuleChanged);
    QCHECK(*slot == reinterpret_cast<void*>(&SubImpl));
    g_synth = nullptr;
  }
  {  // implausible original: null, and a data address, both refused without expectedOriginal
    SynthSpec spec = OneRel();
    SynthImage img;
    Build(spec, nullptr, &img);
    g_synth = &img;
    GotHook hook;
    void* out = nullptr;
    Captured().clear();
    QCHECK_STATUS(Install(hook, SynthTarget(), reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                  GotStatus::kOriginalImplausible);
    QCHECK(Count(LogLevel::kError, "\"status\":\"original_implausible\"") == 1);
    g_synth = nullptr;
    int dataObject = 0;
    SynthImage img2;
    Build(spec, &dataObject, &img2);
    g_synth = &img2;
    QCHECK_STATUS(Install(hook, SynthTarget(), reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                  GotStatus::kOriginalImplausible);
    g_synth = nullptr;
  }
  {  // a slot page that refuses PROT_WRITE: the failure is reported and nothing is held
    SynthSpec spec = OneRel();
    spec.readOnlyBacking = true;
    SynthImage img;
    Build(spec, reinterpret_cast<void*>(&AddImpl), &img);
    g_synth = &img;
    void** slot = reinterpret_cast<void**>(img.image.base + kRelroSlot);
    GotTarget target = SynthTarget();
    target.expectedOriginal = reinterpret_cast<void*>(&AddImpl);
    for (int attempt = 0; attempt < 2; ++attempt) {  // second pass proves the slot was released
      Captured().clear();
      GotHook hook;
      void* sentinelValue = reinterpret_cast<void*>(0x77);
      void* out = sentinelValue;
      QCHECK_STATUS(Install(hook, target, reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                    GotStatus::kProtectFailed);
      QCHECK(out == sentinelValue);
      QCHECK(*slot == reinterpret_cast<void*>(&AddImpl));
      QCHECK(Errors() == 1);
      QCHECK(Count(LogLevel::kError, "\"status\":\"protect_failed\",\"stage\":\"enable_failed\"") == 1);
      QCHECK(Count(LogLevel::kError, "\"errno\":13") == 1);
    }
    g_synth = nullptr;
  }
}


// ---- write serialization ------------------------------------------------------

std::atomic<bool> g_raceArmed{false};
std::atomic<bool> g_raceOtherDone{false};
std::atomic<bool> g_raceOverlapped{false};
std::thread* g_raceThread = nullptr;
GotTarget* g_raceOtherTarget = nullptr;
GotHook* g_raceOtherHook = nullptr;

// Runs with the write lock held, after thread A's page became writable and before
// its store. Starts thread B, which installs a hook in a different slot of the
// SAME page, and gives it 300 ms to finish. With the lock B is parked until A has
// stored and re-protected the page; without it B completes in microseconds and
// protects the page read-only under A, whose store then faults.
void RaceObserver(void**, void*, StorePhase phase) {
  if (phase != StorePhase::kBeforeStore || !g_raceArmed.exchange(false)) return;
  g_raceThread = new std::thread([] {
    void* original = nullptr;
    const GotStatus st = Install(*g_raceOtherHook, *g_raceOtherTarget,
                                 reinterpret_cast<void*>(&SubImpl), &original);
    QCHECK_STATUS(st, GotStatus::kOk);
    g_raceOtherDone.store(true);
  });
  for (int waited = 0; waited < 300 && !g_raceOtherDone.load(); ++waited) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  g_raceOverlapped.store(g_raceOtherDone.load());
}

void SamePageWritesAreSerialized() {
  Prepare();
  Module m = Open("libgotfx_consumer_now.so");
  const char* so = "libgotfx_consumer_now.so";
  const SlotResolution addSlot = Resolve(m, "fx_add", RelocKind::kJumpSlot);
  const SlotResolution subSlot = Resolve(m, "fx_sub", RelocKind::kGlobDat);
  QCHECK_STATUS(addSlot.status, GotStatus::kOk);
  QCHECK_STATUS(subSlot.status, GotStatus::kOk);
  const std::uintptr_t page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
  QCHECK((reinterpret_cast<std::uintptr_t>(addSlot.slot) & ~(page - 1)) ==
         (reinterpret_cast<std::uintptr_t>(subSlot.slot) & ~(page - 1)));  // precondition: one page
  QCHECK(addSlot.restoreReadOnly && subSlot.restoreReadOnly);

  GotTarget addTarget{so, "fx_add", RelocKind::kJumpSlot};
  GotTarget subTarget{so, "fx_sub", RelocKind::kGlobDat};
  GotHook addHook, subHook;
  g_raceOtherTarget = &subTarget;
  g_raceOtherHook = &subHook;
  g_raceOtherDone.store(false);
  g_raceOverlapped.store(false);
  g_raceArmed.store(true);
  SetStoreObserver(&RaceObserver);
  void* addOriginal = nullptr;
  QCHECK_STATUS(Install(addHook, addTarget, reinterpret_cast<void*>(&AddImpl), &addOriginal),
                GotStatus::kOk);
  SetStoreObserver(nullptr);
  QCHECK(!g_raceOverlapped.load());  // B was still waiting while A held the page writable
  if (g_raceThread != nullptr) {
    g_raceThread->join();
    delete g_raceThread;
    g_raceThread = nullptr;
  }
  QCHECK(g_raceOtherDone.load());
  QCHECK(subHook.installed() && addHook.installed());
  QCHECK(PagePerms(addSlot.slot) == "r--p");
  QCHECK_STATUS(addHook.Remove(), GotStatus::kOk);
  QCHECK_STATUS(subHook.Remove(), GotStatus::kOk);

  // And as a plain stress run: two threads, two slots, one page. The capture
  // sink is not thread-safe, so the run uses a silent one.
  SetLogSink(&SilentSink);
  std::atomic<long> failures{0};
  auto loop = [&](const GotTarget& t, void* fn) {
    GotHook hook;
    void* original = nullptr;
    for (int i = 0; i < 1500; ++i) {
      if (Install(hook, t, fn, &original) != GotStatus::kOk) failures.fetch_add(1);
      if (hook.Remove() != GotStatus::kOk) failures.fetch_add(1);
    }
  };
  std::thread a(loop, addTarget, reinterpret_cast<void*>(&AddImpl));
  std::thread b(loop, subTarget, reinterpret_cast<void*>(&SubImpl));
  a.join();
  b.join();
  SetLogSink(&CaptureSink);
  QCHECK(failures.load() == 0);
  QCHECK(*addSlot.slot == m.realAdd && *subSlot.slot == m.realSub);
  dlclose(m.handle);
}

void* g_originalSeenAtStore = nullptr;
void* g_entryAtStore = nullptr;
void PublishObserver(void**, void* value, StorePhase phase) {
  if (phase == StorePhase::kBeforeStore && value == g_entryAtStore) g_originalSeenAtStore = reinterpret_cast<void*>(AddThunk::Original());
}

// The original has to be visible to the detour before the slot points at it. This
// observes the moment of the store itself, so it does not depend on timing.
void OriginalIsPublishedBeforeTheSlotChanges() {
  Prepare();
  Module m = Open("libgotfx_consumer_now.so");
  GotTarget target{"libgotfx_consumer_now.so", "fx_add", RelocKind::kJumpSlot};
  g_entryAtStore = AddThunk::EntryAddress();
  g_originalSeenAtStore = nullptr;
  SetStoreObserver(&PublishObserver);
  GotHook hook;
  QCHECK_STATUS(Install(hook, target, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);
  SetStoreObserver(nullptr);
  QCHECK(g_originalSeenAtStore == m.realAdd);
  QCHECK_STATUS(hook.Remove(), GotStatus::kOk);
  dlclose(m.handle);
}

// The restored protection is the page's real one, not the one RELRO implies: an
// image that declares RELRO over a page that is in fact writable stays writable.
void LiveProtectionDecidesWhatIsRestored() {
  Prepare();
  SynthSpec spec = OneRel();
  SynthImage img;
  Build(spec, reinterpret_cast<void*>(&AddImpl), &img);  // anonymous RW mapping, RELRO declared
  g_synth = &img;
  void** slot = reinterpret_cast<void**>(img.image.base + kRelroSlot);
  QCHECK(PagePerms(slot) == "rw-p");
  GotTarget target = SynthTarget();
  target.expectedOriginal = reinterpret_cast<void*>(&AddImpl);
  GotHook hook;
  void* out = nullptr;
  QCHECK_STATUS(Install(hook, target, reinterpret_cast<void*>(&SubImpl), &out, SynthLookup), GotStatus::kOk);
  QCHECK(PagePerms(slot) == "rw-p");
  QCHECK_STATUS(hook.Remove(), GotStatus::kOk);
  QCHECK(PagePerms(slot) == "rw-p");
  g_synth = nullptr;
}

// A module reloaded at the same base leaves a handle whose slot holds a fresh
// value: Remove reports kSlotChanged and Forget releases the reservation.
void ForgetReleasesAStaleHandle() {
  Prepare();
  Module m = Open("libgotfx_consumer_now.so");
  GotTarget target{"libgotfx_consumer_now.so", "fx_add", RelocKind::kJumpSlot};
  const SlotResolution r = Resolve(m, "fx_add", RelocKind::kJumpSlot);
  AddThunk::Arm(kAddPlus100);
  GotHook stale;
  QCHECK_STATUS(Install(stale, target, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);
  ForceWrite(r.slot, m.realAdd);  // what a reload at the same base looks like
  QCHECK_STATUS(stale.Remove(), GotStatus::kSlotChanged);
  stale.Forget();
  QCHECK(!stale.installed());
  Captured().clear();
  GotHook fresh;
  QCHECK_STATUS(Install(fresh, target, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);
  QCHECK_STATUS(fresh.Remove(), GotStatus::kOk);
  QCHECK(Errors() == 0);
  dlclose(m.handle);
}

// ---- failed re-protect, compare-and-swap, registry bound ----------------------

int g_protectFailuresLeft = 0;  // PROT_READ calls still to fail; negative: all of them
int FailingProtect(void* addr, std::size_t length, int prot) {
  if (prot == PROT_READ && g_protectFailuresLeft != 0) {
    if (g_protectFailuresLeft > 0) --g_protectFailuresLeft;
    errno = ENOMEM;
    return -1;
  }
  return mprotect(addr, length, prot);
}

// The slot is written, then the page cannot be made read-only again. The store is
// rolled back, the failure reported, and the original stays published: a thread
// may have entered the detour while the hook was live, and the original is the
// real function.
void FailedReprotectRollsBackAndKeepsTheOriginal() {
  for (const int failures : {1, -1}) {  // the retry succeeds / the retry fails too
    Prepare();
    SynthSpec spec = OneRel();
    SynthImage img;
    Build(spec, reinterpret_cast<void*>(&AddImpl), &img);
    g_synth = &img;
    void** slot = reinterpret_cast<void**>(img.image.base + kRelroSlot);
    QCHECK(mprotect(img.mem, 3 * kPage, PROT_READ) == 0);  // what the loader does to RELRO
    GotTarget target = SynthTarget();
    target.expectedOriginal = reinterpret_cast<void*>(&AddImpl);
    g_protectFailuresLeft = failures;
    SetProtectFunction(&FailingProtect);
    GotHook hook;
    void* sentinelValue = reinterpret_cast<void*>(0x77);
    void* out = sentinelValue;
    QCHECK_STATUS(Install(hook, target, reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                  GotStatus::kRestoreProtectFailed);
    SetProtectFunction(nullptr);
    QCHECK(!hook.installed());
    QCHECK(*slot == reinterpret_cast<void*>(&AddImpl));  // rolled back
    QCHECK(out == reinterpret_cast<void*>(&AddImpl));    // original stays published
    QCHECK(Count(LogLevel::kError, "\"status\":\"restore_protect_failed\"") == 1);
    QCHECK(Count(LogLevel::kError, "\"errno\":12") == 1);
    if (failures > 0) {
      QCHECK(PagePerms(slot) == "r--p");
      QCHECK(Errors() == 1);
    } else {
      QCHECK(PagePerms(slot) == "rw-p");  // reported, not hidden
      QCHECK(Count(LogLevel::kError, "\"status\":\"page_left_writable\"") == 1);
      QCHECK(mprotect(img.mem, 3 * kPage, PROT_READ) == 0);
    }
    // The reservation was released: the same slot installs and removes normally.
    Captured().clear();
    QCHECK_STATUS(Install(hook, target, reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                  GotStatus::kOk);
    QCHECK_STATUS(hook.Remove(), GotStatus::kOk);
    g_synth = nullptr;
  }
  g_protectFailuresLeft = 0;
}

void ClobberObserver(void** slot, void*, StorePhase phase) {
  if (phase == StorePhase::kBeforeStore) *slot = reinterpret_cast<void*>(&SubImpl);
}

// Install stores only over the original it read. A writer outside the lock that
// changes the slot after the check and before the store wins; Install reports
// kSlotChanged and the foreign value stays.
void InstallIsCompareAndSwap() {
  Prepare();
  Module m = Open("libgotfx_consumer_now.so");
  GotTarget target{"libgotfx_consumer_now.so", "fx_add", RelocKind::kJumpSlot};
  const SlotResolution r = Resolve(m, "fx_add", RelocKind::kJumpSlot);
  GotHook hook;
  void* sentinelValue = reinterpret_cast<void*>(0x99);
  void* out = sentinelValue;
  SetStoreObserver(&ClobberObserver);
  QCHECK_STATUS(Install(hook, target, AddThunk::EntryAddress(), &out), GotStatus::kSlotChanged);
  SetStoreObserver(nullptr);
  QCHECK(*r.slot == reinterpret_cast<void*>(&SubImpl));  // the other writer's value stays
  QCHECK(out == sentinelValue);
  QCHECK(!hook.installed());
  QCHECK(Count(LogLevel::kError, "\"status\":\"slot_changed\"") == 1);
  ForceWrite(r.slot, m.realAdd);
  AddThunk::Arm(kAddPlus100);
  QCHECK_STATUS(Install(hook, target, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);
  QCHECK_STATUS(hook.Remove(), GotStatus::kOk);
  dlclose(m.handle);
}

// 64 hooks fit; the 65th is refused with one log line; removing frees the room.
void RegistryBound() {
  Prepare();
  constexpr int kHooks = 65;
  SynthSpec spec;
  for (int i = 0; i < kHooks; ++i) {
    spec.rels.push_back({true, kRelroSlot + 8u * static_cast<unsigned>(i), spec.nums.jumpSlot, 1, 0});
  }
  SynthImage img;
  Build(spec, reinterpret_cast<void*>(&AddImpl), &img);
  g_synth = &img;
  std::vector<GotHook> hooks(kHooks);
  auto target = [](int i) {
    GotTarget t = SynthTarget();
    t.slotVaddr = kRelroSlot + 8u * static_cast<unsigned>(i);
    t.expectedOriginal = reinterpret_cast<void*>(&AddImpl);
    return t;
  };
  void* out = nullptr;
  int installed = 0;
  for (int i = 0; i < kHooks - 1; ++i) {
    if (Install(hooks[i], target(i), reinterpret_cast<void*>(&SubImpl), &out, SynthLookup) == GotStatus::kOk) {
      ++installed;
    }
  }
  QCHECK(installed == 64);
  Captured().clear();
  QCHECK_STATUS(Install(hooks[64], target(64), reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                GotStatus::kRegistryFull);
  QCHECK(Errors() == 1 && Count(LogLevel::kError, "\"status\":\"registry_full\"") == 1);
  QCHECK(*reinterpret_cast<void**>(img.image.base + kRelroSlot + 8u * 64) ==
         reinterpret_cast<void*>(&AddImpl));  // the refused slot was not written
  QCHECK_STATUS(hooks[0].Remove(), GotStatus::kOk);
  QCHECK_STATUS(Install(hooks[64], target(64), reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                GotStatus::kOk);
  for (int i = 1; i < kHooks; ++i) QCHECK_STATUS(hooks[i].Remove(), GotStatus::kOk);
  g_synth = nullptr;
}

void AfterStoreClobberObserver(void** slot, void*, StorePhase phase) {
  if (phase == StorePhase::kAfterStore) *slot = reinterpret_cast<void*>(&SubImpl);
}

// Our compare-and-swap stored the entry, then another writer overwrote it before the
// read-back: Install reports kWriteVerifyFailed, leaves their value alone, and keeps
// the original published for any thread that already jumped into the entry.
void WriteVerifyFailureKeepsTheOriginalPublished() {
  Prepare();
  Module m = Open("libgotfx_consumer_now.so");
  GotTarget target{"libgotfx_consumer_now.so", "fx_add", RelocKind::kJumpSlot};
  const SlotResolution r = Resolve(m, "fx_add", RelocKind::kJumpSlot);
  GotHook hook;
  void* sentinelValue = reinterpret_cast<void*>(0x55);
  void* out = sentinelValue;
  SetStoreObserver(&AfterStoreClobberObserver);
  QCHECK_STATUS(Install(hook, target, AddThunk::EntryAddress(), &out), GotStatus::kWriteVerifyFailed);
  SetStoreObserver(nullptr);
  QCHECK(*r.slot == reinterpret_cast<void*>(&SubImpl));  // the other writer's value stays
  QCHECK(out == m.realAdd);                               // not reverted to the entry value
  QCHECK(!hook.installed());
  QCHECK(Count(LogLevel::kError, "\"status\":\"write_verify_failed\"") == 1);

  // The slot stays reserved: the foreign value in it may chain our entry, and a retry would
  // publish it as the original and make the entry call itself. The refusal has its own status,
  // so an operator can tell it from a real duplicate.
  Captured().clear();
  void* retryOut = reinterpret_cast<void*>(0x56);
  QCHECK_STATUS(Install(hook, target, AddThunk::EntryAddress(), &retryOut), GotStatus::kSlotPoisoned);
  QCHECK(retryOut == reinterpret_cast<void*>(0x56));
  QCHECK(*r.slot == reinterpret_cast<void*>(&SubImpl));
  QCHECK(Count(LogLevel::kError, "\"status\":\"slot_poisoned\"") == 1);

  // A poisoned slot on a mapped module is never given back, whatever range is named.
  const std::uintptr_t page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
  void* const pageStart =
      reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(r.slot) & ~(page - 1));
  ReleasePoisonedSlotsIn(pageStart, page);
  QCHECK_STATUS(Install(hook, target, AddThunk::EntryAddress(), &retryOut), GotStatus::kSlotPoisoned);

  // Once the module is unmapped the reservation can be released, and a fresh mapping installs.
  ForceWrite(r.slot, m.realAdd);
  QCHECK(dlclose(m.handle) == 0);
  QCHECK(PagePerms(pageStart).empty());  // really unmapped
  ReleasePoisonedSlotsIn(pageStart, page);
  m = Open("libgotfx_consumer_now.so");
  AddThunk::Arm(kAddPlus100);
  GotHook again;
  QCHECK_STATUS(Install(again, target, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);
  QCHECK_STATUS(again.Remove(), GotStatus::kOk);
  dlclose(m.handle);
}

void* g_clobberSlot = nullptr;
bool g_clobberOnce = false;
int ClobberingProtect(void* addr, std::size_t length, int prot) {
  if (prot == PROT_READ && g_clobberOnce) {
    g_clobberOnce = false;
    *static_cast<void**>(g_clobberSlot) = &g_clobberOnce;  // a third value; the page is writable here
    errno = ENOMEM;
    return -1;
  }
  return mprotect(addr, length, prot);
}

// The re-protect fails after the compare-and-swap itself failed (our value never
// stored): the status stays kSlotChanged, the page-left-writable failure is still
// logged, and the entry value of the caller's pointer is restored.
void ReprotectFailureDoesNotHideAnEarlierFailure() {
  Prepare();
  SynthSpec spec = OneRel();
  SynthImage img;
  Build(spec, reinterpret_cast<void*>(&AddImpl), &img);
  g_synth = &img;
  void** slot = reinterpret_cast<void**>(img.image.base + kRelroSlot);
  QCHECK(mprotect(img.mem, 3 * kPage, PROT_READ) == 0);
  GotTarget target = SynthTarget();
  target.expectedOriginal = reinterpret_cast<void*>(&AddImpl);
  g_protectFailuresLeft = -1;
  SetProtectFunction(&FailingProtect);
  SetStoreObserver(&ClobberObserver);
  GotHook hook;
  void* sentinelValue = reinterpret_cast<void*>(0x66);
  void* out = sentinelValue;
  QCHECK_STATUS(Install(hook, target, reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                GotStatus::kSlotChanged);
  SetStoreObserver(nullptr);
  SetProtectFunction(nullptr);
  g_protectFailuresLeft = 0;
  QCHECK(out == sentinelValue);  // our value was never stored: entry value restored
  QCHECK(*slot == reinterpret_cast<void*>(&SubImpl));
  QCHECK(Count(LogLevel::kError, "\"status\":\"page_left_writable\"") == 1);
  QCHECK(Count(LogLevel::kError, "\"status\":\"slot_changed\"") == 1);
  QCHECK(mprotect(img.mem, 3 * kPage, PROT_READ) == 0);
  g_synth = nullptr;
}

// The re-protect fails after our store, and the slot changed again before the
// rollback: the rollback compare-and-swap fails, is logged, the other value stays and
// the original stays published (the entry may still be reachable through it).
void RollbackCompareAndSwapFailureIsLogged() {
  Prepare();
  SynthSpec spec = OneRel();
  auto imgPtr = std::make_unique<SynthImage>();
  SynthImage& img = *imgPtr;
  Build(spec, reinterpret_cast<void*>(&AddImpl), &img);
  g_synth = &img;
  void** slot = reinterpret_cast<void**>(img.image.base + kRelroSlot);
  QCHECK(mprotect(img.mem, 3 * kPage, PROT_READ) == 0);
  GotTarget target = SynthTarget();
  target.expectedOriginal = reinterpret_cast<void*>(&AddImpl);
  g_clobberSlot = slot;
  g_clobberOnce = true;
  SetProtectFunction(&ClobberingProtect);
  GotHook hook;
  void* sentinelValue = reinterpret_cast<void*>(0x88);
  void* out = sentinelValue;
  QCHECK_STATUS(Install(hook, target, reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                GotStatus::kRestoreProtectFailed);
  SetProtectFunction(nullptr);
  QCHECK(*slot == static_cast<void*>(&g_clobberOnce));  // not overwritten by the rollback
  QCHECK(out == reinterpret_cast<void*>(&AddImpl));
  QCHECK(Count(LogLevel::kError, "\"status\":\"rollback_cas_failed\"") == 1);
  QCHECK(PagePerms(slot) == "r--p");  // the retry succeeded
  // Their value is in the slot, so a retry is refused (by the original check here; the slot is
  // also poisoned, which WriteVerifyFailureKeepsTheOriginalPublished shows with a plausible value).
  void* retryOut = nullptr;
  QCHECK_STATUS(Install(hook, target, reinterpret_cast<void*>(&SubImpl), &retryOut, SynthLookup),
                GotStatus::kOriginalMismatch);
  // Released only once the image is really unmapped.
  ReleasePoisonedSlotsIn(img.mem, kImageSize);
  void* const base = img.mem;
  g_synth = nullptr;
  imgPtr.reset();
  ReleasePoisonedSlotsIn(base, kImageSize);
}

// ---- record-only arming, unreadable maps --------------------------------------

// Arm takes only a record that lies in the nevr_hook_records section. A record built at run
// time (the only way to get one outside the macro is HookRecordAccess) is refused, logged, and
// leaves the thunk as it was; HookRecord itself has no public constructor (compile_fail/).
void ArmRefusesARecordOutsideTheSection() {
  Prepare();
  QCHECK(IsRecordedHook(kAddPlus100));
  *AddThunk::OriginalOut() = reinterpret_cast<void*>(&AddImpl);
  auto entry = reinterpret_cast<AddThunk::Fn>(AddThunk::EntryAddress());
  const HookRecord<AddThunk> runtimeRecord = HookRecordAccess::Make<AddThunk>(AddThunk::EntryFn(), &AddPlus100);
  QCHECK(!IsRecordedHook(runtimeRecord));
  AddThunk::Arm(runtimeRecord);
  QCHECK(Count(LogLevel::kError, "\"status\":\"arm_refused\"") == 1);
  QCHECK(entry(2, 3) == 5);  // not armed: plain pass-through
  AddThunk::Arm(kAddPlus100);
  QCHECK(entry(2, 3) == 105);
  AddThunk::Reset();
}

int OpenDirectory() { return open("/", O_RDONLY); }          // read(2) fails with EISDIR
int OpenEmpty() { return open("/dev/null", O_RDONLY); }      // end of file before any data
int OpenNothing() { return -1; }                              // open fails

// A poisoned slot is released only on proof that its address is unmapped. A failed open, a read
// error and an immediate end of file are all "unknown" and keep the reservation (releasing it
// would let a retry publish a foreign hook that chains our entry as the original).
void PoisonedSlotIsKeptWhenMapsAreUnreadable() {
  Prepare();
  Module m = Open("libgotfx_consumer_now.so");
  GotTarget target{"libgotfx_consumer_now.so", "fx_add", RelocKind::kJumpSlot};
  const SlotResolution r = Resolve(m, "fx_add", RelocKind::kJumpSlot);
  GotHook hook;
  void* out = nullptr;
  SetStoreObserver(&AfterStoreClobberObserver);
  QCHECK_STATUS(Install(hook, target, AddThunk::EntryAddress(), &out), GotStatus::kWriteVerifyFailed);
  SetStoreObserver(nullptr);
  const std::uintptr_t page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
  void* const pageStart = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(r.slot) & ~(page - 1));
  ForceWrite(r.slot, m.realAdd);  // a plausible original, so only the poison can refuse the retry

  // The module is still mapped, so the slot was never released; make the "unmapped" evidence
  // unreliable three ways and check nothing is released.
  const MapsOpener opener[] = {&OpenDirectory, &OpenEmpty, &OpenNothing};
  for (MapsOpener fn : opener) {
    Captured().clear();
    SetMapsOpener(fn);
    ReleasePoisonedSlotsIn(pageStart, page);
    SetMapsOpener(nullptr);
    QCHECK(Count(LogLevel::kError, "\"status\":\"mapping_unknown\"") == 1);
    void* retryOut = nullptr;
    QCHECK_STATUS(Install(hook, target, AddThunk::EntryAddress(), &retryOut), GotStatus::kSlotPoisoned);
  }
  // A readable maps file that lists the page: still mapped, reported as such.
  Captured().clear();
  ReleasePoisonedSlotsIn(pageStart, page);
  QCHECK(Count(LogLevel::kError, "\"status\":\"still_mapped\"") == 1);

  // Unmapped and readable: released, with a log line saying so.
  QCHECK(dlclose(m.handle) == 0);
  Captured().clear();
  ReleasePoisonedSlotsIn(pageStart, page);
  QCHECK(Count(LogLevel::kInfo, "\"status\":\"released\"") == 1);
}

// Start and Stop from two threads at once: no orphaned thread, no double join, no hang.
void ReporterStartStopRace() {
  Prepare();
  auto loop = [] {
    for (int i = 0; i < 100; ++i) {
      StartReporter(5, 20, 50);
      StopReporter();
    }
  };
  SetLogSink(&SilentSink);  // the capture sink is not what is under test here
  std::thread a(loop), b(loop);
  a.join();
  b.join();
  StopReporter();
  SetLogSink(&CaptureSink);
  QCHECK(!ReporterRunning());
}

// The counter table holds kMaxReportCounters (48) across the whole program; the next one is refused
// and logged, and so is any registration after the reporter has started.
void ReporterCounterTableBound() {
  static_assert(kMaxReportCounters == 48, "the reporter's capacity changed: update this test and ADR 0003");
  Prepare();
  constexpr unsigned kTotal = kMaxReportCounters + 1;
  static char names[kTotal][16];  // "c00".."c48": must outlive the reporter, like a literal
  for (unsigned i = 0; i < kTotal; ++i) std::snprintf(names[i], sizeof(names[i]), "c%02u", i);
  static std::atomic<std::uint64_t> values[kTotal];
  unsigned accepted = 0;
  for (unsigned i = 0; i < kMaxReportCounters; ++i) accepted += RegisterReportCounter(names[i], &values[i]) ? 1 : 0;
  QCHECK(accepted == kMaxReportCounters);
  QCHECK(Errors() == 0);
  QCHECK(!RegisterReportCounter(names[kMaxReportCounters], &values[kMaxReportCounters]));
  QCHECK(Count(LogLevel::kError, "\"status\":\"register_refused\",\"counter\":\"c48\"") == 1);
  SetLogSink(&SilentSink);
  QCHECK(StartReporter(10, 20, 50));
  QCHECK(!RegisterReportCounter("after_start", &values[0]));  // registration order contract
  StopReporter();                                             // forgets the counters
  SetLogSink(&CaptureSink);
  QCHECK(RegisterReportCounter("again", &values[0]));         // room again after Stop
  StopReporter();
}

// ---- reporter -----------------------------------------------------------------

template <typename Pred>
bool WaitFor(Pred pred) {
  for (int i = 0; i < 600; ++i) {  // up to 3 s
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// A hook only increments a counter; the reporter thread logs it. A counter that never fires
// must not hold the others in fast mode (it once logged the hot counter every pass, forever):
//   grace window (200 ms): the hot counter's first change is logged once; at its end the idle
//   counter is reported "never_fired" once; then the steady cadence (600 ms) takes over for
//   both, logging "changed" at most once per pass.
void ReporterIsBoundedWithAHotAndAnIdleCounter() {
  Prepare();
  std::atomic<std::uint64_t> hot{0};
  std::atomic<std::uint64_t> idle{0};
  QCHECK(RegisterReportCounter("hot_counter", &hot));
  QCHECK(RegisterReportCounter("idle_counter", &idle));
  std::atomic<std::uint64_t> faults{0};
  QCHECK(RegisterReportCounter("fault_counter", &faults, ReportKind::kFaults));
  QCHECK(StartReporter(10, 200, 600));
  QCHECK(StartReporter(10, 200, 600));  // idempotent
  QCHECK(!RegisterReportCounter("late", &hot));  // refused once running
  QCHECK(Count(LogLevel::kInfo, "\"status\":\"reporter_started\"") == 1);

  // The hot counter moves continuously for 450 ms, well past the grace window.
  for (int i = 1; i <= 90; ++i) {
    hot.store(static_cast<std::uint64_t>(i));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  QCHECK(WaitFor([] { return Count(LogLevel::kInfo, "\"counter\":\"idle_counter\",\"value\":0,\"why\":\"never_fired\"") == 1; }));
  // 450+ ms in, before the first steady pass: exactly the first change and the never_fired line.
  QCHECK(Count(LogLevel::kInfo, "\"counter\":\"hot_counter\"") == 1);
  QCHECK(Count(LogLevel::kInfo, "\"event\":\"hook_counter\"") == 2);
  QCHECK(Count(LogLevel::kInfo, "\"why\":\"first_change\"") == 1);

  // The steady pass reports the hot counter's new value once; the idle one stays quiet.
  hot.store(1000);
  QCHECK(WaitFor([] { return Count(LogLevel::kInfo, "\"value\":1000,\"why\":\"changed\"") == 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(700));  // one more steady pass, no change
  QCHECK(Count(LogLevel::kInfo, "\"why\":\"changed\"") == 1);
  QCHECK(Count(LogLevel::kInfo, "never_fired") == 1);  // reported once, not every pass
  QCHECK(Count(LogLevel::kInfo, "fault_counter") == 0);  // zero faults is healthy: no never_fired
  QCHECK(Count(LogLevel::kInfo, "\"event\":\"hook_counter\"") == 3);

  // fork() while the reporter runs: the child has no reporter thread and must not block on it.
  const pid_t child = fork();
  if (child == 0) {
    StopReporter();
    _exit(0);
  }
  int wstatus = -1;
  QCHECK(child > 0 && waitpid(child, &wstatus, 0) == child && WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);

  StopReporter();  // joins
  hot.store(2000);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  QCHECK(Count(LogLevel::kInfo, "\"value\":2000") == 0);  // stopped: nothing more is reported
}

// A counter that fires for the first time after the grace window is reported at the next
// steady pass.
void ReporterReportsALateFirstChange() {
  Prepare();
  std::atomic<std::uint64_t> late{0};
  QCHECK(RegisterReportCounter("late_counter", &late));
  QCHECK(StartReporter(10, 60, 100));
  QCHECK(WaitFor([] { return Count(LogLevel::kInfo, "never_fired") == 1; }));
  late.store(3);
  QCHECK(WaitFor([] { return Count(LogLevel::kInfo, "\"value\":3,\"why\":\"first_change\"") == 1; }));
  StopReporter();
}

// ---- log lines ----------------------------------------------------------------

// Whatever a caller passes, a line is one valid JSON object: quotes, backslashes,
// control bytes and non-ASCII are escaped or replaced, and an oversized field set
// is cut at a field boundary with a marker rather than mid-escape.
void LogLinesAreValidJson() {
  Prepare();
  LogFields(LogLevel::kWarn, "json_test",
            {{"module", "we\"ird\\name\n\t\x01\xc3\xa9"}, {"count", -42}, {"huge", 9000000000LL}});
  QCHECK(Captured().size() == 1);
  const std::string& line = Captured()[0].text;
  QCHECK(line.find("\"level\":\"warn\",\"event\":\"json_test\"") != std::string::npos);
  QCHECK(line.find("\"module\":\"we\\\"ird\\\\name\\n\\t\\u0001??\"") != std::string::npos);
  QCHECK(line.find("\"count\":-42,\"huge\":9000000000}") != std::string::npos);

  Captured().clear();
  const std::string big(300, 'x');
  LogFields(LogLevel::kInfo, "json_test",
            {{"a", big.c_str()}, {"b", big.c_str()}, {"c", big.c_str()}, {"d", big.c_str()}});
  QCHECK(Captured().size() == 1);
  QCHECK(Captured()[0].text.size() < 768);
  QCHECK(Captured()[0].text.find("\"truncated\":true}") != std::string::npos);
  QCHECK(Captured()[0].text.find("\"d\":") == std::string::npos);
  char hex[19];
  QCHECK(std::string(HexString(hex, 0x36c33e8)) == "0x00000000036c33e8");
}

// ---- thunks -----------------------------------------------------------------

// thunk_exception_fixture.cpp, built with exceptions.
extern "C" int fx_throwing_original(int a, int b);
extern "C" int fx_call_catching(int (*entry)(int, int), int a, int b, int* result);

struct FaultTag {};
using FaultThunk = CallbackThunk<FaultTag, int(int, int)>;
int g_originalCalls = 0;
int CountingAdd(int a, int b) {
  ++g_originalCalls;
  return a + b;
}
int Pass(FaultThunk::Fn original, int a, int b) noexcept { return original(a, b); }
NEVR_HOOK_RECORD(kPass, FaultThunk, &Pass);

// A handler that is not declared noexcept does not convert to a Handler.
int NotNoexcept(FaultThunk::Fn original, int a, int b) { return original(a, b); }
static_assert(std::is_convertible_v<decltype(&Pass), FaultThunk::Handler>);
static_assert(!std::is_convertible_v<decltype(&NotNoexcept), FaultThunk::Handler>,
              "thunk handlers must be noexcept");
int ResetThenCallOriginal(FaultThunk::Fn original, int a, int b) noexcept {
  FaultThunk::Reset();  // clears the published original while this call is in flight
  return original(a, b);
}
NEVR_HOOK_RECORD(kResetThenCall, FaultThunk, &ResetThenCallOriginal);

struct VoidTag {};
using VoidThunk = CallbackThunk<VoidTag, void(int*)>;
int g_voidCalls = 0;
void VoidOriginal(int* p) {
  ++g_voidCalls;
  *p += 1;
}
void VoidPass(VoidThunk::Fn original, int* p) noexcept { original(p); }
NEVR_HOOK_RECORD(kVoidPass, VoidThunk, &VoidPass);

using PinnedThunk = pinned::LibR15TStringThunk;
const char* g_seenKey = nullptr;
int g_marker = 0;
const char* PinnedOriginal(const pinned::CJsonOpaque* self, const char* key, const char* fallback,
                           std::uint32_t flag) {
  QCHECK(self == reinterpret_cast<const pinned::CJsonOpaque*>(&g_marker));
  QCHECK(flag == 7u);
  g_seenKey = key;
  return fallback;
}
const char* g_replacement = "replacement";
const char* PinnedHandler(PinnedThunk::Fn original, const pinned::CJsonOpaque* self, const char* key,
                          const char* fallback, std::uint32_t flag) noexcept {
  const char* r = original(self, key, fallback, flag);
  return std::strcmp(key, "login_host") == 0 ? g_replacement : r;
}
NEVR_HOOK_RECORD(kPinnedHandler, PinnedThunk, &PinnedHandler);

void Thunks() {
  Prepare();
  FaultThunk::Reset();
  // Before the original is published: a defined default, one logged fault.
  {
    auto entry = reinterpret_cast<FaultThunk::Fn>(FaultThunk::EntryAddress());
    QCHECK(entry(1, 2) == 0);
    QCHECK(FaultThunk::Faults() == 1);
    QCHECK(Errors() == 0);  // the thunk counts a fault; it never logs on the game's call path
  }
  FaultThunk::Reset();
  Captured().clear();
  *FaultThunk::OriginalOut() = reinterpret_cast<void*>(&CountingAdd);
  auto entry = reinterpret_cast<FaultThunk::Fn>(FaultThunk::EntryAddress());
  // No handler armed: straight through.
  g_originalCalls = 0;
  QCHECK(entry(2, 3) == 5);
  QCHECK(g_originalCalls == 1 && FaultThunk::Calls() == 1 && FaultThunk::Faults() == 0);
  // The handler gets the original and the arguments, and its result is returned.
  FaultThunk::Arm(kPass);
  QCHECK(entry(4, 5) == 9 && g_originalCalls == 2);
  FaultThunk::Disarm();
  QCHECK(entry(1, 1) == 2);

  // An exception thrown by the original crosses the thunk's frames untouched, with
  // and without a handler: the catch in the exception-enabled fixture receives it,
  // the original ran once, and the thunk neither logged nor counted anything.
  FaultThunk::Reset();
  Captured().clear();
  *FaultThunk::OriginalOut() = reinterpret_cast<void*>(&fx_throwing_original);
  int result = -1;
  QCHECK(fx_call_catching(entry, 0, 0, &result) == 1);
  QCHECK(result == -1);
  FaultThunk::Arm(kPass);
  QCHECK(fx_call_catching(entry, 0, 0, &result) == 1);
  QCHECK(result == -1);
  QCHECK(FaultThunk::Calls() == 2 && FaultThunk::Faults() == 0 && Errors() == 0);

  // The in-flight call keeps the original it started with even if the published
  // pointer is cleared underneath it.
  FaultThunk::Reset();
  *FaultThunk::OriginalOut() = reinterpret_cast<void*>(&CountingAdd);
  FaultThunk::Arm(kResetThenCall);
  g_originalCalls = 0;
  QCHECK(entry(3, 4) == 7);
  QCHECK(g_originalCalls == 1 && FaultThunk::Original() == nullptr);

  // Faults are counted, never logged by the thunk.
  FaultThunk::Reset();
  Captured().clear();
  for (int i = 0; i < 5000; ++i) static_cast<void>(entry(0, 0));  // no original published
  QCHECK(FaultThunk::Faults() == 5000);
  QCHECK(Errors() == 0);

  // void signature.
  VoidThunk::Reset();
  *VoidThunk::OriginalOut() = reinterpret_cast<void*>(&VoidOriginal);
  VoidThunk::Arm(kVoidPass);
  auto voidEntry = reinterpret_cast<VoidThunk::Fn>(VoidThunk::EntryAddress());
  int counter = 0;
  voidEntry(&counter);
  QCHECK(counter == 1 && g_voidCalls == 1 && VoidThunk::Faults() == 0);
  VoidThunk::Reset();

  // The CJson::TString signature declared for the Quest libraries: arguments
  // arrive by identity, the original's return value is forwarded, and a handler
  // can replace the result.
  PinnedThunk::Reset();
  *PinnedThunk::OriginalOut() = reinterpret_cast<void*>(&PinnedOriginal);
  PinnedThunk::Arm(kPinnedHandler);
  auto tstring = reinterpret_cast<PinnedThunk::Fn>(PinnedThunk::EntryAddress());
  const auto* self = reinterpret_cast<const pinned::CJsonOpaque*>(&g_marker);
  const char* key = "matchmaker_host";
  const char* fallback = "fallback";
  QCHECK(tstring(self, key, fallback, 7u) == fallback);
  QCHECK(g_seenKey == key);
  QCHECK(tstring(self, "login_host", fallback, 7u) == g_replacement);
  PinnedThunk::Reset();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: got_hook_test <fixture-dir>\n");
    return 2;
  }
  g_dir = argv[1];
  SetLogSink(&CaptureSink);

  JumpSlotRoundTrip("libgotfx_consumer_now.so", true);
  JumpSlotRoundTrip("libgotfx_consumer_norelro.so", false);
  GlobDatRoundTrip("libgotfx_consumer_now.so");
  LazyLinkedModuleRefused();
  NegativeCases();
  DuplicateAndChained();
  ModuleUnloaded();
  ConcurrentCallers();
  SyntheticResolution();
  SyntheticInstall();
  SamePageWritesAreSerialized();
  OriginalIsPublishedBeforeTheSlotChanges();
  LiveProtectionDecidesWhatIsRestored();
  ForgetReleasesAStaleHandle();
  FailedReprotectRollsBackAndKeepsTheOriginal();
  InstallIsCompareAndSwap();
  WriteVerifyFailureKeepsTheOriginalPublished();
  ReprotectFailureDoesNotHideAnEarlierFailure();
  RollbackCompareAndSwapFailureIsLogged();
  RegistryBound();
  ArmRefusesARecordOutsideTheSection();
  PoisonedSlotIsKeptWhenMapsAreUnreadable();
  ReporterStartStopRace();
  ReporterCounterTableBound();
  ReporterIsBoundedWithAHotAndAnIdleCounter();
  ReporterReportsALateFirstChange();
  LogLinesAreValidJson();
  Thunks();

  SetLogSink(nullptr);
  QCHECK(g_invalidLines == 0);
#ifdef NEVR_TEST_HAVE_NLOHMANN
  QCHECK(g_nlohmannParsed > 100);
  std::printf("got_hook_test: %d log lines parsed with nlohmann::json\n", g_nlohmannParsed);
#else
  std::printf("got_hook_test: nlohmann::json not available on this host; strict validator only\n");
#endif
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "got_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("got_hook_test: all checks passed\n");
  return 0;
}
