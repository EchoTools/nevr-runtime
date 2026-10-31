/* Keeps the device extensions libr15 requires inside the window it reads (#868).
 *
 * The fault, measured on a Quest 3 (Adreno 740, driver total 137, VK_KHR_swapchain at index 134): libr15 asks
 * vkEnumerateDeviceExtensionProperties for the count, clamps it to 128 entries (disassembly 0x19ddaa4-0x19ddab0, buffer
 * stride 0x104), asks again with that count and reads only what the driver returned. The driver returns VK_INCOMPLETE
 * with the first 128 entries; VK_KHR_swapchain is not among them; the game logs "Required extension VK_KHR_swapchain does
 * not exist" and stops. Nothing is wrong with the headset's Vulkan: the extension is there, past the game's limit.
 *
 * What this does. libr15 imports no vk* function: it gets vkGetInstanceProcAddr through CSysModule::FindMethod
 * (pinned_targets.h, JUMP_SLOT 0x36db9f8, disassembly 0x19dcaa4) and every other Vulkan function through that. The FindMethod
 * handler returns a wrapper for vkGetInstanceProcAddr, and the wrapper returns a wrapper for
 * vkEnumerateDeviceExtensionProperties; every other name passes through untouched. The enumerate wrapper calls the driver
 * and returns the driver's answer, with one exception: when the call is the game's buffer call (no layer name, a buffer,
 * result VK_INCOMPLETE) and a name the game requires sits in the driver's list past the game's buffer, it rewrites the
 * game's buffer so the required names come first, then the other names in the driver's order. The result (VK_INCOMPLETE),
 * the count the game passed and the total the driver reports are unchanged, so the count query still says 137 and the
 * buffer call still says 5; a call the rewrite does not apply to is the driver's, byte for byte.
 *
 * The names that count as required are the ones the game checks (disassembly 0x19ddad0-0x19ddf5c, in the game's order:
 * VK_KHR_swapchain, VK_KHR_external_memory, VK_KHR_external_semaphore, [the VrApi list], VK_KHR_maintenance1,
 * VK_KHR_relaxed_block_layout, VK_KHR_multiview, VK_EXT_fragment_density_map) and the VrApi list itself. The game reads
 * that list after the enumeration, so the wrapper reads it first, from the real vrapi_GetDeviceExtensionsVulkan, and only
 * when it is about to decide (the buffer call returned VK_INCOMPLETE): a headset whose list fits never reaches VrApi.
 *
 * Install: every build, release included (the cause is proven on hardware; the pass-through makes it inert where the list
 * fits). A GotHook on the FindMethod slot, and a pass-through hook on the VrApi slot whose only purpose is to hold the
 * real function (CallbackThunk::Original).
 *
 * Logging, after the driver has answered and never while game code is on the stack: one `vk_ext_window` line per
 * property call that carries a buffer (the first kMaxLoggedCalls), saying what was done and why, and one
 * `vk_ext_window_install` line.
 *
 * Built with -fno-exceptions (callback_thunk.h).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "pinned_targets.h"

namespace nevr_quest::vk_window {

using FindMethodThunk = sentinel::pinned::LibR15FindMethodThunk;
using DeviceExtensionsThunk = sentinel::pinned::LibR15GetDeviceExtensionsVulkanThunk;

// VkExtensionProperties: char extensionName[256]; uint32_t specVersion.
inline constexpr std::size_t kRecordStride = 260;
inline constexpr std::size_t kRecordNameBytes = 256;
// The most driver records examined (the buffer is static: kFullCapacity * kRecordStride bytes). A driver that reports more
// is read up to this many; a required name past it is not seen.
inline constexpr std::size_t kFullCapacity = 512;
// The largest game buffer rewritten (the game's own clamp is 128).
inline constexpr std::size_t kMaxWindow = 256;
// The game's buffer for the VrApi list is 0x800 bytes.
inline constexpr std::size_t kVrapiChars = 2048;
inline constexpr std::int32_t kVkSuccess = 0;
inline constexpr std::int32_t kVkIncomplete = 5;
// Property calls with a buffer that are logged; later ones are handled the same and not logged.
inline constexpr unsigned kMaxLoggedCalls = 16;

inline constexpr const char* kSwapchain = "VK_KHR_swapchain";
// The names libr15 checks itself, in the game's order (the VrApi list is checked between the third and the fourth).
inline constexpr const char* kGameRequired[] = {
    "VK_KHR_swapchain",           "VK_KHR_external_memory", "VK_KHR_external_semaphore", "VK_KHR_maintenance1",
    "VK_KHR_relaxed_block_layout", "VK_KHR_multiview",       "VK_EXT_fragment_density_map",
};
inline constexpr std::size_t kGameRequiredCount = sizeof(kGameRequired) / sizeof(kGameRequired[0]);

// ---- the decision (pure: no allocation, no I/O) -----------------------------------------------------------------------

struct WindowPlan {
  bool reorder = false;           // `out` holds the game's window
  std::size_t required = 0;       // required names present in the driver's list
  std::size_t outside = 0;        // of those, how many sit at index >= capacity
  long swapchain_before = -1;     // index of VK_KHR_swapchain in the driver's order (-1: absent)
  long swapchain_after = -1;      // ... in the window (-1: absent or not in it)
};

// Index of `token` in the whitespace-separated `list`, whole tokens only; -1 when absent or either is null.
long FindToken(const char* list, const char* token) noexcept;
// Number of whitespace-separated tokens (0 for null).
std::size_t CountTokens(const char* list) noexcept;

// `driver`: `total` VkExtensionProperties records (kRecordStride apart) in the driver's order. `capacity`: the number of
// records the game's buffer holds. `vrapi`: the VrApi device list (NUL-terminated, may be null). Decides whether any
// required name sits at index >= capacity; if so, writes the window (`capacity` records: the required names in the
// driver's order, then the others in the driver's order) to `out` and sets reorder. Otherwise (total <= capacity, nothing
// required outside, capacity 0 or above kMaxWindow, total above kFullCapacity) reorder is false and `out` is untouched.
WindowPlan BuildWindow(const char* driver, std::size_t total, std::size_t capacity, const char* vrapi, char* out) noexcept;

// ---- the hooks -------------------------------------------------------------------------------------------------------

// Arms the two handlers and installs their thunks in libr15.so; one line {"event":"vk_ext_window_install",...} says what
// was installed. True only when both slots hold their thunks.
bool Install() noexcept;

// Arms the handlers without touching a GOT slot, and clears all state.
void ArmForTest() noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::vk_window
