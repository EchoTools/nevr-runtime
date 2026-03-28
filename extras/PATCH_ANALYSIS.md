# nevr-runtime Patch Analysis — Validated

All claims validated against `~/src/echovr-reconstruction` (struct/function names, offsets) and ReVault (Windows PE addresses, decompilation). No TODOs, no unresolved references.

---

## Extreme Hackiness

### 1. Crash Reporter Suppression (4 API hooks + VEH)

BugSplat64.dll is a separate DLL imported by echovr.exe (ReVault string: `BugSplat64.dll` @ 0x141ffb44c). The crash reporter (BsSndRpt64.exe) is launched BY BugSplat64.dll, not by game code. The game's only CreateProcess wrapper is `FUN_1400db9c0` (ReVault), called by `FUN_1400dc890` (generic process launcher for hyperlinks/script nodes), unrelated to crash reporting.

The reconstruction confirms BugSplat as a third-party import (`link/LINK_STATUS.md:44,110`; `link/imports.h:26`) and documents `CNSProvider_CrashReportUserName` @ Quest:0x19323d4 (`src/NRadEngine/Game/CNSProvider.cpp:571-574`) which only provides a username string to the SDK.

**There is no single hook point in echovr.exe that controls crash reporter launch.** The 4-hook+VEH approach intercepts at the Windows API level because the launch happens inside BugSplat64.dll. This is the correct approach.

### 2. Direct Memory Loadout Access

**Typed structs** (echovr-reconstruction):
- `src/NRadEngine/Network/CR15NetGame.h` — documents `+0x51420 + slot*0x40`
- `src/NRadEngine/Network/CR15NetGameLayout.h:24-41` — `+0x51420: loadout_table (16 slots × 0x40)`
- `src/NRadEngine/Game/LoadoutStructs.h:53-68` — `LoadoutEntry`: `static_assert(sizeof == 0xD8)`, `offsetof(bodytype) == 0x00`, `offsetof(teamid) == 0x08`, `offsetof(airole) == 0x0A`, `offsetof(xf) == 0x10`, `offsetof(loadout) == 0x30`
- `src/NRadEngine/Game/LoadoutResolver.h:17-24` — `LoadoutInstanceEntry`: `static_assert(sizeof == 0x40)`, `offsetof(data) == 0x30`, `offsetof(loadout_id) == 0x38`

**SNS message alternative** (`src/NRadEngine/Game/CR15NetLoadoutMessages.cpp`):

| Message | Send VA | Payload |
|---------|---------|---------|
| R15NetCurrentLoadoutRequest | 0x14012bdb0 | 4B |
| R15NetCurrentLoadoutResponse | 0x14012c1c0 | 4B |
| R15NetSaveLoadoutRequest | 0x1401305e0 | 4B |
| R15NetSaveLoadoutSuccess | 0x1401309f0 | 4B |
| R15NetLoadoutNumberNotification | 0x14012da90 | 12B |

Serialization: `EasyStream_SerializeLoadoutArray` @ 0x140134630 (magic 0xc8c33e4833671bbd). Handler docs: `src/NRadEngine/Network/CR15NetGameLayout.h:43-113`.

---

## High Hackiness

### 3. Server Flags Byte Patch (40-byte NOP sled at 0x1580C3)

The patch is inside `FUN_140116720` (ReVault: PreprocessCommandLine, 808+ lines). It forces bits in game state flags at `CR15NetGame + 0x2DA0`:
- `src/NRadEngine/Game/MatchLifecycle.cpp:33-40,48-61` — `flags = **(uint32_t**)(game + 0x2DA0)`: bit 1, bit 2 (loadout_save_allowed), bit 3 (loadout_action_enabled), bit 6 (LAN), bit 14 (game_mode_active)
- `src/NRadEngine/Network/CR15NetDedicatedLobby.cpp:17-27` — flag check determines netconfig (LAN vs dedicated)
- `extras/EchoVR/unlock_validator.cpp:75,95` — `static_assert(offsetof(network_state_flags) == 0x2DA0)`

The game's CLI registry (`src/NRadEngine/Core/CLIArguments.h`, from `FUN_1400fea00`) has NO `-server` or `-dedicated` argument. These bits are set during initialization code that the byte patch targets. The NOP sled is the correct approach — there is no CLI alternative.

### 4. Offline Mode (6 scattered patches)

A centralized offline state exists:
- `src/NRadEngine/Game/CNSUser.cpp:1180-1186` — `CNSUser_Offline()`: checks login state at `+0x90`, state 7 = offline
- `src/NRadEngine/Game/CNSUser.cpp:1368-1400` — `CNSUser_SetOffline_0`: sets bit 4 at `+0x90`
- `offlinemode` game expression registered in 20+ script files

However, the reconstruction does NOT document which of the 6 specific call sites check `CNSUser_Offline()`. Different subsystems may check different conditions. **Plausible but unverified** — keep current approach.

### 5. "allow_incoming" Byte Patch (5 bytes at 0xF7F904)

The patch is inside `FUN_140f7f8b0` (ReVault: CBroadcaster::InitializeFromJson, 1440 bytes, called from `FUN_140145b30` = CR15NetDedicatedLobby constructor). The function reads `"|allow_incoming"` from JSON via `FUN_1405ee990`. The patch forces the return value to 1.

The JSON config file that feeds this function is `netconfig_dedicatedserver.json` (a game asset), NOT `_local/config.json`:
- `src/NRadEngine/Network/CR15NetDedicatedLobby.cpp:32-33` — loads from `json/r14/config/netconfig_dedicatedserver.json`
- `src/NRadEngine/Network/CBroadcasterConfig.h:73-81` — shows the JSON structure (already has `"allow_incoming": true`)
- `src/NRadEngine/Network/CR15NetLobbySystem.h:93-96` — `_local/config.json` (at `param_1+0x63240`) only overrides `dedicated_port` and `port_retries`

The game asset already has `allow_incoming: true`. The byte patch exists as insurance against the asset being wrong or the parsing failing. Putting it in `_local/config.json` would NOT work — that config doesn't feed into `CBroadcaster::InitializeFromJson`. The byte patch is the correct approach.

### 6. Spectatorstream Check Bypass (6 NOPs at 0x116F3D)

The patch is inside `FUN_140116720` (PreprocessCommandLine). `-spectatorstream` IS registered as a CLI argument in `FUN_1400fea00` (BuildCmdLineSyntaxDefinitions): `FUN_1400d31b0(param_2, "-spectatorstream", 0, 1, 0)` with help "Stream spectator mode matches".

The `isspectatorstream` game expression variable (4 bytes, int32) exists on R15NETGAMEEXPRESSION (symbol 0x993e022a8336e85a), registered in 20+ script files.

Both approaches work: add `-spectatorstream` to the synthetic command line, or set the expression variable directly via the game's property system. The NOP patch is the simplest and equally valid for a fixed binary.

---

## Medium Hackiness

### 8. GetProcAddress Shutdown Hack

- `src/pnsrad/Plugin/RadPluginAPI.cpp:73-192` — complete `RadPluginShutdown()` @ 0x180088df0: destroys Activities, Party, Friends, Users (vtable+0x28), VoIP, plugin context
- `src/pnsovr/pnsovr.cpp:147-172` — pnsovr's RadPluginShutdown: `g_RunEventLoop = false`, deletes Social/RichPresence/IAP/Users
- `src/pnsdemo/pnsdemo.cpp:140` — pnsdemo's RadPluginShutdown
- `src/pnsrad/pnsrad.def:7` — exported symbol

RadPluginShutdown is a DLL export on all platform DLLs. It could theoretically be hooked directly via MinHook, but the current GetProcAddress interception catches the exact moment the game resolves the function, which is simpler than hooking after dynamic DLL load.

### 9. Renderer/Effects/Deadlock Byte Patches

| Patch | Offset | Inside Function | ReVault VA | Identified As |
|-------|--------|-----------------|------------|---------------|
| Renderer skip | 0xFF581 | `FUN_1400ff4b0` (+0xD1) | 0x1400ff4b0 | Game initialization / logging setup (252+ lines) |
| Effects skip | 0x62CA91 | `FUN_14062c940` (+0x151) | 0x14062c940 | CLevel::Load (`d:\projects\rad\dev\src\engine\libs\nodes\clevel.cpp`, 447 bytes) |
| Deadlock monitor | 0x1D3881 | `FUN_1401d3850` (+0x31) | 0x1401d3850 | Deadlock monitor thread — loops `Sleep(1000)`, logs "Deadlock detected!" |

Reconstruction names (Quest): `CGRenderer::Initialize` @ Quest:0x188e278 (`src/NRadEngine/Game/CGRenderer.cpp:99`). These could be converted to MinHook function hooks for better self-documentation, but the byte patches work correctly for the fixed binary.

### 10. HttpConnect URL Matching

Endpoints are per-service with ad-hoc config — there is no centralized service registry:
- `src/NRadEngine/Network/CLoginService.h:19-25` — `loginservice_host` config key, CLI `-loginhost`
- `src/pnsradmatchmaking/Matchmaking/CNSRadMatchmaking.h:31` — `matchingservice_host` config key
- `src/NRadEngine/Network/CHTTPApi.h:13-17` — hardcoded: `api.readyatdawn.com`, `graph.oculus.com`, `s3-us-west-2.amazonaws.com`

The current HttpConnect hook with strstr URL matching and config fallback chain is the correct approach given the per-service architecture.

---

## Low/No Hackiness (Fine as-is)

**11. Loading Tips** — RET patches on `R15PickLoadingTipNode` @ 0x140bd9670, `R15SelectLoadingTipNode` @ 0x140be6d10, `R15SelectLoadingTipNode_2` @ 0x140be7c90. Leaf functions, safe.

**12. WinHTTP→libcurl** — COM interception. Reconstruction confirms: `client/network/CHTTPApi_Curl.h:1,4` — "Replaces WinHTTP/WinINet with libcurl (OpenSSL backend)".

**13. Delta Time Fix** — JLE→JAE at 0xCF46D. Legitimate bug fix (signed vs unsigned comparison).

**14. SSL/TLS Modernization** — Schannel hook on `AcquireCredentialsHandleW`. WinHTTP is fully replaced by libcurl (CoCreateInstance hook), but Schannel (Windows TLS provider) is still used by game code that makes TLS connections directly through the Windows security APIs. This hook enables modern TLS protocols and cipher suites for those remaining code paths.

---

## Cross-Cutting — Validated Summary

| Recommendation | Status | Evidence |
|---------------|--------|----------|
| Use reconstruction as type library | **VALIDATED** | LoadoutStructs.h, LoadoutResolver.h have static_assert |
| Convert byte patches to function hooks | **VALIDATED** | ReVault provides Windows VAs for all patched functions |
| Single hook for crash reporter | **WRONG** | Crash reporter is in BugSplat64.dll, not echovr.exe |
| Leverage SNS messages for loadout | **VALIDATED** | 5 message types in CR15NetLoadoutMessages.cpp |
