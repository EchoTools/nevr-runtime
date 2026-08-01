# leVR Porting Analysis — Echo VR to OpenXR/Linux

**Date:** 2026-06-09
**Source Binary:** echovr.exe goldmaster 631547 (SHA256: `b6d08277...`)
**Quest Binary:** libr15.so v34.4 (same CVR engine, Vulkan on Android/Linux)
**Tools Used:** revault, evr-reconstruction porting guide, Gemini Deep Research
**Author:** @metis-sprock

---

## Architecture: Native Vulkan Swapchains (Not D3D11 Extension)

**The critical architectural decision:** bypass `XR_KHR_D3D11_enable` entirely. Use native Vulkan swapchains via `XR_KHR_vulkan_enable2` with manual `vkCmdCopyImage` from DXVK's VkImage to OpenXR's VkImage.

### Why Not XR_KHR_D3D11_enable

The D3D11 OpenXR extension under Wine is fragile because:

- wineopenxr must unwrap DXVK's ID3D11Device to find the underlying Vulkan device
- Proxy/hooked ID3D11Device pointers break this unwrapping → `XR_ERROR_GRAPHICS_DEVICE_INVALID`
- DXGI swapchain lifecycle management under Wine causes `VK_ERROR_OUT_OF_DATE_KHR` on resize/PRIME

### Recommended: Native Vulkan + vkCmdCopyImage Bridge

Proven by openRBRVR (open-source DXVK→OpenXR bridge for Richard Burns Rally).

**Flow:**

1. Game initializes DXVK's `ID3D11Device` normally
2. Translation layer creates OpenXR instance, queries required Vulkan extensions
3. Extract `VkInstance`, `VkPhysicalDevice`, `VkDevice` from DXVK via `IDXGIVkInteropDevice` (`dxvk-interop.h`)
4. Populate `XrGraphicsBindingVulkanKHR` with raw Vulkan pointers → create `XrSession`
5. Call `xrCreateSwapchain` with native Vulkan formats (map `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` → `VK_FORMAT_R8G8B8A8_SRGB`)
6. When game calls `ovr_CreateTextureSwapChainDX` — create D3D11 textures with `D3D11_RESOURCE_MISC_SHARED` flag as proxy targets
7. At frame submission: acquire OpenXR VkImage, extract DXVK VkImage via interop API, `vkCmdCopyImage` between them

---

## Critical: VK_WINE_openxr_device_extensions Negotiation

wineopenxr returns `VK_WINE_openxr_device_extensions` instead of standard Khronos extensions when queried via `xrGetVulkanDeviceExtensionsKHR`. This token signals DXVK to forcibly enable `VK_KHR_external_memory` and `VK_KHR_external_semaphore` on the Vulkan device.

**The initialization order matters:** OpenXR must be initialized BEFORE or CONCURRENTLY with DXVK's D3D11 device creation. If DXVK initializes first without seeing the `VK_WINE_openxr_device_extensions` token, external memory extensions won't be enabled and OpenXR will fail with `XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING`.

**Required extensions:**

| Extension                          | Role                                                     |
| ---------------------------------- | -------------------------------------------------------- |
| `XR_KHR_vulkan_enable2`            | Accept Vulkan graphics bindings (replaces deprecated v1) |
| `VK_KHR_external_memory`           | Share memory between DXVK and OpenXR compositor          |
| `VK_KHR_external_semaphore`        | Cross-process synchronization                            |
| `VK_KHR_maintenance6`              | Required by DXVK 2.7.1+ (Vulkan 1.3 mandatory)           |
| `VK_WINE_openxr_device_extensions` | Proprietary Wine token enabling the above                |

---

## Registry Bootstrap: Software\Wine\VR

Proton's wineopenxr requires `HKEY_CURRENT_USER\Software\Wine\VR` to be populated. Steam's `setup_vr` helper normally does this. For standalone (non-Steam) launches:

- The translation layer MUST write the expected registry values programmatically before any OpenXR call
- Alternative: use GE-Proton which has dynamic `setup_vr` decoupling
- Without this key, wineopenxr fails with status 0x2 (File Not Found) during module loading

---

## Swapchain Lifecycle: Decoupling Implicit from Explicit

LibOVR uses **implicit** acquire: render → `ovr_CommitTextureSwapChain` → runtime handles sync.
OpenXR uses **explicit** acquire: `xrAcquireSwapchainImage` → `xrWaitSwapchainImage` → render → `xrReleaseSwapchainImage`.

### Ring Buffer Decoupling Pattern

Maintain an internal ring buffer of 3 D3D11 proxy textures (triple-buffering). When the engine requests a texture, return the next available proxy immediately — NO blocking calls during `ovr_GetTextureSwapChainCurrentIndex`.

All OpenXR synchronization is deferred to `ovr_SubmitFrame`:

1. `xrAcquireSwapchainImage` — lock target OpenXR VkImage
2. `xrWaitSwapchainImage` — block until compositor is done reading (at end of frame, this aligns with vblank)
3. `ID3D11DeviceContext::Flush` — ensure DXVK dispatched all pending commands
4. `vkCmdCopyImage` — transfer proxy texture → OpenXR image (with explicit layout transitions)
5. Submit Vulkan command buffer with `VkSemaphore` for compositor sync
6. `xrReleaseSwapchainImage` — return image to compositor
7. `xrEndFrame` — submit frame with `XrCompositionLayerProjection`

### Memory Layout Transitions

```
DXVK image: VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL (or SHADER_READ_ONLY_OPTIMAL)
  → barrier → VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL

OpenXR image: VK_IMAGE_LAYOUT_UNDEFINED
  → barrier → VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL

After vkCmdCopyImage:
  → barrier → VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL (before xrReleaseSwapchainImage)
```

### Desktop Mirror

Create a standard D3D11 render target for `ovr_CreateMirrorTextureDX`. During `ovr_SubmitFrame`, after copying eye buffers to OpenXR, issue `ID3D11DeviceContext::CopyResource` to blit left eye → mirror texture. DXVK handles presentation to the X11/Wayland window natively.

---

### Function pointer table (from revault)

Echo VR's loader `FUN_141360790` at `0x141360790` does `LoadLibrary("LibOVRRT64_1.dll")` and 73 `GetProcAddress` calls. Each function pointer is stored in a global `DAT_*` and null-checked.

| Function                       | Stored At       | Notes                                                               |
| ------------------------------ | --------------- | ------------------------------------------------------------------- |
| `ovr_CreateTextureSwapChainDX` | `DAT_1420eb7b8` | **leVR missing** — null check crash                                 |
| `ovr_SubmitFrame2`             | `DAT_1420eb648` | leVR has `ovr_SubmitFrame` (convenience) but not `ovr_SubmitFrame2` |
| `ovr_CreateTextureSwapChainGL` | `DAT_1420eb6c8` | leVR has this — Echo VR loads both                                  |
| `ovr_GetAudioDeviceOutWaveId`  | `DAT_1420eb7e0` | **leVR missing** — likely startup issue                             |

Echo VR uses `ovr_SubmitFrame2`, NOT `ovr_SubmitFrame`. leVR only implements `ovr_SubmitFrame` (a convenience wrapper). Both need to be exported from the shim. Confirmed via revault: `ovr_SubmitFrame2` loaded at `DAT_1420eb648` in the loader `FUN_141360790`, called through the CVR rendering pipeline at `CVRSystem::UpdateDisplay` (`0x14072d110`).

Format mapping: `DXGI_FORMAT` stored as `uint32_t` in `CGTexture`/`CGTextureResource` at offset `+0x78`, default `0xFFFFFFFF` (-1 = unset). The Quest `CVR_VK::CreateRenderTarget` (@ `0x1b73150`) acquires a Vulkan temporary command buffer via `CGRawInterfaceVK::AcquireTemporaryCommandBuffer` to create render targets — confirming the Vulkan pattern.

## Hooking Strategy: LibOVR Only (Not D3D11)

Do NOT proxy the entire `ID3D11Device`. This breaks DXVK's state caching and introduces COM ref-counting bugs.

Instead, hook ONLY the LibOVR entry points:

- `ovr_Initialize` → `xrCreateInstance` + `xrGetSystem` + negotiate `VK_WINE_openxr_device_extensions`
- `ovr_Create` → `xrCreateSession` + `xrCreateReferenceSpace` + DXVK interop bridge init
- `ovr_CreateTextureSwapChainDX` → create D3D11 proxy + `xrCreateSwapchain` (Vulkan)
- `ovr_GetTextureSwapChainBufferDX` → return proxy `ID3D11Texture2D`
- `ovr_CommitTextureSwapChain` → mark proxy ready for copy
- `ovr_SubmitFrame` → full sync + copy + `xrEndFrame`
- `ovr_GetTrackingState` → `xrLocateSpace`
- `ovr_GetPredictedDisplayTime` → OpenXR monotonic clock mapping
- `ovr_GetAudioDeviceOutWaveId` → stub with default device

The wrapper gains access to DXVK's `ID3D11Device` via the `IUnknown*` parameter in `ovr_CreateTextureSwapChainDX`. Query for `IDXGIVkInteropDevice` via COM `QueryInterface` to get Vulkan pointers.

---

## Runtime Choice: Monado > SteamVR

| Factor          | Monado                      | SteamVR                       |
| --------------- | --------------------------- | ----------------------------- |
| Registry deps   | None                        | Requires `Software\Wine\VR`   |
| External memory | Native `_fd` support        | Strict alignment constraints  |
| Debugging       | Full source, gdb attachable | Black-box compositor          |
| Setup overhead  | Minimal                     | Requires Steam client         |
| Performance     | Predictable                 | More mature but less flexible |

**Recommendation:** Develop against Monado first for debugging, then validate on SteamVR for broader hardware support.

For wireless headsets: WiVRn implements its own OpenXR runtime. Ensure swapchain usage flags match WiVRn's expectations (`XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT` vs `INPUT_ATTACHMENT_BIT`).

---

## Known Pitfalls

### DXVK 2.7.1+ Requires VK_KHR_maintenance6

Mandates Vulkan 1.3 driver support. Check before initializing. Older Mesa/NVK builds will fail silently.

### PRIME Dual-GPU `VK_ERROR_OUT_OF_DATE_KHR`

DXVK may misreport surface dimensions on hybrid GPU setups. Trap this error during `xrAcquireSwapchainImage` and dynamically recreate the swapchain with updated extents without invalidating D3D11 proxy textures.

### Frame Pacing Desync

If `xrWaitSwapchainImage` blocks too long due to compositor vblank miss, the proxy ring buffer fills up → engine physics thread stalls. Implement a heuristic safety valve: drop outdated proxy frames before `vkCmdCopyImage` if the compositor is delayed, sacrificing a frame to maintain motion-to-photon latency.

---

## Quest Vulkan Reference (from revault)

The Quest binary (`libr15.so`, 143k functions) has the same CVR engine with native Vulkan:

**CVR lifecycle (libr15.so):** `CVR::InitGraphics` @ `0x1b70b78`, `CVR::BeginFrame` @ `0x1b72c9c`, `CVR::EndFrame` @ `0x1b72de8`
**CVR_VK:** `CVR_VK::CreateRenderTarget` @ `0x1b73150` (takes `VkImage_T*`, `VkFormat`), `CVR_VK::CreateTexture` @ `0x1b73314`

These are the reference implementations for how Echo VR's engine passes Vulkan images through the render pipeline.

---

## leVR Gaps (Confirmed via revault)

| Function                          | Address                | Status     | Impact   |
| --------------------------------- | ---------------------- | ---------- | -------- |
| `ovr_CreateTextureSwapChainDX`    | `0x141360790` (loaded) | ❌ Missing | CRITICAL |
| `ovr_GetTextureSwapChainBufferDX` | loaded                 | ❌ Missing | CRITICAL |
| `ovr_CreateMirrorTextureDX`       | loaded                 | ❌ Missing | HIGH     |
| `ovr_GetAudioDeviceOutWaveId`     | `DAT_1420eb7e0`        | ❌ Missing | MEDIUM   |

Echo VR loads BOTH `DX` and `GL` swapchain variants. The loader's null check on `DAT_1420eb7b8` is the likely crash point.

---

## Reference Implementations

- **openRBRVR** — DXVK→OpenXR via `vkCmdCopyImage`. Proven architecture. ([github.com/Detegr/openRBRVR](https://github.com/Detegr/openRBRVR))
- **OpenComposite** — OpenVR→OpenXR translation. Reference for swapchain lifecycle management.
- **liboculus** (yarp-device-ovrheadset) — Structural substitution pattern for LibOVR→OpenXR.
- **OpenXR SDK** ([github.com/KhronosGroup/OpenXR-SDK-Source](https://github.com/KhronosGroup/OpenXR-SDK-Source)) — Spec and samples.

---

## Scaffolding Gaps (What leVR Needs)

| Gap                 | Priority | Detail                                                     |
| ------------------- | -------- | ---------------------------------------------------------- |
| Test harness        | CRITICAL | Standalone Windows app exercising all 73 Oculus functions  |
| D3D11 swapchain     | CRITICAL | `ovr_CreateTextureSwapChainDX` + DXVK interop              |
| Observability       | HIGH     | Ring-buffer trace, session journal, frame pacing telemetry |
| Frida hook script   | HIGH     | Trace which Oculus calls Echo VR makes at startup          |
| Error code mapping  | HIGH     | Per-function Oculus→OpenXR→POSIX error translation         |
| VK_WINE negotiation | CRITICAL | OpenXR init before DXVK init for extension discovery       |
| Registry bootstrap  | HIGH     | Write `Software\Wine\VR` for standalone launches           |
| Wine prefix         | HIGH     | `.wineprefix/` for testing without Windows machine         |
| CMakePresets        | MEDIUM   | 6-preset matrix (see CPP-MINGW addendum)                   |
| vcpkg manifest      | MEDIUM   | Dependencies via `vcpkg.json`                              |
| Plugin architecture | MEDIUM   | Dynamic backends (DXVK, native Vulkan, stubs)              |
| Code signing        | MEDIUM   | osslsigncode + CA hierarchy                                |
| PE version info     | MEDIUM   | `.rc` resource for DLL                                     |
| CI pipeline         | MEDIUM   | GitHub Actions + Wine test job                             |

---

## Library Versions (spriffy)

| Library       | Version       | Notes                                              |
| ------------- | ------------- | -------------------------------------------------- |
| OpenXR loader | 1.1.59.1      | System-wide at `/usr/lib/libopenxr_loader.so`      |
| DXVK          | 2.7.1-1       | `dxvk-bin` package, requires `VK_KHR_maintenance6` |
| Wine          | 11.9          | Very recent, GE-Proton recommended for VR          |
| Mingw-w64 GCC | 16.1.0        | Cross-compiler                                     |
| Monado        | not installed | Available via nix flake devShell                   |

## Repositories

| Repo         | URL                                    | Status                        |
| ------------ | -------------------------------------- | ----------------------------- |
| leVR         | `github.com/EchoTools/levr`            | 3 commits, needs full rewrite |
| Monado       | `gitlab.freedesktop.org/monado/monado` | Not cloned                    |
| DXVK         | `github.com/doitsujin/dxvk`            | System package 2.7.1-1        |
| openRBRVR    | `github.com/Detegr/openRBRVR`          | Reference architecture        |
| nevr-runtime | `github.com/echotools/nevr-runtime`    | Reference mingw pipeline      |

---

## Citations

1. **OpenXR Specification** — Khronos Group. Swapchain lifecycle: `xrAcquireSwapchainImage`, `xrWaitSwapchainImage`, `xrReleaseSwapchainImage`. https://www.khronos.org/registry/OpenXR/specs/1.0/man/html/XrGraphicsBindingOpenGLWin32KHR.html
2. **VK_WINE_openxr_device_extensions** — ValveSoftware/Proton Issue #7737. wineopenxr returns synthetic extension to force DXVK to enable `VK_KHR_external_memory` and `VK_KHR_external_semaphore`. https://github.com/ValveSoftware/Proton/issues/7737
3. **Steam VR Registry Dependency** — ValveSoftware/Proton Issue #8256. `HKEY_CURRENT_USER\Software\Wine\VR` must be populated for wineopenxr. https://github.com/ValveSoftware/Proton/issues/8256
4. **openRBRVR** — Detegr. DXVK→OpenXR via `vkCmdCopyImage`. Proven architecture for D3D→Vulkan VR translation. https://github.com/Detegr/openRBRVR
5. **DXVK PRIME Dual-GPU Issue** — doitsujin/dxvk Issue #5603. Wrong swapchain resolution on PRIME dual-GPU setups causing `VK_ERROR_OUT_OF_DATE_KHR`. https://github.com/doitsujin/dxvk/issues/5603
6. **DXVK 2.7.1+ Minimum Wine** — doitsujin/dxvk Issue #5419. Requires `VK_KHR_maintenance6` and Vulkan 1.3 driver. https://github.com/doitsujin/dxvk/issues/5419
7. **OpenComposite** — znixian/OpenOVR. OpenVR→OpenXR translation layer, reference for explicit swapchain management. https://gitlab.com/znixian/OpenOVR
8. **Monado OpenXR Runtime** — Freedesktop project. Open-source OpenXR runtime for Linux, native `_fd` external memory support. https://gitlab.freedesktop.org/monado/monado
9. **liboculus / yarp-device-ovrheadset** — Robotology. Reference for LibOVR→OpenXR structural substitution. https://github.com/robotology/yarp-device-ovrheadset
10. **Echo VR PC Binary** — revault decompilation at `0x141360790` (loader, 73 GetProcAddress calls), `0x140531cb0` (CGTexture with DXGI_FORMAT), `0x1b73150` (Quest CVR_VK::CreateRenderTarget)

## Related Documents

- CPP-MINGW addendum: `/home/nous/citadel/src/metis-core/CPP-MINGW-ADDENDUM-GENERIC.md`
- Porting guide: `~/src/evr-reconstruction/docs/kb/porting_guide.md`
- Hook points YAML: `~/src/evr-reconstruction/docs/kb/hook_points.yaml`
- Quest triage cache: `~/src/evr-reconstruction/cache/quest_triage/`
- Gemini Deep Research: `/tmp/D3D11 to Vulkan OpenXR Porting.md`
