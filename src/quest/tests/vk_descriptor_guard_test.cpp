// Host test for the descriptor guard (src/quest/sentinel/vk_descriptor_guard.cpp, #1737 row G6A-030), driven the way the
// game reaches it: CSysModule::FindMethod("vkGetInstanceProcAddr") through vk_ext_window's thunk entry, then
// vkGetInstanceProcAddr("vkGetDeviceProcAddr"), then vkGetDeviceProcAddr for the two descriptor functions, against a fake
// driver that records every vkUpdateDescriptorSets write. No GOT slot is touched.
//
// The central case is the fault the guard exists for: a layout with uniform texel buffer bindings 3 and 4 (the light-UV
// texel buffers) gets every new set's two bindings written with the guard's zeroed view before the game sees the set.
//
// Built and run by `just test-quest-hooks`.

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../plugins/q15t/src/module_switch.h"
#include "hook_log.h"
#include "quest/tests/test_check.h"
#include "vk_descriptor_guard.h"
#include "vk_ext_window.h"

namespace {

namespace vk = nevr_quest::vk_window;
namespace g = nevr_quest::vk_guard;
using VoidFn = void (*)();
using GipaFn = VoidFn (*)(void*, const char*);
using GdpaFn = VoidFn (*)(void*, const char*);
using CreateDslFn = std::int32_t (*)(void*, const g::DslCreateInfo*, const void*, std::uint64_t*);
using AllocSetsFn = std::int32_t (*)(void*, const g::SetAllocateInfo*, std::uint64_t*);

// ---- log capture -------------------------------------------------------------------------------------------------------

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

std::vector<std::string> LinesFor(const char* event) {
  std::vector<std::string> out;
  const std::string needle = std::string("\"event\":\"") + event + "\"";
  for (const std::string& l : g_lines) {
    if (l.find(needle) != std::string::npos) out.push_back(l);
  }
  return out;
}

std::string Field(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t at = line.find(needle);
  if (at == std::string::npos) return "<absent>";
  std::size_t from = at + needle.size();
  if (from < line.size() && line[from] == '"') {
    const std::size_t end = line.find('"', from + 1);
    return line.substr(from + 1, end == std::string::npos ? std::string::npos : end - from - 1);
  }
  const std::size_t end = line.find_first_of(",}", from);
  return line.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

// ---- a driver ---------------------------------------------------------------------------------------------------------

void* const kInstance = reinterpret_cast<void*>(0x10);
void* const kDevice = reinterpret_cast<void*>(0x20);
void* const kOtherDevice = reinterpret_cast<void*>(0x21);
void* const kPhysical = reinterpret_cast<void*>(0x30);
constexpr std::uint64_t kBuffer = 0xB0;
constexpr std::uint64_t kMemory = 0xC0;
constexpr std::uint64_t kView = 0xD0;

struct Written {
  std::uint64_t set;
  std::uint32_t binding;
  std::uint32_t count;
  std::uint32_t type;
  std::vector<std::uint64_t> views;
};
std::vector<Written> g_written;
int g_updateCalls = 0;
int g_createBufferCalls = 0;
std::int32_t g_allocResult = 0;
std::uint64_t g_nextLayout = 0x1000;
std::uint64_t g_reuseLayout = 0;  // nonzero: the next layout create returns this handle
std::uint64_t g_nextSet = 0x5000;
std::uint32_t g_memoryFlags = g::kMemoryHostVisible | g::kMemoryHostCoherent;
alignas(16) unsigned char g_mapped[65536];

std::int32_t FakeCreateDsl(void*, const g::DslCreateInfo*, const void*, std::uint64_t* out) {
  *out = g_reuseLayout != 0 ? g_reuseLayout : g_nextLayout++;
  g_reuseLayout = 0;
  return 0;
}
std::int32_t FakeAllocSets(void*, const g::SetAllocateInfo* ai, std::uint64_t* out) {
  if (g_allocResult != 0) return g_allocResult;
  for (std::uint32_t i = 0; i < ai->descriptorSetCount; ++i) out[i] = g_nextSet++;
  return 0;
}
void FakeUpdateSets(void*, std::uint32_t n, const g::WriteDescriptorSet* w, std::uint32_t, const void*) {
  ++g_updateCalls;
  for (std::uint32_t i = 0; i < n; ++i) {
    Written x{w[i].dstSet, w[i].dstBinding, w[i].descriptorCount, w[i].descriptorType, {}};
    for (std::uint32_t k = 0; k < w[i].descriptorCount; ++k) x.views.push_back(w[i].pTexelBufferView[k]);
    g_written.push_back(x);
  }
}
std::int32_t FakeCreateBuffer(void*, const g::BufferCreateInfo* ci, const void*, std::uint64_t* out) {
  ++g_createBufferCalls;
  QCHECK((ci->usage & g::kBufferUsageUniformTexelBuffer) != 0);
  *out = kBuffer;
  return 0;
}
void FakeBufferReqs(void*, std::uint64_t, g::MemoryRequirements* mr) {
  mr->size = 65536;
  mr->alignment = 256;
  mr->memoryTypeBits = 0x6;  // types 1 and 2
}
std::int32_t FakeAllocMemory(void*, const g::MemoryAllocateInfo*, const void*, std::uint64_t* out) {
  *out = kMemory;
  return 0;
}
std::int32_t FakeBindMemory(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return 0; }
std::int32_t FakeMapMemory(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint32_t, void** out) {
  std::memset(g_mapped, 0x5a, sizeof g_mapped);
  *out = g_mapped;
  return 0;
}
std::int32_t FakeCreateView(void*, const g::BufferViewCreateInfo* ci, const void*, std::uint64_t* out) {
  QCHECK(ci->buffer == kBuffer);
  QCHECK(ci->format == g::kFormatR32G32B32A32Sfloat);
  *out = kView;
  return 0;
}
std::int32_t FakeEnumPd(void*, std::uint32_t* count, void** out) {
  if (out == nullptr) {
    *count = 1;
    return 0;
  }
  out[0] = kPhysical;
  *count = 1;
  return 0;
}
void FakeMemProps(void*, g::PhysicalDeviceMemoryProperties* mp) {
  mp->memoryTypeCount = 3;
  mp->memoryTypes[0].propertyFlags = g_memoryFlags;  // not allowed by the buffer's bits
  mp->memoryTypes[1].propertyFlags = 0x1;           // device local only
  mp->memoryTypes[2].propertyFlags = g_memoryFlags;
}
void FakeOther() {}

VoidFn FakeGetDeviceProcAddr(void*, const char* name);
#define NAME_IS(n) (std::strcmp(name, n) == 0)
VoidFn FakeGetDeviceProcAddr(void*, const char* name) {
  if (NAME_IS("vkGetDeviceProcAddr")) return reinterpret_cast<VoidFn>(&FakeGetDeviceProcAddr);
  if (NAME_IS("vkCreateDescriptorSetLayout")) return reinterpret_cast<VoidFn>(&FakeCreateDsl);
  if (NAME_IS("vkAllocateDescriptorSets")) return reinterpret_cast<VoidFn>(&FakeAllocSets);
  if (NAME_IS("vkUpdateDescriptorSets")) return reinterpret_cast<VoidFn>(&FakeUpdateSets);
  if (NAME_IS("vkCreateBuffer")) return reinterpret_cast<VoidFn>(&FakeCreateBuffer);
  if (NAME_IS("vkGetBufferMemoryRequirements")) return reinterpret_cast<VoidFn>(&FakeBufferReqs);
  if (NAME_IS("vkAllocateMemory")) return reinterpret_cast<VoidFn>(&FakeAllocMemory);
  if (NAME_IS("vkBindBufferMemory")) return reinterpret_cast<VoidFn>(&FakeBindMemory);
  if (NAME_IS("vkMapMemory")) return reinterpret_cast<VoidFn>(&FakeMapMemory);
  if (NAME_IS("vkCreateBufferView")) return reinterpret_cast<VoidFn>(&FakeCreateView);
  if (NAME_IS("vkCmdDraw")) return reinterpret_cast<VoidFn>(&FakeOther);
  return nullptr;
}
VoidFn FakeGetInstanceProcAddr(void*, const char* name) {
  if (NAME_IS("vkGetDeviceProcAddr")) return reinterpret_cast<VoidFn>(&FakeGetDeviceProcAddr);
  if (NAME_IS("vkEnumeratePhysicalDevices")) return reinterpret_cast<VoidFn>(&FakeEnumPd);
  if (NAME_IS("vkGetPhysicalDeviceMemoryProperties")) return reinterpret_cast<VoidFn>(&FakeMemProps);
  return nullptr;
}
#undef NAME_IS

void* RealFindMethod(unsigned long module, const char* name) {
  if (module == 0 || name == nullptr) return nullptr;
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<void*>(&FakeGetInstanceProcAddr);
  return nullptr;
}
std::int32_t RealDeviceExtensions(char* names, std::uint32_t* size) {
  if (*size > 0) names[0] = '\0';
  return 0;
}

std::string g_switchDir;

void Reset() {
  g_lines.clear();
  g_written.clear();
  g_updateCalls = g_createBufferCalls = 0;
  g_allocResult = 0;
  g_nextLayout = 0x1000;
  g_reuseLayout = 0;
  g_nextSet = 0x5000;
  g_memoryFlags = g::kMemoryHostVisible | g::kMemoryHostCoherent;
  std::remove((g_switchDir + "/g40guard.off").c_str());
  g::ResetForTest();
  vk::ResetForTest();
  vk::FindMethodThunk::Reset();
  vk::DeviceExtensionsThunk::Reset();
  *vk::FindMethodThunk::OriginalOut() = reinterpret_cast<void*>(&RealFindMethod);
  *vk::DeviceExtensionsThunk::OriginalOut() = reinterpret_cast<void*>(&RealDeviceExtensions);
  vk::ArmForTest();
}

// The game's resolution chain: FindMethod -> vkGetInstanceProcAddr -> vkGetDeviceProcAddr -> the device functions.
struct Game {
  GdpaFn gdpa = nullptr;
  CreateDslFn create = nullptr;
  AllocSetsFn alloc = nullptr;
};
Game Resolve() {
  Game game;
  auto findMethod = reinterpret_cast<vk::FindMethodThunk::Fn>(vk::FindMethodThunk::EntryAddress());
  auto gipa = reinterpret_cast<GipaFn>(findMethod(7, "vkGetInstanceProcAddr"));
  if (gipa == nullptr) return game;
  game.gdpa = reinterpret_cast<GdpaFn>(gipa(kInstance, "vkGetDeviceProcAddr"));
  if (game.gdpa == nullptr) return game;
  game.create = reinterpret_cast<CreateDslFn>(game.gdpa(kDevice, "vkCreateDescriptorSetLayout"));
  game.alloc = reinterpret_cast<AllocSetsFn>(game.gdpa(kDevice, "vkAllocateDescriptorSets"));
  return game;
}

// set 0 of the light-UV vertex shader: a uniform buffer at 0, two uniform texel buffers at 3 and 4, a sampler at 5.
std::uint64_t LightUvLayout(const Game& game, const void* pNext = nullptr) {
  const g::DslBinding b[] = {{0, 6, 1, 1, nullptr}, {3, 4, 1, 1, nullptr}, {4, 4, 1, 1, nullptr}, {5, 1, 1, 0x10, nullptr}};
  g::DslCreateInfo ci{32, pNext, 0, 4, b};
  std::uint64_t out = 0;
  QCHECK(game.create(kDevice, &ci, nullptr, &out) == 0);
  return out;
}
std::uint64_t PlainLayout(const Game& game) {
  const g::DslBinding b[] = {{0, 6, 1, 1, nullptr}, {1, 1, 2, 0x10, nullptr}};
  g::DslCreateInfo ci{32, nullptr, 0, 2, b};
  std::uint64_t out = 0;
  QCHECK(game.create(kDevice, &ci, nullptr, &out) == 0);
  return out;
}
std::int32_t Allocate(const Game& game, void* device, std::vector<std::uint64_t> layouts, std::vector<std::uint64_t>* sets) {
  sets->assign(layouts.size(), 0);
  g::SetAllocateInfo ai{34, nullptr, 0x77, static_cast<std::uint32_t>(layouts.size()), layouts.data()};
  return game.alloc(device, &ai, sets->data());
}

// ---- the cases --------------------------------------------------------------------------------------------------------

void TheLightUvBindingsOfEveryNewSetGetTheZeroedView() {
  Reset();
  const Game game = Resolve();
  QCHECK(game.gdpa != reinterpret_cast<GdpaFn>(&FakeGetDeviceProcAddr));
  QCHECK(game.create != reinterpret_cast<CreateDslFn>(&FakeCreateDsl));
  QCHECK(game.alloc != reinterpret_cast<AllocSetsFn>(&FakeAllocSets));
  const std::uint64_t layout = LightUvLayout(game);
  std::vector<std::uint64_t> sets;
  QCHECK(Allocate(game, kDevice, {layout, layout}, &sets) == 0);
  QCHECK(sets[0] == 0x5000 && sets[1] == 0x5001);  // the game's sets, as the driver made them
  QCHECK(g_written.size() == 4);
  for (std::size_t i = 0; i < g_written.size() && i < 4; ++i) {
    QCHECK(g_written[i].set == sets[i / 2]);
    QCHECK(g_written[i].binding == (i % 2 == 0 ? 3u : 4u));
    QCHECK(g_written[i].type == g::kDescriptorTypeUniformTexelBuffer);
    QCHECK(g_written[i].views.size() == 1 && g_written[i].views[0] == kView);
  }
  for (unsigned char c : g_mapped) {
    if (c != 0) {
      QCHECK(c == 0);  // the buffer the view reads is zeroed
      break;
    }
  }
  QCHECK(g_createBufferCalls == 1);
  QCHECK(Allocate(game, kDevice, {layout}, &sets) == 0);
  QCHECK(g_createBufferCalls == 1);  // one buffer for the whole run
  QCHECK(g_written.size() == 6);

  const auto install = LinesFor("q15t_g40guard_install");
  QCHECK(install.size() == 1);
  if (!install.empty()) {
    QCHECK(Field(install[0], "result") == "on");
    QCHECK(Field(install[0], "reason") == "default_on");
  }
  QCHECK(LinesFor("g40guard_hook").size() == 2);
  const auto buffer = LinesFor("g40guard_buffer");
  QCHECK(buffer.size() == 1);
  if (!buffer.empty()) {
    QCHECK(Field(buffer[0], "result") == "made");
    QCHECK(Field(buffer[0], "memory_type") == "2");  // host visible + coherent and allowed by the buffer
  }
  const auto fill = LinesFor("g40guard_fill");
  QCHECK(fill.size() == 1);  // the first call crosses 1 and 2 (one line, sets_filled 2); the second reaches 3: no line
  if (fill.size() == 1) {
    QCHECK(Field(fill[0], "sets_filled") == "2");
    QCHECK(Field(fill[0], "writes") == "4");
  }
  const g::Stats s = g::GetStats();
  QCHECK(s.on && s.buffer_ok && s.sets_filled == 3 && s.writes == 6 && s.layouts == 1);
}

void TheOffFileLeavesTheDriversFunctions() {
  Reset();
  FILE* f = std::fopen((g_switchDir + "/g40guard.off").c_str(), "w");
  QCHECK(f != nullptr);
  if (f != nullptr) std::fclose(f);
  const Game game = Resolve();
  QCHECK(game.gdpa == reinterpret_cast<GdpaFn>(&FakeGetDeviceProcAddr));
  QCHECK(game.alloc == reinterpret_cast<AllocSetsFn>(&FakeAllocSets));
  const auto install = LinesFor("q15t_g40guard_install");
  QCHECK(install.size() == 1);
  if (!install.empty()) {
    QCHECK(Field(install[0], "result") == "off");
    QCHECK(Field(install[0], "reason") == "off_file");
  }
  QCHECK(LinesFor("g40guard_hook").empty());
}

void OtherNamesAndTheSelfLookupPassThrough() {
  Reset();
  const Game game = Resolve();
  QCHECK(game.gdpa(kDevice, "vkCmdDraw") == reinterpret_cast<VoidFn>(&FakeOther));
  QCHECK(game.gdpa(kDevice, "vkNoSuchFunction") == nullptr);
  QCHECK(game.gdpa(kDevice, "vkGetDeviceProcAddr") == reinterpret_cast<VoidFn>(game.gdpa));
  QCHECK(game.gdpa(kDevice, "vkAllocateDescriptorSetsX") == nullptr);  // whole names only
}

void AnUntouchedLayoutCostsNoBufferAndNoWrite() {
  Reset();
  const Game game = Resolve();
  const std::uint64_t layout = PlainLayout(game);
  std::vector<std::uint64_t> sets;
  QCHECK(Allocate(game, kDevice, {layout}, &sets) == 0);
  QCHECK(g_written.empty() && g_updateCalls == 0 && g_createBufferCalls == 0);
  QCHECK(LinesFor("g40guard_buffer").empty());
}

void ABindingFlagsLayoutIsLeftAlone() {
  Reset();
  const Game game = Resolve();
  const g::BaseIn flags{g::kStypeBindingFlagsCreateInfo, nullptr};
  const g::BaseIn other{1000000000, &flags};
  const std::uint64_t layout = LightUvLayout(game, &other);
  std::vector<std::uint64_t> sets;
  QCHECK(Allocate(game, kDevice, {layout}, &sets) == 0);
  QCHECK(g_written.empty());
}

void AFailedAllocationIsReturnedAndWritesNothing() {
  Reset();
  const Game game = Resolve();
  const std::uint64_t layout = LightUvLayout(game);
  g_allocResult = -1000069000;  // VK_ERROR_OUT_OF_POOL_MEMORY
  std::vector<std::uint64_t> sets;
  QCHECK(Allocate(game, kDevice, {layout}, &sets) == -1000069000);
  QCHECK(g_written.empty());
}

void AReusedLayoutHandleDropsTheOldPlan() {
  Reset();
  const Game game = Resolve();
  const std::uint64_t layout = LightUvLayout(game);
  g_reuseLayout = layout;  // the driver hands the same value to a layout created after a destroy
  QCHECK(PlainLayout(game) == layout);
  std::vector<std::uint64_t> sets;
  QCHECK(Allocate(game, kDevice, {layout}, &sets) == 0);
  QCHECK(g_written.empty());
}

void NoHostVisibleMemoryTurnsTheGuardOffLoudly() {
  Reset();
  g_memoryFlags = 0x1;
  const Game game = Resolve();
  const std::uint64_t layout = LightUvLayout(game);
  std::vector<std::uint64_t> sets;
  QCHECK(Allocate(game, kDevice, {layout}, &sets) == 0);
  QCHECK(g_written.empty());
  const auto buffer = LinesFor("g40guard_buffer");
  QCHECK(buffer.size() == 1);
  if (!buffer.empty()) {
    QCHECK(Field(buffer[0], "result") == "failed");
    QCHECK(Field(buffer[0], "step") == "memory_type");
  }
  QCHECK(Allocate(game, kDevice, {layout}, &sets) == 0);
  QCHECK(LinesFor("g40guard_buffer").size() == 1);  // tried once
}

void AnotherDeviceIsCountedNotFilled() {
  Reset();
  const Game game = Resolve();
  const std::uint64_t layout = LightUvLayout(game);
  std::vector<std::uint64_t> sets;
  QCHECK(Allocate(game, kDevice, {layout}, &sets) == 0);
  QCHECK(Allocate(game, kOtherDevice, {layout}, &sets) == 0);
  QCHECK(g_written.size() == 2);
  QCHECK(g::GetStats().other_device == 1);
}

void ManySetsAreWrittenInBatches() {
  Reset();
  const Game game = Resolve();
  const std::uint64_t layout = LightUvLayout(game);
  std::vector<std::uint64_t> sets;
  QCHECK(Allocate(game, kDevice, std::vector<std::uint64_t>(40, layout), &sets) == 0);
  QCHECK(g_written.size() == 80);
  QCHECK(g_updateCalls == 2);  // 64 + 16
  QCHECK(g_written.size() == 80 && g_written[79].set == sets[39] && g_written[79].binding == 4);
}

void ThePlanKeepsOnlyUniformTexelBindings() {
  const g::DslBinding b[] = {{0, 4, 0, 1, nullptr},  {1, 5, 1, 1, nullptr},  {2, 4, 100, 1, nullptr},
                             {3, 7, 1, 1, nullptr},  {4, 6, 1, 1, nullptr},  {9, 4, 2, 1, nullptr}};
  g::DslCreateInfo ci{32, nullptr, 0, 6, b};
  const g::LayoutPlan p = g::PlanLayout(&ci);
  QCHECK(!p.skip);
  QCHECK(p.count == 2);
  QCHECK(p.bindings[0].binding == 2 && p.bindings[0].count == g::kMaxDescriptorsPerBinding);
  QCHECK(p.bindings[1].binding == 9 && p.bindings[1].count == 2);
  QCHECK(p.truncated);
  QCHECK(g::PlanLayout(nullptr).count == 0);
  std::vector<g::DslBinding> many;
  for (std::uint32_t i = 0; i < 10; ++i) many.push_back({i, 4, 1, 1, nullptr});
  g::DslCreateInfo big{32, nullptr, 0, 10, many.data()};
  const g::LayoutPlan q = g::PlanLayout(&big);
  QCHECK(q.count == g::kMaxBindingsPerLayout && q.truncated);
}

void TheFillLineCadenceIsPowersOfTwo() {
  QCHECK(!g::IsReportCount(0));
  QCHECK(g::IsReportCount(1) && g::IsReportCount(2) && g::IsReportCount(4) && g::IsReportCount(1024));
  QCHECK(!g::IsReportCount(3) && !g::IsReportCount(6) && !g::IsReportCount(1023));
}

}  // namespace

int main() {
  char dir[] = "/tmp/vk_descriptor_guard_test.XXXXXX";
  if (mkdtemp(dir) == nullptr) return 2;
  g_switchDir = dir;
  q15t::SetModuleSwitchDirForTest(dir);
  sentinel::SetLogSink(&CaptureLine);

  TheLightUvBindingsOfEveryNewSetGetTheZeroedView();
  TheOffFileLeavesTheDriversFunctions();
  OtherNamesAndTheSelfLookupPassThrough();
  AnUntouchedLayoutCostsNoBufferAndNoWrite();
  ABindingFlagsLayoutIsLeftAlone();
  AFailedAllocationIsReturnedAndWritesNothing();
  AReusedLayoutHandleDropsTheOldPlan();
  NoHostVisibleMemoryTurnsTheGuardOffLoudly();
  AnotherDeviceIsCountedNotFilled();
  ManySetsAreWrittenInBatches();
  ThePlanKeepsOnlyUniformTexelBindings();
  TheFillLineCadenceIsPowersOfTwo();

  std::remove((g_switchDir + "/g40guard.off").c_str());
  rmdir(dir);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "vk_descriptor_guard_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("vk_descriptor_guard_test: all checks pass\n");
  return 0;
}
