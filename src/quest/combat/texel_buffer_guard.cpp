// The texel-buffer guard (texel_buffer_guard.h, docs/quest/combat/texel_buffer_guard.md): a new descriptor set's uniform texel buffer
// bindings hold a zeroed view before the game sees the set.
#include "quest/combat/texel_buffer_guard.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>

#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/vk_ext_window.h"

#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
// The rules header's own Vulkan types are the ABI's: checked here wherever the real header exists (the NDK always has it).
namespace nevr_quest::combat::texel_buffer_guard::vk {
static_assert(kDescriptorTypeUniformTexelBuffer == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, "descriptor type");
static_assert(kStructureTypeMemoryAllocateInfo == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, "sType");
static_assert(kStructureTypeBufferCreateInfo == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, "sType");
static_assert(kStructureTypeBufferViewCreateInfo == VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, "sType");
static_assert(kStructureTypeWriteDescriptorSet == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, "sType");
static_assert(kStructureTypeBindingFlagsCreateInfo == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT, "sType");
static_assert(kBufferUsageUniformTexelBuffer == VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, "usage");
static_assert(kSharingModeExclusive == VK_SHARING_MODE_EXCLUSIVE, "sharing mode");
static_assert(kFormatR32G32B32A32Sfloat == VK_FORMAT_R32G32B32A32_SFLOAT, "format");
static_assert(kMemoryPropertyHostVisible == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "memory property");
static_assert(kMemoryPropertyHostCoherent == VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "memory property");
static_assert(kWholeSize == VK_WHOLE_SIZE && kMaxMemoryTypes == VK_MAX_MEMORY_TYPES && kMaxMemoryHeaps == VK_MAX_MEMORY_HEAPS, "limits");
static_assert(kSuccess == VK_SUCCESS && kErrorInitializationFailed == VK_ERROR_INITIALIZATION_FAILED, "results");
static_assert(sizeof(DescriptorSetLayoutBinding) == sizeof(VkDescriptorSetLayoutBinding) &&
                  offsetof(DescriptorSetLayoutBinding, descriptorCount) == offsetof(VkDescriptorSetLayoutBinding, descriptorCount),
              "binding");
static_assert(sizeof(DescriptorSetLayoutCreateInfo) == sizeof(VkDescriptorSetLayoutCreateInfo) &&
                  offsetof(DescriptorSetLayoutCreateInfo, bindingCount) == offsetof(VkDescriptorSetLayoutCreateInfo, bindingCount) &&
                  offsetof(DescriptorSetLayoutCreateInfo, pBindings) == offsetof(VkDescriptorSetLayoutCreateInfo, pBindings),
              "layout info");
static_assert(sizeof(DescriptorSetAllocateInfo) == sizeof(VkDescriptorSetAllocateInfo) &&
                  offsetof(DescriptorSetAllocateInfo, descriptorSetCount) == offsetof(VkDescriptorSetAllocateInfo, descriptorSetCount) &&
                  offsetof(DescriptorSetAllocateInfo, pSetLayouts) == offsetof(VkDescriptorSetAllocateInfo, pSetLayouts),
              "allocate info");
static_assert(sizeof(WriteDescriptorSet) == sizeof(VkWriteDescriptorSet) &&
                  offsetof(WriteDescriptorSet, dstSet) == offsetof(VkWriteDescriptorSet, dstSet) &&
                  offsetof(WriteDescriptorSet, dstBinding) == offsetof(VkWriteDescriptorSet, dstBinding) &&
                  offsetof(WriteDescriptorSet, descriptorCount) == offsetof(VkWriteDescriptorSet, descriptorCount) &&
                  offsetof(WriteDescriptorSet, descriptorType) == offsetof(VkWriteDescriptorSet, descriptorType) &&
                  offsetof(WriteDescriptorSet, pTexelBufferView) == offsetof(VkWriteDescriptorSet, pTexelBufferView),
              "write");
static_assert(sizeof(BufferCreateInfo) == sizeof(VkBufferCreateInfo) && offsetof(BufferCreateInfo, size) == offsetof(VkBufferCreateInfo, size) &&
                  offsetof(BufferCreateInfo, usage) == offsetof(VkBufferCreateInfo, usage),
              "buffer info");
static_assert(sizeof(MemoryRequirements) == sizeof(VkMemoryRequirements) &&
                  offsetof(MemoryRequirements, memoryTypeBits) == offsetof(VkMemoryRequirements, memoryTypeBits),
              "memory requirements");
static_assert(sizeof(MemoryAllocateInfo) == sizeof(VkMemoryAllocateInfo) &&
                  offsetof(MemoryAllocateInfo, memoryTypeIndex) == offsetof(VkMemoryAllocateInfo, memoryTypeIndex),
              "memory allocate info");
static_assert(sizeof(BufferViewCreateInfo) == sizeof(VkBufferViewCreateInfo) &&
                  offsetof(BufferViewCreateInfo, buffer) == offsetof(VkBufferViewCreateInfo, buffer) &&
                  offsetof(BufferViewCreateInfo, format) == offsetof(VkBufferViewCreateInfo, format) &&
                  offsetof(BufferViewCreateInfo, range) == offsetof(VkBufferViewCreateInfo, range),
              "view info");
static_assert(sizeof(PhysicalDeviceMemoryProperties) == sizeof(VkPhysicalDeviceMemoryProperties) &&
                  offsetof(PhysicalDeviceMemoryProperties, memoryHeapCount) == offsetof(VkPhysicalDeviceMemoryProperties, memoryHeapCount),
              "memory properties");
}  // namespace nevr_quest::combat::texel_buffer_guard::vk
#endif

namespace nevr_quest::combat::texel_buffer_guard {

namespace {

using sentinel::LogLevel;
namespace window = ::nevr_quest::vk_window;

using ProcFn = void (*)();
using LookupFn = ProcFn (*)(void* owner, const char* name);
using CreateLayoutFn = vk::Result (*)(void*, const vk::DescriptorSetLayoutCreateInfo*, const void*, vk::Handle*);
using AllocateFn = vk::Result (*)(void*, const vk::DescriptorSetAllocateInfo*, vk::Handle*);
using UpdateFn = void (*)(void*, std::uint32_t, const vk::WriteDescriptorSet*, std::uint32_t, const void*);
using CreateBufferFn = vk::Result (*)(void*, const vk::BufferCreateInfo*, const void*, vk::Handle*);
using BufferRequirementsFn = void (*)(void*, vk::Handle, vk::MemoryRequirements*);
using AllocateMemoryFn = vk::Result (*)(void*, const vk::MemoryAllocateInfo*, const void*, vk::Handle*);
using BindBufferMemoryFn = vk::Result (*)(void*, vk::Handle, vk::Handle, std::uint64_t);
using MapMemoryFn = vk::Result (*)(void*, vk::Handle, std::uint64_t, std::uint64_t, std::uint32_t, void**);
using CreateBufferViewFn = vk::Result (*)(void*, const vk::BufferViewCreateInfo*, const void*, vk::Handle*);
using EnumeratePhysicalDevicesFn = vk::Result (*)(void*, std::uint32_t*, void**);
using MemoryPropertiesFn = void (*)(void*, vk::PhysicalDeviceMemoryProperties*);

constexpr const char* kNames = "vkCreateDescriptorSetLayout vkAllocateDescriptorSets";
constexpr const char* kRequester = "texel_buffer_guard";

// The driver's two functions, published by vk_ext_window before it hands out the wrappers.
std::atomic<void*> g_realCreateLayout{nullptr};
std::atomic<void*> g_realAllocate{nullptr};
std::atomic<bool> g_requested{false};

std::mutex g_tableLock;
LayoutTable g_table;  // under g_tableLock

// The zeroed view: made once (g_makeLock), then read without the lock once g_view is kMade.
constexpr int kNotMade = 0;
constexpr int kMade = 1;
constexpr int kOff = 2;
std::mutex g_makeLock;
std::atomic<int> g_viewState{kNotMade};
void* g_device = nullptr;              // written before g_viewState becomes kMade
UpdateFn g_update = nullptr;           // the driver's vkUpdateDescriptorSets, written before kMade
vk::Handle g_views[kMaxDescriptors];   // kMaxDescriptors copies of the view, written before kMade

std::atomic<std::uint64_t> g_layouts{0}, g_sets{0}, g_bindings{0}, g_unknown{0}, g_skipped{0}, g_otherDevice{0}, g_tableFull{0};
std::atomic<bool> g_firstSaid{false}, g_fullSaid{false};

template <typename Fn>
Fn Lookup(LookupFn lookup, void* owner, const char* name) noexcept {
  return lookup != nullptr ? reinterpret_cast<Fn>(lookup(owner, name)) : nullptr;
}

bool MadeFailed(const char* step, vk::Result result, const char* why) noexcept {
  sentinel::LogFields(LogLevel::kWarn, "combat_texel_buffer_guard_zero_view",
                      {{"result", "failed"}, {"step", step}, {"vk_result", result}, {"bytes", 0}, {"memory_type", -1},
                       {"msg", why}});
  return false;
}

// The zeroed buffer and its view, with the game's `device` (g_makeLock held). Every object comes from the driver's own functions, reached
// through the lookups the game's own lookups returned.
bool Make(void* device) noexcept {
  constexpr const char* kOffMsg = "the zeroed view could not be made, so the guard is off for this process and every call is the driver's";
  const LookupFn gdpa = reinterpret_cast<LookupFn>(window::RealDeviceProcAddr());
  const LookupFn gipa = reinterpret_cast<LookupFn>(window::RealInstanceProcAddr());
  void* const instance = window::LookupInstance();
  const auto createBuffer = Lookup<CreateBufferFn>(gdpa, device, "vkCreateBuffer");
  const auto requirements = Lookup<BufferRequirementsFn>(gdpa, device, "vkGetBufferMemoryRequirements");
  const auto allocateMemory = Lookup<AllocateMemoryFn>(gdpa, device, "vkAllocateMemory");
  const auto bindMemory = Lookup<BindBufferMemoryFn>(gdpa, device, "vkBindBufferMemory");
  const auto mapMemory = Lookup<MapMemoryFn>(gdpa, device, "vkMapMemory");
  const auto createView = Lookup<CreateBufferViewFn>(gdpa, device, "vkCreateBufferView");
  const auto update = Lookup<UpdateFn>(gdpa, device, "vkUpdateDescriptorSets");
  const auto enumerate = Lookup<EnumeratePhysicalDevicesFn>(gipa, instance, "vkEnumeratePhysicalDevices");
  const auto memoryProperties = Lookup<MemoryPropertiesFn>(gipa, instance, "vkGetPhysicalDeviceMemoryProperties");
  if (instance == nullptr || createBuffer == nullptr || requirements == nullptr || allocateMemory == nullptr || bindMemory == nullptr ||
      mapMemory == nullptr || createView == nullptr || update == nullptr || enumerate == nullptr || memoryProperties == nullptr) {
    return MadeFailed("entry_points", vk::kSuccess, kOffMsg);
  }
  // The memory types are the physical device's. The game's device was made on the one the instance lists; more than one, and which is
  // the game's is not known here, so the guard does not guess.
  std::uint32_t physicalCount = 0;
  vk::Result r = enumerate(instance, &physicalCount, nullptr);
  if (r != vk::kSuccess || physicalCount != 1) return MadeFailed("physical_device", r, kOffMsg);
  void* physical = nullptr;
  r = enumerate(instance, &physicalCount, &physical);
  if (r != vk::kSuccess || physical == nullptr) return MadeFailed("physical_device", r, kOffMsg);

  vk::BufferCreateInfo bufferInfo{};
  bufferInfo.sType = vk::kStructureTypeBufferCreateInfo;
  bufferInfo.size = kZeroBufferBytes;
  bufferInfo.usage = vk::kBufferUsageUniformTexelBuffer;
  bufferInfo.sharingMode = vk::kSharingModeExclusive;
  vk::Handle buffer = 0;
  r = createBuffer(device, &bufferInfo, nullptr, &buffer);
  if (r != vk::kSuccess || buffer == 0) return MadeFailed("create_buffer", r, kOffMsg);
  vk::MemoryRequirements needs{};
  requirements(device, buffer, &needs);
  vk::PhysicalDeviceMemoryProperties props{};
  memoryProperties(physical, &props);
  const int type = PickMemoryType(props, needs.memoryTypeBits);
  if (type < 0 || needs.size < kZeroBufferBytes) return MadeFailed("memory_type", vk::kSuccess, kOffMsg);
  vk::MemoryAllocateInfo allocation{};
  allocation.sType = vk::kStructureTypeMemoryAllocateInfo;
  allocation.allocationSize = needs.size;
  allocation.memoryTypeIndex = static_cast<std::uint32_t>(type);
  vk::Handle memory = 0;
  r = allocateMemory(device, &allocation, nullptr, &memory);
  if (r != vk::kSuccess || memory == 0) return MadeFailed("allocate_memory", r, kOffMsg);
  r = bindMemory(device, buffer, memory, 0);
  if (r != vk::kSuccess) return MadeFailed("bind_memory", r, kOffMsg);
  void* mapped = nullptr;
  r = mapMemory(device, memory, 0, vk::kWholeSize, 0, &mapped);
  if (r != vk::kSuccess || mapped == nullptr) return MadeFailed("map_memory", r, kOffMsg);
  std::memset(mapped, 0, static_cast<std::size_t>(needs.size));  // coherent: the GPU reads these zeros with no flush; never written again
  vk::BufferViewCreateInfo viewInfo{};
  viewInfo.sType = vk::kStructureTypeBufferViewCreateInfo;
  viewInfo.buffer = buffer;
  viewInfo.format = vk::kFormatR32G32B32A32Sfloat;
  viewInfo.offset = 0;
  viewInfo.range = vk::kWholeSize;
  vk::Handle view = 0;
  r = createView(device, &viewInfo, nullptr, &view);
  if (r != vk::kSuccess || view == 0) return MadeFailed("create_view", r, kOffMsg);

  for (vk::Handle& v : g_views) v = view;
  g_update = update;
  g_device = device;
  sentinel::LogFields(LogLevel::kInfo, "combat_texel_buffer_guard_zero_view",
                      {{"result", "made"}, {"step", "done"}, {"vk_result", vk::kSuccess}, {"bytes", needs.size}, {"memory_type", type},
                       {"msg", "a zeroed uniform texel buffer view is made: a new set's unwritten uniform texel bindings read zeros"}});
  return true;
}

// The view exists for `device`'s guard: true when made (by this call or an earlier one), false when the guard is off.
bool EnsureZeroView(void* device) noexcept {
  const int state = g_viewState.load(std::memory_order_acquire);
  if (state != kNotMade) return state == kMade;
  std::lock_guard<std::mutex> hold(g_makeLock);
  const int again = g_viewState.load(std::memory_order_acquire);
  if (again != kNotMade) return again == kMade;
  const bool made = Make(device);
  g_viewState.store(made ? kMade : kOff, std::memory_order_release);
  return made;
}

void NoteSetWritten(std::uint32_t writes) noexcept {
  const std::uint64_t sets = g_sets.fetch_add(1, std::memory_order_relaxed) + 1;
  g_bindings.fetch_add(writes, std::memory_order_relaxed);
  if ((sets & (sets - 1)) != 0) return;
  sentinel::LogFields(LogLevel::kDebug, "combat_texel_buffer_guard_count",
                      {{"sets", sets}, {"bindings", g_bindings.load(std::memory_order_relaxed)},
                       {"unknown_layouts", g_unknown.load(std::memory_order_relaxed)},
                       {"skipped_flags", g_skipped.load(std::memory_order_relaxed)},
                       {"other_device", g_otherDevice.load(std::memory_order_relaxed)}});
}

// Writes the zeroed view into each recorded binding of each set of a successful allocation.
void WriteNewSets(void* device, const vk::DescriptorSetAllocateInfo& info, const vk::Handle* sets) noexcept {
  vk::WriteDescriptorSet writes[kWriteBatch];
  std::uint32_t pending = 0;
  std::uint64_t setsWritten = 0, bindingsWritten = 0;
  const auto flush = [&]() noexcept {
    if (pending != 0) g_update(device, pending, writes, 0, nullptr);
    pending = 0;
  };
  for (std::uint32_t i = 0; i < info.descriptorSetCount; ++i) {
    LayoutPicks picks;
    bool known = false;
    {
      std::lock_guard<std::mutex> hold(g_tableLock);
      known = g_table.Find(info.pSetLayouts[i], &picks);
    }
    if (!known || picks.verdict == Verdict::kNone) {
      g_unknown.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (picks.verdict != Verdict::kPicked) {
      g_skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (!EnsureZeroView(device)) return;
    if (device != g_device) {
      g_otherDevice.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    for (std::uint32_t k = 0; k < picks.n; ++k) {
      if (pending == kWriteBatch) flush();
      vk::WriteDescriptorSet& w = writes[pending++];
      w = vk::WriteDescriptorSet{};
      w.sType = vk::kStructureTypeWriteDescriptorSet;
      w.dstSet = sets[i];
      w.dstBinding = picks.picks[k].binding;
      w.dstArrayElement = 0;
      w.descriptorCount = picks.picks[k].count;
      w.descriptorType = vk::kDescriptorTypeUniformTexelBuffer;
      w.pTexelBufferView = g_views;
    }
    ++setsWritten;
    bindingsWritten += picks.n;
    NoteSetWritten(picks.n);
  }
  flush();
  if (setsWritten != 0 && !g_firstSaid.exchange(true, std::memory_order_acq_rel)) {
    sentinel::LogFields(LogLevel::kInfo, "combat_texel_buffer_guard_first",
                        {{"sets", setsWritten}, {"bindings", bindingsWritten},
                         {"msg", "the first new sets with uniform texel bindings hold the zeroed view until the game writes its own"}});
  }
}

}  // namespace

vk::Result CreateDescriptorSetLayout(void* device, const vk::DescriptorSetLayoutCreateInfo* info, const void* allocator,
                                     vk::Handle* layout) noexcept {
  const auto real = reinterpret_cast<CreateLayoutFn>(g_realCreateLayout.load(std::memory_order_acquire));
  if (real == nullptr) return vk::kErrorInitializationFailed;  // unreachable: the wrapper is handed out after the store
  const vk::Result result = real(device, info, allocator, layout);
  if (result != vk::kSuccess || info == nullptr || layout == nullptr || g_viewState.load(std::memory_order_acquire) == kOff) return result;
  const LayoutPicks picks = PickBindings(info);
  LayoutTable::Put put;
  {
    std::lock_guard<std::mutex> hold(g_tableLock);
    put = g_table.Record(*layout, picks);
  }
  if (put == LayoutTable::Put::kFull) {
    g_tableFull.fetch_add(1, std::memory_order_relaxed);
    if (!g_fullSaid.exchange(true, std::memory_order_acq_rel)) {
      sentinel::LogFields(LogLevel::kWarn, "combat_texel_buffer_guard_table_full",
                          {{"capacity", kTableCapacity},
                           {"msg", "the layout table is full: sets of a layout created from now on get no zeroed view (counted)"}});
    }
  } else if (put != LayoutTable::Put::kNotStored && picks.verdict != Verdict::kNone) {
    g_layouts.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

vk::Result AllocateDescriptorSets(void* device, const vk::DescriptorSetAllocateInfo* info, vk::Handle* sets) noexcept {
  const auto real = reinterpret_cast<AllocateFn>(g_realAllocate.load(std::memory_order_acquire));
  if (real == nullptr) return vk::kErrorInitializationFailed;  // unreachable: the wrapper is handed out after the store
  const vk::Result result = real(device, info, sets);
  if (result != vk::kSuccess || info == nullptr || sets == nullptr || info->pSetLayouts == nullptr ||
      g_viewState.load(std::memory_order_acquire) == kOff) {
    return result;
  }
  WriteNewSets(device, *info, sets);
  return result;
}

ModuleResult Early(bool on) noexcept {
  if (!on) {
    sentinel::LogFields(LogLevel::kInfo, "combat_texel_buffer_guard_install",
                        {{"result", "off_config"}, {"reason", "off_config"}, {"names", kNames}, {"owner", "vk_ext_window"},
                         {"msg", "combat.texel_buffer_guard is false: the game's Vulkan lookups are unchanged and an unwritten uniform "
                                 "texel binding reads what the driver leaves there"}});
    return {false, "off", true};
  }
  const window::RequestStatus create = window::RequestDeviceFunction(
      kCreateLayoutName, reinterpret_cast<window::ProcFn>(&CreateDescriptorSetLayout), &g_realCreateLayout, kRequester);
  const window::RequestStatus allocate =
      create != window::RequestStatus::kOk
          ? create
          : window::RequestDeviceFunction(kAllocateName, reinterpret_cast<window::ProcFn>(&AllocateDescriptorSets), &g_realAllocate, kRequester);
  if (create == window::RequestStatus::kOk) g_requested.store(true, std::memory_order_release);
  if (allocate != window::RequestStatus::kOk) {
    const char* reason = window::RequestStatusName(allocate);
    sentinel::LogFields(LogLevel::kError, "combat_texel_buffer_guard_install",
                        {{"result", "refused"}, {"reason", reason}, {"names", kNames}, {"owner", "vk_ext_window"},
                         {"msg", "vk_ext_window refused the request, so no new set gets the zeroed view"}});
    return {false, reason};
  }
  sentinel::LogFields(LogLevel::kInfo, "combat_texel_buffer_guard_install",
                      {{"result", "requested"}, {"reason", "ok"}, {"names", kNames}, {"owner", "vk_ext_window"},
                       {"msg", "the game's lookups of the two functions will get the guard's wrappers: a new set's uniform texel "
                               "bindings hold a zeroed view until the game writes its own"}});
  return {true, "requested"};
}

void End() noexcept {
  if (!g_requested.load(std::memory_order_acquire)) return;
  const bool wrapped = g_realCreateLayout.load(std::memory_order_acquire) != nullptr && g_realAllocate.load(std::memory_order_acquire) != nullptr;
  const Counts c = CurrentCounts();
  sentinel::LogFields(wrapped ? LogLevel::kInfo : LogLevel::kWarn, "combat_texel_buffer_guard_end",
                      {{"wrapped", wrapped ? 1 : 0}, {"layouts", c.layouts}, {"sets", c.sets}, {"bindings", c.bindings},
                       {"unknown_layouts", c.unknownLayouts}, {"skipped_flags", c.skippedFlags}, {"other_device", c.otherDevice},
                       {"table_full", c.tableFull},
                       {"msg", wrapped ? "sets is every new set the guard wrote the zeroed view into, bindings the writes"
                                       : "the guard was requested but the game never took its wrappers: no set got the zeroed view"}});
}

Counts CurrentCounts() noexcept {
  Counts c;
  c.layouts = g_layouts.load(std::memory_order_relaxed);
  c.sets = g_sets.load(std::memory_order_relaxed);
  c.bindings = g_bindings.load(std::memory_order_relaxed);
  c.unknownLayouts = g_unknown.load(std::memory_order_relaxed);
  c.skippedFlags = g_skipped.load(std::memory_order_relaxed);
  c.otherDevice = g_otherDevice.load(std::memory_order_relaxed);
  c.tableFull = g_tableFull.load(std::memory_order_relaxed);
  return c;
}

bool ZeroViewMade() noexcept { return g_viewState.load(std::memory_order_acquire) == kMade; }
bool TurnedOff() noexcept { return g_viewState.load(std::memory_order_acquire) == kOff; }

void ResetForTest() noexcept {
  g_realCreateLayout.store(nullptr);
  g_realAllocate.store(nullptr);
  g_requested.store(false);
  {
    std::lock_guard<std::mutex> hold(g_tableLock);
    g_table.Clear();
  }
  g_viewState.store(kNotMade);
  g_device = nullptr;
  g_update = nullptr;
  for (vk::Handle& v : g_views) v = 0;
  for (auto* a : {&g_layouts, &g_sets, &g_bindings, &g_unknown, &g_skipped, &g_otherDevice, &g_tableFull}) a->store(0);
  g_firstSaid.store(false);
  g_fullSaid.store(false);
}

}  // namespace nevr_quest::combat::texel_buffer_guard
