/* The texel-buffer guard's rules (texel_buffer_guard.h): which bindings of a descriptor set layout get the zeroed view, and the table that
 * keeps them per layout handle. Pure: no game type, no allocation, no I/O, no lock (the caller holds its own).
 *
 * The Vulkan types here are this file's own, laid out as the Vulkan ABI lays them out on a 64-bit target (LP64: a non-dispatchable handle is
 * 8 bytes, a dispatchable one a pointer). The sentinel and its host tests do not include <vulkan/vulkan.h> (a host test must build where no
 * Vulkan SDK is installed); texel_buffer_guard.cpp checks every size, offset and constant below against <vulkan/vulkan.h> where that header
 * exists (the NDK always has it), so a difference fails the build.
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace nevr_quest::combat::texel_buffer_guard {

namespace vk {

using Result = std::int32_t;
using Handle = std::uint64_t;  // a non-dispatchable handle (VkDescriptorSetLayout, VkDescriptorSet, VkBuffer, VkBufferView, VkDeviceMemory)

inline constexpr Result kSuccess = 0;
inline constexpr Result kErrorInitializationFailed = -3;
inline constexpr std::uint32_t kDescriptorTypeUniformTexelBuffer = 4;                        // VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
inline constexpr std::uint32_t kStructureTypeMemoryAllocateInfo = 5;                         // VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
inline constexpr std::uint32_t kStructureTypeBufferCreateInfo = 12;                          // VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
inline constexpr std::uint32_t kStructureTypeBufferViewCreateInfo = 13;                      // VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO
inline constexpr std::uint32_t kStructureTypeWriteDescriptorSet = 35;                        // VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
inline constexpr std::uint32_t kStructureTypeBindingFlagsCreateInfo = 1000161000;            // ..._DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO
inline constexpr std::uint32_t kBufferUsageUniformTexelBuffer = 0x4;                         // VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT
inline constexpr std::uint32_t kSharingModeExclusive = 0;                                    // VK_SHARING_MODE_EXCLUSIVE
inline constexpr std::uint32_t kFormatR32G32B32A32Sfloat = 109;                              // VK_FORMAT_R32G32B32A32_SFLOAT
inline constexpr std::uint32_t kMemoryPropertyHostVisible = 0x2;                             // VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
inline constexpr std::uint32_t kMemoryPropertyHostCoherent = 0x4;                            // VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
inline constexpr std::uint64_t kWholeSize = ~0ULL;                                           // VK_WHOLE_SIZE
inline constexpr std::uint32_t kMaxMemoryTypes = 32;                                         // VK_MAX_MEMORY_TYPES
inline constexpr std::uint32_t kMaxMemoryHeaps = 16;                                         // VK_MAX_MEMORY_HEAPS

struct BaseIn {  // VkBaseInStructure
  std::uint32_t sType;
  const BaseIn* pNext;
};
struct DescriptorSetLayoutBinding {  // VkDescriptorSetLayoutBinding
  std::uint32_t binding;
  std::uint32_t descriptorType;
  std::uint32_t descriptorCount;
  std::uint32_t stageFlags;
  const void* pImmutableSamplers;
};
struct DescriptorSetLayoutCreateInfo {  // VkDescriptorSetLayoutCreateInfo
  std::uint32_t sType;
  const void* pNext;
  std::uint32_t flags;
  std::uint32_t bindingCount;
  const DescriptorSetLayoutBinding* pBindings;
};
struct DescriptorSetAllocateInfo {  // VkDescriptorSetAllocateInfo
  std::uint32_t sType;
  const void* pNext;
  Handle descriptorPool;
  std::uint32_t descriptorSetCount;
  const Handle* pSetLayouts;
};
struct WriteDescriptorSet {  // VkWriteDescriptorSet
  std::uint32_t sType;
  const void* pNext;
  Handle dstSet;
  std::uint32_t dstBinding;
  std::uint32_t dstArrayElement;
  std::uint32_t descriptorCount;
  std::uint32_t descriptorType;
  const void* pImageInfo;
  const void* pBufferInfo;
  const Handle* pTexelBufferView;
};
struct BufferCreateInfo {  // VkBufferCreateInfo
  std::uint32_t sType;
  const void* pNext;
  std::uint32_t flags;
  std::uint64_t size;
  std::uint32_t usage;
  std::uint32_t sharingMode;
  std::uint32_t queueFamilyIndexCount;
  const std::uint32_t* pQueueFamilyIndices;
};
struct MemoryRequirements {  // VkMemoryRequirements
  std::uint64_t size;
  std::uint64_t alignment;
  std::uint32_t memoryTypeBits;
};
struct MemoryAllocateInfo {  // VkMemoryAllocateInfo
  std::uint32_t sType;
  const void* pNext;
  std::uint64_t allocationSize;
  std::uint32_t memoryTypeIndex;
};
struct BufferViewCreateInfo {  // VkBufferViewCreateInfo
  std::uint32_t sType;
  const void* pNext;
  std::uint32_t flags;
  Handle buffer;
  std::uint32_t format;
  std::uint64_t offset;
  std::uint64_t range;
};
struct MemoryType {  // VkMemoryType
  std::uint32_t propertyFlags;
  std::uint32_t heapIndex;
};
struct MemoryHeap {  // VkMemoryHeap
  std::uint64_t size;
  std::uint32_t flags;
};
struct PhysicalDeviceMemoryProperties {  // VkPhysicalDeviceMemoryProperties
  std::uint32_t memoryTypeCount;
  MemoryType memoryTypes[kMaxMemoryTypes];
  std::uint32_t memoryHeapCount;
  MemoryHeap memoryHeaps[kMaxMemoryHeaps];
};

// The LP64 layout (the Vulkan ABI on arm64 and on the x86-64 host the tests run on).
static_assert(sizeof(DescriptorSetLayoutBinding) == 24 && offsetof(DescriptorSetLayoutBinding, pImmutableSamplers) == 16, "binding layout");
static_assert(sizeof(DescriptorSetLayoutCreateInfo) == 32 && offsetof(DescriptorSetLayoutCreateInfo, pBindings) == 24, "layout info layout");
static_assert(sizeof(DescriptorSetAllocateInfo) == 40 && offsetof(DescriptorSetAllocateInfo, pSetLayouts) == 32, "allocate info layout");
static_assert(sizeof(WriteDescriptorSet) == 64 && offsetof(WriteDescriptorSet, pTexelBufferView) == 56, "write layout");
static_assert(sizeof(BufferCreateInfo) == 56 && offsetof(BufferCreateInfo, pQueueFamilyIndices) == 48, "buffer info layout");
static_assert(sizeof(MemoryRequirements) == 24, "memory requirements layout");
static_assert(sizeof(MemoryAllocateInfo) == 32 && offsetof(MemoryAllocateInfo, memoryTypeIndex) == 24, "allocate memory layout");
static_assert(sizeof(BufferViewCreateInfo) == 56 && offsetof(BufferViewCreateInfo, range) == 48, "view info layout");
static_assert(sizeof(PhysicalDeviceMemoryProperties) == 520, "memory properties layout");

}  // namespace vk

// The guard's sizes. A layout with more texel bindings than kMaxPicks, or a binding with more than kMaxDescriptors descriptors, is left alone
// (counted): the writes are built in fixed arrays.
inline constexpr std::size_t kMaxPicks = 4;
inline constexpr std::uint32_t kMaxDescriptors = 64;
inline constexpr std::size_t kTableCapacity = 1024;  // layouts with a uniform texel binding the table can hold
inline constexpr std::size_t kTableSlots = 2048;     // open addressing at a load of at most one half
inline constexpr std::uint64_t kZeroBufferBytes = 65536;

// What PickBindings found in one layout.
enum class Verdict : std::uint8_t {
  kNone,      // no uniform texel binding with a descriptor: nothing to write
  kPicked,    // `picks` holds every uniform texel binding: each gets the zeroed view
  kFlags,     // a binding-flags structure is chained (a variable count or update-after-bind): left alone
  kTooMany,   // more than kMaxPicks uniform texel bindings, or one with more than kMaxDescriptors: left alone
};

struct Pick {
  std::uint32_t binding;
  std::uint32_t count;
};

struct LayoutPicks {
  Verdict verdict = Verdict::kNone;
  std::uint32_t n = 0;
  Pick picks[kMaxPicks] = {};
};

// The bindings of `info` that get the zeroed view: those of type VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER with at least one descriptor, and
// no other type. A binding-flags structure anywhere in the pNext chain leaves the layout alone (kFlags): such a binding may have a variable
// count or be updated after bind, and a write the guard makes there is not one the game expects.
inline LayoutPicks PickBindings(const vk::DescriptorSetLayoutCreateInfo* info) noexcept {
  LayoutPicks out;
  if (info == nullptr) return out;
  for (const vk::BaseIn* p = static_cast<const vk::BaseIn*>(info->pNext); p != nullptr; p = p->pNext) {
    if (p->sType == vk::kStructureTypeBindingFlagsCreateInfo) {
      out.verdict = Verdict::kFlags;
      return out;
    }
  }
  if (info->pBindings == nullptr) return out;
  for (std::uint32_t i = 0; i < info->bindingCount; ++i) {
    const vk::DescriptorSetLayoutBinding& b = info->pBindings[i];
    if (b.descriptorType != vk::kDescriptorTypeUniformTexelBuffer || b.descriptorCount == 0) continue;
    if (out.n == kMaxPicks || b.descriptorCount > kMaxDescriptors) {
      out = LayoutPicks{};
      out.verdict = Verdict::kTooMany;
      return out;
    }
    out.picks[out.n++] = Pick{b.binding, b.descriptorCount};
  }
  if (out.n != 0) out.verdict = Verdict::kPicked;
  return out;
}

// Layout handle -> its picks, for the layouts that have a uniform texel binding (any verdict but kNone). A handle the game creates again
// (a destroyed layout's value reused) replaces its entry; a reused handle whose new layout has none (kNone) replaces it too, so no write is
// ever made for a layout that no longer has the binding. A kNone layout whose handle is not in the table is not stored. Nothing is removed.
class LayoutTable {
 public:
  enum class Put { kStored, kReplaced, kNotStored, kFull };

  Put Record(vk::Handle handle, const LayoutPicks& picks) noexcept {
    if (handle == 0) return Put::kNotStored;
    std::size_t i = Home(handle);
    for (std::size_t probe = 0; probe < kTableSlots; ++probe, i = (i + 1) % kTableSlots) {
      Slot& s = slots_[i];
      if (s.handle == handle) {
        s.picks = picks;
        return Put::kReplaced;
      }
      if (s.handle == 0) {
        if (picks.verdict == Verdict::kNone) return Put::kNotStored;
        if (size_ == kTableCapacity) return Put::kFull;
        s.handle = handle;
        s.picks = picks;
        ++size_;
        return Put::kStored;
      }
    }
    return Put::kFull;  // unreachable: at most kTableCapacity of kTableSlots slots are taken
  }

  // The picks of `handle`; false when the table has no entry for it.
  bool Find(vk::Handle handle, LayoutPicks* out) const noexcept {
    if (handle == 0) return false;
    std::size_t i = Home(handle);
    for (std::size_t probe = 0; probe < kTableSlots; ++probe, i = (i + 1) % kTableSlots) {
      const Slot& s = slots_[i];
      if (s.handle == handle) {
        *out = s.picks;
        return true;
      }
      if (s.handle == 0) return false;
    }
    return false;
  }

  std::size_t Size() const noexcept { return size_; }
  void Clear() noexcept {
    for (Slot& s : slots_) s = Slot{};
    size_ = 0;
  }

 private:
  struct Slot {
    vk::Handle handle = 0;
    LayoutPicks picks;
  };
  // A handle is a driver address (its low bits are alignment): the multiplier spreads the rest over the slots.
  static std::size_t Home(vk::Handle handle) noexcept {
    return static_cast<std::size_t>((handle * 0x9E3779B97F4A7C15ULL) >> 53) % kTableSlots;
  }
  Slot slots_[kTableSlots];
  std::size_t size_ = 0;
};

// The memory type the zeroed buffer is made in: the first type the buffer allows (`typeBits`) that is host-visible and host-coherent, so
// the zeros written once through the mapping are what the GPU reads with no flush. -1 when there is none.
inline int PickMemoryType(const vk::PhysicalDeviceMemoryProperties& props, std::uint32_t typeBits) noexcept {
  const std::uint32_t want = vk::kMemoryPropertyHostVisible | vk::kMemoryPropertyHostCoherent;
  const std::uint32_t count = props.memoryTypeCount < vk::kMaxMemoryTypes ? props.memoryTypeCount : vk::kMaxMemoryTypes;
  for (std::uint32_t i = 0; i < count; ++i) {
    if ((typeBits & (1U << i)) != 0 && (props.memoryTypes[i].propertyFlags & want) == want) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace nevr_quest::combat::texel_buffer_guard
