# ADR 0003: Quest networking shares the PCVR protocol core and differs only in adapters

Status: accepted. The login-profile builder, the EVR frame codec and the redirect policy are
shared today (`src/runtime/compat/login_profile.{h,cpp}`, `src/runtime/compat/evr_codec.{h,cpp}`,
`src/runtime/lifecycle/service_redirect.{h,cpp}`).
The social facade is implemented and host-tested in `src/quest/social/`. The rest is not
implemented; the work is tracked in #158 and the test regime is ADR 0004.

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
them. `src/quest/auth/` holds the Android adapters:

- HTTP is libcurl (>= 7.87, checked at configure time) over OpenSSL from the Quest vcpkg
  manifest (`arm64-android` triplet, no `builtin-baseline`, as in the root manifest), with peer
  and host verification on, https only, no redirects, no proxy environment variables, a capped
  response, and requests that a shutdown interrupts, including during a DNS lookup. Trust anchors
  are read from `/apex/com.android.conscrypt/cacerts` (when it yields a certificate) or
  `/system/etc/security/cacerts` into memory, each file parsed with OpenSSL (PEM or DER) and
  re-encoded, so one corrupt file cannot make libcurl reject the whole blob; the blob is passed
  as `CAINFO_BLOB`. `CAPATH` is not used: OpenSSL looks a CApath file up by the SHA-1 based
  subject hash and Android names its files by the old MD5 based one. With no certificate loaded
  every request fails closed.
- The refresh token is written to `/data/user/<uid / 100000>/<package>/files/.credentials.json`,
  the package taken from `/proc/self/cmdline`, because `/sdcard` does not enforce file modes. The
  write is a fresh exclusive temp file, fsync, rename, directory fsync; a read refuses a symlink.
  If the directory cannot be derived the login runs without persisting and says so. Only the
  login link (`device_login.txt`, under `/sdcard/Android/data/com.readyatdawn.r15/files/`) is on
  external storage.
- `Session` does the login on a worker thread, so `Start()` never blocks the caller. At startup a
  cached refresh token is tried first (three attempts); a refresh the server refuses for the token
  (400/401/403 whose body names the refresh token) goes straight to the device login, with
  `Refreshing -> AwaitingUser`. A 401/403 that does not name the token (nakama answers a wrong
  `http_key` with 401) is logged as such and is not treated as a bad token. A transient failure (no
  connection, 5xx, 429, an unreadable response) of the cached refresh or of the device-code
  request is retried on a bounded backoff (5, 15, 45, 135, 300 s) and then the login is `Failed`;
  a 4xx is never retried, and a transient cached-login failure does not prompt the player. After
  login, a refresh token that has expired or that the server refuses publishes `Expired`, logs
  once and starts the device login again. A poll request that fails does not end the login, but
  five in a row do.

`nevr_quest_token_auth` is not linked into the sentinel, and nothing yet hands the token to the
login path. The tests are `src/quest/tests/auth_core_test.cpp` (fake HTTP and clock) and
`src/quest/tests/tls_ca_test.cpp` (loopback TLS peers with a generated CA and an Android-style
directory), run on the host by `just test-quest-shared`. Not established on a headset: the CA
directories and the libcurl/OpenSSL stack, that the process name is the package name, write access
to the app-internal directory (and that Quest multi-user uses `/data/user/<n>`), and how the player
is shown the login link.

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
 EVR frame codec         | social state | EVR session routing
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
   `nlohmann::json` and is compiled for Windows and Android. `src/runtime/compat/evr_codec.{h,cpp}` holds
   the EVR frame parse and build functions, the LoginRequest payload layout
   (`[UUID 16][platform 8][account 8][profile JSON][NUL]`), the bridge login platform
   (`kBridgeLoginPlatform`, 4) and the bearer-replacing-path test. It has no Windows, Winsock or
   logging dependency; `ws_bridge.cpp` and `src/quest` compile the same file, and Quest does not
   copy that assembly. A reviewed serializer and a server-parser round trip remain open. The platform numbering the bridge sends
   is the server's wire enum; Quest identity values need binary or API evidence.
3. **Session routing.** `src/runtime/compat/session_router.{h,cpp}` is the platform-neutral router:
   no Windows or Winsock headers, no sockets, no threads. Game-side and remote open/frame/close
   events go in; the router owns connection identity (config, login, matchmaker numbered in
   arrival order, matchmakers attached to the login session), once-per-session login injection
   ahead of the frames the game queued, frame order, size limits, bounded queues and
   backpressure, and, when a remote session ends, the close of every game socket on it plus
   forgetting the session so the next connection is a new login. The game transport, the remote
   transport, the login-frame builder and the log sink are injected; the router calls none of
   them under its lock. No token, password, full login frame or credential URL is logged.
   `src/quest/net/` holds the Android adapters: `loopback_game_server` (a POSIX WebSocket
   server on an ephemeral 127.0.0.1 port, RFC 6455 in `ws_wire`), `remote_ws` (the remote
   transport and its policy: `wss://` only, one connect attempt per session, no retry and no
   downgrade after a failure) over `curl_ws_connector` (libcurl from the Quest vcpkg manifest
   with peer and host verification always on, TLS 1.2 or later, and trust loaded from the Android
   CA directories into an in-memory `CURLOPT_CAINFO_BLOB` by the loader token auth uses), and `session_bridge`, which
   composes them and reports the loopback port that `nevr_cfg::ResolveRedirect` needs. The
   Windows `ws_bridge.cpp` does not use the router yet; it keeps its own copy of these rules.
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

Return value and lifetime, from the disassembly of both copies (`libr15.so` `0xfa2e7c`,
`libpnsradmatchmaking.so` `0x209484`): `this` is saved from `x0`, the key from `x1`, the fallback
from `x2`, the flag from `w3`. A lookup helper (`0xfa0950`, `0x207f58`; no symbol at either
address, `CJson::PopulateCache` is at `0xfa06b0` and `0x207cb8` and also calls it) looks the key
up; if it finds a node whose type word is 2 (string) the result is `json_string_value` of that node
(`0xfa2efc`, `0x209504`), a pointer into the `CJson`'s own value, else the result is the caller's
`x2` unchanged. A nonzero `w3` only adds an error log line for a missing key (`0xfa2f08`). The
function returns that pointer in `x0` (`0xfa2f78`) and neither copies nor transfers ownership, so a
fallback or a stored value has the lifetime its owner gives it, and a replacement has to outlive
the caller's next lookup and `CUriContainer::Parse`. Other overloads are imported and not hooked:
`libr15.so` also imports `TString(void*, char const*)` (JUMP_SLOT `0x36e92d0`) and
`TString(CSymbol64, char const*)` (`0x36fa288`); `libpnsradmatchmaking.so` imports only the
`CSymbol64` one (`0x6bb458`).

Keys. Each of the eight literal `_host` key strings in the two libraries has the xrefs below; each is
either a `CJson::TString` first argument or a `CJson::SetString` first argument (the latter at `libr15.so`
`0x11f85fc` and `0x11f868c` for `config_host` and `login_host`, `0x11f871c` and `0x11f87ac` for
`radserverdb_host`, which `CR15Game::PreprocessCommandLine` writes). In each `TString` pair the
first call's fallback is the game's built-in `wss://` default and the second call's fallback is the
first call's result, which then goes to `CUriContainer::Parse`.

| Library | Caller (call site) | Key read first, then second | Built-in default (fallback of the first read) |
| --- | --- | --- | --- |
| `libr15.so` | `Initialize` (`0x1286060`, `0x1286078`) | `config_host`, `configservice_host` | `wss://config.readyatdawn.com/rad/rad15_live` |
| `libr15.so` | `VerifyServerLoginConnection` (`0x12508bc`, `0x12508d4`) and `BeginLogIn` (`0x1291b10`, `0x1291b28`) | `login_host`, `loginservice_host` | `wss://login.readyatdawn.com/rad/rad15_live` |
| `libr15.so` | `LogInSuccess` (`0x126cd1c`, `0x126cd34`) and the function at `0x1289ac8` called from `BeginMultiplayer` (`0x128a790`, `0x128a7a8`) | `transaction_host`, `transactionservice_host` | `wss://transaction.readyatdawn.com/rad/rad15_live` |
| `libpnsradmatchmaking.so` | `MatchmakerUri` (`0x1b2150`) and `ConnectMatchmaker` (`0x1b22b8`, `0x1b22d0`) | `matchmaker_host`, `matchingservice_host` | `wss://matchmaker.readyatdawn.com/rad/rad15_live` |

`libpnsradmatchmaking.so` also builds two keys per match type with `MakeTStrPrintF`: the formats
`matchingservice_%s_host` (string `0x530ac2`) and `matchmaker_%s_host` (`0x530ada`), referenced at
`0x1b2184`, `0x1b219c` (`MatchTypeSpecificMatchmakerUri`, read at `0x1b21c0` and `0x1b21d4`) and
`0x1b24e0`, `0x1b24f4` (the per-match-type loop in `ConnectMatchmaker`, read at `0x1b2510` and
`0x1b2524` with fallback `''`, the result chained as the next fallback and passed to
`CUriContainer::Parse` at `0x1b2538`). The game compares the plain host to
`matchmaker.readyatdawn.com` (`0x530b3c`) at `0x1b2698` and only on a match builds
`matchmaker-%s.readyatdawn.com` from `match_type_specific_matchmakers|%s|host_suffix`; with
`matchmaker_host` redirected that branch is not taken (inferred, not run). These two formats and the
eight literals are the only `_host` key formats in the strings of the three Quest libraries
(`strings -a | grep '%s.*host'`).

The Quest redirect applies to the eight literals and to `matchmaker_<type>_host` and
`matchingservice_<type>_host` with a non-empty type (`src/quest/redirect/service_redirector.h`,
`IsServiceHostKey`). It matches by key, not by value like PCVR, because the two libraries make 175
and 62 `TString` calls (169 and 56 `bl` plus 6 tail-call `b` sites each, one of them the second read
in `MatchmakerUri` at `0x1b216c`), 87 and 28 of the `bl` sites with a key not recovered statically,
many from per-frame script readers (for example `0x23270ac`); a by-value rule would take the cache lock
and run the policy for any `ws://` or `https://` string any of them reads. The key rule is exhaustive
for the host keys by the string search above. Whether the NEVR config response carries
`match_type_specific_matchmakers` or these keys is not verified: no reference exists in
`~/src/nakama`, and the reconstruction note `match_join_flow.md` says the game may take a
match-type-specific URL from that config key.

HTTP is not covered by this hook. `https://api.readyatdawn.com` (`libr15.so` `0x126c270`,
`0x128958c`) goes to `CSysHttp::CreateConnection`, not through `TString`; so does
`CR15NetStoreTransactions::InitializeHttp`, which reads the key `env` (`0x126c214`) and, when it is
not `live`, builds `https://api-%s.readyatdawn.com` (`0x126c240`) for `CreateConnection`
(`0x126c25c`). An HTTP hook is a separate tranche.

The PCVR runtime redirects by value for every key (`config.cpp`, `RedirectServiceUrl`) and uses a
key list only for the login override. Quest keeps the shared value policy
(`nevr_cfg::ResolveRedirect`) and narrows it to the eight keys, so a ws:// value in an unrelated
config key is never rewritten.

Quest hook (`src/quest/redirect`). `tstring_thunks.cpp` is the only redirect file that includes
`callback_thunk.h` and `pinned_targets.h`, so it is built with `-fno-exceptions`: its `noexcept`
handler calls the game's original and then one `noexcept` function pointer (`ApplyFn`,
`tstring_thunks.h`), and has no landing pad. The pointer is set by `hook_adapter.cpp` (exceptions
enabled), which holds the installation, the redirector's lifetime and `ApplyActive`, which hands the
key and the result to `ServiceRedirector::Apply`. The call is indirect on purpose: the callee's frames
are sentinel-only and never on the stack across a call into the game, but they do carry a
personality (they catch), so a direct call would put them in the frame sensor's walk
(`TestHookFramesCarryNoPersonality`), which follows direct edges only. `Apply` catches
`std::exception` in its own frames; an exception of any other type would reach the `noexcept`
boundary and terminate, and nothing it calls throws one. Nothing in a hooked call logs: the
thunks and `Apply` only count, and `RegisterRedirectCounters` hands the counters to the reporter
thread. `just test-quest-redirect` checks that `tstring_thunks.o` has no `zPLR` frames and runs the
test under ThreadSanitizer, where deleting the cache lock fails it. `Apply` returns the original pointer unchanged for any other key, when
the redirect feature is off, for a value the policy declines, and on any failure (a value over 512
bytes, a pool refusal, an exception), logging the key name and a status token and never a URL. For a
redirected value it returns a pointer from the stable string pool, so the same value always maps to
the same address for the rest of the process. The first sight of each distinct value runs the policy
and may intern one string; up to 16 values are remembered, and the game's four built-in defaults are
resolved when the hooks are installed, so the normal reads allocate nothing. The sentinel does not
call `InstallRedirectHooks` yet. The caller's contract (also in `hook_adapter.h`): register the
counters before `StartReporter`; install from the sentinel's ELF constructor so it precedes
`CR15NetGame::Initialize`, which reads `config_host` at `0x1286060` (a value read earlier stays as
parsed; this rests on Bionic constructor ordering and is not tested on a headset); install
`libpnsradmatchmaking` by hooking libr15's dlopen slot (JUMP_SLOT `0x36c6380`, the string
"pnsradmatchmaking") and calling `InstallMatchmakingRedirect` after the real call returns, which is
early enough because `ConnectMatchmaker` (`0x1b22b8`) re-reads the host on every connect. A failed
install keeps the redirector and the next call retries the slot that is not installed; a poisoned
slot is logged and not retried. When all 16 cache entries were computed under the current bridge
state, a further distinct value is not remembered and each read of it runs the policy again.

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
from the current identity. Two guards: the id is put back only while the global still holds
the value the rewrite wrote (a value the game stored in between is left alone), and only a
real id is remembered. 0 (`RadPluginShutdown`) and -1 (the error path and the "fetch again"
marker) are not ids; when the global holds one of them and nothing is remembered the rewrite
is refused, the Oculus login goes out unchanged, and the record says `restored=0`. Every
declined record carries `restored=0|1` (no values). The adapter's mutex serializes its own
accesses only; the game's writers (`0x1ec998`, `0x1ecef0`, `0x1ecf18`, `0x207074`) are not under
it, and the rewrite (set, verify, JSON, restore) is not atomic against them. The login path is
assumed to run on one game thread with no concurrent writer while a login is in flight; that
is an assumption, not a measurement. The -1 gate at `0x1ec980` is not the only way an org-id
fetch starts: one is also issued at plugin init (`ovr_User_GetOrgScopedID` at `0x2069bc`, same
callback key `0x6e2f10`), and the error callback re-issues one at `0x1ecf80` without writing -1
first. What follows from the code (inference, not run): a send needs the global to be neither 0
nor -1. 0 sets `[this+0x170]` to 1 and defers (`0x1ecd14`-`0x1ecd18` to `0x1ecda4`); -1
refetches (`0x1ec980`, `0x1ec998`, `0x1ecd14`) and defers again; a pending login that
`UpdateInternal` sees with -1 goes to `LogInFailed` 500 (`0x1eda10` to `0x1edb54`, `blr` through
`[vtable+0x10]`). So the "refuse to remember 0 or -1" branch can only be reached by a writer
racing between the check and the send, and it never blocks a login the game would otherwise
send.

Readers and writers of the global, and callers of `AccountID()`, in the pinned build (measured
unless marked; addresses are function starts unless a row says "at"):

| Reader or writer | Effect |
| --- | --- |
| `LogInInternal` (reads at `0x1ec96c`) | re-fetches only when -1 (above) |
| `LogInInternal` (writes at `0x1ec998`) | `str xzr,[x8,#0x18]` with `x8` = `0x70e3c8`, i.e. the global at `0x70e3e0`: writes 0 on the re-fetch path |
| `UpdateInternal` (reads at `0x1eda08` and `0x1edba8`) | -1 leads to `LogInFailed` 500 ("prerequisites are missing", string `0x556b40`); zero waits |
| `GotLoggedInUserOrgIdCb` (writes at `0x1ecef0` and `0x1ecf18`) | -1 on its error path, the org id on success; also writes the decimal Oculus id string at `0x70e458` |
| `RadPluginShutdown` (writes at `0x207074`) | writes 0 |
| `CNSOVRUser::OfflineID()` (`0x1ede20`, vtable slot `+0x78`) | returns the decimal Oculus id string at `0x70e458` (`adrp x0,0x70e000; add x0,x0,#0x458; ret`), written by `GotLoggedInUserOrgIdCb` (`0x1ecf18`-`0x1ecf2c`) and never changed by the rewrite: after a rewrite `AccountID()` is the NEVR id and `OfflineID()` is still the Oculus id. Who calls it through the vtable was not traced |
| `CNSOVRUser::AccountID()` `0x1ede14` | returns it (vtable slot `0x6a1300`) |
| `CNSUser::SendLogInRequest`, `LogInSuccessCB`, `LogInFailureCB`, `LogOut`, `RefreshProfile`, `Profile*CB`, `LoginRemovedCB`, `UniqueName`, `SaveClientProfileChanges`, `CNSIUsers::CreateUser`, `User(UserAccountID)`, `DestroyUserInternal` | call `AccountID()` through `vtable+0x70` |
| `CNSLobby::JoinAcceptedCBClient` (`0x3720c0`), `AddEntrantAcceptedCBClient` (`0x3727a0`) | find the local user by `AccountID()` equal to the entrant id the server sent: the id the server uses is required here |
| `CNSUser::UserID()` | about 60 call sites in `libr15.so` (lobby find, join and create, party, friends, profile, IAP, XPlatformId) |
| `CNSOVRSocial::FollowDeepLink` (`0x1f2ab0`, read at `0x1f2b30`), `EnsureLocalMember` (`0x1f2b98`, at `0x1f2bd0`), `JoinInternal` (`0x1f37ac`, at `0x1f38a4`), `AddMember` (`0x2049f4`, at `0x204a30`) | copy it into `[this+0x2e0][0]`, the local party member; `MemberId` (`0x205260`) and `Host` (`0x2051fc`) hand that to the game, while remote members carry Oculus org ids (`GotRemoteOrgIdCB` `0x1f9090`) |
| `ovr_Room_KickUser`, `SyncRoom`, `ReceiveData` | use `[0x2c8]` (the app-scoped id), not the global: Oculus room calls are not affected (independent review; not re-traced here) |
| `CNSIParty::Update` (`0x369764`), `CNSIRichPresence::Update`, `CNSIFriends::Sent` | call `vtable+0x70` on their own object, not `AccountID()` |
| `libpnsrad.so` `CNSRADFriends`, `CNSRADParty` | use `CNSRADUser` (vtable `0x6f1e00`, `AccountID` = `[this+0x88]` at `0x3cd4c0`), not this global (independent review) |

Open: party, room and friends flows that read the global through `CNSOVRSocial` see the NEVR id
for the local member and Oculus org ids for remote members, two id spaces in one flow; and
`OfflineID()` keeps the Oculus id while `AccountID()` is the NEVR id (its virtual callers were
not traced). The
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

CJson behaviour for a write, in `libpnsovr.so` (the code the rewrite calls): the setter walks the
`|`-separated path (`0x35ba84`, which branches to `0x364bdc`), and when a parent exists and is
not an object (`ldr w8,[x1]; cbz w8` at `0x364bf8`-`0x364c00`) it logs `$ json path: %s is not an
object.` (string `0x5825b1`) and writes nothing. The parent check in the rewrite is therefore
defensive: with it off, the refused write fails the read-back and the rollback finds nothing to
undo, so real behaviour is the same; the test that exercises the check models a build that would
overwrite the parent, which the game does not do, and does not pin real behaviour. Every setter also refuses when the CJson is
cached (`[this+8] != 0`, `$ json path: %s: ERROR, json db is cached, read only.`, string
`0x5820c0`). The rewrite does not attempt a nested write under a non-object parent, and the
read-back after each write covers the cached case.

Exception frames. The login code is split by whether it calls into the game. The apply phase
(`login_apply.cpp`: `Observe`, the account-id write and its read-back, the JSON snapshot, write
and rollback, `RewriteLogin`) and the thunk handler (`login_hook.cpp`) are built `-fno-exceptions`:
no frame in them carries a personality or an LSDA, so every frame live while libpnsovr's CJson
functions or the virtual `AccountID()` run sits under the personality-free `"zR"` CIE. The compose
phase (`login_rewrite.cpp`, exceptions enabled) builds the profile and calls no game code;
`ComposePlan` catches every `std::exception` (named types, no catch-all) and returns plain data.
The handler calls observe, compose, apply, then the original, and `ComposePlan` has returned
before the next game call. `tests/quest` `TestLoginHookFramesCarryNoPersonality` pins this on a
probe executable that links the whole login archive: it walks every direct `bl`/`b` edge from every
hook record's entry and handler (`nevr_hook_records`) and fails on any reachable function under a personality-bearing
CIE, except `ComposePlan` (required to be reached and to carry a personality, so the exemption
cannot go stale) and the cold noreturn tail of libc++ (`__throw_length_error`, `terminate`, the
exception allocator).

Residual. A foreign exception thrown by the game while these frames are live (libpnsovr's
allocator hooks installed by `CJson::InitializeForGame` `0x357cb0`, or a registered log
callback in `CLoggingData::ExecuteAllCallbacks`) passes through on CFI alone, as it would
without this code. Reachability from the CJson functions the rewrite calls and from `AccountID()`
(`0x1ede14`), from the static call graph of the pinned `libpnsovr.so` (independent review): none
of the 10 CJson functions nor `AccountID()` reaches `__cxa_throw`, `__cxa_allocate_exception`,
`operator new`, terminate or `_Unwind_Resume` by direct edges (39 to 62 functions each); all 8
throw and allocate sites in `libpnsovr.so` are libc++ container code (breakpad
`std::list::push_back` at `0x20ebf0`/`0x20f1ac`, `__throw_length_error` at `0x21059c`/`0x2113c0`,
vector and `__split_buffer`); the unresolved indirect edges are the allocator hooks (`br x2` at
`0x357d28`), three in `CMemBlock::Resize`, two in `ExecuteAllCallbacks`, and one each in
`json_delete` and `fn_5083b0`. The apply phase has no try/catch; a failure of the sentinel's own
allocation there is expected to raise `std::bad_alloc` from libc++'s `operator new` (the library is
built with exceptions, whatever the caller's flags) through `zR` frames, which nothing catches:
the out-of-memory case only, inferred and not run. The
call graph was not run on a device.

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
  libr15 0x129b6f8). pnsovr's export returns the global at libpnsovr 0x70e380, a value set at run time
  that was not read here.

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
  game: +0x1e8 (`CR15NetGame::Initialize` stores 2, libr15 0x12866bc) and the party CJson at +0x1f0,
  which `CR15NetGame::Update` fills whenever `IsHost` answers true (`SetInt` 0x12951d4/0x1295628,
  `SetSymbol` 0x1295240, `Clear` 0x1295254) and then sets bit 0 of +0x27c (0x1295044, 0x1295204). The
  facade keeps a pointer to its owner in the last word.
- **Member count.** The game indexes the member JSON array at +0x248 by the member count the object
  reports and checks the index only against slot 27 (`PartyMemberData` 0x129b3fc,
  `PartyMemberHeadsetType` 0x129b168). The array holds `kMemberJsonSlots` = 10 entries, so slot 27,
  +0x200/+0x204, `MemberId` and `MemberName` never exceed 10 whatever the server sends; a clamp is counted
  (`social_members_clamped`). The model keeps its full list. Before the first `Update` the count is
  at least the local-user count `AddMember` wrote, so the engine's `MemberCount - [+0x200]`
  (`PlatformPurchaseSucceededCB`) is never negative. The PCVR facade has the same 10-entry array and no
  clamp (#234).
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
  "creating" or "joining" and defer every later join. The create is retried after four seconds. A deferred
  join is counted every frame (`social_join_deferred`) and logged once per party.
- **Events.** One `Update` delivers at most 32 callbacks; the rest are carried to the next frame in order,
  so a `Left`, `Kicked` or `MemberJoined` is delayed, not lost. The carry queue holds 256; past that the
  newest are dropped and counted (`social_events_dropped`).
- **Logging.** Every request logs its name, symbol, the id it carries (invite target, party, scope or
  policy) and whether it was sent; no secret is in any of them. The lines go to logcat unless the
  integration installs the sentinel's disk sink (`sentinel::SetLogSink`, PR #220's `sentinel_log.h`), so
  durability is the integration step's to provide; this package does not install one.

What the facade does not carry: the member and party JSON. `MemberDataWritable` returns null, so the member
array at +0x248 stays empty; the party CJson at +0x1f0 is the game's: it writes lobby settings into it while
this client leads a party (see the field list) and the facade neither serializes nor shares it, and
`Reset` leaves it alone (zeroing the pointer the game owns would orphan the tree it allocated). So headset
type in the party list and the lobby id a non-host party member follows are not shared; the engine's base `CNSISocial::Update` (0x1919868) would do that sharing
given the dirty-bit array at +0x208 and the `ShareData` slots. `libr15.so` exports the CJson calls it would
need (`DecodeFrom(char const*, unsigned long long)`, `EncodeToCompactTStr`, `Reset`). `RefreshInvites`,
`FriendsRefreshed` are not driven, as on PCVR. Display names need a registered `SocialNames::SetDecoder`
(zstd) that the Quest build does not link; the profile replies are unreadable without it, so no profile
request is sent and friends and party members show account ids until an adapter registers one.

### Integration contract

What the integration commit calls, and when:

1. **Install, in the sentinel constructor** (`nevr_sentinel_ctor`, after `InitActivation()`, next to the
   existing GOT hooks): `quest_social::RegisterSocialReportCounters()` before `StartReporter` (it takes 6
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
   request logs `NOT_sent`, is counted and is rolled back, so it is sent again later), and `quest_social::ObserveFrames(ProductionPorts(), direction, bytes, length,
   nowSeconds)` for every frame the bridge relays on the login connection, both directions, after the
   remote EVR login session is open.
5. **Link:** `nevr_quest_social` into `ovrplatformloader`. `social_install.cpp` and
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
- `ExitLobby` stores `NRadEngine::SUuid::kInvalid`, looked up once in libr15's own handle
  (`dlopen(RTLD_NOLOAD)`, then `RTLD_DEFAULT`) because libr15 is loaded `RTLD_LOCAL`; if neither finds it
  (logged `missing_zero_fallback`) it stores sixteen zero bytes.
- `UserProviderID` still comes from pnsovr; if its symbol differs from the one the game maps to platform
  code 4, friend rows are dropped silently, as they were on PCVR before the provider patch.
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
