# Quest Hijack-Point Map — Windows nevr-runtime → Quest/arm64 equivalents

**Task:** #7 — symbol-grounded inventory mapping every Windows nevr-runtime
hijack / patch / hook point to its Quest arm64 equivalent, so the whole hack
set (pnsrad reconstruction + all patch points) can be ported to Quest.

**Scope of this pass:** **symbol-level only.** Every Quest row is matched by
**demangled C++ symbol NAME** against the extracted arm64 libraries. arm64
addresses are NOT the x86 VAs — they are the real ELF dynsym VAs (base `0x0`).
**Logic verification is PENDING DECOMPILATION** (libr15.so id 576 in ReVault
is 0% decompiled — 139 638 `fcn.*` bodies, all `source: imported`). A NAME
match proves the target function *exists* on Quest; it does not prove the byte
offset / prologue / branch that each Windows patch rewrites. Those are the
"REQUIRES DECOMPILATION" list at the end.

## Ground-truth sources

| Side | Source | Command |
| --- | --- | --- |
| Windows | `src/runtime/hook/addresses.h`, `mode_patches.cpp`, `wave0_instrumentation.cpp`, `asset_cdn.cpp`, `initialize.cpp`, `src/abi/echovr_functions.cpp`, `plugins/common/include/address_registry.h`, `plugins/{broadcaster-bridge,log-filter,crash-handler}` | source read |
| Quest symbols | `.../quest_triage/apk_contents/lib/arm64-v8a/{libr15,libpnsrad,libpnsovr,libpnsradmatchmaking,libovrplatformloader}.so` | `nm -DC <lib>` (demangled name + arm64 VA) |
| Quest addresses | ReVault binary **id 576 = libr15.so** (aarch64 ELF, base `0x0`) | `revault fn show <0xVA> --binary libr15.so` |

Symbol counts (defined dynsyms): libr15 110 621, libpnsrad 16 070,
libpnsovr 15 982, libpnsradmatchmaking 15 514, libovrplatformloader 1 487.

**Injection vector (from the skill / `CLAUDE.md`):** Windows statically imports
`BugSplat64.dll` + `LibOVRPlatform64_1.dll`; nevr ships replacements under those
names. On Quest there is **no BugSplat .so** — the crash reporter is
statically-linked `google_breakpad` (confirmed below). The Quest startup vector
is replacing an APK-bundled `NEEDED` lib; `libovrplatformloader.so` is the
natural pick (it is in libr15's `DT_NEEDED` and exports the ovr_* symbols the
game resolves). That is a separate porting task; this doc only maps the points.

Legend for **found**: `yes` = exact-name symbol present with an arm64 VA;
`partial` = the containing class/subsystem is present but the exact Windows
target function name is not a 1:1 symbol (inlined, renamed, or template-bound);
`no` = no Quest equivalent (see DIVERGENT section).

---

## 1. Boot / Mode — engine byte-patches & function hooks (echovr.exe → libr15.so)

These are byte patches / MinHook detours nevr applies into the game image
itself. Windows VA = ImageBase `0x140000000` + RVA. Quest VA from `nm -DC`.

| Windows point | echovr.exe VA | Purpose | Quest symbol | .so | arm64 VA | found | notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| PreprocessCommandLine (`FUN_140116720`) — `SERVER_FLAGS_CHECK`, `SPECTATORSTREAM_CHECK`, `NOOVR_SPECTATOR` NOP-sleds | `0x140116720` | force server flags @ game+0x2DA0; bypass -spectatorstream/-noovr gates | `NRadEngine::CncaGame::PreprocessCommandLine()`; `NRadEngine::NRadGame::CR15NetServer::PreprocessCommandLine()` | libr15 | `0x17e84dc` / `0x124af80` | yes | server has its own override `CR15NetServer::PreprocessCommandLine` — likely no byte-patch needed on Quest, just call the server variant |
| BuildCmdLineSyntaxDefinitions | `0x1400FEA00` | CLI arg registry (hooked to inject args) | `NRadEngine::CncaGame::BuildCmdLineSyntaxDefinitions(NRadEngine::CMainArgSyntax&) const`; `...CR15NetServer::BuildCmdLineSyntaxDefinitions(...)` | libr15 | `0x17e93d4` / `0x124ad98` | yes | takes `CMainArgSyntax&` |
| CR15Game::InitializeGlobalGameSpace (`INIT_GLOBAL_GAMESPACE`) | `0x140110ab0` | client-only actor/dialogue lookup fatals in server mode; hook returns early setting gamespace ptr | `NRadEngine::NRadGame::CR15Game::InitializeGlobalGameSpace(NRadEngine::CncaGameSpace*)` | libr15 | `0x11f6058` | yes | **ReVault-confirmed** (688 bytes, imported). arg is `CncaGameSpace*` |
| CBroadcaster::InitializeFromJson (`ALLOW_INCOMING` MOV eax,1) | `0x140f7f8b0` | force `allow_incoming:true` in netconfig | `NRadEngine::CBroadcaster::Initialize(NRadEngine::CJson const&, NRadEngine::CSymbol64)` | libr15 | `0x24ea8b8` | yes | **ReVault-confirmed** (1344 bytes, imported). The Windows "InitializeFromJson" = Quest `Initialize(CJson const&, CSymbol64)` |
| CGRenderer::Initialize (`HEADLESS_RENDERER`) | `0x1400ff4b0`(+0xD1) | skip renderer init headless | `NRadEngine::CGRenderer::Initialize()` | libr15 | `0x188e278` | yes | **ReVault-confirmed** — patch_addresses.h already claimed `Quest:0x188e278`; **verified correct** |
| CLevel::Load (`HEADLESS_EFFECTS`) | `0x14062c940`(+0x151) | skip effects resource load | `NRadEngine::CLevel::Load(NRadEngine::CSymbol64, NRadEngine::CLevelLoadOptions const&)` | libr15 | `0x194988c` | yes | also a `CModelLoadOptions` overload `0x1954b88` |
| ApplyGraphicsSettings (`HEADLESS_APPLY_GRAPHICS`, `FUN_140c31870`) | `0x140c31870` | ~66 CGRenderer setting calls, crash headless | — | libr15 | — | partial | no standalone `ApplyGraphicsSettings` symbol; the settings calls live under `CGRenderer::*` (122 syms). Needs decompilation to locate the call site to NOP |
| DirectInput8Create call (`HEADLESS_DINPUT`) | `0x141055DBB` | kill HID enum thread | — | — | — | **no** | DirectInput is Windows-only; Quest input is Android/OpenXR — DIVERGENT |
| R15PickLoadingTipNode (`LOADING_TIP_PICK`) | `0x140bd9670` | RET in server mode | `NRadEngine::NRadGame::CR15PickLoadingTipNode` (via `BindBranchingNode<SR15PickLoadingTipNodeData, CR15PickLoadingTipNode>`) | libr15 | `0x1ffc530` (bind thunk) | partial | node class exists; the node's own entry-point symbol is template-bound, not a bare `R15PickLoadingTipNode`. Needs decompilation for the RET target |
| R15SelectLoadingTipNode (`LOADING_TIP_SELECT`) | `0x140be6d10` | RET in server mode | `NRadEngine::NRadGame::CR15SelectLoadingTipNode` | libr15 | `0x1ffd574` (bind thunk) | partial | as above; also `CR15ShuffleLoadingTipsNode` present |
| CPrecisionSleep::BusyWait (`PRECISION_SLEEP_BUSYWAIT` → RET; BUG#13) | `0x1401ce4c0` | eliminate QPC+SwitchToThread spin | — (see `CSysThread::PrecisionSleep`) | libr15 | — | partial | Quest has `NRadEngine::CSysThread::PrecisionSleep(unsigned long long)` `0xf89b7c` — the busy-wait is almost certainly inlined there. Wine-specific optimization; may not be needed on Android. Needs decompilation |
| CPrecisionSleep::Wait (wave0 0e, BUG#11/#12) | `0x1401ce0b0` | persistent hi-res timer | `NRadEngine::CSysThread::PrecisionSleep(unsigned long long)` | libr15 | `0xf89b7c` | partial | Windows `WaitableTimer` rewrite is OS-specific; Quest equivalent is the same `PrecisionSleep` but the Android backing (nanosleep) differs — DIVERGENT logic |
| CSpinWait::WaitForValue (`SPINWAIT_WAIT_FOR_VALUE`; BUG#14) | `0x141500ed8` | fix inverted backoff | — (`CSpinLockPtr::*` present) | libr15 | `0xf91a54`… | partial | no `WaitForValue`/`CSpinWait` symbol; spin primitive is `NRadEngine::CSpinLockPtr`. Needs decompilation to find the backoff loop |
| CDeadlockMonitor (`DEADLOCK_MONITOR` NOP) | `0x1401d3850`(+0x31) | disable panic check | `NRadEngine::CDeadlockMonitor::Main()` / `::Tick()` / `::Crash()` | libr15 | `0xf90d98` / `0xf90e4c` / `0xf90e64` | yes | **exact class match** — 3 named methods. The NOP target is the check inside `Tick`→`Crash` |
| CJson_GetFloat / CJson::Real (`CJSON_GET_FLOAT` hook) | `0x1405fca60` | override arena rule floats | `NRadEngine::CJson::Real(char const*, float, unsigned int) const` | libr15 | `0xfa4358` | yes | **ReVault-confirmed** (24 bytes — the thunk). Signature matches Windows `CJsonGetFloatFunc(root,path,default,required)` exactly. Also `Real(CSymbol64,float)` `0xfa53c0` |
| JsonValueAsString (hook) | `0x1405fe290` | string config reads | `NRadEngine::CJson::String(...)` | libr15 | — | partial | no bare `CJson::String` symbol surfaced; JSON string accessor exists under CJson (431 syms) but the exact overload needs decompilation |
| LoadLocalConfig (hook) | `0x140179eb0` | inject `_local/config.json` values | `NRadEngine::NRadGame::CR15NetGame::LoadLocalConfig()` | libr15 | `0x124d594` | yes | **exact match** |
| NetGameSwitchState (hook, state machine) | `0x1401b8650` | drive server through lobby states | `NRadEngine::NRadGame::CR15NetGame::SetState(CR15NetGame::EState)` | libr15 | `0x125b884` | yes | Quest names it `SetState(EState)` (Windows "SwitchState"); same role |
| NetGameScheduleReturnToLobby (export) | `0x1401a89f0` | force return-to-lobby | `NRadEngine::NRadGame::CR15NetGame::ReturnToLobby()` | libr15 | `0x1291e58` | yes | also the `InvokeExclusiveUpdate<...ReturnToLobby>` thunk `0x12e59a8` |
| CSysDLL_GetSymbol (hook → ServerLib factory) | `0x1400eaef0` | inject in-proc `GameServerLib` for `"ServerLib"` | — (game-internal `CSysDLL` loader) | libr15 | — | partial | on Quest all libs are already `dlopen`'d `.so`s; the `CSysDLL` wrapper symbol isn't exported. Quest equivalent is `dlsym`; the injection strategy differs. Needs decompilation |
| CSysDLL_Load (hook → pnsradgameserver redirect) | `0x14105aa70` | redirect failed load to in-proc factory | — | libr15 | — | partial | same as above — Quest uses the real `libpnsradmatchmaking.so`/gameserver `.so`, so the "DLL eliminated" redirect may be unnecessary |
| GetProcAddress (hook — `"Users"` export → platform-DLL detect) | Windows API | exit(0) on `RadPluginShutdown` of platform DLL | `Users` (export in libpnsrad) | libpnsrad | `0x206448` | yes | the `"Users"` export the hook keys on **exists** in libpnsrad; Quest teardown crash is a `dlclose` concern, different mechanism |
| CLog::PrintfImpl (`VA_CLOG_PRINTF_IMPL`, log-filter/builtin-log-filter) | `0x1400ebe70` | capture/filter/file all game log output | `NRadEngine::CLogger::FormatString()`, `NRadEngine::CLoggingData::ExecuteAllCallbacks(char const*, unsigned long long, NWriteLog::ELogLevel)` | libr15 | `0xf8928c` / `0xf88dbc` | partial | logging subsystem present (`CLogger`, `CLoggingData`, `NWriteLog::ELogLevel`) but not a symbol literally named `CLog::PrintfImpl`. The callback-registration path (`ExecuteAllCallbacks`) is a cleaner Quest hook than a byte detour. Needs decompilation to pin the printf sink |
| Wwise init/renderaudio (`WWISE_INIT` / `WWISE_RENDERAUDIO`) | `0x140209920` / `0x140fa5610` | disable non-VOIP audio | `NRadEngine::CWwiseEngine::Initialize(CFlagsT<unsigned int,1>)`, `::RenderAudio()`, `::InitMemory()` | libr15 | `0x25489bc` / `0x2548308` / `0x2548370` | yes | **Quest HAS Wwise** (contra any "no audio" assumption). Named methods map cleanly; `libOculusSpatializerWwise.so` also bundled |
| HttpConnect (hook) | `0x1401f60c0` | route HTTP through curl/stub | `evhttp_connection_base_new` / `evhttp_connection_connect` (libevent, statically linked) | libr15 | `0xff620c` / `0xff5364` | partial | Quest HTTP is **libevent** (`evhttp_*`), not a `CHttp::Connect` method. Different hook surface — intercept at libevent, not a game method |
| HTTP-listener bringup (wave0 0h, `FUN_1401F5B00`; BUG#62) | `0x1401f5b00` | fatal on session-API bind failure | — (libevent `evhttp_bind_socket*`) | libr15 | — | partial | same libevent surface; needs decompilation to find the game's listener wrapper on Quest |
| EndMultiplayer (wave0 0c; BUG#6) | `0x140162450` | null-check session ptr @ +0x2DA0 | `NRadEngine::NRadGame::CR15NetGame::EndMultiplayer()`; `CR15Game::EndMultiplayer()` | libr15 | `0x1288c64` / `0x11fa300` | yes | exact; the +0x2DA0 offset is x86-struct-specific and must be re-derived on arm64 |
| GetTimeMicroseconds (wave0 0a; BUG#1 overflow) | `0x1400d00c0` | overflow-safe µs clock | `NRadEngine::CSysTime::IcGetElapsedUs(unsigned long long, unsigned long long)` | libr15 | `0x379d050` (guard var; fn body via ReVault) | yes | µs elapsed-time primitive present. The `(count*1e6)/freq` overflow may or may not exist in the arm64 build — verify by decompilation |
| CTimer_GetMilliSeconds (wave0 0b) | `0x1400d0110` | ms overflow observation | `NRadEngine::CTimer::DelayTicks(...)`, `CSysTime::*` | libr15 | `0x11cd290` | partial | ms accessor not 1:1 named; observation-only, low priority |
| CleanupPeers (BUG#2, fixed via BUG#1) | `0x140f76500` | peer-timeout mass-disconnect | `NRadEngine::SBroadcasterData::CleanupPeers()`; `SClientData::CleanupPeers()` | libr15 | `0x24ef9b0` / `0x250114c` | yes | exact; fixed indirectly by the GetTime fix so no direct patch needed |
| ~~HandleDXError~~ **NOT A REAL FIX — see N24** (was 0d; BUG#7) | `0x140551070` | ~~recover transient DXGI errors~~ — the x86 hook was removed: the original only handles DEVICE_REMOVED, so there were no transient errors to recover. ReVault names it `ReportDXGIDeviceRemoved`. **Nothing to port.** | — | — | — | **no** | D3D11/DXGI is Windows; Quest renders **Vulkan** (`CVR_VK`, `CGTimerQueryPoolVK`, VkLayers bundled) — DIVERGENT, needs a Vulkan-error analogue |

## 2. pnsrad — social + networking reconstruction (pnsrad.dll → libpnsrad.so)

**This is the cleanest map in the set: the pnsrad reconstruction (`~/src/echovr-reconstruction/src/pnsrad/`, removed from this repo
2026-07-27 — it built into nothing and was superseded by the ws_bridge
runtime approach) is a from-scratch
reconstruction of the RAD social platform library, and `libpnsrad.so` IS the
Quest build of that same library.** Every reconstructed `NRadEngine::*` class
is present in libpnsrad.so with dozens of named methods each. The port target
is essentially "build our pnsrad reconstruction for arm64 / match its ABI to
libpnsrad.so," not a byte-patch.

| Reconstructed class (echovr-reconstruction, src/pnsrad) | Quest symbol (representative) | .so | method count | found |
| --- | --- | --- | --- | --- |
| `NRadEngine::CNSRADUsers` | `CNSRADUsers::CreateUserInternal(LocalUserID)`, `CNSRADUsers::CNSRADUsers(CTcpBroadcaster&)` | libpnsrad | 8 | yes |
| `NRadEngine::CNSRADFriends` | (+ `CNSRADFriends_protocol.h`) | libpnsrad | 62 | yes |
| `NRadEngine::CNSRADParty` (+ `cnsrad_party_full`) | | libpnsrad | 93 | yes |
| `NRadEngine::CNSRADActivities` | | libpnsrad | 62 | yes |
| `NRadEngine::CNSIUsers` (interface) | | libpnsrad | 59 | yes |
| `NRadEngine::CNSIParty` (interface) | | libpnsrad | 24 | yes |
| `NRadEngine::CNSUser` | | libpnsrad | 99 | yes |
| `NRadEngine::CBasicErr` / `CBaseErr` | | libpnsrad | 16 | yes |
| `NRadEngine::CJsonTraversal` | | libpnsrad | 20 | yes |
| `NRadEngine::CProfileJsonTraversal` | | libpnsrad | 4 | yes |
| RadPlugin ABI exports (`pnsrad.def`) | `RadPluginInit`, `RadPluginMain`, `RadPluginShutdown`, `RadPluginSetAllocator`, `RadPluginSetEnvironment`, `RadPluginSetPresenceFactory`, `RadPluginSetFileTypes`, … | libpnsrad | all present (+ `RadPluginSetJavaEnv` — Android-only extra) | yes |

Broadcaster message registry (the `SBroadcaster*` event types the reconstruction
serializes) — all present as `NRadEngine::SBroadcaster*::TypeId()` in **libr15**:
`SBroadcasterPingUpdate`, `SBroadcasterConnectEvent`, `SBroadcasterDisconnectEvent`,
`SBroadcasterIntroduceAckEvent`, `SBroadcasterIntroduceReqEvent`,
`SBroadcasterIntroduceFinEvent`, `SBroadcasterConnectFailedEvent`,
`SBroadcasterIntroduceFailEvent`.

Sister social libs (not yet reconstructed on Windows, present on Quest for
reference): **libpnsovr.so** (7 730 `NRadEngine` syms — the Oculus-platform
social backend, mirrors Windows `pnsovr.dll`) and **libpnsradmatchmaking.so**
(7 476 `NRadEngine` syms — matchmaking, mirrors `pnsradmatchmaking.dll`).

## 3. Broadcaster network core (echovr.exe / broadcaster-bridge → libr15.so)

| Windows point | echovr.exe VA | Purpose | Quest symbol | .so | arm64 VA | found |
| --- | --- | --- | --- | --- | --- | --- |
| BroadcasterSend (`VA_BROADCASTER_SEND`, bridge `CBroadcaster_Send`) | `0x140f89af0` | wire-send of broadcaster messages; mirror-tap | `NRadEngine::CBroadcaster::Send(CMatSym, int, void const*, unsigned long long, void const*, unsigned long long, SPeer, unsigned long long, float, CSymbol64)` | libr15 | `0x24e7e94` | yes |
| BroadcasterReceiveLocal (`VA_BROADCASTER_RECEIVE_LOCAL`) | `0x140f87aa0` | local-event receive tap | `NRadEngine::CBroadcaster::SendLocalEvent(CMatSym, unsigned int, void const*, unsigned long long)` + `ReceiveLocalEvent` path | libr15 | `0x24eda04` | yes |
| BroadcasterListen (`VA_BROADCASTER_LISTEN`) — also `ENGINE_ENTITY_LOOKUP` null-guard target | `0x140f80ed0` | message listen registration | `NRadEngine::CBroadcaster::ListenProxy<SBroadcasterData, S*Event>(...)` (per-message templates) | libr15 | `0x250bbd4`… | yes |
| TcpBroadcasterListen | `0x140f81100` | TCP listen registration | `NRadEngine::CTcpBroadcaster::ListenProxy<CNSConfigs, S*>(...)` | libr15 | `0x1936978`… | yes |
| EnableBodyComponents (bridge `VA_ENABLE_BODY_COMPONENTS`) | `0x140cd07e0` | spectator body-component enable for mirror | `NRadEngine::NRadGame::CR15NetSpectatorCameraCS::EnableBodyComponents(unsigned int, CncaGameSpaceMT*)` | libr15 | `0x219c8dc` | yes |
| ENGINE_ENTITY_PROP_DISPATCH null-guard (`fcn.140f87aa0`) | `0x140f87aa0` | server-mode AV guard (same VA as ReceiveLocal) | (as BroadcasterReceiveLocal above) | libr15 | `0x24eda04` | yes |

**Note:** the Windows patch set overloads three raw VAs
(`0x140f80ed0`/`0x140f87aa0`) between "broadcaster listen/receive" (bridge) and
"engine entity lookup/prop-dispatch null-guard" (mode_patches). On arm64 these
resolve to distinct `CBroadcaster` methods — the null-guard hooks are
**server-mode AV workarounds that must be re-derived** against the arm64 struct
layout, not reused by VA.

## 4. Gameserver (BugSplat64.dll / ex-pnsradgameserver.dll → libr15.so CR15NetServer)

The Windows gameserver is an in-process `IServerLib` factory injected via
`CSysDLL_GetSymbol("ServerLib")`. Quest ships the server code inside libr15 as
`CR15NetServer` (12 named methods).

| Windows role | Quest symbol | .so | arm64 VA | found |
| --- | --- | --- | --- | --- |
| Server library init | `NRadEngine::NRadGame::CR15NetServer::Initialize()` | libr15 | `0x12b303c` | yes |
| Engine config | `CR15NetServer::ConfigureEngine()` | libr15 | `0x124b154` | yes |
| Gamespace creation | `CR15NetServer::CreateGameSpace(ELevelLoadChannel, SLevelLoad const&, unsigned int, float)` | libr15 | `0x12b3034` | yes |
| Console close CB | `CR15NetServer::ConsoleCloseCB()` | libr15 | `0x12b2ca0` | yes |
| LAN server variant | `CR15NetLanServer::PreprocessCommandLine()` | libr15 | `0x12adfcc` | yes |

`ServerLib`/`IServerLib` is a nevr-side wrapper interface — no direct Quest
symbol; on Quest the server is `CR15NetServer` directly (no external DLL to
substitute), so the whole `CSysDLL_Load`/`GetSymbol` injection dance is
**not required** on Quest.

## 5. Crash reporting (BugSplat64.dll replacement → statically-linked breakpad)

| Windows point | Purpose | Quest equivalent | .so | arm64 VA | found |
| --- | --- | --- | --- | --- | --- |
| BugSplat crash handler (`BUGSPLAT_CRASH_HANDLER`, hooked to suppress) | `0x1400dbbc0` fatal handler → ExitProcess(1)+int3; hook logs & returns in server mode | game-side `NRadEngine::CDebugCrashReport` (`GetCmdLineEnableBugSplat`, `crashcbfunc`, `dumpfilepath`, `GetFileStamp`); globals `kEnableBugSplat`, `gBugSplatShowDialog` | libr15 | `0xf8a370` etc. | yes |
| BugSplat64.dll (import replaced) / BsSndRpt64.exe launch (crash-handler plugin blocks) | provide breakpad minidumps instead of BugSplat | **statically-linked** `google_breakpad::ExceptionHandler::HandleSignal(int, siginfo*, void*)`, `::WriteMinidump(...)`, `::WriteMinidumpForChild(...)`; global `NRadEngine::gBreakpadExceptionHandler` | libr15 | `0xfb1fcc` / `0xfb3290` / `0xfb3e18`; global `0x3763a50` | yes | **Confirms the skill's note: there is NO BugSplat .so to hijack on Quest — breakpad is compiled in.** The crash-handler plugin's ExitProcess/TerminateProcess/CreateProcess suppression is Windows-only; the Quest analogue is registering/replacing the breakpad `ExceptionHandler` callback |

## 6. OVR / Platform (LibOVRPlatform64_1.dll → libovrplatformloader.so)

| Windows point | Purpose | Quest equivalent | .so | found |
| --- | --- | --- | --- | --- |
| PatchBypassOvrPlatform (`0x1580e5` JNE NOP) | skip OVR-platform init branch when DLL unavailable | branch inside `CR15Game` platform-decision (`PlatformModuleDecisionAndInitialize`) — needs decompilation for the arm64 branch | libr15 | partial |
| PatchBlockOculusSDK (LoadLibraryW/ExW hook blocks `libovrplatform*`) | save RAM/CPU by blocking OVR SDK load | on Quest `libovrplatformloader.so` is a hard `DT_NEEDED` of libr15 — cannot simply be blocked; would stub the exports instead | libovrplatformloader | n/a (DIVERGENT strategy) |
| LibOVRPlatform64_1 import (statically imported, replaced on Windows) | platform SDK entry points | `libovrplatformloader.so` exports **1 056 `ovr_*` functions** (`ovr_PlatformInitialize*`, `ovr_Message_*`, `ovr_AbuseReport_*`, achievements, …) | libovrplatformloader | yes |
| LibOVRRT64 render loader (`FUN_141360790`, 73 `GetProcAddress`) — the leVR swapchain surface | HMD swapchain / `ovr_CreateTextureSwapChainDX` etc. | Quest is **native Vulkan** via `vrapi` (`libvrapi.so` bundled) + `CVR_VK::CreateRenderTarget`; the D3D→OpenXR bridge is Windows/Wine-only | libr15 / libvrapi | DIVERGENT — see `docs/design/2026-06-09-levr-porting-analysis.md` |

## 7. XPID provider string patch (echovr.exe .rdata → libr15.so .rodata)

| Windows point | echovr.exe VA | Purpose | Quest equivalent | notes |
| --- | --- | --- | --- | --- |
| `XPID_PLATFORM_SHORT_NAME` "PSN"→"DSC" | `0x1416d0ee0` | Discord-based XPID formatting | .rodata string edit on arm64 | The three PSN→DSC string edits are **data patches**, not code — on Quest they are equivalent `.rodata` string edits (find the `OlPrEfIx`/"PSN"/"PSN-" strings in libr15's read-only data via `revault search-strings`). Not yet located; low-risk, decompilation-independent. `found: pending string search` |

---

## REQUIRES DECOMPILATION (feeds the libr15 decompile task, id 576)

A NAME match proves the function exists; every **byte patch** and every
**struct-offset** below needs the arm64 function body decompiled before it can
be ported. Priority order (highest = blocks the most of the hack set):

1. **PreprocessCommandLine** (`0x17e84dc` / server `0x124af80`) — the
   server-flags NOP-sled writes bits at `game+0x2DA0`; the arm64 struct offset
   and the branch layout are entirely different. Decompile to re-derive the
   flag offset and the spectatorstream/noovr gates. **Blocks all of server mode.**
2. **CR15Game::InitializeGlobalGameSpace** (`0x11f6058`) — the early-return
   hook needs the arm64 offset of the gamespace pointer (Windows `+0x7AF0`) and
   the actor/dialogue-scene lookup to skip.
3. **CBroadcaster::Initialize(CJson const&, CSymbol64)** (`0x24ea8b8`) — locate
   the `allow_incoming` parse to force-true (Windows `MOV eax,1`).
4. **CR15NetGame::SetState** (`0x125b884`) + **EState enum** — the state-machine
   driver; need the EState values to reproduce the lobby-state sequencing.
5. **Broadcaster send/receive/listen** (`CBroadcaster::Send` `0x24e7e94`,
   `SendLocalEvent` `0x24eda04`, `ListenProxy` family) — for the broadcaster
   bridge mirror-tap: need the arm64 calling convention & the message-dispatch
   struct offsets (Windows `+0x51420`/`+0x51450` loadout array).
6. **The logging sink** — pin the real printf/callback entry
   (`CLogger::FormatString` `0xf8928c` / `CLoggingData::ExecuteAllCallbacks`
   `0xf88dbc`) so log-filter can hook it.
7. **CJson::Real** (`0xfa4358`) — 24-byte thunk; confirm the arm64 thunk shape
   for the arena-rules override hook.
8. **CGRenderer::Initialize +offset** (`0x188e278`) and **CLevel::Load +offset**
   (`0x194988c`) — the headless skip points are *interior* offsets, not the
   function entry; decompile to find them.
9. **CDeadlockMonitor::Tick/Crash** (`0xf90e4c`/`0xf90e64`) — locate the panic
   check to NOP.
10. **Loading-tip nodes** (`CR15PickLoadingTipNode` / `CR15SelectLoadingTipNode`
    via `BindBranchingNode`) — find the node run function to RET.
11. **Spin/precision-sleep** (`CSysThread::PrecisionSleep` `0xf89b7c`,
    `CSpinLockPtr`) — only if the Wine-specific CPU pathology reproduces on
    Android; likely lower priority.
12. **EndMultiplayer** (`0x1288c64`) — re-derive the session-ptr offset
    (Windows `+0x2DA0`) on arm64.
13. **XPID PSN→DSC** — `revault search-strings libr15.so` for the prefix table
    (data, not code, but must be located).

## NO QUEST EQUIVALENT / DIVERGENT (do not port by name)

| Windows point | Why divergent |
| --- | --- |
| DirectInput8Create kill (`HEADLESS_DINPUT` `0x141055DBB`) | DirectInput is Win32; Quest input is Android/OpenXR/vrapi. No analogue |
| ~~HandleDXError transient recovery~~ (`0x140551070`; BUG#7) | **Struck 2026-07-29.** Two reasons, and the first is sufficient: the x86 fix does not exist — it was removed when its premise was falsified (N24), so there is no port to make and a Vulkan device-lost analogue would be a new fix justified by nothing. (The second reason, that DXGI is Windows-only and Quest is Vulkan, stands but is now moot.) |
| Headless graphics stubs (DXGI/D3D11 interception, `InstallHeadlessGraphicsHooks`) | same — Vulkan surface; the leVR bridge doc covers the render port separately |
| PatchBlockOculusSDK (LoadLibraryW/ExW hook) | `libovrplatformloader.so` is a hard `DT_NEEDED`; you cannot block-load it. Would stub exports instead |
| LibOVRRT64 D3D→OpenXR swapchain bridge (`FUN_141360790`) | Windows/Wine-only; Quest renders native Vulkan via `vrapi`. See `2026-06-09-levr-porting-analysis.md` |
| CPrecisionSleep::Wait `WaitableTimer` rewrite (BUG#11/#12) | Windows kernel-timer specific; Android backing is nanosleep — the *bug* may not exist on arm64 |
| CSysDLL_GetSymbol / CSysDLL_Load ServerLib injection | Quest server is `CR15NetServer` in-process; no external `pnsradgameserver.dll` to substitute, so no injection needed |
| GetProcAddress `RadPluginShutdown` exit(0) guard | Windows FreeLibrary teardown crash; Quest `dlclose` teardown is a different (if analogous) concern |
| BugSplat suppression plugin (CreateProcess/ExitProcess/TerminateProcess hooks) | breakpad is statically linked on Quest — no `BsSndRpt64.exe` to block; replace the breakpad `ExceptionHandler` callback instead |
| winhttp_stub / WinHTTP hook | Quest HTTP is statically-linked **libevent** (`evhttp_*`); intercept there, not WinHTTP |

---

## Verification status

- **Symbol NAMES + arm64 VAs:** ground truth = `nm -DC` on the real ELF `.so`
  files (instant, authoritative). Every `found: yes` row above carries a real
  demangled symbol and its arm64 VA.
- **ReVault (id 576) spot-confirmed** for: `InitializeGlobalGameSpace` @
  `0x11f6058` (688 B), `CGRenderer::Initialize` @ `0x188e278`,
  `CBroadcaster::Initialize(CJson const&,CSymbol64)` @ `0x24ea8b8` (1344 B),
  `CJson::Real(char const*,float,unsigned int)` @ `0xfa4358` (24 B),
  `google_breakpad::IsValidElf` @ `0xfd6d30`. All report `source: imported`,
  **`(no decompilation available)`** — i.e. every logic-verification is
  explicitly pending the decompile task.
- **Every byte-offset / struct-offset / prologue is `[UNVERIFIED]` on arm64**
  until decompiled. This doc does not assert any Quest *patch byte* — only that
  the target function exists by name.
