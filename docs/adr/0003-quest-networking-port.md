# ADR 0003: Quest networking shares the PCVR protocol core and differs only in adapters

Status: accepted. The login-profile builder and the redirect policy are shared today
(`src/runtime/compat/login_profile.{h,cpp}`, `src/runtime/lifecycle/service_redirect.{h,cpp}`).
The rest is not implemented; the work is tracked in #158 and the test regime is ADR 0004.

## Outcome

The Android/arm64 Quest client reaches the community service, completes config and login,
connects to matchmaking, enters a social lobby, and supports the Windows client's friends and
party behavior. There is one implementation of each NEVR protocol and social rule. Android
supplies its own loader, game ABI, hook installation, logging, configuration discovery and
socket adapter, and the original Oculus loader stays available to the game.

Scope is the client. The dedicated server, `src/legacy/` and production server deployment are
out of scope.

## What the Quest target is today

`src/quest/` builds a crash sentinel (`sentinel/entry.cpp`, `sentinel/sentinel.cpp`) and a GOT
import hook (`sentinel/got_hook.{h,cpp}`); it does not build the Windows runtime. The Android
preset (`src/quest/CMakePresets.json`) uses the Quest-local vcpkg manifest, the `arm64-android`
triplet and the NDK chainload at API 26. `nevr_quest_login_profile` compiles the shared login
profile but is not linked into the sentinel.

`sentinel::GotHook` (`sentinel/got_hook.{h,cpp}`) replaces the GOT slot a module uses for a
symbol it resolves at load time. It cannot hook an arbitrary internal function of `libr15.so`.
A target names one slot by module, symbol and relocation type (`R_AARCH64_JUMP_SLOT` or
`R_AARCH64_GLOB_DAT`), and optionally pins the build ID, the slot's link-time address and the
expected original value. `Install` refuses, logs one structured line and leaves the slot, its
page protection and the caller's original pointer unchanged when: the module is absent or its
build ID differs; zero or several relocations match; the relocation has an addend, a misaligned
slot or a slot outside a writable segment; a JUMP_SLOT module is not `BIND_NOW` (a lazily bound
slot starts as a lazy-binding stub, not the target; the pinned libraries are `BIND_NOW` and Bionic's
lazy behavior is unmeasured, so it is refused); the slot holds neither the expected original nor an
address in an executable mapping; or another handle owns the slot. `Remove` revalidates the
module and writes the original back only if the slot still holds the hook (compare-and-swap).
Every write runs under one process-wide lock, and the protection it restores is read from
`/proc/self/maps` under that lock (`PT_GNU_RELRO` is the fallback). Log lines are JSON objects
(`hook_log.h`). Order and rollback are the shared
`core/hook_lifecycle.h` contract that the MinHook path in `core/hooking.h` also uses.

`sentinel/callback_thunk.h` gives each hooked function a typed entry point, original-call
pointer and handler. The entry reads the original once per call. An exception thrown by the
original is rethrown unchanged; a `std::exception` thrown by a handler falls back to one call of
the original, never a second. libr15.so links `libc++_shared.so` and the sentinel links libc++
statically (`readelf -d` and `nm` on the built library; `tests/quest` `TestStlContract` pins it),
so the game's exceptions belong to a different C++ runtime than the thunk's catch clauses. The
thunk is written not to depend on matching them, and that behavior is inferred, not measured on a
device. The sentinel exports only `nevr_sentinel_marker` and `JNI_OnLoad`
(`TestExportAllowlist`), keeps no `thread_local` state (`TestNoEmulatedTLSInHookPath`), and
compiles the hook backend once into the `nevr_quest_got_hook` library that every Quest target
links.

`sentinel/pinned_targets.h` holds the targets and callback types for the pinned artifact:
`clock_gettime` (installed by `entry.cpp`), `CJson::TString` in both libraries, and the
`SNSConfigRequestv24Send` and `GLOB_DAT` slots as fixtures. Only `clock_gettime` is installed.
`SNSConfigRequestv24Send` has no thunk because its return type is not established.

## Architecture

```text
           shared source, compiled for both targets
 config key map/defaults | URL policy | login profile JSON
 EVR frame codec (later) | social state | EVR session routing
                         ^
                         | typed inputs and events, no game pointers
             +-----------+-----------+
             |                       |
       Windows adapters          Quest adapters
  Win32 config/log/identity   Android config/log/identity
  MinHook + PE addresses      ELF validation + hook backend
  WS transport + game ABI     WS transport + game ABI
```

A shared translation unit compiles under MinGW and the NDK without Windows headers, MinHook,
`EchoVR::g_GameBaseAddress`, `GetModuleFileNameA`, `ResolveModuleProc` or a game object layout.
`src/runtime/lifecycle/service_map.cpp` and `src/core/nevr_config.cpp` are extraction
candidates; the latter still includes `core/logging.h` and yaml-cpp, so a cross compile has to
prove it. Windows and Quest adapters call the same protocol and state functions.

## Contracts

1. **Configuration.** A portable resolved value carries public endpoints and optional client
   auth inputs; platform adapters supply the environment and embedded defaults. The first
   Quest use is the shared URL policy at the config-string lookup below. It neither adds nor
   depends on a game `config.json`, and a test shows any such file is ignored. Quest file
   discovery and precedence need their own measured design before use.
2. **Identity and wire.** `login_profile.{h,cpp}` builds the login profile with
   `nlohmann::json` and is compiled for Windows and Android. Windows EVR frame assembly stays
   in `src/runtime/compat/ws_bridge.cpp` until a reviewed serializer and a server-parser round
   trip replace it; Quest does not copy that assembly. The platform numbering the bridge sends
   is the server's wire enum; Quest identity values need binary or API evidence.
3. **Session routing.** A pure state machine takes game-side and remote open/frame/close events
   and returns send, close and log actions. Connections have explicit identities so a
   reconnect never confuses a new login with matchmaking. The Windows topology is preserved:
   separate game-facing EVR sockets, with the matchmaker connection attached to the
   authenticated remote EVR login session. Queueing, once-per-session login injection,
   callback lifetime and close propagation are requirements. Adapters own sockets, threads and
   TLS. No token, password, full login frame or credential URL is logged.
4. **Game hooks.** The Android adapter records the ELF build ID or SHA-256, module and load
   bias, validates each instruction, string and relocation, then installs a typed callback.
   Callbacks use bounded copies, preserve object ownership and return semantics, never throw
   through the game ABI, and defer networking and file I/O out of loader constructors. An
   unknown binary, a failed validation or a partial install leaves the original call intact
   and emits one structured error.
5. **Social.** Portable roster, party and name rules in
   `src/runtime/compat/social_{roster,party,names}.*` are reused after dependency validation.
   The 75-slot Windows facade and `echovr.exe` offsets are not a Quest ABI: a Quest provider
   adapter maps verified Quest slots, objects and callbacks to the same events. `nevr_social`
   is declared only for implemented handlers.

## Config-string seam (the first hook)

PCVR detours `EchoVR::JsonValueAsString` (`CJson::String`, `echovr.exe` RVA `0x5fe290`) and
applies `RedirectServiceUrl` to the result (`src/runtime/lifecycle/config.cpp`). Quest calls the
matching `NRadEngine::CJson::TString(char const*, char const*, unsigned int) const`
(`_ZNK10NRadEngine5CJson7TStringEPKcS2_j`) through `R_AARCH64_JUMP_SLOT` relocations:

| Quest ELF | Defined function | PLT entry and JUMP_SLOT | Call sites |
| --- | --- | --- | --- |
| `libr15.so` | `0xfa2e7c` | PLT `0xf28e40`, slot `0x36ebe08` | `CR15NetGame::BeginLogIn` at `0x1291b10` and `0x1291b28` reads `login_host`, then `loginservice_host`; `Initialize` reads `configservice_host`. |
| `libpnsradmatchmaking.so` | `0x209484` | PLT `0x19c200`, slot `0x6b4768` | `CNSRadMatchmaking::ConnectMatchmaker` at `0x1b22b8` and `0x1b22d0` reads `matchmaker_host`, then `matchingservice_host`, with more host lookups in its match-type handling. |

Pinned artifact: `build/android-arm64/repack/r15_nevr-sentinel_signed.apk`, package
`com.readyatdawn.r15`, version 4987566, SHA-256
`4757d50c0e307281d2840243cdb69e1ead6bc7ca157d7c415d38f5195d3ab67f3`.

| Library | SHA-256 | Build ID |
| --- | --- | --- |
| `libr15.so` | `8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20` | `b243509c08ce677aeb95fa348016949b3fc45230` |
| `libpnsradmatchmaking.so` | `36236ab1df5783da57c064b0fbccc3a61c0e1d150c208022fbfc9cd6e5ed60ee` | `8c4fddc079eae65909530132a56c48da48b2708c` |
| `libpnsrad.so` | `d9995c877a6623d8e48879f80c5749f313a0eba8a00b35a936c0df60e6d23aa8` | `c58fb82e42d9ed6564744cfa10f05af74aedd0ee` |
| `libpnsovr.so` | `26e9a216a710d42a303346a4ca5b84037ff38250ea725dc7112b055fcacada79` | `ca47bb8d03e6f43c1825133bbb9c15f174705c51` |

These match the ReVault records. They identify a local repack, not an installed headset build.

Argument registers at the call sites: CJson object in `x0`, key in `x1`, fallback in `x2`, a
zero flag in `w3`. A missing key returns the fallback unchanged in `x0`; a found key returns the
pointer from `json_string_value`. Callers pass the returned `x0` straight into the next lookup
or `CUriContainer::Parse`, so both pointers are borrowed and a replacement must stay valid
across that second lookup and the URI parse. Replacement storage is process-lifetime
(tranche 1b of #158). The candidate callback type is
`const char* (*)(const CJsonOpaque*, const char*, const char*, uint32_t)`; it is checked against
the exact callers before it is final.

The relocation resolves the PLT stub `0xf28e40` to the defined `CJson::TString` at `0xfa2e7c`,
not to the unrelated function at `0x10a2e7c`. The `libr15.so` string locations `0x2bae9b8`,
`0x2baeca2` and `0x2baecf7` are unverified and must not be patched from this record.
ReVault's `source: imported` on a decompilation describes how it entered the warehouse; only the
ELF's own symbol and relocation tables establish a GOT hook.

`libr15.so` and `libpnsrad.so` do not list `libpnsradmatchmaking.so` in `DT_NEEDED`.
`CNSLobby::LoadMatchmakingSupport` (`libpnsrad.so` `0x3c096c`) builds a module name and calls
`CModuleLoader::Load` at `0x3c0a3c`; the name it selects and the load time are not established.
The sentinel constructor therefore cannot assume the matchmaking module is loaded or install
its slot. Its slot stays inactive until a post-load install is validated.

`GotHook` reaches this seam by symbol name after the owning module is loaded, so there is no
need to detour `CNSRadMatchmaking::ConnectMatchmaker` (`libpnsradmatchmaking.so` `0x1b2274`);
its calling convention is unknown and it is not a hook site. This covers config-string reads
only. It does not establish a Quest HTTP connect hook or every URL source.

## Login interception

`CNSOVRUser::SendLogInRequest(CJson&)` (`libpnsovr.so` `0x1ec584`) tail-calls
`CNSUser::SendLogInRequest(CJson&)` through a PLT stub; the BIND_NOW `R_AARCH64_JUMP_SLOT` at
GOT `0x6dd1b8` names `_ZN10NRadEngine7CNSUser16SendLogInRequestERNS_5CJsonE`, so `GotHook`
takes it (`src/quest/login/login_hook.cpp`). At that call `x0` is the `CNSOVRUser` and `x1` the
Oculus login CJson; nothing is serialized yet. `SNSLogInRequestv2::Send` (`libr15.so`
`0x1932a08`, `libpnsovr.so` `0x382c1c`) then writes `SNSLoginId` (16 bytes), `SNSUserID`
(16 bytes) and the compact JSON in one `CTcpBroadcaster::Send`.

| Wire field | Source on Quest | What the rewrite does |
| --- | --- | --- |
| JSON | the CJson argument | replaces the login fields with the shared `LoginProfile` set, all-or-nothing |
| platform | `[CNSUser+0x90] & 0xf`; the `CNSOVRUser` constructor (`0x1edd68`-`0x1edd74`) stores 4 (OVR_ORG) | checks it is 4 |
| account id | `this->AccountID()` by virtual call (`vtable+0x70`, `0x382b90`/`0x382b9c`); `CNSOVRUser` overrides it (vtable slot `0x6a1300`) with `0x1ede14`: `adrp x8,0x70e000; ldr x0,[x8,#0x3e0]; ret` | writes that global (`0x70e3e0`, filled by `GotLoggedInUserOrgIdCb` from `ovr_OrgScopedID_GetID`) after checking the three instructions, then calls the same virtual to prove the wire value |

Which login members the rewrite replaces. The server reads the login JSON as a client
description plus an identity (nakama `server/evr/login_request.go`, `LoginProfile`):

| Members | Source on Quest | Reason |
| --- | --- | --- |
| `accountid`, `access_token`, `nonce`, `displayname`, `bypassauth`, `desiredclientprofileversion`, `hmdserialnumber`, `nevr_identity`, `nevr_social` | NEVR identity and the shared `LoginProfile` | identity; the Oculus token and proof nonce are replaced, not relayed |
| `buildversion`, `appid`, `lobbyversion`, `publisher_lock` | the game's own value, never written, never invented | they classify the client: `SessionParameters.IsPCVR()` is `BuildNumber != StandaloneBuildNumber (630783)`, `evr_lobby_joinentrant.go` sets `UseQuestFlags` only when `!IsPCVR()` (a different encoder flag layout), and `evr_discord_integrator.go` maps `appid` to a platform. The shared builder's `buildversion` 631547 would make a Quest a PCVR client. The Quest sends 630783 itself (`libpnsovr.so` `0x1ed938`-`0x1ed944`: `mov w2,#0x9fff; movk w2,#0x9,lsl #16`) |
| `system_info\|*` | the game's measurement; only members it left out are added | real headset values (CPU, cores, memory, network type, OS build) instead of the PCVR builder's empty placeholders |

The CJson type rules the rollback relies on (libr15 `SetString` `0xfa3444`, `SetInt`
`0xfa5edc`; `libpnsovr.so` carries the same code): `SetString` writes over an absent, string
or null path and refuses any other type; `SetInt` writes over an absent, integer or null path
and refuses any other type (a real is refused); a refusal is reported only in the game's log.
`TypeOf` returns 0 for both a null value and an absent path (`Valid` separates them), and maps
1 string, 2 int, 3 real, 4 boolean, 5 array, 6 object (`libpnsovr.so` table `0x582a80`).

The account-id global holds the NEVR id from the rewrite until the next login that is not
rewritten. `CNSUser::LogInSuccessCB` (`libpnsovr.so` `0x383a60`-`0x383a8c`) builds
`{[this+0x90], AccountID()}` and compares it with the server's reply; a mismatch drops the
success, so it cannot be restored after a successful send. `CNSOVRUser::LogInInternal`
re-reads the Oculus org id only when the global holds -1 (`0x1ec96c`-`0x1ec984`), so after a
rewritten login the NEVR id stays in the global for later logins in the process. The rule:
the Oculus value is remembered once, before the first write (`OculusIdMemory`), and put back
on every outcome other than `Rewritten`, because the declined login goes out with the Oculus
token and must carry the Oculus account id; a login after a rewritten one is rewritten again
from the current identity.

Readers of the global and of `AccountID()` in the pinned build (measured unless marked):

| Reader | Effect |
| --- | --- |
| `LogInInternal` `0x1ec96c` | re-fetches only when -1 (above) |
| `UpdateInternal` `0x1eda08`, `0x1edba4` | -1 leads to `LogInFailed` 500 ("prerequisites are missing", string `0x556b40`); zero waits |
| `GotLoggedInUserOrgIdCb` `0x1ecef0` / `0x1ecf18` | writes -1 on its error path, the org id on success |
| `RadPluginShutdown` `0x207074` | writes 0 |
| `CNSOVRUser::AccountID()` `0x1ede14` | returns it (vtable slot `0x6a1300`) |
| `CNSUser::SendLogInRequest`, `LogInSuccessCB`, `LogInFailureCB`, `LogOut`, `RefreshProfile`, `Profile*CB`, `LoginRemovedCB`, `UniqueName`, `SaveClientProfileChanges`, `CNSIUsers::CreateUser`, `User(UserAccountID)`, `DestroyUserInternal` | call `AccountID()` through `vtable+0x70` |
| `CNSLobby::JoinAcceptedCBClient`, `AddEntrantAcceptedCBClient` (`0x3720c0`) | find the local user by `AccountID()` equal to the entrant id the server sent: the id the server uses is required here |
| `CNSUser::UserID()` | about 60 call sites in `libr15.so` (lobby find, join and create, party, friends, profile, IAP, XPlatformId) |
| `CNSOVRSocial::FollowDeepLink` `0x1f2b30`, `EnsureLocalMember` `0x1f2bd0`, `JoinInternal` `0x1f38a4`, `AddMember` `0x204a30` | copy it into `[this+0x2e0][0]`, the local party member; `MemberId` (`0x205260`) and `Host` (`0x2051fc`) hand that to the game, while remote members carry Oculus org ids (`GotRemoteOrgIdCB` `0x1f9090`) |
| `ovr_Room_KickUser`, `SyncRoom`, `ReceiveData` | use `[0x2c8]` (the app-scoped id), not the global: Oculus room calls are not affected (independent review; not re-traced here) |
| `CNSIParty::Update` (`0x369764`), `CNSIRichPresence::Update`, `CNSIFriends::Sent` | call `vtable+0x70` on their own object, not `AccountID()` |
| `libpnsrad.so` `CNSRADFriends`, `CNSRADParty` | use `CNSRADUser` (vtable `0x6f1e00`, `AccountID` = `[this+0x88]` at `0x3cd4c0`), not this global (independent review) |

Open: party, room and friends flows that read the global through `CNSOVRSocial` see the NEVR id
for the local member and Oculus org ids for remote members, two id spaces in one flow. The
lobby path needs the NEVR id; the Oculus-room path was not shown to break, and was not shown
to work. A separate social package owns this.

## Hook activation

`TryInstallLoginHook` has no caller yet and the only `IdentitySource` is the test fake; the
sentinel does not install it. The install point is `libr15.so`'s `dlopen` import:
`CSysModule::Load` (`0x2a9e16c`) calls `dlopen@plt` at `0x2a9e1ec` through the BIND_NOW
`R_AARCH64_JUMP_SLOT` at `0x36c6380` (the only `dlopen` reference in `libr15.so`; `readelf -rW`
lists one). A `GotHook` on that slot lets the sentinel run the `libpnsovr.so`-dependent installs
right after the real `dlopen` returns with the module mapped: the login hook, and the
matchmaking redirect once `libpnsradmatchmaking.so` is loaded. The `dlopen` handler calls the
original, and on a non-null handle calls `TryInstallLoginHook(source, build)`; a
`ModuleNotLoaded` result means another module was opened and the install is retried on the
next `dlopen`. The handler must not throw and must not block. The install needs an
`IdentitySource` backed by token auth: until it answers `Ok` the Oculus login is left
unchanged. `SendLogInRequest` is reached only after the Oculus org-id fetch and
`ovr_User_GetUserProof` succeed (`0x1edca0`, `0x1ece10`); if the Oculus services do not answer
for this app the hook never fires.

CJson behaviour for a nested write: `FUN_00faef34` (`libr15.so` `0xfaef34`) walks the
`|`-separated path and, when a parent exists and is not an object, logs `$ json path: %s is not
an object.` and returns without writing. The rewrite does not attempt such a write.

`[CNSUser+0x88]` is not the wire account id for a `CNSOVRUser`. A virtual slot is a data
relocation, which `GotHook` does not reach, so the global is written instead.

`libpnsovr.so` has no `DT_NEEDED` on `libr15.so` and defines its own `NRadEngine::CJson`
(`SetString` `0x35917c`, `SetInt` `0x35bc14`, `SetBoolean` `0x358ccc`, `Clear` `0x358098`,
`TString` `0x358bb4`, `Int` `0x359e24`, `Boolean` `0x35a0a8`, `IsObject` `0x359a6c`). The login
CJson is built by `libpnsovr.so`, so the rewrite edits it with those exports resolved from the
`libpnsovr.so` handle, never with `libr15.so`'s.

The game logs the outgoing login: `Send` copies the CJson, clears `access_token`, `nonce`,
`authticket`, `authcode`, `authtoken` and `userhash`, and logs the copy at level 2
(`[LOGIN] Logging in %s: %s`, string `0x3173c79`). Any other member is logged in clear, so
the login JSON carries no credential: the server authenticates the session from the WebSocket
upgrade (`session_ws.go` reads `password` from the URL query; `LoginProfile` in
`server/evr/login_request.go` has no `password` member). The NEVR token travels in
`access_token`, which the game's own logger clears.

## Endpoint and authentication

The client uses one EVR frame protocol; Quest and PCVR run the same game networking code and the
service does not select a Quest variant. Nakama chooses a format per WebSocket session, and its
EVR branch dispatches lobby find/create/join and player-session requests, so the Quest client
needs no protobuf matchmaking frame and no mixed-format session. A public endpoint URI may name
`/nevr`; a password or account JWT is never embedded.

| EVR client route | Upgrade URL and header | Behavior |
| --- | --- | --- |
| Token-auth account | `/nevr?format=evr` with `Authorization: Bearer <Nakama account JWT>`, no `discordid/password` query | The `/nevr` ingress forwards the bearer; Nakama validates the JWT and links the login XPID to the account. The production `/ws` catch-all replaces Authorization with its server key, so the JWT is not sent there (issue #52). |
| Configured legacy account | `/nevr?format=evr&discordid=<percent-encoded id>&password=<percent-encoded password>` with `Authorization: Bearer <public socket server key>` | The ingress admits the upgrade with the server key and Nakama authenticates from the URL credentials. `ServerDbUri::BuildBridgeCredentialUri` percent-encodes; a password is never concatenated into a URL. With URL credentials present, the bridge chooses the server key over a JWT. |

The Quest adapter selects the route from the auth material it actually has and logs only route
names. Missing JWT and missing configured credentials are a login failure, not permission to
fabricate an identity. `LoginProfile.access_token` is a profile field, not upgrade
authentication. Credential-bearing query strings stay out of logs.

## Network behavior

1. Config and login sockets route to loopback while the remote target stays the community
   WebSocket URI, preserving path and query semantics from `service_map.cpp` and
   `ws_bridge.cpp`. HTTP asset and API requests do not pass through an EVR WebSocket listener.
   Config uses `SNSConfigRequestv2` / `SNSConfigSuccessv2`; login uses `SNSLogInRequestv2` and
   `SNSLogInSuccess` or failure.
2. One route from the table above is used. If the Quest identity supplies neither a valid JWT nor
   linked credentials, client auth stops there. Game configuration is not mutated, `/ws` with a
   JWT is not a fallback, and server authentication is unchanged.
3. One login request per login session; only a valid EVR `SNSLogInSuccess` completes it, and a
   failure status allows a retry. The Windows dedicated-server fake success path is not client
   authentication.
4. The `libpnsradmatchmaking.so` EVR socket attaches to the authenticated remote EVR login
   session, as the Windows matchmaker connection does, and `format=evr` stays on that remote
   connection. The matchmaker redirect needs a verified post-load install before its first dial,
   on a dynamically chosen local port; no fixed port or Windows string offset is assumed.
5. When the remote closes, its game sockets close. A new login gets a new session, and a
   matchmaker close does not destroy a live login callback.

The transport is an in-process loopback bridge for the game's existing EVR sockets. The
Windows bridge runs local config/login and matchmaking listeners and maps matchmaking onto the
remote login session; Nakama's `LobbySession` documents the secondary-session relationship. The
remote WebSocket and TLS library has to build and run under the NDK through the repository
dependency system.

## Binary and hook discovery gate

Pin fixtures to the artifact above; a different artifact needs a new identity check. For every
interception, collect ReVault decompilation, callers, callees and xrefs, then independently
check `readelf -r/-Ws`, section mapping, bytes or disassembly and load bias. Record the calling
convention, argument ownership, lifetime, call frequency and failure return.

| Control | Proof to seek | If absent |
| --- | --- | --- |
| Config/login URL before dial | Imported URL or connect boundary, or a verified URL builder or config accessor | A validated internal hook; never rewrite unrelated sockets by hostname. |
| Login frame before encryption | Imported send boundary with an identifiable EVR frame, or a verified `CNSUser` send path | A validated internal hook; never assume an Oculus token authenticates with Nakama. |
| Matchmaker endpoint and frame type | Verified matchmaking config reader or URI builder, or an imported connect boundary | A validated internal hook, or a separately byte-validated fixed string with size and xref proof. |
| Social provider and callbacks | Quest provider vtable and runtime object evidence | Social stays disabled; Windows offsets never cross the ISA. |

`GotHook` is suitable only for a module with a named `R_AARCH64_JUMP_SLOT` or `R_AARCH64_GLOB_DAT`
relocation, a build-ID-pinned ELF and a confirmed signature. An internal hook needs a backend that validates
the exact prologue, relocates PC-relative instructions, handles branch range, builds an
original-call trampoline, restores page permissions and instruction-cache coherence, and works
under Android's security policy. No blind branch at a cached address. A third-party backend is
integrated through the dependency system and verified on the real binary. A failed install
leaves no partial patch. Hook frequency is measured before any blocking call is added.

## Consequences

- Both targets compile the same portable sources; a Quest copy of URL policy, login
  transformation, EVR framing or session behavior is rejected by test (ADR 0004).
- Hook activation stays off until the gates in #158 pass. The open gates are the CJson ABI and
  hook lifecycle, exact artifact identity, the Quest client identity and auth source, a reviewed
  binary serialization library with a server-parser round trip, an NDK transport dependency
  with certificate validation, and Bionic behavior. An unresolved fact is never filled from
  Windows offsets or method names.
- `just build-android` needs `VCPKG_ROOT` because the Quest preset chainloads the vcpkg
  toolchain.
