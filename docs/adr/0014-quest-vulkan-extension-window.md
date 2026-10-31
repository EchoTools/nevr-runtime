# ADR 0014: the device extensions the game requires stay inside the list it reads

Status: accepted, implemented (`src/quest/sentinel/vk_ext_window.{h,cpp}`, #868, #869). On in every Quest build, with no feature flag; where the driver's list fits the game's buffer it changes nothing.
Not yet observed on a Quest 3: the "Prediction" section is what the first headset run must show.

## Context

libr15 checks the device extensions it needs before it creates the Vulkan device. It calls `vkEnumerateDeviceExtensionProperties` for the count, clamps that count to 128 entries (the disassembly `0x19ddaa4`-`0x19ddab0`; records are `VkExtensionProperties`, stride 0x104), calls again with the clamped count, and compares only the entries the driver returned. `VK_KHR_swapchain` is the first name checked. A driver that has more than 128 device extensions answers the second call with `VK_INCOMPLETE` and its first 128; a required name past index 127 is not seen, the game logs "Required extension VK_KHR_swapchain does not exist", and it goes on to create a device without the extension.

Measured on a Quest 3 (Adreno 740): the driver reports 137 device extensions and `VK_KHR_swapchain` is at index 134. The game's call returns count 128, result 5, and the name is not among the 128. The Quest 2 driver's list is shorter than 128 and is not affected.

libr15 imports no `vk*` function. It gets `vkGetInstanceProcAddr` from `CSysModule::FindMethod` (the disassembly `0x19dcaa4`, `JUMP_SLOT 0x36db9f8`) and every other Vulkan function through it. It passes no layers (`enabledLayerCount` is 0 for the instance and the device). The VrApi device list (`vrapi_GetDeviceExtensionsVulkan`, `JUMP_SLOT 0x36dd440`) is read after the enumeration and its names are added to the required set.

## Decision

Two GOT hooks, installed in every build. The `FindMethod` hook returns a wrapper for `vkGetInstanceProcAddr`; that wrapper returns a wrapper for `vkEnumerateDeviceExtensionProperties`; every other name, in either, is the platform's own answer. The `vrapi_GetDeviceExtensionsVulkan` hook passes every call through; it exists so the wrapper holds the real function.

The enumerate wrapper calls the driver and returns the driver's result and count unchanged. Only one call can differ: the game's buffer call (no layer name, a buffer, result `VK_INCOMPLETE`, the count the game passed returned in full, at most 256). For that call the wrapper asks the driver for its whole list (at most 512 entries) and reads the VrApi list. When a required name sits at or past the game's buffer size it overwrites the game's buffer: the required names first, in the driver's order, then the other names in the driver's order. The names that count as required are the ones libr15 checks (`VK_KHR_swapchain`, `VK_KHR_external_memory`, `VK_KHR_external_semaphore`, `VK_KHR_maintenance1`, `VK_KHR_relaxed_block_layout`, `VK_KHR_multiview`, `VK_EXT_fragment_density_map`) and the VrApi list. The result stays 5, the count stays 128 and the count query still reports the driver's total, so the game sees a longer list than it can read, as before, with the names it needs inside the part it reads. A list that fits, a layer-specific call, a count query, a driver error, a changed count and a call that arrives while another is being decided are the driver's answer, byte for byte. VrApi is called only when the buffer call returned `VK_INCOMPLETE`, so a headset whose list fits never reaches it.

Each buffer call logs one `vk_ext_window` line (the first 16): the action (`reordered`, `unchanged`, `passthrough`), the reason, the counts, the driver total, how many required names were present and how many were outside, where `VK_KHR_swapchain` sat before and after, and the VrApi result. `vk_ext_window_install` reports the install.

## Prediction (written before a headset run)

On a Quest 3: `vk_ext_window_install` `result=installed`; one `vk_ext_window` line with `action=reordered reason=required_names_past_game_buffer count_in=128 count_out=128 result=5 driver_total=137 required_outside=1 swapchain_before=134` and `swapchain_after` below 12; no "Required extension VK_KHR_swapchain does not exist" line; the game creates its device and reaches the main menu with a rendered frame. On a Quest 2: the same install line and one line with `action=passthrough reason=complete`.

What would prove it wrong: the "does not exist" line for any name after a `reordered` line (a required name the wrapper did not know); a different failure after the device is created (the swapchain is a separate step); no `vk_ext_window` line at all (the game does not reach the function through `FindMethod`); the game stopping before the first `vk_ext_window` line.

## Not claimed

That the Quest 3 then renders: the extension check is one of the steps between the library load and the first frame. Whether a log callback of the game ends the process on the error-level line is not examined: `NWriteLog::WriteLogV` (the disassembly `0x1088720`) itself only picks the `[ERROR] ` prefix and the Android log priority for level 8; the registered callbacks (`CLoggingData::ExecuteAllCallbacks`) are not read.
