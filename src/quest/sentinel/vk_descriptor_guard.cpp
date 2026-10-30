#include "vk_descriptor_guard.h"

#include <time.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>

#include "../plugins/q15t/src/module_switch.h"
#include "hook_log.h"

#if __has_include(<vulkan/vulkan_core.h>)
#include <vulkan/vulkan_core.h>
// The mirror in the header against the real ABI, wherever the real header is available (the NDK always has it).
#define NEVR_GUARD_SAME(ours, theirs, field) \
  static_assert(offsetof(ours, field) == offsetof(theirs, field), #ours "." #field " is not at Vulkan's offset")
static_assert(sizeof(nevr_quest::vk_guard::DslBinding) == sizeof(VkDescriptorSetLayoutBinding));
NEVR_GUARD_SAME(nevr_quest::vk_guard::DslBinding, VkDescriptorSetLayoutBinding, descriptorCount);
NEVR_GUARD_SAME(nevr_quest::vk_guard::DslCreateInfo, VkDescriptorSetLayoutCreateInfo, pBindings);
NEVR_GUARD_SAME(nevr_quest::vk_guard::SetAllocateInfo, VkDescriptorSetAllocateInfo, pSetLayouts);
static_assert(sizeof(nevr_quest::vk_guard::WriteDescriptorSet) == sizeof(VkWriteDescriptorSet));
NEVR_GUARD_SAME(nevr_quest::vk_guard::WriteDescriptorSet, VkWriteDescriptorSet, pTexelBufferView);
static_assert(sizeof(nevr_quest::vk_guard::BufferCreateInfo) == sizeof(VkBufferCreateInfo));
NEVR_GUARD_SAME(nevr_quest::vk_guard::BufferCreateInfo, VkBufferCreateInfo, usage);
static_assert(sizeof(nevr_quest::vk_guard::BufferViewCreateInfo) == sizeof(VkBufferViewCreateInfo));
NEVR_GUARD_SAME(nevr_quest::vk_guard::BufferViewCreateInfo, VkBufferViewCreateInfo, range);
static_assert(sizeof(nevr_quest::vk_guard::MemoryAllocateInfo) == sizeof(VkMemoryAllocateInfo));
static_assert(sizeof(nevr_quest::vk_guard::MemoryRequirements) == sizeof(VkMemoryRequirements));
static_assert(sizeof(nevr_quest::vk_guard::PhysicalDeviceMemoryProperties) == sizeof(VkPhysicalDeviceMemoryProperties));
NEVR_GUARD_SAME(nevr_quest::vk_guard::PhysicalDeviceMemoryProperties, VkPhysicalDeviceMemoryProperties, memoryHeaps);
static_assert(nevr_quest::vk_guard::kStypeBindingFlagsCreateInfo ==
              static_cast<std::uint32_t>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO));
static_assert(nevr_quest::vk_guard::kStypeWriteDescriptorSet == static_cast<std::uint32_t>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET));
static_assert(nevr_quest::vk_guard::kStypeBufferCreateInfo == static_cast<std::uint32_t>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO));
static_assert(nevr_quest::vk_guard::kStypeBufferViewCreateInfo == static_cast<std::uint32_t>(VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO));
static_assert(nevr_quest::vk_guard::kStypeMemoryAllocateInfo == static_cast<std::uint32_t>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO));
static_assert(nevr_quest::vk_guard::kFormatR32G32B32A32Sfloat == static_cast<std::uint32_t>(VK_FORMAT_R32G32B32A32_SFLOAT));
static_assert(nevr_quest::vk_guard::kDescriptorTypeUniformTexelBuffer == static_cast<std::uint32_t>(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER));
static_assert(nevr_quest::vk_guard::kBufferUsageUniformTexelBuffer == static_cast<std::uint32_t>(VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT));
#undef NEVR_GUARD_SAME
#endif

namespace nevr_quest::vk_guard {

// ---- the decision ------------------------------------------------------------------------------------------------------

LayoutPlan PlanLayout(const DslCreateInfo* ci) noexcept {
  LayoutPlan plan;
  if (ci == nullptr) return plan;
  for (const BaseIn* p = static_cast<const BaseIn*>(ci->pNext); p != nullptr; p = p->pNext) {
    if (p->sType == kStypeBindingFlagsCreateInfo) plan.skip = true;
  }
  if (plan.skip || ci->pBindings == nullptr) return plan;
  for (std::uint32_t i = 0; i < ci->bindingCount; ++i) {
    const DslBinding& b = ci->pBindings[i];
    if (b.descriptorType != kDescriptorTypeUniformTexelBuffer || b.descriptorCount == 0) continue;
    if (plan.count == kMaxBindingsPerLayout) {
      plan.truncated = true;
      break;
    }
    TexelBinding& t = plan.bindings[plan.count++];
    t.binding = b.binding;
    t.count = b.descriptorCount;
    if (t.count > kMaxDescriptorsPerBinding) {
      t.count = kMaxDescriptorsPerBinding;
      plan.truncated = true;
    }
  }
  return plan;
}

bool IsReportCount(std::uint64_t n) noexcept { return n != 0 && (n & (n - 1)) == 0; }

namespace {

using GdpaFn = VoidFn (*)(void* device, const char* name);
using CreateDslFn = std::int32_t (*)(void* device, const DslCreateInfo* ci, const void* alloc, std::uint64_t* out);
using AllocSetsFn = std::int32_t (*)(void* device, const SetAllocateInfo* ai, std::uint64_t* out);
using UpdateSetsFn = void (*)(void* device, std::uint32_t writes, const WriteDescriptorSet* w, std::uint32_t copies,
                              const void* c);
using CreateBufferFn = std::int32_t (*)(void* device, const BufferCreateInfo* ci, const void* alloc, std::uint64_t* out);
using BufferReqsFn = void (*)(void* device, std::uint64_t buffer, MemoryRequirements* out);
using AllocMemoryFn = std::int32_t (*)(void* device, const MemoryAllocateInfo* ai, const void* alloc, std::uint64_t* out);
using BindMemoryFn = std::int32_t (*)(void* device, std::uint64_t buffer, std::uint64_t memory, std::uint64_t offset);
using MapMemoryFn = std::int32_t (*)(void* device, std::uint64_t memory, std::uint64_t offset, std::uint64_t size,
                                     std::uint32_t flags, void** out);
using CreateViewFn = std::int32_t (*)(void* device, const BufferViewCreateInfo* ci, const void* alloc, std::uint64_t* out);
using EnumPdFn = std::int32_t (*)(void* instance, std::uint32_t* count, void** out);
using MemPropsFn = void (*)(void* physical_device, PhysicalDeviceMemoryProperties* out);

constexpr const char* kGetDeviceProcAddr = "vkGetDeviceProcAddr";
constexpr const char* kCreateDsl = "vkCreateDescriptorSetLayout";
constexpr const char* kAllocSets = "vkAllocateDescriptorSets";
constexpr const char* kOffFile = "g40guard.off";
constexpr std::uint64_t kBufferBytes = 65536;
constexpr std::size_t kTableSize = 4096;  // power of two
constexpr std::size_t kMaxWrites = 64;    // writes per vkUpdateDescriptorSets call

// The decision, made once (a compare-exchange rather than std::call_once, so ResetForTest can clear it).
enum class Decision : int { kUndecided = 0, kOn = 1, kOff = 2 };
std::atomic<int> g_decision{0};

// The real functions as the game's resolution returned them.
std::atomic<void*> g_realGipa{nullptr};
std::atomic<void*> g_instance{nullptr};
std::atomic<void*> g_realGdpa{nullptr};
std::atomic<void*> g_realCreateDsl{nullptr};
std::atomic<void*> g_realAllocSets{nullptr};
std::atomic<bool> g_loggedCreateDsl{false};
std::atomic<bool> g_loggedAllocSets{false};

std::int64_t (*g_clock)() = nullptr;

std::int64_t NowMs() noexcept {
  if (g_clock != nullptr) return g_clock();
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Layout table (g_lock): open addressing on the layout handle. A handle the driver reuses after a destroy replaces its
// entry, so a stale plan never outlives the next create of that value; a layout with no texel binding that reuses a tracked
// handle clears the plan (count 0).
struct Entry {
  std::uint64_t handle = 0;
  LayoutPlan plan;
};
std::mutex g_lock;
Entry g_table[kTableSize];
std::uint64_t g_layouts = 0;
std::uint64_t g_layoutsLost = 0;

// The zeroed buffer (g_lock).
struct Fill {
  bool tried = false;
  bool ok = false;
  void* device = nullptr;
  std::uint64_t view = 0;
  UpdateSetsFn update = nullptr;
};
Fill g_fill;
std::uint64_t g_views[kMaxDescriptorsPerBinding];
WriteDescriptorSet g_writes[kMaxWrites];
std::uint64_t g_setsFilled = 0;
std::uint64_t g_writeCount = 0;
std::uint64_t g_otherDevice = 0;
std::int64_t g_firstFillMs = 0;

std::size_t Slot(std::uint64_t h) noexcept { return static_cast<std::size_t>((h ^ (h >> 17)) * 0x9E3779B97F4A7C15ULL >> 52) & (kTableSize - 1); }

Entry* Find(std::uint64_t h) noexcept {
  for (std::size_t i = Slot(h), n = 0; n < kTableSize; i = (i + 1) & (kTableSize - 1), ++n) {
    if (g_table[i].handle == h) return &g_table[i];
    if (g_table[i].handle == 0) return nullptr;
  }
  return nullptr;
}

// The handle's entry, or a free one claimed for it; nullptr when the table is full.
Entry* Claim(std::uint64_t h) noexcept {
  for (std::size_t i = Slot(h), n = 0; n < kTableSize; i = (i + 1) & (kTableSize - 1), ++n) {
    if (g_table[i].handle == h) return &g_table[i];
    if (g_table[i].handle == 0) {
      g_table[i].handle = h;
      ++g_layouts;
      return &g_table[i];
    }
  }
  return nullptr;
}

void LogHook(const char* name, std::atomic<bool>* logged) noexcept {
  if (logged->exchange(true)) return;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "g40guard_hook", {{"function", name}, {"status", "interposed"}});
}

// The buffer, its memory and its view, made once on `device` (g_lock held). Each failure names the step and its result.
bool MakeFill(void* device) noexcept {
  if (g_fill.tried) return g_fill.ok;
  g_fill.tried = true;
  g_fill.device = device;
  const GdpaFn gdpa = reinterpret_cast<GdpaFn>(g_realGdpa.load(std::memory_order_acquire));
  const GipaFn gipa = reinterpret_cast<GipaFn>(g_realGipa.load(std::memory_order_acquire));
  void* const instance = g_instance.load(std::memory_order_acquire);
  const char* step = "entry_points";
  std::int32_t result = kVkSuccess;
  std::uint32_t type = ~0u;
  std::uint32_t devices = 0;
  std::uint64_t buffer = 0, memory = 0, bytes = 0;
  do {
    if (gdpa == nullptr || gipa == nullptr || instance == nullptr) break;
    const auto create = reinterpret_cast<CreateBufferFn>(gdpa(device, "vkCreateBuffer"));
    const auto reqs = reinterpret_cast<BufferReqsFn>(gdpa(device, "vkGetBufferMemoryRequirements"));
    const auto alloc = reinterpret_cast<AllocMemoryFn>(gdpa(device, "vkAllocateMemory"));
    const auto bind = reinterpret_cast<BindMemoryFn>(gdpa(device, "vkBindBufferMemory"));
    const auto map = reinterpret_cast<MapMemoryFn>(gdpa(device, "vkMapMemory"));
    const auto view = reinterpret_cast<CreateViewFn>(gdpa(device, "vkCreateBufferView"));
    const auto update = reinterpret_cast<UpdateSetsFn>(gdpa(device, "vkUpdateDescriptorSets"));
    const auto enumerate = reinterpret_cast<EnumPdFn>(gipa(instance, "vkEnumeratePhysicalDevices"));
    const auto props = reinterpret_cast<MemPropsFn>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
    if (!create || !reqs || !alloc || !bind || !map || !view || !update || !enumerate || !props) break;

    step = "physical_device";
    result = enumerate(instance, &devices, nullptr);
    if (result != kVkSuccess || devices == 0) break;
    // The Quest has one GPU; with more than one the first is taken (the line carries the count).
    void* pd = nullptr;
    std::uint32_t one = 1;
    result = enumerate(instance, &one, &pd);
    if (result < 0 || pd == nullptr) break;

    step = "create_buffer";
    BufferCreateInfo bi{};
    bi.sType = kStypeBufferCreateInfo;
    bi.size = kBufferBytes;
    bi.usage = kBufferUsageUniformTexelBuffer;
    result = create(device, &bi, nullptr, &buffer);
    if (result != kVkSuccess) break;

    step = "memory_type";
    MemoryRequirements mr{};
    reqs(device, buffer, &mr);
    PhysicalDeviceMemoryProperties mp{};
    props(pd, &mp);
    const std::uint32_t want = kMemoryHostVisible | kMemoryHostCoherent;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount && i < 32 && type == ~0u; ++i) {
      if ((mr.memoryTypeBits & (1u << i)) != 0 && (mp.memoryTypes[i].propertyFlags & want) == want) type = i;
    }
    if (type == ~0u) break;

    step = "allocate_memory";
    MemoryAllocateInfo ma{};
    ma.sType = kStypeMemoryAllocateInfo;
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = type;
    bytes = mr.size;
    result = alloc(device, &ma, nullptr, &memory);
    if (result != kVkSuccess) break;
    step = "bind_memory";
    result = bind(device, buffer, memory, 0);
    if (result != kVkSuccess) break;
    step = "map_memory";
    void* p = nullptr;
    result = map(device, memory, 0, kWholeSize, 0, &p);
    if (result != kVkSuccess || p == nullptr) break;
    std::memset(p, 0, static_cast<std::size_t>(mr.size));  // coherent; stays mapped and is never written again

    step = "create_view";
    BufferViewCreateInfo vi{};
    vi.sType = kStypeBufferViewCreateInfo;
    vi.buffer = buffer;
    vi.format = kFormatR32G32B32A32Sfloat;
    vi.range = kWholeSize;
    result = view(device, &vi, nullptr, &g_fill.view);
    if (result != kVkSuccess) break;

    for (std::uint64_t& v : g_views) v = g_fill.view;
    g_fill.update = update;
    g_fill.ok = true;
    step = "done";
  } while (false);

  char devHex[19], bufHex[19], viewHex[19];
  sentinel::LogFields(g_fill.ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "g40guard_buffer",
                      {{"result", g_fill.ok ? "made" : "failed"},
                       {"step", step},
                       {"vk_result", result},
                       {"device", sentinel::HexString(devHex, reinterpret_cast<std::uintptr_t>(device))},
                       {"physical_devices", devices},
                       {"buffer", sentinel::HexString(bufHex, buffer)},
                       {"view", sentinel::HexString(viewHex, g_fill.view)},
                       {"bytes", bytes},
                       {"memory_type", static_cast<long long>(type == ~0u ? -1 : static_cast<long long>(type))},
                       {"consequence", g_fill.ok ? "unwritten uniform texel bindings read zeros"
                                                 : "guard off: unwritten uniform texel bindings stay undefined"}});
  return g_fill.ok;
}

std::int32_t WrappedCreateDsl(void* device, const DslCreateInfo* ci, const void* alloc, std::uint64_t* out) {
  const CreateDslFn real = reinterpret_cast<CreateDslFn>(g_realCreateDsl.load(std::memory_order_acquire));
  if (real == nullptr) return -3;  // VK_ERROR_INITIALIZATION_FAILED; unreachable, the wrapper is handed out after the store
  const std::int32_t r = real(device, ci, alloc, out);
  if (r != kVkSuccess || out == nullptr || *out == 0) return r;
  const LayoutPlan plan = PlanLayout(ci);
  std::lock_guard<std::mutex> l(g_lock);
  if (plan.count == 0 || plan.skip) {
    if (Entry* e = Find(*out)) e->plan = LayoutPlan{};  // a reused handle: the old plan no longer applies
    return r;
  }
  if (Entry* e = Claim(*out)) {
    e->plan = plan;
  } else {
    ++g_layoutsLost;
    if (IsReportCount(g_layoutsLost)) {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "g40guard_table_full",
                          {{"layouts_lost", g_layoutsLost}, {"capacity", kTableSize},
                           {"consequence", "sets on these layouts are not filled"}});
    }
  }
  return r;
}

void Flush(void* device, std::size_t* n) noexcept {
  if (*n == 0) return;
  g_fill.update(device, static_cast<std::uint32_t>(*n), g_writes, 0, nullptr);
  g_writeCount += *n;
  *n = 0;
}

std::int32_t WrappedAllocSets(void* device, const SetAllocateInfo* ai, std::uint64_t* out) {
  const AllocSetsFn real = reinterpret_cast<AllocSetsFn>(g_realAllocSets.load(std::memory_order_acquire));
  if (real == nullptr) return -3;
  const std::int32_t r = real(device, ai, out);
  if (r != kVkSuccess || ai == nullptr || ai->pSetLayouts == nullptr || out == nullptr) return r;

  std::uint64_t filledNow = 0;
  bool report = false;
  {
    std::lock_guard<std::mutex> l(g_lock);
    // Only allocations on a layout we track pay for anything beyond these lookups.
    bool any = false;
    for (std::uint32_t i = 0; i < ai->descriptorSetCount && !any; ++i) {
      const Entry* e = Find(ai->pSetLayouts[i]);
      any = e != nullptr && e->plan.count != 0;
    }
    if (!any) return r;
    if (!MakeFill(device)) return r;
    if (device != g_fill.device) {
      ++g_otherDevice;
      return r;
    }
    std::size_t n = 0;
    for (std::uint32_t i = 0; i < ai->descriptorSetCount; ++i) {
      const Entry* e = Find(ai->pSetLayouts[i]);
      if (e == nullptr || e->plan.count == 0 || out[i] == 0) continue;
      for (std::size_t b = 0; b < e->plan.count; ++b) {
        if (n == kMaxWrites) Flush(device, &n);
        WriteDescriptorSet& w = g_writes[n++];
        w = WriteDescriptorSet{};
        w.sType = kStypeWriteDescriptorSet;
        w.dstSet = out[i];
        w.dstBinding = e->plan.bindings[b].binding;
        w.descriptorCount = e->plan.bindings[b].count;
        w.descriptorType = kDescriptorTypeUniformTexelBuffer;
        w.pTexelBufferView = g_views;
      }
      ++filledNow;
    }
    Flush(device, &n);
    if (filledNow != 0 && g_setsFilled == 0) g_firstFillMs = NowMs();
    for (std::uint64_t k = 1; k <= filledNow; ++k) report = report || IsReportCount(g_setsFilled + k);
    g_setsFilled += filledNow;
    if (report) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "g40guard_fill",
                          {{"sets_filled", g_setsFilled},
                           {"writes", g_writeCount},
                           {"layouts", g_layouts},
                           {"layouts_lost", g_layoutsLost},
                           {"other_device", g_otherDevice},
                           {"ms_since_first_fill", NowMs() - g_firstFillMs}});
    }
  }
  return r;
}

VoidFn WrappedGetDeviceProcAddr(void* device, const char* name) {
  const GdpaFn real = reinterpret_cast<GdpaFn>(g_realGdpa.load(std::memory_order_acquire));
  if (real == nullptr) return nullptr;
  const VoidFn found = real(device, name);
  if (found == nullptr || name == nullptr) return found;
  if (std::strcmp(name, kGetDeviceProcAddr) == 0) return reinterpret_cast<VoidFn>(&WrappedGetDeviceProcAddr);
  if (std::strcmp(name, kCreateDsl) == 0) {
    g_realCreateDsl.store(reinterpret_cast<void*>(found), std::memory_order_release);
    LogHook(kCreateDsl, &g_loggedCreateDsl);
    return reinterpret_cast<VoidFn>(&WrappedCreateDsl);
  }
  if (std::strcmp(name, kAllocSets) == 0) {
    g_realAllocSets.store(reinterpret_cast<void*>(found), std::memory_order_release);
    LogHook(kAllocSets, &g_loggedAllocSets);
    return reinterpret_cast<VoidFn>(&WrappedAllocSets);
  }
  return found;
}

// The switch, read once; logs q15t_g40guard_install with the outcome.
bool Decide() noexcept {
  const int seen = g_decision.load(std::memory_order_acquire);
  if (seen != static_cast<int>(Decision::kUndecided)) return seen == static_cast<int>(Decision::kOn);
  const bool off = q15t::ModuleSwitchPresent(kOffFile);
  int expected = static_cast<int>(Decision::kUndecided);
  const int want = static_cast<int>(off ? Decision::kOff : Decision::kOn);
  if (g_decision.compare_exchange_strong(expected, want, std::memory_order_acq_rel)) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "q15t_g40guard_install",
                        {{"result", off ? "off" : "on"},
                         {"reason", off ? "off_file" : "default_on"},
                         {"switch", kOffFile},
                         {"hooks", off ? "none" : "vkCreateDescriptorSetLayout vkAllocateDescriptorSets"},
                         {"via", "vkGetInstanceProcAddr(vkGetDeviceProcAddr)"},
                         {"consequence", off ? "unwritten uniform texel bindings stay undefined (GPU fault 0x1FF0000000000)"
                                             : "unwritten uniform texel bindings will read zeros"}});
    return !off;
  }
  return expected == static_cast<int>(Decision::kOn);
}

}  // namespace

VoidFn OnInstanceProcAddr(GipaFn real, void* instance, const char* name, VoidFn found) noexcept {
  if (found == nullptr || name == nullptr || std::strcmp(name, kGetDeviceProcAddr) != 0) return found;
  if (!Decide()) return found;
  g_realGipa.store(reinterpret_cast<void*>(real), std::memory_order_release);
  if (instance != nullptr) g_instance.store(instance, std::memory_order_release);
  g_realGdpa.store(reinterpret_cast<void*>(found), std::memory_order_release);
  return reinterpret_cast<VoidFn>(&WrappedGetDeviceProcAddr);
}

Stats GetStats() noexcept {
  Stats s;
  const int d = g_decision.load(std::memory_order_acquire);
  s.decided = d != static_cast<int>(Decision::kUndecided);
  s.on = d == static_cast<int>(Decision::kOn);
  std::lock_guard<std::mutex> l(g_lock);
  s.buffer_ok = g_fill.ok;
  s.sets_filled = g_setsFilled;
  s.writes = g_writeCount;
  s.other_device = g_otherDevice;
  s.layouts = g_layouts;
  s.layouts_lost = g_layoutsLost;
  return s;
}

void SetClockForTest(std::int64_t (*clock_ms)()) noexcept { g_clock = clock_ms; }

void ResetForTest() noexcept {
  std::lock_guard<std::mutex> l(g_lock);
  g_decision.store(0, std::memory_order_relaxed);
  g_realGipa.store(nullptr);
  g_instance.store(nullptr);
  g_realGdpa.store(nullptr);
  g_realCreateDsl.store(nullptr);
  g_realAllocSets.store(nullptr);
  g_loggedCreateDsl.store(false);
  g_loggedAllocSets.store(false);
  for (Entry& e : g_table) e = Entry{};
  g_layouts = g_layoutsLost = 0;
  g_fill = Fill{};
  for (std::uint64_t& v : g_views) v = 0;
  g_setsFilled = g_writeCount = g_otherDevice = 0;
  g_firstFillMs = 0;
}

}  // namespace nevr_quest::vk_guard
