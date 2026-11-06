/* An allocation of GPU memory that libr15 reports as made, with no memory behind it, is refused (#2262).
 *
 * What the game does (pinned libr15; pinned_targets.h has the addresses and the words they were read from).
 * NRadEngine::CGRawInterfaceVK::Allocate(VkMemoryRequirements, desired flags, minimal flags, pooled, pool, SGAllocatedBase&)
 * calls vkAllocateMemory and never reads its result: only vkMapMemory's is checked, and only for a host-visible type. So when
 * the device is out of memory, Allocate returns 0 (success) with whatever handle the driver left, and a pooled allocation
 * enters that block into the free-range table (vk_hole_table.h) for every later allocation to cut from.
 * NRadEngine::CreateDeviceResource(SDeviceTextureReplaceDesc*) then binds its new image to that memory
 * (vkBindImageMemory, libr15 +0x18e6c74), and the Adreno driver reads +0x10c of a null object: the Quest 3/3S crash at
 * vulkan.adreno.so+0x1a41c4 (Quest 2: +0x14b2d4), called from libr15 +0x18e6c78.
 *
 * What this does. One GOT hook, on Allocate's own slot (pinned_targets.h, LibR15VkAllocate). The handler runs the game's
 * Allocate and, when it returned success with a null memory handle, clears the result and returns the error the game makes for
 * a failed allocation (CErrMsg::CreateAndAddError with Allocate's own error symbol), so every caller takes its own error path:
 * CreateDeviceResource returns before the bind, and CResource::CreateResource logs "Failed to serialize resource". The texture
 * is missing instead of the game crashing. A non-null handle, or an error from the game, passes through untouched.
 *
 * Not shown: that the driver leaves the handle null on a failed vkAllocateMemory (the fault address 0x10c says a null object
 * reached the driver; the pooled path's handle is an uninitialised stack word the driver writes). A garbage handle passes this
 * check; the refusal line's absence from the log of such a crash says so.
 *
 * Log lines (sentinel log, hook_log.h):
 *   install  {"event":"vk_alloc_check_install","result":"installed|refused","status":...,"slot":...,"why":...,"pin":...}
 *   refusal  {"event":"vk_alloc_refused","size":N,"alignment":N,"type_bits":"0x..","desired_flags":"0x..","minimal_flags":"0x..",
 *             "pooled":0|1,"pool":N,"table_count":N,"guard_leaked":N,"guard_high_water":N,"refused":K,...}
 *            the first refusal, then every 256th. The table count and the free guard's leaks say whether the GPU ran out
 *            because ranges were leaked (vk_free_guard.h) or because the textures grew.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "pinned_targets.h"

namespace nevr_quest::vk_alloc_check {

using Thunk = sentinel::pinned::LibR15VkAllocateThunk;
using MemoryRequirements = sentinel::pinned::VkMemoryRequirementsLayout;
using AllocatedBase = sentinel::pinned::SGAllocatedBaseLayout;
using ErrorFn = sentinel::pinned::ErrMsgCreateAndAddErrorFn;

struct Counts {
  std::uint64_t refused;    // successes with a null memory handle, turned into errors
  std::uint64_t zero_code;  // refusals where the game's error maker returned 0 (the caller then sees success)
};
Counts CurrentCounts() noexcept;

// Installs the hook into libr15's slot after proving the pinned words. True when the slot holds the thunk. Logs one
// vk_alloc_check_install line either way.
bool Install() noexcept;

// The install on a given image and slot (the host test passes an image it laid out, and a slot that does not resolve).
bool InstallAt(unsigned char* base, const sentinel::GotTarget& target) noexcept;

// Test support: arms the handler with an error maker and the free-range table's count word (null: not read), without
// touching a GOT slot. ResetForTest clears the counters and disarms.
void ArmForTest(ErrorFn error, const volatile std::uint32_t* tableCount) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::vk_alloc_check
