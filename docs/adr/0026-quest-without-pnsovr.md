# ADR 0026: The Quest build never loads libpnsovr; the game runs on the rad provider, as on PC

Status: proposed (#944). Not implemented. "Prediction" is what the first headset run must show; "What this does not
know" lists what no measurement in this tree covers.

## Context

`libpnsovr.so` is the game's adapter to Meta's platform SDK. It is the only library in the APK that calls the SDK: of
the game's libraries, `nm -D --undefined-only` counts 170 `ovr_*` imports in `libpnsovr.so` and none in `libr15.so`,
`libpnsrad.so` or `libpnsradmatchmaking.so`. It is also where the store's entitlement check lives:
its `RadPluginMain` (0x20671c) calls `ovr_PlatformInitializeAndroid` (0x2067ac), `ovr_Entitlement_GetIsViewerEntitled`
(0x206824) and `ovr_GetLoggedInUserID` (0x206828), and its `Update` (0x207534) pumps `ovr_PopMessage` (0x20754c,
0x207588) and calls `CSysOS::HardExit` (0x207624).

Everything this tree does on Quest today to log in, to stand in for missing Oculus answers and to replace the social
object depends on that library: the login rewrite hooks `CNSUser::SendLogInRequest` inside it (ADR 0003, "Login
interception"); the login prerequisites hook its `ovr_*` imports (#1183, `src/quest/login/login_prerequisites*`); the
social facade is selected by comparing a vtable pointer with its `CNSOVRSocial` (`src/quest/social/social_install.cpp`);
and #1154 maps 79 edits the community patcher makes to it (`tools/quest_patcher_sites.tsv`). Every one of those is a
contract with Meta's SDK and with a Meta-built binary, so each can break when either changes.

PC has none of this. `src/runtime/patch/pnsrad_enabler.cpp` rewrites the provider names `pnsovr` and `pnsdemo` to
`pnsrad`, so `pnsovr.dll` is never loaded, and repairs what pnsrad does not do for an online login.

## Decision

The Quest build never loads `libpnsovr.so` and stops shipping it. Every place `libr15.so` calls into the provider is
answered on libr15's side, through libr15's own GOT slots (`R_AARCH64_JUMP_SLOT`, BIND_NOW, the `GotHook` backend), never
by a hook inside a provider library and never by a write to a code page. The provider libr15 loads in pnsovr's place is
its own `libpnsrad.so`, already shipped and already loaded on every run, which is the PC design. Once nothing loads it,
`libpnsovr.so` and Meta's `libovrplatformloader_orig.so` leave the APK (see "Migration").

All addresses below are ELF vaddrs in the pinned store build (`libr15.so` build ID
`b243509c08ce677aeb95fa348016949b3fc45230`; the disassembly shows libr15 0x100000 higher). They were read with `readelf -rW`,
`nm -D` and `aarch64-linux-gnu-objdump -d` on the pinned libraries, and PC addresses from the installed `pnsrad.dll`
(sha256 `2410095d...`, the copy the disassembly indexes).

## 1. How libr15 loads the provider, and the hook that stops pnsovr loading

`CR15Game::Initialize` (0x11f9b40) calls `CNSProvider::InitGlobals`, then `CNSProvider::Create(handle&, name, json,
callbacks)` for the platform provider (`"pnsovr"`, rodata 0x2ba11f7, or `"pnsdemo"` 0x2ba1215 when `-noovr` sets bit 27;
the APK has no `libpnsdemo.so`) and for `"pnsrad"` (0x2ba1233), always (ADR 0003, "Which provider serves what"). The
platform provider's json carries `appid`, `lobbyversion`, `headsettype`, `activity` and `javaenv`; the pnsrad call passes
none.

`CNSProvider::Create` (0x192f3e4) unloads a handle it is given non-zero (0x192f414), copies the name into a 512-byte
buffer and calls `CModuleLoader::Load` (0x192f4ac); on success it looks up `InitGlobals` (0x192f4cc) and calls it with the
broadcaster. `CModuleLoader::LoadExplicit` (0x2819d4c):

1. `CSysModule::FindModule(name)` (0x2819d74). A module already loaded is stored in the handle and the function returns 0
   (success) at once, without running any `RadPlugin*` entry again (0x2819d78-0x2819da0).
2. Otherwise `CSysModule::Load` (0x2819db4), which ends in `dlopen("lib" + name + ".so", RTLD_NOW)` (0x2a9e1ec), then
   the module's `RadPluginSetAllocator`, the Java environment, presence factory, file types and environment setters,
   `RadPluginInit`, and `RadPluginMain(json)` (0x2819e2c); a nonzero result shuts the module down, unloads it and zeroes
   the handle.

`CR15NetGame`'s constructor (0x1283db8) stores the handles: +0x8 game, +0x10 platform, +0x18 microphone, +0x20 RAD.
`echovr.exe`'s `ConfigureEngine` (0x140145e50) has the same layout.

**The hook.** libr15's JUMP_SLOT for `CNSProvider::Create` (0x36f01a8; its two calls are both in `CR15Game::Initialize`) is answered by ours:
when the name is `pnsovr` or `pnsdemo` it calls the original with `pnsrad` and a null json, exactly the call libr15
makes for the RAD provider; any other name passes through. The platform handle then holds libpnsrad, as on PC. The two
orders the calls can come in end the same way: whichever runs first loads libpnsrad and runs its `RadPlugin*` entries
once; the second finds it (step 1) and both handles name the same module. `InitGlobals` runs once per `Create`, twice in
all, which is what PC does on every run (`echovr.exe`'s `CNSProvider::Create`, 0x140606690, calls `InitGlobals` on every
call, after its loader, 0x14105ab40, skips the `RadPlugin*` entries on a `GetModuleHandleA` hit). `dlopen("libpnsovr.so")` is never called, so
`RadPluginMain` (the SDK initialization and the entitlement check) and `Update`'s `HardExit` never run.

Rejected: refusing or redirecting `dlopen` (our dlopen GOT hook already sees it). A refused load makes `Create` fail
and zero the handle, so every platform call becomes an error path no run exercises; a redirected one gives libpnsrad's
entries the pnsovr json. The `Create` slot hands libr15 the same state the PC game is in.

## 2. Every call libr15 makes into the provider, and what it needs back

75 `CNSProvider` methods are defined in libr15 and 31 are called; every call goes through a libr15 JUMP_SLOT, and no
other library in the APK imports one. `CNSProvider::<Method>(handle)` is `CSysModule::FindMethod(handle, "<Method>")`
and a call; a method the module does not export logs "Method '%s' is unimplemented" and returns 0.

| Method | libr15 slot | Handle | Called from | pnsovr today | After (libpnsrad + our answer) |
| --- | --- | --- | --- | --- | --- |
| `Create`, `InitGlobals`, `ShutGlobals`, `Destroy`, `Update` | 0x36f01a8, 0x36f4320, 0x36cc3e0, 0x37012d8, 0x36c3350 | all | `CR15Game::Initialize`, `ShutdownEngine`, `Update`, `UpdateBroadcaster` | the SDK pump and `HardExit` in `Update` | libpnsrad's; its `Update` is a bare `ret` |
| `TcpBroadcaster`, `ConnectLogin`, `LoginConnected`, `DisconnectLogin` | 0x36e1e70, 0x36eda58, 0x36e6b40, 0x36e2130 | none (libr15's own) | `CR15NetGame` ctor, `BeginLogIn`, dtor | not involved | unchanged: libr15 opens the login connection itself |
| `Users` | 0x36efd08 | platform | `CR15NetGame::Initialize` (null-checked) | `CNSOVRUsers` | libpnsrad's `CNSRADUsers`; the login is answered at `CNSUser::LogIn` (section 3) |
| `Social` | 0x36ef528 | platform | `CR15NetGame::Initialize` (null-checked) | `CNSOVRSocial` | our facade (exists, ADR 0003 "The facade"); libpnsrad exports no `Social` |
| `IAP` | 0x36fae50 | platform | `CR15NetGame::Initialize` (null-checked) | pnsovr's IAP object | null, as on PC (`pnsrad.dll` exports no `IAP`) |
| `RichPresence` | 0x36d2710 | platform | `CR15NetGame::Initialize` (null-checked) | `CNSOVRRichPresence`, which our slot hook wraps in a pass-through trace (#393, `SelectRichPresenceObject`) | null, as on PC; the trace has no object to wrap |
| `ProviderID` | 0x36ed158 | platform | lobby `Find`/`Join` (9 calls), `PartyAddDemoMember` | the word at libpnsovr 0x70e380 | the `OVR` symbol `0xc8e8d0b1a89ff4f8`, ours |
| `UserProviderID` | 0x36f1c28 | platform | party, invite, friend-request UI and party member callbacks (19 calls) | same word | the `OVR` symbol, ours |
| `Mic*` (7) | 0x36e8788 and others | microphone | `CR15NetVoipBroadcasterCS` | Meta's microphone, through the SDK | libpnsrad's OpenSL ES microphone (`slCreateEngine` in its `MicCreate`/`MicStart` path) |
| `Voip*` (8) | 0x36fcaa8 and others | RAD | `CR15NetVoipBroadcasterCS`, `CR15NetVoipReceiverCS` | not involved | unchanged |
| `Leaderboards` | 0x36ea790 | — | `CNSLeaderboards` | not exported by either | unchanged (unimplemented) |

The four `Initialize` calls null-check the result at the call site in both games (Quest offsets +0x64770 `Users`,
+0x647b8 `IAP`, +0x647c0 `RichPresence`, +0x647c8 `Social`; PC `CR15NetGame::Initialize` 0x140173820 at 0x140173a06,
0x140173f00, 0x140174070, 0x1401740b1). About 12 later PC consumers of these pointers were sampled and all null-check;
about 150 were not read (see "What this does not know").

## 3. The rad provider carries the game, as on PC

`libpnsrad.so` exports `Users`, `Friends`, `Party`, `Activities`, `AllowGuests`, `ProviderID`, `UserProviderID`, `Mic*`
and `Voip*`, and not `IAP`, `RichPresence`, `Social` or `CheckEntitlement`. PC `pnsrad.dll` has the same gaps. PC runs on it
with these changes, each answered on Quest from libr15's side:

| PC change (`pnsrad_enabler.cpp`) | Why | Quest |
| --- | --- | --- |
| `pnsovr`/`pnsdemo` strings in `echovr.exe` become `pnsrad` | load pnsrad as the platform provider | the `Create` slot (section 1) |
| NOP the OVR branch in `PlatformModuleDecisionAndInitialize` (`echovr.exe` 0x1401580e5) | the OVR init path needs the Oculus runtime | none in libr15: `CR15Game::Initialize` has two `Create` calls (0x11f9f80, reached with `pnsovr` from 0x11f9cb0 or `pnsdemo`; 0x11f9fdc, `pnsrad`) and, after them, picks the microphone handle by its symbol (OVR the platform handle, DMO the demo handle, else RAD, 0x11fa00c-0x11fa090); nothing else in it depends on the provider |
| NOP `jne` at `pnsrad.dll` 0x180085b53 | the login check | `CNSUser::LogIn` slot, below |
| NOP the identity guard in `LogInSuccessCB` (0x18008ef25) and the state check in `LoginIdResponseCB` (0x18008f1b6) | the bridge injects a login pnsrad's state machine never made | not needed if our login goes out through `CNSUser::SendLogInRequest` with the identity set (below); the prediction checks it |
| `UserProviderID` body returns `OVR` (`provider_identity.h`) | the game compares a friend's provider with it; every id is `OVR-ORG-<id>` | the `UserProviderID` and `ProviderID` slots return `OVR` (on PC both exports share the patched body, 0x180088d70) |
| the social facade answers a null `Social()` | pnsrad has no `Social` | the Quest facade, selected when the original returns null instead of by pnsovr's vtable |
| `pnsradmatchmaking.dll` host rewritten | the matchmaker's host | unchanged (the Quest config-string seam, ADR 0003) |

**The login gate, on both.** `CNSRADUser::LogInInternal` (libpnsrad 0x1fd8ac) calls `CSysNet::HasInternet`; nonzero
fails the login with 400 "Bad log in request: Unsupported provider" (0x1fd8bc-0x1fd8d0, string 0x61f93d); zero calls
`CTcpBroadcaster::RequireConnection`. libr15's `HasInternet` (0xf989fc) asks Android's connectivity service, so it is
nonzero on an online headset. PC's twin is the same: `pnsrad.dll` 0x180085b40 calls 0x1800a00e0 (Network List Manager
`GetConnectivity`, nonzero on bit 0x40, IPv4 internet) and fails with the same string (0x180222ea8) — the `jne` PC NOPs.

**Nothing in rad sends a login.** `CNSRADUser`'s vtable (address point 0x6f1e10) is slot 0 `LogInInternal`, slot 1
`CNSUser::SendLogInRequest` (0x3ca208), slot 3 `CNSUser::UpdateInternal` (0x3ca6f4, a bare `ret`); libpnsrad has no direct
call to `SendLogInRequest`. Past the gate rad only opens the connection. On PC the bridge supplies the `LoginRequest`
(N91). On Quest pnsovr's `CNSOVRUser` sends it today, from its own `UpdateInternal` after the Oculus token callbacks
(ADR 0003, "Login interception").

**The Quest answer.** libr15's `CR15NetGame::LogIn` (0x125ea7c) builds the login json (`publisher_lock`, `lobbyversion`,
`hmdserialnumber`, `hmdproductname`, the game settings) and calls `CNSUser::LogIn` through libr15's JUMP_SLOT 0x36ca730, its
one call (0x125ebb4). The slot binds when libr15 loads, before any provider exists, to libr15's own copy (0x19349a0, the
same code as libpnsrad's 0x3cd370: return if flags at +0x9c have any of 0xe, set 0x2, tail-call vtable slot 0). Ours takes
that slot. It sets the user's identity (`[+0x88]` the NEVR account id, `[+0x90]` platform 4, `OVR_ORG`: the fields
`CNSUser::AccountID`, libpnsrad 0x3cd4c0, and the wire read), adds the login members the provider used to add, sets flag 0x2 as
`LogIn` does, and calls libr15's `CNSUser::SendLogInRequest` (0x1932838) on the open login connection. The internet gate
is never reached, so nothing inside libpnsrad is written. pnsovr adds its members in `CNSOVRUser::GotUserProofCB`, on a
copy of the game's json (0x1ed720), at 0x1ed73c-0x1ed9ac; each, and where it comes from after:

| Member | Today (pnsovr) | After |
| --- | --- | --- |
| `accountid`, `access_token`, `nonce`, `displayname`, `bypassauth`, `desiredclientprofileversion`, `nevr_identity`, `nevr_social` | rewritten by the login rewrite from the shared `LoginProfile` | the same builder, writing a fresh json instead of rewriting the Oculus one |
| `buildversion` | 630783, written by pnsovr (libpnsovr 0x1ed938-0x1ed944) | the same value, the game's own, cited at that address; the server reads it to classify the client as Quest |
| `appid` | from the platform `Create` json | captured by the `Create` hook before it drops that json |
| `lobbyversion`, `publisher_lock`, `hmdserialnumber` | set again by pnsovr (0x1ed930, 0x1ed95c, 0x1ed970) over the game's own | the game's own, set by `CR15NetGame::LogIn` |
| `system_info\|*` | `system_info\|headset_type` (0x1ed984), then `CNSUser::SystemInfo(json)` (0x1ed98c) and `system_info\|build_version` from `CSysInfo::OSBuildVersion` (0x1ed994-0x1ed9ac), libpnsovr's copies | libr15's own copies of the same two functions, `CNSUser::SystemInfo` (0x1932770) and `CSysInfo::OSBuildVersion` (0xf875c4); `headset_type`'s value in pnsovr is not traced |

## 4. Where pnsovr's `ovr_*` calls land today

Our sentinel is installed as `libovrplatformloader.so` (the name libr15, libpnsrad and libpnsovr list as `DT_NEEDED`).
The build adds `DT_NEEDED libovrplatformloader_orig.so`, Meta's loader renamed (`src/quest/sentinel/CMakeLists.txt`,
`justfile` `patchelf --set-soname`), and the sentinel defines no `ovr_*` symbol, so every one of libpnsovr's 170 imports
binds to Meta's library. No `ovr_*` call reaches code of ours except where a GOT hook in libpnsovr takes it (#1183). The
APK's `classes.dex` loads only `r15` and `vrapi` and has no `com/oculus/platform` class, so the Java side does not use the
platform SDK. libr15 also opens an HTTP connection to `graph.oculus.com` before it asks for `IAP`; with `IAP` null
nothing was found that sends on it.

After the change nothing imports an `ovr_*` symbol, so `libovrplatformloader_orig.so` is needed by nothing and leaves
the APK with libpnsovr. The sentinel keeps the `libovrplatformloader.so` name, which libr15 still lists as `DT_NEEDED`.
`libvrapi.so` is Meta's VR runtime, not the platform SDK, and stays.

## Migration

Each step is its own PR, tests first, and the build stays shippable after each:

1. **The libr15 answers, behind the Create hook** (#1220). `CNSProvider::Create` (rename), `ProviderID` and `UserProviderID`
   (`OVR`), `CNSUser::LogIn` (our login). Tests: pinned-slot tests in `got_pinned_test.cpp` for the four slots; unit tests
   of the name rule (`pnsovr`, `pnsdemo` renamed, `pnsrad` and others untouched, null json); a login test that parses
   the json the builder makes and checks every member in the table above; a mutant per changed line.
2. **The facade selects on a null `Social()`** (#1221) instead of pnsovr's vtable pointer, as the PC facade does.
   `SelectSocialObject` (`social_install.cpp`) passes a null original through today and selects only an object whose
   vtable is `CNSOVRSocial`'s; its `SetPnsovrBias` lets the facade's `Initialize` read pnsovr's provider word
   (`kPnsovrProviderSymbolVaddr`), which becomes the `OVR` symbol the `ProviderID` slot answers. The RichPresence
   trace (#393) and the presence names built on it (#1185) lose pnsovr's object; what replaces them is decided in that
   PR, from what `SyncRichPresence` does with a null object.
3. **Retire what lived on pnsovr** (#1222), in the same PR as the headset run that shows step 1 logging in: the login hook and
   rewrite on libpnsovr (`login_hook.cpp`, the libpnsovr half of `login_rewrite.h`), the login prerequisites and their
   `ovr_*` stand-ins (#1183, `login_prerequisites*`, `login_standin.h`, the `prereq_*` counters), the entitlement-related
   stand-ins (#411), `game_login_failures.h`'s libpnsovr sites, `post_load.cpp`'s `libpnsovr_loaded` line (it becomes a
   check that the line never appears), and `tools/pinned_ovr_*`. In `tools/quest_patcher_sites.tsv` all 79 rows (8 from
   `libpnsovr.so.yaml`, 71 from `license_patch.yaml`) are covered by "not loaded (ADR 0026)", and the checker accepts
   that as their cover only while the repack recipe ships no `libpnsovr.so`.
4. **Stop shipping** (#1223) `libpnsovr.so` and `libovrplatformloader_orig.so`: the repack recipe drops both, the sentinel's
   added `DT_NEEDED` goes, and `tools/quest_repack_libs.py` and its tests follow.

## Prediction

On a headset, a build with steps 1 and 2, from cleared data (`pns_provider_create` and the `original` field are lines
this design adds; the names are proposals until the PR that adds them):

- one `pns_provider_create` line per platform `Create`, with `"asked":"pnsovr"`, `"loaded":"pnsrad"`,
  `"handle_nonzero":1`, and one with `"asked":"pnsrad"` passed through;
- no `libpnsovr_loaded` line, and no line from the login prerequisites or the libpnsovr login hook;
- no `Bad log in request: Unsupported provider` in the game's log;
- one `quest_login_send` line from the new path, then the game's own
  `NetGame switching state (from logging in, to logged in)`;
- `social_install` selected with `"original":"null"`;
- voice: `CR15NetVoipBroadcasterCS` records through libpnsrad (`MicCreate` succeeds; a listener hears the player).

A login that fails at `LogInSuccessCB` or in `LoginIdResponseCB` (the two checks PC NOPs) is a finding against
section 3's claim that setting the identity first makes them pass: it goes back to reading those two functions in
libr15, not to trying a variant.

## What this does not know

- The ~150 consumers of the `Users`, `IAP`, `RichPresence` and `Social` pointers that were not read. PC runs with the
  last three null; whether a Quest-only consumer dereferences one without a check is not shown.
- What libpnsrad's `InitGlobals` does when it runs a second time on Quest. PC does this on every run.
- Whether the server accepts a login without the Oculus `nonce` proof. The NEVR identity already replaces it on
  both platforms (ADR 0003); a headset run confirms it on Quest.
- The OpenSL ES microphone on a current Horizon OS: the permission it needs is requested by the game (#947); whether
  recording works is the voice check above.
- What libr15 does with the `graph.oculus.com` connection when `IAP` is null.
- What the game's `SyncRichPresence` (libr15 0x1260424) shows under a player's name when `RichPresence` is null. On PC
  it is null on every run.

## Consequences

The APK carries no library that calls Meta's platform SDK, so no store entitlement check runs and no hook of ours
depends on that SDK or a Meta binary; the community patcher's libpnsovr edits have nothing left to edit. The Quest's
provider state is PC's: the same provider, the same null `IAP`, `RichPresence` and `Social`, the same `OVR` provider
id and the same facade, with the login built by our code on both. Quest gives up Meta's microphone for the game's own
OpenSL ES one, and anything the Oculus platform gave that NEVR does not (Oculus friends and rooms, Oculus presence)
was already replaced by the facade.
