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

Token auth is shared the same way. The token model, refresh handling and device-code loop are
platform-neutral sources in `src/core/` (`auth_token_model.h`, `auth_refresh.{h,cpp}`,
`device_auth_flow.{h,cpp}`, `device_poll_response.{h,cpp}`) behind injected HTTP, clock and log
interfaces (`auth_types.h`); the Windows `token_auth` module and `src/quest/auth/` both compile
them. `src/quest/auth/` holds the Android adapters: libcurl over OpenSSL from the Quest vcpkg
manifest (`arm64-android` triplet) with peer and host verification on and
`/system/etc/security/cacerts` as the CA directory, an atomic mode-0600 credential file under
`/sdcard/Android/data/com.readyatdawn.r15/files/`, a login-link file there, and a `Session` whose
worker thread does the login so `Start()` never blocks the caller. `nevr_quest_token_auth` is not
linked into the sentinel, and nothing yet hands the token to the login path. The tests are
`src/quest/tests/auth_core_test.cpp`, run on the host by `just test-quest-shared`. Not
established: the CA directory and the libcurl/OpenSSL stack on a headset, and how the player is
shown the login link.

`HookImport` replaces the GOT slot a module uses for a symbol it imports. It cannot hook an
arbitrary internal function of `libr15.so`. It has no detach, no duplicate-install guard and
ignores the result of its last `mprotect` (tranche 1e of #158).

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

`HookImport` reaches this seam by symbol name after the owning module is loaded, so there is no
need to detour `CNSUser::SendLogInRequest` (`libr15.so` `0x1932838`) or
`CNSRadMatchmaking::ConnectMatchmaker` (`libpnsradmatchmaking.so` `0x1b2274`); both have unknown
calling conventions and neither is a hook site. This covers config-string reads only. It does
not establish a Quest HTTP connect hook or every URL source.

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

`HookImport` is suitable only for a module with a named `R_AARCH64_JUMP_SLOT`, `GLOB_DAT` or
`ABS64` relocation and a confirmed signature. An internal hook needs a backend that validates
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
