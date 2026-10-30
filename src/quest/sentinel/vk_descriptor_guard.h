/* g40guard: unwritten uniform texel buffer bindings read zeros, not a bad address (#1737 row G6A-030).
 *
 * The fault: on the Quest, every map, the GPU takes a read page fault at 0x1FF0000000000 (+0x900 / +0x1B40) each frame. The
 * reading is a draw whose descriptor set has uniform texel buffer bindings (the light-UV texel buffers of the vertex shader
 * at set 0, bindings 3 and 4) that the game writes only when a sublevel streams in; a draw that runs before that reads a
 * descriptor nobody wrote. Vulkan leaves an unwritten descriptor undefined; the Adreno driver reads it as that address.
 *
 * What this does. Every descriptor set layout the game creates is noted with its UNIFORM_TEXEL_BUFFER bindings
 * (binding number, descriptor count). Every set the game allocates on such a layout has those bindings written, before the
 * game gets the set, with one view of a single zeroed 64 KiB host-visible buffer (format R32G32B32A32_SFLOAT, which the
 * Vulkan specification requires every implementation to support for uniform texel buffers). The game's own
 * vkUpdateDescriptorSets then replaces the view as before; a binding it never writes reads zeros. No other descriptor kind
 * is touched, and a layout created with VkDescriptorSetLayoutBindingFlagsCreateInfo (variable counts, update-after-bind) is
 * left alone. Every layout passes through the wrapper: the game resolves vkCreateDescriptorSetLayout through the chain below before it
 * creates any layout.
 *
 * Hook point. libr15 imports no Vulkan function: it resolves vkGetInstanceProcAddr through CSysModule::FindMethod, then
 * vkGetDeviceProcAddr as an instance function (libr15 string 0x2d6242d, "Could not find Vulkan Instance function
 * vkGetDeviceProcAddr"), then vkAllocateDescriptorSets and the rest as device functions (libr15 string 0x2d6375d, "Could not
 * find Vulkan Device function vkAllocateDescriptorSets"). vk_ext_window.cpp already owns that chain (the FindMethod GOT slot
 * and the vkGetInstanceProcAddr wrapper); its wrapper hands the name vkGetDeviceProcAddr to OnInstanceProcAddr, which
 * returns this module's vkGetDeviceProcAddr; that returns this module's two wrappers for the two names and passes every
 * other name through. No GOT slot, table word or code page is written by this module.
 *
 * Switch: on by default. An empty file `g40guard.off` in the q15t data directory (module_switch.h) turns it off; the
 * decision is made once, when the game first resolves vkGetDeviceProcAddr.
 *
 * Logging: one `q15t_g40guard_install` line (result, reason, the switch file's state); one `g40guard_hook` line per
 * interposed function; one `g40guard_buffer` line when the zeroed buffer is made or fails (with the Vulkan result); one
 * `g40guard_fill` line at 1, 2, 4, 8, ... sets filled (sets, writes, layouts tracked, layouts lost, ms since the
 * first fill, so the rate is readable from two lines).
 *
 * Built with -fno-exceptions. Quest only: the platform reason is Vulkan (the PC game renders with Direct3D 11).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace nevr_quest::vk_guard {

using VoidFn = void (*)();
using GipaFn = VoidFn (*)(void* instance, const char* name);

// ---- the Vulkan ABI this module reads (vulkan_core.h; the offsets are checked against the real header where it exists) --

inline constexpr std::uint32_t kDescriptorTypeUniformTexelBuffer = 4;           // VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
inline constexpr std::uint32_t kStypeBindingFlagsCreateInfo = 1000161000;       // ..._DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO
inline constexpr std::uint32_t kStypeMemoryAllocateInfo = 5;
inline constexpr std::uint32_t kStypeBufferCreateInfo = 12;
inline constexpr std::uint32_t kStypeBufferViewCreateInfo = 13;
inline constexpr std::uint32_t kStypeWriteDescriptorSet = 35;
inline constexpr std::uint32_t kFormatR32G32B32A32Sfloat = 109;
inline constexpr std::uint32_t kBufferUsageUniformTexelBuffer = 0x4;
inline constexpr std::uint32_t kMemoryHostVisible = 0x2;
inline constexpr std::uint32_t kMemoryHostCoherent = 0x4;
inline constexpr std::uint64_t kWholeSize = ~0ULL;
inline constexpr std::int32_t kVkSuccess = 0;

struct BaseIn {
  std::uint32_t sType;
  const BaseIn* pNext;
};
struct DslBinding {
  std::uint32_t binding;
  std::uint32_t descriptorType;
  std::uint32_t descriptorCount;
  std::uint32_t stageFlags;
  const void* pImmutableSamplers;
};
struct DslCreateInfo {
  std::uint32_t sType;
  const void* pNext;
  std::uint32_t flags;
  std::uint32_t bindingCount;
  const DslBinding* pBindings;
};
struct SetAllocateInfo {
  std::uint32_t sType;
  const void* pNext;
  std::uint64_t descriptorPool;
  std::uint32_t descriptorSetCount;
  const std::uint64_t* pSetLayouts;
};
struct WriteDescriptorSet {
  std::uint32_t sType;
  const void* pNext;
  std::uint64_t dstSet;
  std::uint32_t dstBinding;
  std::uint32_t dstArrayElement;
  std::uint32_t descriptorCount;
  std::uint32_t descriptorType;
  const void* pImageInfo;
  const void* pBufferInfo;
  const std::uint64_t* pTexelBufferView;
};
struct BufferCreateInfo {
  std::uint32_t sType;
  const void* pNext;
  std::uint32_t flags;
  std::uint64_t size;
  std::uint32_t usage;
  std::uint32_t sharingMode;
  std::uint32_t queueFamilyIndexCount;
  const std::uint32_t* pQueueFamilyIndices;
};
struct BufferViewCreateInfo {
  std::uint32_t sType;
  const void* pNext;
  std::uint32_t flags;
  std::uint64_t buffer;
  std::uint32_t format;
  std::uint64_t offset;
  std::uint64_t range;
};
struct MemoryAllocateInfo {
  std::uint32_t sType;
  const void* pNext;
  std::uint64_t allocationSize;
  std::uint32_t memoryTypeIndex;
};
struct MemoryRequirements {
  std::uint64_t size;
  std::uint64_t alignment;
  std::uint32_t memoryTypeBits;
};
struct MemoryType {
  std::uint32_t propertyFlags;
  std::uint32_t heapIndex;
};
struct MemoryHeap {
  std::uint64_t size;
  std::uint32_t flags;
};
struct PhysicalDeviceMemoryProperties {
  std::uint32_t memoryTypeCount;
  MemoryType memoryTypes[32];
  std::uint32_t memoryHeapCount;
  MemoryHeap memoryHeaps[16];
};

// ---- the decision (pure: no allocation, no I/O) ----------------------------------------------------------------------

// The most uniform texel bindings kept per layout, and the largest descriptor count written for one binding.
inline constexpr std::size_t kMaxBindingsPerLayout = 8;
inline constexpr std::uint32_t kMaxDescriptorsPerBinding = 64;

struct TexelBinding {
  std::uint32_t binding = 0;
  std::uint32_t count = 0;
};
struct LayoutPlan {
  bool skip = false;          // a binding-flags struct is chained: left alone
  bool truncated = false;     // more than kMaxBindingsPerLayout texel bindings, or one above kMaxDescriptorsPerBinding
  std::size_t count = 0;      // entries of `bindings` in use
  TexelBinding bindings[kMaxBindingsPerLayout];
};

// What a set on this layout gets written: its UNIFORM_TEXEL_BUFFER bindings with a non-zero count, in the order given. A
// binding with more than kMaxDescriptorsPerBinding descriptors is written up to that many (truncated is set).
LayoutPlan PlanLayout(const DslCreateInfo* ci) noexcept;

// The first index i >= 1 with a power of two: true for 1, 2, 4, 8, ... (the fill line's cadence).
bool IsReportCount(std::uint64_t n) noexcept;

// ---- the hooks -------------------------------------------------------------------------------------------------------

// Called by vk_ext_window's vkGetInstanceProcAddr wrapper for every name, after the real call (`found`). Returns what the
// game gets: this module's vkGetDeviceProcAddr for that name while the module is on, `found` otherwise.
VoidFn OnInstanceProcAddr(GipaFn real, void* instance, const char* name, VoidFn found) noexcept;

struct Stats {
  bool decided = false;
  bool on = false;
  bool buffer_ok = false;
  std::uint64_t sets_filled = 0;
  std::uint64_t writes = 0;
  std::uint64_t other_device = 0;   // allocations on a device other than the one the buffer was made on
  std::uint64_t layouts = 0;        // layouts tracked now
  std::uint64_t layouts_lost = 0;   // layouts with texel bindings that found no room in the table
};
Stats GetStats() noexcept;

// Test seams: a millisecond clock; clears every piece of state (the decision included).
void SetClockForTest(std::int64_t (*clock_ms)()) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::vk_guard
