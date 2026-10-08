# ADR 0003: Quest networking shares the PCVR protocol core and differs only in adapters

Status: accepted. The login-profile builder and the redirect policy are shared today
(`src/runtime/compat/login_profile.{h,cpp}`, `src/runtime/lifecycle/service_redirect.{h,cpp}`).
The social facade is implemented and host-tested in `src/quest/social/` but not linked into the
sentinel. The rest is not implemented; the work is tracked in #158 and the test regime is ADR 0004.

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
pointer and handler; the entry reads the original once per call.

**No exception crosses the thunk.** Measured: libr15.so and libpnsovr.so NEED `libc++_shared.so`,
which is a libgcc-style unwinder build (`.comment`: GCC 4.9.x, clang 5.0) exporting
`_Unwind_Find_FDE`, `_Unwind_GetCFA`, `_Unwind_GetIP`, `_Unwind_RaiseException`,
`_Unwind_Resume`, `__gxx_personality_v0` and `__cxa_throw`. The sentinel does not NEED it; it
links LLVM libunwind and libc++abi statically, with `_Unwind_Resume`, `_Unwind_GetIP`,
`__unw_getcontext`, `__gxx_personality_v0`, `__cxa_throw` and `__cxa_begin_catch` as local
symbols, and its `.eh_frame` has a personality-bearing `zPLR` CIE and a personality-free `zR`
CIE. A sentinel frame with an LSDA would run the sentinel's personality on the game's unwind
context (inferred from the layouts, not run on a device); the reverse direction is the same. The
contract:

1. A frame that is live while game code runs under a hook must be personality-free (`zR` CIE):
   no try/catch, no object with a destructor. Live frames are the thunk's entry, the handler, and
   any sentinel function the handler calls that is still on the stack when it calls the original.
   A function that runs entirely before or after the call into the game is not live during it.
2. Translation units that include `callback_thunk.h` are built with `-fno-exceptions` (the header
   refuses otherwise), so the entry has no landing pad and a game exception passes through it on
   CFI alone.
3. A hook is a record: `NEVR_HOOK_RECORD(name, Thunk, handler)` emits `{entry, handler}` into the
   `nevr_hook_records` section and `Thunk::Arm` takes only a record. `HookRecord` has no public
   constructor, and `Arm` refuses (and logs) a record whose address is not inside that section, so a
   record built at run time cannot arm a handler the sensor never saw. Handlers are `noexcept`
   function pointers. Under `-fno-exceptions` `noexcept` is a type marker only; it adds no
   terminate landing pad. A callee of a handler that can throw must contain the exception inside
   sentinel-only frames that are not live across a call into game code.
4. A hook is installed only through `InstallThunk<Thunk>` (`hook_install.h`), which accepts only a
   `CallbackThunk` instantiation (a `static_assert`; a fake type with `EntryAddress()` does not
   compile); the raw `GotHook::Install` taking any function pointer is private and reachable only
   through the test access class. `just test-quest-hooks` compiles snippets that break each
   type-level rule and requires them to fail with the message that names the rule;
   `TestRawInstallOnlyInTests` reads every `.cpp`, `.cc`, `.cxx`, `.h`, `.hpp` and `.inc` file under
   `src/` and fails if a file outside a `tests/` directory names the test access class or includes
   (resolved relative to the including file, to `src/` and to `src/quest/sentinel`) anything under
   `src/quest/tests`, recursively.
5. A function a handler calls directly and that can be on the stack across the call into game
   code must be personality-free, and must not make an indirect call (function pointer, virtual,
   `std::function`) into code built with exceptions.
6. `tests/quest` `TestHookFramesCarryNoPersonality` checks the built library. It requires exactly
   one record per `CallbackThunk` entry (an entry without one fails), starts from each record's
   entry and handler, follows direct `bl`/`b` edges and fails on any reachable function under a
   personality-bearing CIE. There is no allowlist: a hook does not log, so no logging code is
   reachable from it. Its limits: it does not follow indirect calls (rule 5 is a rule there, not
   a check), and a hook installed some other way is invisible to it, which is what rule 4 and
   `TestRawInstallOnlyInTests` prevent. The checks catch honest mistakes by packages that use the
   API; a macro that forwards to the record access class, or a test-directory wrapper that
   production includes through a path the scan does not recognise, gets around them, and the
   `#error` in `callback_thunk.h` is advisory (`#undef __cpp_exceptions` defeats it) while the
   frame sensor on the built library is the real check. `TestBackendBuiltWithoutExceptions` pins the backend
   (`GotHook`, `ResolveSlot`, the logger and the reporter) to `zR`, matching the flag the host
   tests use, and `TestStlContract` fails if the sentinel starts linking `libc++_shared.so`.

A hook never logs on the game's call path (the log call is not async-signal-safe): it increments
an atomic counter, and a reporter thread (`hook_report.h`, created from the constructor before the
first hook is installed) logs "reporter_started", then a counter's first change within the first
10 seconds, then "never_fired" once for each counter still zero when that window closes (the hook
is installed and the game never called it), and from then on one pass a minute that logs a counter
only if it changed. The counter table holds 32 counters for the whole program, and every
`RegisterReportCounter` call must come before `StartReporter` (a later registration, or the 33rd, is
refused and logged as `register_refused`). The thread ends with the process; creating it from a constructor on a Quest is
inferred from the Bionic main-branch source and has not been tried on a headset. A slot where a
failed install left the sentinel's entry possibly reachable through another writer's hook stays
reserved for the process and a retry is refused with its own status, `slot_poisoned`;
`ReleasePoisonedSlotsIn` gives such a reservation back only for an address that
`/proc/self/maps` shows unmapped after one complete pass (a failed open, a read error or an empty
file reads as "unknown" and keeps the reservation; each outcome is logged), and nothing in the sentinel calls it because the game's libraries
are never unloaded. Log lines go to logcat, which does not meet the durable-log rule in
`AGENTS.md`; the planned sink is the sentinel's disk log.

Whether the declared hook targets can throw, from the pinned ELFs: `CJson::TString`
(`libr15.so` `0xfa2e7c`, `libpnsradmatchmaking.so` `0x209484`): ReVault's callee graph (partial:
146967 of 178574 functions) lists no throw or allocation entry, and a direct-call scan of the ELF
(22 functions to depth 7, no indirect call in the set) finds no PLT call to `__cxa_throw`,
`__cxa_allocate_exception`, `__cxa_rethrow`, `operator new`/`delete`, `malloc`/`calloc`/`realloc`
or `abort` in libr15's own code; its calls go to engine imports (`CMemoryContext`,
`CMemory::Fill`, `CFixedString::SPrintF`, `NWriteLog::WriteLog`, `json_string_value`) whose bodies
were not scanned. `CNSUser::SendLogInRequest` (`libpnsovr.so` `0x382a4c`, 464 bytes) calls
`CJson::SetInt`, `SNSLogInRequestv24Send`, `CTcpBroadcaster::ConnectionPeer` and
`STcpPeer::operator` through imports and makes one indirect call (`blr x9` at `0x382b9c`); it
cannot be shown non-throwing. Neither is established as exception-free, which is why the contract
does not depend on it.

The sentinel exports only `nevr_sentinel_marker` and `JNI_OnLoad` (`TestExportAllowlist`), has no
`thread_local` state of its own (`TestNoEmulatedTLSInHookPath`), and compiles the hook backend
once into `nevr_quest_got_hook`, which every Quest target links (`TestBackendCompiledOnce`).

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
   depends on a game `config.json`, and a test shows the Quest config path never names one.
   `src/quest/sentinel/quest_config.{h,cpp}` resolves each key from `nevr-quest.json` in
   `/sdcard/Android/data/com.readyatdawn.r15/files/`, else from the value embedded at build
   time (`cmake/nevr_builtin_defaults.cmake`, read from the environment or `.env` at configure
   time only), else absent. Keys: `nevr_socket_uri`, `nevr_http_uri`, `nevr_http_key`,
   `nevr_server_key`, plus `features` with boolean `redirect`, `bridge` and `login`. A feature
   is off unless the file turns it on, and is forced off while its prerequisite is missing
   (bridge needs redirect and a socket URI, login needs bridge and the server key). A malformed,
   non-object or oversized (64 KiB) file is rejected whole: embedded values, all features off.
   Every key source, requested and effective feature state, and rejection is logged by key or
   feature name, never by value, to logcat tag `NEVR-Sentinel` and to `nevr-sentinel.log` in the
   same directory, as one JSON object per line. A value the file gives as the empty string is
   rejected and cannot clear an embedded default; a key given twice in one object takes the
   last value and logs a warning. The sentinel constructor reads the file once (contract 4 states the constructor's I/O
   limits). Warnings about the file are capped at 32 lines plus one suppression line, and
   repeated duplicate keys collapse to one line per name. `sentinel_host_test` runs
   the constructor-then-main order, the non-regular-file paths and the log-failure paths on the
   host; the read has not been run on a headset.
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
   through the game ABI, and defer networking and file I/O out of loader constructors. The one
   exception is the sentinel constructor's startup record: it opens `nevr-quest.json` read-only
   and `nevr-sentinel.log` append-only in the app's external files directory, each opened
   non-blocking and required to be a regular file, with a 64 KiB read bound and a 1 MiB log
   rotation bound, and nothing else. No network, TLS or hook install happens in a constructor.
   The constructor cannot throw: the resolution catch block allocates nothing. The remaining
   risk is a stall in the storage layer of the headset (FUSE-backed external storage); it has not
   been measured on a device. An
   unknown binary, a failed validation or a partial install leaves the original call intact
   and emits one structured error.
5. **Social.** Portable roster, party and name rules in
   `src/runtime/compat/social_{roster,party,names}.*` are shared with the Windows facade. The 75-slot
   Windows facade and `echovr.exe` offsets are not a Quest ABI: `src/quest/social/` is a Quest
   provider adapter over the same models with the 76-slot Quest vtable (section "Social provider").
   `nevr_social` is declared only for implemented handlers.

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

`GotHook` reaches this seam by symbol name after the owning module is loaded, so there is no
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

`GotHook` is suitable only for a module with a named `R_AARCH64_JUMP_SLOT` or `R_AARCH64_GLOB_DAT`
relocation, a build-ID-pinned ELF and a confirmed signature. An internal hook needs a backend that validates
the exact prologue, relocates PC-relative instructions, handles branch range, builds an
original-call trampoline, restores page permissions and instruction-cache coherence, and works
under Android's security policy. No blind branch at a cached address. A third-party backend is
integrated through the dependency system and verified on the real binary. A failed install
leaves no partial patch. Hook frequency is measured before any blocking call is added.

## Social provider

The Quest game has two social providers and uses one of them for friends, parties and rooms.
Everything below was read from the pinned store build (`libr15.so` build id `b243509c...`,
`libpnsovr.so` `ca47bb8d...`, `libpnsrad.so` `c58fb82e...`) with `readelf`, `llvm-objdump` and ReVault.
Addresses are ELF vaddrs. ReVault's decompilation prints pointer immediates into `libpnsovr.so` data and
`FUN_` names 0x100000 higher than the ELF (the vtable it shows as `0x7a1240` is the ELF's `0x6a1240`);
the ELF values are used here.

### Which provider serves what

`CR15Game::Initialize` (`libr15.so` 0x11f9b40) calls `CNSProvider::Create` (0x192f3e4). That function
loads the module by name, then calls its exported `InitGlobals` with the game's `CTcpBroadcaster`
(`CNSProvider::TcpBroadcaster()` at 0x1915d04 returns the global at 0x379d3e8; the call is at 0x192f530).
The providers it creates:

| Handle | Module | Condition |
| --- | --- | --- |
| `CR15Game` +0x84f8 | `pnsovr` (rodata 0x2ba11f7) | bit 27 of the flags word at `CR15Game`+0x7ad8 is clear. The constructor stores 0x40000000. |
| `CR15Game` +0x8508 | `pnsdemo` (0x2ba1215) | bit 27 set. `PreprocessCommandLine` sets it at 0x11f7348 when the command line has `-noovr`. The APK has no `libpnsdemo.so`. |
| `CR15Game` +0x8500 | `pnsrad` (0x2ba1233) | always |

Nothing else selects between them: the function reads that flags word, the Android permission check and
string literals. There is no configuration key. The microphone provider is chosen separately, by a
symbol at +0x84f0 that defaults to OVR and becomes RAD when a `-moderator*` argument sets bit 20.

`CR15NetGame`'s constructor (0x1283db8) stores the handles at +0x10 (primary: pnsovr), +0x18
(microphone) and +0x20 (pnsrad). The calls into `CNSProvider` in `libr15.so`, by handle:

| Handle | Calls |
| --- | --- |
| +0x10 (pnsovr) | `Social` (1, at 0x12866a4), `Users`, `IAP`, `RichPresence` (1 each, 0x12860a4, 0x1286530, 0x1286670), `ProviderID` (9), `UserProviderID` (19) |
| +0x18 (microphone) | `Mic*` |
| +0x20 (pnsrad) | `Voip*` (encoder, decoder, packet size, sample rate) |

`CNSProvider::Friends`, `Party` and `Activities` have no call site in `libr15.so`: no PLT stub exists for
them and no direct branch targets them. Each provider module exports only the methods it implements,
and `CNSProvider::Social` looks the name up with `dlsym`:

- `libpnsovr.so`: `Users`, `IAP`, `RichPresence`, `Social`, `CheckEntitlement`, `Mic*`, `Voip*`.
- `libpnsrad.so`: `Users`, `Friends`, `Party`, `Activities`, `Mic*`, `Voip*`. It exports no `Social`.

So on Quest the social object (friends, party, rooms, invites, recently met) is pnsovr's `CNSOVRSocial`,
returned by `Social()` (the exported function at 0x2077d8 returns the global at 0x70e420). The game
stores it at `CR15NetGame` +0x647c8 and reaches the friend list, party and invites through its vtable.
pnsrad's `Users`/`Friends`/`Party` objects are constructed (`InitGlobals` at 0x20632c, sizes 0x428,
0x420, 0x430, 0x2c8, on the game's broadcaster) but nothing in the game asks for them.

### What the PCVR pnsrad-enabler does and why

`src/runtime/patch/pnsrad_enabler.cpp` makes `echovr.exe` load `pnsrad.dll` for every provider slot and
repairs what that breaks:

- It overwrites the `pnsovr` and `pnsdemo` strings in `echovr.exe` data with `pnsrad`, and NOPs the
  OVR branch in `PlatformModuleDecisionAndInitialize`, so the Oculus runtime is never required.
- It patches `pnsrad.dll`'s login-provider check and the identity and state guards in `LogInSuccessCB` and
  `LoginIdResponseCB` so the `LoginRequest` the bridge injects is accepted although pnsrad's own state
  machine never produced it.
- It makes pnsrad's exported `UserProviderID` return the OVR symbol, because the game compares a friend
  id's provider with it and every id the runtime builds is `OVR-ORG-<id>`.
- It re-points `pnsradmatchmaking.dll`'s compiled matchmaker host to the loopback listener.

pnsrad exports no `Social`, so on PCVR the social accessor (`echovr.exe` 0x1406169c0) returns null; the
runtime's social facade (`src/runtime/patch/social_facade*.cpp`) supplies a `CNSISocial` object whose
vtable mirrors `CNSOVRSocial`'s, and `ObserveSocialFrames` in `src/runtime/compat/ws_bridge.cpp` feeds
`SocialRoster` and `SocialParty` from the service's SNS messages. Every id in that path is a NEVR
account id (`SocialParty::MemberUuid` derives the party UUID from `OVR-ORG-<id>`). On PCVR the enabler is
what removes the Oculus provider; the facade is what provides the social object. Both are needed there.

### Id spaces on Quest

Traced in the pinned libraries (ELF vaddrs):

- **Local member, native value.** `SCallbacks::GotLoggedInUserOrgIdCb` (libpnsovr 0x1ece60) stores
  `ovr_OrgScopedID_GetID(ovr_Message_GetOrgScopedID(msg))`, the logged-in Oculus user's org-scoped id,
  at libpnsovr 0x70e3e0 (and its decimal text at 0x70e458); on an error reply it stores -1.
  `CNSOVRSocial::AddMember` (0x2049f4) copies the 8 bytes at 0x70e3e0 into `[this+0x2e0][0]`. So the
  local member is also an Oculus id unless the login rewrite overwrites that global before `AddMember`
  runs; whether it does is the login package's to establish, and the facade does not read the global.
- **Remote members.** `CNSOVRSocial::GotRemoteOrgIdCB` (0x1f8d58) calls `ovr_Message_GetOrgScopedID` and
  `ovr_OrgScopedID_GetID` and stores the result at `[[this+0x2e0] + index*8]` (0x1f91bc): an Oculus
  org-scoped id.
- **Readers.** `Host` (0x2051fc) returns `[this+0x2e0][owner index]` and `MemberId` (0x205260)
  returns `[this+0x2e0][i]`, both in x0, so a party mixes the two sources.
- **pnsrad's identity.** `CNSUser::AccountID()` (libpnsrad 0x3cd4c0) returns `[this+0x88]` when it is
  nonzero and otherwise `CSysUsers::GetAccountID` of the `LocalUserID` at `[this+0x98]`; `[this+0x90]`
  is the provider/flags word. In `0x3c9000..0x3cd600` the only store to `+0x88` is
  `CNSUser::SetGuest(UserAccountID)` (0x3cd530, `stp x1, x9, [x0, #0x88]`); the PCVR bridge writes the
  fields directly. `CNSRADUsers::CreateUserInternal` (0x1fd814) allocates 0xb0 bytes, runs the
  `CNSUser` constructor and `CNSRADUser`'s vtable (0x6f1e00 + 0x10) and sets flags |= 0x30.
  `CNSUser::SendLogInRequest` (0x3ca208) reads `[this+0x90]` (0x3ca344); the message layout beyond
  that is from the #221 review.
- The game compares a friend id's provider with `CNSProvider::UserProviderID(primary)` (`FriendId`,
  libr15 0x129b6f8): the symbol is compared in turn with seven CSymbol64 constants in libr15's rodata and the match
  picks the platform code of the id (the "OVR" hash, 0xc8e8d0b1a89ff4f8 at 0x2ba11c0, gives 4; "PSN" 2; "DMO" 7); no
  match gives 0. pnsovr's `UserProviderID` and `ProviderID` exports both return the word at libpnsovr 0x70e380 (.bss,
  written at run time; no store to it was found by static search). pnsovr's own `SNSUserID` constructor and
  `OpenFriendRequestUI` compare that word with the same "OVR" hash and make code 4, and the login rewrite refuses to run
  unless `CNSOVRUser`'s platform word is 4 (the platform the NEVR login carries and `SocialParty::MemberUuid`
  derives ids for: `OVR-ORG-<id>`). So the chain is consistent when the word holds the "OVR" hash, which is what pnsovr's
  native friend flow needs; the value itself was not read from a running process. `Initialize` reads the word once and
  logs `social_provider` (symbol, the platform code the game will derive, the code the login carries, match yes/NO), at
  warn level on a mismatch; a mismatch is the case that drops friend rows silently.

### Options

1. **Select pnsrad as the primary provider** (the PCVR enabler's route; the string `pnsovr` at libr15
   0x2ba11f7, or the `-noovr` flag, which selects `pnsdemo`). `Social`, `IAP` and `RichPresence` would
   resolve against a module that exports none of them, so the game gets no social object at all
   (`CR15NetGame::Initialize` skips the social callback registration when `Social()` returns null), and pnsovr's login, entitlement
   check and `Users` go with it. It also replaces the login flow the #221 rewrite hooks. pnsrad's
   `Friends` and `Party` stay unreachable. This alone does not produce NEVR friends or parties.
2. **Rewrite the remote-member id path** (`GotRemoteOrgIdCB`). Party and room state stay in the Oculus
   platform: the service never sees a party, the friend list is the Oculus one, and a NEVR id for an
   Oculus org id needs a server-side mapping the client does not have. It fixes the ids and delivers none
   of the PCVR friends or party behavior.
3. **Replace the `CNSOVRSocial` slots in place** (76 `R_AARCH64_ABS64` relocations in libpnsovr's
   `.data.rel.ro`, `_ZTVN10NRadEngine12CNSOVRSocialE` at 0x6a1468). Same logic as the facade, but it needs
   a data-slot write primitive `GotHook` does not have, and the Oculus-side state machine keeps running
   against a half-replaced object.
4. **Hand the game a different `CNSISocial` object** by swapping the `CNSProvider::Social` PLT slot in
   libr15. Chosen.

### The facade

`src/quest/social/` is the Quest counterpart of the PCVR facade.

- **Hook point.** libr15's `R_AARCH64_JUMP_SLOT` for `_ZN10NRadEngine11CNSProvider6SocialEm` at
  0x36ef528 (BIND_NOW, one relocation for the symbol, defined in libr15 itself at 0x192fe20), the same
  kind of seam as the config-string hook. It has one call site, so it runs once per run. The handler calls
  the original and replaces the result only when its first word equals libpnsovr's load bias plus
  0x6a1478 (the address point of `CNSOVRSocial`'s vtable, stored by its constructor at 0x203238) and
  libpnsovr's build id is the pinned one. A null result, a missing pnsovr, another build or another
  class passes through unchanged and a counter records which.
- **Object.** 0xbb0 bytes (what pnsovr allocates), vtable of 76 free functions in Quest slot order
  (`social_abi.h`). Quest slots equal the PCVR facade's plus one from slot 12, where the Itanium ABI has
  two destructor slots. The fields the game and the engine's non-virtual `CNSISocial` code read
  directly keep their offsets: counts at +0x200/+0x204, member JSON array +0x248, max members +0x250
  (`CNSIRichPresence::Set`, 0x1919000), lobby uuid/match type/team/type/flags at +0x260/+0x270/+0x278/
  +0x27a/+0x27c, room id +0x2a8, owner index +0x2b0, join policy +0x2b4. Two more are written by the
  game: +0x1e8 (`CR15NetGame::Initialize` stores 2, libr15 0x12866bc) and the party CJson at +0x1f0.
  That CJson belongs to the social object (pnsovr's constructor builds it, 0x203254, its destructor
  destroys it, `CJson::~CJson` at 0x20845c), and `CR15NetGame::Update` fills it whenever `IsHost` answers
  true (`SetInt` 0x12951d4/0x1295628, `SetSymbol` 0x1295240, `Clear` 0x1295254) and then sets bit 0 of
  +0x27c (0x1295044, 0x1295204). `Reset` clears it, as the native `CNSISocial::Reset` does with
  `CJson::Reset` (libpnsovr 0x36a92c, libr15 0x19197cc): the facade calls the game's own `CJson::Reset`
  (libr15 export, 36 bytes at 0xfa227c) on it from `SlotResetEntry` in `social_game_calls.cpp`, the
  `-fno-exceptions` unit, because the call frees a tree the game allocated. The installer resolves the
  function as libr15's load address plus the pinned export address, after checking libr15's build id;
  `social_pinned_test` checks that address against the library's dynamic symbol table and the function's
  first instruction. Where it cannot be resolved, `Reset` leaves the CJson alone and counts
  (`social_json_failed`). The facade keeps a pointer to its owner in the last word.
- **Member count.** The game indexes the member JSON array at +0x248 by the member count the object
  reports and by the index each member callback carries, and checks the index only against slot 27
  (`PartyMemberData` 0x129b3fc, `PartyMemberHeadsetType` 0x129b168; `PartyMemberJoinedCB` 0x126f8ac loads the
  array pointer at +0x248, adds `index << 4` and calls `CJson::Int` with no check at all). The array holds
  `kMemberJsonSlots` = 10 entries, so the game is shown the first 10 members of the model and no more,
  whatever the server sends: slot 27, +0x200/+0x204, `MemberId` and `MemberName` stay within 10, and no
  `MemberJoined`, `MemberUpdated` or `MemberLeft` is delivered for a member past it. Such a member exists in
  the model and is invisible to the game (counted, `social_members_hidden`, logged once per party). When a
  visible member leaves, the first hidden one moves into the window and is announced then, at its new
  position, after the `MemberLeft`. A callback's index is the member's position when the frame is published,
  not the one it had when the model queued the event. `Host` and `IsHost` answer from the model's owner id,
  so a hidden host is still the host. Before the first `Update` the count is
  at least the local-user count `AddMember` wrote, so the engine's `MemberCount - [+0x200]`
  (`PlatformPurchaseSucceededCB`) is never negative. The PCVR facade has the same 10-entry array and no
  such handling (#234).
- **Callbacks.** `Initialize` copies the 15 delegates (0x20 bytes each, context, 16 inline bytes, proxy)
  that `CR15NetGame::Initialize` builds (0x12866ac..0x1286a60). Their order is the PCVR order:
  created, joined, join failed, updated, host changed, left, kicked, invitation accepted (the gate),
  deep link, member joined, member updated, member left, friends refreshed, invite failed, invite
  received. They run on the game thread from `Update`.
- **Ids.** IDs are returned by value in x0 (AAPCS64); there is no hidden result pointer.
  Everything the facade reports is a NEVR account id from `SocialParty`/`SocialRoster`, which the
  service's messages fill; the local member is the account passed to `SetLocalAccount`.
- **Frames.** `ObserveFrames` walks the EVR frames the loopback bridge relays and applies the
  server-to-game ones by CSymbol64 hash, as `ObserveSocialFrames` does by name. Requests the models ask
  for go out through `SocialParty::SetSender`.
- **Exceptions.** None crosses between game frames and sentinel frames in either direction
  (`callback_thunk.h`). The slots that call the game (`Update` delivering the party callbacks,
  `JoinInternal` and `AcceptInvite` asking the accept gate) are in `social_game_calls.cpp`, built
  `-fno-exceptions`: no landing pad, no LSDA. They call `social_internal.h` functions in
  `social_facade.cpp` that catch `std::exception`, count and log it, and have returned before the game
  is called; events are copied into a fixed batch (32 per frame, the rest counted and logged) so no heap
  object is alive across a game call. The `Social()` hook is a `NEVR_HOOK_RECORD` in `social_install.cpp`
  (`-fno-exceptions`) installed with `InstallThunk<SocialThunk>`; its `noexcept` handler reads the
  facade object published at install, constructs nothing and never logs: it increments counters
  (selected, null result, pnsovr unavailable, foreign object) that the sentinel's reporter thread logs.
  The frame sensor walks the direct edges from the record's entry and handler. Every other
  slot is `noexcept`, catches `std::exception` and answers its zero value; none of them calls the game.
  `tools/check_quest_social_frames.sh` pins the built objects (no `__gxx_personality_v0`, no
  `.gcc_except_table`, a negative control on the object that has both); the facade test throws from a
  callback and from the accept gate and checks the exception reaches the game's caller uncaught and
  uncounted. Measured on the host with one C++ runtime; the two-runtime crossing is the thunk contract's
  inferred consequence and was not run on a device.
- **Lifetime.** `Facade::Instance()` is constructed on first use and never destroyed (a leaked
  object): the game and network threads use it until the process ends, and a function-local static
  would be destroyed during exit while they run. A test registers an exit check before first use.
- **Before login.** With no signed-in account the facade sends no party create, defers joins, and
  reports no local member.
- **Refused requests.** A create, join or lock request the sender refuses (no `SetSender` yet, a closed
  connection) is logged `NOT_sent`, counted (`social_send_failed`) and rolled back in the model
  (`SocialParty::State::AbandonCreate`, `AbandonJoining`, `ForgetLockRequest`), so the state does not stay
  "creating" or "joining" and defer every later join. What happens next differs by request. A create is asked
  again by `Update` once the game still wants a party and five seconds have passed since the last attempt
  (pnsovr's own interval, libpnsovr 0x2045f4, `cmp w8, #5`). A lock is asked again by `Update` on the same
  interval, one log line per attempt. A join is not sent again by the facade: the model restores the invites
  the join consumed (so a retry by party id is the invite's accept again, not a plain join) and the game is told
  `JoinFailed` with code 0, which `CR15NetGame::PartyJoinFailedCB` (libr15 0x126f630) takes down its
  "unknown" branch (any code outside 1..6: the `b.hi` at 0x126f648 and 0x126f674, to 0x126f69c and 0x126f6c4) to the
  generic failure event; the player
  retries. A deferred join is counted every frame (`social_join_deferred`) and logged once per party. An
  invite queued behind a create is queued once per target.
- **Unanswered requests.** A create, join or lock the sender took and the server never answers is treated
  as refused after 10 seconds (`social_request_timeout`, one warning line each): the same rollback, the same
  retry for the create and the lock, and for a join the same `JoinFailed` to the game. The 10 seconds is twice
  the longest wait the game's own code applies (the 5 s create retry). Nakama answers a create at once and
  answers a join at once, except that a join to a locked party waits for the leader with no reply at all
  (`snsPartyJoinRequest`), so silence is a normal outcome there. A reply that arrives after the deadline is
  applied as usual. A create asked twice is not two parties: Nakama leaves the caller's current party before it
  creates the new one (`snsPartyCreateRequest`), so the later `PartyCreateSuccess` is the party the model ends
  in. Reply times of a live server were not measured.
- **Sender contract.** `SocialParty::Send` returns false when any frame of the batch was refused, and still
  hands the remaining frames of the batch to the sender; a false return means that frame was not written. The
  PCVR sender returns false only with no login connection or when the websocket refused the frame, and true
  for a frame queued while the connection opens, which is the unanswered case above. The Quest network
  adapter's sender must follow the same rule (false: nothing of that frame left; true: taken, possibly queued).
  Create, join and lock requests are single frames.
- **Events.** One `Update` delivers at most 32 callbacks; the rest are carried to the next frame in order,
  so a `Left`, `Kicked` or `MemberJoined` is delayed, not lost. A repeat of an `Updated` or `MemberUpdated` of
  the same index directly after its twin, and any `InviteReceived` while one is due (its argument is always 0
  and the invite list is read when it runs), are merged. The carry queue holds 256; past that the oldest
  `Updated`, `MemberUpdated` or `InviteReceived` is dropped to make room. Events that say something no later
  event repeats (`Created`, `Joined`, `JoinFailed`, `HostChanged`, `Left`, `Kicked`, `MemberJoined`,
  `MemberLeft`) are never dropped for room; the queue may grow to 4096 with them, and only past that is the
  newest dropped. Every drop is counted (`social_events_dropped`).
- **Observability.** Nothing on the game's call path logs. Deliveries are counted by class and the reporter thread logs
  the counters (`hook_counter`): `social_cb_created`, `social_cb_member_joined`,
  `social_cb_join_failed` and `social_cb_other` (every other callback, `PartyJoinedCB` and the accept gate included); the server frames
  the observer applied or ignored are logged on the network adapter's thread (`social_frame`, `social_frame_ignored`,
  `social_party_data_received`). Friend rows have no callback: their deliveries are the `FriendListResponse` and
  `FriendStatusNotify` frames logged by `social_frame` and the row reads counted by `social_slot`. The package registers 19
  counters (the hook's 6; the facade's 5 above; the four callback classes; `social_json_failed`, which also counts a
  Reset that could not call the game's `CJson::Reset`; `social_frames_ignored`; the invite gate's 2); the budget is the
  reporter's 32 for the whole program.
- **Logging.** Every request logs its name, symbol and whether it was sent, with the ids it carries: the
  account it is aimed at (`target`: the invite target, the kicked, passed or answered member, the profile
  asked about), the party, and the Standard message's subject (`arg`) or a Targeted message's parameter
  (`param`); a Targeted message carries only a UUID derived from the account id, so its builder records the
  account id for the log. No secret is in any of them. The lines go to logcat unless the
  integration installs the sentinel's disk sink (`sentinel::SetLogSink`, PR #220's `sentinel_log.h`), so
  durability is the integration step's to provide; this package does not install one.

Party and member data (headset type per member, the lobby id a non-host member follows) is carried as on the PC:

- **Receive.** `ObserveFrames` takes `SNSPartyDataNotify` (`social_frames.cpp`), checks it is a JSON object and hands
  it to `SocialParty::State::ReceiveData`. The first `Update` after it loads it into the game's CJson before the
  callbacks fire (a `MemberJoined` callback already finds the member's `headsettype`, `PartyMemberJoinedCB`
  0x126f8ac): each remote member's data into its slot of the member array at +0x248 (16 bytes per slot, slot 0 is the
  local member's own and is never loaded), the party's into +0x1f0 for a member (the leader's party data is its own).
  A slot whose member changed is reloaded or cleared. Then `MemberUpdated` for a member whose data loaded, `Updated`
  for the party's. A frame that changes nothing (unreadable, not an object, for another party, the leader's own) is
  counted (`social_frames_ignored`) and logged with its reason (`social_frame_ignored`).
- **Share.** `MemberDataWritable` (slot 31) hands out the local member's CJson (the first slot of the array) and notes
  it; after the callbacks `Update` reads out what the game wrote (the leader's party data when the game set bit 0 of
  +0x27c, which is cleared; the local member's after `MemberDataWritable`; both once on entering a party) and sends it
  as `PartyDataUpdateRequest`. Only a JSON object is sent; the game's empty document is `{}`.
- **The game's functions.** All three are libr15 exports called through their pinned addresses (build id checked, as
  for `CJson::Reset`): `CJson::DecodeFrom(char const*, unsigned long long)` at 0xfa7e8c replaces a CJson's document
  with the text and returns 0, or an engine error id when the text does not parse; `CJson::EncodeToCompact(char*,
  unsigned long long&, unsigned, char const*) const`, a thunk at 0xfa7e64 to `EncodeTo` at 0xfa7a38, writes the compact
  text of the node at a path (`""`: the document, `{}` when empty) into a caller buffer of the capacity in the size
  argument and returns 0, or an error id when the text is longer. A caller buffer avoids `EncodeToCompactTStr`, which
  returns an engine `CMemBlock` that would have to be released with engine code. `social_pinned_test` checks the three
  exports, their sizes and first instructions.
- **Where the calls are.** Every call into the game is in `social_game_calls.cpp` (`-fno-exceptions`). The facade
  builds a plan of plain operations (`JsonPlan`: load or clear a slot, with a pointer and length into strings it keeps
  alive until the next plan) and a share job (`ShareJob`: which CJson to read, into buffers the facade owns); the
  game-call side runs them and records each outcome; the facade reads the outcomes back. No object with a destructor and
  no landing pad is live across a call.
- **Without the functions** (libr15 absent or not the pinned build) server data is held, not lost: it is counted once
  (`social_json_failed`) and loads when the functions are known; nothing is shared. A load the game refuses, or a CJson it
  cannot read out, is counted and logged and does not stop the other data.

`RefreshInvites` and `FriendsRefreshed` are not driven, as on PCVR. The login must declare `nevr_social` level 1 for the
server to send `SNSPartyDataNotify` at all (the login package's input; this package cannot set it).

Invite gate: the game refuses a party invite before it reaches the social object unless the profile JSON reads
`npe|firstmatch|completed` true. Nine sites in libr15 read it with `CJson::Boolean(profile, "npe|firstmatch|completed",
0, 0)` (`PartySendInvite` 0x129c7d8, `OpenInviteUI` 0x129c9c8, `OpenNewInviteUI` 0x129ca6c and 0x129cb20, `OpenPartyUI`
0x129cd1c and 0x129cdd0, `PartyLobbyUnjoinable` 0x1259488, `DeepLinkCB` 0x126efb4, `CreateMatch` 0x126f1b8); the flag is written
true by `LogInSuccess` (0x126cc38..0x126cc94) only for an account whose two profile stats do not sum to zero or when a
configuration bit is set, so a community account would never invite. libr15 reaches `CJson::Boolean` (export,
0xfa4370) only through its PLT stub at 0xf3e540 (84 calls, no direct call), whose GOT slot is the BIND_NOW JUMP_SLOT at
0x36f6988, the only relocation for the symbol. `social_invite_gate.cpp` hooks that slot (`NEVR_HOOK_RECORD` +
`InstallThunk`, `-fno-exceptions`, `noexcept` handler): it calls the original and returns true for that one path, as the
PC does (`party_invite_gate.cpp`). It counts (`social_boolean_calls`, `social_invite_gate_forced`) and never logs.
`CJson::Boolean` is a general reader, so the handler is one path comparison per call. `InstallSocialHook` installs it
after the facade hook succeeds.

Display names: the server sends friends and party members as account ids; their names come from the game's own
profile request, whose reply is a zstd frame. `nevr_quest_social` compiles the PC's decoder
(`runtime/compat/social_names.cpp`) with the `zstd` port in `src/quest/vcpkg.json` (static, linked into the
sentinel, which links `-Wl,--exclude-libs,ALL` and exports only `JNI_OnLoad` and `nevr_sentinel_marker`; the
game's `libr15.so` and `libpnsovr.so` each export their own 138 `ZSTD_*` symbols, which a hidden static copy does
not meet). The PC registers the decoder from a namespace-scope initializer, which the sentinel may not carry
(`tools/check_quest_static_init.sh`), so the Quest compile defines `NEVR_SOCIAL_NAMES_NO_STATIC_REGISTRATION` and
`InstallSocialHook` calls `SocialNames::RegisterDefaultDecoder()`. Without that call no profile is requested and
rows show account ids. `social_names_test` decodes the real zstd frame the PC tests use and shows the name on a
friend row.

### Integration contract

What the integration commit calls, and when:

1. **Install, in the sentinel constructor** (`nevr_sentinel_ctor`, after `InitActivation()`, next to the
   existing GOT hooks): `quest_social::RegisterSocialReportCounters()` before `StartReporter` (it takes 19
   of the reporter's 32 counters; the clock hook takes 2 more, leaving room for the login, redirect and router hooks), then
   `quest_social::InstallSocialHook(sentinel::FeatureEnabled(Feature::kSocial))` after it.
   The target is libr15's own BIND_NOW slot, so libr15 only has to be mapped, which it is when its
   `DT_NEEDED` dependencies' constructors run (the `clock_gettime` hook installs there today); libpnsovr
   does not have to be loaded, because the handler looks it up when `Social()` is called. The hook must
   be live before `CR15NetGame::Initialize` reaches 0x12866a4 (inside `CR15Game::Initialize`, after the
   providers are created); a constructor install is always earlier.
2. **Order against the other hooks.** None is required. The login hook (#221) is installed from a libr15
   dlopen JUMP_SLOT (0x36c6380, from that branch's login hook source; not in this tree) and needs libpnsovr
   loaded (`CSysModule::Load`, libr15 0x2a9e16c); the matchmaking redirect is on
   libpnsradmatchmaking's GOT and needs that library, which `CNSLobby::LoadMatchmakingSupport` loads at
   the lobby stage; the config-string hooks are on libr15 and libpnsradmatchmaking. Different modules,
   different slots, no shared state.
3. **Login adapter:** `quest_social::SetLocalAccount(accountId, displayName)` once the service accepts the
   login (the NEVR account id, the id space of everything the facade reports).
4. **Network adapter:** `SocialParty::SetSender(fn)` before the first request can be sent (until then a
   request logs `NOT_sent`, is counted and is rolled back; the create and the lock are asked again by `Update`,
   a join is reported to the game as failed), and `quest_social::ObserveFrames(ProductionPorts(), direction, bytes, length,
   nowSeconds)` for every frame the bridge relays on the login connection, both directions, after the
   remote EVR login session is open.
   `InstallSocialHook` also installs the invite-gate override (a second GOT hook, on libr15's `CJson::Boolean` slot).
5. **Link:** `nevr_quest_social` (with its `zstd` and `nlohmann-json` dependencies) into `ovrplatformloader`. `social_install.cpp` and
   `social_game_calls.cpp` are `-fno-exceptions` (CMake source properties); the sentinel's link must not
   change that.

The social switch (for `quest_config.*`, owned by the config package; not edited here): a fourth
`Feature`, `kSocial`, named `social`, read from `features.social` in `nevr-quest.json`, off unless the file
turns it on, like the others. Its prerequisite is the login feature being effective (the facade is only
meaningful with an account and the service connection behind it); otherwise it is forced off with the
logged reason `login_not_enabled`. `Features` gains a `social` flag, `kFeatures` a fourth entry, and the
config test vectors cover: social alone forced off; social with login, bridge and redirect on; social
absent from the file. The only consumer is the install call above.

### Risks

- The object is a hand-built vtable over a layout read from two binaries; nothing ran on a headset.
  A wrong field offset shows as a wrong value, not a crash, except where the game or base code
  dereferences a pointer inside the object (+0x208, +0x2c8, +0x2e0, +0x2f8, +0x310, +0x328 are
  pointers `CNSOVRSocial::Initialize` (0x203604) and the base class set up; the facade leaves them
  null). The non-virtual wrappers checked (`LobbyId`, `CNSIRichPresence::Set`, `UpdateLobbyData`,
  `Join`, `Leave`, `Kick`, `PassOwnership`, `SendInvite`) read only the fields and slots listed above;
  `AddLocalMember`, `RemoveLocalMember`, `SwapMembers`, `RemoveRemoteMember` and the base `Initialize`,
  `Reset`, `Update` and `Shutdown` were not checked and are not called by the facade's slots.
- Oculus friends, invites and the Oculus party overlay no longer reach the game; social is the NEVR
  service's. Three more consequences follow from `CNSOVRSocial::Update` never being called:
  (a) **Oculus deep links stop.** "Launch to join" reaches the game through `CNSOVRSocial::Update` ->
  `FollowDeepLink` (libpnsovr 0x20460c, taken when the JSON at +0x290 is non-empty); nothing calls it now
  and the facade does not drive the deep-link callback. This is a loss on Quest, not parity with PCVR
  (which has no Oculus deep link). (b) **Oculus rich presence now advertises NEVR data.** `RichPresence`
  stays on pnsovr, but `CNSIRichPresence::Set` (libr15 0x1919000) reads the facade's `Id` (slot 26),
  `MemberCount` (27), +0x250 and `Joinable` (23), so the presence Oculus shows carries NEVR party ids,
  sizes and joinability. This is accepted: the data is informational on the Oculus side, and an Oculus
  "join" from it is not served anyway (a). Zeroing it would need a hook on `RichPresence` that this
  package does not install; if the owner wants it zeroed, that is a separate change. (c) **The
  invitable-users refresh goes away** (`RefreshInvitableUsers`, bit 1 of `Update`'s flags, libpnsovr
  0x20455c): the facade's friend list is the NEVR service's, so there is nothing to refresh.
- Name pointers: the facade returns `const char*` from the roster and party views. The three callers
  checked copy them into a 64-byte buffer before returning (`CR15NetFriendExpression` 0x2322db0,
  `CR15NetRecentlyMetUserExpression` 0x2332768, `CR15NetPartyMemberExpression` 0x232c098), so no pointer is
  kept across calls. The party view keeps a name valid for 128 `Update`s, the friend and recently-met
  rosters for 8 publishes (`social_roster.h`, shared with PCVR); only a burst of eight roster publishes on
  the network thread between the slot call and the copy, a few instructions, could free one. Not changed.
- `LocalId` (slot 30) returns pnsovr's invalid value for a non-local member, 0xFFFFFFFF (32-bit -1
  zero-extended, libpnsovr 0x205280). No direct call of that slot was found in libr15 (the review's
  slot-call scan and `PartyMemberIsLocal`, which compares `[+0x200]` itself).
- `ExitLobby` and `Reset` store sixteen zero bytes where the native code copies `NRadEngine::SUuid::kInvalid`.
  libr15 defines that object in .bss (0x376c3b8, 16 bytes) and the only write through its GOT entry
  (0x372adc8) is its initialiser, `CMemory::Fill(&kInvalid, 0, 16)` at 0xf54c4c..0xf54c58; the other 136
  loads of the entry read it. libpnsovr defines its own copy (0x71ab68), which the facade never reads.
- `UserProviderID` still comes from pnsovr (no pnsrad-enabler equivalent is installed). If its word is not the "OVR"
  hash, friend rows are dropped silently, as they were on PCVR before its provider patch; `social_provider` at
  `Initialize` shows which case a headset run is in. The fix for a mismatch (storing the "OVR" hash in that word) is not
  made without that evidence, because pnsovr uses the word for its own user and presence code.
- The packaged APK differs from the pinned one only if its `libr15.so`/`libpnsovr.so` hashes differ;
  the hook refuses on a build id mismatch.

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
