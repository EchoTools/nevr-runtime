// Host test for the texel-buffer guard (src/quest/combat/texel_buffer_guard.{h,cpp}, texel_buffer_guard_rules.h; #1650, #1737).
//
// The rules (PickBindings, LayoutTable, PickMemoryType) on their own, then the module as the game reaches it: the early step registers its two
// requests with the real vk_ext_window, and the game's own chain (CSysModule::FindMethod -> vkGetInstanceProcAddr -> vkGetDeviceProcAddr ->
// the two device functions) is driven through the real FindMethod thunk entry against a fake driver that records every call. No GOT slot is
// touched (vk_ext_window_test drives the same chain; got_pinned_test resolves the slot in the real library).
//
// Built and run by `just test-quest-combat texel_buffer_guard`.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "quest/combat/combat_install.h"
#include "quest/combat/texel_buffer_guard.h"
#include "quest/combat/texel_buffer_guard_rules.h"
#include "quest/sentinel/vk_ext_window.h"
#include "quest/tests/test_check.h"

namespace {

namespace tb = nevr_quest::combat::texel_buffer_guard;
namespace vk = nevr_quest::combat::texel_buffer_guard::vk;
namespace window = nevr_quest::vk_window;
using VoidFn = void (*)();
using LookupFn = VoidFn (*)(void*, const char*);
using CreateLayoutFn = vk::Result (*)(void*, const vk::DescriptorSetLayoutCreateInfo*, const void*, vk::Handle*);
using AllocateFn = vk::Result (*)(void*, const vk::DescriptorSetAllocateInfo*, vk::Handle*);

void* const kInstance = reinterpret_cast<void*>(0x10);
void* const kDevice = reinterpret_cast<void*>(0x30);
void* const kOtherDevice = reinterpret_cast<void*>(0x31);
void* const kPhysical = reinterpret_cast<void*>(0x20);
constexpr vk::Handle kBuffer = 0xb000;
constexpr vk::Handle kMemory = 0xc000;
constexpr vk::Handle kView = 0xd000;

// ---- log capture ------------------------------------------------------------------------------------------------------

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
bool LevelIs(const std::string& line, const char* level) { return line.find(std::string("\"level\":\"") + level + "\"") != std::string::npos; }

// ---- a driver ---------------------------------------------------------------------------------------------------------

std::vector<std::string> g_calls;  // every driver call, in order
std::string g_failStep;            // the driver call that fails ("" none)
vk::Result g_layoutResult = vk::kSuccess;
vk::Result g_allocateResult = vk::kSuccess;
vk::Handle g_nextLayout = 0x1000;
std::uint32_t g_physicalCount = 1;
struct SeenWrite {
  vk::WriteDescriptorSet write;
  std::vector<vk::Handle> views;
};
std::vector<std::vector<SeenWrite>> g_updates;  // one entry per vkUpdateDescriptorSets call
vk::BufferCreateInfo g_bufferInfo{};
vk::MemoryAllocateInfo g_memoryInfo{};
vk::BufferViewCreateInfo g_viewInfo{};
alignas(16) unsigned char g_mapped[tb::kZeroBufferBytes];
void* g_deviceSeen = nullptr;

vk::Result FakeCreateLayout(void* device, const vk::DescriptorSetLayoutCreateInfo*, const void*, vk::Handle* out) {
  g_calls.push_back("create_layout");
  g_deviceSeen = device;
  if (g_layoutResult != vk::kSuccess) return g_layoutResult;
  *out = g_nextLayout;
  g_nextLayout += 0x40;
  return vk::kSuccess;
}
vk::Result FakeAllocate(void*, const vk::DescriptorSetAllocateInfo* info, vk::Handle* sets) {
  g_calls.push_back("allocate");
  if (g_allocateResult != vk::kSuccess) return g_allocateResult;
  for (std::uint32_t i = 0; i < info->descriptorSetCount; ++i) sets[i] = 0x9000 + i * 0x10;
  return vk::kSuccess;
}
void FakeUpdate(void* device, std::uint32_t count, const vk::WriteDescriptorSet* writes, std::uint32_t copies, const void*) {
  g_calls.push_back("update");
  QCHECK(device == kDevice && copies == 0);
  std::vector<SeenWrite> seen;
  for (std::uint32_t i = 0; i < count; ++i) {
    SeenWrite s{writes[i], {}};
    for (std::uint32_t k = 0; k < writes[i].descriptorCount && writes[i].pTexelBufferView != nullptr; ++k) {
      s.views.push_back(writes[i].pTexelBufferView[k]);
    }
    seen.push_back(s);
  }
  g_updates.push_back(seen);
}
vk::Result Step(const char* name, vk::Result ok = vk::kSuccess) {
  g_calls.push_back(name);
  return g_failStep == name ? -2 /* VK_ERROR_OUT_OF_DEVICE_MEMORY */ : ok;
}
vk::Result FakeCreateBuffer(void*, const vk::BufferCreateInfo* info, const void*, vk::Handle* out) {
  g_bufferInfo = *info;
  *out = kBuffer;
  return Step("create_buffer");
}
void FakeRequirements(void*, vk::Handle buffer, vk::MemoryRequirements* out) {
  g_calls.push_back("requirements");
  QCHECK(buffer == kBuffer);
  *out = vk::MemoryRequirements{tb::kZeroBufferBytes, 256, 0x0e};  // types 1, 2 and 3 allowed
}
vk::Result FakeAllocateMemory(void*, const vk::MemoryAllocateInfo* info, const void*, vk::Handle* out) {
  g_memoryInfo = *info;
  *out = kMemory;
  return Step("allocate_memory");
}
vk::Result FakeBind(void*, vk::Handle buffer, vk::Handle memory, std::uint64_t offset) {
  QCHECK(buffer == kBuffer && memory == kMemory && offset == 0);
  return Step("bind_memory");
}
vk::Result FakeMap(void*, vk::Handle memory, std::uint64_t offset, std::uint64_t size, std::uint32_t flags, void** out) {
  QCHECK(memory == kMemory && offset == 0 && size == vk::kWholeSize && flags == 0);
  *out = g_mapped;
  return Step("map_memory");
}
vk::Result FakeCreateView(void*, const vk::BufferViewCreateInfo* info, const void*, vk::Handle* out) {
  g_viewInfo = *info;
  *out = kView;
  return Step("create_view");
}
vk::Result FakeEnumeratePhysical(void* instance, std::uint32_t* count, void** out) {
  g_calls.push_back(out == nullptr ? "enumerate_count" : "enumerate");
  QCHECK(instance == kInstance);
  if (out == nullptr) {
    *count = g_physicalCount;
    return vk::kSuccess;
  }
  if (*count >= 1) out[0] = kPhysical;
  return vk::kSuccess;
}
void FakeMemoryProperties(void* physical, vk::PhysicalDeviceMemoryProperties* out) {
  g_calls.push_back("memory_properties");
  QCHECK(physical == kPhysical);
  *out = vk::PhysicalDeviceMemoryProperties{};
  out->memoryTypeCount = 4;
  out->memoryTypes[0].propertyFlags = 0x1;        // device local, not allowed by the buffer either
  out->memoryTypes[1].propertyFlags = 0x1;        // device local
  out->memoryTypes[2].propertyFlags = 0x2;        // host visible, not coherent
  out->memoryTypes[3].propertyFlags = 0x2 | 0x4;  // host visible and coherent: the one
}

VoidFn FakeGetDeviceProcAddr(void* device, const char* name) {
  QCHECK(device == kDevice || device == kOtherDevice);
  struct Entry { const char* name; VoidFn fn; };
  static const Entry kEntries[] = {
      {"vkCreateDescriptorSetLayout", reinterpret_cast<VoidFn>(&FakeCreateLayout)},
      {"vkAllocateDescriptorSets", reinterpret_cast<VoidFn>(&FakeAllocate)},
      {"vkUpdateDescriptorSets", reinterpret_cast<VoidFn>(&FakeUpdate)},
      {"vkCreateBuffer", reinterpret_cast<VoidFn>(&FakeCreateBuffer)},
      {"vkGetBufferMemoryRequirements", reinterpret_cast<VoidFn>(&FakeRequirements)},
      {"vkAllocateMemory", reinterpret_cast<VoidFn>(&FakeAllocateMemory)},
      {"vkBindBufferMemory", reinterpret_cast<VoidFn>(&FakeBind)},
      {"vkMapMemory", reinterpret_cast<VoidFn>(&FakeMap)},
      {"vkCreateBufferView", reinterpret_cast<VoidFn>(&FakeCreateView)},
  };
  if (g_failStep == std::string("lookup:") + name) return nullptr;
  for (const Entry& e : kEntries) {
    if (std::strcmp(name, e.name) == 0) return e.fn;
  }
  return nullptr;
}
VoidFn FakeGetInstanceProcAddr(void*, const char* name) {
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<VoidFn>(&FakeGetDeviceProcAddr);
  if (std::strcmp(name, "vkEnumeratePhysicalDevices") == 0) return reinterpret_cast<VoidFn>(&FakeEnumeratePhysical);
  if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0) return reinterpret_cast<VoidFn>(&FakeMemoryProperties);
  return nullptr;
}
void* FakeFindMethod(unsigned long, const char* name) {
  return std::strcmp(name, "vkGetInstanceProcAddr") == 0 ? reinterpret_cast<void*>(&FakeGetInstanceProcAddr) : nullptr;
}

// What the game holds after CGS::Initialize's fill.
struct Game {
  LookupFn gdpa = nullptr;
  CreateLayoutFn createLayout = nullptr;
  AllocateFn allocate = nullptr;
};

// A fresh process: the window and the guard forgotten, the fake driver reset, the window armed with the fake FindMethod.
void Reset() {
  g_lines.clear();
  g_calls.clear();
  g_updates.clear();
  g_failStep.clear();
  g_layoutResult = vk::kSuccess;
  g_allocateResult = vk::kSuccess;
  g_nextLayout = 0x1000;
  g_physicalCount = 1;
  g_bufferInfo = {};
  g_memoryInfo = {};
  g_viewInfo = {};
  g_deviceSeen = nullptr;
  std::memset(g_mapped, 0xab, sizeof(g_mapped));
  window::ResetForTest();
  window::FindMethodThunk::Reset();
  window::DeviceExtensionsThunk::Reset();
  *window::FindMethodThunk::OriginalOut() = reinterpret_cast<void*>(&FakeFindMethod);
  window::ArmForTest();
  tb::ResetForTest();
}

// The game's fill: FindMethod -> vkGetInstanceProcAddr -> vkGetDeviceProcAddr -> the two functions, as CGS::Initialize does it.
Game GameFills() {
  Game g;
  auto findMethod = reinterpret_cast<window::FindMethodThunk::Fn>(window::FindMethodThunk::EntryAddress());
  auto gipa = reinterpret_cast<LookupFn>(findMethod(7, "vkGetInstanceProcAddr"));
  if (gipa == nullptr) return g;
  g.gdpa = reinterpret_cast<LookupFn>(gipa(kInstance, "vkGetDeviceProcAddr"));
  if (g.gdpa == nullptr) return g;
  g.createLayout = reinterpret_cast<CreateLayoutFn>(g.gdpa(kDevice, "vkCreateDescriptorSetLayout"));
  g.allocate = reinterpret_cast<AllocateFn>(g.gdpa(kDevice, "vkAllocateDescriptorSets"));
  return g;
}

// A guard that is on, its requests taken by the game: the usual start of a case.
Game Started() {
  Reset();
  const nevr_quest::combat::ModuleResult r = tb::Early(true);
  QCHECK(r.installed && !r.off && std::strcmp(r.status, "requested") == 0);
  return GameFills();
}

vk::DescriptorSetLayoutBinding Binding(std::uint32_t binding, std::uint32_t type, std::uint32_t count) {
  return vk::DescriptorSetLayoutBinding{binding, type, count, 0x1, nullptr};
}
vk::DescriptorSetLayoutCreateInfo LayoutInfo(const std::vector<vk::DescriptorSetLayoutBinding>& b, const void* next = nullptr) {
  vk::DescriptorSetLayoutCreateInfo info{};
  info.sType = 32;  // VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
  info.pNext = next;
  info.bindingCount = static_cast<std::uint32_t>(b.size());
  info.pBindings = b.data();
  return info;
}
vk::Handle MakeLayout(const Game& g, const std::vector<vk::DescriptorSetLayoutBinding>& b, const void* next = nullptr) {
  const vk::DescriptorSetLayoutCreateInfo info = LayoutInfo(b, next);
  vk::Handle out = 0;
  QCHECK(g.createLayout(kDevice, &info, nullptr, &out) == vk::kSuccess);
  return out;
}
std::vector<vk::Handle> Allocate(const Game& g, const std::vector<vk::Handle>& layouts, vk::Result* result = nullptr, void* device = kDevice) {
  vk::DescriptorSetAllocateInfo info{};
  info.sType = 34;  // VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
  info.descriptorPool = 0x7000;
  info.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
  info.pSetLayouts = layouts.data();
  std::vector<vk::Handle> sets(layouts.size(), 0);
  const vk::Result r = g.allocate(device, &info, sets.data());
  if (result != nullptr) *result = r;
  return sets;
}
std::size_t CallCount(const char* name) {
  std::size_t n = 0;
  for (const std::string& c : g_calls) n += c == name ? 1 : 0;
  return n;
}
std::vector<SeenWrite> AllWrites() {
  std::vector<SeenWrite> out;
  for (const auto& u : g_updates) out.insert(out.end(), u.begin(), u.end());
  return out;
}

// ---- the rules --------------------------------------------------------------------------------------------------------

void RulesPickUniformTexelOnly() {
  const std::vector<vk::DescriptorSetLayoutBinding> b = {Binding(0, 1, 1), Binding(3, 4, 1), Binding(4, 4, 2), Binding(5, 5, 1),
                                                         Binding(6, 6, 1), Binding(7, 7, 1), Binding(9, 2, 3)};
  const vk::DescriptorSetLayoutCreateInfo info = LayoutInfo(b);
  const tb::LayoutPicks p = tb::PickBindings(&info);
  QCHECK(p.verdict == tb::Verdict::kPicked);
  QCHECK(p.n == 2);
  QCHECK(p.picks[0].binding == 3 && p.picks[0].count == 1);
  QCHECK(p.picks[1].binding == 4 && p.picks[1].count == 2);
  // No uniform texel binding at all.
  const std::vector<vk::DescriptorSetLayoutBinding> none = {Binding(0, 5, 1), Binding(1, 6, 1), Binding(2, 1, 1)};
  const vk::DescriptorSetLayoutCreateInfo noneInfo = LayoutInfo(none);
  QCHECK(tb::PickBindings(&noneInfo).verdict == tb::Verdict::kNone);
  QCHECK(tb::PickBindings(nullptr).verdict == tb::Verdict::kNone);
}

void RulesBindingFlagsSkip() {
  const vk::BaseIn other{1000000000, nullptr};
  const vk::BaseIn flags{vk::kStructureTypeBindingFlagsCreateInfo, nullptr};
  const vk::BaseIn chain{12345, &flags};  // the flags structure second in the chain
  const std::vector<vk::DescriptorSetLayoutBinding> b = {Binding(3, 4, 1)};
  vk::DescriptorSetLayoutCreateInfo info = LayoutInfo(b, &flags);
  QCHECK(tb::PickBindings(&info).verdict == tb::Verdict::kFlags);
  info.pNext = &chain;
  QCHECK(tb::PickBindings(&info).verdict == tb::Verdict::kFlags);
  info.pNext = &other;
  QCHECK(tb::PickBindings(&info).verdict == tb::Verdict::kPicked);
}

void RulesZeroCountSkipped() {
  const std::vector<vk::DescriptorSetLayoutBinding> b = {Binding(3, 4, 0), Binding(4, 4, 1)};
  const vk::DescriptorSetLayoutCreateInfo info = LayoutInfo(b);
  const tb::LayoutPicks p = tb::PickBindings(&info);
  QCHECK(p.verdict == tb::Verdict::kPicked && p.n == 1 && p.picks[0].binding == 4);
  const std::vector<vk::DescriptorSetLayoutBinding> only = {Binding(3, 4, 0)};
  const vk::DescriptorSetLayoutCreateInfo onlyInfo = LayoutInfo(only);
  QCHECK(tb::PickBindings(&onlyInfo).verdict == tb::Verdict::kNone);
}

void RulesTooManyLeftAlone() {
  std::vector<vk::DescriptorSetLayoutBinding> b;
  for (std::uint32_t i = 0; i < tb::kMaxPicks; ++i) b.push_back(Binding(i, 4, 1));
  vk::DescriptorSetLayoutCreateInfo info = LayoutInfo(b);
  QCHECK(tb::PickBindings(&info).verdict == tb::Verdict::kPicked && tb::PickBindings(&info).n == tb::kMaxPicks);
  b.push_back(Binding(9, 4, 1));
  info = LayoutInfo(b);
  const tb::LayoutPicks over = tb::PickBindings(&info);
  QCHECK(over.verdict == tb::Verdict::kTooMany && over.n == 0);
  const std::vector<vk::DescriptorSetLayoutBinding> wide = {Binding(0, 4, tb::kMaxDescriptors)};
  info = LayoutInfo(wide);
  QCHECK(tb::PickBindings(&info).verdict == tb::Verdict::kPicked);
  const std::vector<vk::DescriptorSetLayoutBinding> wider = {Binding(0, 4, tb::kMaxDescriptors + 1)};
  info = LayoutInfo(wider);
  QCHECK(tb::PickBindings(&info).verdict == tb::Verdict::kTooMany);
}

void TableRecordsReplacesAndFills() {
  static tb::LayoutTable table;
  table.Clear();
  tb::LayoutPicks picked;
  picked.verdict = tb::Verdict::kPicked;
  picked.n = 1;
  picked.picks[0] = {3, 1};
  tb::LayoutPicks none;
  tb::LayoutPicks out;
  QCHECK(table.Record(0, picked) == tb::LayoutTable::Put::kNotStored);
  QCHECK(table.Record(0x1000, none) == tb::LayoutTable::Put::kNotStored);  // a layout with nothing to write takes no slot
  QCHECK(!table.Find(0x1000, &out));
  QCHECK(table.Record(0x1000, picked) == tb::LayoutTable::Put::kStored);
  QCHECK(table.Find(0x1000, &out) && out.verdict == tb::Verdict::kPicked && out.picks[0].binding == 3);
  // The handle comes back for a layout with no texel binding: its entry says so, so no write is made for the new layout.
  QCHECK(table.Record(0x1000, none) == tb::LayoutTable::Put::kReplaced);
  QCHECK(table.Find(0x1000, &out) && out.verdict == tb::Verdict::kNone);
  QCHECK(table.Size() == 1);
  // Fill it: capacity layouts, then the next is refused; a handle already there is still replaced.
  table.Clear();
  for (std::size_t i = 0; i < tb::kTableCapacity; ++i) QCHECK(table.Record(0x10000 + i * 0x40, picked) == tb::LayoutTable::Put::kStored);
  QCHECK(table.Size() == tb::kTableCapacity);
  QCHECK(table.Record(0x900000, picked) == tb::LayoutTable::Put::kFull);
  QCHECK(table.Record(0x10000, none) == tb::LayoutTable::Put::kReplaced);
  for (std::size_t i = 1; i < tb::kTableCapacity; ++i) QCHECK(table.Find(0x10000 + i * 0x40, &out) && out.verdict == tb::Verdict::kPicked);
  QCHECK(!table.Find(0x900000, &out));
}

void RulesMemoryTypeHostVisibleCoherent() {
  vk::PhysicalDeviceMemoryProperties p{};
  p.memoryTypeCount = 4;
  p.memoryTypes[0].propertyFlags = 0x1;
  p.memoryTypes[1].propertyFlags = 0x2 | 0x4;
  p.memoryTypes[2].propertyFlags = 0x2;
  p.memoryTypes[3].propertyFlags = 0x1 | 0x2 | 0x4;
  QCHECK(tb::PickMemoryType(p, 0xf) == 1);
  QCHECK(tb::PickMemoryType(p, 0xd) == 3);  // type 1 not allowed by the buffer
  QCHECK(tb::PickMemoryType(p, 0x5) == -1);
  p.memoryTypeCount = 1;
  QCHECK(tb::PickMemoryType(p, 0xf) == -1);  // types past the count are not types
}

// ---- the module, as the game reaches it --------------------------------------------------------------------------------

void EarlyRequestsBothAndTheGameGetsTheWrappers() {
  Reset();
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  const nevr_quest::combat::ModuleResult r = tb::Early(true);
  const Game g = GameFills();
  sentinel::SetLogSink(previous);
  QCHECK(r.installed && !r.off);
  QCHECK(g.createLayout == &tb::CreateDescriptorSetLayout);
  QCHECK(g.allocate == &tb::AllocateDescriptorSets);
  QCHECK(g.gdpa(kDevice, "vkUpdateDescriptorSets") == reinterpret_cast<VoidFn>(&FakeUpdate));  // not requested: the driver's
  const std::vector<std::string> install = LinesFor("combat_texel_buffer_guard_install");
  QCHECK(install.size() == 1);
  if (install.size() == 1) {
    QCHECK(Field(install[0], "result") == "requested" && Field(install[0], "owner") == "vk_ext_window");
    QCHECK(Field(install[0], "names") == "vkCreateDescriptorSetLayout vkAllocateDescriptorSets");
    QCHECK(LevelIs(install[0], "info"));
  }
  QCHECK(LinesFor("vk_ext_window_device_function").size() == 2);
}

// The install step run twice in one process (a host test's second InstallEarly) is the same request, not a refusal.
void EarlyTwiceIsOneRequest() {
  Reset();
  QCHECK(tb::Early(true).installed);
  const nevr_quest::combat::ModuleResult again = tb::Early(true);
  QCHECK(again.installed && std::strcmp(again.status, "requested") == 0);
  const Game g = GameFills();
  QCHECK(g.createLayout == &tb::CreateDescriptorSetLayout && g.allocate == &tb::AllocateDescriptorSets);
}

void KeyOffRequestsNothing() {
  Reset();
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  const nevr_quest::combat::ModuleResult r = tb::Early(false);
  const Game g = GameFills();
  tb::End();
  sentinel::SetLogSink(previous);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off") == 0);
  QCHECK(g.gdpa == reinterpret_cast<LookupFn>(&FakeGetDeviceProcAddr));  // the game's lookups are the driver's
  QCHECK(g.createLayout == &FakeCreateLayout && g.allocate == &FakeAllocate);
  const std::vector<std::string> install = LinesFor("combat_texel_buffer_guard_install");
  QCHECK(install.size() == 1 && Field(install[0], "result") == "off_config");
  QCHECK(LinesFor("combat_texel_buffer_guard_end").empty());  // nothing was requested: nothing to account for
}

void RequestAfterTheLookupIsRefused() {
  Reset();
  GameFills();  // the game asked before the guard's step
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  const nevr_quest::combat::ModuleResult r = tb::Early(true);
  sentinel::SetLogSink(previous);
  QCHECK(!r.installed && !r.off && std::strcmp(r.status, "sealed") == 0);
  const std::vector<std::string> install = LinesFor("combat_texel_buffer_guard_install");
  QCHECK(install.size() == 1 && Field(install[0], "result") == "refused" && Field(install[0], "reason") == "sealed");
  if (install.size() == 1) QCHECK(LevelIs(install[0], "error"));
}

void AllocateNoTexelPassesThrough() {
  const Game g = Started();
  const vk::Handle plain = MakeLayout(g, {Binding(0, 1, 1), Binding(1, 6, 1)});
  vk::Result r = 99;
  const std::vector<vk::Handle> sets = Allocate(g, {plain, plain}, &r);
  QCHECK(r == vk::kSuccess && sets[0] == 0x9000 && sets[1] == 0x9010);
  QCHECK(g_updates.empty());
  QCHECK(CallCount("create_buffer") == 0);  // nothing to write: the zeroed buffer is not made
  QCHECK(tb::CurrentCounts().unknownLayouts == 2 && tb::CurrentCounts().sets == 0);
}

void AllocateWritesViewPerBinding() {
  const Game g = Started();
  const vk::Handle texel = MakeLayout(g, {Binding(0, 1, 1), Binding(3, 4, 1), Binding(4, 4, 3), Binding(5, 5, 1)});
  const vk::Handle plain = MakeLayout(g, {Binding(0, 1, 1)});
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  vk::Result r = 99;
  const std::vector<vk::Handle> sets = Allocate(g, {texel, plain, texel}, &r);
  sentinel::SetLogSink(previous);
  QCHECK(r == vk::kSuccess);
  // The driver's allocation first, then the zeroed buffer, then one update with the writes.
  QCHECK(g_calls.size() >= 3 && g_calls[2] == "allocate");
  QCHECK(g_calls.back() == "update");
  QCHECK(CallCount("update") == 1 && CallCount("allocate") == 1);
  const std::vector<SeenWrite> w = AllWrites();
  QCHECK(w.size() == 4);  // two bindings in each of the two texel sets; none in the plain one
  const std::uint32_t wantBinding[] = {3, 4, 3, 4};
  const std::uint32_t wantCount[] = {1, 3, 1, 3};
  const vk::Handle wantSet[] = {sets[0], sets[0], sets[2], sets[2]};
  for (std::size_t i = 0; i < w.size() && i < 4; ++i) {
    QCHECK(w[i].write.sType == vk::kStructureTypeWriteDescriptorSet && w[i].write.pNext == nullptr);
    QCHECK(w[i].write.dstSet == wantSet[i] && w[i].write.dstBinding == wantBinding[i] && w[i].write.dstArrayElement == 0);
    QCHECK(w[i].write.descriptorCount == wantCount[i]);
    QCHECK(w[i].write.descriptorType == vk::kDescriptorTypeUniformTexelBuffer);
    QCHECK(w[i].write.pImageInfo == nullptr && w[i].write.pBufferInfo == nullptr);
    QCHECK(w[i].views.size() == wantCount[i]);
    for (vk::Handle v : w[i].views) QCHECK(v == kView);
  }
  // The zeroed buffer: 64 KiB of uniform texel buffer, in the host-visible coherent type, zeroed, viewed as R32G32B32A32_SFLOAT.
  QCHECK(g_bufferInfo.sType == vk::kStructureTypeBufferCreateInfo && g_bufferInfo.size == tb::kZeroBufferBytes);
  QCHECK(g_bufferInfo.usage == vk::kBufferUsageUniformTexelBuffer && g_bufferInfo.sharingMode == vk::kSharingModeExclusive);
  QCHECK(g_memoryInfo.sType == vk::kStructureTypeMemoryAllocateInfo && g_memoryInfo.memoryTypeIndex == 3);
  QCHECK(g_memoryInfo.allocationSize == tb::kZeroBufferBytes);
  bool zero = true;
  for (unsigned char b : g_mapped) zero = zero && b == 0;
  QCHECK(zero);
  QCHECK(g_viewInfo.sType == vk::kStructureTypeBufferViewCreateInfo && g_viewInfo.buffer == kBuffer);
  QCHECK(g_viewInfo.format == vk::kFormatR32G32B32A32Sfloat && g_viewInfo.offset == 0 && g_viewInfo.range == vk::kWholeSize);
  QCHECK(tb::ZeroViewMade() && !tb::TurnedOff());
  const tb::Counts c = tb::CurrentCounts();
  QCHECK(c.sets == 2 && c.bindings == 4 && c.unknownLayouts == 1 && c.layouts == 1);
  // The lines: the view made (Info), the first allocation once, the count at powers of two (1 and 2).
  const std::vector<std::string> made = LinesFor("combat_texel_buffer_guard_zero_view");
  QCHECK(made.size() == 1);
  if (made.size() == 1) {
    QCHECK(Field(made[0], "result") == "made" && Field(made[0], "bytes") == "65536" && Field(made[0], "memory_type") == "3");
    QCHECK(LevelIs(made[0], "info"));
  }
  const std::vector<std::string> first = LinesFor("combat_texel_buffer_guard_first");
  QCHECK(first.size() == 1);
  if (first.size() == 1) QCHECK(Field(first[0], "sets") == "2" && Field(first[0], "bindings") == "4" && LevelIs(first[0], "info"));
  const std::vector<std::string> counts = LinesFor("combat_texel_buffer_guard_count");
  QCHECK(counts.size() == 2);
  if (counts.size() == 2) QCHECK(LevelIs(counts[0], "debug") && Field(counts[1], "sets") == "2");
  // A second allocation: no new buffer, no second first line, the count line only at the next power of two (4).
  g_lines.clear();
  previous = sentinel::SetLogSink(&CaptureLine);
  Allocate(g, {texel});
  Allocate(g, {texel});
  sentinel::SetLogSink(previous);
  QCHECK(CallCount("create_buffer") == 1);
  QCHECK(LinesFor("combat_texel_buffer_guard_first").empty());
  const std::vector<std::string> later = LinesFor("combat_texel_buffer_guard_count");
  QCHECK(later.size() == 1 && Field(later[0], "sets") == "4");
}

void AllocateFailureNoWrite() {
  const Game g = Started();
  const vk::Handle texel = MakeLayout(g, {Binding(3, 4, 1)});
  g_allocateResult = -1000069000;  // VK_ERROR_OUT_OF_POOL_MEMORY
  vk::Result r = 99;
  Allocate(g, {texel}, &r);
  QCHECK(r == -1000069000);  // the driver's result, unchanged
  QCHECK(g_updates.empty() && CallCount("create_buffer") == 0);
  QCHECK(tb::CurrentCounts().sets == 0);
}

void LayoutFailureRecordsNothing() {
  const Game g = Started();
  g_layoutResult = -1;
  const std::vector<vk::DescriptorSetLayoutBinding> b = {Binding(3, 4, 1)};
  const vk::DescriptorSetLayoutCreateInfo info = LayoutInfo(b);
  vk::Handle out = 0x4440;  // what the caller's variable held: not a layout
  QCHECK(g.createLayout(kDevice, &info, nullptr, &out) == -1);
  QCHECK(g_deviceSeen == kDevice);
  g_layoutResult = vk::kSuccess;
  Allocate(g, {0x4440});
  QCHECK(g_updates.empty() && tb::CurrentCounts().layouts == 0 && tb::CurrentCounts().unknownLayouts == 1);
}

void UnknownLayoutCounted() {
  const Game g = Started();
  Allocate(g, {0x5555, 0x6666});  // layouts the guard never saw created
  QCHECK(g_updates.empty());
  QCHECK(tb::CurrentCounts().unknownLayouts == 2);
}

void FlaggedAndOversizeLayoutsSkippedAndCounted() {
  const Game g = Started();
  const vk::BaseIn flags{vk::kStructureTypeBindingFlagsCreateInfo, nullptr};
  const vk::Handle flagged = MakeLayout(g, {Binding(3, 4, 1)}, &flags);
  const vk::Handle wide = MakeLayout(g, {Binding(3, 4, tb::kMaxDescriptors + 1)});
  Allocate(g, {flagged, wide});
  QCHECK(g_updates.empty());
  QCHECK(tb::CurrentCounts().skippedFlags == 2 && tb::CurrentCounts().sets == 0);
}

void AReusedHandleWithNoTexelBindingGetsNoWrite() {
  const Game g = Started();
  const vk::Handle texel = MakeLayout(g, {Binding(3, 4, 1)});
  g_nextLayout = texel;  // the driver hands the same value out again for a layout with no texel binding
  const vk::Handle again = MakeLayout(g, {Binding(3, 5, 1)});
  QCHECK(again == texel);
  Allocate(g, {again});
  QCHECK(g_updates.empty());
}

void ManyWritesAreBatched() {
  const Game g = Started();
  const vk::Handle texel = MakeLayout(g, {Binding(1, 4, 1), Binding(2, 4, 1), Binding(3, 4, 1), Binding(4, 4, 1)});
  const std::vector<vk::Handle> layouts(20, texel);  // 80 writes
  Allocate(g, layouts);
  QCHECK(g_updates.size() == 3);  // 32 + 32 + 16
  if (g_updates.size() == 3) QCHECK(g_updates[0].size() == tb::kWriteBatch && g_updates[2].size() == 16);
  const std::vector<SeenWrite> w = AllWrites();
  QCHECK(w.size() == 80);
  if (w.size() == 80) QCHECK(w[79].write.dstSet == 0x9000 + 19 * 0x10 && w[79].write.dstBinding == 4);
}

void OtherDevicePassesCounted() {
  const Game g = Started();
  const vk::Handle texel = MakeLayout(g, {Binding(3, 4, 1)});
  Allocate(g, {texel});  // the view is made with kDevice
  QCHECK(CallCount("update") == 1);
  Allocate(g, {texel}, nullptr, kOtherDevice);
  QCHECK(CallCount("update") == 1);
  QCHECK(tb::CurrentCounts().otherDevice == 1);
}

void ZeroViewFailureTurnsOff() {
  const char* const steps[] = {"create_buffer", "allocate_memory", "bind_memory", "map_memory", "create_view", "lookup:vkCreateBufferView",
                               "physical_devices"};
  for (const char* step : steps) {
    const Game g = Started();
    const vk::Handle texel = MakeLayout(g, {Binding(3, 4, 1)});
    g_failStep = step;
    if (std::strcmp(step, "physical_devices") == 0) g_physicalCount = 2;
    sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
    vk::Result r = 99;
    Allocate(g, {texel}, &r);
    const std::size_t updatesAfterFirst = g_updates.size();
    const std::size_t createdAfterFirst = CallCount("create_buffer");
    Allocate(g, {texel}, &r);
    sentinel::SetLogSink(previous);
    QCHECK(r == vk::kSuccess);  // the game's allocation is never changed
    QCHECK(updatesAfterFirst == 0 && g_updates.empty());
    QCHECK(CallCount("create_buffer") == createdAfterFirst);  // tried once, then off for the process
    QCHECK(tb::TurnedOff() && !tb::ZeroViewMade());
    const std::vector<std::string> failed = LinesFor("combat_texel_buffer_guard_zero_view");
    QCHECK(failed.size() == 1);
    if (failed.size() == 1) {
      QCHECK(Field(failed[0], "result") == "failed" && LevelIs(failed[0], "warn"));
      const std::string want = std::strcmp(step, "lookup:vkCreateBufferView") == 0 ? "entry_points"
                               : std::strcmp(step, "physical_devices") == 0      ? "physical_device"
                                                                                 : step;
      if (Field(failed[0], "step") != want) std::fprintf(stderr, "step %s: line says %s\n", step, Field(failed[0], "step").c_str());
      QCHECK(Field(failed[0], "step") == want);
    }
    // Off: a later layout is not recorded.
    MakeLayout(g, {Binding(3, 4, 1)});
    QCHECK(tb::CurrentCounts().layouts == 1);
  }
}

void TableFullWarnsOnce() {
  const Game g = Started();
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  for (std::size_t i = 0; i < tb::kTableCapacity + 3; ++i) MakeLayout(g, {Binding(3, 4, 1)});
  sentinel::SetLogSink(previous);
  QCHECK(tb::CurrentCounts().layouts == tb::kTableCapacity && tb::CurrentCounts().tableFull == 3);
  const std::vector<std::string> full = LinesFor("combat_texel_buffer_guard_table_full");
  QCHECK(full.size() == 1);
  if (full.size() == 1) QCHECK(Field(full[0], "capacity") == "1024" && LevelIs(full[0], "warn"));
}

void EndSaysWrappedOrNot() {
  // Taken by the game: Info with the counts.
  Game g = Started();
  Allocate(g, {MakeLayout(g, {Binding(3, 4, 1)})});
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  tb::End();
  sentinel::SetLogSink(previous);
  std::vector<std::string> end = LinesFor("combat_texel_buffer_guard_end");
  QCHECK(end.size() == 1);
  if (end.size() == 1) {
    QCHECK(Field(end[0], "wrapped") == "1" && Field(end[0], "sets") == "1" && Field(end[0], "bindings") == "1" && LevelIs(end[0], "info"));
  }
  // Requested, never taken (the game never looked the functions up): Warning, wrapped 0.
  Reset();
  QCHECK(tb::Early(true).installed);
  g_lines.clear();
  previous = sentinel::SetLogSink(&CaptureLine);
  tb::End();
  sentinel::SetLogSink(previous);
  end = LinesFor("combat_texel_buffer_guard_end");
  QCHECK(end.size() == 1);
  if (end.size() == 1) QCHECK(Field(end[0], "wrapped") == "0" && LevelIs(end[0], "warn"));
}

}  // namespace

int main() {
  RulesPickUniformTexelOnly();
  RulesBindingFlagsSkip();
  RulesZeroCountSkipped();
  RulesTooManyLeftAlone();
  TableRecordsReplacesAndFills();
  RulesMemoryTypeHostVisibleCoherent();
  EarlyRequestsBothAndTheGameGetsTheWrappers();
  EarlyTwiceIsOneRequest();
  KeyOffRequestsNothing();
  RequestAfterTheLookupIsRefused();
  AllocateNoTexelPassesThrough();
  AllocateWritesViewPerBinding();
  AllocateFailureNoWrite();
  LayoutFailureRecordsNothing();
  UnknownLayoutCounted();
  FlaggedAndOversizeLayoutsSkippedAndCounted();
  AReusedHandleWithNoTexelBindingGetsNoWrite();
  ManyWritesAreBatched();
  OtherDevicePassesCounted();
  ZeroViewFailureTurnsOff();
  TableFullWarnsOnce();
  EndSaysWrappedOrNot();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "texel_buffer_guard_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("texel_buffer_guard_test: all checks pass\n");
  return 0;
}
