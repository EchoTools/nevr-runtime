# Quest networking port: shared protocol core and Android adapters

Date: 2026-10-06
Status: historical architecture; tranche 1 completed in `4b11c98`; subsequent work follows `2026-10-06-quest-networking-implementation-plan.md` after review

## Outcome

The Android/arm64 Quest client should reach the community service, finish config and login, connect to matchmaking, enter a social lobby, and support the Windows client's friends and party behavior. There will be one implementation of each NEVR protocol and social rule. Android provides its own loader, game ABI, hook installation, logging, configuration discovery, and socket adapter. The original Oculus loader remains available to the game.

The scope is the client. The dedicated server, `src/legacy/`, and production server deployment are outside implementation scope. This architecture document does not authorize game runs, device installs or production-service checks; the offline implementation and acceptance gates are in the companion implementation plan.

## Evidence and corrections to the first draft

| Observation | Evidence | Consequence |
| --- | --- | --- |
| The Quest target builds a crash sentinel and a GOT import hook, not the Windows runtime. | `src/quest/CMakeLists.txt:1-37`, `src/quest/sentinel/CMakeLists.txt:1-30` | Extend this target and existing loader path. |
| `HookImport` finds a relocation for an **imported symbol**. Its header explicitly excludes arbitrary internal `libr15.so` functions. | `src/quest/sentinel/got_hook.h:10-19`, `got_hook.cpp:49-106` | The proposed `CNSUser::SendLogInRequest` and `CNSRadMatchmaking::ConnectMatchmaker` addresses cannot be passed to this API. The CJson `TString` JUMP_SLOTs below supply a measured config-string seam; hook lifecycle and ABI proof remain gates. |
| The September GOT experiment intercepted `clock_gettime`; its on-device verification was still pending in that record. | `docs/design/2026-09-15-quest-got-hook-basics-verification.md:37-62,93-116` | The probe is not proof of a login or matchmaker hook on a headset. |
| `service_map.cpp` is pure; `service_config.cpp` uses Win32 paths and game ABI. `ws_bridge.cpp` mixes protocol rules with Windows boot, auth and socket state. | `src/runtime/lifecycle/service_map.h:1-14`, `service_config.cpp:17-31,39-73`, `src/runtime/compat/ws_bridge.cpp:1-42,117-160` | Share pure sources and extract protocol/session rules. Do not link the whole Windows `compat/` or `server/` directory into Android unchanged. |
| Quest and PCVR run the same game networking code; the game service does not select a distinct Quest protocol. The Windows bridge attaches the matchmaker game connection to the EVR login remote; its later `format=evr` removal is in the **new-remote** branch and does not run for that shared connection. | Owner clarification 2026-10-06; `src/runtime/compat/ws_bridge.cpp:914-927,977-1032,1390-1410` | Preserve the working game-facing EVR flow on Quest. The old comment at `ws_bridge.cpp:1014-1026` describes a different, unreachable branch in this path and is not a basis for a Quest protobuf change. |
| Nakama chooses a format per WebSocket session; the EVR branch validates the EVR marker and dispatches EVR messages, including lobby find/create/join and player-session requests. Protobuf is a separate server-facing/future path. | `../nakama/server/socket_ws.go:46-58`, `session_ws.go:449-457,505-513`, `evr_pipeline.go:598-610`; owner clarification 2026-10-06 | No mixed-format session or protobuf matchmaking frame is required from the Quest client. |
| Nakama authenticates the WebSocket with a bearer token or `discordid` plus `password` URL parameters, then validates login XPID/account. | `../nakama/server/session_ws.go:173-182,246-284`, `../nakama/server/evr_pipeline_login.go:357-475` | The earlier tokenless-login claim is unsupported. Do not strip credentials or invent an identity. |
| Windows `BuildLoginRequest` now calls the shared `nlohmann::json` profile builder, while its EVR frame assembly remains in the bridge. | `src/runtime/compat/ws_bridge.cpp:696-840`; `src/runtime/compat/login_profile.cpp`; commit `4b11c98` | Preserve this shared source and parsed-output tests. Quest hardware facts must be measured or omitted. |
| The Windows matchmaker fallback patches a validated **Windows DLL string** and uses a second local listener. | `src/runtime/patch/pnsrad_enabler.cpp:97-130,231-286`; `src/runtime/compat/ws_bridge.cpp:1581-1634` | Its RVA, bytes and slot size have no authority on `libpnsradmatchmaking.so`. |

The PCVR and Quest builds have the same logical configuration-string hook point. PCVR detours `EchoVR::JsonValueAsString` (`CJson::String`, `echovr.exe` RVA `0x5fe290`) and runs the result through `RedirectServiceUrl` in `src/runtime/lifecycle/config.cpp:443-503`. Quest calls the corresponding `NRadEngine::CJson::TString(char const*, char const*, unsigned int) const` method through `R_AARCH64_JUMP_SLOT` relocations:

| Quest ELF | Defined function | PLT entry and JUMP_SLOT | Relevant callsites |
| --- | --- | --- | --- |
| `libr15.so` | `0xfa2e7c`, `_ZNK10NRadEngine5CJson7TStringEPKcS2_j` | PLT `0xf28e40`; slot `0x36ebe08` | `CR15NetGame::BeginLogIn` at `0x1291b10` and `0x1291b28` reads `login_host` and `loginservice_host`; `Initialize` reads `configservice_host`. |
| `libpnsradmatchmaking.so` | `0x209484`, same symbol | PLT `0x19c200`; slot `0x6b4768` | `CNSRadMatchmaking::ConnectMatchmaker` at `0x1b22b8` and `0x1b22d0`, with further host lookups in its match-type handling. |

The Quest APK artifact inspected for this mapping is the existing repack `build/android-arm64/repack/r15_nevr-sentinel_signed.apk`, package `com.readyatdawn.r15`, version code/name `4987566`, SHA-256 `4757d50c0e307281d2840243cdb69e1ead6bc7ca157d7c415d38f5195d3ab67f3`. Its `libr15.so` SHA-256/build ID are `8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20` / `b243509c08ce677aeb95fa348016949b3fc45230`; `libpnsradmatchmaking.so` are `36236ab1df5783da57c064b0fbccc3a61c0e1d150c208022fbfc9cd6e5ed60ee` / `8c4fddc079eae65909530132a56c48da48b2708c`. Those hashes match ReVault's indexed binaries. This identifies the existing repack artifact; it does not claim the artifact is currently installed on a headset.

The callsite disassembly and each ELF's symbol/relocation tables independently confirm the same method and PLT path. `HookImport` can reach this boundary by symbol name **after its owning module is loaded**; there is no need to detour `CNSUser::SendLogInRequest` or `CNSRadMatchmaking::ConnectMatchmaker` merely to rewrite config-string results. The pinned `libr15.so` and `libpnsrad.so` do not list `libpnsradmatchmaking.so` in `DT_NEEDED`; `libpnsrad.so`'s `CNSLobby::LoadMatchmakingSupport` at `0x3c096c` calls a module loader, but the selected module name and load timing remain a gate. The sentinel constructor cannot assume the matchmaking module is loaded. Reuse `service_map`/redirect policy at this boundary. Keep the hook contract and callback ABI exactness under the test-regime gate. This finding covers config-string reads only; it does not establish the Quest HTTP connect hook or every URL source.

ReVault is the source of truth for binary facts. Its current indexed function records identify `libr15.so:0x1932838` as `CNSUser::SendLogInRequest` and `libpnsradmatchmaking.so:0x1b2274` as `CNSRadMatchmaking::ConnectMatchmaker`; both raw decompilations report unknown calling conventions. Neither is the service-URL hook site selected above. The earlier timeout report is obsolete. The earlier decompiler label `thunk_FUN_010a2e7c` at PLT stub `0xf28e40` is resolved by the exact ELF relocation: it calls the defined `CJson::TString` symbol at `0xfa2e7c`, not the unrelated function at `0x10a2e7c`. The proposed `libr15.so` string locations `0x2bae9b8`, `0x2baeca2` and `0x2baecf7` remain unverified and must not be patched from this design.

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

A shared translation unit must compile under MinGW and the NDK without platform-specific Windows headers, MinHook, `EchoVR::g_GameBaseAddress`, `GetModuleFileNameA`, `ResolveModuleProc` or a game object layout. Existing `src/runtime/lifecycle/service_map.cpp` and `src/core/nevr_config.cpp` are extraction candidates; the latter still includes `core/logging.h` and yaml-cpp (`src/core/nevr_config.cpp:18-36`), so cross compilation must be proven. Windows and Quest adapters can differ internally but call the same protocol and state functions.

### Contracts

1. **Configuration:** a portable resolved value should carry public endpoints and optional client auth inputs, with platform adapters supplying environment and embedded defaults. The next tranche uses the existing shared URL policy at the measured CJson lookup seam. It does not add or depend on a game `config.json`; a regression test must show that any such file is ignored by this port. Quest file discovery and precedence require their own measured design before use.
2. **Identity and wire:** `src/runtime/compat/login_profile.{h,cpp}` and its `nlohmann::json` builder were completed in `4b11c98` and are compiled for Windows and Android (`src/runtime/CMakeLists.txt:33,588`; `src/quest/CMakeLists.txt:30-35`). The Windows EVR frame assembly remains in `ws_bridge.cpp` until a separately reviewed serializer and server-parser round trip replace it; Quest must not copy that assembly. Preserve `test_behavioral.cpp:833-910`. The Windows game-internal platform nibble differs from the server wire enum (`docs/reference/xpid.md:17-65`); Quest identity values still need binary/API evidence.
3. **Session routing (after the codec tranche):** a pure state machine accepts game-side and remote open/frame/close events, returning send/close/log actions. Connections have explicit identities so reconnect does not confuse a new login with matchmaking. Preserve the established Windows topology: separate game-facing EVR sockets, with the matchmaker connection attached to the authenticated remote EVR login session. Queueing, once-per-session login injection, callback lifetime and closure propagation are requirements. Adapters own sockets, threads and TLS. No token, password, full login frame or unredacted credential URL is logged.
4. **Game hooks:** the Android adapter records ELF build ID or SHA-256, module and load bias, validates each instruction/string/relocation, then installs a typed callback. Callbacks use bounded copies, preserve object ownership and return semantics, never throw through the game ABI and defer networking/file I/O out of loader constructors. Unknown binary, failed validation or partial installation leaves the original call intact and emits one structured error.
5. **Social:** reuse portable roster/party/name rules from `src/runtime/compat/social_{roster,party,names}.*` after dependency validation. The 75-slot Windows facade and `echovr.exe` offsets are not a Quest ABI (`src/runtime/patch/social_facade.cpp:1-79`; `docs/design/2026-10-01-social-features-test-plan.md:13-25`). A Quest provider adapter maps verified Quest slots, objects and callbacks to the same events. Declare `nevr_social` only for implemented handlers (`src/runtime/compat/social_party.h:25-42`).

The transport design is an **in-process loopback bridge** for the game's existing EVR sockets. The Windows implementation has local config/login and matchmaking listeners, and maps matchmaking to the remote authenticated EVR login session (`src/runtime/compat/ws_bridge.cpp:223-255,914-927,1581-1634`). Nakama dispatches lobby requests in its EVR pipeline (`../nakama/server/evr_pipeline.go:598-610`); `LobbySession` also documents the game's secondary-session relationship (`../nakama/server/evr_session.go:17-35`). Existing console capture shows `conn=2` sharing the login session followed by lobby success in `/var/tmp/work-nevr-runtime/client-run-20261002T000607/console.log:464,485` (secondary evidence). The Quest port retains that EVR flow in offline tests; this does not assert runtime success. The remote WebSocket/TLS library must build and run under the NDK through the repository dependency system.

Android dependency wiring was completed in `4b11c98`: `src/quest/CMakePresets.json:7-17` selects the vcpkg toolchain, Quest-local manifest, `arm64-android` target and NDK chainload; `src/quest/vcpkg.json` currently contains `nlohmann-json`. Later dependencies enter that Quest manifest only when selected and verified.

## Binary and hook discovery gate

The inspected local repack is pinned above by APK hash and relevant ELF build IDs. Pin fixtures to this artifact before hook work; a different artifact needs a new identity check. No device installation or installed-build assumption is part of the routine tranches.

For every interception, collect ReVault decompilation, callers/callees and xrefs, then independently check real ELF `readelf -r/-Ws`, section mapping, bytes/disassembly and load bias. Record calling convention, argument ownership, lifetime, call frequency and failure return. The needed controls are:

| Control | Proof to seek | If absent |
| --- | --- | --- |
| Config/login URL before dial | Imported URL/connect boundary, or verified URL builder/config accessor | Use a validated internal hook; do not rewrite unrelated sockets by hostname. |
| Login frame before encryption | Imported send boundary with identifiable EVR frame, or verified `CNSUser` send path | Use a validated internal hook; do not assume an Oculus token authenticates with Nakama. |
| Matchmaker endpoint and frame type | Verified matchmaking config reader/URI builder or imported connect boundary | Use a validated internal hook or separately byte-validated fixed string with size and xref proof. |
| Social provider and callbacks | Quest provider vtable and runtime object evidence | Leave social disabled; never transfer Windows offsets across ISA. |

`HookImport` is suitable only if the module has a named `R_AARCH64_JUMP_SLOT`, `GLOB_DAT` or `ABS64` relocation and the signature is confirmed. Internal hooks need a backend that validates the exact prologue, relocates PC-relative instructions, handles branch range, creates an original-call trampoline, restores page permissions and instruction-cache coherence, and works under the installed Android security policy. Do not write a blind branch at a cached address. Any third-party backend must be integrated through the supported dependency/build process and verified on the actual binary. A failed install must not leave a partial patch. Measure per-frame/per-tick hook frequency before adding any blocking call.

## Endpoint and authentication contract

The client uses one EVR frame protocol, with endpoint and authentication chosen deliberately. The ingress behavior below is from `src/runtime/compat/ws_bridge.cpp:670-687,985-1009,1085-1108`, `src/runtime/tests/test_behavioral.cpp:923-947`, `../nakama/server/socket_ws.go:46-104` and `../nakama/server/session_ws.go:173-182,230-284`. Public endpoints and secrets are distinct: an embedded public URI may name `/nevr`, but a password or account JWT is never embedded.

| EVR client route | Upgrade URL and header | Required behavior |
| --- | --- | --- |
| Token-auth account | `/nevr?format=evr` with `Authorization: Bearer <Nakama account JWT>`; no `discordid/password` query | The `/nevr` ingress forwards this bearer. Nakama validates the JWT and links the EVR login XPID to that account. Sending this JWT to the production `/ws` catch-all is invalid for this route because that front replaces Authorization with its server key (issue #52, as recorded in the bridge). |
| Configured legacy account | `/nevr?format=evr&discordid=<percent-encoded id>&password=<percent-encoded password>` with `Authorization: Bearer <public socket server key>` | The ingress admits the upgrade with the server key, then Nakama authenticates the account from URL credentials. `ServerDbUri::BuildBridgeCredentialUri` supplies percent encoding; do not concatenate a password into a URL. When URL credentials are present, the bridge deliberately chooses the server key over a JWT. |
| Local direct Nakama test | `/ws?format=evr&token=<local server key>` plus seeded `discordid/password` | This is the documented offline rig (`docs/reference/local-nakama.md:51-68`), where the proxy's `/nevr` behavior is absent. It is a test configuration, not the production token-auth route. |

The Quest adapter must select the route from actual available auth material and log only route names. Missing account JWT and missing configured ID/password are a login failure, not permission to fabricate identity. Keep URL credentials off diagnostic logs; query strings may contain a password. `LoginProfile.access_token` is a profile field, not a substitute for the WebSocket upgrade authentication.

## Network behavior

1. Route game config/login sockets to loopback while the remote target remains the community WebSocket URI. Preserve URL path/query semantics from `service_map.cpp` and `ws_bridge.cpp`. HTTP asset/API requests do not go through an EVR WebSocket listener. Config expects `SNSConfigRequestv2`/`SNSConfigSuccessv2`; login expects `SNSLogInRequestv2`/`SNSLogInSuccess` or failure (`.claude/memory/ws-bridge-login-connection-model.md:9-30`).
2. Use exactly one route from the endpoint/authentication table. If Quest identity supplies neither a valid account JWT nor linked ID/password, stop that client-auth tranche pending a separately reviewed source of supported material. Do not mutate game configuration, fall back to `/ws` JWT, or change server authentication.
3. Send one login request per login session and accept only a valid EVR `SNSLogInSuccess` response in the offline client flow. The Windows dedicated-server fake success path is not client authentication (`src/runtime/compat/ws_bridge.cpp:1220-1260`). Retain failure status and allow retry.
4. Attach the game's `libpnsradmatchmaking.so` **EVR** socket to the authenticated remote EVR login session, as the working Windows `conn>=2` branch does (`src/runtime/compat/ws_bridge.cpp:914-927`). Keep `format=evr` on that remote connection. The `format=evr` removal at `ws_bridge.cpp:1014-1028` is in the new-remote branch and is not part of this route. A Quest matchmaking redirect needs a verified post-load install before its first dial; module selection/timing is still unresolved. Choose the local port dynamically; no fixed port or Windows string offset is assumed.
5. On remote close, close associated game sockets; a new login gets a new session; matchmaker close must not destroy a live login callback. The relevant Windows fixes are described at `src/runtime/compat/ws_bridge.cpp:170-255,300-303,1520-1557`.

## Implementation and acceptance

The prior tranche table is historical: login-profile extraction and Android dependency wiring were completed in `4b11c98`. The ordered remaining work, beginning with the measured PCVR/Quest CJson callback seam and using offline automated gates, is specified in `docs/design/2026-10-06-quest-networking-implementation-plan.md`. `docs/design/2026-10-06-quest-testing-regime.md` defines exact-binary callback and shared EVR tests. No routine test needs a game process, APK installation, headset, local Nakama, production service or log. Broad social work and the still-unmapped Quest HTTP request hook are deferred. The portable sources must be compiled by both targets; no Quest wire fork is accepted.

The remaining gates are the CJson ABI and hook lifecycle, exact artifact identity, the Quest client identity/auth source, a reviewed binary serialization library and server-parser round trip, an NDK transport dependency with certificate validation, and Android Bionic behavior. Unresolved facts are not filled by Windows offsets or method names.

## Design review passes

**Sol self-review 1, architecture:** removed unsupported GOT-to-internal-method and unchanged-Windows-link assumptions; separated pure rules from platform adapters. Windows matchmaker session sharing is now explicit.

**Sol self-review 2, failure and test paths:** checked auth against current Nakama source, replaced tokenless login with explicit `/nevr` JWT and URL-credential routes, made exact APK identity and log correlation prerequisites, and added malformed-wire, reconnect, hook-rollback, no-config, redaction and linked-artifact checks. Astra review identified that the old Windows protobuf comment is in an unreachable branch for shared matchmaker connections; the server EVR dispatcher and a live lobby success show the supported path. The first tranche now extracts JSON only, preserving existing frame bytes; the binary codec has a separate library-choice gate.

**Astra rereview, 2026-10-06:** approved the now-completed tranche 1 as bounded. The common EVR route, `/nevr` auth matrix, JSON-only extraction, direct-source `test_behavioral` wiring and Quest-local dependency manifest resolved the review findings. Later work requires review of the new implementation plan.
