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
// Run: got_hook_test <fixture-dir>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "callback_thunk.h"
#include "got_hook.h"
#include "hook_log.h"
#include "pinned_targets.h"
#include "quest/tests/test_check.h"

namespace {

using namespace sentinel;

// ---- log capture ------------------------------------------------------------

struct Line {
  LogLevel level;
  std::string text;
};
std::vector<Line>& Captured() {
  static std::vector<Line> lines;
  return lines;
}
void CaptureSink(LogLevel level, const char* line) { Captured().push_back({level, line}); }

std::size_t Count(LogLevel level, const std::string& needle) {
  std::size_t n = 0;
  for (const Line& l : Captured()) {
    if (l.level == level && l.text.find(needle) != std::string::npos) ++n;
  }
  return n;
}
std::size_t Errors() {
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

struct AddTag {};
struct SubTag {};
using AddThunk = CallbackThunk<AddTag, int(int, int)>;
using SubThunk = CallbackThunk<SubTag, int(int, int)>;

int AddPlus100(AddThunk::Fn original, int a, int b) { return original(a, b) + 100; }
int SubPlus100(SubThunk::Fn original, int a, int b) { return original(a, b) + 100; }

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

  AddThunk::Arm(&AddPlus100);
  GotHook hook;
  QCHECK_STATUS(hook.Install(target, AddThunk::EntryAddress(), AddThunk::OriginalOut()),
                GotStatus::kOk);
  QCHECK(hook.installed());
  QCHECK(AddThunk::Original() == reinterpret_cast<AddThunk::Fn>(m.realAdd));
  QCHECK(*r.slot == AddThunk::EntryAddress());
  QCHECK(PagePerms(r.slot) == before);
  QCHECK(m.callAdd(2, 3) == 105);
  QCHECK(AddThunk::Calls() == 1);
  QCHECK(Count(LogLevel::kInfo, "op=install status=ok") == 1);
  QCHECK(Errors() == 0);

  QCHECK_STATUS(hook.Remove(), GotStatus::kOk);
  QCHECK(!hook.installed());
  QCHECK(*r.slot == m.realAdd);
  QCHECK(PagePerms(r.slot) == before);
  QCHECK(m.callAdd(2, 3) == 5);
  QCHECK(Count(LogLevel::kInfo, "op=remove status=ok") == 1);
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
  SubThunk::Arm(&SubPlus100);
  GotHook hook;
  GotTarget target{soname, "fx_sub", RelocKind::kGlobDat};
  QCHECK_STATUS(hook.Install(target, SubThunk::EntryAddress(), SubThunk::OriginalOut()),
                GotStatus::kOk);
  QCHECK(m.callSub(5, 3) == 102);
  QCHECK_STATUS(hook.Remove(), GotStatus::kOk);
  QCHECK(m.callSub(5, 3) == 2);
  dlclose(m.handle);
}

void LazyBindingRefused() {
  Prepare();
  Module m = Open("libgotfx_consumer_lazy.so");
  const SlotResolution probe = Resolve(m, "fx_add", RelocKind::kJumpSlot);
  QCHECK_STATUS(probe.status, GotStatus::kLazyBinding);
  GotHook hook;
  void* sentinelValue = reinterpret_cast<void*>(0x1234);
  void* out = sentinelValue;
  GotTarget target{"libgotfx_consumer_lazy.so", "fx_add", RelocKind::kJumpSlot};
  QCHECK_STATUS(hook.Install(target, AddThunk::EntryAddress(), &out), GotStatus::kLazyBinding);
  QCHECK(out == sentinelValue);
  QCHECK(Errors() == 1 && Count(LogLevel::kError, "status=lazy_binding") == 1);
  QCHECK(m.callAdd(2, 3) == 5);
  // GLOB_DAT slots are resolved at load time even in a lazy module.
  GotTarget glob{"libgotfx_consumer_lazy.so", "fx_sub", RelocKind::kGlobDat};
  SubThunk::Arm(&SubPlus100);
  GotHook globHook;
  QCHECK_STATUS(globHook.Install(glob, SubThunk::EntryAddress(), SubThunk::OriginalOut()),
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
    const GotStatus status = hook.Install(c.target, AddThunk::EntryAddress(), &out);
    if (status != c.expected) std::fprintf(stderr, "case: %s\n", c.name);
    QCHECK_STATUS(status, c.expected);
    QCHECK(!hook.installed());
    QCHECK(*addSlot.slot == before);
    QCHECK(out == sentinelValue);
    QCHECK(Errors() == 1);
    QCHECK(Count(LogLevel::kError, std::string("status=") + GotStatusName(c.expected)) == 1);
  }
  // build-id mismatch names both ids
  Captured().clear();
  GotHook hook;
  void* out = nullptr;
  QCHECK_STATUS(hook.Install(wrongBuildId, AddThunk::EntryAddress(), &out), GotStatus::kBuildIdMismatch);
  QCHECK(Count(LogLevel::kError, "expected=0000000000000000000000000000000000000000 actual=") == 1);

  GotHook nullFn;
  QCHECK_STATUS(nullFn.Install(GotTarget{so, "fx_add", RelocKind::kJumpSlot}, nullptr, &out),
                GotStatus::kBadArgument);

  // The right pin and the right expected original install.
  GotTarget exact{so, "fx_add", RelocKind::kJumpSlot, nullptr, addSlot.slotVaddr, m.realAdd};
  AddThunk::Arm(&AddPlus100);
  GotHook good;
  QCHECK_STATUS(good.Install(exact, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);
  QCHECK_STATUS(good.Remove(), GotStatus::kOk);
  dlclose(m.handle);
}

void DuplicateAndChained() {
  Prepare();
  Module m = Open("libgotfx_consumer_now.so");
  const char* so = "libgotfx_consumer_now.so";
  GotTarget target{so, "fx_add", RelocKind::kJumpSlot};
  const SlotResolution r = Resolve(m, "fx_add", RelocKind::kJumpSlot);
  AddThunk::Arm(&AddPlus100);
  GotHook first;
  QCHECK_STATUS(first.Install(target, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);

  QCHECK_STATUS(first.Install(target, AddThunk::EntryAddress(), AddThunk::OriginalOut()),
                GotStatus::kAlreadyInstalled);
  GotHook second;
  void* out = nullptr;
  QCHECK_STATUS(second.Install(target, AddThunk::EntryAddress(), &out), GotStatus::kAlreadyInstalled);
  QCHECK(out == nullptr);
  // A different hook function reaches the slot registry rather than the value check.
  QCHECK_STATUS(second.Install(target, reinterpret_cast<void*>(&AddImpl), &out),
                GotStatus::kAlreadyInstalled);
  QCHECK(out == nullptr);
  QCHECK(m.callAdd(2, 3) == 105);

  // Someone else overwrote the slot: Remove refuses to write over them.
  ForceWrite(r.slot, m.realSub);
  Captured().clear();
  QCHECK_STATUS(first.Remove(), GotStatus::kSlotChanged);
  QCHECK(first.installed());
  QCHECK(*r.slot == m.realSub);
  QCHECK(Count(LogLevel::kError, "op=remove status=slot_changed") == 1);
  ForceWrite(r.slot, AddThunk::EntryAddress());
  QCHECK_STATUS(first.Remove(), GotStatus::kOk);
  QCHECK(*r.slot == m.realAdd);
  // The slot is free again for a new handle.
  QCHECK_STATUS(second.Install(target, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);
  QCHECK_STATUS(second.Remove(), GotStatus::kOk);
  dlclose(m.handle);
}

void ModuleUnloaded() {
  Prepare();
  Module m = Open("libgotfx_consumer_now.so");
  GotTarget target{"libgotfx_consumer_now.so", "fx_add", RelocKind::kJumpSlot};
  AddThunk::Arm(&AddPlus100);
  GotHook hook;
  QCHECK_STATUS(hook.Install(target, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);
  QCHECK(dlclose(m.handle) == 0);
  Captured().clear();
  QCHECK_STATUS(hook.Remove(), GotStatus::kModuleChanged);
  QCHECK(!hook.installed());
  QCHECK(Count(LogLevel::kError, "op=remove status=module_changed") == 1);
}

void ConcurrentCallers() {
  Prepare();
  Module m = Open("libgotfx_consumer_now.so");
  GotTarget target{"libgotfx_consumer_now.so", "fx_add", RelocKind::kJumpSlot};
  AddThunk::Arm(&AddPlus100);
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
    QCHECK_STATUS(hook.Install(target, AddThunk::EntryAddress(), AddThunk::OriginalOut()), GotStatus::kOk);
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
  for (std::uint64_t slot : {kRelroSlot, kRelroSlot + 8, kRelroSlot + 16, kPlainSlot}) {
    put(slot, &originalValue, sizeof(originalValue));
  }

  if (spec.readOnlyBacking) {
    // Written through a read-write descriptor, mapped through a read-only one:
    // the mapping then cannot be given PROT_WRITE (mprotect fails with EACCES).
    char path[] = "/var/tmp/work-nevr-runtime/claude-main/hooks/host/synthXXXXXX";
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
    QCHECK_STATUS(hook.Install(target, reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                  GotStatus::kOk);
    QCHECK(out == reinterpret_cast<void*>(&AddImpl));
    QCHECK(*slot == reinterpret_cast<void*>(&SubImpl));
    QCHECK(PagePerms(slot) == perms);
    QCHECK_STATUS(hook.Remove(), GotStatus::kOk);
    QCHECK(*slot == reinterpret_cast<void*>(&AddImpl));
    QCHECK(PagePerms(slot) == perms);

    // The module moves: Remove must not write into the old mapping.
    QCHECK_STATUS(hook.Install(target, reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
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
    QCHECK_STATUS(hook.Install(SynthTarget(), reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                  GotStatus::kOriginalImplausible);
    QCHECK(Count(LogLevel::kError, "status=original_implausible") == 1);
    g_synth = nullptr;
    int dataObject = 0;
    SynthImage img2;
    Build(spec, &dataObject, &img2);
    g_synth = &img2;
    QCHECK_STATUS(hook.Install(SynthTarget(), reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
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
      QCHECK_STATUS(hook.Install(target, reinterpret_cast<void*>(&SubImpl), &out, SynthLookup),
                    GotStatus::kProtectFailed);
      QCHECK(out == sentinelValue);
      QCHECK(*slot == reinterpret_cast<void*>(&AddImpl));
      QCHECK(Errors() == 1);
      QCHECK(Count(LogLevel::kError, "status=protect_failed stage=enable_failed") == 1);
      QCHECK(Count(LogLevel::kError, "errno=13") == 1);
    }
    g_synth = nullptr;
  }
}

// ---- thunks -----------------------------------------------------------------

struct FaultTag {};
using FaultThunk = CallbackThunk<FaultTag, int(int, int)>;
int g_originalCalls = 0;
int CountingAdd(int a, int b) {
  ++g_originalCalls;
  return a + b;
}
int Throwing(FaultThunk::Fn, int, int) { throw std::runtime_error("handler failure"); }
int Pass(FaultThunk::Fn original, int a, int b) { return original(a, b); }

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
                          const char* fallback, std::uint32_t flag) {
  const char* r = original(self, key, fallback, flag);
  return std::strcmp(key, "login_host") == 0 ? g_replacement : r;
}

void Thunks() {
  Prepare();
  FaultThunk::Reset();
  // Before the original is published: a defined default, one logged fault.
  {
    auto entry = reinterpret_cast<FaultThunk::Fn>(FaultThunk::EntryAddress());
    QCHECK(entry(1, 2) == 0);
    QCHECK(FaultThunk::Faults() == 1);
    QCHECK(Count(LogLevel::kError, "event=callback_thunk status=no_original") == 1);
  }
  FaultThunk::Reset();
  Captured().clear();
  *FaultThunk::OriginalOut() = reinterpret_cast<void*>(&CountingAdd);
  auto entry = reinterpret_cast<FaultThunk::Fn>(FaultThunk::EntryAddress());
  // No handler armed: straight through.
  g_originalCalls = 0;
  QCHECK(entry(2, 3) == 5);
  QCHECK(g_originalCalls == 1 && FaultThunk::Calls() == 1 && FaultThunk::Faults() == 0);
  // Handler gets the original and the arguments, and its result is returned.
  FaultThunk::Arm(&Pass);
  QCHECK(entry(4, 5) == 9 && g_originalCalls == 2);
  // A handler that throws never unwinds into the caller: logged, counted, original runs.
  FaultThunk::Arm(&Throwing);
  QCHECK(entry(6, 7) == 13);
  QCHECK(g_originalCalls == 3 && FaultThunk::Faults() == 1);
  QCHECK(Count(LogLevel::kError, "status=handler_threw action=call_original faults=1") == 1);
  // Disarm restores pass-through.
  FaultThunk::Arm(nullptr);
  QCHECK(entry(1, 1) == 2);
  // Faults are counted always and logged first, then one in 4096.
  Captured().clear();
  FaultThunk::Arm(&Throwing);
  for (int i = 0; i < 5000; ++i) (void)entry(0, 0);
  QCHECK(FaultThunk::Faults() == 5001);
  QCHECK(Count(LogLevel::kError, "status=handler_threw") == 1);  // fault #4096 is the only one

  // The CJson::TString signature declared for the Quest libraries: arguments
  // arrive by identity, the original's return value is forwarded, and a handler
  // can replace the result.
  PinnedThunk::Reset();
  *PinnedThunk::OriginalOut() = reinterpret_cast<void*>(&PinnedOriginal);
  PinnedThunk::Arm(&PinnedHandler);
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
  LazyBindingRefused();
  NegativeCases();
  DuplicateAndChained();
  ModuleUnloaded();
  ConcurrentCallers();
  SyntheticResolution();
  SyntheticInstall();
  Thunks();

  SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "got_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("got_hook_test: all checks passed\n");
  return 0;
}
