# ADR 0003: Quest networking shares the PCVR protocol core and differs only in adapters

Status: accepted. The login-profile builder, the EVR frame codec and the redirect policy are
shared today (`src/runtime/compat/login_profile.{h,cpp}`, `src/runtime/compat/evr_codec.{h,cpp}`,
`src/runtime/lifecycle/service_redirect.{h,cpp}`).
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
- `Session` does the login on a worker thread named `nevr-auth`, so `Start()` never blocks the
  caller. Failures are handled by what they say:
  - A cached refresh token is tried first (three attempts). If the server refuses the token itself
    (400/401/403 whose JSON `message` is exactly one of the refresh RPC's own errors: `invalid or
    expired refresh token`, `refresh token expired`, `not a refresh token`, `invalid payload:
    refresh_token required`) the device login runs, `Refreshing -> AwaitingUser`.
  - A transient failure (no connection, 5xx, 408, 429, an unreadable response) of the cached
    refresh or of the device-code request is retried after 5, 15, 45, 135 and 300 s; the cache is
    kept and the player is not prompted for it.
  - Any other 4xx (a bare 401/403, which is also nakama's answer to a wrong `http_key`; a 400
    `missing payload`; a 404 for a missing RPC) is one attempt, no backoff: the cache is kept, the
    player is not prompted, an Error is logged with the status (never the body), and the login is
    `Failed`.
  - A `Failed` login that is not final is attempted again every five minutes with one request, for
    as long as the process runs, logging one Warning per failure class. Final, because the player
    was involved: the link could not be delivered, or the server refused a poll with a 4xx.
  - A code that runs out (the server answers `expired`, or its five minutes pass) is replaced: the
    state stays `AwaitingUser`, a new code is requested (no sooner than 30 s after the previous
    request) and shown in place of the old one, and the prompt is not taken down in between. This
    repeats until the player signs in, a poll is refused, or the game stops.
  - While the player holds a link, poll failures that are transient (no connection, 5xx, 408, 429)
    are waited out until the code's own five-minute deadline at the normal poll interval.
  - After login, a refresh token that has expired or that the server refuses publishes `Expired`,
    logs once and starts the device login again; other refresh failures keep the login and retry
    next period.

`nevr_quest_token_auth` is not linked into the sentinel, and nothing yet hands the token to the
login path. The tests are `src/quest/tests/auth_core_test.cpp` (fake HTTP and clock) and
`src/quest/tests/tls_ca_test.cpp` (loopback TLS peers with a generated CA and an Android-style
directory), run on the host by `just test-quest-shared`. Not established on a headset: the CA
directories and the libcurl/OpenSSL stack, that the process name is the package name, write access
to the app-internal directory (and that Quest multi-user uses `/data/user/<n>`), and that the
game's login-error screen shows the sign-in prompt below.

### Sign-in prompt in the headset

`QuestTokenAuth` hands each prompt (verification page, code, expiry) to every mechanism at once
(`auth/prompt_presenters.h`, `FanOutPresenter`), so none of them has to work alone. Each logs one
JSON line per attempt, `{"event":"login_prompt","mechanism":...,"result":...}`, with the reason
when it fails and never the code:

| Mechanism | What the player gets | Line |
| --- | --- | --- |
| `file` | `device_login.txt` (URL, code, instructions, expiry) under the external files dir; when it cannot be written, the direct link in logcat | `written` / `write_failed` |
| `game_error_text` | the prompt as the game's own login-error text | `published` / `refused` / `withdrawn` |

There is no Android intent, toast or notification: the sentinel holds no `JavaVM` or activity
object (it is loaded as a `DT_NEEDED` dependency, so its `JNI_OnLoad` is not called).

The game-text path, measured on the pinned `libr15.so` and `libpnsovr.so`:

- A login that fails before it is sent (`CNSOVRUser::UpdateInternal`, `libpnsovr.so` `0x1edb64`
  to `0x1edb7c`: code 500, "Log in request failed: One or more prerequisites are missing") calls
  `LogInFailed` through the vtable; `CNSOVRUser::LogInFailed` (`0x1ec5d0`) tail-calls
  `CNSUser::LogInFailed`, which logs `[LOGIN] %s` and calls the login-failed delegate. A server
  `SNSLogInFailure` reaches the same `CNSUser::LogInFailed` through `LogInFailureCB`
  (`libr15.so` `0x1933928`).
- `CR15NetGame::LogInFailedCB` (`libr15.so` `0x125f298`, address taken by the `GLOB_DAT` at
  `0x3708518`) calls `CR15NetGame::SetDelimitedErrorMessage(char const*)` through PLT `0xf23510`,
  whose `R_AARCH64_JUMP_SLOT` is `0x36e9170`, then `SwitchTo(-0x5e)`. The other two callers of
  that PLT entry are `LoginRemovedCB` (`0x125f9b4`) and `LocalUserProfileErrorCB` (`0x126d588`),
  both followed by the same `SwitchTo`.
- `SetDelimitedErrorMessage` (`0x125f768`) splits the message on `'\n'` into at most four lines
  and calls `SetErrorMessage`, which copies each line into a 64-byte buffer (63 characters) at
  `CR15NetGame+0x63309`, logs `[NETGAME] %s %s %s %s`, and `CR15NetErrorMessageExpression`
  (`0x23225d0`) hands those buffers to the UI script. Which screen renders the expression is in the
  game's assets, not in the ELF.

The token-auth worker writes the four-line prompt (`FormatGamePromptText`) to the prompt board
(`auth/prompt_board.h`, a sequence-locked fixed buffer built without exceptions) and withdraws it
when the login ends. The sentinel installs a GOT hook on `0x36e9170`
(`sentinel/login_prompt_hook.h`): while the board holds a prompt, the game's message is replaced by
it; otherwise it passes unchanged. The hook runs once per failed login, takes no lock and does not
log; its counters `login_prompt_text_shown` and `login_prompt_text_passed` are reported by
`hook_report.h`. The game's own `[NETGAME]` line then carries the prompt, code included. The host
tests are `src/quest/tests/login_prompt_hook_test.cpp` and the presenter and session tests in
`auth_core_test.cpp`; `got_pinned_test.cpp` resolves the slot in the pinned `libr15.so`.

Nothing here holds the game's login while the player signs in; that belongs to whatever intercepts
the game's login request. The prompt is shown each time that login fails while the board holds it.

Linking token auth into the sentinel brings OpenSSL and libcurl with it (the sentinel grows from
about 1.8 MB to about 35 MB unstripped). `libr15.so`, `libpnsrad.so`, `libpnsovr.so` and
`libpnsradmatchmaking.so` each export about 2411 OpenSSL and libcurl symbols (OpenSSL 3.0.0-dev,
libcurl 7.68.0) and have the sentinel as `DT_NEEDED`. The sentinel therefore keeps
`-Wl,--exclude-libs,ALL` and exports only `JNI_OnLoad` and `nevr_sentinel_marker`
(`TestExportAllowlist`): in a probe, a sentinel-like library linked with the flag exported 2 symbols
and had no PLT/GOT relocation bound to OpenSSL or libcurl, and without it exported 12130 and bound
1367. That the flag keeps the sentinel's OpenSSL calls from resolving into the game's older copy
is an inference from the probe, not run on a device. `just verify` fails if the sentinel links
`nevr_quest_token_auth` without the flag or without `TestExportAllowlist`; run `just test-android`
on the built artifact.

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
`clock_gettime` and `CR15NetGame::SetDelimitedErrorMessage` (both installed by `entry.cpp`),
`CJson::TString` in both libraries, and the `SNSConfigRequestv24Send` and `GLOB_DAT` slots as
fixtures. Only `clock_gettime` and `SetDelimitedErrorMessage` are installed.
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
   `nevr_server_key`, plus `features` with boolean `redirect`, `bridge`, `login` and `social`. A feature
   is off unless the file turns it on, and is forced off while its prerequisite is missing
   (bridge needs redirect and a socket URI, login needs bridge and the server key, social needs login to be effective; they resolve in that order, so a feature that loses its prerequisite takes the ones above it down with it, and each logs `forced off reason=<name>`). A malformed,
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
   `nlohmann::json` and is compiled for Windows and Android. `src/runtime/compat/evr_codec.{h,cpp}` holds
   the EVR frame parse and build functions, the LoginRequest payload layout
   (`[UUID 16][platform 8][account 8][profile JSON][NUL]`), the bridge login platform
   (`kBridgeLoginPlatform`, 4) and the bearer-replacing-path test. It has no Windows, Winsock or
   logging dependency; `ws_bridge.cpp` and `src/quest` compile the same file, and Quest does not
   copy that assembly. A reviewed serializer and a server-parser round trip remain open. The platform numbering the bridge sends
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
   adapter maps verified Quest slots, objects and callbacks to the same events. `nevr_social` is declared
   only when the social feature is effective AND the social facade is actually installed;
   otherwise the login carries 0. The shared value is `SocialParty::kSocialLevel`
   (`runtime/compat/social_level.h`, the PCVR login's constant); the production
   `IdentitySource` sets `Identity::social_level` to it when, and only when, both conditions
   hold, and the default is 0. The server sends friend presence, recently met, the lobby tablet
   and party data only to a session that declared level 1 or more (nakama
   `evr_friend_presence.go:122`, `evr_recently_met.go:181`, `evr_lobby_tablet.go:52`,
   `evr_pipeline_party_data.go:256`; `LoginProfile.SocialLevel()` in `server/evr/login_request.go`),
   so a login that declares 0 leaves them empty and a login that declares 1 without handlers in
   place would be sent messages the Quest cannot parse. What the Quest game does with a social
   message it does not parse was not verified.

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
unchanged. `SendLogInRequest` is reached only after the game holds four Oculus answers (below);
`TryInstallLoginHook` installs the hooks that measure and, when needed, supply them.

### Login prerequisites

`CNSOVRUser::LogInInternal` (`0x1ec634`) and, for a pending login, `UpdateInternal` (`0x1ed9e8`)
call `ovr_User_GetUserProof` only when the org-id global `0x70e3e0` is neither 0 nor -1, the
user-name buffer `0x70e470` is neither `"?"` (string `0x61d3cb`) nor empty, and the
`CNSLoggedInUserAccessToken` string is neither `"?"` nor shorter than two bytes. A pending login
that `UpdateInternal` sees with -1, `"?"` user or `"?"` token fails with "One or more prerequisites
are missing" (string `0x556b40`, `0x1edb54`); an empty user or token waits. `GotUserProofCB`
(`0x1ed578`) then calls `SendLogInRequest` through `vtable+0x8` (`0x1ed9c4`), or fails the login with
500 on an error. The three values are fetched by `RadPluginMain` (`0x206960`, `0x2069bc`, `0x206a00`),
which `CSysModule::Load` calls through `dlsym` (`libr15.so` `0x2a9e20c`, `0x2a9e224`) after the `dlopen`
at `0x2a9e1ec` returns, and again by `LogInInternal` when one is missing.

| Prerequisite | Callback (GLOB_DAT slot) | Success path reads | Error path |
| --- | --- | --- | --- |
| org-scoped id | `SCallbacks::GotLoggedInUserOrgIdCb` `0x1ece60` (`0x6e2f10`) | `ovr_OrgScopedID_GetID(ovr_Message_GetOrgScopedID(msg))` into `0x70e3e0` | -1 |
| logged-in user | `SCallbacks::GotLoggedInUserCb` `0x1ecfe4` (`0x6e48a8`) | `ovr_User_GetOculusID(ovr_Message_GetUser(msg))` into `0x70e470` | nothing written |
| access token | `SCallbacks::GotLoggedInUserAccessTokenCb` `0x1ed1c0` (`0x6e43f0`) | `ovr_Message_GetString(msg)` | `"?"` |
| user proof | `CNSOVRUser::GotUserProofCB` `0x1ed578` (`0x6e4340`) | `ovr_UserProof_GetNonce(ovr_Message_GetUserProof(msg))` | login fails, 500 |

Every registration of these callbacks reads its GLOB_DAT slot, and every callback reads the answer
only through Platform SDK imports (JUMP_SLOTs), so `src/quest/login/login_prerequisites.h`,
`login_prerequisites.cpp` and `login_prerequisites_install.cpp` hook the four callback slots,
the eight accessors and the four request functions
(`login_prerequisite_targets.h`; `just test-quest-hooks-pinned` resolves all of them in the pinned
`libpnsovr.so`). The callback handler asks the real `ovr_Message_IsError` first, claims the message
for the duration of the game's callback, and the accessor handlers act only on the claimed message
(or the handle its real accessor returned): an Oculus error makes `ovr_Message_IsError` answer false
and the accessors return a synthesized value; a usable real answer passes through unchanged; an
unusable one (null, empty, `"?"`, an id of 0 or -1) is replaced. The game's own success path then
writes its globals and continues. The synthesized values are not credentials; the login rewrite
replaces `accountid`, `access_token`, `nonce` and `displayname`. Substitution is enabled only when
all eight accessor hooks are installed; otherwise the callback hooks only measure.

Each callback logs one `quest_login_prerequisite` record (call, `result` real or synthesized,
`reason`, `accessor`, `ovr_error`, `error_code`, `http_code`) and each request one
`quest_login_prerequisite_request` record (the request id; the first eight per call), so a request
that is never answered shows as a request with no callback. No record carries a token, nonce, user
name or id value. The install logs one `quest_login_prerequisites_install` summary. Measured on the
host against a fake Platform SDK (`src/quest/tests/login_prerequisites_test.cpp`); not run on a
device.

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
