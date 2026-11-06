/* An allocation of GPU memory that libr15 reports as made, with no memory behind it, is refused while libr15's arrays have room
 * for what the refusal leaves behind (#2262).
 *
 * What the game does (pinned libr15; pinned_targets.h has the addresses and the words they were read from).
 * NRadEngine::CGRawInterfaceVK::Allocate(VkMemoryRequirements, desired flags, minimal flags, pooled, pool, SGAllocatedBase&)
 * calls vkAllocateMemory and never reads its result: only vkMapMemory's is checked, and only for a host-visible type. So when
 * the device is out of memory, Allocate returns 0 (success) with whatever handle the driver left. NRadEngine::CreateDeviceResource
 * (SDeviceTextureReplaceDesc*) then binds its new texture image to that memory (vkBindImageMemory, libr15 +0x18e6c74), and the
 * Adreno driver reads +0x10c of a null object: the Quest 3/3S crash at vulkan.adreno.so+0x1a41c4 (Quest 2: +0x14b2d4), called
 * from libr15 +0x18e6c78.
 *
 * What a refusal does. The handler runs the game's Allocate and, when it returned success with a null memory handle, clears the
 * result and returns the error the game makes for a failed allocation (CErrMsg::CreateAndAddError with Allocate's own error
 * symbol). Allocate has four callers in libr15 (the `bl` sites of its PLT stub), and each treats that error its own way:
 *   - CreateDeviceResource (+0x18e6c54): `cbnz w0` to its error return before the bind. The texture is missing; no crash.
 *   - CreateBuffer (+0x18891f4): `cbnz w0` past its bind; it returns the error to the 16 `bl` sites of its PLT stub. One of
 *     them, CGTempResourceMgr::AllocNewChunk (+0x18e132c), logs it and stops the game on `brk #0x1` (+0x18e1364): a crash in
 *     libr15, not in the driver.
 *   - the image path at +0x18db188 and CGSurfaceManager::CreateDepthBuffer (+0x1832d74): OutputError, then they bind [base+0]
 *     anyway (+0x18db1ac, +0x1832d94). The refusal has cleared the result, so this is still a null bind and still the driver crash.
 * So the refusal removes the crash of the texture path, the one the reports show, and leaves the other three as they were.
 *
 * The bound. A refused allocation is not undone: when it was pooled, Allocate has already appended its null block to the
 * free-range array (vk_hole_table.h, count at +0x5000) and to the array of device blocks (vk_block_layout, count at +0x3000), both
 * 512 entries with no bound check, and a null block is never freed. Before the hook the first such failure crashed; with it the
 * game goes on, and a device that stays out of memory would grow both arrays to the overflow vk_free_guard exists to prevent
 * (the 513th block entry lands on the block count, the next on free-range entry 0). A crash where the game always crashed is
 * better than that corruption, so a refusal is made only while the block count is below kBlockRefuseBelow and the free-range
 * count below vk_hole::kGuardFrom. Past either, the handler hands the game its null result unchanged, the game crashes where it
 * did before the hook, and that decision is logged once with both counts.
 *
 * The margin. The counts are read after the game's Allocate returned, so they include the entries this call added; the game
 * appends one entry per array per call, under its own lock, so another task thread can add one each between this read and the
 * bind. kBlockHeadroom leaves 16 such entries: twice the free guard's headroom (vk_hole::kHeadroom, 8), because a pooled refusal
 * appends to both arrays and a dedicated one to the block array alone, so the block array fills at least as fast.
 *
 * Not shown: that the driver leaves the handle null on a failed vkAllocateMemory (the fault address 0x10c says a null object
 * reached the driver; the pooled path's handle is an uninitialised stack word the driver writes). A garbage handle passes this
 * check; the refusal line's absence from the log of such a crash says so.
 *
 * Log lines (sentinel log, hook_log.h):
 *   install      {"event":"vk_alloc_check_install","result":"installed|refused","status":...,"slot":...,"why":...,"pin":...}
 *   refusal      {"event":"vk_alloc_refused","size":N,"alignment":N,"type_bits":"0x..","desired_flags":"0x..","minimal_flags":"0x..",
 *                 "pooled":0|1,"pool":N,"block_count":N,"table_count":N,"guard_leaked":N,"guard_high_water":N,"refused":K,...}
 *                the first refusal, then every 256th. The counts and the free guard's leaks say whether the GPU ran out because
 *                ranges were leaked (vk_free_guard.h) or because the textures grew.
 *   let through  {"event":"vk_alloc_let_through","block_count":N,"table_count":N,"block_refuse_below":N,"table_refuse_below":N,...}
 *                once, the first time the bound hands the game its null result.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "pinned_targets.h"
#include "vk_hole_table.h"

namespace nevr_quest::vk_alloc_check {

using Thunk = sentinel::pinned::LibR15VkAllocateThunk;
using MemoryRequirements = sentinel::pinned::VkMemoryRequirementsLayout;
using AllocatedBase = sentinel::pinned::SGAllocatedBaseLayout;
using ErrorFn = sentinel::pinned::ErrMsgCreateAndAddErrorFn;

inline constexpr std::uint32_t kBlockCapacity = sentinel::pinned::vk_block_layout::kCapacity;
inline constexpr std::uint32_t kBlockHeadroom = 2 * vk_hole::kHeadroom;
inline constexpr std::uint32_t kBlockRefuseBelow = kBlockCapacity - kBlockHeadroom;  // 496

enum class Verdict : std::uint8_t {
  kPass,         // the game's result goes to the caller as it is: memory, or the game's own error
  kRefuse,       // a success with no memory, and room left: clear it and return the game's allocation error
  kLetThrough,   // a success with no memory, and an array near its end (or a count not readable): the game's result as it is
};

// The verdict for one result. `status` is Allocate's return, whose low 32 bits its callers test (`cbnz w0`); `blockCount` and
// `tableCount` are the arrays' counts after the call, null when not readable.
inline Verdict Judge(std::uint64_t status, std::uint64_t memory, const volatile std::uint32_t* blockCount,
                     const volatile std::uint32_t* tableCount) noexcept {
  if (static_cast<std::uint32_t>(status) != 0 || memory != 0) return Verdict::kPass;
  if (blockCount == nullptr || tableCount == nullptr) return Verdict::kLetThrough;
  if (*blockCount >= kBlockRefuseBelow || *tableCount >= vk_hole::kGuardFrom) return Verdict::kLetThrough;
  return Verdict::kRefuse;
}

struct Counts {
  std::uint64_t refused;      // successes with a null memory handle, turned into errors
  std::uint64_t zero_code;    // refusals where the game's error maker returned 0 (the caller then sees success)
  std::uint64_t let_through;  // successes with a null memory handle handed to the game because an array was near its end
};
Counts CurrentCounts() noexcept;

// Installs the hook into libr15's slot after proving the pinned words. True when the slot holds the thunk. Logs one
// vk_alloc_check_install line either way.
bool Install() noexcept;

// The install on a given image and slot (the host test passes an image it laid out, and a slot that does not resolve).
bool InstallAt(unsigned char* base, const sentinel::GotTarget& target) noexcept;

// Test support: arms the handler with an error maker and the two arrays' count words, without touching a GOT slot.
// ResetForTest clears the counters and the once-only line and disarms.
void ArmForTest(ErrorFn error, const volatile std::uint32_t* blockCount, const volatile std::uint32_t* tableCount) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::vk_alloc_check
