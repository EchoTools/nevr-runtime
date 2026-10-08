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
   repeated duplicate keys collapse to one line per name. A name taken from the file is logged
   only when it is one of the keys or features above; any other name is counted ("unknown key
   #N"), never echoed. `sentinel_host_test` runs
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
   through the game ABI, and defer networking and file I/O out of loader constructors. The
   sentinel's ELF constructor (`nevr_sentinel_ctor`) is the exception, and does exactly this, in
   this order:
   - logs one `sentinel_ctor` record (logcat only);
   - `Arm()`: `mkdir`/`access` on up to four candidate crash-dump directories, then allocates the
     Breakpad exception handler (`new`), outside any `try` block;
   - `InitActivation()`: opens `nevr-quest.json` read-only and `nevr-sentinel.log` read-write in
     append mode in the app's external files directory. Both opens are non-blocking, the log open
     does not follow symlinks, and each target must be a regular file (a FIFO or device is
     refused, not opened for I/O). The config read is bounded at 64 KiB. A failed log open is
     retried at most once per 30 s, and never when the path is not a regular file. The catch block
     of `InitActivation()` allocates nothing, so a failure there cannot escape the constructor;
   - `InstallBasicsHook()`: installs one GOT hook on `libr15.so`'s `clock_gettime` import (an
     `mprotect` of the slot's page) and creates the counter reporter thread.

   It opens no socket and does no TLS. `Arm()` can still throw `std::bad_alloc` (it allocates
   outside a `try`). The on-disk log is rotated, never deleted: at open once it is 1 MiB or more,
   and in-process once this run has written 1 MiB; the old file keeps a `nevr-sentinel.<unix_ms>`
   name, so every process start that begins with a full log, and every 1 MiB written, leaves one
   more file. Nothing prunes them; whether that is acceptable is the owner's decision. The remaining
   risk is a stall in the storage layer of the headset (FUSE-backed external storage): the
   non-blocking opens do not bound it, and it has not been measured on a device. An
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
