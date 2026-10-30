// Host test for meshstep (src/quest/plugins/q15t/src/meshstep.cpp): a fake game image at the pinned ELF offsets, the pinned words
// written at their vaddrs, the GOT word holding base + the constructor, and fake helpers recording the calls the stock order makes.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "meshstep.h"
#include "module_switch.h"

namespace {

int g_failures = 0;
#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::fprintf(stderr, "meshstep_test FAIL %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      ++g_failures;                                                   \
    }                                                                 \
  } while (0)

constexpr std::size_t kImageBytes = 0x4000000;  // 64 MiB: the vtable words sit near 0x371f860
constexpr std::uintptr_t kTableOffsets[] = {0x2b0, 0x2e8, 0x320, 0x358, 0x390, 0x3c8, 0x400, 0x438, 0x470, 0x4a8, 0x4e0};

// Fake helpers: record what the reconstruction asks for.
unsigned g_list_calls = 0;
void* g_list_self = nullptr;
unsigned g_block_calls = 0;
void* g_block_self[11] = {};
std::uint64_t g_block_a[11] = {};
std::uint64_t g_block_b[11] = {};
void* g_block_alloc[11] = {};
constexpr std::uint64_t kCurrent = 0xC0FFEE;
void* const kCurrentPtr = reinterpret_cast<void*>(0xA110C);

std::uint64_t FakeCurrent() { return kCurrent; }
void* FakeCurrentPtr() { return kCurrentPtr; }
void FakeListDataCtor(void* self) {
  ++g_list_calls;
  g_list_self = self;
}
void FakeMemBlockCtor(void* self, std::uint64_t a, std::uint64_t b, void* alloc) {
  if (g_block_calls < 11) {
    g_block_self[g_block_calls] = self;
    g_block_a[g_block_calls] = a;
    g_block_b[g_block_calls] = b;
    g_block_alloc[g_block_calls] = alloc;
  }
  ++g_block_calls;
}

q15t::MeshStepFns FakeFns() {
  q15t::MeshStepFns f{};
  f.current = &FakeCurrent;
  f.current_ptr = &FakeCurrentPtr;
  f.list_data_ctor = &FakeListDataCtor;
  f.memblock_ctor = &FakeMemBlockCtor;
  return f;
}

void ResetFakes() {
  g_list_calls = 0;
  g_list_self = nullptr;
  g_block_calls = 0;
}

void PutWord(std::uint8_t* img, std::uintptr_t va, std::uint32_t w) { std::memcpy(img + va, &w, sizeof w); }

std::uintptr_t GetWord(const std::uint8_t* img, std::uintptr_t va) {
  std::uintptr_t v = 0;
  std::memcpy(&v, img + va, sizeof v);
  return v;
}

void PutPtr(std::uint8_t* img, std::uintptr_t va, std::uintptr_t v) { std::memcpy(img + va, &v, sizeof v); }

// The pinned words, the vtable words and the GOT word, as the stock game image holds them.
void BuildImage(std::uint8_t* img, std::uintptr_t base) {
  for (const q15t::WordPin& p : q15t::kMeshStepWordPins) PutWord(img, p.va, p.want);
  PutPtr(img, q15t::kMeshStepPinned.vtbl_presence, 0x1000);  // stored word; the reconstruction adds 0x10
  PutPtr(img, q15t::kMeshStepPinned.vtbl_mesh, 0x2000);
  PutPtr(img, q15t::kMeshStepPinned.got_slot, base + q15t::kMeshStepPinned.ctor);
}

}  // namespace

int main() {
  // Not the real directory: the kill file never exists here.
  q15t::SetModuleSwitchDirForTest("/nonexistent-meshstep-test-dir");
  auto* img = static_cast<std::uint8_t*>(std::calloc(1, kImageBytes));
  CHECK(img != nullptr, "fake image allocated");
  if (img == nullptr) return 1;
  const auto base = reinterpret_cast<std::uintptr_t>(img);
  const q15t::MeshStepAddrs& a = q15t::kMeshStepPinned;

  // 1. Happy path: installed, the slot holds the wrapper, and the wrapper builds the object in the stock order with step 1.
  q15t::ResetMeshStepForTest();
  ResetFakes();
  BuildImage(img, base);
  q15t::InstallResult r = q15t::InstallMeshStepAt(base, a, FakeFns());
  CHECK(r.status == q15t::SwapStatus::kOk, "install status ok");
  CHECK(std::strcmp(r.reason, "") == 0, "install reason empty");
  const std::uintptr_t wrapper = GetWord(img, a.got_slot);
  CHECK(wrapper != base + a.ctor, "slot no longer holds the stock constructor");
  auto build = reinterpret_cast<void (*)(void*, const void*)>(wrapper);

  auto* obj = static_cast<std::uint8_t*>(std::calloc(1, 0x800));
  CHECK(obj != nullptr, "object allocated");
  if (obj == nullptr) return 1;
  std::uint64_t init[3] = {0, 0x1111, 0x2222};  // CPresenceInit: +8 and +0x10 are copied
  build(obj, init);

  CHECK(GetWord(obj, 0x00) == 0x2010, "vtable is the mesh vtable (stored word 0x2000 + 0x10), set last");
  CHECK(GetWord(obj, 0x08) == 0, "+0x08 zero");
  CHECK(GetWord(obj, 0x10) == 0 && GetWord(obj, 0x18) == 0, "+0x10 and +0x18 zero");
  CHECK(GetWord(obj, 0x20) == 0x1111, "+0x20 copied from init+8");
  CHECK(GetWord(obj, 0x28) == 0x2222, "+0x28 copied from init+0x10");
  CHECK(GetWord(obj, 0x30) == kCurrent, "+0x30 is CMemoryContext::Current()");
  CHECK(GetWord(obj, 0x38) == ~std::uintptr_t{0}, "+0x38 is -1");
  CHECK(GetWord(obj, 0x40) == 0 && GetWord(obj, 0x48) == 0 && GetWord(obj, 0x50) == 0 && GetWord(obj, 0x58) == 0,
        "+0x40..+0x58 zero");
  CHECK(GetWord(obj, 0x60) == 7 && GetWord(obj, 0x68) == 0, "+0x60 is 7, +0x68 zero (the stock q1 constant)");
  CHECK(g_list_calls == 1 && g_list_self == obj + 0x70, "CGMeshListData constructed once at +0x70");
  CHECK(g_block_calls == 11, "eleven CMemBlock constructions");
  for (unsigned i = 0; i < 11 && i < g_block_calls; ++i) {
    CHECK(g_block_self[i] == obj + kTableOffsets[i], "CMemBlock at its stock offset");
    CHECK(g_block_a[i] == 0 && g_block_b[i] == 0, "CMemBlock gets 0, 0");
    CHECK(g_block_alloc[i] == kCurrentPtr, "CMemBlock gets CurrentPtr()");
    CHECK(GetWord(obj, kTableOffsets[i] + 0x20) == 1, "growth step is 1 (stock: 32)");
    CHECK(GetWord(obj, kTableOffsets[i] + 0x28) == 0, "+0x28 of the block zero");
    CHECK(GetWord(obj, kTableOffsets[i] + 0x30) == 0, "+0x30 of the block zero");
  }
  CHECK(q15t::GetMeshStepStats().builds == 1, "one build counted");

  // 2. A pin that does not read back refuses the module: nothing swapped.
  q15t::ResetMeshStepForTest();
  ResetFakes();
  BuildImage(img, base);
  PutWord(img, 0x186784c, 0x52800034);  // mov w20,#1 in the image: not the stock word
  r = q15t::InstallMeshStepAt(base, a, FakeFns());
  CHECK(r.status != q15t::SwapStatus::kOk, "a mismatched pin is refused");
  CHECK(std::strcmp(r.reason, "word_pin_mismatch") == 0, "refusal reason is word_pin_mismatch");
  CHECK(GetWord(img, a.got_slot) == base + a.ctor, "slot untouched after a refused pin");

  // 3. A zero vtable word refuses the module: nothing swapped.
  BuildImage(img, base);
  PutPtr(img, a.vtbl_mesh, 0);
  r = q15t::InstallMeshStepAt(base, a, FakeFns());
  CHECK(std::strcmp(r.reason, "vtable_unreadable") == 0, "zero vtable refused");
  CHECK(GetWord(img, a.got_slot) == base + a.ctor, "slot untouched after a refused vtable");

  // 4. A missing helper refuses the module before anything is read or swapped.
  BuildImage(img, base);
  q15t::MeshStepFns missing = FakeFns();
  missing.memblock_ctor = nullptr;
  r = q15t::InstallMeshStepAt(base, a, missing);
  CHECK(std::strcmp(r.reason, "helper_missing") == 0, "missing helper refused");
  CHECK(GetWord(img, a.got_slot) == base + a.ctor, "slot untouched after a missing helper");

  std::free(obj);
  std::free(img);
  if (g_failures != 0) {
    std::fprintf(stderr, "meshstep_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("meshstep_test: all checks passed\n");
  return 0;
}
